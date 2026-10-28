// MGPU Bridge - D2.0: the stall watch. See stall_watch.hpp.
//
// THE WATCHDOG MUST NOT NEED ANYTHING THE STALLED THREAD MAY HOLD. So, while a
// thread is suspended, the watchdog only calls GetThreadContext,
// ReadProcessMemory and ResumeThread, into stack buffers. Names come from a
// module table it built earlier, while nothing was stalled. Numbers are
// formatted by the small appenders below, not by the CRT (whose locale lock
// the stalled thread may hold). The report goes to our own file through
// WriteFile; one short pointer line goes to ReShade's log through the queue.
#include <windows.h>
#include <psapi.h>     // K32EnumProcessModules & co. (kernel32 exports, no psapi.lib needed)
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "stall_watch.hpp"
#include "log_queue.hpp"
#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu::stallwatch
{
namespace
{
std::atomic<int> g_ms{-1};                      // -1 not read, 0 off, else the limit
LARGE_INTEGER g_freq{};
HANDLE g_file = INVALID_HANDLE_VALUE;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_loop_left{false};
std::atomic<bool> g_started{false};

const char *const k_names[S_COUNT] = {
    "none", "present-callback", "finish-present-callback", "discovery", "latency-probe",
    "latency-probe NvAPI (GetSleepStatus+GetLatency)", "latency-probe log write", "stream_on_present",
    "own-reflex", "own-reflex GetSleepStatus", "own-reflex NvAPI_D3D_Sleep",
    "own-reflex SetSleepMode", "own-reflex SetLatencyMarker", "swap-chain event" };

// ---- slots ----
constexpr unsigned SLOTS = 16u;
struct slot_t
{
    std::atomic<unsigned long> tid{0};         // 0 = free
    std::atomic<unsigned> stage{0};
    std::atomic<long long> entry{0};           // QPC at the outermost scope
    std::atomic<long long> stage_at{0};        // QPC at the current stage
    // watchdog-only bookkeeping
    long long reported_entry = 0;
    long long last_report = 0;
    unsigned long reported_tid = 0;
};
slot_t g_slots[SLOTS];
thread_local int t_slot = -1;
thread_local int t_depth = 0;

// ---- game presents ----
std::atomic<void *> g_game_chain{nullptr};
std::atomic<long long> g_last_present{0};
std::atomic<unsigned long long> g_presents{0};
bool g_outside_reported = false;               // watchdog only
long long g_outside_since = 0;                 // watchdog only

// ---- module table (watchdog only) ----
struct mod_t { unsigned long long base; unsigned long long size; char name[48]; };
constexpr unsigned MODS = 512u;
mod_t g_mods[MODS];
unsigned g_nmods = 0;
long long g_mods_at = 0;

long long qpc() { LARGE_INTEGER c{}; QueryPerformanceCounter(&c); return c.QuadPart; }
long long ms_of(long long dq) { return g_freq.QuadPart ? dq * 1000 / g_freq.QuadPart : 0; }

void refresh_modules()
{
    HMODULE hm[MODS];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), hm, sizeof hm, &need)) return;
    unsigned n = need / sizeof(HMODULE);
    if (n > MODS) n = MODS;
    unsigned k = 0;
    for (unsigned i = 0; i < n; ++i)
    {
        MODULEINFO mi{};
        if (!K32GetModuleInformation(GetCurrentProcess(), hm[i], &mi, sizeof mi)) continue;
        g_mods[k].base = (unsigned long long)mi.lpBaseOfDll;
        g_mods[k].size = mi.SizeOfImage;
        if (!K32GetModuleBaseNameA(GetCurrentProcess(), hm[i], g_mods[k].name, sizeof g_mods[k].name))
            g_mods[k].name[0] = '\0';
        ++k;
    }
    g_nmods = k;
    g_mods_at = qpc();
}

const mod_t *find_mod(unsigned long long a)
{
    for (unsigned i = 0; i < g_nmods; ++i)
        if (a >= g_mods[i].base && a < g_mods[i].base + g_mods[i].size) return &g_mods[i];
    return nullptr;
}

