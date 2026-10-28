// MGPU Bridge - D2.0 NR step one: the frame-gen vector map. See fg_map.hpp.
//
// Read only. Every line goes through the log queue (never written from the
// game's threads). Counters are cumulative from init; two summaries 10 s apart
// give the rates by difference.
//
// POSITIONS. Seals (one per captured present of the game) and copies are
// placed against the last frame-gen evaluate:
//   0   after a frame-gen evaluate, before our next seal
//   1   the first seal after it
//   2   the second seal after it
//   3   any later one, and everything before the run's first frame-gen
//       evaluate (frame gen off reads 3 throughout)
// With frame gen x2 and one frame-gen evaluate per rendered frame, every
// seal is 1 or 2.
//
// THE LABEL, READ. At each seal, the back buffer we capture is compared with
// the last frame-gen evaluate's
// DLSSG.OutputInterpolated, DLSSG.OutputReal and DLSSG.Backbuffer. A match
// says which frame that present is - provided frame gen writes into the
// presented back buffer itself. If it writes into its own texture and copies,
// no handle matches, and the summary says so (all zero).
#include <windows.h>
#include <d3d12.h>
#include <d3d11.h>   // R233: desc_of on a D3D11 resource
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>

#include "fg_map.hpp"
#include "log_queue.hpp"
#include "journal.hpp"
#include "mgpu_ini_parser.hpp"
#include "motion_source_audit.hpp"

