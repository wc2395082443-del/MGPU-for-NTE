#include "../src/sl_tags.cpp"
#include "../src/tagged_resource_validation.hpp"

namespace mgpu::diag
{
void info(const char *) {}
void warn(const char *) {}
}

namespace mgpu::official_bridge
{
extern unsigned test_sleep_count, test_marker_count;
extern std::uint32_t test_source_frame;
extern std::uint64_t test_native_begin_ns, test_native_end_ns;
}

namespace
{
struct base_structure
{
    void *next = nullptr;
    GUID type{};
    size_t version = 1;
};

struct resource : base_structure
{
    char resource_type = 0;
    void *native = reinterpret_cast<void *>(0x1234);
    void *memory = nullptr;
    void *view = nullptr;
    unsigned state = 0x40;
    unsigned width = 0;
    unsigned height = 0;
    unsigned format = 0;

    resource()
    {
        type = {0x3a9d70cf, 0x2418, 0x4b72, {0x83, 0x91, 0x13, 0xf8, 0x72, 0x1c, 0x72, 0x61}};
    }
};

struct resource_tag : base_structure
{
    resource *native_resource = nullptr;
    unsigned buffer_type = 1;
    int lifecycle = 0;
    unsigned top = 0, left = 0, width = 2562, height = 1441;

    resource_tag()
    {
        type = {0x4c6a5aad, 0xb445, 0x496c, {0x87, 0xff, 0x1a, 0xf3, 0x84, 0x5b, 0xe6, 0x53}};
    }
};

unsigned callback_count = 0;
unsigned original_source_calls = 0;
sl::Result original_source_result = sl::Result::eOk;
sl::Result fake_source_sleep(const sl::FrameToken &) { ++original_source_calls; return original_source_result; }
sl::Result fake_source_marker(unsigned, const sl::FrameToken &) { ++original_source_calls; return original_source_result; }
bool callback_fields = false;
mgpu::motion_packet::identity callback_engine{};
struct frame_token : sl::FrameToken
{
    uint32_t index = 123;
    operator uint32_t() const override { return index; }
};
struct non_texture : IUnknown
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **result) override
    {
        *result = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
};
void capture(void *command, unsigned long long handle, unsigned state, int lifecycle,
             unsigned top, unsigned left, unsigned width, unsigned height,
             mgpu::motion_packet::identity engine)
{
    ++callback_count;
    callback_engine = engine;
    callback_fields = command == reinterpret_cast<void *>(0x5678) && handle == 0x1234 &&
        state == 0x40 && lifecycle == 0 && top == 0 && left == 0 && width == 2562 && height == 1441;
}
}

