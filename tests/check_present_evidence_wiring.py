"""Source guards complement the CPU policy test; they do not validate GPU output."""

from pathlib import Path


root = Path(__file__).resolve().parents[1]
gpu = (root / "src/gpu1_context.cpp").read_text(encoding="utf-8")
calibrator = (root / "src/calibrator.cpp").read_text(encoding="utf-8")
fg_map = (root / "src/fg_map.cpp").read_text(encoding="utf-8")
present = gpu.split("bool present_frame(", 1)[1].split("// T5:", 1)[0]
checks = {
    "composition output explicitly ignores texture alpha":
        "scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;\n            swapchain_hr = factory->CreateSwapChainForComposition("
        in gpu,
    "motion policy uses the consumed seal, not latest FG calls":
        "const auto fg_policy = mgpu::fgmap::policy_for_seal(slot, f);" in gpu
        and "const float fg_frac = fg_policy.motion_fraction;" in gpu
        and "fg_act ? mgpu::fgmap::step_fraction()" not in gpu,
    "generated label no longer uses first-seal position":
        "return g_slot_pos[slot].load" not in fg_map,
    "both evaluate taps run after the real call returns":
        calibrator.count("fgmap_tap(h, p, result == NVSDK_NGX_Result_Success)") == 2
        and "fgmap_tap(h, p);" not in calibrator,
    "marker occurs after source copying and before command submission":
        present.index("cl->CopyTextureRegion") < present.index("marker_border")
        < present.index("queue->ExecuteCommandLists"),
    "marker is opt-in and performs no readback":
        'ini_read_flag("PresentAudit")' in present
        and "if (audit_enabled && source_copied)" in present
        and "READBACK" not in present and "->Map(" not in present,
    "audit counts source only after Present returns":
        present.index("sc->Present(") < present.index("++audit_counts[kind]"),
    "guide controls change bindings after user parameters, before evaluate":
        gpu.index('for (unsigned si = 0; si < s.set_n; ++si)')
        < gpu.index('if ((s.nr_guide_mask & 1u) == 0u)')
        < gpu.index('if (s.nrs_reset_next) { s.nr_params->Set'),
    "guide changes reset history without changing transport or presentation mode":
        's.nr_first = true;\n                        char guide_line' in gpu
        and 'ini_read_float("NRGuideMask", (float)s.nr_guide_mask)' in gpu,
    "nonfinite vector samples cannot become a units recommendation":
        'r.n - r.zeros - r.nonfinite' in gpu
        and 'if (r.nonfinite != 0) r.verdict = "NONFINITE samples:' in gpu
        and 'else r.verdict = "PIXEL units: scale 1' not in gpu,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