namespace mgpu::fgmap
{
namespace
{
std::atomic<int> g_mode{0};                       // 0 off (also before init), 1 map, 2 fix, 3 fix + thin (D2.0 NR2/NR3)
std::atomic<unsigned> g_keep{2u};                   // NR3: keep 1 generated frame in N (NRFrameKeep, 2..8)
// ---- D2.0 NR2 / NR3: what the acting modes read (see the top of the file) ----
// Recent calls are diagnostic activity, not proof of presented interpolation.
std::atomic<unsigned> g_fg_idle{255u};
std::atomic<unsigned long long> g_gen_seen{0};     // generated seals the consumer asked about (thin counter)
std::atomic<unsigned long long> c_reused{0};       // NR2: generated frames evaluated with the kept vectors
// Position remains diagnostic only. Acting policy is tied to captured resource
// identity and stored with the seal, never inferred from call position.
constexpr unsigned SLOT_N = 8u;                     // >= the stream's RING (6)
std::atomic<unsigned long long> g_slot_frame[SLOT_N];
std::atomic<unsigned> g_slot_pos[SLOT_N];
std::mutex g_evidence_mutex;
present_evidence g_present_evidence;
struct sealed_policy
{
    unsigned long long frame = 0;
    captured_policy policy{};
};
sealed_policy g_sealed_policies[SLOT_N];
std::atomic<unsigned long long> c_verified_seals{0}, c_unknown_seals{0};
std::atomic<bool> g_last_verified{false};
std::atomic<unsigned long long> c_thinned{0};      // NR3: generated frames not evaluated (last output re-presented)
std::atomic<bool> g_inited{false};
// Set once in init(), before g_mode is published (release); on() acquires.
double g_qpc_to_s = 0.0;
long long g_qpc_freq = 0;
long long g_t0 = 0;                               // QPC at init: every t= is from there

// ---- what the evaluates said (last values) ----
std::atomic<unsigned long long> g_fg_handle{0};    // learned at CreateFeature (id 11)
std::atomic<unsigned long long> g_scene_mvec{0};   // last scene evaluate's MotionVectors
std::atomic<float> g_scene_sx{0.0f}, g_scene_sy{0.0f};
std::atomic<unsigned long long> g_fg_mvecs{0};
std::atomic<unsigned long long> g_fg_interp{0}, g_fg_real{0}, g_fg_bb{0};
std::atomic<float> g_fg_sx{0.0f}, g_fg_sy{0.0f};
std::atomic<unsigned> g_fg_mf_count{0}, g_fg_mf_index{0}, g_fg_cam_motion{0}, g_fg_jittered{0};
std::atomic<unsigned long long> g_fg_frame_id{0};
std::atomic<unsigned> g_since_fg{3u};              // position, see the top of the file
std::mutex g_camera_mutex;
camera_snapshot g_camera{};

// ---- counters (cumulative) ----
std::atomic<unsigned long long> c_scene{0}, c_fg{0}, c_other{0};
std::atomic<unsigned long long> c_fg_mv_same{0}, c_fg_mv_diff{0}, c_fg_mv_none{0};
std::atomic<unsigned long long> c_fg_reset{0}, c_fg_not_rendering{0};
std::atomic<unsigned long long> c_fg_id_step1{0}, c_fg_id_same{0}, c_fg_id_other{0};
std::atomic<unsigned long long> c_seals{0}, c_seals_vec{0};
std::atomic<unsigned long long> c_vec_at[4][2];    // [position][0 without vectors, 1 with]
std::atomic<unsigned long long> c_bb_interp[4], c_bb_real[4], c_bb_input[4];
std::atomic<unsigned long long> c_copy_at[4], c_scene_at[4];

// ---- first sight of each evaluating handle; frame gen's MVecs changes ----
constexpr unsigned HSEEN = 8u;
std::atomic<unsigned long long> g_hseen[HSEEN];
std::atomic<unsigned> g_hseen_n{0};
std::atomic<unsigned> g_fg_mv_changes{0};

// ---- the ordered event stream ----
constexpr unsigned ERING = 1024u;
struct evt
{
    std::atomic<unsigned long long> seq{0};   // 1-based, 0 while being written
    std::atomic<long long> qpc{0};
    std::atomic<unsigned long long> a{0};     // seal: frame index; fg evaluate: BackbufferFrameID
    std::atomic<unsigned> kind{0};            // 'S' 'G' 'O' 'C' 'V'
    std::atomic<unsigned> b{0};               // copy: slot; seal: valid | position << 1 | label << 8; fg: MultiFrameIndex
    std::atomic<unsigned> tid{0};
};
evt g_ering[ERING];
std::atomic<unsigned long long> g_eseq{0};

std::atomic<long long> g_next_report{0};
std::atomic<unsigned> g_reports{0};
constexpr unsigned MAX_BURSTS = 30u;
constexpr unsigned MAX_REPORTS = 360u;          // one hour of summaries

long long qpc_now()
{
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

double now_s() { return (double)(qpc_now() - g_t0) * g_qpc_to_s; }

void say(const char *l) { mgpu::lq::post(mgpu::lq::TO_INFO, l); }

unsigned long long ld(const std::atomic<unsigned long long> &x) { return x.load(std::memory_order_relaxed); }

void ev_put(unsigned kind, unsigned long long a, unsigned b)
{
    const unsigned long long n = g_eseq.fetch_add(1, std::memory_order_acq_rel) + 1ull;
    evt &e = g_ering[(n - 1ull) % ERING];
    e.seq.store(0ull, std::memory_order_release);
    e.qpc.store(qpc_now(), std::memory_order_release);
    e.a.store(a, std::memory_order_release);
    e.kind.store(kind, std::memory_order_release);
    e.b.store(b, std::memory_order_release);
    e.tid.store((unsigned)GetCurrentThreadId(), std::memory_order_release);
    e.seq.store(n, std::memory_order_release);
}

unsigned pos_now()
{
    const unsigned k = g_since_fg.load(std::memory_order_relaxed);
    return k > 3u ? 3u : k;
}

void desc_of(unsigned long long h, unsigned &w, unsigned &hh, unsigned &fmt,
             char *name = nullptr, size_t name_size = 0)
{
    w = hh = fmt = 0u;
    if (name != nullptr && name_size != 0) std::snprintf(name, name_size, "<unavailable>");
    if (h == 0ull) return;
    // R233. The handle is whatever the game set in the NGX block: an
    // ID3D12Resource on D3D12, an ID3D11Resource on D3D11 (the calibrator's
    // get_res reads both). GetDesc sits in different vtable slots, so ask
    // first - QueryInterface is slot 0 on both. Neither: zeros, as before.
    IUnknown *u = reinterpret_cast<IUnknown *>((void *)(uintptr_t)h);
    ID3D12Resource *r12 = nullptr;
    if (SUCCEEDED(u->QueryInterface(__uuidof(ID3D12Resource), reinterpret_cast<void **>(&r12))) && r12 != nullptr)
    {
        const D3D12_RESOURCE_DESC d = r12->GetDesc();
        w = (unsigned)d.Width; hh = (unsigned)d.Height; fmt = (unsigned)d.Format;
        mgpu::motion_audit::read_name(r12, name, name_size);
        r12->Release();
        return;
    }
    ID3D11Texture2D *t11 = nullptr;
    if (SUCCEEDED(u->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&t11))) && t11 != nullptr)
    {
        D3D11_TEXTURE2D_DESC d{};
        t11->GetDesc(&d);
        w = d.Width; hh = d.Height; fmt = (unsigned)d.Format;
        mgpu::motion_audit::read_name(t11, name, name_size);
        t11->Release();
    }
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

int read_key()
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
    // D2.0 NR3: NRFrameKeep=N, keep 1 generated frame in N (2..8; default 2).
    const char *k = mgpu::config::find(buf, got, "NRFrameKeep");
    if (k != nullptr)
    {
        const int n = atoi(k);
        if (n >= 2 && n <= 8) g_keep.store((unsigned)n, std::memory_order_relaxed);
        else
        {
            char l[160];
            snprintf(l, sizeof l, "[MGPU][D20][NR3] NRFrameKeep=%d is outside 2..8: 2 is used.", n);
            mgpu::lq::post(mgpu::lq::TO_WARN, l);
        }
    }
    const char *v = mgpu::config::find(buf, got, "NRFrameFilter");
    // DEFAULT 3 (Marcelo, 2026-10-05 22:01, after Dawnwalker): fix the vectors
    // and evaluate 1 generated frame in 2. Absent = 3; an explicit 0 is off.
    if (v == nullptr) return 3;
    const int x = atoi(v);
    if (x >= 0 && x <= 3) return x;
    char l[220];
    snprintf(l, sizeof l, "[MGPU][D20][NR1] NRFrameFilter=%d is not a mode (0 off, 1 map, 2 fix the "
                          "vectors, 3 fix and thin). The default, 3, is used.", x);
    mgpu::lq::post(mgpu::lq::TO_WARN, l);
    return 3;
}

// The last 96 events in order, 12 per line:
//   S@q    scene evaluate (SR / RR)
//   G<i>#<id>@q   frame-gen evaluate: MultiFrameIndex i, BackbufferFrameID id
//   O@q    any other evaluate
//   C<slot>@q     our vector copy into that ring slot
//   V<frame><+|-><pos><l>@q   our seal: + the slot had vectors, - it had
//                 none; pos as at the top of the file; l = i if the captured
//                 back buffer is fg OutputInterpolated, r OutputReal, b fg's
//                 input Backbuffer, . none of them
//   " t<id>" when the thread differs from the previous event.
void burst(unsigned bn)
{
    const unsigned long long top = g_eseq.load(std::memory_order_acquire);
    if (top < 96ull) return;
    const unsigned long long from = top - 96ull + 1ull;
    unsigned prev_tid = 0, skipped = 0, in_line = 0, line_no = 0;
    char l[1000];
    int w = 0;
    for (unsigned long long k = from; k <= top; ++k)
    {
        const evt &e = g_ering[(k - 1ull) % ERING];
        const unsigned long long s1 = e.seq.load(std::memory_order_acquire);
        const long long q = e.qpc.load(std::memory_order_acquire);
        const unsigned long long a = e.a.load(std::memory_order_acquire);
        const unsigned kind = e.kind.load(std::memory_order_acquire);
        const unsigned b = e.b.load(std::memory_order_acquire);
        const unsigned tid = e.tid.load(std::memory_order_acquire);
        const unsigned long long s2 = e.seq.load(std::memory_order_acquire);
        if (s1 != k || s2 != s1) { ++skipped; continue; }
        if (in_line == 0)
            w = snprintf(l, sizeof l, "[MGPU][D20][NR1] events b%u.%u t=%.2fs (seq %llu-%llu):",
                         bn, ++line_no, now_s(), from, top);
        if (w > 0 && w < (int)sizeof l - 64)
        {
            if (kind == 'C')      w += snprintf(l + w, sizeof l - (size_t)w, " C%u@%lld", b, q);
            else if (kind == 'V') w += snprintf(l + w, sizeof l - (size_t)w, " V%llu%c%u%c@%lld", a,
                                                 (b & 1u) ? '+' : '-', (b >> 1) & 3u, (char)(b >> 8), q);
            else if (kind == 'G') w += snprintf(l + w, sizeof l - (size_t)w, " G%u#%llu@%lld", b, a, q);
            else                  w += snprintf(l + w, sizeof l - (size_t)w, " %c@%lld", (char)kind, q);
            if (tid != prev_tid && w > 0 && w < (int)sizeof l - 16)
                w += snprintf(l + w, sizeof l - (size_t)w, " t%u", tid);
        }
        prev_tid = tid;
        if (++in_line == 12u) { say(l); in_line = 0; }
    }
    if (in_line != 0) say(l);
    if (skipped != 0)
    {
        char m[140];
        snprintf(m, sizeof m, "[MGPU][D20][NR1] events b%u: %u event(s) skipped (rewritten while read)", bn, skipped);
        say(m);
    }
}

void report()
{
    const unsigned rn = g_reports.fetch_add(1, std::memory_order_relaxed) + 1u;
    if (rn > MAX_REPORTS) return;
    const unsigned long long fgh = g_fg_handle.load(std::memory_order_relaxed);
    char l[1000];
    snprintf(l, sizeof l,
             "[MGPU][D20][NR1] r%u t=%.1fs | evaluates scene=%llu fg=%llu other=%llu | fg handle %p %s | "
             "fg DLSSG.MVecs vs scene MotionVectors: same=%llu different=%llu absent=%llu | "
             "fg MvecScale %.4f,%.4f (scene %.4f,%.4f) | fg MultiFrameCount %u last index %u | "
             "CameraMotionIncluded %u MvecJittered %u | fg Reset x%llu NotRenderingGameFrames x%llu | "
             "BackbufferFrameID step +1 x%llu same x%llu other x%llu (last %llu)",
             rn, now_s(), ld(c_scene), ld(c_fg), ld(c_other), (void *)(uintptr_t)fgh,
             fgh != 0ull ? "(id 11 at create)" : "(no id 11 create seen: unidentified handles remain other)",
             ld(c_fg_mv_same), ld(c_fg_mv_diff), ld(c_fg_mv_none),
             (double)g_fg_sx.load(std::memory_order_relaxed), (double)g_fg_sy.load(std::memory_order_relaxed),
             (double)g_scene_sx.load(std::memory_order_relaxed), (double)g_scene_sy.load(std::memory_order_relaxed),
             g_fg_mf_count.load(std::memory_order_relaxed), g_fg_mf_index.load(std::memory_order_relaxed),
             g_fg_cam_motion.load(std::memory_order_relaxed), g_fg_jittered.load(std::memory_order_relaxed),
             ld(c_fg_reset), ld(c_fg_not_rendering),
             ld(c_fg_id_step1), ld(c_fg_id_same), ld(c_fg_id_other), ld(g_fg_frame_id));
    say(l);
    snprintf(l, sizeof l,
             "[MGPU][D20][NR1] r%u seals=%llu (with vectors %llu) | by position after a fg evaluate "
             "(1 first seal, 2 second, 3 later or no fg yet): with/without vectors "
             "#1 %llu/%llu #2 %llu/%llu #3 %llu/%llu | captured back buffer = OutputInterpolated #1 %llu #2 %llu #3 %llu, "
             "= OutputReal #1 %llu #2 %llu #3 %llu, = fg input Backbuffer #1 %llu #2 %llu #3 %llu | "
             "our vector copy at 0:%llu 1:%llu 2:%llu 3:%llu | scene evaluate at 0:%llu 1:%llu 2:%llu 3:%llu",
             rn, ld(c_seals), ld(c_seals_vec),
             ld(c_vec_at[1][1]), ld(c_vec_at[1][0]), ld(c_vec_at[2][1]), ld(c_vec_at[2][0]),
             ld(c_vec_at[3][1]), ld(c_vec_at[3][0]),
             ld(c_bb_interp[1]), ld(c_bb_interp[2]), ld(c_bb_interp[3]),
             ld(c_bb_real[1]), ld(c_bb_real[2]), ld(c_bb_real[3]),
             ld(c_bb_input[1]), ld(c_bb_input[2]), ld(c_bb_input[3]),
             ld(c_copy_at[0]), ld(c_copy_at[1]), ld(c_copy_at[2]), ld(c_copy_at[3]),
             ld(c_scene_at[0]), ld(c_scene_at[1]), ld(c_scene_at[2]), ld(c_scene_at[3]));
    say(l);
    if (g_mode.load(std::memory_order_relaxed) >= 2)
    {
        snprintf(l, sizeof l,
                 "[MGPU][R313] r%u mode=%d | recent FG API calls=%d (idle %u seals, window %u) | nominal fraction %.3f | "
                 "kept vectors=%llu | thinned (NRFrameKeep=%u)=%llu | verified captures=%llu unknown=%llu; "
                 "unknown captures get fraction=1, no FG reuse or thinning",
                 rn, g_mode.load(std::memory_order_relaxed), fg_active() ? 1 : 0,
                 g_fg_idle.load(std::memory_order_relaxed), reuse_window(), (double)step_fraction(),
                 ld(c_reused), g_keep.load(std::memory_order_relaxed), ld(c_thinned),
                 ld(c_verified_seals), ld(c_unknown_seals));
        say(l);
    }
    if (rn <= MAX_BURSTS) burst(rn);
}

// First sight of an evaluating handle: everything its block said, once.
void first_sight(unsigned long long h, const eval_rec &r, char kind)
{
    const unsigned n = g_hseen_n.load(std::memory_order_acquire);
    for (unsigned i = 0; i < n && i < HSEEN; ++i)
        if (g_hseen[i].load(std::memory_order_relaxed) == h) return;
    if (n >= HSEEN) return;
    unsigned expect = n;
    if (!g_hseen_n.compare_exchange_strong(expect, n + 1u, std::memory_order_acq_rel)) return;
    g_hseen[n].store(h, std::memory_order_release);
    char l[1000];
    unsigned w1, h1, f1, w2, h2, f2, w3, h3, f3;
    char motion_name[192];
    if (kind == 'S' || kind == 'O')
    {
        desc_of(r.mvec, w1, h1, f1, motion_name, sizeof motion_name);
        desc_of(r.color, w2, h2, f2);
        desc_of(r.output, w3, h3, f3);
        snprintf(l, sizeof l,
                 "[MGPU][D20][NR1] FIRST EVALUATE of handle %p (%s) t=%.2fs thread %lu | generic names, have=0x%02X | "
                 "MotionVectors %p %ux%u fmt %u | Color %p %ux%u fmt %u | Output %p %ux%u fmt %u | Depth %p | "
                 "MV scale %.4f,%.4f | jitter %.4f,%.4f | reset %u",
                 (void *)(uintptr_t)h, kind == 'S' ? "scene feature" : "other feature",
                 now_s(), (unsigned long)GetCurrentThreadId(), r.have,
                 (void *)(uintptr_t)r.mvec, w1, h1, f1, (void *)(uintptr_t)r.color, w2, h2, f2,
                 (void *)(uintptr_t)r.output, w3, h3, f3, (void *)(uintptr_t)r.depth,
                 (double)r.mv_scale_x, (double)r.mv_scale_y, (double)r.jitter_x, (double)r.jitter_y, r.reset);
        say(l);
        snprintf(l, sizeof l, "[MGPU][R316-NGX-MV] kind=%c source=0x%llx name=%s %ux%u fmt=%u; declared input, not selected by size",
                 kind, r.mvec, motion_name, w1, h1, f1);
        say(l);
        return;
    }
    desc_of(r.g_mvecs, w1, h1, f1, motion_name, sizeof motion_name);
    desc_of(r.g_backbuffer, w2, h2, f2);
    desc_of(r.g_out_interp, w3, h3, f3);
    snprintf(l, sizeof l,
             "[MGPU][D20][NR1] FIRST EVALUATE of handle %p (frame generation) t=%.2fs thread %lu | DLSSG names, "
             "have=0x%04X | MVecs %p %ux%u fmt %u (subrect %ux%u) | Backbuffer %p %ux%u fmt %u | "
             "OutputInterpolated %p %ux%u fmt %u | OutputReal %p | Depth %p | HUDLess %p | MvecScale %.4f,%.4f | "
             "MultiFrameCount %u Index %u | BackbufferFrameID %llu | CameraMotionIncluded %u MvecJittered %u | "
             "Reset %u NotRenderingGameFrames %u | generic MotionVectors %p",
             (void *)(uintptr_t)h, now_s(), (unsigned long)GetCurrentThreadId(), r.g_have,
             (void *)(uintptr_t)r.g_mvecs, w1, h1, f1, r.g_mv_sub_w, r.g_mv_sub_h,
             (void *)(uintptr_t)r.g_backbuffer, w2, h2, f2,
             (void *)(uintptr_t)r.g_out_interp, w3, h3, f3, (void *)(uintptr_t)r.g_out_real,
             (void *)(uintptr_t)r.g_depth, (void *)(uintptr_t)r.g_hudless,
             (double)r.g_mv_scale_x, (double)r.g_mv_scale_y, r.g_mf_count, r.g_mf_index,
             r.g_bb_frame_id, r.g_cam_motion, r.g_mv_jittered, r.g_reset, r.g_not_rendering,
             (void *)(uintptr_t)r.mvec);
    say(l);
    snprintf(l, sizeof l, "[MGPU][R316-NGX-MV] kind=F source=0x%llx name=%s %ux%u fmt=%u; does not imply displayed frame generation",
             r.g_mvecs, motion_name, w1, h1, f1);
    say(l);
    if (r.g_have & (1u << 15))
    {
        const float *m = r.g_clip_to_prev;
        snprintf(l, sizeof l,
                 "[MGPU][D20][NR1] fg ClipToPrevClip (row major) | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | "
                 "%.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f",
                 (double)m[0], (double)m[1], (double)m[2], (double)m[3], (double)m[4], (double)m[5],
                 (double)m[6], (double)m[7], (double)m[8], (double)m[9], (double)m[10], (double)m[11],
                 (double)m[12], (double)m[13], (double)m[14], (double)m[15]);
        say(l);
    }
}
}   // namespace

