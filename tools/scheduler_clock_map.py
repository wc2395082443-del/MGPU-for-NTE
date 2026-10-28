"""Interpolate only inside bounded, validated read-only KMT clock samples."""
from bisect import bisect_right


class ClockMap:
    def __init__(self, report, max_gap_ms=100, max_call_us=250):
        self.adapters={}
        self.validation={}
        for adapter in report["adapters"]:
            if adapter["open_status"]!="0x0" or adapter.get("close_status")!="0x0":
                raise ValueError("Clock adapter open/close failed")
            samples=adapter["samples"]
            if len(samples)<3: raise ValueError("At least three clock samples required")
            if any(row["status"]!="0x0" or not row["cpu_in_call_window"] or
                   not row["qpc_before"]<=row["cpu_counter"]<=row["qpc_after"] or
                   row["query_us"]>max_call_us or min(row["qpc_frequency"],row["gpu_frequency"])<=0
                   for row in samples):
                raise ValueError("Failed, cached or excessive-latency clock sample")
            signatures={(row["node"],row["physical_adapter"],row["flags"],row["qpc_frequency"],row["gpu_frequency"]) for row in samples}
            if len(signatures)!=1: raise ValueError("Clock sample domain changed")
            for first,last in zip(samples,samples[1:]):
                cpu_delta=last["cpu_counter"]-first["cpu_counter"]
                gpu_delta=last["gpu_counter"]-first["gpu_counter"]
                if cpu_delta<=0 or gpu_delta<=0: raise ValueError("Nonmonotonic clock samples")
                if cpu_delta*1000/first["qpc_frequency"]>max_gap_ms: raise ValueError("Clock sample gap too large")
                rate=cpu_delta*first["gpu_frequency"]/(gpu_delta*first["qpc_frequency"])
                if abs(rate-1)>0.005: raise ValueError("Clock rate inconsistent with returned frequencies")
            luid=int(adapter["luid"],0)
            if luid in self.adapters: raise ValueError("Duplicate clock adapter")
            self.adapters[luid]=samples
            residuals=[]
            for first,middle,last in zip(samples,samples[1:],samples[2:]):
                predicted=first["cpu_counter"]+(middle["gpu_counter"]-first["gpu_counter"])*(last["cpu_counter"]-first["cpu_counter"])/(last["gpu_counter"]-first["gpu_counter"])
                residuals.append(abs(predicted-middle["cpu_counter"])*1000/first["qpc_frequency"])
            if max(residuals)>0.1: raise ValueError("Clock holdout interpolation error exceeds0.1ms")
            self.validation[hex(luid)]=dict(samples=len(samples),
                max_query_us=max(row["query_us"] for row in samples),
                max_holdout_residual_ms=max(residuals),
                start_qpc_ms=samples[0]["cpu_counter"]*1000/samples[0]["qpc_frequency"],
                end_qpc_ms=samples[-1]["cpu_counter"]*1000/samples[-1]["qpc_frequency"],
                extrapolation_allowed=False,absolute_precision_guaranteed=False)

    def project(self, luid, gpu, node=0):
        if luid not in self.adapters: return None
        samples=self.adapters[luid]
        if samples[0]["node"]!=node: return None
        stamps=[row["gpu_counter"] for row in samples]
        if gpu<stamps[0] or gpu>stamps[-1]: return None
        index=min(bisect_right(stamps,gpu)-1,len(samples)-2)
        first,last=samples[index:index+2]
        cpu=first["cpu_counter"]+(gpu-first["gpu_counter"])*(last["cpu_counter"]-first["cpu_counter"])/(last["gpu_counter"]-first["gpu_counter"])
        return cpu*1000/first["qpc_frequency"]
