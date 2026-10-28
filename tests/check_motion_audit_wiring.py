"""Guards for the read-only motion provenance instrument, not image quality."""

from pathlib import Path


root = Path(__file__).resolve().parents[1]
gpu = (root / "src/gpu1_context.cpp").read_text(encoding="utf-8")
names = (root / "src/motion_source_audit.hpp").read_text(encoding="utf-8")
copy = gpu.split("void stream_mvec_copy(", 1)[1].split("// ---- R300d", 1)[0]
checks = {
    "source identity is published after copy recording, before validity":
        copy.index("gl->CopyTextureRegion")
        < copy.index("s.mvec_audit_source[slot].store")
        < copy.index("s.mvec_packets.recorded"),
    "resource names are read only from the live copy source":
        "mgpu::motion_audit::read_name(msrc, resource_name, sizeof resource_name)" in copy
        and "s.mvec_audit_names < 24u" in copy,
    "name lookup adds no GPU commands or private-data writes":
        "GetPrivateData" in names
        and not any(token in names for token in
                    ("SetPrivateData", "CopyTexture", "ResourceBarrier", "->Map", "CreateCommittedResource")),
    "readback stamps match the requested sealed frame":
        "s.vp_stamp = {f, s.mvec_audit_sealed[slot].source_at(f)};" in gpu,
    "delayed probe output uses the sample stamp":
        "s.vp_n, s.vp_stamp.frame, f, s.vp_stamp.source" in gpu,
    "the existing three-sample budget remains":
        "s.vp_n < 3u" in gpu
        and "(f == 300ull || f == 900ull || f == 1800ull)" in gpu,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
