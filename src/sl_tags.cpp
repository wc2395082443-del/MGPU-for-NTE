// MGPU Bridge - SLT1: the Streamline tag tap. See sl_tags.hpp.
//
// Self-contained like probe.cpp, calibrator.cpp, screen.cpp and sl_probe.cpp:
// no ReShade type, no gpu1_context type and no Streamline link
// library. Resource tag offsets are quoted from the public header beside them.
// FrameToken uses its documented SDK conversion, never a guessed vtable offset.
#include "sl_tags.hpp"
#include "game_work_trace.hpp"
#include "frame_measurement.hpp"
#include "diag.hpp"
#include "tagged_motion_policy.hpp"
#include "source_marker_slot.hpp"
#include "streamline_frame_identity.hpp"
#include "native_reflex.hpp"
#include "../experimental/official_fg/official_bridge.hpp"
#include "../ext/streamline/sl_core_api.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>   // _wcsnicmp, for skipping sl.* modules by name
#include <atomic>
#include <mutex>

namespace mgpu::sltags
{
namespace
{
    // ---- THE ABI, QUOTED ----
    //
    // sl_struct.h:
    //   struct BaseStructure {
    //       BaseStructure* next{};          // +0   8 bytes
    //       StructType     structType{};    // +8  16 bytes (u32,u16,u16,u8[8])
    //       size_t         structVersion;   // +24  8 bytes
    //   };                                  // = 32 bytes, no virtuals
    //
    //   SL_STRUCT_BEGIN inserts only a constructor and a CONSTEXPR STATIC
    //   s_structType, so a tagged struct is BaseStructure followed by its own
    //   members and nothing else. That is why these offsets are just sums.
    const unsigned BASE_SIZE   = 32u;
    const unsigned OFF_TYPE    = 8u;    // StructType, within BaseStructure

    // sl_core_types.h:
    //   SL_STRUCT_BEGIN(ResourceTag, {4c6a5aad-b445-496c-87ff-1af3845be653}, 1)
    //       Resource*         resource{};   // +32
    //       BufferType        type{};       // +40  (using BufferType = uint32_t)
    //       ResourceLifecycle lifecycle{};  // +44  (plain enum -> int)
    //       Extent            extent{};     // +48  SIZE NOT PINNED - see below
    const unsigned TAG_RESOURCE  = 32u;
    const unsigned TAG_TYPE      = 40u;
    const unsigned TAG_LIFECYCLE = 44u;
    const unsigned TAG_MIN_SIZE  = 48u;   // everything we read lies below this

    // sl_core_types.h:
    //   SL_STRUCT_BEGIN(Resource, {3a9d70cf-2418-4b72-8391-13f8721c7261}, 1)
    //       ResourceType type;            // +32  enum class : char  -> 1 byte
    //       void*        native{};        // +40  (7 bytes of padding above)
    //       void*        memory{};        // +48
    //       void*        view{};          // +56
    //       uint32_t     state;           // +64
    //       uint32_t     width{};         // +68
    //       uint32_t     height{};        // +72
    //       uint32_t     nativeFormat{};  // +76
    //       uint32_t     mipLevels{};     // +80
    //       uint32_t     arrayLayers{};   // +84
    //       uint64_t     gpuVirtualAddress{}; // +88
    //       uint32_t     flags;           // +96
    //       uint32_t     usage{};         // +100
    //       uint32_t     reserved{};      // +104
    const unsigned RES_NATIVE = 40u;
    const unsigned RES_STATE  = 64u;   // R300d: uint32 state, after view (sl_core_types.h)
    const unsigned RES_WIDTH  = 68u;
    const unsigned RES_HEIGHT = 72u;
    const unsigned RES_FORMAT = 76u;
    const unsigned RES_MIN_SIZE = 80u;

    // The two GUIDs, as 16 raw bytes each, in the layout StructType stores
    // them: uint32 then uint16 then uint16 then 8 bytes, all little-endian on
    // x64. Comparing 16 bytes is how an element is validated before it is
    // read, and how the array stride is measured instead of assumed.
    const unsigned char GUID_RESOURCE_TAG[16] = {
        0xAD,0x5A,0x6A,0x4C, 0x45,0xB4, 0x6C,0x49,
        0x87,0xFF,0x1A,0xF3,0x84,0x5B,0xE6,0x53 };
    const unsigned char GUID_RESOURCE[16] = {
        0xCF,0x70,0x9D,0x3A, 0x18,0x24, 0x72,0x4B,
        0x83,0x91,0x13,0xF8,0x72,0x1C,0x72,0x61 };

    // sl_core_types.h constants. Only the ones this instrument names.
    const unsigned BT_DEPTH          = 0u;
    const unsigned BT_MVEC           = 1u;
    const unsigned BT_HUDLESS        = 2u;
    const unsigned BT_SCALING_IN     = 3u;
    const unsigned BT_SCALING_OUT    = 4u;
    const unsigned BT_UI_COLOR_ALPHA = 23u;

    const char *buffer_name(unsigned t)
    {
        switch (t)
        {
        case BT_DEPTH:          return "Depth";
        case BT_MVEC:           return "MotionVectors";
        case BT_HUDLESS:        return "HUDLessColor";
        case BT_SCALING_IN:     return "ScalingInputColor";
        case BT_SCALING_OUT:    return "ScalingOutputColor";
        case BT_UI_COLOR_ALPHA: return "UIColorAndAlpha";
        default:                return "other";
        }
    }

