"""Read-only NVML/PDH telemetry; never launches GPU work or changes settings."""
import argparse
import csv
import ctypes
import datetime
import hashlib
import json
from pathlib import Path
import random
import shutil
import time


class Utilization(ctypes.Structure):
    _fields_ = [("gpu", ctypes.c_uint), ("memory", ctypes.c_uint)]


class CounterValue(ctypes.Structure):
    _fields_ = [("status", ctypes.c_uint32), ("value", ctypes.c_double)]


class CounterItem(ctypes.Structure):
    _fields_ = [("name", ctypes.c_wchar_p), ("counter", CounterValue)]


def bind(library, name, arguments, result=ctypes.c_int):
    function = getattr(library, name)
    function.argtypes = arguments
    function.restype = result
    return function


def timestamp():
    return datetime.datetime.now().astimezone().isoformat()


def file_hash(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_csv(path, rows):
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


class EngineCounters:
    def __init__(self):
        self.query = ctypes.c_void_p()
        self.counter = ctypes.c_void_p()
        library = ctypes.WinDLL("pdh.dll")
        self.close_query = bind(library, "PdhCloseQuery", [ctypes.c_void_p], ctypes.c_uint32)
        self.collect_query = bind(library, "PdhCollectQueryData", [ctypes.c_void_p], ctypes.c_uint32)
        self.read_array = bind(library, "PdhGetFormattedCounterArrayW", [ctypes.c_void_p, ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p], ctypes.c_uint32)
        open_query = bind(library, "PdhOpenQueryW", [ctypes.c_wchar_p, ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_void_p)], ctypes.c_uint32)
        add_counter = bind(library, "PdhAddEnglishCounterW", [ctypes.c_void_p, ctypes.c_wchar_p,
            ctypes.c_size_t, ctypes.POINTER(ctypes.c_void_p)], ctypes.c_uint32)
        status = open_query(None, 0, ctypes.byref(self.query))
        if status == 0:
            status = add_counter(self.query, r"\GPU Engine(*)\Utilization Percentage", 0, ctypes.byref(self.counter))
        if status:
            self.close()
            raise RuntimeError(f"PDH initialization: 0x{status:08x}")
        status = self.collect_query(self.query)
        if status:
            self.close()
            raise RuntimeError(f"PDH first sample: 0x{status:08x}")

    def read(self, process_id):
        status = self.collect_query(self.query)
        if status:
            raise RuntimeError(f"PDH collect: 0x{status:08x}")
        byte_count = ctypes.c_uint32()
        count = ctypes.c_uint32()
        status = self.read_array(self.counter, 0x200, ctypes.byref(byte_count), ctypes.byref(count), None)
        if status != 0x800007D2:
            raise RuntimeError(f"PDH array size: 0x{status:08x}")
        storage = ctypes.create_string_buffer(byte_count.value)
        status = self.read_array(self.counter, 0x200, ctypes.byref(byte_count), ctypes.byref(count), storage)
        if status:
            raise RuntimeError(f"PDH array read: 0x{status:08x}")
        items = ctypes.cast(storage, ctypes.POINTER(CounterItem))
        return [(items[index].name, items[index].counter.value) for index in range(count.value)
            if items[index].name.startswith(f"pid_{process_id}_") and items[index].counter.status in (0, 1)]

    def close(self):
        if self.query.value:
            self.close_query(self.query)
            self.query = ctypes.c_void_p()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--wait-foreground", type=float, default=90)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    if not 1 <= options.seconds <= 120 or not 0 <= options.wait_foreground <= 180:
        parser.error("Duration must be 1-120s and foreground wait 0-180s.")
    options.output.mkdir(parents=True, exist_ok=False)
    game = Path(r"C:\Program Files\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64")
    config = game / "mgpu.ini"
    addon = game / "nvngx.dll_mgpu_bridge.addon64"
    before = {"config": file_hash(config), "dll": file_hash(addon)}
    shutil.copyfile(config, options.output / "mgpu.ini.snapshot")

    user32 = ctypes.WinDLL("user32.dll")
    get_window = bind(user32, "GetForegroundWindow", [], ctypes.c_void_p)
    get_process = bind(user32, "GetWindowThreadProcessId", [ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint32)], ctypes.c_uint32)

    def foreground_pid():
        process = ctypes.c_uint32()
        get_process(get_window(), ctypes.byref(process))
        return process.value

    library = ctypes.WinDLL(r"C:\Windows\System32\nvml.dll")
    error_string = bind(library, "nvmlErrorString", [ctypes.c_int], ctypes.c_char_p)
    initialize = bind(library, "nvmlInit_v2", [])
    shutdown = bind(library, "nvmlShutdown", [])
    status = initialize()
    if status:
        raise RuntimeError(error_string(status).decode())
    handles = []
    gpu_rows, engine_rows, errors = [], [], []
    engines = None
    metadata = {"pid": options.pid, "created": timestamp(), "before": before,
        "sampling": "NVML PCIe rate snapshots at randomized 75-125ms intervals; PDH engine rates approximately 1s",
        "units": "PCIe values use NVML KB/s, as returned; not isolated bridge payload traffic.",
        "limitations": "Rate snapshots are not byte-counter integrals; engine percentages overlap and must not be added."}
    try:
        get_count = bind(library, "nvmlDeviceGetCount_v2", [ctypes.POINTER(ctypes.c_uint)])
        get_handle = bind(library, "nvmlDeviceGetHandleByIndex_v2", [ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)])
        get_name = bind(library, "nvmlDeviceGetName", [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint])
        throughput = bind(library, "nvmlDeviceGetPcieThroughput", [ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint)])
        utilization = bind(library, "nvmlDeviceGetUtilizationRates", [ctypes.c_void_p, ctypes.POINTER(Utilization)])
        scalar_functions = {field: bind(library, function, [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint)])
            for field, function in {
                "pcie_gen": "nvmlDeviceGetCurrPcieLinkGeneration",
                "pcie_width": "nvmlDeviceGetCurrPcieLinkWidth",
                "replays": "nvmlDeviceGetPcieReplayCounter",
                "power_mw": "nvmlDeviceGetPowerUsage",
            }.items()}
        throttle = bind(library, "nvmlDeviceGetCurrentClocksThrottleReasons",
            [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint64)])
        count = ctypes.c_uint()
        status = get_count(ctypes.byref(count))
        if status:
            raise RuntimeError(error_string(status).decode())
        for index in range(count.value):
            handle = ctypes.c_void_p()
            status = get_handle(index, ctypes.byref(handle))
            if status:
                raise RuntimeError(error_string(status).decode())
            name = ctypes.create_string_buffer(128)
            status = get_name(handle, name, len(name))
            if status:
                raise RuntimeError(error_string(status).decode())
            handles.append((index, name.value.decode(), handle))
        if options.wait_foreground:
            print("Waiting for three continuous seconds with the game in foreground.", flush=True)
            deadline = time.monotonic() + options.wait_foreground
            stable_since = None
            while time.monotonic() < deadline:
                if foreground_pid() == options.pid:
                    stable_since = stable_since or time.monotonic()
                    if time.monotonic() - stable_since >= 3:
                        break
                else:
                    stable_since = None
                time.sleep(0.2)
            else:
                raise RuntimeError("Foreground wait timed out; no gameplay sample was collected.")
        try:
            engines = EngineCounters()
        except RuntimeError as error:
            errors.append(str(error))
        metadata["start"] = timestamp()
        started = time.monotonic()
        next_engine = started + 1
        print("Sampling started: " + metadata["start"], flush=True)
        while time.monotonic() - started < options.seconds:
            observed = timestamp()
            foreground = foreground_pid()
            for index, name, handle in handles:
                row = {"timestamp": observed, "elapsed_s": time.monotonic() - started,
                    "index": index, "name": name, "foreground_pid": foreground,
                    "game_foreground": foreground == options.pid}
                for field, direction in [("tx_kb_s", 0), ("rx_kb_s", 1)]:
                    value = ctypes.c_uint()
                    status = throughput(handle, direction, ctypes.byref(value))
                    row[field] = value.value if status == 0 else None
                    row[field + "_status"] = status
                for field, function in scalar_functions.items():
                    value = ctypes.c_uint()
                    status = function(handle, ctypes.byref(value))
                    row[field] = value.value if status == 0 else None
                busy = Utilization()
                status = utilization(handle, ctypes.byref(busy))
                row["gpu_util"] = busy.gpu if status == 0 else None
                row["memory_util"] = busy.memory if status == 0 else None
                reasons = ctypes.c_uint64()
                status = throttle(handle, ctypes.byref(reasons))
                row["throttle_reasons"] = reasons.value if status == 0 else None
                gpu_rows.append(row)
            if engines is not None and time.monotonic() >= next_engine:
                try:
                    for name, percent in engines.read(options.pid):
                        engine_rows.append({"timestamp": timestamp(), "elapsed_s": time.monotonic() - started,
                            "instance": name, "percent": percent, "game_foreground": foreground_pid() == options.pid})
                except RuntimeError as error:
                    errors.append(str(error))
                next_engine = time.monotonic() + 1
            time.sleep(random.uniform(0.075, 0.125))
        metadata["elapsed_s"] = time.monotonic() - started
        metadata["end"] = timestamp()
        shutil.copyfile(game / "ReShade.log", options.output / "after.log")
    finally:
        if engines is not None:
            engines.close()
        shutdown()
        write_csv(options.output / "gpu-bus.csv", gpu_rows)
        write_csv(options.output / "gpu-engines.csv", engine_rows)
        metadata["gpu_rows"] = len(gpu_rows)
        metadata["engine_rows"] = len(engine_rows)
        metadata["foreground_gpu_rows"] = sum(row["game_foreground"] for row in gpu_rows)
        metadata["errors"] = errors
        metadata["after"] = {"config": file_hash(config), "dll": file_hash(addon)}
        metadata["unchanged"] = metadata["before"] == metadata["after"]
        (options.output / "sample-window.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
