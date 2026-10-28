#include "../src/neural_history.hpp"
#include "../src/neural_pipeline.hpp"
#include <cstdio>

int main()
{
    using namespace mgpu::neural_history;
    unsigned failures = 0;
    const auto check = [&](bool passed, const char *name)
    {
        std::printf("%s: %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    history temporal;
    configuration settings;
    settings.strengths = {0.5f, 0.25f};
    check(temporal.changes(settings) == initial, "first evaluation discards uninitialized history");
    temporal.complete(settings);
    check(temporal.changes(settings) == none && temporal.revision() == 1, "unchanged settings retain history");

    settings.strengths[0] = 2.0f;
    check(temporal.changes(settings) == intensity, "one pass low-to-high intensity resets history");
    check(temporal.changes(settings) == intensity, "uncompleted evaluation cannot acknowledge a change");
    temporal.complete(settings);
    check(temporal.changes(settings) == none && temporal.revision() == 2, "high intensity resets once, not every frame");
    settings.strengths[1] = 1.5f;
    check(temporal.changes(settings) == none, "inactive pass intensity does not disturb active history");
    temporal.complete(settings);
    settings.active_passes = 2;
    check(temporal.changes(settings) == passes, "activating another pass resets the cascade");
    temporal.complete(settings);
    settings.active_passes = 1;
    check(temporal.changes(settings) == passes, "reducing the pass count resets once");
    temporal.complete(settings);
    settings.active_passes = 2;
    check(temporal.changes(settings) == passes, "reactivating a pass cannot reuse its ancient history");
    temporal.complete(settings);
    settings.strengths[1] = 1.0f;
    check(temporal.changes(settings) == intensity, "changing an active later pass resets the cascade");
    temporal.complete(settings);

    settings.tone = 2.0f;
    check(temporal.changes(settings) == none, "disabled tuning values do not reset history");
    settings.tuning_enabled = true;
    check(temporal.changes(settings) == tuning, "enabling tuning resets once");
    temporal.complete(settings);
    settings.skin = 2.0f;
    check(temporal.changes(settings) == tuning, "active tuning changes reset once");
    temporal.complete(settings);
    settings.auto_mask = true;
    check(temporal.changes(settings) == tuning, "mask changes reset once");
    temporal.complete(settings);
    settings.tuning_enabled = false;
    check(temporal.changes(settings) == tuning, "disabling tuning marks a transition");
    temporal.complete(settings);
    check(temporal.changes(settings) == none, "unchanged disabled tuning does not continually reset");

    mgpu::neural_pipeline::schedule pipeline;
    mgpu::neural_pipeline::job submitted;
    submitted.seal = 1;
    submitted.fence = 10;
    submitted.settings = settings;
    submitted.settings_change = temporal.changes(settings);
    submitted.bound_strengths = settings.strengths;
    submitted.bound_strength_mask = 3;
    check(pipeline.begin(submitted), "submission owns immutable settings alongside its frame");
    settings.strengths[0] = 0.75f;
    check(pipeline.current().settings.strengths[0] == 2.0f &&
          pipeline.current().bound_strengths[0] == 2.0f, "in-flight UI edits cannot rewrite completion telemetry");
    check(pipeline.check(9) == mgpu::neural_pipeline::completion::waiting, "pending work does not acknowledge settings");
    check(pipeline.check(UINT64_MAX) == mgpu::neural_pipeline::completion::failed, "failed GPU work does not acknowledge settings");
    check(pipeline.check(10) == mgpu::neural_pipeline::completion::ready, "only a real fence completion retires the job");
    temporal.complete(pipeline.current().settings);
    pipeline.publish(10);
    check(temporal.changes(settings) == intensity, "completing the old job cannot swallow a newer UI change");
    temporal.complete(settings);
    check(temporal.changes(settings) == none, "newer UI change is acknowledged only after its own completion");
    temporal = {};
    check(temporal.changes(settings) == initial, "rebuilt stream starts without inherited settings history");

    struct strength_control
    {
        const char *name;
        float configuration::*value;
    };
    const std::array<strength_control, 3> controls{{
        {"Local Tone", &configuration::tone},
        {"Local Structure", &configuration::structure},
        {"Skin Structure", &configuration::skin},
    }};
    for (unsigned pass_count : {1u, 2u})
        for (const auto &control : controls)
        {
            history strength_history;
            configuration active;
            active.active_passes = pass_count;
            active.strengths = {2.0f, 2.0f};
            active.tuning_enabled = true;
            strength_history.complete(active);
            char label[160];
            active.*(control.value) = 2.0f;
            std::snprintf(label, sizeof label, "%s at high strength resets %u-pass history", control.name, pass_count);
            check(strength_history.changes(active) == tuning, label);
            strength_history.complete(active);
            std::snprintf(label, sizeof label, "%s kept high does not reset repeatedly (%u passes)", control.name, pass_count);
            check(strength_history.changes(active) == none && strength_history.revision() == 2, label);
            active.*(control.value) = 0.5f;
            std::snprintf(label, sizeof label, "%s high-to-low also resets once (%u passes)", control.name, pass_count);
            check(strength_history.changes(active) == tuning, label);
            strength_history.complete(active);
            check(strength_history.changes(active) == none, "settled strength retains temporal history");
        }

    history combined_history;
    configuration combined;
    combined.tuning_enabled = true;
    combined_history.complete(combined);
    combined.tone = combined.structure = combined.skin = 2.0f;
    check(combined_history.changes(combined) == tuning, "simultaneous strength edits use one frame-level reset");
    combined_history.complete(combined);
    check(combined_history.revision() == 2 && combined_history.changes(combined) == none,
          "simultaneous edits do not queue a separate reset for each slider");
    return failures == 0 ? 0 : 1;
}
