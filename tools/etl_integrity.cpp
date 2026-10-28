#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <cstdio>

namespace
{
unsigned long long count = 0, first = 0, last = 0;
void WINAPI event(EVENT_RECORD *value)
{
    ++count;
    const auto timestamp = static_cast<unsigned long long>(value->EventHeader.TimeStamp.QuadPart);
    if (!first || timestamp < first) first = timestamp;
    if (timestamp > last) last = timestamp;
}
}

int wmain(int arguments, wchar_t **values)
{
    if (arguments != 2) return 2;
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LogFileName = values[1];
    logfile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    logfile.EventRecordCallback = event;
    auto trace = OpenTraceW(&logfile);
    if (trace == INVALID_PROCESSTRACE_HANDLE) return 3;
    const auto result = ProcessTrace(&trace, 1, nullptr, nullptr);
    CloseTrace(trace);
    std::printf("{\"status\":%lu,\"events\":%llu,\"events_lost\":%lu,\"header_events_lost\":%lu,\"buffers_lost\":%lu,\"qpc_frequency\":%lld,\"clock_type\":%lu,\"first_raw\":%llu,\"last_raw\":%llu,\"provider_semantics_validated\":false}\n",
                result, count, logfile.EventsLost, logfile.LogfileHeader.EventsLost,
                logfile.LogfileHeader.BuffersLost, logfile.LogfileHeader.PerfFreq.QuadPart,
                logfile.LogfileHeader.ReservedFlags, first, last);
    return result == ERROR_SUCCESS && !logfile.EventsLost && !logfile.LogfileHeader.EventsLost &&
        !logfile.LogfileHeader.BuffersLost && count && logfile.LogfileHeader.ReservedFlags == 1 ? 0 : 4;
}
