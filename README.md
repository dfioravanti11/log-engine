# log-engine

A replicated append-only log — Kafka's durable core, in C++20 — validated by a
deterministic fault simulator that can replay any failure from a seed.

> **Status: week 6 of 9, in progress.** Transport, storage, the simulator, Raft
> (elections and replication), and a **real three-process cluster** are built and tested.
> What is not built: a client library, and the benchmark suite. Every section below marks what
> is real and what is `[planned]`, because a README that describes the design as though it
> were the implementation is the main way this kind of project misleads.

---

## 1. What and why

Most from-scratch Raft implementations can tell you the algorithm is right. Very few can
tell you *their code* is right, because the interesting failures — a torn write, a node
that forgets a vote, a partition that heals at exactly the wrong moment — are
non-deterministic, rare, and gone by the time you look.

So the deliverable here is not the log. It is the machine that makes the log's failures
reproducible: every clock, socket, disk, and random number goes through an interface with
a real implementation and a simulated one. Under simulation the whole cluster runs on one
thread, on virtual time, driven by a single seed. **One simulated cluster-hour takes about
two wall-clock seconds**, and any failure it finds replays exactly:

```
./build/dev/tools/sim --seed 3 --dump-trace /tmp/trace.txt
```

And it is not only a simulation. `scripts/demo_week6.sh` brings up three real processes
over real TCP on real disks, `kill -9`s the leader, and checks that the commit index never
moves backwards across the failover:

```
leader elected: node 0 (term 1)
committed before the kill: 548 records
SIGKILL sent to node 0 — no shutdown, no flush, no mercy
new leader: node 2 (term 2)
committed now:             1116
```

The broker in that cluster is the *same object* the simulator has been crashing for three
weeks — the only difference is which `io::` implementations were constructed at startup.
`[planned — week 8]` the same run as a GIF, with the throughput graph.

## 2. Architecture

```
   producer ──┐                        ┌── Broker 0 (leader) ──┐
              ├── binary proto / TCP ──┤                       │  Raft group
   consumer ──┘                        ├── Broker 1 ───────────┤  per partition
                                       └── Broker 2 ───────────┘
                                                  │
                              segment files + sparse index + raft.state
```

Everything above the `io/` seam is portable and runs byte-identically in production and in
the simulator. That is the entire architectural bet.

```
┌──────────────────────────────────────────────────────────┐
│ client/    producer · consumer                [planned]   │
│ server/    broker ✓ · dispatch [planned]                  │
│ raft/      election ✓ · replication ✓ · log matching ✓     │
│ storage/   segment ✓ · sparse index ✓ · recovery ✓        │
│ wire/      framing ✓ · codecs ✓ · versioning ✓            │
│ runtime/   event loop ✓ · timers ✓                        │
├──────────────────────────────────────────────────────────┤
│ io/        Clock │ Network │ Disk │ Random    ← THE SEAM  │
│            real/ ✓          sim/ ✓                        │
└──────────────────────────────────────────────────────────┘
```

Full structure, dependency rules, and request paths: [`docs/architecture.md`](docs/architecture.md).

## 3. Design decisions

Each of these had a real alternative that lost. The full list, with the reasoning that
produced them, is in [`project_spec.md`](project_spec.md) and
[`docs/retrospective.md` §2](docs/retrospective.md).

**The user's log *is* the Raft log.** The textbook layering — Raft as a command stream
feeding a separate state machine — writes every record twice and needs its own snapshot
mechanism. Replicating the log directly means a snapshot is just a retention boundary.
The cost is four follow-on problems, and having answers to all four is the point: consumer
visibility (fetch is clamped to the commit index), control records occupying real offsets
(so offsets are monotonic but **not dense**, exactly like Kafka), per-batch leader epochs
for divergence detection (KIP-101), and segment shipping in place of snapshot install.

