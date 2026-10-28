#include "../src/tagged_motion_policy.hpp"
#include "../src/source_marker_slot.hpp"
#include <windows.h>
#include <cstdio>

int main()
{
    unsigned failures = 0;
    const auto check = [&failures](bool passed, const char *name)
    {
        std::printf("%s: %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    using namespace mgpu::tagged_motion;
    source_priority policy;
    check(policy.accepts(false), "legacy route remains until a successful tagged copy");
    check(!policy.recorded(false), "heuristic recording does not claim authority");
    check(policy.recorded(true), "first tagged copy requests one transition reset");
    check(!policy.accepts(false), "late heuristic cannot overwrite tagged motion");
    check(policy.accepts(true) && !policy.recorded(true), "rotating tagged sources do not repeat resets");
    check(whole_extent(0, 0, 0, 0, 2562, 1441), "unspecified extent means whole resource");
    check(whole_extent(0, 0, 2562, 1441, 2562, 1441), "explicit whole extent accepted");
    check(!whole_extent(0, 1, 2562, 1441, 2562, 1441), "offset extent rejected");
    check(!whole_extent(0, 0, 2560, 1440, 2562, 1441), "cropped extent rejected");
    check(!whole_extent(0, 0, 0, 1441, 2562, 1441), "partial empty extent rejected");
    int original = 0, replacement = 0, other = 0;
    void *slots[] = {nullptr, &original, &other};
    pointer_matches unique;
    unique.scan(slots, sizeof slots, &original);
    check(unique.count == 1 && unique.unique_slot() == &slots[1], "exact aligned unique pointer found");
    check(slots[1] == &original, "inventory is read only");
    check(InterlockedCompareExchangePointer(unique.unique_slot(), &replacement, &original) == &original,
          "replacement only succeeds for the observed original");
    slots[1] = &other;
    check(InterlockedCompareExchangePointer(unique.unique_slot(), &original, &replacement) == &other &&
          slots[1] == &other, "restore preserves another writer's changes");
    slots[0] = slots[1] = &original;
    pointer_matches ambiguous;
    ambiguous.scan(slots, sizeof slots, &original);
    check(ambiguous.count == 2 && ambiguous.unique_slot() == nullptr, "ambiguous slots refused");
    check(select_cached_slot(ambiguous.unique_slot(), nullptr, &original) == nullptr,
          "ambiguous pointers still refused without a validated PCL route");
    auto **verified = select_cached_slot(ambiguous.unique_slot(), &slots[1], &original);
    check(verified == &slots[1], "validated caller identifies PCL cache despite a duplicate pointer");
    check(InterlockedCompareExchangePointer(verified, &replacement, &original) == &original &&
          slots[0] == &original && slots[1] == &replacement,
          "only the caller-validated cache changes while duplicate stays untouched");
    check(select_cached_slot(nullptr, &slots[1], &original) == nullptr,
          "changed validated cache refuses the old function");
    check(InterlockedCompareExchangePointer(verified, &original, &replacement) == &replacement,
          "caller-validated cache restores its own hook");
    check(pcl_marker_route_matches(0xa463911e, 0x10d43000, pcl_resolver_bytes, pcl_caller_bytes),
          "observed game identity and both PCL references accepted");
    check(!pcl_marker_route_matches(0xa463911f, 0x10d43000, pcl_resolver_bytes, pcl_caller_bytes) &&
          !pcl_marker_route_matches(0xa463911e, 0x10d44000, pcl_resolver_bytes, pcl_caller_bytes),
          "other executable builds cannot authorize the known PCL cache");
    unsigned char changed_resolver[sizeof pcl_resolver_bytes];
    std::memcpy(changed_resolver, pcl_resolver_bytes, sizeof changed_resolver);
    changed_resolver[3] ^= 1;
    unsigned char changed_caller[sizeof pcl_caller_bytes];
    std::memcpy(changed_caller, pcl_caller_bytes, sizeof changed_caller);
    changed_caller[2] ^= 1;
    check(!pcl_marker_route_matches(0xa463911e, 0x10d43000, changed_resolver, pcl_caller_bytes) &&
          !pcl_marker_route_matches(0xa463911e, 0x10d43000, pcl_resolver_bytes, changed_caller),
          "either PCL reference targeting another slot invalidates the route");
    check(pcl_resolver_rva + sizeof pcl_resolver_bytes + 0x95aa9e1 == pcl_cache_rva &&
          pcl_caller_rva + sizeof pcl_caller_bytes + 0x95aa841 == pcl_cache_rva,
          "resolver and actual indirect call resolve the same cache");
    pointer_matches absent;
    absent.scan(slots, sizeof slots, &replacement);
    check(absent.unique_slot() == nullptr, "missing target has no writable slot");
    pointer_matches clipped;
    clipped.scan(slots, sizeof(void *) - 1, &original);
    check(clipped.count == 0, "incomplete pointer excluded");
    return failures == 0 ? 0 : 1;
}
