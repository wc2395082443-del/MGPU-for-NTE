#include "../src/sl_tags.cpp"
#include <type_traits>

namespace mgpu::source_fg_guard { extern bool test_ready; }

namespace mgpu::diag
{
void info(const char *) {}
void warn(const char *) {}
}

namespace
{
struct test_token : sl::FrameToken
{
    operator uint32_t() const override { return 23; }
};
test_token frame;
sl::ViewportHandle viewport(7);
sl::Constants constants;
const sl::BaseStructure *inputs[] = {&constants};
unsigned original_calls = 0;
unsigned constants_callbacks = 0;
unsigned fg_calls = 0;
sl::DLSSGMode received_mode = sl::DLSSGMode::eCount;
DWORD received_thread = 0;
void *resolved_feature = nullptr;
sl::Feature feature_requested = 0;
const char *function_requested = nullptr;
bool forwarded = true;
int status = 0;
uint32_t frame_index = 23;

int original_tags(const void *token, const void *view, const void *tags, unsigned count, void *command)
{
    ++original_calls;
    forwarded = forwarded && token == &frame && view == &viewport && tags == nullptr &&
        count == 0 && command == nullptr;
    return status;
}
int original_legacy(const void *view, const void *tags, unsigned count, void *command)
{
    return original_tags(&frame, view, tags, count, command);
}
int original_constants(const void *values, const void *token, const void *view)
{
    ++original_calls;
    forwarded = forwarded && values == &constants && token == &frame && view == &viewport;
    return status;
}
sl::Result original_evaluate(sl::Feature feature, const sl::FrameToken &token,
    const sl::BaseStructure **values, uint32_t count, sl::CommandBuffer *command)
{
    ++original_calls;
    forwarded = forwarded && feature == 11 && &token == &frame && values == inputs &&
        count == 1 && command == nullptr;
    return static_cast<sl::Result>(status);
}
sl::Result original_token(sl::FrameToken *&token, const uint32_t *index)
{
    ++original_calls;
    forwarded = forwarded && index == &frame_index;
    token = &frame;
    return static_cast<sl::Result>(status);
}
void capture_constants(const mgpu::aligned::guide_constants &values)
{
    ++constants_callbacks;
    forwarded = forwarded && values.key.frame == 23 && values.key.viewport == 7;
}
sl::Result original_fg_options(const sl::ViewportHandle &, const sl::DLSSGOptions &options)
{
    ++fg_calls;
    received_mode = options.mode;
    received_thread = GetCurrentThreadId();
    return static_cast<sl::Result>(status);
}
sl::Result original_resolver(sl::Feature feature, const char *name, void *&function)
{
    feature_requested = feature;
    function_requested = name;
    function = resolved_feature;
    return static_cast<sl::Result>(status);
}
}

