#include "../src/nr_preset.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

struct parameters
{
    std::vector<unsigned> requests;
    void Set(const char *key, unsigned value)
    {
        check(std::string(key) == "DLSSNR.Hint.Render.Preset", "exact NR key required");
        requests.push_back(value);
    }
    void Set(const char *, float) { throw std::runtime_error("preset must not use float overload"); }
    void Set(const char *, int) { throw std::runtime_error("preset must not use signed overload"); }
};
}

int main()
{
    try
    {
        mgpu::nr_preset::selection selection;
        parameters defaults;
        selection.bind(&defaults);
        selection.load(0);
        selection.begin_create();
        selection.bind(&defaults);
        check(defaults.requests.empty(), "absent/default preset must preserve the runtime parameter block");
        check(!selection.pending(), "default initialization has no staged change");

        check(selection.request(2) && selection.pending(), "new request must remain pending");
        selection.bind(&defaults);
        check(defaults.requests.empty() && selection.active == 0, "UI request cannot mutate active feature preset");
        selection.begin_create();
        parameters passes;
        selection.bind(&passes);
        selection.bind(&passes);
        check(passes.requests == std::vector<unsigned>{2, 2}, "both passes receive the same exact unsigned preset at creation");

        check(selection.request(4), "valid alternate request accepted");
        parameters rebuild;
        selection.bind(&rebuild);
        check(rebuild.requests == std::vector<unsigned>{2}, "scale-only feature recreation keeps the active preset until next initialization");
        check(!selection.request(-1) && !selection.request(5) && !selection.request(0x00ffffff),
            "unknown and driver-only Latest values refused");
        check(selection.requested == 4 && selection.active == 2, "invalid values cannot corrupt pending or active selection");

        selection.begin_create();
        selection.bind(&rebuild);
        check(rebuild.requests == std::vector<unsigned>{2, 4} && !selection.pending(), "fresh initialization consumes requested hint");
        selection.load(999);
        selection.begin_create();
        selection.bind(&defaults);
        check(selection.requested == 0 && selection.active == 0 && defaults.requests.empty(),
            "invalid file values use default without writing an unsupported hint");
        for (int value = 1; value <= 4; ++value)
        {
            selection.load(value);
            selection.begin_create();
            parameters model;
            selection.bind(&model);
            check(model.requests == std::vector<unsigned>{static_cast<unsigned>(value)}, "raw preset IDs never remap to Style");
        }
        std::puts("NR preset creation contract passed");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