**Two durability knobs, never conflated.** Raft metadata (`currentTerm`, `votedFor`)
fsyncs before *any* response that changes it — always, not tunable. User data follows the
request's `acks`. Conflating them is the classic bug, and the simulator demonstrates it on
demand: same seed, one knob, [§4](#4-correctness).

**`raft::Node` touches nothing.** No clock, no disk, no socket — not even an injected one.
It counts ticks and returns a description of what it wants done; the driver carries it out.
A three-node election is therefore three objects and a message queue, so 19 election tests
run in **1 ms** with no infrastructure at all; the persist-before-you-respond rule becomes
structural rather than remembered, because the state machine cannot send; and a wall-clock
jump has no route into consensus, which is a test rather than a claim.

**Callback event loop, not coroutines.** Ergonomics, not capability, and the single most
likely way to lose a week early. Revisitable behind the same interface if it ever becomes
unreadable.

## 4. Correctness

Eight invariants are checked inside the simulator as it runs:

| # | Invariant | Checked |
|---|---|---|
| I1 | An acked write is never lost | ✓ |
| I2 | Offsets are monotonic in commit order (gaps allowed) | ✓ |
| I3 | No reordering within a partition | ✓ via log matching (§5.3) |
| I4 | A committed entry is never overwritten | ✓ a leader must hold the whole committed prefix |
| I5 | No consumer reads an offset ≥ the commit index | ✓ structurally — fetch is clamped |
| I6 | At most one leader per term | ✓ |
| I7 | The same seed produces a byte-identical event trace | ✓ **required CI check** |
| I8 | The cluster regains a leader within a bounded time of being able to | ✓ |

**I8 is there because the other seven could not see a broken cluster.** Every one of I1–I6
is a *safety* property, and a cluster that does nothing at all satisfies all of them — which
is exactly how the bug below hid behind a thousand green seeds. It is stated conditionally,
because the unconditional version is false: during a partition that costs the majority,
having no leader is correct.

**One knob, one invariant, one seed.** The fsync of `currentTerm`/`votedFor` is the thing
§13 says is never tunable. Here is why, on seed 4:

```
$ ./build/dev/tools/sim --seed 4 --duration-s 120 --crash-s 3 --restart-ms 120
raft                75 elections over 84 terms, 245 state fsyncs     ← green

$ ./build/dev/tools/sim --seed 4 --duration-s 120 --crash-s 3 --restart-ms 120 --unsafe-metadata
invariant:  I6
detail:     term 18 has two leaders: node 0 and node 2
```

A node voted, crashed before the write reached the platter, came back not remembering, and
voted again in the same term. Two candidates, two majorities, one term — with no bug
anywhere in the algorithm.

And the same treatment for user data — the knob a producer actually sets. Seed 2, sixty
simulated seconds, a crash every four:

```
$ ./build/dev/tools/sim --seed 2 --duration-s 60 --crash-s 4
acked               1882 records over 939 batches
faults              30 crashes, 2 partitions, 164 resets     ← every promise kept

$ ./build/dev/tools/sim --seed 2 --duration-s 60 --crash-s 4 --acks-1
invariant:  I1
detail:     node 2: won an election without the committed prefix (1848, 1866)
at:         56.902290 simulated seconds
```

Eighteen records the producer was told were safe, on a leader that died before replicating
them; a new leader was elected that had never seen them, correctly, by every rule in the
paper. **Neither setting is a bug.** They are different promises, and the simulator holds
the system to whichever one it made.

### The bug journal

Every simulator-found bug gets an entry with its seed, the invariant it broke, the cause,
and the fix commit: [`docs/retrospective.md` §1](docs/retrospective.md). **3 entries so
far**, and the second is the one worth reading:

> One cut link between two of three nodes made leadership ping-pong every ~200 ms for the
> entire partition — 28 elections in 40 simulated seconds — while **every invariant held
> throughout**. I1–I6 are all safety properties, and a cluster that does nothing satisfies
> every one of them. A thousand green seeds had nothing to say about a cluster that was
> completely unavailable.

A conditional liveness invariant is week 5's debt, and that entry is why.

### Test layers

| Layer | What | Where |
|---|---|---|
| Unit | 20 binaries — CRC, framing, index, recovery, elections, `raft.state` | every commit |
| Property | Segment recovery cut at *every* byte position; codec round-trips | every commit |
| Fuzz | Batch decoder (libFuzzer) | `LOGENGINE_BUILD_FUZZERS=ON` |
| Simulation | Full cluster + faults + invariant checker | 50 seeds/push, sweeps locally |
| Determinism | Same seed twice → identical trace hash | every commit, **required** |
| Sanitizers | ASan + UBSan everywhere; TSan on the real runtime only (the simulator is single-threaded by construction) | matrix build |

## 5. Benchmarks

**Methodology before numbers, always.** Everything below comes from one command:

```bash
./bench/run_all.sh                  # a real 3-node cluster
BENCH_LOCAL=true ./bench/run_all.sh # harness validation on one machine
```

It reports simulator totals, failover time, a throughput/latency sweep, and the durability
trade-off, each with its conditions attached — hardware, filesystem, mount options, record
size, batch size, `acks` setting, and the offered load next to every latency. A p99 without
an offered load is not a weak measurement; it is not a measurement.

**Load is generated open-loop.** A closed-loop producer — issue, wait for the ack, issue
the next — stalls when the system stalls and simply stops sampling, so the stall disappears
from the histogram exactly when it matters. That is coordinated omission. Here the issue
schedule is fixed in advance and latency is measured from when a record was *due* to be
sent, so a delay caused by the system stays in the number.

### What is measured today

| Measurement | Value | Conditions |
|---|---|---|
| **Failover time** (NFR-3) | **p50 178 ms · p99 489 ms · p99.9 657 ms** | 200 induced leader failures across 10 seeds, 3 nodes, a crash every 5 s. **PASS** against the 900 ms bound (3× election timeout) |
| Simulation coverage | 1000 seeds → 50 simulated node-hours in 13.7 s | 60 s each, 3 nodes, crashes + partitions + clock jumps; 2.3 M records committed, 7,205 crashes survived |
| Distinct bugs found by the simulator | 3 | each replayable from a seed — [the bug journal](docs/retrospective.md) |
| Echo RPC throughput | 1.46 M RPC/s, p50 22 µs / p99 38 µs | loopback, single core, 32-deep pipeline — **transport only, not a cluster** |

Failover time is measured *in the simulator*, on virtual time, and that is a deliberate
choice rather than a shortcut: fifty real `kill -9`s take minutes, land wherever the
scheduler puts them, and produce a p99 that moves every run — and an interesting outlier is
gone forever. Here every failure lands where a seed put it and replays exactly. The trade,
stated plainly: it measures election + campaign + vote round trip and **excludes** process
restart, page cache, and scheduler latency. The real cluster's number is larger, and
`scripts/demo_week6.sh` shows one of them end to end.

### What is not in this table yet, and why

Sustained throughput and append-ack latency are **deliberately absent**. The harness
produces them — the sweep runs, the numbers are consistent, the knee is clean — but every
run so far has been three brokers sharing one laptop's CPU and one SSD, contending for the
exact resources being measured. Those results validate the harness and by this project's
own rule they never appear here. They go on real hardware next.

What the local runs *did* establish is a design gap worth stating up front: **§13.1's group
commit is specified and not implemented.** Every append currently costs one synchronous
`fsync`, so throughput is pinned to the device's flush rate and the median latency is one
`F_FULLFSYNC` rather than anything this code does. That is the top of the optimization list
and the intended subject of the before/after measurement, with the "before" already taken.

Also pending: a comparison against Kafka on identical hardware, consumer fetch throughput,
and end-to-end produce→consume latency — the last two need a client library.

## 6. What's not implemented, and why

Deliberate non-goals, not omissions — the full list is `project_spec.md` §3.

- **Multiple topics, partition rebalancing, a metadata controller.** One partition per Raft
  group is the interesting problem; a controller is orchestration on top of it.
- **Exactly-once transactions across partitions.** Idempotent produce within a partition is
  in scope; cross-partition atomic commit is a second consensus protocol.
- **Tiered storage, schema registry, ACLs, a wire-compatible Kafka client.** Each is a
  product feature, not a distributed-systems problem.
- **Membership changes.** Static config in v1. The design composes — joint consensus
  encoded as control records — precisely because control records already exist.
- **Pre-Vote and check-quorum.** Not yet. A node behind a cut link still inflates its term
  (81 terms against 1 election, measured) and disrupts the leader once on heal; an isolated
  leader keeps believing it is one. Both are recorded with their numbers rather than left
  implicit, because "we knew and chose" and "we didn't notice" look identical six months
  later.

## 7. Build and run

Clang 17+ or GCC 13 and CMake ≥ 3.24. Nothing else to install — GoogleTest is fetched, or
used from the system if it is already there.

```bash
cmake --preset dev && cmake --build --preset dev -j   # build
ctest --preset dev                                    # 20 test binaries
./build/dev/tools/sim --seeds 1000                    # fault sweep; prints node-hours
```

Other presets: `release | asan | ubsan | tsan | msan | fuzz`.

```bash
./scripts/demo_week6.sh                # 3 real processes; kill -9 the leader; no loss
./scripts/demo_week5.sh                # replication; acks=1 vs acks=quorum on one seed
./scripts/demo_week4.sh                # elections under faults; breaks I6 on demand
./build/dev/tools/sim --seed X --dump-trace /tmp/t    # replay a failure exactly
./build/dev/tools/log-dump <segment.log> --records    # read-only; never repairs
```

A real three-node cluster, one process each:

```bash
./build/dev/src/logengine --id 0 --port 9000 --dir data/0 \
    --peers 1@127.0.0.1:9001,2@127.0.0.1:9002 --produce
```

`scripts/demo_week6.sh` does all three and kills the leader. `[planned]` a `client/`
library, so producing and consuming happen from outside a broker process rather than from
the built-in generator.

---

**Living documents**, written as the work happens rather than at the end:
[status](docs/project_status.md) ·
[changelog](docs/changelog.md) ·
[architecture](docs/architecture.md) ·
[retrospective + bug journal](docs/retrospective.md)
