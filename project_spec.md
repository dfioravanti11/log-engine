# Distributed Replicated Log Engine — Project Specification

**Status:** draft v1 · **Owner:** solo · **Budget:** ~9 weeks (~15–20 hrs/wk) · **Language:** C++20

This document has two parts:

- **Part I — Product Requirements.** What the system does, who it's for, what "done" means, and what is explicitly out of scope.
- **Part II — Engineering & Technical Design.** Tech stack, engineering requirements, architecture, system design, and the build plan.

A third short part (**Part III — Execution**) covers milestones, risk, and cut order, because a spec with no schedule is a wish list.

---

# Part I — Product Requirements

## 1. Problem statement

Build a **replicated, append-only log service** — the durable core of Kafka — and *prove it correct* with a deterministic fault simulator in the style of FoundationDB and TigerBeetle.

Clients append batched records to a named partition. Records are durable, totally ordered within the partition, and replicated across 3 nodes. Consumers read from any offset and stream forward. The system survives node crashes, network partitions, disk corruption, and loss of unflushed writes on power failure — and it can demonstrate exactly what it survives, reproducibly, from a seed.

**The differentiator is the simulator, not the log.** A working Raft implementation is a common student project. A Raft implementation whose bugs are each reproducible from a printed seed, with a written bug journal, is not.

## 2. Goals

| # | Goal | Success signal |
|---|---|---|
| G1 | Durable, ordered, replicated log with a clear correctness contract | Invariant checker green over ≥1000 seeds in CI |
| G2 | Reproducible correctness evidence | Every bug in the journal (`docs/retrospective.md` §1) replays from its seed via `sim-replay` |
| G3 | Honest, methodology-first performance numbers | README benchmark table reproducible from one script |
| G4 | Systems-programming depth on paper | Custom wire protocol, event loop, on-disk format, hand-written Raft — no borrowed consensus library |

## 3. Non-goals (scope guard)

This list goes verbatim into the README. Naming what you didn't build reads as judgment; leaving it unnamed reads as a gap.

- No consumer groups, rebalancing protocol, or group coordinator
- No cross-partition transactions or exactly-once (idempotency within a producer session only)
- No tiered/object storage; no key-based compaction (retention by time/size only)
- No auth, TLS, quotas, or multi-tenancy
- No cluster metadata controller — membership is **static config**
- No cross-partition ordering, ever, by design
- No compression in v1 (format reserves the attribute bit)

## 4. Users and use cases

There is one real user — a reviewer reading the repo — and two simulated ones the system must serve correctly:

| Actor | Needs | Primary surface |
|---|---|---|
| **Producer** | Append batches at high throughput; choose a durability level; never see a silent duplicate on retry | `Produce` API, client library with batching + idempotent sequences |
| **Consumer** | Read from an arbitrary offset, stream forward, never observe a record that later disappears | `Fetch` (long-poll), `ListOffsets`, `OffsetCommit/Fetch` |
| **Operator / reviewer** | Inspect a segment, replay a failing seed, see the invariants | `log-dump`, `sim-replay`, README, the bug journal |

## 5. Functional requirements

**FR-1 — Append.** A producer sends a batch of records to `(topic, partition)` with an `acks` level. The broker returns the base offset assigned, or a per-partition error code.

**FR-2 — Durability levels.**

| `acks` | Acked when | Data lost if |
|---|---|---|
| `0` | Request written to socket | Anything |
| `1` | Leader appended to page cache | Leader crashes before fsync **and** before replication |
| `quorum` | Majority appended | Majority simultaneously loses unflushed writes |
| `quorum+fsync` | Majority fsynced | Majority disk loss |

**FR-3 — Fetch.** A consumer requests `(partition, offset, max_bytes)` and receives batches starting at or after that offset, up to the high watermark. Long-poll: the request parks for up to `max_wait_ms` if no data is available.

**FR-4 — Visibility.** A consumer may only read offsets **strictly below the commit index** (the high watermark). Records exist on disk before they are readable; that gap is replication latency and must be measured, not hidden.

**FR-5 — Idempotent produce.** Each producer has a `(producer_id, producer_epoch)` and a monotonically increasing `base_sequence`. A retried batch with an already-seen sequence is acked without being re-appended. Out-of-order sequences are rejected with a terminal error.

**FR-6 — Offset semantics.** Offsets are **monotonic but not dense**: internal control records (leader no-op entries, and later membership changes) occupy real offsets and are filtered from fetch responses. The client library must never assume `next_offset == last_offset + 1`.

**FR-7 — Crash recovery.** On restart, a broker recovers its log by scanning the tail of the last segment, validating CRC per batch, truncating at the first failure or short read, and rebuilding the sparse index tail. A torn write at the tail is **normal**, logged at info.

**FR-8 — Leader election and failover.** On leader loss, a new leader is elected and the cluster resumes accepting appends. Clients see `NOT_LEADER` with a leader hint and retry with jittered backoff.

