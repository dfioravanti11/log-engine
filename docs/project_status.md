# Project Status

> **Living document.** Update at the end of every work session — at minimum, the
> *Right now* block. This file answers three questions and nothing else: where are we,
> what's done, what's next. Narrative goes in `changelog.md`; rationale in `project_spec.md`.

**Last updated:** 2026-08-16 · **Week 6 of 9** · **Target:** ship before new-grad recruiting

---

## Right now

| | |
|---|---|
| **Phase** | **Week 8 complete.** `bench/run_all.sh` produces every number from one command. NFR-3 passes; the benchmark found that §13.1's group commit was specced and never built |
| **Current focus** | Running the suite on GCP for README numbers, then implementing group commit as §19 #5's before/after |
| **Last demo** | ☑ `bench/run_all.sh` — simulator totals, failover p50 178 / p99 489 ms, a saturation sweep, and the `acks` trade-off |
| **This week's demo** | ☑ one command, every number, each with its conditions attached |
| **Blockers** | None |
| **Next decision due** | None open. Benchmark VMs are **no longer a risk** — GCP is available, so week 6 has somewhere to deploy without waiting on an approval |

### Decisions on the table

**Week 4 — Raft is a `tick()`-driven pure state machine** (etcd-style), not a self-timed
one. It holds no `io::` interface except `Random`. Three consequences, all of which paid
immediately: a three-node election is three objects and a message queue, so the 19 election
tests run in 1 ms with no infrastructure; §13's persist-before-you-respond rule became
structural, because the state machine *cannot* send; and timeouts counted in ticks mean
the clock-jump fault has no route into Raft at all. See `retrospective.md` §2.

**Week 5 — the node owns decisions, the driver owns bytes.** §12 says the user's log *is*
the Raft log, which rules out `raft::Node` holding its own copy of the entries — that is
the double write the decision exists to avoid. So the node holds only the log's end and an
epoch map (§12.3), names an index for the driver to read out of storage, and says whether
an arriving entry is acceptable; the driver, which is already holding the bytes, writes
them. One entry per AppendEntries, because §16.2 already says a batch *is* an entry.

## Milestones

Legend: ☐ not started · ◐ in progress · ☑ demo ran · ✗ cut

| Wk | Deliverable | Demo (the definition of done) | State |
|---|---|---|---|
| 1 | CMake skeleton, CI with sanitizers, `io/` interfaces, wire framing, callback event loop | `bench/echo` prints RPCs/sec and p99 over loopback | ☑ |
| 2 | Single-partition storage: segments, sparse index, CRC, crash recovery, append/fetch | `kill -9` mid-append → restart → `log-dump` shows no acked record lost | ☑ |
| 3 | Simulator core: virtual clock, deterministic scheduler, sim network + disk, trace hashing | Same seed twice → identical trace hash; 1 simulated hour in < 5 s | ☑ |
| 4–5 | Raft: elections → replication → persistence, under fault injection; invariant checker | 1000 seeds green in CI; ≥ 3 entries in the bug journal | ☑ |
| — | Provision benchmark VMs (`project_spec.md` §24) | 4 hosts reachable over SSH | ☐ GCP available, no approval to wait on |
| 6 | Real transport, multi-process cluster, client library, consumer offsets, deploy script | 3 real processes; produce and consume across a leader kill | ◐ 3 real processes and a survivable leader kill ☑; client library + consumer offsets ☐ |
| 7 | Multi-partition + thread-per-core, buffer pools, zero-copy fetch; profile and optimize | Flamegraph before/after; one number that moved | ◐ the "before" is measured — group commit is the named target |
| 8 | Benchmark suite, open-loop load gen, failover experiments, Prometheus + Grafana, README | Full README benchmark table, reproducible from one script | ◐ harness done and reproducible; throughput numbers await real hardware. Prometheus ✗ cut |
| 9 | Buffer: chaos on the real cluster, follower reads, "what I'd do differently" written down | — | ☐ |

**Week 3 was load-bearing and it landed, and week 4 collected on it.** Both of week 4's
findings arrived with a seed attached and a one-command replay — including the one that
broke no invariant at all.

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

### Week 4 — Raft leader election · 2026-08-15
- Decided Raft's shape: **`tick()`-driven pure state machine**, holding no `io::`
  interface except `Random`. No clock, no disk, no socket — it asks, the driver acts
- `raft/node`: elections with randomized timeouts, one vote per term, the §5.4.1
  up-to-date check, step-down on a higher term, heartbeats
- `raft/state_file`: `raft.state` as two alternating CRC'd slots with a sequence number,
  so a torn write cannot take both. Both halves gone keeps the node **down** rather than
  restarting it as a term-0 voter
- The driver's `drive()` is the one function that sends a Raft message, and it persists
  first — §13 made structural rather than remembered. Review then found the composition
  bug that survives that discipline: the persist debt is now sticky until the fsync
  returns (`retrospective.md` §5)