int main()
{
    static_assert(sizeof(base_structure) == 32);
    static_assert(sizeof(resource_tag) == 64);
    unsigned failures = 0;
    const auto check = [&failures](bool passed, const char *name)
    {
        std::printf("%s: %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    resource native;
    resource_tag tags[2];
    tags[0].native_resource = &native;
    mgpu::sltags::tag_fields fields{};
    check(mgpu::sltags::is_tag(&tags[0]), "documented ResourceTag GUID accepted");
    check(mgpu::sltags::measure_stride(tags) == sizeof(resource_tag), "array stride matches pinned 64-byte ABI");
    check(mgpu::sltags::read_fields(&tags[0], fields) && fields.native == 0x1234 &&
          fields.state == 0x40 && fields.extent_w == 2562 && fields.extent_h == 1441,
          "native pointer, declared state and extent decoded together");
    native.version = 2;
    check(mgpu::sltags::read_fields(&tags[0], fields), "new resource version preserves the documented v1 prefix");
    native.version = 0;
    check(!mgpu::sltags::read_fields(&tags[0], fields) &&
          std::strcmp(fields.reason, "resource_version_zero") == 0, "version zero refused with reason");
    native.version = 1;
    native.type.Data1 = 0;
    check(!mgpu::sltags::read_fields(&tags[0], fields), "invalid resource GUID refused");
    native = resource{};
    native.resource_type = 1;
    check(!mgpu::sltags::read_fields(&tags[0], fields) &&
          std::strcmp(fields.reason, "resource_type") == 0, "declared buffer type refused with reason");
    native.resource_type = 8;
    check(mgpu::sltags::read_fields(&tags[0], fields), "IUnknown resource proceeds to D3D12 interface validation");
    native.resource_type = 0;
    native.type = {};
    native.version = 0;
    check(mgpu::sltags::read_fields(&tags[0], fields) && fields.zero_resource_header &&
          fields.native == 0x1234 && fields.state == 0x40,
          "exact zero resource prefix with v1 texture tag reaches native validation");
    native.next = reinterpret_cast<void *>(0x1000);
    check(!mgpu::sltags::read_fields(&tags[0], fields) && fields.resource_next == 0x1000,
          "zero GUID with nonzero extension pointer refused and reported");
    native.next = nullptr;
    native.version = 1;
    check(!mgpu::sltags::read_fields(&tags[0], fields), "zero GUID with nonzero resource version refused");
    native.version = 0;
    native.resource_type = 8;
    check(!mgpu::sltags::read_fields(&tags[0], fields), "zero prefix is not accepted for an unknown resource type");
    native.resource_type = 0;
    tags[0].version = 2;
    check(!mgpu::sltags::read_fields(&tags[0], fields), "zero prefix compatibility is confined to tag v1");
    tags[0].version = 1;
    native = resource{};
    tags[0].version = 2;
    check(mgpu::sltags::read_fields(&tags[0], fields), "new tag version preserves the documented v1 prefix");
    tags[0].version = 0;
    check(!mgpu::sltags::read_fields(&tags[0], fields) &&
          std::strcmp(fields.reason, "tag_version_zero") == 0, "tag version zero refused with reason");
    tags[0].version = 1;
    tags[0].type.Data1 = 0;
    check(!mgpu::sltags::read_fields(&tags[0], fields) &&
          std::strcmp(fields.reason, "tag_guid") == 0, "invalid tag GUID refused with reason");
    tags[0].type = resource_tag{}.type;
    check(!mgpu::sltags::read_fields(nullptr, fields), "null tag refused without dereferencing it");
    mgpu::sltags::set_mvec_capture(capture);
    mgpu::sltags::read_array(tags, 2, reinterpret_cast<void *>(0x5678));
    check(callback_count == 1 && callback_fields, "only valid motion resource reaches synchronous callback");
    mgpu::sltags::read_array(tags, 1, nullptr);
    check(callback_count == 1, "no command list means no GPU capture callback");
    native.type = {};
    native.version = 0;
    mgpu::sltags::read_array(tags, 1, reinterpret_cast<void *>(0x5678));
    check(callback_count == 2 && callback_fields, "zero-prefix motion tag preserves synchronous capture arguments");
    ID3D12Resource *queried_resource = nullptr;
    D3D12_RESOURCE_DESC description{};
    check(!mgpu::tagged_motion::query_resource(nullptr, queried_resource, description) && queried_resource == nullptr,
          "null native resource refused before GPU work");
    check(!mgpu::tagged_motion::query_resource(reinterpret_cast<void *>(8), queried_resource, description) &&
          queried_resource == nullptr, "unreadable native pointer refused without crashing");
    non_texture wrong_interface;
    check(!mgpu::tagged_motion::query_resource(&wrong_interface, queried_resource, description) &&
          queried_resource == nullptr, "IUnknown without ID3D12Resource refused before GPU work");
    frame_token token;
    sl::ViewportHandle viewport(7);
    mgpu::sltags::hook_stff(&token, &viewport, tags, 1, reinterpret_cast<void *>(0x5678));
    check(callback_engine.frame_known && callback_engine.frame == 123 &&
          callback_engine.viewport_known && callback_engine.viewport == 7,
          "official FrameToken conversion reaches the motion callback with viewport");
    token.index = 0;
    const auto zero = mgpu::sltags::read_identity(&token, &viewport);
    check(zero.frame_known && zero.frame == 0, "engine frame zero is distinct from unknown");
    mgpu::sltags::hook_st(&viewport, tags, 1, reinterpret_cast<void *>(0x5678));
    check(!callback_engine.frame_known && callback_engine.viewport_known,
          "legacy tag API never invents an engine frame");
    token.structType = {};
    check(!mgpu::sltags::read_identity(&token, &viewport).frame_known,
          "invalid FrameToken header never invokes conversion");
    const auto invalid = mgpu::sltags::read_identity(reinterpret_cast<void *>(8), reinterpret_cast<void *>(8));
    check(!invalid.frame_known && !invalid.viewport_known, "unreadable identity is unknown, not a crash");
    sl::Constants constants;
    constants.mvecScale = {1.0f / 2562.0f, -1.0f / 1441.0f};
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    constants.depthInverted = sl::Boolean::eTrue;
    constants.reset = sl::Boolean::eFalse;
    auto guides = mgpu::sltags::read_constants(&constants, {0, 7, true, true});
    check(guides.valid && guides.key.frame == 0 && guides.key.viewport == 7 && guides.depth_inverted &&
          guides.motion_scale_x == constants.mvecScale.x && guides.motion_scale_y == constants.mvecScale.y,
          "same-frame constants preserve normalized motion scale, sign and depth encoding");
    check(!mgpu::sltags::read_constants(&constants, {}).valid, "constants without a frame identity are refused");
    constants.cameraMotionIncluded = sl::Boolean::eFalse;
    check(!mgpu::sltags::read_constants(&constants, {1, 7, true, true}).valid,
          "object-only motion cannot silently stand in for complete camera motion");
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectorsJittered = sl::Boolean::eTrue;
    check(!mgpu::sltags::read_constants(&constants, {1, 7, true, true}).valid,
          "unsupported jittered motion is not given guessed offsets");
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    constants.mvecScale.x = 0.0f;
    check(!mgpu::sltags::read_constants(&constants, {1, 7, true, true}).valid, "zero motion scale is refused");
    check(!mgpu::sltags::read_constants(reinterpret_cast<void *>(8), {1, 7, true, true}).valid,
          "unreadable constants cannot cause a crash");
    frame_token source_token;
    mgpu::sltags::g_real_source_sleep.store(&fake_source_sleep);
    mgpu::sltags::g_real_source_marker.store(&fake_source_marker);
    check(mgpu::sltags::hook_source_sleep(source_token) == sl::Result::eOk && original_source_calls == 1 &&
        mgpu::official_bridge::test_sleep_count == 1 && mgpu::official_bridge::test_source_frame == 123,
        "source sleep preserves original call and forwards its exact frame");
    check(mgpu::official_bridge::test_native_begin_ns > 0 &&
        mgpu::official_bridge::test_native_end_ns > mgpu::official_bridge::test_native_begin_ns,
        "original Sleep call bracket survives forwarding");
    original_source_result = sl::Result::eErrorReflexAPI;
    check(mgpu::sltags::hook_source_sleep(source_token) == sl::Result::eErrorReflexAPI &&
        mgpu::official_bridge::test_sleep_count == 1, "failed source sleep is not fabricated as a completed relay");
    original_source_result = sl::Result::eOk;
    mgpu::sltags::hook_source_marker(0, source_token);
    check(mgpu::official_bridge::test_marker_count == 1, "real simulation marker relayed");
    mgpu::sltags::hook_source_marker(4, source_token);
    check(mgpu::official_bridge::test_marker_count == 1, "original game Present marker is not relayed");
    mgpu::sltags::hook_source_marker(8, source_token);
    check(mgpu::official_bridge::test_marker_count == 2 && mgpu::official_bridge::test_source_frame == 123,
        "actual input ping keeps source frame identity");
    original_source_result = sl::Result::eErrorReflexAPI;
    mgpu::sltags::hook_source_marker(8, source_token);
    check(mgpu::official_bridge::test_marker_count == 2, "failed native input ping is not fabricated");
    return failures == 0 ? 0 : 1;
}