void init()
{
    bool expect = false;
    if (!g_inited.compare_exchange_strong(expect, true, std::memory_order_acq_rel)) return;
    // D2.0 NR2: the clocks and arrays are set up whatever the key says, so
    // the panel can switch a mode on in a run that started at 0. The key
    // only decides the starting mode.
    const int mode = read_key();
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    g_qpc_freq = (f.QuadPart > 0) ? f.QuadPart : 10000000;
    g_qpc_to_s = 1.0 / (double)g_qpc_freq;
    g_t0 = qpc_now();
    for (unsigned p = 0; p < 4u; ++p)
    {
        c_vec_at[p][0].store(0ull); c_vec_at[p][1].store(0ull);
        c_bb_interp[p].store(0ull); c_bb_real[p].store(0ull); c_bb_input[p].store(0ull);
        c_copy_at[p].store(0ull); c_scene_at[p].store(0ull);
    }
    for (unsigned i = 0; i < HSEEN; ++i) g_hseen[i].store(0ull);
    for (unsigned i = 0; i < SLOT_N; ++i) { g_slot_frame[i].store(0ull); g_slot_pos[i].store(0u); }
    if (mode == 0) return;
    g_mode.store(mode, std::memory_order_release);
    if (mode >= 2)
    {
        char l[400];
        snprintf(l, sizeof l,
                 "[MGPU][R313] CAPTURE-VERIFIED FG policy (NRFrameFilter=%d%s). API calls alone never "
                 "change motion scale. Unknown or unmatched captures keep fraction=1 and cannot reuse "
                 "FG vectors or be thinned.%s",
                 mode, (mode == 3) ? ", thin" : "",
                 (mode == 3) ? " Mode 3 also evaluates only 1 generated frame in N (NRFrameKeep) and "
                               "re-presents the last output for the rest." : "");
        say(l);
        mgpu::journal::event("NR2 on", "NRFrameFilter>=2 in mgpu.ini: frame-gen vector fix");
    }
    say("[MGPU][D20][NR1] FRAME-GEN VECTOR MAP ON (NRFrameFilter=1). Read only: through the calibrator's "
        "existing EvaluateFeature tap it reads the scene feature's block (generic NGX names) and frame "
        "generation's (DLSSG names, nvsdk_ngx_defs_dlssg.h), and records evaluates, our vector copies and "
        "our seals in order. NR, the copies and the seals are unchanged. Needs the calibrator on (Calib) to "
        "see evaluates, an armed stream for seals, and real vectors (MVec=3) for the vector counts.");
    mgpu::journal::event("NR1 map on", "NRFrameFilter=1 in mgpu.ini: read-only frame-gen vector map");
}