- **I6 in the oracle**, plus `OneKnobDecidesWhetherATermCanElectTwoLeaders`: the same
  seed is clean with the fsync and elects two leaders in term 18 without it
- **Bug journal entry #2** — one cut link made leadership ping-pong every 200 ms for the
  whole partition (28 elections in 40 s), with **every invariant holding throughout**.
  Fixed with the dissertation's §4.2.3 lease rule: 28 → 2
- 20 test binaries (17 → 20); 19 pure election tests run in 1 ms, no infrastructure
- Demo ran: 1000 seeds green, 50 simulated node-hours in 9 s wall clock, I7 still holds

### Week 5 — Raft replication · 2026-08-15
- **The node owns decisions, the driver owns bytes.** §12 rules out `raft::Node` holding
  its own entries, so it holds the log's end and an epoch map (§12.3) and names indices;
  the driver reads and writes the bytes. One entry per AppendEntries, because §16.2
  already says a batch *is* an entry
- `storage/`: `append_replicated` (offset chosen upstream, bytes verbatim, CRC checked
  before writing), `truncate_to` at a named batch boundary, `scan_epochs` to rebuild the
  leader epoch cache
- §5.3 log matching with truncate-on-conflict, `match`/`next` per follower, and the
  **§5.4.2 rule** — a majority is not enough to commit an entry from an earlier term
- **Bug journal entry #3** — a follower reported its log *length* instead of the extent it
  had agreed to, so a leader counted entries it had never seen toward a quorum and
  committed records that existed nowhere. Found by a new I1 check on its first simulated
  hour, seed 11
- Two of my own found by the numbers looking wrong: Raft indices are record offsets, not
  entry numbers (89 elections in 30 s with no faults), and an unsized scratch buffer that
  silently stopped the leader sending anything at all
- **I8 — liveness**, closing journal #2's debt. Longest leaderless: 0.182 s healthy,
  2.156 s under crashes
- **Acceptance criterion 4** — seed 2, thirty crashes: `acks=quorum+fsync` keeps all 1,882
  records, `acks=1` loses 18
- 21 test binaries; green under dev/asan/ubsan, and tsan on the non-simulation set.
  Full four-preset CI: **82 s**, down from ~40 min once replication made simulated time
  expensive (`retrospective.md` §5 — and the fault test caught the fix going vacuous)
- Demo ran: `scripts/demo_week5.sh`

### Week 6 — the architectural bet, settled · 2026-08-16
- **`server::Broker`** — the driver moved out of `sim/` unchanged. `sim::NodeWorkload` now
  *owns* one rather than being one, and watches through `server::BrokerObserver`, whose
  every hook is an observation and never a decision
- **The proof it was faithful**: the 1000-seed sweep produced byte-identical totals before
  and after — 2,320,262 records, 7,205 crashes — with all 21 test binaries green
- `src/main/logengine.cpp` — the product. One broker process over `io/real/`, with an
  optional built-in producer
- Demo ran: three real processes, real TCP, real disks; `kill -9` on the leader; a new
  leader in the next term; commit index 548 → 1,116 with no regression; both survivors'
  segments read back by `log-dump`
- The I2 check was sampling the commit index after proposing, which under `acks=1` is
  after the append commits itself. Caught by the checker on the first post-extraction run

### Week 8 — the benchmark suite · 2026-08-16
- `bench/run_all.sh` — every number from one command, each with hardware, filesystem,
  mount options, record size, batch size, `acks` level and offered load attached
- **NFR-3 passes**: failover p50 178 ms · p99 489 ms · p99.9 657 ms over 200 induced
  leader failures, against a 900 ms bound
- **Open-loop** load generation, so a stall stays in the histogram instead of vanishing
  exactly when it matters. Coordinated omission avoided by construction, not disclaimer
- A saturation *sweep* rather than a single rate, because NFR-2 asks for a p99 at 70% of
  saturation and saturation has to be measured before it can be divided
- **Found: §13.1's group commit was specified and never implemented.** One fsync per
  append pins throughput to the device flush rate; p50 of 8.5 ms is one `F_FULLFSYNC`.
  Invisible to every correctness test, because it is a throughput decision
- Found: past saturation the cluster burns Raft terms — the event loop starves its own
  tick timer while fsyncing
- Fixed a measurement bug that made NFR-3 fail: the first version counted leaderless
  stretches where two of three nodes were down, which is `restart_delay_max` in disguise

### Week 4 — done

1. ☑ Raft's shape decided and built: `tick()`-driven pure state machine
2. ☑ Leader election, green over 1000 seeds under crashes, partitions and clock jumps
3. ☑ `currentTerm`/`votedFor` persisted and fsynced before any response that changes them
   (§13) — two alternating CRC'd slots, so a torn write cannot lose both
4. ☑ I6 in the oracle, plus a test that deliberately breaks the fsync to watch it go red
5. ☑ Bug journal entry #2

