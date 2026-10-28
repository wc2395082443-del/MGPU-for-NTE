#include "../src/fg_present_evidence.hpp"

#include <cstdio>
#include <cstdlib>

static void require(bool condition, const char *name)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", name);
        std::exit(1);
    }
}

int main()
{
    using namespace mgpu::fgmap;
    present_evidence evidence;
    const evaluated_outputs valid{10, 20, 30, 1, 1, true, true};
    require(!evidence.capture(30).verified, "no evaluate is not interpolation evidence");
    for (unsigned attempt = 0; attempt < 100; ++attempt)
    {
        evidence.evaluated(valid);
        const auto unknown = evidence.capture(99);
        require(!unknown.verified && !unknown.generated && unknown.motion_fraction == 1.0f,
                "repeated calls without matching captured output leave motion unchanged");
    }
    evidence.evaluated(valid);
    require(!evidence.capture(20).verified, "real frame alone cannot prove interpolation");
    const auto generated = evidence.capture(30);
    require(generated.verified && generated.generated && generated.motion_fraction == 0.5f,
            "distinct captured interpolation identifies a generated frame");
    const auto real = evidence.capture(20);
    require(real.verified && !real.generated && real.motion_fraction == 0.5f,
            "matching real output after interpolation belongs to that pair");
    evidence.evaluated(valid);
    require(!evidence.capture(20).verified, "next evaluate cannot inherit previous pair evidence");
    evidence.capture(30);
    evidence.capture(99);
    require(!evidence.capture(20).verified, "unknown capture breaks pair evidence");
    evidence.evaluated(valid);
    for (unsigned attempt = 0; attempt < 5; ++attempt) evidence.capture(30);
    require(!evidence.capture(30).verified, "expired output identities are rejected");
    evidence.reset();
    require(!evidence.capture(30).verified, "released feature cannot classify later presents");

    for (unsigned variant = 0; variant < 8; ++variant)
    {
        auto invalid = valid;
        switch (variant)
        {
        case 0: invalid.succeeded = false; break;
        case 1: invalid.rendering = false; break;
        case 2: invalid.count = 0; break;
        case 3: invalid.count = 999; break;
        case 4: invalid.index = 0; break;
        case 5: invalid.index = 2; break;
        case 6: invalid.input = invalid.interpolated; break;
        case 7: invalid.real = invalid.interpolated; break;
        }
        evidence.evaluated(invalid);
        require(!evidence.capture(30).verified, "failed, paused, malformed or aliased output is unknown");
    }
    auto multiple = valid;
    multiple.count = 3;
    multiple.index = 2;
    evidence.evaluated(multiple);
    require(evidence.capture(30).motion_fraction == 0.25f, "verified multiframe count is respected");
    std::puts("PASS: captured-frame evidence policy (CPU only)");
}
