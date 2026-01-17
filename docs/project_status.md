# Project Status

> **Living document.** Update at the end of every work session — at minimum, the
> *Right now* block. This file answers three questions and nothing else: where are we,
> what's done, what's next. Narrative goes in `changelog.md`; rationale in `project_spec.md`.

**Last updated:** 2026-08-14 · **Week 3 of 9** · **Target:** ship before new-grad recruiting

---

## Right now

| | |
|---|---|
| **Phase** | Week 3 complete — **the load-bearing week landed**. Simulator core runs, first simulator-found bug in the journal |
| **Current focus** | Weeks 4–5: Raft — election → replication → persistence, under fault injection, with the invariant checker |
| **This week's demo** | ☑ `scripts/demo_week3.sh` — same seed twice → byte-identical trace; 1 simulated hour of 3 nodes in **~2 s**; 500 seeds green |
| **Blockers** | None |
| **Next decision due** | Raft's shape: `tick()`-driven state machine (etcd-style, trivially simulatable) vs. owning its own timers. Due before any `raft/` code lands |

## Milestones

Legend: ☐ not started · ◐ in progress · ☑ demo ran · ✗ cut

| Wk | Deliverable | Demo (the definition of done) | State |
|---|---|---|---|
| 1 | CMake skeleton, CI with sanitizers, `io/` interfaces, wire framing, callback event loop | `bench/echo` prints RPCs/sec and p99 over loopback | ☑ |
| 2 | Single-partition storage: segments, sparse index, CRC, crash recovery, append/fetch | `kill -9` mid-append → restart → `log-dump` shows no acked record lost | ☑ |
| 3 | Simulator core: virtual clock, deterministic scheduler, sim network + disk, trace hashing | Same seed twice → identical trace hash; 1 simulated hour in < 5 s | ☑ |
| 4–5 | Raft: elections → replication → persistence, under fault injection; invariant checker | 1000 seeds green in CI; ≥ 3 entries in the bug journal | ☐ |
| — | **⚠ Provision benchmark VMs** (`project_spec.md` §24) | 4 hosts reachable over SSH | ☐ |
| 6 | Real transport, multi-process cluster, client library, consumer offsets, deploy script | 3 real processes; produce and consume across a leader kill | ☐ |
| 7 | Multi-partition + thread-per-core, buffer pools, zero-copy fetch; profile and optimize | Flamegraph before/after; one number that moved | ☐ |
| 8 | Benchmark suite, open-loop load gen, failover experiments, Prometheus + Grafana, README | Full README benchmark table, reproducible from one script | ☐ |
| 9 | Buffer: chaos on the real cluster, follower reads, "what I'd do differently" written down | — | ☐ |

**Week 3 was load-bearing and it landed.** Weeks 4–5 now have the tooling they depend on:
every Raft bug from here should arrive with a seed attached.

## Accomplished

### Week 0 — Planning · 2026-08-12
- Full specification written: product requirements, engineering & technical design, execution plan
- Scope guard fixed (§3) — the non-goals list is now a commitment, not a mood
- Three scoping calls made and recorded: multi-partition promoted to week 7, schema
  compilers rejected, coroutines declared non-load-bearing
- Repo scaffolding: `CLAUDE.md`, `.env.example`, `.gitignore`, four living docs

### Week 1 — Transport stack · 2026-08-12
- Build system: CMake + 7 presets; `dev`/`asan`/`ubsan`/`tsan` all green locally
- CI workflow with the sanitizer matrix, two compiler frontends, and the ER-1 guard
- `base/`: Slice, Buffer (stream-capable), Result/ErrorCode, little-endian codecs, CRC32C with an ARM/x86 hardware path
- `io/` seam: Clock, Network, Disk, Random interfaces + real implementations
  (kqueue **and** epoll backends, pread/pwrite/F_FULLFSYNC, xoshiro256\*\*)
- `runtime/`: callback event loop with a min-heap timer wheel
- `wire/`: framed codec, API-key registry, max-frame enforcement before allocation
- 8 test binaries, 66 cases — unit + property, all green under 3 sanitizers
- `bench/echo` demo ran: **1.46M RPC/s, p50 22 µs / p99 38 µs / p99.9 93 µs**

### Week 2 — Single-partition storage · 2026-08-13
- `storage/record_batch`: 52-byte header per §16.2, CRC32C, reusable `BatchBuilder`,
  record iterator bounded by the batch's own declared length
- **Found and fixed a real correctness bug in the format**: the CRC did not cover
  `attributes`, so one flipped bit could turn an acked data batch into a control batch
  that fetch filters out — an I1 violation with a valid checksum. Field order changed;
  `project_spec.md` §16.2 corrected (`retrospective.md` §5)
- `storage/sparse_index`: binary-searched, never fsynced, and distrusted on decode
- `storage/segment`: append, read-from-offset, CRC-validated tail recovery with a
  `RecoveryReport` the tests assert against