    // sl_core_api.h, verbatim:
    //   SL_API sl::Result slSetTagForFrame(const sl::FrameToken& frame,
    //                                      const sl::ViewportHandle& viewport,
    //                                      const sl::ResourceTag* resources,
    //                                      uint32_t numResources,
    //                                      sl::CommandBuffer* cmdBuffer);
    //   SL_API sl::Result slSetTag(const sl::ViewportHandle& viewport,
    //                              const sl::ResourceTag* tags,
    //                              uint32_t numTags,
    //                              sl::CommandBuffer* cmdBuffer);
    //
    // SL_API is `extern "C"`, sl::Result is an enum, and a C++ reference is a
    // pointer at the ABI. x64 has one calling convention, so these two
    // typedefs are the call exactly. Nothing is inferred.
    typedef int (*pf_set_tag_for_frame)(const void *, const void *,
                                        const void *, unsigned, void *);
    typedef int (*pf_set_tag)(const void *, const void *, unsigned, void *);

    // ---- STATE ----
    std::mutex g_cs;
    std::atomic<int>  g_mode{0};
    std::atomic<bool> g_installed{false};
    std::atomic<bool> g_done{false};      // install attempted, win or lose

    pf_set_tag_for_frame g_real_stff = nullptr;
    pf_set_tag           g_real_st   = nullptr;
    using pf_set_constants = int (*)(const void *, const void *, const void *);
    pf_set_constants g_real_constants = nullptr;
    std::atomic<constants_capture_fn> g_constants_capture{nullptr};
    HMODULE g_self = nullptr;
    struct cached_slot { void **slot = nullptr; void *original = nullptr; void *hook = nullptr; };
    cached_slot g_cached_slots[9];
    using pf_get_feature_function = decltype(&slGetFeatureFunction);
    using pf_source_sleep = sl::Result (*)(const sl::FrameToken &);
    using pf_source_marker = sl::Result (*)(unsigned, const sl::FrameToken &);
    pf_get_feature_function g_real_get_feature_function = nullptr;
    std::atomic<pf_source_sleep> g_real_source_sleep{nullptr};
    std::atomic<pf_source_marker> g_real_source_marker{nullptr}, g_real_source_legacy_marker{nullptr};
    std::atomic<unsigned> g_changed_logs{0};
    using pf_evaluate_feature = decltype(&slEvaluateFeature);
    using pf_get_frame_token = decltype(&slGetNewFrameToken);
    pf_evaluate_feature g_real_evaluate_feature = nullptr;
    pf_get_frame_token g_real_get_frame_token = nullptr;
    std::atomic<unsigned long long> g_stff_calls{0}, g_st_calls{0};
    std::atomic<unsigned long long> g_empty_arrays{0}, g_null_arrays{0}, g_tag_errors{0};
    std::atomic<unsigned long long> g_constants_calls{0}, g_constants_valid{0}, g_constants_errors{0};
    std::atomic<unsigned long long> g_evaluate_calls{0}, g_evaluate_errors{0};
    std::atomic<unsigned long long> g_token_calls{0}, g_token_errors{0};
    std::atomic<unsigned> g_last_feature{0};

    std::atomic<unsigned long long> g_calls{0};
    std::atomic<unsigned long long> g_tags_read{0};
    std::atomic<unsigned long long> g_slots{0};
    std::atomic<unsigned long long> g_stride{0};   // measured, 0 until it is
    std::atomic<unsigned long long> g_skipped{0};  // elements not validated

    std::atomic<unsigned long long> g_h_mvec{0};
    std::atomic<unsigned long long> g_h_hudless{0};
    std::atomic<unsigned long long> g_h_scale_in{0};
    std::atomic<resource_capture_fn> g_resource_capture{nullptr};
    std::atomic<mvec_capture_fn> g_mvec_capture{nullptr};       // R300d
    std::atomic<unsigned long long> g_mvec_capture_calls{0};    // R300d

    // One "said" bit per buffer type we name, plus a catch-all.
    bool g_said[24] = {};
    unsigned long long g_last[24] = {};