### Week 5 — done
1. ☑ Replication inside `raft::Node` — epoch-cache log metadata, `match`/`next`, §5.3 log
   matching with truncate-on-conflict, §5.4.2 commit rule. 18 pure tests
2. ☑ Driver wired to `storage::Log` — `append_replicated`, `truncate_to`, `scan_epochs`;
   entries on the wire; acks from the commit index. Found journal #3 on its first hour
3. ☑ **I8, liveness** — the debt journal #2 left. In `project_spec.md` §6
4. ☑ **The `acks` trade-off** — acceptance criterion 4, on one seed

## Up next — the endgame

Weeks 0–6 and 8 are done. What remains is one measurement run, one optimization, and the
write-up.

1. **Run `bench/run_all.sh` on GCP.** The harness is finished and reproducible; the
   throughput and latency numbers stay out of the README until they come off hardware that
   is not three brokers sharing one laptop. This is the only thing standing between here
   and criterion 7
2. **Implement §13.1's group commit**, and measure it. This is now the obvious §19 #5
   ("one before/after optimization with a percentage") — the "before" is taken, the design
   is already specified, and the ceiling it lifts is the one every other optimization is
   hiding behind
3. **Two more bug-journal entries** for criterion 6 (3 of 5). Turning on
   `corruption_interval` is the cheapest honest source now that there is a peer to
   re-replicate from
4. **A client library**, to make criterion 1 literal — producing and consuming from outside
   a broker process rather than from the built-in generator

**Cut, and recorded as cut:** Prometheus + Grafana (§FR-11). Metrics are a product feature,
not a distributed-systems problem, and the benchmark harness already produces the numbers
that matter with better provenance than a dashboard would.

**Week 7 (multi-partition, thread-per-core) stays optional.** Its best argument is now the
overload finding above — taking fsync off the loop that owns consensus timing — but that is
the same work as group commit, from the other end.

**Deferred, deliberately, with the numbers written down:**

- **Pre-Vote.** A node behind a cut link still inflates its term and disrupts the leader
  once on heal — 81 terms against 1 election, measured (`retrospective.md` §2)
- **Check-quorum.** An isolated leader keeps believing it is one. Harmless while it cannot
  commit; it matters the moment a leader may serve a read from its own state
- **I3–I5 as explicit oracle checks.** I4 is covered from another angle by the
  leader-completeness check that found journal #3; I5 is structural, since fetch is clamped
  to the commit index; I3 follows from the log-matching property the pure tests cover

## Acceptance criteria — the finish line

Tracked from day one so none of it is a surprise in week 9 (`project_spec.md` §8).

| # | Criterion | State |
|---|---|---|
| 1 | Three README commands produce a running 3-node cluster on a clean machine | ◐ README written to the §20 outline and its three commands work on a clean checkout — but they build, test, and sweep seeds. A *cluster* needs week 6 |
| 2 | `./sim --seeds 1000` green, prints simulated node-hours | ◐ 1000 seeds green **with Raft elections** and 50 simulated node-hours in 9 s — reopens once replication lands |
| 3 | Same seed twice → identical trace hash, CI-enforced | ☑ |
| 4 | Under one seed: `acks=1` loses data, `acks=quorum+fsync` does not — side by side in the README | ☑ seed 2, 30 crashes: 1,882 records kept vs 18 lost. In the README and in `demo_week5.sh` |
| 5 | `kill -9` the leader under load → GIF of throughput dipping and recovering, no acked record lost | ◐ the substance is done and scripted (`demo_week6.sh`); the GIF is week 8 |
| 6 | Bug journal (`retrospective.md` §1) has ≥ 5 entries, each with seed / invariant / cause / fix commit | ◐ 3 of 5 |
| 7 | README benchmark table regenerable from `bench/run_all.sh` with full environment stated | ◐ regenerable from one command, environment stated; awaiting a run on GCP |

## Risk watch

| Risk | State | Trigger to act |
|---|---|---|
| Coroutines eat week 1 | Mitigated by design — callback loop only | If any coroutine appears before week 7, stop |
| Week 3 slips → Raft debugged the slow way | **Closed** — simulator landed on time | — |
| Raft in two weeks | **Half closed** — elections green over 1000 seeds at the end of week 4, on schedule | If replication isn't green by end of week 5, drop everything optional |
| Safety-only invariant set hides an unavailable cluster | **Open, and it already bit once** (journal #2) | Week 5 ships a conditional liveness check, or every green sweep after this one is worth less |
| Cloud account not ready for week 6 | **Closed** — GCP account already available, so no student-pack approval to wait on | — |
| Thread-per-core claim untested | Mitigated — multi-partition moved to week 7 | If week 7 slips, this is the claim that weakens |
| Real-cluster benchmarks are pure infra | Open | Budget 2 days in week 8, not 2 hours |

## Cut list, in order

follower reads → compression → io_uring → coroutine retrofit → metadata controller →
multi-partition *(last resort)*

**Never cut:** the simulator, the benchmarks, the README, the bug journal.
