"""Bounded read-only NVML and PDH snapshots; no CUDA workload or settings writes."""
import argparse
import ctypes
import json
from pathlib import Path
import time

from sample_bus_runtime import bind, EngineCounters, timestamp, Utilization


class MemoryInfo(ctypes.Structure):
    _fields_ = [("total", ctypes.c_uint64), ("free", ctypes.c_uint64), ("used", ctypes.c_uint64)]


def read_uint(function, handle, *arguments):
    result = ctypes.c_uint()
    status = function(handle, *arguments, ctypes.byref(result))
    return dict(value=result.value if status == 0 else None, status=status)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", required=True, type=int)
    parser.add_argument("--seconds", type=float, default=12)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 45:
        parser.error("Duration must be 1-45 seconds.")
    args.output.mkdir(parents=True, exist_ok=False)
    library = ctypes.WinDLL(r"C:\Windows\System32\nvml.dll")
    initialize = bind(library, "nvmlInit_v2", [])
    shutdown = bind(library, "nvmlShutdown", [])
    if initialize():
        raise RuntimeError("NVML initialization failed.")
    engines = None
    errors = []
    rows = 0
    try:
        count_function = bind(library, "nvmlDeviceGetCount_v2", [ctypes.POINTER(ctypes.c_uint)])
        get_handle = bind(library, "nvmlDeviceGetHandleByIndex_v2", [ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)])
        get_name = bind(library, "nvmlDeviceGetName", [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint])
        functions = {name: bind(library, symbol, [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint)])
                     for name, symbol in {
                         "pcie_gen": "nvmlDeviceGetCurrPcieLinkGeneration",
                         "pcie_width": "nvmlDeviceGetCurrPcieLinkWidth",
                         "replays": "nvmlDeviceGetPcieReplayCounter",
                         "power_mw": "nvmlDeviceGetPowerUsage",
                         "power_limit_mw": "nvmlDeviceGetPowerManagementLimit",
                     }.items()}
        throughput = bind(library, "nvmlDeviceGetPcieThroughput", [ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint)])
        clocks = bind(library, "nvmlDeviceGetClockInfo", [ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint)])
        temperature = bind(library, "nvmlDeviceGetTemperature", [ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint)])
        utilization = bind(library, "nvmlDeviceGetUtilizationRates", [ctypes.c_void_p, ctypes.POINTER(Utilization)])
        memory = bind(library, "nvmlDeviceGetMemoryInfo", [ctypes.c_void_p, ctypes.POINTER(MemoryInfo)])
        throttle = bind(library, "nvmlDeviceGetCurrentClocksThrottleReasons", [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint64)])
        get_uuid = bind(library, "nvmlDeviceGetUUID", [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint])
        count = ctypes.c_uint()
        if count_function(ctypes.byref(count)):
            raise RuntimeError("GPU enumeration failed.")
        devices = []
        for index in range(count.value):
            handle = ctypes.c_void_p()
            if get_handle(index, ctypes.byref(handle)):
                raise RuntimeError("GPU handle lookup failed.")
            name, uuid = ctypes.create_string_buffer(128), ctypes.create_string_buffer(128)
            get_name(handle, name, len(name))
            get_uuid(handle, uuid, len(uuid))
            devices.append((index, name.value.decode(), uuid.value.decode(), handle))
        try:
            engines = EngineCounters()
        except RuntimeError as error:
            errors.append(str(error))
        started = time.perf_counter()
        next_engine = started + 1
        with (args.output / "environment.jsonl").open("x", encoding="utf-8") as stream:
            while time.perf_counter() - started < args.seconds:
                observed = timestamp()
                qpc_ns = time.perf_counter_ns()
                for index, name, uuid, handle in devices:
                    row = dict(kind="gpu", timestamp=observed, qpc_ns=qpc_ns, index=index, name=name, uuid=uuid)
                    for field, function in functions.items():
                        row[field] = read_uint(function, handle)
                    for field, selector in (("graphics_mhz", 0), ("sm_mhz", 1), ("memory_mhz", 2)):
                        row[field] = read_uint(clocks, handle, selector)
                    row["temperature_c"] = read_uint(temperature, handle, 0)
                    row["tx_kb_s"] = read_uint(throughput, handle, 0)
                    row["rx_kb_s"] = read_uint(throughput, handle, 1)
                    busy, used, reasons = Utilization(), MemoryInfo(), ctypes.c_uint64()
                    busy_status = utilization(handle, ctypes.byref(busy))
                    memory_status = memory(handle, ctypes.byref(used))
                    reason_status = throttle(handle, ctypes.byref(reasons))
                    row["utilization"] = dict(gpu=busy.gpu if busy_status == 0 else None,
                                              memory=busy.memory if busy_status == 0 else None, status=busy_status)
                    row["vram"] = dict(used=used.used if memory_status == 0 else None,
                                       total=used.total if memory_status == 0 else None, status=memory_status)
                    row["throttle"] = dict(value=reasons.value if reason_status == 0 else None, status=reason_status)
                    stream.write(json.dumps(row) + "\n")
                    rows += 1
                if engines is not None and time.perf_counter() >= next_engine:
                    try:
                        for instance, percent in engines.read(args.pid):
                            stream.write(json.dumps(dict(kind="engine", timestamp=timestamp(), qpc_ns=time.perf_counter_ns(),
                                                         instance=instance, percent=percent)) + "\n")
                    except RuntimeError as error:
                        errors.append(str(error))
                    next_engine = time.perf_counter() + 1
                time.sleep(0.2)
    finally:
        if engines is not None:
            engines.close()
        shutdown()
        (args.output / "status.json").write_text(json.dumps(dict(gpu_rows=rows, errors=errors,
            complete=rows > 0 and not errors,
            limitations="Device-wide NVML rates are not isolated bridge bytes; PDH engines overlap. No direct VRAM junction-temperature claim."), indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
