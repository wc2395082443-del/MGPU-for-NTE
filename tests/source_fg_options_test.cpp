#include "../src/source_fg_options.hpp"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <thread>

int main()
{
    using namespace mgpu::source_fg_options;
    unsigned checks = 0, failures = 0, calls = 0;
    const auto check = [&](bool valid, const char *name) {
        ++checks;
        failures += valid ? 0 : 1;
        std::printf("%s: %s\n", valid ? "PASS" : "FAIL", name);
    };
    sl::ViewportHandle viewport(1), other_viewport(2);
    sl::DLSSGOptions input{};
    input.mode = sl::DLSSGMode::eOn;
    input.numFramesToGenerate = 1;
    input.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    input.colorWidth = 3840;
    input.colorHeight = 2160;
    input.dynamicTargetFrameRate = 165.0f;
    const auto original_bytes = input;
    const sl::DLSSGOptions *received = nullptr;
    sl::DLSSGOptions captured{};
    DWORD thread = 0;
    sl::Result return_value = sl::Result::eOk;
    const auto original = [&](const sl::ViewportHandle &view, const sl::DLSSGOptions &options) {
        ++calls;
        thread = GetCurrentThreadId();
        received = &options;
        std::memcpy(&captured, &options, supported_size(options.structVersion));
        check(static_cast<uint32_t>(view) == 1, "viewport identity reaches original unchanged");
        return return_value;
    };
    action selected{};
    check(forward(original, viewport, input, true, selected) == sl::Result::eOk && selected == action::rewritten &&
          captured.mode == sl::DLSSGMode::eOff && calls == 1, "On request becomes one original Off call");
    check(thread == GetCurrentThreadId(), "original runs synchronously on engine calling thread");
    auto expected = original_bytes;
    expected.mode = sl::DLSSGMode::eOff;
    check(std::memcmp(&captured, &expected, supported_size(5)) == 0, "all fields except source mode preserved");
    check(std::memcmp(&input, &original_bytes, sizeof(input)) == 0, "caller options memory is never changed");
    forward(original, viewport, input, false, selected);
    check(selected == action::forwarded && received == &input && captured.mode == sl::DLSSGMode::eOn,
        "inactive or missing resource guard keeps original object and mode");
    sl::DLSSGOptions output{};
    check(prepare(other_viewport, input, output, true) == action::forwarded, "other viewport not overridden");
    input.mode = sl::DLSSGMode::eOff;
    forward(original, viewport, input, true, selected);
    check(selected == action::already_off && received == &input, "Off keeps original pointer");
    for (auto mode : {sl::DLSSGMode::eOn, sl::DLSSGMode::eAuto, sl::DLSSGMode::eDynamic}) {
        input.mode = mode;
        check(prepare(viewport, input, output, true) == action::rewritten && output.mode == sl::DLSSGMode::eOff,
            "all supported active source modes become Off");
    }
    input.mode = sl::DLSSGMode::eCount;
    check(prepare(viewport, input, output, true) == action::forwarded, "unknown enum forwarded unchanged");
    input.mode = sl::DLSSGMode::eOn;
    input.next = reinterpret_cast<sl::BaseStructure *>(&output);
    check(prepare(viewport, input, output, true) == action::forwarded, "unknown extension chain not flattened");
    input.next = nullptr;
    input.structVersion = 99;
    check(prepare(viewport, input, output, true) == action::forwarded, "future struct version forwarded");
    input.structVersion = 5;
    input.structType = sl::StructType{};
    check(prepare(viewport, input, output, true) == action::forwarded, "wrong structure identity forwarded");
    input.structType = sl::DLSSGOptions::s_structType;
    return_value = sl::Result::eErrorNotInitialized;
    check(forward(original, viewport, input, true, selected) == return_value, "SDK error preserved without retry");
    auto *memory = static_cast<unsigned char *>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!memory) return 2;
    DWORD old = 0;
    if (!VirtualProtect(memory + 4096, 4096, PAGE_NOACCESS, &old)) return 2;
    for (std::size_t version = 1; version <= 5; ++version) {
        input.structVersion = version;
        const auto size = supported_size(version);
        const auto allocated = (size + alignof(sl::DLSSGOptions) - 1) & ~(alignof(sl::DLSSGOptions) - 1);
        auto *bounded = reinterpret_cast<sl::DLSSGOptions *>(memory + 4096 - allocated);
        std::memcpy(bounded, &input, size);
        check(prepare(viewport, *bounded, output, true) == action::rewritten && output.structVersion == version &&
              output.mode == sl::DLSSGMode::eOff, "bounded version prefix near guard page accepted");
    }
    VirtualFree(memory, 0, MEM_RELEASE);
    input.structVersion = 4;
    input.mode = sl::DLSSGMode::eOn;
    for (unsigned index = 0; index < 100; ++index) forward(original, viewport, input, true, selected);
    check(captured.mode == sl::DLSSGMode::eOff && input.mode == sl::DLSSGMode::eOn,
        "repeated upstream On cannot restore source SDK On through this route");
    const auto before_calls = calls;
    DWORD worker_thread = 0;
    std::thread worker([&]() { worker_thread = GetCurrentThreadId(); forward(original, viewport, input, true, selected); });
    worker.join();
    check(calls == before_calls + 1 && thread == worker_thread, "no cross-thread asynchronous SDK calls introduced");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