**FR-9 — Follower catch-up.** A rejoining follower reconciles divergence via the leader-epoch cache (truncate to the end of the last common epoch). A follower behind the retention boundary is caught up by **segment file shipping** (`InstallSnapshot`).

**FR-10 — Retention.** Segments are deleted by age or total size. Retention never deletes below the highest offset any in-sync replica still needs.

**FR-11 — Observability.** Prometheus metrics on the real runtime: append/fetch rate, per-op latency histograms, commit-index lag per follower, leader term, fsync latency, segment count/bytes.

**FR-12 — Tooling.** `log-dump <segment>` prints human-readable batch headers with CRC validation and epoch boundaries. `sim-replay <seed>` reruns a seed, dumps the event trace, and can break at event N.

## 6. Correctness requirements (the invariant set)

No general linearizability checker. These six concrete invariants run inside the simulator after every scheduled event:

| # | Invariant |
|---|---|
| I1 | An acked write (at its stated `acks` level) is never lost |
| I2 | Offsets are monotonic in commit order (gaps allowed — see FR-6) |
| I3 | No reordering within a partition |
| I4 | A committed entry is never overwritten |
| I5 | No consumer ever reads an offset ≥ the commit index |
| I6 | At most one leader per term |

Plus one meta-invariant, tested separately: **I7 — the same seed produces a byte-identical event trace** (see §14).