- `storage/log`: offset authority, segment rolling, fsync-before-roll, discards
  segments stranded behind a truncation
- `io::Disk::list_directory()` — sorted, because recovery order must be reproducible
- `tools/log-dump` (read-only by construction) and `tools/crash-demo`
- 13 test binaries — added batch codec, sparse index, segment, log, and a seeded
  recovery property test. Green under dev, asan, ubsan, tsan
- libFuzzer target for the batch decoder; `LOGENGINE_BUILD_FUZZERS` now exists
- Demo ran: 5× `kill -9` mid-append + an injected torn tail, no acked record lost

### Week 3 — Simulator core · 2026-08-14
- `sim/scheduler`: virtual time, global event queue, total `(when, id)` ordering. Time
  moves only forward and only onto the next event, so idle time is free
- `sim/trace`: one line per event, FNV-1a rolling hash, ring buffer for violation reports
- `io/sim/SimClock`: virtual monotonic time + a wall clock faults may jump. Nothing may
  move the monotonic clock (§17)
- `io/sim/SimDisk`: durable vs visible images, crash keeps/tears/drops unflushed writes,
  silent bit-flips, I/O errors, and **power-off semantics** so a dying process's
  destructors cannot tidy up
- `io/sim/SimNetwork` + `Fabric`: the full readiness interface over in-memory wires —
  send window, short writes, per-link latency, directional partitions that reset
  connections. No byte-level drop/reorder/duplicate, and `retrospective.md` §2 says why
- `sim/simulation` + `sim/workload`: 3 nodes running the **real** `storage::Log` and the
  **real** `runtime::EventLoop` on virtual time, with an `Oracle` shadow model outside
  the nodes
- `tools/sim`: `--seeds`, `--seed`, `--dump-trace`, `--io-errors`
- **Bug journal entry #1** — the simulator caught recovery deleting 320 acked records on
  a transient read error, reproducible from seed 1, fixed with two regression tests
- Three bugs found *in the simulator itself* by review, all fixed: a use-after-free on
  the reply path, nodes with a clock offset running at two-thirds speed, and a node that
  failed to boot never being retried (`changelog.md`, `retrospective.md` §5)
- 17 test binaries; determinism (I7) is now a CI-required check
- Demo ran: identical trace hash across runs, 1 simulated hour in 1.9 s, 500 seeds green

## Up next

1. Decide Raft's shape — `tick()`-driven state machine vs. self-timed. Affects everything
2. `raft/`: leader election first, nothing else, until it is green over many seeds
3. Extend the `Oracle` to I3–I6 (no reordering, no overwrite of a committed entry, no
   read past the commit index, at most one leader per term)
4. Replication, then persistence of `currentTerm`/`votedFor` (fsync before any response
   that changes them — §13, not tunable)
5. Turn on `corruption_interval` once there is a peer to re-replicate from
6. Weeks 4–5 demo: 1000 seeds green in CI; ≥ 3 entries in the bug journal

## Acceptance criteria — the finish line

Tracked from day one so none of it is a surprise in week 9 (`project_spec.md` §8).

| # | Criterion | State |
|---|---|---|
| 1 | Three README commands produce a running 3-node cluster on a clean machine | ☐ |
| 2 | `./sim --seeds 1000` green, prints simulated node-hours | ◐ command works and 1000 seeds are green — but the criterion means *with Raft*, so it reopens in week 4 |
| 3 | Same seed twice → identical trace hash, CI-enforced | ☑ |
| 4 | Under one seed: `acks=1` loses data, `acks=quorum+fsync` does not — side by side in the README | ☐ |
| 5 | `kill -9` the leader under load → GIF of throughput dipping and recovering, no acked record lost | ☐ |
| 6 | Bug journal (`retrospective.md` §1) has ≥ 5 entries, each with seed / invariant / cause / fix commit | ◐ 1 of 5 |
| 7 | README benchmark table regenerable from `bench/run_all.sh` with full environment stated | ☐ |

## Risk watch

| Risk | State | Trigger to act |
|---|---|---|
| Coroutines eat week 1 | Mitigated by design — callback loop only | If any coroutine appears before week 7, stop |
| Week 3 slips → Raft debugged the slow way | **Closed** — simulator landed on time | — |
| Raft in two weeks | Watching | If elections aren't green by end of week 4, drop everything optional |
| Cloud account not ready for week 6 | Open | Apply for the student pack at end of week 5 |
| Thread-per-core claim untested | Mitigated — multi-partition moved to week 7 | If week 7 slips, this is the claim that weakens |
| Real-cluster benchmarks are pure infra | Open | Budget 2 days in week 8, not 2 hours |

## Cut list, in order

follower reads → compression → io_uring → coroutine retrofit → metadata controller →
multi-partition *(last resort)*

**Never cut:** the simulator, the benchmarks, the README, the bug journal.