bool on() { return g_mode.load(std::memory_order_acquire) >= 1; }

// ---- D2.0 NR2 / NR3: the acting modes, read by the consumer on the bridge ----
bool act()  { return g_mode.load(std::memory_order_acquire) >= 2; }
unsigned reuse_window()
{
    const unsigned mf = g_fg_mf_count.load(std::memory_order_relaxed);
    return 2u * ((mf == 0u ? 1u : mf) + 1u);
}
bool fg_active()
{
    return g_fg_handle.load(std::memory_order_acquire) != 0ull &&
           g_fg_idle.load(std::memory_order_relaxed) <= reuse_window();
}
bool fg_presented_active()
{
    return g_last_verified.load(std::memory_order_acquire) && fg_active();
}
float step_fraction()
{
    const unsigned mf = g_fg_mf_count.load(std::memory_order_relaxed);
    return 1.0f / (float)((mf == 0u ? 1u : mf) + 1u);
}
bool thin_this()
{
    if (g_mode.load(std::memory_order_acquire) != 3) return false;
    const unsigned n = g_keep.load(std::memory_order_relaxed);
    const unsigned long long c = g_gen_seen.fetch_add(1, std::memory_order_relaxed) + 1ull;
    const bool thin = (c % (unsigned long long)n) != 0ull;   // the N-th is kept
    if (thin) c_thinned.fetch_add(1, std::memory_order_relaxed);
    return thin;
}
void on_reuse() { c_reused.fetch_add(1, std::memory_order_relaxed); }
bool seal_generated(unsigned slot, unsigned long long frame_index)
{
    return policy_for_seal(slot, frame_index).generated;
}
captured_policy policy_for_seal(unsigned slot, unsigned long long frame_index)
{
    if (!act() || slot >= SLOT_N || frame_index == 0ull) return {};
    std::lock_guard<std::mutex> lock(g_evidence_mutex);
    const auto &record = g_sealed_policies[slot];
    return record.frame == frame_index ? record.policy : captured_policy{};
}

