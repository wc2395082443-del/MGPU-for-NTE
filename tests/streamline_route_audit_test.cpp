#include "../src/sl_tags.cpp"
#include <type_traits>

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
}

int main()
{
    using namespace mgpu::sltags;
    static_assert(std::is_same_v<decltype(&hook_evaluate_feature), decltype(&slEvaluateFeature)>);
    static_assert(std::is_same_v<decltype(&hook_get_frame_token), decltype(&slGetNewFrameToken)>);
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
    return failures == 0 ? 0 : 1;
}