**Added in week 5 — I8, liveness.** Every invariant above is a *safety* property, and a
cluster that does nothing at all satisfies every one of them. That gap is not theoretical:
it let a livelock hide behind a thousand green seeds for a week (`docs/retrospective.md`
§1 entry #2), with the cluster electing a new leader every 200 ms and committing nothing.

| # | Invariant |
|---|---|
| I8 | The cluster is never leaderless for longer than a bounded recovery time *once a majority can communicate* |

It is stated conditionally because the unconditional version is false — during a partition
that costs the majority, having no leader is correct. The simulator measures the longest
leaderless stretch and the caller decides what is tolerable given the faults it injected.
**"A leader exists" is not the invariant; "the cluster gets one back within a bounded time
of being able to" is.**

## 7. Non-functional requirements

| ID | Requirement | Target |
|---|---|---|
| NFR-1 | Sustained throughput, 1 KB records, `acks=quorum`, 3 nodes | ≥ 100 MB/s (stretch: within 0.7× of Kafka on the same hardware) |
| NFR-2 | Append latency p99, open-loop at stated offered load | ≤ 10 ms at 70% of measured saturation |
| NFR-3 | Leader failover time, p50/p99 over ≥50 induced failures | p99 ≤ 3× election timeout |
| NFR-4 | Simulation speed | ≥ 1 simulated cluster-hour per 5 wall-clock seconds |
| NFR-5 | Hot-path allocation | Zero heap allocations on append/fetch, asserted by a test |
| NFR-6 | CI fast job | Unit + property + 50 fixed seeds in under 5 minutes |

Targets are hypotheses. If a number lands badly, publish it with the explanation — a slow path you understand beats a fast path you can't account for.

## 8. Acceptance criteria ("done")

The project is done when all of these hold on a clean machine:

1. `cmake --build` → three commands in the README produce a running 3-node cluster
2. `./sim --seeds 1000` runs green, printing simulated node-hours
3. `./sim --seed <X>` twice produces identical trace hashes (CI-enforced)
4. Under a single seed, `acks=1` with unflushed-write-loss demonstrably **loses data** and `acks=quorum+fsync` demonstrably **does not** — the side-by-side result is in the README
5. `kill -9` on the leader of a live 3-node cluster under load: throughput graph dips and recovers, no acked record lost, captured as a GIF
6. The bug journal (`docs/retrospective.md` §1) has ≥ 5 entries, each with a seed, a violated invariant, a cause, and a fix commit
7. The README benchmark table is regenerable from `bench/run_all.sh` with stated hardware, kernel, fs, and mount options

---

# Part II — Engineering & Technical Design

## 9. Tech stack

### 9.1 Language and toolchain

| Choice | Decision | Rationale |
|---|---|---|
| Language | **C++20** | Concepts, `std::span`, designated initializers, `<bit>`. Coroutines deliberately **not** load-bearing (see §12.1) |
| Build | **CMake ≥ 3.24** + Ninja, presets in `CMakePresets.json` | Presets make the CI matrix and the local build the same thing |
| Compilers | Clang 17+ primary, GCC 13 in CI | Two frontends catch UB the other misses |
| Standard flags | `-Wall -Wextra -Wconversion -Werror`, `-fno-exceptions` **not** used | Exceptions allowed at startup/config; hot paths return `Result<T>`/error codes |
| Package mgmt | **vcpkg** manifest mode | Reproducible on a clean machine, which acceptance criterion #1 requires |

### 9.2 Dependencies (buy)

| Library | Used for | Notes |
|---|---|---|
| `crc32c` (Google) or xxhash | Batch checksums | Hardware CRC32C via SSE4.2/ARMv8 |
| liburing | Batched I/O submission | **Stretch only** — v1 ships `epoll` + `pwritev` (§12.2) |
| spdlog | Logging | Disabled/compiled-out on hot paths |
| GoogleTest + RapidCheck | Unit + property tests | RapidCheck for round-trip and recovery properties |
| libFuzzer (Clang built-in) | Wire decoder, segment recovery | Corpus checked into repo |
| prometheus-cpp | Metrics endpoint | Real runtime only; the simulator has no HTTP |
| Catch2/benchmark or a hand-rolled harness | Micro-benchmarks | Macro-benchmarks are custom (open-loop, §17) |

### 9.3 Explicitly **not** used

- **Any Raft library** — defeats the purpose
- **gRPC / protobuf-over-HTTP2** — the framing story is part of the project
- **Cap'n Proto / FlatBuffers** — considered, rejected for v1: the on-disk and wire formats are hand-written because the byte layout *is* a design artifact (CRC placement, §13.1). Adds a dependency without adding evidence of skill.
- **Boost.Asio** — the event loop is the deliverable

### 9.4 Environment

- Dev + benchmark: Linux (kernel ≥ 5.15), ext4 or xfs, mount options recorded
- macOS builds for local iteration are best-effort; the simulator is portable, the real runtime is Linux-only
- Benchmarks: 3 cloud VMs (4 vCPU, local NVMe) or containers with `tc netem` for latency injection

## 10. Engineering requirements

These are constraints on *how* the code is written, enforced mechanically where possible.

**ER-1 — The one rule.** Nothing in `storage/`, `raft/`, `server/`, or `client/` may touch the OS directly. No `<chrono>` clocks, no sockets, no file I/O, no `rand()`, no `std::thread`. All of it goes through an `io/` interface injected at construction.

> *Enforcement:* a ~10-line CI script greps for `#include <sys/`, `chrono::.*::now`, `rand(`, `std::thread`, `std::this_thread` outside `io/real/`, `runtime/`, `tools/`, `bench/`. This single check is what makes the simulator possible and is the cheapest high-value thing in the repo.

**ER-2 — Determinism.** No iteration over `unordered_map`/`unordered_set` in simulated code paths (use sorted containers or `vector`). No pointer-value-dependent ordering. No uninitialized reads (MSAN in CI). Violations are caught by the trace-hash test (§14.3), not by discipline alone.

**ER-3 — Error handling.** Hot paths return `Result<T, ErrorCode>`; no exceptions past construction. Every error is a `u16` code in the wire enum, classified **retryable** or **terminal**.

**ER-4 — Allocation discipline.** No heap allocation on append or fetch. Buffers come from per-core fixed-size page-aligned free lists. Asserted by a test that hooks the allocator and fails on any call during a steady-state loop.

**ER-5 — Sanitizers.** ASAN+UBSAN on every commit; TSAN on the real runtime only (the simulator is single-threaded by construction, so TSAN there is noise); MSAN nightly.

**ER-6 — Every week ends with a runnable demo.** If the demo doesn't run, the week isn't done regardless of how much code exists.

**ER-7 — Documentation is incremental.** The README and the living docs in `docs/` are written as the work happens, not in week 8. A bug-journal entry costs five minutes and is the most persuasive artifact in the repo.

**ER-8 — Commit hygiene.** Bug-fix commits reference the journal entry number; the journal references the commit SHA. The link runs both directions or the evidence is worthless.

## 11. Architecture

```
   producer ──┐                        ┌── Broker 0 (leader) ──┐
              ├── binary proto / TCP ──┤                       │  Raft group
   consumer ──┘                        ├── Broker 1 ───────────┤  per partition
                                       └── Broker 2 ───────────┘
                                                  │
                              segment files + sparse index + raft.state
```

### 11.1 Layer stack

```
┌──────────────────────────────────────────────────────────┐
│ client/    producer (batching, idempotency) · consumer    │
├──────────────────────────────────────────────────────────┤
│ server/    broker · partition→core assignment · dispatch  │
├──────────────────────────────────────────────────────────┤
│ raft/      election · replication · persistence · matching│
├──────────────────────────────────────────────────────────┤
│ storage/   segment · sparse index · recovery · retention  │
│            · leader epoch cache                            │
├──────────────────────────────────────────────────────────┤
│ wire/      framing · codecs · schemas · versioning        │
├──────────────────────────────────────────────────────────┤
│ runtime/   event loop · timers · MPSC queue · buffer pool │
├──────────────────────────────────────────────────────────┤
│ io/        Clock │ Network │ Disk │ Random    ← ER-1 line │
│            real/            sim/                          │
└──────────────────────────────────────────────────────────┘
```

Everything above the `io/` line is portable and runs identically in production and in the simulator. That is the entire architectural bet.

### 11.2 Repository layout

```
src/
  base/       slice, buffer, crc32c, intrusive list, Result<T>
  io/         Clock, Network, Disk, Random interfaces + real/ and sim/ impls
  runtime/    event loop, timers, MPSC queue, buffer pool
  wire/       framing, codecs, schemas
  storage/    segment, sparse index, log, recovery, retention, epoch cache
  raft/       election, replication, persistence, log matching
  server/     broker, partition→core assignment, request dispatch
  client/     producer, consumer
  sim/        scheduler, fault injector, invariant checker, trace
tests/        unit/ property/ fuzz/ simulation/
bench/        load generator, failover harness, run_all.sh
tools/        log-dump, sim-replay
docs/         architecture.md, changelog.md, project_status.md,
              retrospective.md (§1 is the bug journal), diagrams
```

## 12. System design — the core decision

**Replicate the user's log directly as the Raft log**, rather than running Raft over a separate state machine.

*Alternative considered:* the textbook layering — Raft log as an ordered command stream, user log as the state machine it feeds. *Why it lost:* every record is written twice (once to the Raft log, once to the state machine), doubling write amplification and fsync cost, and requiring a separate snapshot mechanism.

*Consequence:* the state **is** the log, so a snapshot is just a retention boundary. This choice generates four follow-up problems, and having answers to all four is what separates this from a Raft tutorial. Each has a real Kafka analogue.

### 12.1 Consumers must not read past the commit index

Raft followers truncate uncommitted suffixes when a new leader's log diverges. If the user log *is* the Raft log, a consumer could read a record that later gets truncated — an "un-read," which no log service may do.

**Resolution:** the Raft commit index **is** Kafka's high watermark. Fetch is clamped to `offset < commitIndex` (I5). The gap between "on disk" and "readable" is replication latency; benchmark both append-ack latency and end-to-end produce→consume latency to expose it (§17, benchmark #10).

### 12.2 Internal entries occupy offsets

Raft appends entries that aren't user data: the no-op a new leader commits to establish its term's commit index, and (later) membership changes.

| Option | Verdict |
|---|---|
| Side channel — keep them out of the log | **Rejected.** Reintroduces a second write path and a second thing to make durable — it un-makes the core decision |
| **Control records in the log** | **Chosen.** They take real offsets; the fetch path filters them; offset numbering still advances |

This is exactly Kafka's transaction-marker design, and it is why Kafka consumer offsets are not dense. Set bit 1 in the batch `attributes`. The invariant checker asserts **monotonicity, not density** (I2).

### 12.3 Leader epoch per batch

Comparing offsets alone cannot detect divergence — two nodes can hold different records at the same offset. Every batch header carries the Raft term that produced it (`leader_epoch`), and each node keeps a **leader epoch cache**: an append-only `vector<{u32 epoch, u64 start_offset}>`, binary-searched, rebuilt on startup by scanning batch headers.

Rejoin protocol: ask the leader for the end offset of my last known epoch → truncate to it → resume replication. This is **Kafka KIP-101**, which fixed a real log-divergence bug in production Kafka. The problem is not hypothetical.

### 12.4 Snapshot install is segment shipping

A follower behind the leader's retention boundary can't be caught up by `AppendEntries`. In classic Raft you'd ship a state-machine snapshot; here the "snapshot" is the byte range of segment files from the retention boundary forward. `InstallSnapshot` = segment file transfer + the leader's epoch cache for that range. Simpler than the paper — say so in the README.

### 12.5 Membership changes — out of scope, answer ready

Static config in v1. The design composes: single-server joint-consensus changes (Raft §4) encoded as **control records in the log**, which works precisely because control records already exist for §12.2.

## 13. System design — durability

Raft's safety proof assumes stable storage. A node that votes in term T, crashes, and restarts must still remember that vote; an amnesiac node can vote twice in one term and elect two leaders. There are **two different durability knobs** and conflating them is the classic bug:

| What | Requirement | Tunable? |
|---|---|---|
| Raft metadata (`currentTerm`, `votedFor`) | fsync **before** responding to any RPC that changes it. Always. | **No** |
| Log entries, for Raft's log-matching guarantee | fsync before acking `AppendEntries` — or accept a weaker crash model | Only via §13.1 |
| Log entries, for the user's `acks` setting | Whatever the client asked for | Yes (FR-2) |

### 13.1 Acking before fsync

Batching fsyncs is where the throughput is. Two honest paths:

**Status after week 8 — specified, not implemented.** `Broker::propose()` fsyncs per batch,
so there is one device flush per Raft entry and no amortization at all. The benchmark is
what surfaced it: median append-ack latency of 8.5 ms is one `F_FULLFSYNC`, and throughput
flattens at ~1,228 records/s, which is the flush rate times the batch size and nothing to
do with this code. It is the top of the optimization list and the intended subject of
§19 #5's before/after measurement — the "before" is taken. See `docs/retrospective.md` §5.

- **Chosen — delay the response, not the write.** Append to the page cache immediately; respond only after the next group fsync completes (every N ms or N bytes). Throughput comes from amortizing fsync across many in-flight appends, not from lying about durability.
- **Documented alternative — explicit recovery protocol.** A node that may have lost unflushed writes rejoins as a *learner* with a fresh identity, refuses to vote until caught up past its pre-crash log end, and can't count toward quorum meanwhile. Roughly Viewstamped Replication's recovery protocol / TigerBeetle's storage-fault model. Described in the README as evaluated-and-rejected-for-scope.

### 13.2 The headline result

Under one seed, with the unflushed-write-loss fault injected: `acks=1` loses records; `acks=quorum+fsync` does not. Same seed, printed in the README. That single side-by-side is worth more in an interview than the rest of the correctness section combined.

## 14. System design — the simulator

Every source of nondeterminism sits behind an `io/` interface with two implementations:

| Interface | Real | Simulated |
|---|---|---|
| `Clock` | `CLOCK_MONOTONIC` | virtual time, advanced by the scheduler |
| `Network` | epoll + TCP | in-memory message queues with injectable faults |
| `Disk` | `pwritev` / `fsync` (io_uring stretch) | in-memory files with latency, torn writes, unflushed-loss |
| `Random` | `/dev/urandom` | seeded PRNG, the single source of all nondeterminism |

In simulation, all N nodes run as state machines on **one thread**, scheduled deterministically from a seeded RNG, on virtual time. Hours of cluster life simulate in seconds (NFR-4).

### 14.1 Fault menu

drop · reorder · duplicate · delay · **symmetric and asymmetric** partitions · crash/restart · slow disk · unflushed-write loss · silent bit-flip corruption · clock jumps.

Asymmetric partitions (A→B works, B→A doesn't) get their own test: they must not cause a livelock of repeated elections.

### 14.2 Invariant checker

Runs after every scheduled event, checking I1–I6 against a shadow model held by the simulator (the set of acked writes, the per-node logs, the leader-per-term map). On violation: print the seed, the event index, the violated invariant, and the last 50 trace lines.

### 14.3 Determinism is a tested property, not an aspiration

Determinism breaks silently the first time someone iterates an `unordered_map` or calls `chrono::now()`. Defense:

- Emit an **event trace** — one line per scheduled event: `(virtual_time, node, event_type, key_fields)` — and hash it
- A test runs the same seed twice and asserts identical hashes. **Required CI check.** When it fails, diffing the two traces localizes the nondeterminism immediately.

### 14.4 Tooling

- **`sim-replay <seed>`** — rerun a seed, dump the full trace, `--break-at N` for a debugger session. Turns "flaky distributed bug" into a breakpoint.
- **`log-dump <segment>`** — batch headers, CRC validation, epoch boundaries. Used daily in weeks 2–5.
- **The bug journal** — `docs/retrospective.md` §1, one entry per simulator-found bug. Deliberately not a separate file: the record and the story are the same bugs, and two files would drift. The record half:

```
### #7 — Follower ack'd entries it hadn't fsynced
seed:       0x3f2a91c4
invariant:  I1 — acked writes are never lost
symptom:    3 records lost after node 2 restart w/ unflushed-write-loss at t=41.2s
cause:      AppendEntries response sent from the write-submit path, not the completion path
fix:        commit a1b3f9e
```

## 15. System design — concurrency and runtime

**Thread-per-core, shared-nothing.** One pinned event loop per core; each partition owned by exactly one core, so partition state is touched by exactly one thread — no locks on the hot path. Cross-core work routes through a cache-line-aligned lock-free MPSC queue with explicit backpressure.

**The honest trade-off** (claiming thread-per-core is strictly better is the wrong answer and interviewers know it):

- **For:** no lock acquisition on the hot path, so tail latency doesn't degrade under contention; partition state stays resident in one core's L2; no false sharing; scheduling is explicit rather than at the mercy of the OS scheduler.
- **Against:** no work-stealing — one hot partition saturates one core while others idle, which a thread pool load-balances for free. Requires a partition→core assignment strategy and, at scale, a rebalancing story. Blocking anywhere in an event loop stalls every partition on that core.
- **When the thread pool wins:** few partitions, highly skewed load, or unavoidable blocking calls.

> **Scoping note — read this one.** Thread-per-core is only *exercised* with multiple partitions. If multi-partition stays a week-9 stretch, the strongest concurrency claim in the project is untested. **Recommendation: promote multi-partition (N partitions, N independent Raft groups, round-robin to cores) into week 7 and cut io_uring instead.** Once single-partition Raft works, a second partition is mostly plumbing, and it converts a design paragraph into a benchmark.

### 15.1 Event loop — deliberately boring

Plain callback / explicit state machine, **not** C++20 coroutines. Coroutines are ergonomics, not capability, and they are the single most likely way to lose two weeks in week 1. Retrofit in week 7 if there's slack, or never.

### 15.2 I/O

v1 ships `epoll` + `pwritev` + `fsync`. io_uring is a stretch goal on the cut list — ship epoll and nobody will care; ship a half-finished io_uring path and everyone will.

## 16. System design — data structures and formats

### 16.1 Key data structures

| Structure | Representation | Notes |
|---|---|---|
| Sparse index | sorted `vector<{u32 rel_offset, u32 file_pos}>`, binary search | One entry per ~4 KB. **Never fsynced** — rebuilt by scan on unclean shutdown, which is why it may be lossy |
| Leader epoch cache | append-only `vector<{u32 epoch, u64 start_offset}>` | Binary search; rebuilt by header scan on startup |
| Producer dedup | bounded ring per `(producer_id, epoch)`, last N sequences | Bound: `max_producers × N × sizeof(entry)`; LRU eviction on inactivity timeout. **The timeout must exceed the client's retry window** — otherwise an evicted producer's retry becomes a duplicate |
| Buffer pool | per-core free list of fixed-size page-aligned buffers | Zero allocation on append (ER-4) |
| Raft log index | in-memory index over the on-disk log | Not a separate structure — this is the point of §12 |

### 16.2 On-disk layout

```
data/<topic>-<partition>/
  00000000000000000000.log      # segment (~128 MB), named by base offset, 20 digits
  00000000000000000000.index    # sparse index, rebuildable
  00000000000000524288.log
  00000000000000524288.index
  raft.state                    # currentTerm, votedFor, CRC — fsynced, tiny
```

Batch header, little-endian, written once per Raft entry (**a batch is an entry** — this is what amortizes replication cost):

```
u64  base_offset           // assigned by the log; NOT covered by the CRC
u32  batch_length          // bytes following this field; NOT covered by the CRC
u32  crc32c                // covers everything AFTER this field
u32  leader_epoch          // Raft term; drives the epoch cache
u8   magic                 // format version
u8   attributes            // bit 0: compressed, bit 1: control batch
u32  last_offset_delta
i64  timestamp_ms
u64  producer_id
u16  producer_epoch
u32  base_sequence
u32  record_count
[records...]
```

The CRC deliberately sits *after* the two fields it does not cover, so `base_offset` and `batch_length` can be assigned or rewritten without recomputing it. Kafka makes the same choice for the same reason.

**Corrected in week 2.** The first draft of this table listed `leader_epoch`, `magic`, and `attributes` *ahead* of the CRC as well, leaving them unprotected for no reason — nothing ever rewrites them. `attributes` carries the control-batch bit (§12.2), so one flipped bit would have turned an acknowledged data batch into an internal record that the fetch path filters out, with the checksum still verifying: a silent loss of an acked write (I1) that no test below the format level could see. Only fields that must stay rewritable belong in front of the CRC. See `docs/retrospective.md` §2.

**Recovery:** open the last segment → seek to the last index entry → scan forward validating CRC per batch → truncate at the first failure or short read → rebuild the index tail.

### 16.3 Wire protocol

```
u32 length | u16 api_key | u16 api_version | u32 correlation_id | payload
```

- Little-endian; `length` excludes itself. **Enforce a max frame size before allocating** — the first thing the fuzzer will attack.
- `correlation_id` enables pipelining: multiple in-flight requests per connection, responses may return out of order.
- Version every API from day one, even at v0. Costs nothing; demonstrates compatibility thinking.

| Key | API | Direction |
|---|---|---|
| 0 | Produce | client → broker |
| 1 | Fetch | client → broker |
| 2 | Metadata | client → broker |
| 3 | ListOffsets | client → broker |
| 4 | OffsetCommit / OffsetFetch | client → broker |
| 100 | RequestVote | broker ↔ broker |
| 101 | AppendEntries | broker ↔ broker |
| 102 | InstallSnapshot | broker ↔ broker |

**Error model:** a `u16` error code **per partition inside the response**, not per connection — one bad partition must not fail a batched request.

| Class | Codes | Client behavior |
|---|---|---|
| Retryable | `NOT_LEADER` (with leader hint), `REQUEST_TIMED_OUT`, `NOT_ENOUGH_REPLICAS` | Retry with jittered backoff |
| Terminal | `OFFSET_OUT_OF_RANGE`, `CORRUPT_RECORD`, `INVALID_PRODUCER_EPOCH`, `MESSAGE_TOO_LARGE` | Surface to caller |

## 17. Failure-mode matrix

| Failure | Designed response |
|---|---|
| Leader crash post-ack, pre-replication | Depends on `acks` — FR-2 |
| Torn write at segment tail | CRC mismatch → truncate to last valid batch (info-level, not an error) |
| Follower far behind | Log shipping; segment ship if the prefix was reclaimed (§12.4) |
| Log divergence after leader change | Epoch cache lookup → truncate to end of last common epoch (§12.3) |
| Network partition | Minority cannot commit; client retries; idempotency prevents duplicates |
| Asymmetric partition | Must not livelock into repeated elections — dedicated simulator test |
| Silent disk corruption | CRC on read → refuse to serve, re-replicate from peer |
| Unflushed writes lost on power loss | Modeled explicitly (§13) — where most student Raft gets it wrong |
| Clock skew / jumps | Correctness never depends on wall clock. Monotonic time for timers; wall clock only for record timestamps, which are metadata, not ordering |

**Amended in week 4 — the partition row was too narrow.** The livelock above was specced as
an *asymmetric* partition problem. It is not: a plain symmetric cut of a single link
between two of three nodes produces it just as reliably, because the third node can still
reach both and will happily grant a vote to whichever peer has just lost contact with the
leader. Leadership then alternates every election timeout for as long as the cut lasts —
28 elections in 40 simulated seconds, with every invariant in §6 holding throughout
(`docs/retrospective.md` §1 entry #2). The designed response is the Raft dissertation's
§4.2.3 rule: a follower that has heard from its leader inside a full election timeout
drops a `RequestVote` without answering and without adopting its term. **Pre-Vote is not
yet implemented**, so a node behind a cut still inflates its own term and disrupts the
leader once when the partition heals; that is week 5.

The wider lesson belongs in §6 and is recorded here because it changes what the invariant
set is for: **I1–I6 are all safety properties, and a cluster that does nothing satisfies
every one of them.** No check in this document can distinguish a working system from a
stopped one. Week 5 adds a conditional liveness invariant — over a window in which some
majority stayed continuously connected, a leader existed for most of it.

## 18. Testing strategy

| Layer | What | Where it runs |
|---|---|---|
| Unit | CRC, framing codec, index binary search, ring buffer, epoch cache lookup | Every commit |
| Property | Encode/decode round-trip; index lookup vs. linear scan; segment recovery vs. known-good log | Every commit |
| Fuzz | Wire decoder (libFuzzer, corpus seeded from real traffic); segment recovery on corrupted files | Nightly + corpus in repo |
| Simulation | Full cluster + faults + invariant checker | 50 fixed seeds every commit; random sweep nightly |
| Determinism | Same seed twice → identical trace hash (I7) | Every commit — **required check** |
| Sanitizers | ASAN, UBSAN (all); TSAN (real runtime only); MSAN (nightly) | Matrix build |
| Real cluster | 3 nodes, `tc netem`, `kill -9` loops, disk fill | Week 8, scripted |

**CI shape:** a **fast job** (unit + property + 50 fixed seeds, < 5 min) on every push, plus a **nightly soak** running random seeds for an hour that opens an issue with the failing seed. The nightly is where the interesting bugs come from, because it explores seeds you'd never pick.

## 19. Benchmark plan

Real 3-node cluster — cloud VMs or containers with `tc netem`. **Methodology before numbers, always.**

**Required before this goes on a resume:**

1. Sustained throughput (MB/s + records/s), 1 KB records, quorum acks, 3 nodes
2. p50/p99/p99.9 append latency at stated offered load, **open-loop** — the README says "coordinated omission" and explains why closed-loop numbers would have been dishonest
3. Leader failover time p50/p99 across ≥50 induced failures
4. Simulator totals: seeds run, simulated node-hours, distinct bugs found
5. One before/after optimization with a percentage and a `perf` flamegraph behind it

**Strongly recommended:**

6. Durability trade-off curve: throughput vs. fsync policy (`acks=1` vs quorum; fsync-per-batch vs every-N-ms)
7. Honest comparison vs. Kafka or Redpanda on identical hardware — "within 0.7× of Kafka" is more credible than beating it, and *explaining the gap* is the real content
8. Consumer fetch throughput, zero-copy vs. copy path
9. Follower recovery time rejoining under load
10. End-to-end produce→consume latency alongside append-ack latency (the §12.1 gap)

**Reporting rules.** State hardware, kernel, filesystem, and mount options. Publish the exact command. Report offered load next to every latency number — a p99 without a load figure is meaningless. Publish bad numbers with the explanation.

## 20. README outline (written incrementally from week 1)

1. **What and why**, one paragraph + an asciinema/GIF of a leader kill with the throughput graph recovering
2. **Architecture diagram** and the request path, produce and fetch
3. **Design decisions** — log-as-Raft-log (§12), thread-per-core (§15), durability (§13); for each, the alternative considered and why it lost
4. **Correctness** — the invariant set, the simulator, simulated node-hours, link to the bug journal (`docs/retrospective.md` §1)
5. **Benchmarks** — methodology, then numbers, then the honest comparison
6. **What's not implemented, and why** (§3)
7. **Build and run** — three commands, verified on a clean machine

---

# Part III — Execution

## 21. Milestones

Each week ends with a demo (ER-6).

| Week | Deliverable | Demo |
|---|---|---|
| 1 | CMake skeleton, CI with sanitizers, `io/` interfaces, wire framing, callback event loop | `bench/echo` prints RPCs/sec and p99 over a loopback socket |
| 2 | Single-partition storage: segments, sparse index, CRC, crash recovery, append/fetch | `kill -9` mid-append → restart → `log-dump` shows no acked record lost |
| 3 | Simulator core: virtual clock, deterministic scheduler, sim network + disk, trace hashing | Same seed twice → identical trace hash; 1 simulated hour in < 5 s |
| 4–5 | Raft: elections → replication → persistence, under fault injection; invariant checker | 1000 seeds green in CI; ≥ 3 entries in the bug journal |
| — | **⚠ End of week 5: provision the benchmark cluster (§24).** Nothing before this point needs a cloud account; week 6 needs somewhere to deploy to. | 4 hosts reachable over SSH |
| 6 | Real transport, multi-process cluster, client library (batching, idempotency), consumer offsets, **deploy script** | 3 real processes; produce and consume across a leader kill |
| 7 | **Multi-partition + thread-per-core** (see §15 note), buffer pools, zero-copy fetch; profile and optimize | Flamegraph before/after; one number that moved |
| 8 | Benchmark suite, open-loop load gen, failover experiments, Prometheus + Grafana, README with diagrams | Full README benchmark table, reproducible from one script |
| 9 | Buffer: chaos on the real cluster, follower reads, "what I'd do differently" written down while fresh | — |

## 22. Risk register

| Risk | Likelihood | Mitigation |
|---|---|---|
| C++20 coroutines eat two weeks in week 1 | High if new to them | Callback/state-machine event loop (§15.1). Coroutines are ergonomics, not capability |
| Simulator slips, so Raft debugging happens the slow way | Medium | **Week 3 is load-bearing for weeks 4–5.** If week 3 runs long, cut from week 6, never from week 3 |
| Raft in two weeks | Medium | Realistic *only* with the simulator working. Order: election → replication → persistence → (skip membership). Optimize nothing until all three are green |
| Real-cluster benchmarks are pure infra work | High | Budget **2 days** in week 8 for VMs, deploy scripts, `tc netem` — not 2 hours. Write the deploy script in week 6 at first multi-process |
| Cloud account not ready when week 6 needs it | Medium | §24 — apply for the student pack at end of week 5; approval takes days, and the deploy script has nothing to target without it |
| Scope creep | Medium | The cut list below is a commitment, not a suggestion |
| Thread-per-core claim goes untested | Medium | Promote multi-partition to week 7 (§15 note) |

## 23. Cut list, in order

follower reads → compression → io_uring (ship epoll + `pwritev`) → coroutine retrofit → metadata controller → multi-partition *(last resort — see §15)*

**Never cut:** the simulator, the benchmarks, the README, the bug journal.

> A finished 3-node single-partition system with rigorous testing and honest numbers beats an unfinished 8-node system every time.

## 24. External dependencies — what to sign up for, and when

Weeks 1–5 need **no accounts beyond GitHub**. The simulator, storage engine, and all of Raft run on a laptop. Don't provision anything early; an idle VM bills for nothing.

| When | What | Cost | Why then |
|---|---|---|---|
| Week 0 | **GitHub** — repo + Actions | Free (public repo) | The repo is the deliverable; CI runs from day one |
| **End of week 5** ⚠ | **Cloud VMs** — 3 brokers + 1 load generator | ~$0 with student credit, else ~$5–25 | Week 6 writes the deploy script and needs a target. Also: the [GitHub Student Developer Pack](https://education.github.com/pack) (Cornell email → **$200 DigitalOcean credit**) takes a few days to approve, so applying in week 8 makes it a blocker |
| Never | Grafana Cloud | — | Self-host Grafana + Prometheus via docker-compose; dashboards live in the repo as JSON, which is more reproducible |
| Never | asciinema.org, Docker Hub, OSS-Fuzz | — | `asciinema rec` + `agg` work offline; GHCR ships with GitHub; OSS-Fuzz only onboards widely-depended-on projects — run libFuzzer in your own CI |

**The one hardware constraint that matters:** dedicated vCPU, not shared/burstable. Steal time on a shared vCPU lands directly in p99.9 and makes every latency number in the README unfalsifiable. Same region for all four hosts; record region, instance type, kernel, filesystem, and mount options in the README (§19).

Until the cluster exists, `BENCH_LOCAL=true` runs the harness against 3 local containers. That validates the harness only — shared kernel, page cache, and disk queue. **Never let a local number reach the README.**

Placeholders to fill in at that point live in `.env.example`: `BENCH_NODE_HOSTS`, `BENCH_CLIENT_HOST`, `BENCH_LOCAL=false`, and one provider token.

## 25. Open questions

1. **Segment size** — 128 MB is the Kafka default; verify it's sane for local NVMe benchmark runs, or drop to 32 MB so retention/rolling gets exercised within a benchmark window.
2. **Producer dedup window `N`** — pick from the client's max in-flight × retry window, then document the arithmetic (§16.1).
3. **Group fsync policy defaults** — N ms vs N bytes; decide from the week-8 durability curve (benchmark #6), not from intuition.
4. **Partition→core assignment** — round-robin in v1; whether to expose a rebalancing hook is a week-9 question.
5. **Comparison baseline** — Kafka (more recognizable) vs Redpanda (closer architecturally, thread-per-core C++). Probably Kafka for reviewer legibility.
