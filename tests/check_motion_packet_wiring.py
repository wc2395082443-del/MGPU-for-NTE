"""CPU packet ownership guards; not proof of same-engine-frame GPU inputs."""

from pathlib import Path

root = Path(__file__).resolve().parents[1]
gpu = (root / "src/gpu1_context.cpp").read_text(encoding="utf-8")
copy = gpu.split("void stream_mvec_copy(", 1)[1].split("// ---- R300d", 1)[0]
seal = gpu.split("seal.mvec_mode   =", 1)[1].split("// ---- R78: THE MID-FRAME", 1)[0]
checks = {
    "capture owns motion lock throughout pending selection and GPU copy recording":
        copy.index("lk(s.mvec_cs") < copy.index("s.mvec_packets.pending()")
        < copy.index("gl->CopyTextureRegion") < copy.index("s.mvec_packets.recorded"),
    "capture never selects a slot using the concurrently published producer count":
        "produced_a.load" not in copy,
    "packet close holds the same motion lock before recording any color copy":
        seal.index("motion_lock(s.mvec_cs)") < seal.index("s.mvec_packets.close(fi)")
        < seal.index("gl->CopyTextureRegion"),
    "no mutable ring validity flag remains":
        "mvec_slot_valid[" not in gpu,
    "all consumer validity uses the requested seal generation":
        gpu.count("s.mvec_packets.valid(slot, f)") == 6
        and "s.mvec_packets.valid(gs, g)" in gpu,
    "audit does not claim color engine identity or GPU ordering":
        "color_engine_frame=unknown; CPU handoff only, GPU ordering not proven" in gpu,
    "packet logs are bounded to twelve per three hundred seals":
        "if (!aligned_mode && fi % 300ull < 12ull)" in gpu,
    "engine identity does not escape its tag callback scope":
        "t_tag_engine = engine;" in gpu and "t_tag_engine = previous_engine;" in gpu,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
