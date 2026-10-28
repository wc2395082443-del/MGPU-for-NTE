"""Settings history guards; these do not establish visual correctness."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "src/gpu1_context.cpp").read_text(encoding="utf-8")
poll = source.split("void stream_poll()", 1)[1]
retire = poll.split("while (s.pipeline.pending())", 1)[1].split("if (s.fg_requested)", 1)[0]
checks = {
    "configuration reset is decided once before evaluating the pass cascade":
        poll.index("submission_history.changes(neural_settings)")
        < poll.index("if (neural_settings_change != 0u) rst = 1u;")
        < poll.index('s.nr_params->Set("DLSSNR.Reset", rst);'),
    "all active passes receive the same settings reset decision":
        "if (neural_settings_change != 0u) rst = 1u;" in poll,
    "history acknowledgement follows actual GPU completion":
        retire.index("status != mgpu::neural_pipeline::completion::ready")
        < retire.index("s.neural_settings_history.complete(pending.settings)"),
    "queued settings compare with the immediately preceding submitted job":
        poll.index("submission_history.complete(s.pipeline.latest().settings)")
        < poll.index("submission_history.changes(neural_settings)"),
    "queued game frames do not compare with a stale completed frame":
        "? s.pipeline.latest().engine_frame : s.aligned_history_frame;" in poll
        and "consecutive_frame(static_cast<unsigned>(gnow), predecessor_frame)" in poll,
    "in-flight job owns settings and bound strength telemetry":
        "next.settings = neural_settings;" in poll
        and "next.bound_strengths = neural_bound_strengths;" in poll
        and "next.settings_change = neural_settings_change;" in poll,
    "telemetry reads the actual parameter block, not the current UI slider":
        's.nr_params->Get("DLSSNR.Intensity", &neural_bound_strengths[pi])' in poll
        and "pending.bound_strengths[0], pending.bound_strengths[1]" in retire,
    "teardown invalidates settings history": "s.neural_settings_history = {};" in source,
    "UI edit counter is not used as a per-frame reset condition":
        "if (s.intensity_edits" not in poll,
    "tone, local structure and skin structure are included in the job settings snapshot":
        "neural_settings.tone = s.tone_strength;" in poll
        and "neural_settings.structure = s.structure_strength;" in poll
        and "neural_settings.skin = s.skin_strength;" in poll,
    "tone, local structure and skin structure keep their own float parameter bindings":
        'Set("DLSSNR.LocalToneStrength",      s.tone_strength)' in poll
        and 'Set("DLSSNR.LocalStructureStrength", s.structure_strength)' in poll
        and 'Set("DLSSNR.SkinStructureStrength",  s.skin_strength)' in poll,
    "editing these controls enables their binding group":
        "s.tuning_on = true;" in source.split("void ui_set_tuning_value(int which, float v)", 1)[1]
        .split("void ui_set_mvec_mode", 1)[0],
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
