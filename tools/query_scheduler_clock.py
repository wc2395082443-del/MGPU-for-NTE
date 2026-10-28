"""Read documented KMT clock data without a graphics device or GPU commands."""
import argparse
import ctypes as native
from datetime import datetime, timezone
import json
from pathlib import Path
import time


class Luid(native.Structure):
    _fields_=[("low",native.c_uint32),("high",native.c_int32)]


class OpenAdapter(native.Structure):
    _fields_=[("luid",Luid),("handle",native.c_uint32)]


class CloseAdapter(native.Structure):
    _fields_=[("handle",native.c_uint32)]


class ClockData(native.Structure):
    _pack_=1
    _fields_=[("frequency",native.c_uint64),("gpu",native.c_uint64),
              ("cpu",native.c_uint64),("flags",native.c_uint32)]


class QueryClock(native.Structure):
    _pack_=1
    _fields_=[("handle",native.c_uint32),("node",native.c_uint32),
              ("physical",native.c_uint32),("data",ClockData)]


def abi():
    sizes={name:native.sizeof(kind) for name,kind in
           (("LUID",Luid),("OPENADAPTERFROMLUID",OpenAdapter),("CLOSEADAPTER",CloseAdapter),
            ("GPUCLOCKDATA",ClockData),("QUERYCLOCKCALIBRATION",QueryClock))}
    expected=dict(LUID=8,OPENADAPTERFROMLUID=12,CLOSEADAPTER=4,GPUCLOCKDATA=28,QUERYCLOCKCALIBRATION=40)
    if sizes!=expected or QueryClock.data.offset!=12 or ClockData.flags.offset!=24:
        raise ValueError("KMT structure layout differs from the verified Microsoft bindings")
    return sizes


def probe(luids, count, interval):
    abi()
    graphics=native.WinDLL("gdi32.dll")
    kernel=native.WinDLL("kernel32.dll")
    open_adapter=graphics.D3DKMTOpenAdapterFromLuid
    open_adapter.argtypes=[native.POINTER(OpenAdapter)]
    open_adapter.restype=native.c_int32
    close_adapter=graphics.D3DKMTCloseAdapter
    close_adapter.argtypes=[native.POINTER(CloseAdapter)]
    close_adapter.restype=native.c_int32
    query=graphics.D3DKMTQueryClockCalibration
    query.argtypes=[native.POINTER(QueryClock)]
    query.restype=native.c_int32
    counter=kernel.QueryPerformanceCounter
    counter.argtypes=[native.POINTER(native.c_int64)]
    counter.restype=native.c_int32
    frequency=native.c_int64()
    kernel.QueryPerformanceFrequency.argtypes=[native.POINTER(native.c_int64)]
    kernel.QueryPerformanceFrequency.restype=native.c_int32
    if not kernel.QueryPerformanceFrequency(native.byref(frequency)):
        raise OSError("QueryPerformanceFrequency failed")
    adapters=[]
    handles=[]
    try:
        for luid in luids:
            opened=OpenAdapter(Luid(luid & 0xffffffff,(luid>>32) & 0xffffffff),0)
            status=open_adapter(native.byref(opened))
            row=dict(luid=hex(luid),open_status=hex(status & 0xffffffff),samples=[])
            adapters.append(row)
            if status==0: handles.append((opened.handle,row))
        for index in range(count):
            for handle,row in handles:
                request=QueryClock()
                request.handle=handle
                before,after=native.c_int64(),native.c_int64()
                if not counter(native.byref(before)): raise OSError("QPC failed")
                result=query(native.byref(request))
                if not counter(native.byref(after)): raise OSError("QPC failed")
                row["samples"].append(dict(index=index,utc=datetime.now(timezone.utc).isoformat(),
                    status=hex(result & 0xffffffff),node=0,physical_adapter=0,
                    qpc_before=before.value,qpc_after=after.value,qpc_frequency=frequency.value,
                    gpu_frequency=request.data.frequency,gpu_counter=request.data.gpu,
                    cpu_counter=request.data.cpu,flags=request.data.flags,
                    cpu_in_call_window=result==0 and before.value<=request.data.cpu<=after.value,
                    query_us=(after.value-before.value)*1000000/frequency.value))
            if index+1<count: time.sleep(interval)
    finally:
        for handle,row in handles:
            closed=CloseAdapter(handle)
            row["close_status"]=hex(close_adapter(native.byref(closed)) & 0xffffffff)
    return dict(created_graphics_device=False,submitted_gpu_commands=False,
                structures=abi(),adapters=adapters,precision_validated=False)


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--abi-only",action="store_true")
    parser.add_argument("--luid",type=lambda text:int(text,0),action="append")
    parser.add_argument("--count",type=int,choices=range(3,257),default=9)
    parser.add_argument("--interval",type=float,default=0.25)
    parser.add_argument("--output",type=Path)
    args=parser.parse_args()
    if args.abi_only:
        print(json.dumps(abi()));return
    if not args.luid or not args.output or not 0.01<=args.interval<=1 or (args.count-1)*args.interval>30:
        parser.error("Require --luid, new --output, interval 0.01-1 seconds and duration at most30 seconds")
    if args.output.exists(): raise FileExistsError(args.output)
    result=probe(args.luid,args.count,args.interval)
    result["raw_reads_complete"]=all(adapter["open_status"]=="0x0" and adapter.get("close_status")=="0x0" and
        len(adapter["samples"])==args.count and all(sample["status"]=="0x0" and sample["cpu_in_call_window"]
        for sample in adapter["samples"]) for adapter in result["adapters"])
    args.output.write_text(json.dumps(result,indent=2),encoding="utf8")
    print(json.dumps(result,indent=2))
    if not result["raw_reads_complete"]: raise SystemExit("Clock query/freshness failed; raw results preserved")


if __name__=="__main__": main()