// ---- appenders (no CRT) ----
struct buf_t { char b[2048]; unsigned n = 0; };
void app(buf_t &o, const char *s) { while (*s && o.n < sizeof o.b - 1) o.b[o.n++] = *s++; o.b[o.n] = 0; }
void app_u(buf_t &o, unsigned long long v)
{
    char t[24]; int i = 0;
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < 23);
    while (i > 0 && o.n < sizeof o.b - 1) o.b[o.n++] = t[--i];
    o.b[o.n] = 0;
}
void app_x(buf_t &o, unsigned long long v)
{
    static const char h[] = "0123456789ABCDEF";
    char t[20]; int i = 0;
    do { t[i++] = h[v & 15]; v >>= 4; } while (v && i < 19);
    app(o, "0x");
    while (i > 0 && o.n < sizeof o.b - 1) o.b[o.n++] = t[--i];
    o.b[o.n] = 0;
}
void app_addr(buf_t &o, unsigned long long a)
{
    const mod_t *m = find_mod(a);
    if (m != nullptr) { app(o, m->name); app(o, "+"); app_x(o, a - m->base); }
    else app_x(o, a);
}
void app_time(buf_t &o)
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    unsigned v[3] = { st.wHour, st.wMinute, st.wSecond };
    for (int i = 0; i < 3; ++i)
    {
        if (v[i] < 10) app(o, "0");
        app_u(o, v[i]);
        app(o, i < 2 ? ":" : ".");
    }
    if (st.wMilliseconds < 100) app(o, "0");
    if (st.wMilliseconds < 10) app(o, "0");
    app_u(o, st.wMilliseconds);
    app(o, " ");
}
void emit(buf_t &o)
{
    if (g_file == INVALID_HANDLE_VALUE) return;
    app(o, "\r\n");
    DWORD w = 0;
    WriteFile(g_file, o.b, o.n, &w, nullptr);
}

// Where is that thread? Suspend, copy the context and the top of its stack,
// resume; name the addresses afterwards.
bool where(buf_t &o, unsigned long tid, const slot_t &s, long long entry)
{
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                           FALSE, tid);
    if (th == nullptr) { app(o, " | OpenThread failed"); return true; }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    unsigned long long stack[128];
    SIZE_T got = 0;
    bool ok = false;
    if (SuspendThread(th) != (DWORD)-1)
    {
        if (GetThreadContext(th, &ctx))
        {
            ok = true;
            ReadProcessMemory(GetCurrentProcess(), (const void *)ctx.Rsp, stack, sizeof stack, &got);
        }
        ResumeThread(th);
    }
    CloseHandle(th);
    // The slot may have been released and claimed by another thread between
    // the watchdog's reads and the suspend: then this context is not the
    // stall's. Checked after the resume, so the cost is one brief suspend.
    if (s.tid.load(std::memory_order_acquire) != tid || s.entry.load(std::memory_order_acquire) != entry)
        return false;
    if (!ok) { app(o, " | context not read"); return true; }
    app(o, " | rip="); app_addr(o, ctx.Rip);
    app(o, " | code addresses on the stack, top first:");
    unsigned shown = 0;
    for (SIZE_T i = 0; i < got / sizeof(unsigned long long) && shown < 16u; ++i)
    {
        const unsigned long long a = stack[i];
        const mod_t *m = find_mod(a);
        if (m == nullptr) continue;
        app(o, " "); app_addr(o, a);
        ++shown;
    }
    if (shown == 0) app(o, " (none found in the first 1 KB)");
    return true;
}

