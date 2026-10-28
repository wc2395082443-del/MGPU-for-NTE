#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <cstdio>

static FILE *output_file = nullptr;
static unsigned long long event_index = 0;
static const GUID dxg_provider = {0x802ec45a,0x1e99,0x4b83,{0x99,0x20,0x87,0xc9,0x82,0x77,0xba,0x9d}};
static const GUID d3d_provider = {0x5d8087dd,0x3a9b,0x4f56,{0x90,0xdf,0x49,0x19,0x6c,0xdc,0x4f,0x11}};
static const GUID dxgi_provider = {0xca11c036,0x0102,0x4a2d,{0xa6,0xad,0xf0,0x3c,0xfe,0xd5,0xd3,0xc9}};
static const GUID thread_provider = {0x3d6fa8d1,0xfe05,0x11d0,{0x9d,0xda,0x00,0xc0,0x4f,0xd7,0xba,0x7c}};
static bool output_failed = false;

static void WINAPI on_event(EVENT_RECORD *record)
{
    ++event_index;
    const auto &header = record->EventHeader;
    const char *provider = IsEqualGUID(header.ProviderId,dxg_provider) ? "dxg" :
        IsEqualGUID(header.ProviderId,d3d_provider) ? "d3d12" :
        IsEqualGUID(header.ProviderId,dxgi_provider) ? "dxgi" :
        IsEqualGUID(header.ProviderId,thread_provider) ? "cpu_thread" : nullptr;
    if (!provider) return;
    if (std::fprintf(output_file,"%llu,%lld,%s,%u,%u,%u,%lu,%lu,",event_index,header.TimeStamp.QuadPart,
        provider,unsigned(header.EventDescriptor.Id),unsigned(header.EventDescriptor.Version),
        unsigned(header.EventDescriptor.Opcode),header.ProcessId,header.ThreadId) < 0) output_failed = true;
    const auto *bytes = static_cast<const unsigned char *>(record->UserData);
    for (unsigned index = 0; index < record->UserDataLength; ++index)
        if (std::fprintf(output_file,"%02X",unsigned(bytes[index])) < 0) output_failed = true;
    if (std::fputc('\n',output_file) == EOF) output_failed = true;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 3) return 2;
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);
    if (_wfopen_s(&output_file,argv[2],L"wx") || !output_file) return 3;
    std::fprintf(output_file,"index,raw_timestamp,provider,event_id,version,opcode,pid,tid,payload_hex\n");
    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LogFileName = argv[1];
    logfile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    logfile.EventRecordCallback = on_event;
    auto trace = OpenTraceW(&logfile);
    if (trace == INVALID_PROCESSTRACE_HANDLE) { std::fclose(output_file); return 4; }
    const auto status = ProcessTrace(&trace,1,nullptr,nullptr);
    CloseTrace(trace);
    if (std::fclose(output_file) != 0) output_failed = true;
    std::printf("{\"status\":%lu,\"events\":%llu,\"lost\":%lu,\"frequency\":%lld,\"clock_type\":%lu,\"output_failed\":%s}\n",
        status,event_index,logfile.EventsLost,logfile.LogfileHeader.PerfFreq.QuadPart,
        logfile.LogfileHeader.ReservedFlags,output_failed ? "true" : "false");
    return status == ERROR_SUCCESS && !output_failed ? 0 : 5;
}
