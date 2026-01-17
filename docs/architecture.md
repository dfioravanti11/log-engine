# Architecture

> **Living document.** Update whenever a component is added, a boundary moves, or a
> dependency rule changes. Describes the system *as it currently exists* — mark
> unbuilt pieces `[planned]` rather than describing them as real.
> Rationale and alternatives-considered live in `../project_spec.md`; this file is
> structure only.

**Last updated:** 2026-08-14 · **Build state:** week 3 — `base/`, `io/` (real **and** sim), `runtime/`, `wire/`, `storage/`, `sim/` built; `raft/`, `server/`, `client/` `[planned]`

---

## 1. What the system is

Three broker processes hold a replicated append-only log. Clients append batched
records to a partition and fetch from any offset. Each partition is one Raft group.
The user's log **is** the Raft log — there is no separate state machine (`project_spec.md` §12).

```
   producer ──┐                        ┌── Broker 0 (leader) ──┐
              ├── binary proto / TCP ──┤                       │  Raft group
   consumer ──┘                        ├── Broker 1 ───────────┤  per partition
                                       └── Broker 2 ───────────┘
                                                  │
                              segment files + sparse index + raft.state
```

## 2. Layer stack

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
│ io/        Clock │ Network │ Disk │ Random    ← THE SEAM  │
│            real/            sim/                          │
└──────────────────────────────────────────────────────────┘
```

**Everything above the seam is portable** and runs byte-identically in production and
in the simulator. That is the entire architectural bet. Below the seam there are two
implementations of each interface and nothing else.

## 3. Components

| Module | Owns | Key types | State |
|---|---|---|---|
| `base/` | Primitives with no dependencies | `Slice`/`MutSlice`, `Buffer`, `Result<T>`, `ErrorCode`, endian codecs, crc32c | **built** |
| `io/` | The nondeterminism seam | `Clock`, `Network`, `Disk`, `Random`, `SeededRandom` + `real/` **and** `sim/` | **built** |
| `runtime/` | Execution machinery | `EventLoop` + timer heap; MPSC queue and buffer pool week 7 | **built** (loop + timers) |
| `wire/` | Bytes on the network | `FrameDecoder`/`encode_frame`, `ApiKey`, versioning | **built** (framing) |
| `storage/` | Bytes on disk | `BatchBuilder`, `SparseIndex`, `Segment`, `Log`, recovery | **built**; retention + `EpochCache` `[planned]` |
| `raft/` | Consensus | election, replication, persistent metadata, log matching | `[planned]` |
| `server/` | Wiring | `Broker`, partition→core map, request dispatch | `[planned]` |
| `client/` | Client-side protocol | `Producer` (batching, dedup sequences), `Consumer` (offsets, long poll) | `[planned]` |
| `sim/` | Adversary + oracle | `Scheduler` (virtual time), `Trace` (+ hash), `Simulation`, `Oracle`, `NodeWorkload` | **built**; I3–I6 arrive with Raft |

## 4. Dependency rules

Dependencies point **strictly downward**. A cycle is a design error, not a build error
to work around.

| Module | May depend on | Must never depend on |
|---|---|---|
| `base/` | — | anything |
| `io/` | `base/` | everything else |
| `runtime/` | `base/`, `io/` | `storage/`, `raft/`, `server/`, `client/` |
| `wire/` | `base/` | `io/`, `storage/`, `raft/` |
| `storage/` | `base/`, `wire/`, `io/` *(interfaces only)* | `raft/`, `server/`, real I/O |
| `raft/` | `base/`, `wire/`, `storage/`, `io/` *(interfaces only)* | `server/`, real I/O |
| `server/` | all of the above | `sim/` |
| `client/` | `base/`, `wire/`, `io/` | `storage/`, `raft/`, `server/` |
| `sim/` | `base/`, `io/`, `storage/`, `runtime/` + read access to everything for the checker | — |

`io/sim/` and `sim/` are **one link target** (`logengine_sim`). They are one component:
a simulated `Disk` with a private notion of time would not be a simulation of anything,
so they share one clock, one event queue, and one seed. Splitting them would buy nothing
but a circular link edge.

**The one rule (ER-1):** `storage/`, `raft/`, `server/`, `client/` must not touch the OS.
No `<chrono>` clocks, sockets, file I/O, `rand()`, or `std::thread`. All of it arrives
through an `io/` interface injected at construction. CI greps for violations.

## 5. Request paths

### 5.1 Produce

```
socket
  → wire::decode        frame length checked BEFORE allocating
  → dispatch            hash(partition) → owning core; cross-core via MPSC queue
  → raft::append        assign offset + leader_epoch, append to local log
  → storage::append     write to page cache, update sparse index in memory
  → raft::replicate     AppendEntries fan-out to followers
  ⇢ [wait]              quorum acks, then the next group fsync boundary
  → commit index advances → high watermark advances
  → wire::encode        base offset, or per-partition u16 error code
```

The wait is where `acks` is honored: `1` responds at local append, `quorum` at majority
append, `quorum+fsync` after the group fsync. The response is delayed — the write never
is (`project_spec.md` §13.1).

### 5.2 Fetch

```
socket
  → wire::decode
  → dispatch            to the core owning the partition
  → clamp               offset must be < commit index (invariant I5) — this is the
                        high watermark, and it is why a truncated entry can never
                        have been read
  → SparseIndex         binary search → nearest file position ≤ target
  → Segment             scan forward to the exact batch
  → CRC                 every batch served is validated on the way out (§17). Recovery
                        only proves a segment was intact at open; a bit that flips
                        afterwards is visible on no other path
  → filter              control batches dropped; offsets still advance (I2)
  → respond             zero-copy from page cache where possible
  ⇢ or park             long poll up to max_wait_ms if nothing is available
