"""Source contracts and executable old-source negative controls, not latency proof."""
from pathlib import Path
import argparse


def inspect(root):
    folder = root / "experimental/official_fg"
    host = (folder / "official_host.cpp").read_text(encoding="utf-8")
    runtime = (folder / "official_runtime.cpp").read_text(encoding="utf-8")
    tags = (root / "src/sl_tags.cpp").read_text(encoding="utf-8")
    present = host.split("double present(", 1)[1].split("void drain()", 1)[0]
    start = present.find("eRenderSubmitStart")
    end = present.find("eRenderSubmitEnd")
    return {
        "helper RenderStart and End bracket actual queue execution":
            0 <= start < present.find("ExecuteCommandLists") < end < present.find("ePresentStart"),
        "old game render markers are not replayed into helper render scope":
            "else if (relay_marker_to_sdk(marker.kind))" in host,
        "PCL configuration follows deprecated Reflex overwrite":
            runtime.find('reflex_options_(reflex)') < runtime.find('pcl_options(pcl)'),
        "PCL targets validated source-window thread rather than helper thread":
            "window_process != input_process" in runtime and "pcl.idThread = input_thread" in runtime,
        "genuine input ping counted without inventing simulation":
            "InterlockedIncrement64(&channel.source_pings)" in host,
        "native Sleep and SDK Sleep remain intact":
            "const auto result = original(frame);" in tags and "runtime.source_sleep(marker.frame)" in host,
        "native Sleep timing taken before and after original":
            "source_sleep(identity.frame, begin_ns, end_ns)" in tags,
        "source event timings retained under real source frame identity":
            "history.record_timing(marker.frame, marker.kind, timing)" in host and
            "frame.reflex = source.timing(packet.color_frame)" in host,
        "shared and private input fences preserved":
            all(token in present for token in ["queue()->Wait(slot.official_fence.Get(), slot.official_value)",
                "transport_.wait_ready", "transport_.signal_released"]),
        "sixfold options unchanged": "options.numFramesToGenerate = multiplier - 1" in runtime,
        "no extra source marker acknowledgement waits":
            "source_marker_ack" not in host,
        "no built-in OSD or fabricated HUDless":
            "kBufferTypeUIAlpha" not in host and "kBufferTypeHUDLessColor" not in host,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--negative-control", type=Path)
    arguments = parser.parse_args()
    checks = inspect(Path(__file__).resolve().parents[1])
    for name, passed in checks.items():
        print(f"{'PASS' if passed else 'FAIL'}: {name}")
    if not all(checks.values()):
        raise SystemExit(1)
    if arguments.negative_control:
        old = inspect(arguments.negative_control)
        required = list(checks)[:4]
        if any(old[name] for name in required):
            raise SystemExit("negative control unexpectedly passed a corrected contract")
        print("PASS: archived M355 fails all four corrected wiring contracts")