DWORD WINAPI watchdog(LPVOID)
{
    refresh_modules();
    const long long five_s = g_freq.QuadPart * 5;
    while (!g_stop.load(std::memory_order_acquire))
    {
        Sleep(100);
        const int lim = g_ms.load(std::memory_order_relaxed);
        if (lim <= 0) continue;
        const long long now = qpc();
        const long long limq = g_freq.QuadPart * lim / 1000;
        bool any_stalled = false, any_open = false;
        for (unsigned i = 0; i < SLOTS; ++i)
        {
            slot_t &s = g_slots[i];
            const unsigned long tid = s.tid.load(std::memory_order_acquire);
            const long long entry = s.entry.load(std::memory_order_acquire);
            if (s.tid.load(std::memory_order_acquire) != tid) continue;   // changed while read: next round
            if (tid == 0 || entry != s.reported_entry)
            {
                // A stall we reported has ended (slot freed or re-entered).
                if (s.reported_entry != 0)
                {
                    buf_t o; app_time(o);
                    app(o, "STALL ENDED slot "); app_u(o, i); app(o, " tid "); app_u(o, s.reported_tid);
                    app(o, " - it lasted at least "); app_u(o, (unsigned long long)ms_of(s.last_report - s.reported_entry));
                    app(o, " ms (ended within 100 ms before this line)");
                    emit(o);
                    s.reported_entry = 0;
                }
            }
            if (tid == 0 || entry == 0) continue;   // free, or claimed but not stamped yet
            any_open = true;
            if (now - entry < limq) continue;
            any_stalled = true;
            if (s.reported_entry == entry && now - s.last_report < five_s) continue;
            const unsigned st = s.stage.load(std::memory_order_acquire);
            const long long sat = s.stage_at.load(std::memory_order_acquire);
            buf_t o; app_time(o);
            app(o, s.reported_entry == entry ? "STALL (still) " : "STALL ");
            app(o, "slot "); app_u(o, i); app(o, " tid "); app_u(o, tid);
            app(o, " | stage: "); app(o, st < S_COUNT ? k_names[st] : "?");
            app(o, " for "); app_u(o, (unsigned long long)ms_of(now - sat));
            app(o, " ms | callback open for "); app_u(o, (unsigned long long)ms_of(now - entry)); app(o, " ms");
            if (!where(o, tid, s, entry)) continue;   // the slot moved on: not this stall
            emit(o);
            // One pointer line for ReShade's log, formatted without the CRT and
            // posted only to a worker that already runs: the watchdog never
            // starts a thread or touches ReShade's log lock itself.
            if (s.reported_entry != entry && mgpu::lq::running())
            {
                buf_t p;
                app(p, "[MGPU][D20] STALL: thread "); app_u(p, tid);
                app(p, " in stage '"); app(p, st < S_COUNT ? k_names[st] : "?");
                app(p, "' for over "); app_u(p, (unsigned long long)lim);
                app(p, " ms - details in mgpu\\stall.log");
                mgpu::lq::post(mgpu::lq::TO_WARN, p.b);
            }
            s.reported_entry = entry;
            s.reported_tid = tid;
            s.last_report = now;
        }

        // No callback of ours open, yet no game present for the limit.
        const long long lp = g_last_present.load(std::memory_order_relaxed);
        if (!any_open && g_presents.load(std::memory_order_relaxed) > 300ull && lp != 0)
        {
            if (!g_outside_reported && now - lp > limq)
            {
                buf_t o; app_time(o);
                app(o, "STALL OUTSIDE OUR CALLBACK: no game present for ");
                app_u(o, (unsigned long long)ms_of(now - lp));
                app(o, " ms and no callback of ours is open (game or driver side)");
                emit(o);
                g_outside_reported = true;
                g_outside_since = lp;
            }
        }
        if (g_outside_reported && lp != g_outside_since)
        {
            buf_t o; app_time(o);
            app(o, "game presents resumed after ");
            app_u(o, (unsigned long long)ms_of(lp - g_outside_since));
            app(o, " ms");
            emit(o);
            g_outside_reported = false;
        }

        // Module table: refresh every 30 s, never while something is stalled.
        if (!any_stalled && now - g_mods_at > g_freq.QuadPart * 30) refresh_modules();
    }
    g_loop_left.store(true, std::memory_order_release);
    return 0;
}

bool addon_dir(wchar_t *out, size_t n)
{
    static const int anchor = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &hm))
        return false;
    const DWORD len = GetModuleFileNameW(hm, out, (DWORD)n);
    if (len == 0 || len >= n) return false;
    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    return true;
}

int read_ms()
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!addon_dir(dir, MAX_PATH)) return 0;
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsmgpu.ini", dir);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return 0;
    static char buf[mgpu::config::MAX_BYTES + 2u];
    const size_t got = fread(buf, 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return 0;
    buf[got] = '\0';
    const char *v = mgpu::config::find(buf, got, "StallWatch");
    if (v == nullptr) return 0;
    const int ms = atoi(v);
    if (ms <= 0) return 0;
    return ms < 100 ? 100 : (ms > 60000 ? 60000 : ms);
}
}   // namespace

