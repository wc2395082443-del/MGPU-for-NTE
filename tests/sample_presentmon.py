"""Bounded, non-injecting PresentMon capture with foreground and file-integrity evidence."""
import argparse
import ctypes
import json
from pathlib import Path
import shutil
import subprocess
import time

from sample_bus_runtime import bind, file_hash, timestamp, write_csv


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--seconds", type=int, default=25)
    parser.add_argument("--frame-type", action="store_true")
    parser.add_argument("--app-timing", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    if not 5 <= options.seconds <= 60:
        parser.error("Capture duration must be 5-60 seconds.")
    root = Path(__file__).resolve().parent.parent
    executable = root / "devtools/PresentMon-2.6.0-x64.exe"
    expected = "b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af"
    if file_hash(executable) != expected:
        raise RuntimeError("PresentMon official release hash mismatch")
    game = Path(r"C:\Program Files\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64")
    files = {"config": game / "mgpu.ini", "dll": game / "nvngx.dll_mgpu_bridge.addon64"}
    user32 = ctypes.WinDLL("user32.dll")
    get_window = bind(user32, "GetForegroundWindow", [], ctypes.c_void_p)
    get_process = bind(user32, "GetWindowThreadProcessId", [ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint32)], ctypes.c_uint32)

    def foreground_pid():
        process = ctypes.c_uint32()
        get_process(get_window(), ctypes.byref(process))
        return process.value

    deadline = time.monotonic() + 90
    ready_since = None
    while time.monotonic() < deadline:
        now = time.monotonic()
        if foreground_pid() == options.pid:
            ready_since = now if ready_since is None else ready_since
            if now - ready_since >= 2:
                break
        else:
            ready_since = None
        time.sleep(0.2)
    else:
        raise RuntimeError("Game did not remain foreground; no capture started")

    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    metadata = {"pid": options.pid, "start": timestamp(), "presentmon_sha256": expected,
        "before": {name: file_hash(path) for name, path in files.items()}}
    shutil.copyfile(files["config"], output / "mgpu.ini.snapshot")
    command = [str(executable), "--process_id", str(options.pid), "--timed", str(options.seconds),
        "--terminate_after_timed", "--terminate_on_proc_exit", "--no_console_stats", "--no_track_input",
        "--v2_metrics", "--date_time", "--write_display_metadata", "--session_name", f"MGPUAudit-{time.time_ns()}",
        "--output_file", str(output / "frames.csv")]
    if options.frame_type:
        command.append("--track_frame_type")
    if options.app_timing:
        command.append("--track_app_timing")
    metadata["command"] = command
    rows = []
    with (output / "presentmon.log").open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline = time.monotonic() + options.seconds + 30
            while process.poll() is None and time.monotonic() < deadline:
                foreground = foreground_pid()
                rows.append({"timestamp": timestamp(), "foreground_pid": foreground,
                    "game_foreground": foreground == options.pid})
                time.sleep(0.25)
            if process.poll() is None:
                metadata["timeout"] = True
                subprocess.run([str(executable), "--session_name", command[command.index("--session_name") + 1],
                    "--terminate_existing_session"], stdout=log, stderr=subprocess.STDOUT,
                    timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
                process.wait(timeout=10)
            metadata["exit_code"] = process.returncode
        finally:
            metadata["end"] = timestamp()
            metadata["after"] = {name: file_hash(path) for name, path in files.items()}
            metadata["unchanged"] = metadata["before"] == metadata["after"]
            metadata["foreground_samples"] = sum(row["game_foreground"] for row in rows)
            metadata["total_samples"] = len(rows)
            write_csv(output / "foreground.csv", rows)
            shutil.copyfile(game / "ReShade.log", output / "after.log")
            (output / "sample-window.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