```

## 6. Concurrency

Thread-per-core, shared-nothing. One pinned event loop per core; each partition owned by
exactly one core, so partition state is touched by exactly one thread and the hot path
takes no locks. Cross-core work routes through a cache-line-aligned lock-free MPSC queue
with explicit backpressure.

Consequence to respect: **blocking anywhere in an event loop stalls every partition on
that core.** Disk work goes through `io::Disk` and completes asynchronously; nothing in
a handler may block.

Trade-off against thread-pool + per-partition-mutex: `project_spec.md` §15. Note the
scoping caveat there — thread-per-core is only *exercised* once multiple partitions exist.

## 7. On-disk layout

```
data/<topic>-<partition>/
  00000000000000000000.log      # segment, named by base offset, 20 digits
  00000000000000000000.index    # sparse index — rebuildable, never fsynced
  00000000000000524288.log
  00000000000000524288.index
  raft.state                    # currentTerm, votedFor, CRC — fsynced, tiny  [planned]
```

Batch header layout and the CRC-placement rationale: `project_spec.md` §16.2.
A batch is exactly one Raft entry — that identity is what amortizes replication cost.
Segments default to 32 MB. Names are 20 zero-padded digits so lexicographic order is
numeric order, which is why sorting `list_directory()` is enough to recover in write order.

**Ownership.** `Log` is the sole offset authority: it assigns every `base_offset`, and a
`Segment` refuses any batch that does not continue exactly where its last one ended.
Nothing else in the system may choose an offset, which is what makes I2 (monotonicity) a
property of one counter instead of a rule everyone has to remember.

### 7.1 Recovery

Runs inside `Segment::open()`, on every open, clean or not:

```
read .index          never fsynced → may be absent, stale, or half an entry.
                     decode() drops the tail at the first entry that isn't
                     strictly increasing or that points past the log's end.
verify the last entry  does a batch header actually start there? if not, drop it
                     and try the previous. usually none survive after a crash,
                     and the scan starts at byte 0 — that is the normal case.
scan forward         per batch: decode header → bounds-check the declared length
                     → validate CRC → check base_offset continues the log.
truncate             at the first failure. a torn tail is info, not an error (FR-7).
```

Recovery cost is therefore bounded by **segment size**, not log size. That makes segment
size a recovery-latency knob, which is the real reason for 32 MB over Kafka's 128 MB.

`Log::open()` runs the same scan over every segment in order and discards any segment
stranded behind a truncation — a hole in an append-only log makes every offset after it
unreachable by a sequential read, so keeping those segments would be worse than losing
them. `tools/log-dump` deliberately does **not** share this path: it walks the bytes
read-only, so it shows the damage instead of repairing it.

## 8. The simulator's relationship to everything else

`sim/` does not sit in the stack; it *replaces the bottom of it* and observes the rest.

```
   production                          simulation
   ──────────                          ──────────
   1 process per node                  N nodes, 1 thread, 1 process
   io/real/  epoll, pwritev,           io/sim/   in-memory wires + files,
             CLOCK_MONOTONIC,                    virtual clock, seeded PRNG
             /dev/urandom
   OS scheduler                        sim::Scheduler (seeded, virtual time)
   metrics → Prometheus                event trace → hash → CI canary
   —                                   Oracle checks invariants after every event
```

**What actually runs.** Each simulated node owns a real `runtime::EventLoop` over a
`SimClock` and a `SimNetwork`, and a real `storage::Log` over a `SimDisk`. Nothing above
the seam is modified or mocked — that is the architectural bet being cashed. The one
change week 1's event loop needed was `next_timer_deadline()`: under a real clock a loop
decides for itself how long to block, but under virtual time it cannot, because the next
thing to happen may belong to another node. So the simulator asks every loop when it next
wants to wake, moves the clock to the earliest, and drains the nodes in a rotating order.

**The main loop.** Advance to the earliest deadline across the scheduler's queue and every
node's timers → run every event due at that instant → drain each node to quiescence.
Time never moves backwards and never moves anywhere nothing happens, which is why an hour
of cluster life costs ~2 s of wall clock.

**The Oracle** is the shadow model, and it lives in the simulator rather than in a node —
a node checking its own durability against its own memory would agree with itself after
losing both. It currently validates I1 (acked writes survive) and I2 (offsets are
monotonic); I3–I6 need a leader and arrive with Raft. Same seed must produce a
byte-identical trace; that test is the canary for accidental nondeterminism (§14.3) and
is a required CI check.

**Faults injected** (§14.1): crash/restart with unflushed-write loss and torn writes,
per-operation disk I/O errors, silent bit-flips, symmetric *and* asymmetric partitions,
connection resets, per-link latency, short writes, send-window backpressure, and wall
clock jumps. Byte-level drop/reorder/duplicate are deliberately **not** modeled — over a
stream they are fiction, and `docs/retrospective.md` §2 explains the reasoning.

## 9. Open structural questions

Tracked here until resolved, then folded into the sections above.

1. Where partition→core assignment lives — `server/` today; may need a `runtime/` hook if
   rebalancing is ever added.
2. Whether `client/` needs its own connection-pool abstraction or reuses `runtime/`.
3. Whether `EpochCache` belongs in `storage/` (it is rebuilt by scanning batch headers)
   or `raft/` (it is consumed entirely by log matching). Currently `storage/`.
