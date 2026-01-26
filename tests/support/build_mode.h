#pragma once

// Is this an optimized, uninstrumented build?
//
// The sanitizer presets are `Debug` — `-O0` *and* instrumented — which makes the
// simulator roughly two orders of magnitude slower there than under `dev`
// (`RelWithDebInfo`). A 2-second test becomes a 4-minute one.
//
// That matters for two different reasons, and it is worth keeping them apart:
//
//   1. **Wall-clock assertions are meaningless.** Timing anything under a sanitizer
//      measures the sanitizer. A test that fails for a reason it is not about is worse
//      than no test.
//   2. **Seed sweeps stop paying for themselves.** ASan and UBSan look for memory errors
//      and undefined behaviour, and they find those by *covering a code path*, not by
//      covering it forty times. The fortieth seed exercises the same lines as the first;
//      only the logic assertion needs the breadth, and the logic is build-independent
//      because the simulation is deterministic.
//
// So instrumented builds run a reduced sweep and skip the stopwatch. What they must never
// do is skip the *path* — every branch a sweep would reach is still reached at least once.
namespace tests {

inline constexpr bool kOptimizedUninstrumented =
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
    false;
#else
    true;
#endif
#else
    true;
#endif
#else
    false;
#endif

// How many seeds a sweep should run. Full breadth where it is affordable, a token few
// where it is not.
constexpr unsigned long long seeds(unsigned long long full, unsigned long long reduced) {
  return kOptimizedUninstrumented ? full : reduced;
}

// How long a simulated run should be. Same argument as `seeds()`, and week 5 is what made
// it necessary: replication put entries on the wire and an fsync behind every one of them,
// so a simulated second costs several times what it did, and the two simulation binaries
// went from seconds to **29 and 36 minutes** under UBSan.
//
// Shorter runs still walk every line — append, replicate, truncate, commit, elect, crash,
// recover — which is what a sanitizer is looking for. What they lose is *depth* of fault
// interleaving, and that is a logic property the optimized build covers at full length.
constexpr long long sim_ns(long long full, long long reduced) {
  return kOptimizedUninstrumented ? full : reduced;
}

// Some tests assert an outcome that belongs to one specific seed — a particular crash
// landing in a particular window. Those cannot be shortened without becoming a different
// test, so under instrumentation they step aside entirely rather than pretend.
// They are logic assertions, they are build-independent because the simulation is
// deterministic, and the optimized build runs them in full on every push.
constexpr bool kRunSeedSpecificScenarios = kOptimizedUninstrumented;

}  // namespace tests