int main()
{
    using namespace mgpu::sltags;
    static_assert(std::is_same_v<decltype(&hook_evaluate_feature), decltype(&slEvaluateFeature)>);
    static_assert(std::is_same_v<decltype(&hook_get_frame_token), decltype(&slGetNewFrameToken)>);
    static_assert(std::is_same_v<decltype(&hook_source_fg_options), PFun_slDLSSGSetOptions *>);
    unsigned failures = 0;
    const auto check = [&failures](bool passed, const char *name)
    {
        std::printf("%s: %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    g_real_stff = original_tags;
    g_real_st = original_legacy;
    g_real_constants = original_constants;
    g_real_evaluate_feature = original_evaluate;
    g_real_get_frame_token = original_token;
    set_constants_capture(capture_constants);
    constants.mvecScale = {1.0f / 2562.0f, 1.0f / 1441.0f};
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    constants.depthInverted = sl::Boolean::eTrue;
    constants.reset = sl::Boolean::eFalse;

    check(hook_stff(&frame, &viewport, nullptr, 0, nullptr) == 0 &&
        g_stff_calls == 1 && g_calls == 0 && g_tags_read == 0,
        "empty tag call is raw ingress, not a nonempty declaration or captured input");
    status = 7;
    check(hook_st(&viewport, nullptr, 0, nullptr) == 7 && g_st_calls == 1 &&
        g_empty_arrays == 2 && g_null_arrays == 2 && g_tag_errors == 1,
        "legacy empty call and original failure are counted without changing the return code");
    check(hook_constants(&constants, &frame, &viewport) == 7 && g_constants_calls == 1 &&
        g_constants_errors == 1 && g_constants_valid == 0 && constants_callbacks == 0,
        "rejected constants never become usable inputs");
    status = 0;
    check(hook_constants(&constants, &frame, &viewport) == 0 && g_constants_valid == 1 &&
        constants_callbacks == 1, "accepted same-frame constants preserve the existing callback");
    constants.cameraMotionIncluded = sl::Boolean::eFalse;
    check(hook_constants(&constants, &frame, &viewport) == 0 && g_constants_calls == 3 &&
        g_constants_valid == 1 && constants_callbacks == 2,
        "successful API return is distinct from usable metadata and callback behavior is unchanged");
    status = 9;
    check(static_cast<int>(hook_evaluate_feature(11, frame, inputs, 1, nullptr)) == 9 &&
        g_evaluate_calls == 1 && g_evaluate_errors == 1 && g_last_feature == 11,
        "feature evaluate forwards the pinned SDK ABI and original error unchanged");
    sl::FrameToken *token = nullptr;
    status = 0;
    check(hook_get_frame_token(token, &frame_index) == sl::Result::eOk && token == &frame &&
        g_token_calls == 1 && g_token_errors == 0,
        "token output reference and caller frame-index pointer are forwarded unchanged");
    status = 5;
    check(static_cast<int>(hook_get_frame_token(token, &frame_index)) == 5 && g_token_errors == 1,
        "frame-token errors remain visible without fabricating a successful frame");
    check(forwarded && original_calls == 8, "each original function runs exactly once with original arguments");
    check(g_calls == 0 && g_tags_read == 0 && g_mvec_capture_calls == 0,
        "ingress-only diagnostics cannot turn empty declarations into neural resources");
    g_real_get_feature_function = original_resolver;
    resolved_feature = reinterpret_cast<void *>(&original_fg_options);
    void *resolved = nullptr;
    status = 0;
    check(hook_get_feature_function(sl::kFeatureDLSS_G, "slDLSSGSetOptions", resolved) == sl::Result::eOk &&
        resolved == reinterpret_cast<void *>(&hook_source_fg_options) &&
        g_real_source_fg_options.load() == original_fg_options && feature_requested == sl::kFeatureDLSS_G,
        "dynamic source FG resolution reaches typed wrapper and retains original");
    const auto caller = reinterpret_cast<PFun_slDLSSGSetOptions *>(resolved);
    sl::DLSSGOptions fg_options{};
    fg_options.mode = sl::DLSSGMode::eOn;
    check(caller(viewport, fg_options) == sl::Result::eOk && fg_calls == 1 &&
        fg_options.mode == sl::DLSSGMode::eOn && g_source_fg_rewritten.load() == 0,
        "unvalidated game binding forwards source options once without mutation");
    resolved_feature = resolved;
    hook_get_feature_function(sl::kFeatureDLSS_G, "slDLSSGSetOptions", resolved);
    check(g_real_source_fg_options.load() == original_fg_options && caller(viewport, fg_options) == sl::Result::eOk &&
        fg_calls == 2, "re-resolving wrapper cannot replace original with itself or recurse");
    resolved_feature = reinterpret_cast<void *>(&original_fg_options);
    hook_get_feature_function(sl::kFeatureDLSS_G, "slDLSSGGetState", resolved);
    check(resolved == resolved_feature, "FG state accessor is not rewritten");
    hook_get_feature_function(sl::kFeatureReflex, "slDLSSGSetOptions", resolved);
    check(resolved == resolved_feature, "other features including Reflex are unaffected");
    status = 5;
    check(static_cast<int>(hook_get_feature_function(sl::kFeatureDLSS_G, "slDLSSGSetOptions", resolved)) == 5 &&
        resolved == resolved_feature, "resolver failure preserved without hook substitution");
    status = 0;
    mgpu::source_fg_guard::test_ready = true;
    const sl::ViewportHandle source_viewport(1);
    const auto before_calls = fg_calls;
    const auto source_thread = GetCurrentThreadId();
    for (unsigned repeat = 0; repeat < 5; ++repeat) {
        check(caller(source_viewport, fg_options) == sl::Result::eOk &&
            received_mode == sl::DLSSGMode::eOff && received_thread == source_thread &&
            fg_calls == before_calls + repeat + 1 && fg_options.mode == sl::DLSSGMode::eOn,
            "every validated source request reaches SDK Off once on original thread without changing caller options");
    }
    check(g_source_fg_rewritten.load() == 5, "actual production wrapper records five rewrites");
    status = static_cast<int>(sl::Result::eErrorReflexAPI);
    check(caller(source_viewport, fg_options) == sl::Result::eErrorReflexAPI &&
        received_mode == sl::DLSSGMode::eOff && fg_calls == before_calls + 6,
        "actual SDK failure survives validated rewrite");
    status = 0;
    mgpu::source_fg_guard::test_ready = false;
    check(caller(source_viewport, fg_options) == sl::Result::eOk && received_mode == sl::DLSSGMode::eOn,
        "loss of live guard stops source option rewrites");
    return failures == 0 ? 0 : 1;
}