// ---- panel ----
int ui_mode() { return g_mode.load(std::memory_order_acquire); }
void ui_set_mode(int m)
{
    if (m < 0 || m > 3) return;
    if (!g_inited.load(std::memory_order_acquire)) return;
    const int was = g_mode.exchange(m, std::memory_order_acq_rel);
    if (was == m) return;
    char l[160];
    snprintf(l, sizeof l, "[MGPU][D20][NR2] panel: NRFrameFilter %d -> %d (live; the panel also writes it to mgpu.ini)", was, m);
    say(l);
}
unsigned ui_keep() { return g_keep.load(std::memory_order_relaxed); }
void ui_set_keep(unsigned n)
{
    if (n < 2u || n > 8u) return;
    const unsigned was = g_keep.exchange(n, std::memory_order_relaxed);
    if (was == n) return;
    char l[160];
    snprintf(l, sizeof l, "[MGPU][D20][NR3] panel: NRFrameKeep %u -> %u (live; the panel also writes it to mgpu.ini)", was, n);
    say(l);
}
void ui_counters(bool &fg_now, unsigned long long &reused, unsigned long long &thinned)
{
    fg_now = fg_presented_active();
    reused = c_reused.load(std::memory_order_relaxed);
    thinned = c_thinned.load(std::memory_order_relaxed);
}

