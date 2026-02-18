# CLAUDE.md

Replicated append-only log service in C++20 (Kafka's durable core), validated by a
deterministic fault simulator. Full spec: `project_spec.md` — read it before any
design decision. This file holds only what must be true in *every* session.

**Keep this file under ~80 lines.** Always in context; bloat here taxes every turn. Detail
belongs in `project_spec.md`. Update Status as work lands, rules only when one changes.

## Status

- **Phase:** weeks 4–6 + 8 done. `server::Broker` is the one driver — `sim/` **owns** one and
  never copies it. `bench/run_all.sh` gives every number from one command; NFR-3 passes
  (failover p99 489 ms). **Journal has 3; criterion 4 done; 1, 5, 7 ◐.**
- **Next:** (1) the GCP benchmark — tooling + procedure ready in `docs/benchmarking.md`, not
  yet run, (2) §13.1 **group commit** — specced, never built, one fsync per append caps
  throughput at the device flush rate — as §19 #5's before/after. Then `client/`.
- **Loopback:** brokers bind loopback unless given `--bind-all`. Every cluster run before
  week 8 — CI, demos, `run_all.sh` — was three processes on one machine.

## Non-negotiable rules

1. **The one rule (ER-1).** Nothing in `storage/`, `raft/`, `server/`, `client/` touches
   the OS — no `<chrono>` clocks, sockets, file I/O, `rand()`, `std::thread`. Everything
   goes through an `io/` interface injected at construction; CI greps for violations. If
   a change seems to need an exception, the design is wrong, not the rule. (`raft/` goes
   further: no `io/` at all except `Random`. It counts ticks; the driver acts.)
2. **Determinism (ER-2).** No `unordered_map`/`unordered_set` iteration in simulated
   paths, no pointer-value-dependent ordering, no uninitialized reads. The trace-hash
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
- Build via CMake presets, never raw flags: `dev|release|asan|ubsan|tsan|msan|fuzz`.
- Errors are `u16` wire codes, retryable vs terminal. Per-*partition* in responses.
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

The bug journal lives in `retrospective.md` §1 — no separate `bugs.md`. Five minutes an
entry, most persuasive artifact in the repo. **A bug that broke no invariant still gets
one** (see #2).

## Commands

```bash
cmake --preset dev && cmake --build --preset dev -j   # build (also: asan|ubsan|tsan)
ctest --preset dev                                    # 21 binaries, incl. the I7 canary
./scripts/check_one_rule.sh --self-test && ./scripts/check_one_rule.sh  # ER-1 guard
./build/dev/bench/echo --duration-s 5 --pipeline 32   # week 1 demo
./scripts/demo_week{2,3,4,5,6}.sh                     # weeks 2-6 demos
BENCH_LOCAL=true ./bench/run_all.sh                   # every number, one command
RATES= ./bench/run_all.sh                             # skip the sweep; ZONE=.. bench/run_gcp.sh
./build/dev/src/logengine --id 0 --port 9000 --dir d0 --peers 1@h:p,2@h:p  # +--bind-all
./build/dev/tools/log-dump <segment.log> --records    # read-only; never repairs
./build/dev/tools/sim --seeds 1000                    # fault sweep; prints node-hours
./build/dev/tools/sim --seed X --dump-trace /tmp/t    # reproduce a failure exactly
./build/dev/tools/sim --seed 2 --duration-s 60 --crash-s 4 --acks-1   # loses data (§13.2)
./build/dev/tools/sim --seed 4 --unsafe-metadata --crash-s 3 --restart-ms 120  # breaks I6
```

Not built yet: `client/`. Cut: Prometheus/Grafana (FR-11).

## Working agreements

- Each week ends with a **runnable demo**. No demo, no week — regardless of code volume.
- Cut order when behind: follower reads → compression → io_uring → coroutines →
  metadata controller → multi-partition (last resort).
- **Never cut:** the simulator, the benchmarks, the README, the bug journal.
- Benchmark numbers ship with hardware, kernel, fs, mount options, offered load, and the
  exact command; a p99 without an offered load is meaningless. Publish bad numbers with the
  explanation. `BENCH_LOCAL=true` and loopback results never reach the README.
