# CLAUDE.md

Replicated append-only log service in C++20 (Kafka's durable core), validated by a
deterministic fault simulator. Full spec: `project_spec.md` — read it before any
design decision. This file holds only what must be true in *every* session.

**Keep this file under ~80 lines.** It is always in context; bloat here is a tax on
every turn. Detail belongs in `project_spec.md`. Update the Status block as work
lands; update the rules only when a rule actually changes.

## Status

- **Phase:** week 3 done — simulator core (`sim/` + `io/sim/`): virtual clock, seeded
  scheduler, sim disk/network, trace hash. 17 test binaries green under
  dev/asan/ubsan/tsan. `scripts/demo_week3.sh` ran. **Bug journal has its first entry.**
- **Next:** weeks 4–5 — Raft: election → replication → persistence, under fault
  injection. Decide `tick()`-driven vs self-timed first. Demo: 1000 seeds green in CI;
  ≥ 3 bug-journal entries.
- **⚠ End of week 5:** provision benchmark VMs (`project_spec.md` §24) — student-pack
  approval takes days, and week 6's deploy script has nothing to target without it.

## Non-negotiable rules

1. **The one rule (ER-1).** Nothing in `storage/`, `raft/`, `server/`, `client/` touches
   the OS. No `<chrono>` clocks, sockets, file I/O, `rand()`, `std::thread`. Everything
   goes through an `io/` interface injected at construction. CI greps for violations.
   This is what makes the simulator possible — if a change seems to need an exception,
   the design is wrong, not the rule.
2. **Determinism (ER-2).** No `unordered_map`/`unordered_set` iteration in simulated
   paths. No pointer-value-dependent ordering. No uninitialized reads. The trace-hash
   test is the canary, not discipline.
3. **Never assume dense offsets.** Control records occupy real offsets and are filtered
   from fetch. Assert monotonicity, never density. Client must not compute
   `last_offset + 1`.
4. **Two durability knobs, never conflated.** Raft metadata (`currentTerm`, `votedFor`)
   fsyncs before *any* response that changes it, always, not tunable. User data follows
   the request's `acks`. See §13.
5. **Never print a seed-less failure.** A lost seed is a lost bug.
6. **Zero heap allocation on append/fetch (ER-4).** Buffers come from per-core pools.
7. **Hot paths return `Result<T, ErrorCode>`.** Exceptions only at startup/config.

## Conventions

- C++20, Clang 17+ primary / GCC 13 in CI. `-Wall -Wextra -Wconversion -Werror`.
- Build via CMake presets, never raw `cmake` flags: `dev | release | asan | ubsan |
  tsan | msan | fuzz`.
- Errors are `u16` wire codes, classified retryable vs terminal. Per-*partition* in
  responses, never per-connection.
- Little-endian on disk and wire. Every API versioned from v0.

## Living docs — update these as work lands, not at the end

| File | Update when | Rule |
|---|---|---|
| `docs/project_status.md` | End of every session | At minimum the *Right now* block + milestone state |
| `docs/changelog.md` | Anything notable ships, breaks, or reverses | Append under `[Unreleased]`. Record reversed decisions, don't delete them |
| `docs/architecture.md` | A component, boundary, or dependency rule changes | Structure only; mark unbuilt pieces `[planned]` |
| `docs/retrospective.md` §1 | **Every simulator-found bug** | Seed · invariant · symptom · cause · fix commit, then the story. Fix commit references the entry number back |
| `docs/retrospective.md` §2–11 | While confused, not after | Write the *wrong belief*, not just the fix. Details decay in ~48h |
| `CLAUDE.md` (this file) | A rule actually changes | Keep under ~80 lines |

The bug journal lives in `retrospective.md` §1 — there is no separate `bugs.md`, it is the
most persuasive artifact in the repo, and it costs five minutes an entry. Do not skip it.

## Commands

```bash
cmake --preset dev && cmake --build --preset dev -j   # build (also: asan|ubsan|tsan)
ctest --preset dev                                    # 17 binaries, incl. the I7 canary
./scripts/check_one_rule.sh --self-test && ./scripts/check_one_rule.sh  # ER-1 guard
./build/dev/bench/echo --duration-s 5 --pipeline 32   # week 1 demo
./scripts/demo_week2.sh ; ./scripts/demo_week3.sh     # week 2 + 3 demos
./build/dev/tools/log-dump <segment.log> --records    # read-only; never repairs
./build/dev/tools/sim --seeds 500                     # fault sweep; prints node-hours
./build/dev/tools/sim --seed X --dump-trace /tmp/t    # reproduce a failure exactly
```

Not built yet — week 4+: `tools/sim-replay`, `bench/run_all.sh`.

## Working agreements

- Each week ends with a **runnable demo**. No demo, no week — regardless of code volume.
- Cut order when behind: follower reads → compression → io_uring → coroutines →
  metadata controller → multi-partition (last resort).
- **Never cut:** the simulator, the benchmarks, the README, the bug journal.
- Benchmark numbers ship with hardware, kernel, fs, mount options, offered load, and the
  exact command. A p99 without an offered load is meaningless. Publish bad numbers with
  the explanation. `BENCH_LOCAL=true` results validate the harness and never reach the README.