void init()
{
    // Re-arm after signal_stop (a new game device in the same load): a new
    // watchdog only once the old one has left its loop.
    if (g_ms.load(std::memory_order_acquire) > 0 && g_stop.load(std::memory_order_acquire) &&
        g_loop_left.load(std::memory_order_acquire))
    {
        g_loop_left.store(false, std::memory_order_release);
        g_stop.store(false, std::memory_order_release);
        HANDLE h = CreateThread(nullptr, 0, &watchdog, nullptr, 0, nullptr);
        if (h != nullptr) CloseHandle(h);
        buf_t o; app_time(o); app(o, "STALL WATCH re-armed for a new game device"); emit(o);
        return;
    }
    if (g_ms.load(std::memory_order_acquire) >= 0) return;
    const int ms = read_ms();
    QueryPerformanceFrequency(&g_freq);
    int expect = -1;
    if (!g_ms.compare_exchange_strong(expect, 0, std::memory_order_acq_rel)) return;
    if (ms <= 0) return;

    wchar_t dir[MAX_PATH], sub[MAX_PATH], path[MAX_PATH];
    if (!addon_dir(dir, MAX_PATH)) return;
    _snwprintf_s(sub, MAX_PATH, _TRUNCATE, L"%lsmgpu", dir);
    CreateDirectoryW(sub, nullptr);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsmgpu\\stall.log", dir);
    g_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE)
    {
        mgpu::diag::warn("[MGPU][D20] STALL WATCH requested but mgpu\\stall.log could not be opened - off.");
        return;
    }
    {
        buf_t o; app_time(o);
        app(o, "STALL WATCH ON | limit "); app_u(o, (unsigned long long)ms);
        app(o, " ms | pid "); app_u(o, GetCurrentProcessId());
        emit(o);
    }
    g_ms.store(ms, std::memory_order_release);
    bool exp2 = false;
    if (g_started.compare_exchange_strong(exp2, true))
    {
        HANDLE h = CreateThread(nullptr, 0, &watchdog, nullptr, 0, nullptr);
        if (h != nullptr) CloseHandle(h);
    }
    char l[240];
    snprintf(l, sizeof l, "[MGPU][D20] STALL WATCH ON (StallWatch=%d): a callback of ours open longer "
             "than %d ms is reported to mgpu\\stall.log with the stage and where the thread is.", ms, ms);
    mgpu::diag::warn(l);
}

void note_game_chain(void *chain)
{
    if (g_ms.load(std::memory_order_relaxed) <= 0) return;
    g_game_chain.store(chain, std::memory_order_release);
}

void on_present(void *chain)
{
    if (g_ms.load(std::memory_order_relaxed) <= 0) return;
    if (chain == nullptr || chain != g_game_chain.load(std::memory_order_acquire)) return;
    g_last_present.store(qpc(), std::memory_order_relaxed);
    g_presents.fetch_add(1, std::memory_order_relaxed);
}

scope::scope(stage s)
{
    if (g_ms.load(std::memory_order_relaxed) <= 0) return;
    const long long now = qpc();
    if (t_slot < 0)
    {
        const unsigned long me = GetCurrentThreadId();
        for (unsigned i = 0; i < SLOTS; ++i)
        {
            unsigned long expect = 0;
            if (g_slots[i].tid.compare_exchange_strong(expect, me, std::memory_order_acq_rel))
            {
                g_slots[i].stage.store((unsigned)s, std::memory_order_release);
                g_slots[i].stage_at.store(now, std::memory_order_release);
                g_slots[i].entry.store(now, std::memory_order_release);
                t_slot = (int)i;
                break;
            }
        }
        if (t_slot < 0) return;   // all slots busy: not watched
        prev_ = S_NONE;
    }
    else
    {
        prev_ = g_slots[t_slot].stage.load(std::memory_order_relaxed);
        prev_q_ = g_slots[t_slot].stage_at.load(std::memory_order_relaxed);
        g_slots[t_slot].stage.store((unsigned)s, std::memory_order_release);
        g_slots[t_slot].stage_at.store(now, std::memory_order_release);
    }
    ++t_depth;
    active_ = true;
}

scope::~scope()
{
    if (!active_ || t_slot < 0) return;
    if (--t_depth == 0)
    {
        slot_t &sl = g_slots[t_slot];
        sl.stage.store(S_NONE, std::memory_order_release);
        sl.entry.store(0, std::memory_order_release);
        sl.tid.store(0, std::memory_order_release);
        t_slot = -1;
    }
    else
    {
        g_slots[t_slot].stage.store(prev_, std::memory_order_release);
        g_slots[t_slot].stage_at.store(prev_q_, std::memory_order_release);
    }
}

void signal_stop()
{
    g_stop.store(true, std::memory_order_release);
}

void stop(bool process_exit)
{
    g_stop.store(true, std::memory_order_release);
    if (!g_started.load(std::memory_order_acquire)) return;
    if (process_exit) return;   // every other thread is already gone
    for (int i = 0; i < 30 && !g_loop_left.load(std::memory_order_acquire); ++i) Sleep(10);
}
}   // namespace mgpu::stallwatch
