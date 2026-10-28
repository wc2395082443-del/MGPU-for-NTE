#include "../src/native_reflex_request.hpp"
#include <array>
#include <cstdio>
#include <vector>

int main()
{
    using namespace mgpu::native_reflex;
    unsigned failures = 0;
    unsigned checks = 0;
    const auto check = [&](bool valid, const char *name)
    {
        ++checks;
        std::printf("%s: %s\n", valid ? "PASS" : "FAIL", name);
        failures += valid ? 0 : 1;
    };
    std::array<variable, 3> values;
    std::vector<unsigned> calls;
    int reject = -1;
    bool reject_rollback = false;
    int reject_value = -1;
    const auto read = [&](unsigned index) { return values[index]; };
    const auto write = [&](unsigned index, int value)
    {
        calls.push_back(index);
        if (static_cast<int>(index) == reject || (reject_rollback && value == 0) || value == reject_value) return;
        values[index] = {value, set_by_code | (values[index].flags & ~priority_mask)};
    };
    const auto reset = [&]()
    {
        values = {{{0, 0x0c000020}, {0, 0x0c000020}, {1, 4}}};
        calls.clear();
        reject = -1;
        reject_rollback = false;
        reject_value = -1;
    };
    reset();
    check(request_on(read, write) == request_result::applied && values[0].value == 1 && values[1].value == 1,
        "requests ordinary On over the observed command-line priority");
    check(calls == std::vector<unsigned>{1, 0}, "sets mode before enabling native control");
    calls.clear();
    check(request_on(read, write) == request_result::unchanged && calls.empty(), "already satisfied request does not call setters");
    reset();
    values[0].flags = 0x0e000020;
    check(request_on(read, write) == request_result::refused && calls.empty(), "higher priority explicit Off is respected");
    reset();
    values[1] = {2, 0x0c000020};
    check(request_on(read, write) == request_result::applied && values[1].value == 1, "explicit ordinary On never enables Boost");
    reset();
    values[1] = {2, 0x0e000020};
    check(request_on(read, write) == request_result::refused && calls.empty(), "higher priority Boost is not overwritten");
    reset();
    values[1].flags |= 4;
    check(request_on(read, write) == request_result::refused && calls.empty(), "unexpected read-only flags fail closed");
    reset();
    values[1].value = 7;
    check(request_on(read, write) == request_result::refused && calls.empty(), "unknown modes fail closed");
    reset();
    values[0].value = -1;
    check(request_on(read, write) == request_result::refused && calls.empty(), "invalid enable value fails closed");
    reset();
    reject = 0;
    check(request_on(read, write) == request_result::rolled_back && values[1].value == 0,
        "rejected enable restores previous mode through the same API");
    reset();
    reject = 1;
    check(request_on(read, write) == request_result::rolled_back && values[0].value == 0,
        "rejected mode restores previous enable through the same API");
    reset();
    reject = 0;
    reject_rollback = true;
    check(request_on(read, write) == request_result::rollback_failed, "rollback failure is never reported as success");
    reset();
    values[0] = {1, 0x0e000020};
    check(request_on(read, write) == request_result::applied && calls == std::vector<unsigned>{1},
        "matching higher-priority enable is preserved while mode is requested");
    check(configured_mode(L"1", L"0", L"0") == 0 && configured_mode(L"1", L"1") == 1,
        "explicit consistent Off and On requests are recognized");
    check(configured_mode(L"", L"") == -1 && configured_mode(L"0", L"") == -1 && configured_mode(L"", L"0") == -1,
        "missing INI keys do not silently request Off");
    check(configured_mode(L"0", L"1") == -1 && configured_mode(L"1", L"0") == -1 && configured_mode(L"1", L"2") == -1,
        "mixed configuration and unrequested Boost fail closed");
    check(configured_mode(L"garbage", L"0") == -1 && configured_mode(L"0", L"false") == -1 && configured_mode(L"00", L"0") == -1,
        "malformed strings are not parsed as zero");
    const auto reset_on = [&]() {
        reset();
        values = {{{1, 0x0c000020}, {1, 0x0c000020}, {1, 4}}};
    };
    reset_on();
    check(request_mode(0, read, write) == request_result::applied && values[0].value == 1 && values[1].value == 0 && values[2].value == 0,
        "Off disables automatic promotion and low latency while keeping integration enabled");
    check(calls == std::vector<unsigned>{2, 1, 0}, "Off disables auto promotion before selecting mode and preserving integration");
    calls.clear();
    check(request_mode(0, read, write) == request_result::unchanged && calls.empty(), "already satisfied Off does not repeat setters");
    reset();
    check(request_mode(0, read, write) == request_result::applied && calls == std::vector<unsigned>{2, 1, 0},
        "observed enable0 mode0 auto1 failure is corrected through native Set API");
    reset_on();
    values[1].flags = 0x0e000020;
    check(request_mode(0, read, write) == request_result::refused && calls.empty(), "higher-priority On is respected");
    reset_on();
    values[0] = {1, 0x0e000020};
    check(request_mode(0, read, write) == request_result::applied && calls == std::vector<unsigned>{2, 1},
        "matching higher-priority integration enable is preserved");
    reset_on();
    values[1] = {2, 0x0c000020};
    check(request_mode(0, read, write) == request_result::applied && values[1].value == 0, "Off also disables lower-priority Boost");
    reset_on();
    values[1] = {2, 0x0e000020};
    check(request_mode(0, read, write) == request_result::refused && calls.empty(), "Off does not overwrite higher-priority Boost");
    reset_on();
    values[1].flags |= 4;
    check(request_mode(0, read, write) == request_result::refused && calls.empty(), "Off preserves unsupported CVar flags");
    reset_on();
    reject = 2;
    check(request_mode(0, read, write) == request_result::rolled_back && values[1].value == 1,
        "failed Auto disable rolls the previous mode back");
    reset_on();
    reject = 1;
    check(request_mode(0, read, write) == request_result::rolled_back && values[0].value == 1,
        "failed Off mode rolls enable back");
    reset_on();
    reject = 2;
    reject_value = 1;
    check(request_mode(0, read, write) == request_result::rollback_failed, "failed Off rollback is reported");
    reset_on();
    values[2].flags = 0x0e000004;
    check(request_mode(0, read, write) == request_result::refused && calls.empty(), "higher-priority automatic On is not overwritten");
    reset_on();
    values[2].flags = 0x20;
    check(request_mode(0, read, write) == request_result::refused && calls.empty(), "unexpected Auto flags fail closed");
    reset_on();
    values[2].value = 2;
    check(request_mode(0, read, write) == request_result::refused && calls.empty(), "invalid Auto value fails closed");
    reset_on();
    values[0] = {0, 0x0e000020};
    check(request_mode(0, read, write) == request_result::refused && calls.empty(), "higher-priority disabled integration is respected");
    check(configured_mode(L"0", L"0", L"0") == -1 && configured_mode(L"1", L"0", L"1") == -1,
        "Off configuration must preserve integration and explicitly disable Auto");
    reset_on();
    check(request_mode(-1, read, write) == request_result::refused && request_mode(2, read, write) == request_result::refused && calls.empty(),
        "invalid requests never call a native setter");
    std::printf("%u request checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
