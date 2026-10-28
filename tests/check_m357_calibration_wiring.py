import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1])
args = parser.parse_args()
gpu = (args.source / "src/gpu1_context.cpp").read_text(encoding="utf-8")
policy_path = args.source / "src/calibration_policy.hpp"
policy = policy_path.read_text(encoding="utf-8") if policy_path.exists() else ""
poll = gpu.split("void stream_poll()", 1)[1]

def ordered(*tokens):
    positions = [poll.find(token) for token in tokens]
    return all(position >= 0 for position in positions) and positions == sorted(positions)

checks = {
    "authority requires current seal, color frame, motion identity and viewport": all(
        token in policy for token in (
            "seal != 0", "motion.seal == seal", "motion.source != 0",
            "motion.engine.frame_known", "motion.engine.viewport_known", "guides.valid",
            "guides.key.frame == source_frame", "motion.engine.frame == guides.key.frame",
            "motion.engine.viewport == guides.key.viewport")),
    "production uses current slot guides and validated depth and motion":
        "mgpu::aligned::enabled(), depth_here && mvec_fresh," in poll and
        "s.mvec_packets.at(slot, f), f, s.slot_game_frame[slot], s.aligned_constants[slot]" in poll,
    "pending readback fence retires before authority cancels old matching": ordered(
        "if (probe_done < s.probe_read_fence) break;",
        "const bool authoritative_guides =", "s.cp_stage = 0u;", "s.cp_rb[0]->Map"),
    "authority guards both CPU matcher and GPU readback recording":
        "if (!authoritative_guides && s.cp_stage == 2u" in poll and
        "if (!authoritative_guides && (s.cp_stage != 0u || calibration_allowed) && s.mvec_bytes2 != 0)" in poll,
    "bounded cadence depends on attempts, not scale changes":
        "authoritative_guides, x_judge, s.cp_n, s.cp_written, s.learn_on, f" in poll and
        "x_verdicts == 0" not in poll and "probe_count < (learning ? 40u : 24u)" in policy and
        "(frame % 1000ull) < 100ull" in policy,
    "existing diagnostic writer fence and bounded vector probe remain":
        "if (SUCCEEDED(h) && probe_recorded) s.probe_read_fence = s.nf_value;" in poll and
        "mvec_fresh && s.vp_n < 3u" in poll,
    "NR still binds same-frame guides with original units": all(token in poll for token in (
        "guides.motion_subrect_scale(s.mvec_w, s.mvec_h)",
        's.nr_params->Set("DLSSNR.MVec", (s.nr_guide_mask & 1u) ? s.tex_mvec_r[ii] : nullptr)',
        's.nr_params->Set("DLSSNR.DepthInverted", unsigned(guides.depth_inverted))')),
    "new diagnostic log is session-bounded and reset at teardown":
        "if (!s.cp_authoritative_seen)" in poll and
        "s.cp_authoritative_seen = true;" in poll and
        "s.cp_authoritative_seen = false;" in gpu and "[MGPU][M357-CAL]" in poll,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