    // ---- READING, UNDER A HANDLER, ALWAYS ----
    //
    // Every dereference below is of memory another module owns. The layout is
    // quoted rather than guessed, but a quoted layout against a future SDK is
    // still a bet, and a bet taken on the game's own render thread must not be
    // able to take the game down. The handler is what makes the bet bounded:
    // the worst outcome is that the tap reads nothing and says so.
    bool bytes_equal(const void *p, const unsigned char *want, unsigned n)
    {
        __try { return memcmp(p, want, n) == 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool is_tag(const void *p)
    {
        if (p == nullptr) return false;
        return bytes_equal((const unsigned char *)p + OFF_TYPE,
                           GUID_RESOURCE_TAG, 16u);
    }

    // Measure sizeof(ResourceTag) from the data, once. The array is
    // contiguous by definition - it is a C array passed by pointer with a
    // count - so the distance to the next element's structType GUID IS the
    // stride. Bounded to a window that cannot be a struct, and 8-aligned
    // because BaseStructure begins with a pointer.
    unsigned measure_stride(const void *first)
    {
        const unsigned char *p = (const unsigned char *)first;
        __try
        {
            for (unsigned off = TAG_MIN_SIZE; off <= 512u; off += 8u)
                if (memcmp(p + off + OFF_TYPE, GUID_RESOURCE_TAG, 16u) == 0)
                    return off;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0u; }
        return 0u;
    }

    // POD only, and no object with a destructor anywhere near the handler:
    // a function that mixes SEH with C++ unwinding does not compile (C2712),
    // and the lock below is exactly such an object. So the dereferences live
    // alone in here and the bookkeeping lives in the caller.
    struct tag_fields
    {
        unsigned type;
        int      life;
        unsigned long long native;
        unsigned state, w, h, fmt;
        unsigned top, left, extent_w, extent_h;
        size_t tag_version, resource_version;
        unsigned long long resource_next;
        unsigned resource_type;
        bool zero_resource_header;
        unsigned char resource_guid[16];
        const char *reason;
    };

    bool read_fields(const void *tag, tag_fields &out)
    {
        out = {};
        out.type = 0xFFFFFFFFu;
        out.life = -1;
        out.resource_type = 0xFFFFFFFFu;
        out.reason = "read_fault";
        __try
        {
            const unsigned char *t = (const unsigned char *)tag;
            if (!is_tag(tag)) { out.reason = "tag_guid"; return false; }
            out.tag_version = *(const size_t *)(t + 24u);
            if (out.tag_version < 1u) { out.reason = "tag_version_zero"; return false; }
            out.type = *(const unsigned *)(t + TAG_TYPE);
            out.life = *(const int *)(t + TAG_LIFECYCLE);
            out.top = *(const unsigned *)(t + 48u);
            out.left = *(const unsigned *)(t + 52u);
            out.extent_w = *(const unsigned *)(t + 56u);
            out.extent_h = *(const unsigned *)(t + 60u);
            const void *res = *(const void *const *)(t + TAG_RESOURCE);
            if (res != nullptr)
            {
                const unsigned char *r = (const unsigned char *)res;
                out.resource_next = *(const unsigned long long *)r;
                memcpy(out.resource_guid, r + OFF_TYPE, 16u);
                out.resource_version = *(const size_t *)(r + 24u);
                out.resource_type = r[32u];
                const unsigned char zero_header[BASE_SIZE] = {};
                out.zero_resource_header = out.tag_version == 1u && out.resource_type == 0u &&
                    memcmp(r, zero_header, BASE_SIZE) == 0;
                if (!out.zero_resource_header && memcmp(out.resource_guid, GUID_RESOURCE, 16u) != 0)
                { out.reason = "resource_guid"; return false; }
                if (!out.zero_resource_header && out.resource_version < 1u)
                { out.reason = "resource_version_zero"; return false; }
                if (out.resource_type != 0u && out.resource_type != 8u)
                { out.reason = "resource_type"; return false; }
                out.native = (unsigned long long)(uintptr_t)
                             *(const void *const *)(r + RES_NATIVE);
                out.state = *(const unsigned *)(r + RES_STATE);
                out.w   = *(const unsigned *)(r + RES_WIDTH);
                out.h   = *(const unsigned *)(r + RES_HEIGHT);
                out.fmt = *(const unsigned *)(r + RES_FORMAT);
            }
            out.reason = "ok";
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void read_one(const void *tag, void *cmd, motion_packet::identity engine = {})
    {
        tag_fields f{};
        if (!read_fields(tag, f))
        {
            g_skipped.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<unsigned> failures_logged{0};
            if (failures_logged.fetch_add(1, std::memory_order_relaxed) < 4u)
            {
                char line[400];
                snprintf(line, sizeof line,
                         "[MGPU][R319-TAG-REJECT] reason=%s tag_version=%zu buffer_type=%u "
                         "resource_version=%zu resource_type=%u resource_next=0x%llx resource_guid_bytes="
                         "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                         f.reason, f.tag_version, f.type, f.resource_version, f.resource_type, f.resource_next,
                         f.resource_guid[0], f.resource_guid[1], f.resource_guid[2], f.resource_guid[3],
                         f.resource_guid[4], f.resource_guid[5], f.resource_guid[6], f.resource_guid[7],
                         f.resource_guid[8], f.resource_guid[9], f.resource_guid[10], f.resource_guid[11],
                         f.resource_guid[12], f.resource_guid[13], f.resource_guid[14], f.resource_guid[15]);
                mgpu::diag::warn(line);
            }
            return;
        }
        const unsigned type = f.type;
        const int life = f.life;
        const unsigned long long native = f.native;
        const unsigned w = f.w, h = f.h, fmt = f.fmt;
        if (const auto callback = g_resource_capture.load(std::memory_order_acquire); callback != nullptr)
            callback(cmd, type, {native, f.state, life, f.top, f.left, f.extent_w, f.extent_h}, engine);
        if (type == BT_MVEC)
        {
            static std::atomic<unsigned> motion_headers_logged{0};
            if (motion_headers_logged.fetch_add(1, std::memory_order_relaxed) < 4u)
            {
                char line[320];
                snprintf(line, sizeof line,
                         "[MGPU][R319-TAG-HEADER] tag_version=%zu resource_version=%zu resource_type=%u header=%s "
                         "source=0x%llx cmd=%p state=0x%X lifecycle=%d extent=(%u,%u,%u,%u)",
                         f.tag_version, f.resource_version, f.resource_type,
                         f.zero_resource_header ? "zero-prefix" : "typed", native, cmd, f.state,
                         life, f.top, f.left, f.extent_w, f.extent_h);
                mgpu::diag::info(line);
            }
        }

        // R300d (SLTagsCapture only: the callback is installed by dllmain only with the key). The Streamline
        // call is the one point where identity, declared D3D12 state, lifetime and the command list coexist.
        if (type == BT_MVEC && native != 0ull && cmd != nullptr)
        {
            const mvec_capture_fn cb = g_mvec_capture.load(std::memory_order_acquire);
            if (cb != nullptr)
            {
                cb(cmd, native, f.state, life, f.top, f.left, f.extent_w, f.extent_h, engine);
                g_mvec_capture_calls.fetch_add(1, std::memory_order_relaxed);
            }
        }

        g_tags_read.fetch_add(1, std::memory_order_relaxed);

        switch (type)
        {
        case BT_MVEC:       g_h_mvec.store(native, std::memory_order_relaxed); break;
        case BT_HUDLESS:    g_h_hudless.store(native, std::memory_order_relaxed); break;
        case BT_SCALING_IN: g_h_scale_in.store(native, std::memory_order_relaxed); break;
        default: break;
        }

        const unsigned slot = (type < 24u) ? type : 23u;
        bool say = false;
        {
            std::lock_guard<std::mutex> lk(g_cs);
            if (!g_said[slot]) { g_said[slot] = true; say = true; }
            else if (g_mode.load(std::memory_order_relaxed) >= 2 &&
                     g_last[slot] != native) say = true;
            g_last[slot] = native;
        }
        if (!say) return;
        if (g_mode.load(std::memory_order_relaxed) == 3 &&
            g_changed_logs.fetch_add(1, std::memory_order_relaxed) >= 24u) return;

        char line[900];
        snprintf(line, sizeof line,
            "[MGPU][SLT1] TAG %s (BufferType=%u) native=0x%llx %ux%u nativeFormat=%u "
            "lifecycle=%d. THIS IS THE GAME'S OWN DECLARATION, not a ranking. For "
            "MotionVectors it is the address every round from R70 to R103 tried to "
            "infer, stated by the title itself with its extent and format attached; "
            "compare it against the transport source on the R71 line, and where they "
            "differ THIS one is right. For HUDLessColor and ScalingInputColor it is a "
            "colour buffer NAMED rather than deduced from a format, which is what an "
            "HDR path needs before it can be built. lifecycle 0 is valid only now, 1 "
            "until present, 2 until evaluate - anything above 0 is a resource that "
            "can be read outside the tagging call.",
            buffer_name(type), type, native, w, h, fmt, life);
        mgpu::diag::info(line);
    }

    void read_array(const void *tags, unsigned n, void *cmd, motion_packet::identity engine = {})
    {
        if (tags == nullptr || n == 0u) return;
        g_calls.fetch_add(1, std::memory_order_relaxed);

        if (!is_tag(tags))
        {
            // The first element does not carry the ResourceTag GUID. Either
            // the SDK's layout moved or this is not what we think it is.
            // Either way: read nothing, say it once, and leave the call alone.
            static std::atomic<bool> said{false};
            if (!said.exchange(true))
                mgpu::diag::warn(
                    "[MGPU][SLT1] TAG ARRAY NOT RECOGNISED: the first element does not carry "
                    "the documented ResourceTag structType GUID, so the layout this build "
                    "quotes does not match the Streamline in this process. NOTHING WAS READ "
                    "and the call was passed through untouched. This is the guard working, "
                    "not a fault - and it is the finding: the ABI moved, and the offsets in "
                    "sl_tags.cpp need re-reading against the SDK this title ships.");
            return;
        }

        read_one(tags, cmd, engine);
        if (n == 1u) return;

        unsigned stride = (unsigned)g_stride.load(std::memory_order_relaxed);
        if (stride == 0u)
        {
            stride = measure_stride(tags);
            if (stride == 0u)
            {
                // One tag read, the rest skipped. Information lost, nothing
                // risked - which is the right way round.
                g_skipped.fetch_add(n - 1u, std::memory_order_relaxed);
                return;
            }
            g_stride.store(stride, std::memory_order_relaxed);
            char sl[420];
            snprintf(sl, sizeof sl,
                "[MGPU][SLT1] TAG STRIDE MEASURED: sizeof(sl::ResourceTag) = %u bytes, found "
                "by locating the next element's structType GUID rather than by assuming "
                "sizeof(sl::Extent). Every element is still validated against that GUID "
                "before it is read, so a wrong stride skips tags instead of reading memory "
                "that is not a tag.", stride);
            mgpu::diag::info(sl);
        }

        const unsigned char *p = (const unsigned char *)tags;
        for (unsigned i = 1u; i < n && i < 64u; ++i)
        {
            const void *e = p + (size_t)i * stride;
            if (!is_tag(e)) { g_skipped.fetch_add(1, std::memory_order_relaxed); break; }
            read_one(e, cmd, engine);
        }
    }

    // ---- THE THUNKS ----
    //
    // Read, then call the real function with every argument exactly as it
    // arrived. No argument is copied, rewritten or held.
    int hook_stff(const void *frame, const void *viewport,
                  const void *tags, unsigned n, void *cmd)
    {
        g_stff_calls.fetch_add(1, std::memory_order_relaxed);
        if (n == 0u) g_empty_arrays.fetch_add(1, std::memory_order_relaxed);
        if (tags == nullptr) g_null_arrays.fetch_add(1, std::memory_order_relaxed);
        read_array(tags, n, cmd, read_identity(frame, viewport));
        const int result = (g_real_stff != nullptr)
                   ? g_real_stff(frame, viewport, tags, n, cmd) : 0;
        if (result != 0) g_tag_errors.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    int hook_st(const void *viewport, const void *tags, unsigned n, void *cmd)
    {
        g_st_calls.fetch_add(1, std::memory_order_relaxed);
        if (n == 0u) g_empty_arrays.fetch_add(1, std::memory_order_relaxed);
        if (tags == nullptr) g_null_arrays.fetch_add(1, std::memory_order_relaxed);
        read_array(tags, n, cmd, read_identity(nullptr, viewport));
        const int result = (g_real_st != nullptr) ? g_real_st(viewport, tags, n, cmd) : 0;
        if (result != 0) g_tag_errors.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    int hook_constants(const void *values, const void *frame, const void *viewport)
    {
        g_constants_calls.fetch_add(1, std::memory_order_relaxed);
        const auto constants = read_constants(values, read_identity(frame, viewport));
        const int result = g_real_constants(values, frame, viewport);
        if (result != 0) g_constants_errors.fetch_add(1, std::memory_order_relaxed);
        if (result == 0 && constants.valid) g_constants_valid.fetch_add(1, std::memory_order_relaxed);
        if (result == 0 && constants.valid && constants.camera.valid && mgpu::measurement::continuous() && mgpu::measurement::session())
        {
            char line[1500];
            const auto &camera = constants.camera;
            std::snprintf(line, sizeof line,
                "[MGPU][M360] kind=camera session=%llu engine_frame=%u viewport=%u capture_ns=%llu observed_qpc=%llu "
                "forward=%.9g,%.9g,%.9g position=%.9g,%.9g,%.9g reset=%u",
                mgpu::measurement::session(), constants.key.frame, constants.key.viewport, camera.capture_ns,
                mgpu::timing::cpu_ticks(), camera.forward[0], camera.forward[1], camera.forward[2],
                camera.position[0], camera.position[1], camera.position[2], unsigned(constants.reset));
            mgpu::measurement::emit(line);
        }
        if (result == 0)
            if (const auto callback = g_constants_capture.load(std::memory_order_acquire); callback != nullptr)
                callback(constants);
        return result;
    }

    sl::Result hook_evaluate_feature(sl::Feature feature, const sl::FrameToken &frame,
        const sl::BaseStructure **inputs, uint32_t count, sl::CommandBuffer *command)
    {
        g_evaluate_calls.fetch_add(1, std::memory_order_relaxed);
        g_last_feature.store(feature, std::memory_order_relaxed);
        if (feature == sl::kFeatureDLSS)
            mgpu::game_work::marker(reinterpret_cast<ID3D12GraphicsCommandList *>(command),
                mgpu::game_work::phase::sl_sr_begin);
        const auto result = g_real_evaluate_feature(feature, frame, inputs, count, command);
        if (feature == sl::kFeatureDLSS)
            mgpu::game_work::marker(reinterpret_cast<ID3D12GraphicsCommandList *>(command),
                mgpu::game_work::phase::sl_sr_end);
        if (result != sl::Result::eOk) g_evaluate_errors.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    sl::Result hook_source_sleep(const sl::FrameToken &frame)
    {
        const auto original = g_real_source_sleep.load(std::memory_order_acquire);
        if (original == nullptr) return sl::Result::eErrorNotInitialized;
        mgpu::native_reflex::on_source_sleep();
        const bool observe = mgpu::official_bridge::selected();
        const auto begin_ns = observe ? mgpu::official_bridge::source_clock_ns() : 0;
        const auto result = original(frame);
        const auto end_ns = observe ? mgpu::official_bridge::source_clock_ns() : 0;
        const auto identity = read_identity(&frame, nullptr);
        if (result == sl::Result::eOk && identity.frame_known)
            mgpu::official_bridge::source_sleep(identity.frame, begin_ns, end_ns);
        return result;
    }

    sl::Result hook_source_marker(unsigned marker, const sl::FrameToken &frame)
    {
        const auto original = g_real_source_marker.load(std::memory_order_acquire);
        if (original == nullptr) return sl::Result::eErrorNotInitialized;
        const bool observe = mgpu::official_bridge::selected() && mgpu::official_fg::source_marker_supported(marker);
        const auto begin_ns = observe ? mgpu::official_bridge::source_clock_ns() : 0;
        const auto result = original(marker, frame);
        const auto end_ns = observe ? mgpu::official_bridge::source_clock_ns() : 0;
        const auto identity = read_identity(&frame, nullptr);
        if (result == sl::Result::eOk && identity.frame_known && mgpu::official_fg::source_marker_supported(marker))
            mgpu::official_bridge::source_marker(identity.frame, marker, begin_ns, end_ns);
        return result;
    }

    sl::Result hook_source_legacy_marker(unsigned marker, const sl::FrameToken &frame)
    {
        const auto original = g_real_source_legacy_marker.load(std::memory_order_acquire);
        if (original == nullptr) return sl::Result::eErrorNotInitialized;
        const bool observe = mgpu::official_bridge::selected() && mgpu::official_fg::source_marker_supported(marker);
        const auto begin_ns = observe ? mgpu::official_bridge::source_clock_ns() : 0;
        const auto result = original(marker, frame);
        const auto end_ns = observe ? mgpu::official_bridge::source_clock_ns() : 0;
        const auto identity = read_identity(&frame, nullptr);
        if (result == sl::Result::eOk && identity.frame_known && mgpu::official_fg::source_marker_supported(marker))
            mgpu::official_bridge::source_marker(identity.frame, marker, begin_ns, end_ns);
        return result;
    }

    sl::Result hook_get_feature_function(sl::Feature feature, const char *name, void *&function)
    {
        const auto result = g_real_get_feature_function(feature, name, function);
        if (result != sl::Result::eOk || name == nullptr || function == nullptr) return result;
        if (feature == sl::kFeatureReflex && std::strcmp(name, "slReflexSleep") == 0)
        {
            g_real_source_sleep.store(reinterpret_cast<pf_source_sleep>(function), std::memory_order_release);
            function = reinterpret_cast<void *>(&hook_source_sleep);
        }
        else if (feature == sl::kFeaturePCL && std::strcmp(name, "slPCLSetMarker") == 0)
        {
            g_real_source_marker.store(reinterpret_cast<pf_source_marker>(function), std::memory_order_release);
            function = reinterpret_cast<void *>(&hook_source_marker);
        }
        else if (feature == sl::kFeatureReflex && std::strcmp(name, "slReflexSetMarker") == 0)
        {
            g_real_source_legacy_marker.store(reinterpret_cast<pf_source_marker>(function), std::memory_order_release);
            function = reinterpret_cast<void *>(&hook_source_legacy_marker);
        }
        return result;
    }

    sl::Result hook_get_frame_token(sl::FrameToken *&token, const uint32_t *index)
    {
        g_token_calls.fetch_add(1, std::memory_order_relaxed);
        const auto result = g_real_get_frame_token(token, index);
        if (result != sl::Result::eOk) g_token_errors.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    // ---- THE IMPORT SWAP ----
    //
    // The same mechanism as the calibrator's rung A, and deliberately NOT its
    // rung B: no data section is written, so no module's memory is modified
    // while that module may be running. If the game resolved these entry
    // points dynamically rather than importing them, this finds nothing, and
    // finding nothing is reported as the measurement it is.
    unsigned patch_module(HMODULE mod, void *find, void *repl)
    {
        if (mod == nullptr || mod == g_self) return 0;
        unsigned char *base = (unsigned char *)mod;
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

        const IMAGE_DATA_DIRECTORY dir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (dir.VirtualAddress == 0 || dir.Size == 0) return 0;

        volatile unsigned hits = 0;
        IMAGE_IMPORT_DESCRIPTOR *imp =
            (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
        __try
        {
            for (; imp->Name != 0; ++imp)
            {
                if (imp->FirstThunk == 0) continue;
                IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
                for (; t->u1.Function != 0; ++t)
                {
                    if ((void *)(uintptr_t)t->u1.Function != find) continue;
                    DWORD old = 0;
                    if (!VirtualProtect(&t->u1.Function, sizeof(void *),
                                        PAGE_READWRITE, &old))
                        continue;
                    t->u1.Function = (ULONGLONG)(uintptr_t)repl;
                    VirtualProtect(&t->u1.Function, sizeof(void *), old, &old);
                    ++hits;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return (unsigned)hits; }
        return (unsigned)hits;
    }

    unsigned swap_everywhere(void *find, void *repl)
    {
        if (find == nullptr) return 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snap == INVALID_HANDLE_VALUE) return 0;
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        unsigned hits = 0;
        if (Module32FirstW(snap, &me))
        {
            do
            {
                // Skip Streamline's own modules. Their internal calls are not
                // the game declaring anything, and a thunk in that path would
                // put us inside the very interposer we are staying out of.
                if (_wcsnicmp(me.szModule, L"sl.", 3) == 0) continue;
                hits += patch_module(me.hModule, find, repl);
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
        return hits;
    }

    bool find_main_slots(void *target, mgpu::tagged_motion::pointer_matches &matches)
    {
        __try
        {
            auto *base = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr));
            if (base == nullptr) return false;
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE ||
                nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
            const auto *sections = IMAGE_FIRST_SECTION(nt);
            size_t remaining = 32u * 1024u * 1024u;
            for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index)
            {
                const auto &section = sections[index];
                if ((section.Characteristics & IMAGE_SCN_MEM_WRITE) == 0 ||
                    (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) continue;
                if (section.VirtualAddress > nt->OptionalHeader.SizeOfImage ||
                    section.Misc.VirtualSize > nt->OptionalHeader.SizeOfImage - section.VirtualAddress ||
                    section.Misc.VirtualSize > remaining) return false;
                remaining -= section.Misc.VirtualSize;
                matches.scan(base + section.VirtualAddress, section.Misc.VirtualSize, target);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void **verified_pcl_marker_slot(void *target)
    {
        __try
        {
            auto *base = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr));
            if (base == nullptr || target == nullptr) return nullptr;
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
            const auto *image = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
            if (image->Signature != IMAGE_NT_SIGNATURE ||
                image->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
                image->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
                image->OptionalHeader.SizeOfImage != 0x10d43000) return nullptr;
            using namespace mgpu::tagged_motion;
            if (!pcl_marker_route_matches(image->FileHeader.TimeDateStamp, image->OptionalHeader.SizeOfImage,
                                         base + pcl_resolver_rva, base + pcl_caller_rva)) return nullptr;
            const auto *sections = IMAGE_FIRST_SECTION(image);
            for (unsigned index = 0; index < image->FileHeader.NumberOfSections; ++index)
            {
                const auto &section = sections[index];
                if ((section.Characteristics & IMAGE_SCN_MEM_WRITE) == 0 ||
                    (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 ||
                    pcl_cache_rva < section.VirtualAddress) continue;
                const auto offset = pcl_cache_rva - section.VirtualAddress;
                if (offset > section.Misc.VirtualSize ||
                    sizeof(void *) > section.Misc.VirtualSize - offset) continue;
                auto **slot = reinterpret_cast<void **>(base + pcl_cache_rva);
                return *slot == target ? slot : nullptr;
            }
            return nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    unsigned install_cached_slot(unsigned index, void *target, void *hook, const char *name)
    {
        mgpu::tagged_motion::pointer_matches matches;
        const bool scanned = target != nullptr && find_main_slots(target, matches);
        void **verified = scanned && index == 7 ? verified_pcl_marker_slot(target) : nullptr;
        void **slot = scanned ? mgpu::tagged_motion::select_cached_slot(matches.unique_slot(), verified, target) : nullptr;
        bool installed = false;
        if (slot != nullptr && InterlockedCompareExchangePointer(slot, hook, target) == target)
        {
            g_cached_slots[index] = {slot, target, hook};
            installed = true;
        }
        char line[320];
        snprintf(line, sizeof line,
                 "[MGPU][R317-SL-CACHE] %s scan_ok=%u matches=%u installed=%u slot=%p verified_pcl_route=%u; "
                 "main executable writable data only, exact atomic pointer swap, no code/protection changes",
                 name, scanned ? 1u : 0u, matches.count, installed ? 1u : 0u, slot, verified ? 1u : 0u);
        mgpu::diag::info(line);
        return installed ? 1u : 0u;
    }

    void install_now()
    {
        HMODULE sl = GetModuleHandleW(L"sl.interposer.dll");
        if (sl == nullptr)
        {
            mgpu::diag::info("[MGPU][SLT1] TAP NOT INSTALLED: sl.interposer.dll is not "
                             "loaded. Nothing was touched.");
            return;
        }

        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&install_now, &g_self);

        g_real_stff = (pf_set_tag_for_frame)GetProcAddress(sl, "slSetTagForFrame");
        g_real_st   = (pf_set_tag)GetProcAddress(sl, "slSetTag");
        if (g_mode.load(std::memory_order_relaxed) == 3)
        {
            g_real_evaluate_feature = reinterpret_cast<pf_evaluate_feature>(GetProcAddress(sl, "slEvaluateFeature"));
            g_real_get_frame_token = reinterpret_cast<pf_get_frame_token>(GetProcAddress(sl, "slGetNewFrameToken"));
            g_real_get_feature_function = reinterpret_cast<pf_get_feature_function>(GetProcAddress(sl, "slGetFeatureFunction"));
        }
        if (g_constants_capture.load(std::memory_order_acquire) != nullptr)
            g_real_constants = reinterpret_cast<pf_set_constants>(GetProcAddress(sl, "slSetConstants"));

        mgpu::diag::info(
            "[MGPU][SLT1] TAP step 1/2 BEGIN: swapping import slots for slSetTagForFrame. "
            "One pointer store per slot, in the GAME's modules only - sl.* modules are "
            "skipped - and no data section is written anywhere. IF THIS IS THE LAST SLT1 "
            "LINE, IT DIED HERE.");
        const unsigned a = swap_everywhere((void *)g_real_stff, (void *)&hook_stff);

        mgpu::diag::info(
            "[MGPU][SLT1] TAP step 2/2 BEGIN: the same for the deprecated slSetTag. IF THIS "
            "IS THE LAST SLT1 LINE, IT DIED HERE.");
        const unsigned b = swap_everywhere((void *)g_real_st, (void *)&hook_st);
        const unsigned constants_slots = g_real_constants != nullptr
            ? swap_everywhere((void *)g_real_constants, (void *)&hook_constants) : 0;
        const unsigned route_slots =
            swap_everywhere((void *)g_real_evaluate_feature, (void *)&hook_evaluate_feature) +
            swap_everywhere((void *)g_real_get_frame_token, (void *)&hook_get_frame_token);

        unsigned cached = 0;
        if (g_mode.load(std::memory_order_relaxed) == 3)
        {
            cached += install_cached_slot(0, (void *)g_real_stff, (void *)&hook_stff, "slSetTagForFrame");
            cached += install_cached_slot(1, (void *)g_real_st, (void *)&hook_st, "slSetTag");
            if (g_real_constants != nullptr)
                cached += install_cached_slot(2, (void *)g_real_constants, (void *)&hook_constants, "slSetConstants");
            cached += install_cached_slot(3, (void *)g_real_evaluate_feature,
                (void *)&hook_evaluate_feature, "slEvaluateFeature");
            cached += install_cached_slot(4, (void *)g_real_get_frame_token,
                (void *)&hook_get_frame_token, "slGetNewFrameToken");
            HMODULE source_pin = nullptr;
            if (g_real_get_feature_function != nullptr && GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&hook_source_sleep), &source_pin))
            {
                void *sleep = nullptr, *marker = nullptr, *legacy_marker = nullptr;
                if (g_real_get_feature_function(sl::kFeatureReflex, "slReflexSleep", sleep) == sl::Result::eOk && sleep)
                {
                    g_real_source_sleep.store(reinterpret_cast<pf_source_sleep>(sleep), std::memory_order_release);
                    cached += install_cached_slot(6, sleep, (void *)&hook_source_sleep, "slReflexSleep");
                    if (mgpu::official_bridge::selected()) mgpu::native_reflex::prepare();
                }
                if (g_real_get_feature_function(sl::kFeaturePCL, "slPCLSetMarker", marker) == sl::Result::eOk && marker)
                {
                    g_real_source_marker.store(reinterpret_cast<pf_source_marker>(marker), std::memory_order_release);
                    cached += install_cached_slot(7, marker, (void *)&hook_source_marker, "slPCLSetMarker");
                }
                if (g_real_get_feature_function(sl::kFeatureReflex, "slReflexSetMarker", legacy_marker) == sl::Result::eOk && legacy_marker && legacy_marker != marker)
                {
                    g_real_source_legacy_marker.store(reinterpret_cast<pf_source_marker>(legacy_marker), std::memory_order_release);
                    cached += install_cached_slot(8, legacy_marker, (void *)&hook_source_legacy_marker, "slReflexSetMarker");
                }
                cached += swap_everywhere((void *)g_real_get_feature_function, (void *)&hook_get_feature_function);
                cached += install_cached_slot(5, (void *)g_real_get_feature_function,
                    (void *)&hook_get_feature_function, "slGetFeatureFunction");
                mgpu::diag::info("[MGPU][M349-SOURCE] source Reflex/PCL routes installed where uniquely identified; originals retained, game Present markers are not forwarded");
            }
        }
        g_slots.store(a + b + cached + constants_slots + route_slots, std::memory_order_relaxed);
        g_installed.store((a + b + cached + constants_slots + route_slots) != 0u, std::memory_order_relaxed);
        mgpu::diag::info("[MGPU][R336-INPUT] raw API ingress audit active; nonempty tag arrays are not all calls. "
            "No new resource capture, GPU commands, NGX retry or driver-plugin pointer scan added.");
        if (g_mode.load(std::memory_order_relaxed) == 3) return;

        char line[1100];
        snprintf(line, sizeof line,
            "[MGPU][SLT1] TAP INSTALLED: %u slot(s) for slSetTagForFrame + %u for slSetTag. "
            "HOW TO READ IT. A non-zero count means the game imports the tagging call and "
            "the next frame will start naming buffers. ZERO IS A RESULT, NOT A FAILURE: it "
            "says this title resolves Streamline dynamically - the static interposer library "
            "does LoadLibrary and GetProcAddress and keeps the pointer in its own data - so "
            "an import table was never going to reach it. Reaching THAT would mean writing "
            "into another module's data section while it runs, which is the calibrator's "
            "R102 rung and is exactly the thing this round is trying not to do. If this "
            "reads zero, the decision is a deliberate one and belongs in the ledger, not in "
            "a reflex.",
            a, b);
        if ((a + b) != 0u) mgpu::diag::info(line);
        else               mgpu::diag::warn(line);
    }
} // namespace

void tick(unsigned long long frames, int mode)
{
    if (mode == 0) return;
    g_mode.store(mode, std::memory_order_relaxed);
    if (g_done.load(std::memory_order_relaxed)) return;

    // ---- THE DEFERRAL ----
    //
    // 900 frames, not zero. The one rig that still faults is the one whose
    // startup we cannot see, and the standing theory is that it reaches a
    // window our rig stops reaching once shaders are warm. Nothing in this
    // file may exist inside that window. By 900 presented frames the title
    // has compiled, resized, settled and is tagging every frame, and an
    // instrument that arrives then cannot be part of a startup race.
    if (frames < 900ull) return;

    {
        std::lock_guard<std::mutex> lk(g_cs);
        if (g_done.load(std::memory_order_relaxed)) return;
        g_done.store(true, std::memory_order_relaxed);
    }

    char line[600];
    snprintf(line, sizeof line,
        "[MGPU][SLT1] TAP INSTALL BEGIN mode=%d at frame %llu. DEFERRED ON PURPOSE: nothing "
        "in this instrument exists during startup, because the fault we are avoiding has "
        "only ever been seen during startup. Nothing has been touched yet.",
        mode, frames);
    mgpu::diag::info(line);
    install_now();
}

void report()
{
    if (!g_installed.load(std::memory_order_relaxed)) return;

    char ingress[1200];
    snprintf(ingress, sizeof ingress,
        "[MGPU][R336-INPUT] raw_stff=%llu raw_st=%llu empty_arrays=%llu null_arrays=%llu "
        "tag_errors=%llu nonempty_arrays=%llu tags_read=%llu constants_calls=%llu "
        "constants_usable=%llu constants_errors=%llu evaluate_calls=%llu evaluate_errors=%llu "
        "last_feature=%u token_calls=%llu token_errors=%llu. Raw ingress is counted before decoding. "
        "Zero ingress means no calls observed at installed slots, not proof that the game disabled DLSS. "
        "Empty arrays name no resources; nonzero counters alone do not prove complete same-frame input or neural output.",
        g_stff_calls.load(std::memory_order_relaxed), g_st_calls.load(std::memory_order_relaxed),
        g_empty_arrays.load(std::memory_order_relaxed), g_null_arrays.load(std::memory_order_relaxed),
        g_tag_errors.load(std::memory_order_relaxed), g_calls.load(std::memory_order_relaxed),
        g_tags_read.load(std::memory_order_relaxed), g_constants_calls.load(std::memory_order_relaxed),
        g_constants_valid.load(std::memory_order_relaxed), g_constants_errors.load(std::memory_order_relaxed),
        g_evaluate_calls.load(std::memory_order_relaxed), g_evaluate_errors.load(std::memory_order_relaxed),
        g_last_feature.load(std::memory_order_relaxed), g_token_calls.load(std::memory_order_relaxed),
        g_token_errors.load(std::memory_order_relaxed));
    mgpu::diag::info(ingress);

    char line[1000];
    snprintf(line, sizeof line,
        "[MGPU][SLT1] TAGS mode=%d slots=%llu | tagging calls seen=%llu | tags read=%llu | "
        "elements skipped=%llu | measured stride=%llu | MVEC=0x%llx HUDLessColor=0x%llx "
        "ScalingInputColor=0x%llx. This call counter covers NONEMPTY arrays only; consult R336-INPUT "
        "for raw calls, empty declarations and return errors. Neither zero counter establishes "
        "which unobserved caller is active. MVEC non-zero is the authoritative motion vector address "
        "and should be compared against the R71 TRANSPORT SOURCE: if they differ, the ranked "
        "pick was wrong and this is the correction. skipped counts elements that failed the "
        "structType check or faulted while being read - a non-zero number there is the guard "
        "doing its job and a reason to re-read the offsets, never a reason to widen them.",
        g_mode.load(std::memory_order_relaxed),
        g_slots.load(std::memory_order_relaxed),
        g_calls.load(std::memory_order_relaxed),
        g_tags_read.load(std::memory_order_relaxed),
        g_skipped.load(std::memory_order_relaxed),
        g_stride.load(std::memory_order_relaxed),
        g_h_mvec.load(std::memory_order_relaxed),
        g_h_hudless.load(std::memory_order_relaxed),
        g_h_scale_in.load(std::memory_order_relaxed));
    mgpu::diag::info(line);
    if (g_mvec_capture.load(std::memory_order_relaxed) != nullptr)
    {   // R300d: its own line, so the [SLT1] line keeps exactly its live shape
        char r309[200];
        snprintf(r309, sizeof r309, "[MGPU][R300d] SLT1 tag-time MVec callbacks=%llu",
                 g_mvec_capture_calls.load(std::memory_order_relaxed));
        mgpu::diag::info(r309);
    }
}

unsigned long long mvec_handle()          { return g_h_mvec.load(std::memory_order_relaxed); }
void set_mvec_capture(mvec_capture_fn fn) { g_mvec_capture.store(fn, std::memory_order_release); }   // R300d
void set_resource_capture(resource_capture_fn fn) { g_resource_capture.store(fn, std::memory_order_release); }
void set_constants_capture(constants_capture_fn fn) { g_constants_capture.store(fn, std::memory_order_release); }
unsigned long long hudless_handle()       { return g_h_hudless.load(std::memory_order_relaxed); }
unsigned long long scaling_input_handle() { return g_h_scale_in.load(std::memory_order_relaxed); }

void uninstall()
{
    std::lock_guard<std::mutex> lk(g_cs);
    if (!g_installed.load(std::memory_order_relaxed)) return;
    swap_everywhere((void *)&hook_stff, (void *)g_real_stff);
    swap_everywhere((void *)&hook_st,   (void *)g_real_st);
    if (g_real_constants != nullptr)
        swap_everywhere((void *)&hook_constants, (void *)g_real_constants);
    if (g_real_evaluate_feature != nullptr)
        swap_everywhere((void *)&hook_evaluate_feature, (void *)g_real_evaluate_feature);
    if (g_real_get_frame_token != nullptr)
        swap_everywhere((void *)&hook_get_frame_token, (void *)g_real_get_frame_token);
    if (g_real_get_feature_function != nullptr)
        swap_everywhere((void *)&hook_get_feature_function, (void *)g_real_get_feature_function);
    for (auto &entry : g_cached_slots)
    {
        if (entry.slot != nullptr)
            InterlockedCompareExchangePointer(entry.slot, entry.original, entry.hook);
        entry = {};
    }
    g_installed.store(false, std::memory_order_relaxed);
    mgpu::diag::info("[MGPU][SLT1] TAP REMOVED. Import slots restored.");
}

} // namespace mgpu::sltags