void on_create(unsigned int feature_id, unsigned long long handle)
{
    if (!on() || feature_id != 11u || handle == 0ull) return;
    {
        std::lock_guard<std::mutex> lock(g_evidence_mutex);
        g_present_evidence.reset();
    }
    g_last_verified.store(false, std::memory_order_relaxed);
    g_fg_handle.store(handle, std::memory_order_release);
    char l[200];
    snprintf(l, sizeof l, "[MGPU][D20][NR1] frame generation created (NGX feature id 11): handle %p, t=%.2fs",
             (void *)(uintptr_t)handle, now_s());
    say(l);
}

void on_release(unsigned long long handle)
{
    if (handle == 0ull) return;
    auto expected = handle;
    if (!g_fg_handle.compare_exchange_strong(expected, 0ull, std::memory_order_acq_rel)) return;
    std::lock_guard<std::mutex> lock(g_evidence_mutex);
    g_present_evidence.reset();
    g_last_verified.store(false, std::memory_order_relaxed);
    g_fg_idle.store(255u, std::memory_order_relaxed);
}

void on_evaluate(unsigned long long handle, const eval_rec &r)
{
    if (!on()) return;
    const unsigned long long fgh = g_fg_handle.load(std::memory_order_acquire);
    char kind;
    if (r.scene) kind = 'S';
    else if (fgh != 0ull) kind = (handle == fgh) ? 'G' : 'O';
    else kind = 'O';
    first_sight(handle, r, kind);

    if (kind == 'S')
    {
        c_scene.fetch_add(1, std::memory_order_relaxed);
        c_scene_at[pos_now()].fetch_add(1, std::memory_order_relaxed);
        if (r.have & 4u) g_scene_mvec.store(r.mvec, std::memory_order_relaxed);
        if (r.have & 16u)
        {
            g_scene_sx.store(r.mv_scale_x, std::memory_order_relaxed);
            g_scene_sy.store(r.mv_scale_y, std::memory_order_relaxed);
        }
        ev_put('S', 0ull, 0u);
        return;
    }
    if (kind == 'O')
    {
        c_other.fetch_add(1, std::memory_order_relaxed);
        ev_put('O', 0ull, 0u);
        return;
    }

    c_fg.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_evidence_mutex);
        const bool count_known = (r.g_have & (1u << 10)) != 0u;
        const bool index_known = (r.g_have & (1u << 11)) != 0u;
        g_present_evidence.evaluated({
            r.g_backbuffer, r.g_out_real, r.g_out_interp,
            count_known ? r.g_mf_count : 0u, index_known ? r.g_mf_index : 0u,
            r.succeeded, (r.g_have & (1u << 12)) != 0u && r.g_not_rendering == 0u});
    }
    const bool has_mv = (r.g_have & 2u) != 0u && r.g_mvecs != 0ull;
    if (!has_mv) c_fg_mv_none.fetch_add(1, std::memory_order_relaxed);
    else if (r.g_mvecs == g_scene_mvec.load(std::memory_order_relaxed)) c_fg_mv_same.fetch_add(1, std::memory_order_relaxed);
    else c_fg_mv_diff.fetch_add(1, std::memory_order_relaxed);
    if ((r.g_have & (1u << 7)) && r.g_reset != 0u) c_fg_reset.fetch_add(1, std::memory_order_relaxed);
    if ((r.g_have & (1u << 12)) && r.g_not_rendering != 0u) c_fg_not_rendering.fetch_add(1, std::memory_order_relaxed);
    if (r.g_have & (1u << 6))
    {
        g_fg_sx.store(r.g_mv_scale_x, std::memory_order_relaxed);
        g_fg_sy.store(r.g_mv_scale_y, std::memory_order_relaxed);
    }
    if (r.g_have & (1u << 8))  g_fg_cam_motion.store(r.g_cam_motion, std::memory_order_relaxed);
    if (r.g_have & (1u << 9))  g_fg_jittered.store(r.g_mv_jittered, std::memory_order_relaxed);
    if (r.g_have & (1u << 10)) g_fg_mf_count.store(r.g_mf_count, std::memory_order_relaxed);
    if (r.g_have & (1u << 11)) g_fg_mf_index.store(r.g_mf_index, std::memory_order_relaxed);
    if (r.g_have & (1u << 13))
    {
        const unsigned long long prev = g_fg_frame_id.exchange(r.g_bb_frame_id, std::memory_order_relaxed);
        if (prev != 0ull)
        {
            if (r.g_bb_frame_id == prev + 1ull) c_fg_id_step1.fetch_add(1, std::memory_order_relaxed);
            else if (r.g_bb_frame_id == prev)   c_fg_id_same.fetch_add(1, std::memory_order_relaxed);
            else                                c_fg_id_other.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_fg_interp.store((r.g_have & (1u << 4)) ? r.g_out_interp : 0ull, std::memory_order_relaxed);
    g_fg_real.store((r.g_have & (1u << 5)) ? r.g_out_real : 0ull, std::memory_order_relaxed);
    g_fg_bb.store((r.g_have & 1u) ? r.g_backbuffer : 0ull, std::memory_order_relaxed);

    if (r.camera_valid != 0u)
    {
        std::lock_guard<std::mutex> lock(g_camera_mutex);
        g_camera = r.camera;
        g_camera.scene_sequence = c_scene.load(std::memory_order_relaxed);
        g_camera.backbuffer = r.g_backbuffer;
        g_camera.output_real = r.g_out_real;
        g_camera.output_interpolated = r.g_out_interp;
        g_camera.depth = r.g_depth;
        g_camera.motion = r.g_mvecs;
    }

    // Frame gen's vector handle changing: a resource recreation, or a ring of
    // its own. First 20 changes, with the scene's handle beside it.
    const unsigned long long prev_mv = g_fg_mvecs.exchange(has_mv ? r.g_mvecs : 0ull, std::memory_order_relaxed);
    if (prev_mv != 0ull && has_mv && r.g_mvecs != prev_mv &&
        g_fg_mv_changes.fetch_add(1, std::memory_order_relaxed) < 20u)
    {
        unsigned w, h, f;
        desc_of(r.g_mvecs, w, h, f);
        char l[300];
        snprintf(l, sizeof l, "[MGPU][D20][NR1] fg DLSSG.MVecs changed %p -> %p (%ux%u fmt %u) t=%.2fs | "
                              "scene's last %p", (void *)(uintptr_t)prev_mv, (void *)(uintptr_t)r.g_mvecs, w, h, f,
                 now_s(), (void *)(uintptr_t)g_scene_mvec.load(std::memory_order_relaxed));
        say(l);
    }

    g_since_fg.store(0u, std::memory_order_release);
    g_fg_idle.store(0u, std::memory_order_relaxed);   // NR2: fg is evaluating
    ev_put('G', (r.g_have & (1u << 13)) ? r.g_bb_frame_id : 0ull,
           (r.g_have & (1u << 11)) ? r.g_mf_index : 0u);
}

bool read_camera(camera_snapshot &out)
{
    std::lock_guard<std::mutex> lock(g_camera_mutex);
    if (g_camera.valid == 0u) return false;
    out = g_camera;
    return true;
}

std::uint64_t scene_sequence()
{
    return c_scene.load(std::memory_order_relaxed);
}

void on_mvec_copy(unsigned int slot)
{
    if (!on()) return;
    c_copy_at[pos_now()].fetch_add(1, std::memory_order_relaxed);
    ev_put('C', 0ull, slot);
}

void on_seal(unsigned long long frame_index, unsigned int slot, unsigned int mvec_valid,
             unsigned long long back_buffer)
{
    if (!on()) return;
    {
        std::lock_guard<std::mutex> lock(g_evidence_mutex);
        const auto policy = g_present_evidence.capture(back_buffer);
        if (slot < SLOT_N) g_sealed_policies[slot] = {frame_index, policy};
        g_last_verified.store(policy.verified, std::memory_order_relaxed);
        if (policy.verified) c_verified_seals.fetch_add(1, std::memory_order_relaxed);
        else c_unknown_seals.fetch_add(1, std::memory_order_relaxed);
    }
    {
        unsigned idle = g_fg_idle.load(std::memory_order_relaxed);   // NR2
        if (idle < 255u) g_fg_idle.store(idle + 1u, std::memory_order_relaxed);
    }
    unsigned k = g_since_fg.load(std::memory_order_relaxed);
    if (k < 3u) ++k;
    if (k > 3u) k = 3u;
    g_since_fg.store(k, std::memory_order_relaxed);
    if (slot < SLOT_N)   // NR3b: the label for the consumer, published after the position
    {
        g_slot_pos[slot].store(k, std::memory_order_relaxed);
        g_slot_frame[slot].store(frame_index, std::memory_order_release);
    }
    c_vec_at[k][mvec_valid ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
    c_seals.fetch_add(1, std::memory_order_relaxed);
    if (mvec_valid) c_seals_vec.fetch_add(1, std::memory_order_relaxed);
    char label = '.';
    if (back_buffer != 0ull)
    {
        if (back_buffer == g_fg_interp.load(std::memory_order_relaxed))
        { c_bb_interp[k].fetch_add(1, std::memory_order_relaxed); label = 'i'; }
        else if (back_buffer == g_fg_real.load(std::memory_order_relaxed))
        { c_bb_real[k].fetch_add(1, std::memory_order_relaxed); label = 'r'; }
        else if (back_buffer == g_fg_bb.load(std::memory_order_relaxed))
        { c_bb_input[k].fetch_add(1, std::memory_order_relaxed); label = 'b'; }
    }
    ev_put('V', frame_index, (mvec_valid ? 1u : 0u) | (k << 1) | ((unsigned)(unsigned char)label << 8));

    // Every 10 s from the first seal: summaries + burst. One caller wins each
    // slot (compare-exchange); the others return. Lines go to the queue.
    const long long now = qpc_now();
    long long next = g_next_report.load(std::memory_order_relaxed);
    if (next == 0)
    {
        g_next_report.compare_exchange_strong(next, now + 10ll * g_qpc_freq, std::memory_order_relaxed);
        return;
    }
    if (now < next) return;
    if (!g_next_report.compare_exchange_strong(next, now + 10ll * g_qpc_freq, std::memory_order_relaxed))
        return;
    report();
}
}   // namespace mgpu::fgmap
