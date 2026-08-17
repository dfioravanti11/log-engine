# log-engine

A replicated append-only log in C++20, the durable core of a system like Kafka. Three
server processes (brokers) keep identical copies of a log. Clients append records, and
once the system says "stored", the record survives crashes and leader changes. The
unusual part is how it is tested: the whole cluster also runs inside a deterministic
fault simulator, on a fake clock, fake disks, and a fake network, with crashes,
partitions, and disk corruption injected on a schedule chosen by a random seed. Same
seed in, identical run out, so every bug it finds replays exactly. The simulator and the
real servers run the same code; only the I/O implementations differ.

## Quick start

Clang 17+ or GCC 13, CMake ≥ 3.24. Nothing else to install; GoogleTest is fetched.

```bash
cmake --preset dev && cmake --build --preset dev -j   # build
ctest --preset dev                                    # 21 test binaries
./build/dev/tools/sim --seeds 1000                    # fault sweep, ~14 s, prints node-hours
```

A three-node cluster, one process per broker (`scripts/demo_week6.sh` does this and then
`kill -9`s the leader):

```bash
./build/dev/src/logengine --id 0 --port 9000 --dir data/0 \
    --peers 1@127.0.0.1:9001,2@127.0.0.1:9002 --produce
# ... plus --id 1 and --id 2. Brokers bind loopback unless given --bind-all.
```

Other useful commands:

```bash
./scripts/demo_week2.sh ... demo_week6.sh             # one runnable demo per week
./build/dev/tools/sim --seed X --dump-trace /tmp/t    # replay a failure exactly
./build/dev/tools/log-dump <segment.log> --records    # inspect a segment; never repairs
BENCH_LOCAL=true ./bench/run_all.sh                   # every benchmark, one command
```

Presets: `dev | release | asan | ubsan | tsan | msan | fuzz`.

## What it does

A producer appends batches of records to the log; each record gets an **offset**, a
permanent position number, and consumers fetch from any offset. Three brokers each hold
a full copy, coordinated by **Raft**, a consensus algorithm in which the brokers elect
one **leader** that accepts all appends and replicates them to the other two. A record
is **committed** once a majority (two of three, a **quorum**) holds it; if the leader
dies, an election picks a new one from the brokers that are up to date.

There are two durability knobs, and they are deliberately never conflated:

- **Per-request `acks`.** With `acks=quorum+fsync` (the default), the append is
  acknowledged only after a majority has the record on disk. **fsync** is the system
  call that forces a write out of the OS cache onto the physical disk; without it,
  "written" means "in memory somewhere". With `acks=1`, the leader answers as soon as
  its own write is durable. That is faster and can lose acknowledged records when a
  crash is badly timed. Neither is a bug; they are different promises, and the simulator
  demonstrates the difference on demand (below).
- **Raft's own metadata** (the current term, and which candidate this node voted for) is
  fsynced before any response that changes it, always, not tunable. A node that forgets
  its vote can vote twice in the same term and elect two leaders.

Two design points worth knowing before reading the code:

- **The user's log is the Raft log.** There is no separate command stream feeding a
  state machine, which would write every record twice. Consumers are protected from
  leader-change truncation by clamping fetch to the commit index: a record cannot be
  read until it can no longer be un-written.
- **Offsets are monotonic, not dense.** Raft writes internal control records into the
  same log; they occupy real offsets and are filtered out of fetch responses. A client
  must never compute `last_offset + 1`. This is Kafka's behavior too.

## How it is tested

### The one rule

No file in `storage/`, `raft/`, `server/`, or `client/` may touch the operating system.
No clocks, no sockets, no file I/O, no threads, no `rand()`. Every such need goes
through one of four `io/` interfaces (`Clock`, `Network`, `Disk`, `Random`) handed in at
construction. CI greps for violations, and the grep has a `--self-test` that plants a
violation and checks the guard actually fires, because a check that cannot fail is not a
check.

The real binary constructs `io/real/` (epoll/kqueue sockets, `pwrite`, `fdatasync` or
`F_FULLFSYNC`). The simulator constructs `io/sim/` (a virtual clock, in-memory disks and
wires). Everything above that seam is the same code either way. Since week 6 the driver,
`server::Broker`, exists exactly once: the simulator owns one and observes it through
hooks that can watch but never decide, and `logengine` runs the same object on real
sockets. The evidence the extraction was faithful: a 1,000-seed sweep produced
byte-identical totals before and after the move (2,320,262 records, 7,205 crashes).

That is the whole bet. The code the simulator has crashed thousands of times is the code
that ships, not a sibling of it.

`raft/` goes further than the rule requires: `raft::Node` holds no `io/` interface at
all except `Random`. It counts ticks and returns a description of what it wants done
(persist this, send that); the broker carries it out. I chose ticks over timers because
it makes the state machine testable with no infrastructure (19 election tests run in
1 ms as three objects and a message queue), and because a machine with no clock has no
way to be confused by one: the simulator's clock-jump fault has no path into consensus.

### The simulator

The whole cluster runs on one thread, on virtual time. Faults are injected on a schedule
drawn from the seed: crash/restart with loss of unflushed writes and torn (half-written)
writes, per-operation disk errors, silent bit flips, symmetric and asymmetric network
partitions, connection resets, and wall-clock jumps. Idle time is skipped, so 1,000
seeds cover **50 simulated node-hours in 13.7 s** of wall clock. Determinism is a tested
property, not a hope: every run emits an event trace, the trace is hashed, and a
required CI check runs the same seed twice and compares. A failure prints its seed, and
`--seed X --dump-trace` replays it exactly. No result is ever printed without its seed.

An oracle outside the nodes (so it survives the crashes they don't) checks eight
invariants after every event:

| # | Invariant |
|---|---|
| I1 | An acknowledged write is never lost |
| I2 | Offsets are monotonic in commit order (gaps allowed, density never assumed) |
| I3 | No reordering within a partition |
| I4 | A committed entry is never overwritten |
| I5 | No consumer reads past the commit index (structural: fetch is clamped) |
| I6 | At most one leader per term |
| I7 | The same seed produces a byte-identical event trace (required CI check) |
| I8 | Once a majority can communicate, a leader appears within a bounded time |

I8 exists because I1–I7 are safety properties, and a cluster that does nothing satisfies
all of them. That is not hypothetical; it is how bug #2 below hid behind a thousand
green seeds.

### Two runs you can do yourself

The metadata fsync, on seed 4. Same seed, one flag:

```
$ ./build/dev/tools/sim --seed 4 --duration-s 120 --crash-s 3 --restart-ms 120
raft                75 elections over 84 terms, 245 state fsyncs     <- green

$ ./build/dev/tools/sim --seed 4 --duration-s 120 --crash-s 3 --restart-ms 120 --unsafe-metadata
invariant:  I6
detail:     term 18 has two leaders: node 0 and node 2
```

A node voted, crashed before the write reached the disk, restarted without the memory of
it, and voted again in the same term. No bug in the algorithm; the fsync is the
algorithm's precondition.

The `acks` knob, on seed 2, with a crash every four simulated seconds:

```
$ ./build/dev/tools/sim --seed 2 --duration-s 60 --crash-s 4
acked               1882 records over 939 batches
faults              30 crashes, 2 partitions, 164 resets     <- every promise kept

$ ./build/dev/tools/sim --seed 2 --duration-s 60 --crash-s 4 --acks-1
invariant:  I1
detail:     node 2: won an election without the committed prefix (1848, 1866)
```


### Test layers

| Layer | What | When |
|---|---|---|
| Unit | 21 binaries: CRC, framing, index, recovery, elections, replication | every commit |
| Property | Segment recovery cut at every byte position; codec round-trips | every commit |
| Fuzz | Batch decoder (libFuzzer) | `LOGENGINE_BUILD_FUZZERS=ON` |
| Simulation | Full cluster + faults + invariant checker | 50 seeds per push, 1,000 locally |
| Determinism | Same seed twice → identical trace hash | every commit, required |
| Sanitizers | ASan + UBSan everywhere; TSan on the real runtime only (the simulator is single-threaded by construction) | CI matrix |

## Results

Methodology first. Every number here ships with its hardware, kernel, filesystem, batch
size, and offered load, and `bench/run_all.sh` regenerates all of it from one command
(`bench/run_gcp.sh` for the hardware section; the procedure is
[`docs/benchmarking.md`](docs/benchmarking.md)). Load generation is **open-loop**: the
issue schedule is fixed in advance and latency is measured from when a record was *due*,
so a stall stays in the histogram instead of disappearing exactly when it matters (the
closed-loop mistake known as coordinated omission). Numbers from loopback or from three
brokers sharing one laptop never appear here; that is this project's own rule.

### Throughput and latency

Conditions: three GCE `n2-standard-4` VMs (Intel Xeon @ 2.80 GHz, 4 vCPU, 15.6 GiB), one
broker each, one zone, 150 GB pd-ssd, ext4
(`rw,relatime,discard,errors=remount-ro,commit=30`), kernel 6.17.0-1022-gcp. 1 KiB
records, 16 per batch, `acks=quorum+fsync`, open-loop, 30 s per rate. The `terms` column
is Raft's election counter: 1 means the cluster never held an election under load.

| offered rec/s | achieved | MB/s | p50 ms | p99 ms | terms |
|---|---|---|---|---|---|
| 1,500 | 1,490 | 1.46 | 3.85 | 4.83 | 1 |
| 2,000 | 1,985 | 1.94 | 3.79 | 4.77 | 1 |
| 2,500 | 2,479 | 2.42 | 4.43 | 5.65 | 1 |
| 3,000 | 2,980 | 2.91 | 3.60 | 4.50 | 1 |
| 3,500 | 3,470 | 3.39 | 3.53 | 4.43 | 1 |
| 4,000 | 3,970 | 3.88 | 3.96 | 7.29 | 1 |
| 4,400 | 4,364 | 4.26 | 3.29 | 5.58 | 1 |
| 4,600 | 4,573 | 4.47 | 5.53 | 7.30 | 1 |
| 4,800 | 4,765 | 4.65 | 3.27 | 5.48 | 1 |
| **5,000** | **4,969** | **4.86** | **3.18** | **5.25** | **1** |
| 6,000 | — | — | 955 | 2,207 | **39** |

Saturation is **4,969 records/s (4.9 MB/s)**: the cluster tracks offered load to within
about 30 records/s all the way up, then collapses at 6,000. **p50** (median latency)
stays between 3.2 and 5.5 ms across the entire clean range and barely moves with load.
**p99** — the latency 99% of requests beat — at 70% of saturation (3,500 rec/s) is
**4.43 ms**, against the project's 10 ms target. Pass.

### Failover

| | p50 | p99 | p99.9 | Conditions |
|---|---|---|---|---|
| Time to a new leader | **178 ms** | **489 ms** | 657 ms | 200 induced leader failures across 10 seeds, 3 nodes, a crash every 5 s; 900 ms bound (3× election timeout). Pass |

Measured in the simulator, on virtual time, on purpose: every failure lands where a seed
put it and any outlier replays exactly, where fifty real `kill -9`s land wherever the OS
scheduler puts them and an interesting outlier is gone forever. The trade, stated
plainly: this covers election, campaigning, and the vote round trip, and **excludes
process restart and OS scheduling. A real cluster is slower.** `scripts/demo_week6.sh`
shows one real failover end to end:

```
leader elected: node 0 (term 1)
committed before the kill: 548 records
SIGKILL sent to node 0 — no shutdown, no flush, no mercy
new leader: node 2 (term 2)
committed now:             1116
```

### Simulator coverage

| | Value | Conditions |
|---|---|---|
| Fault sweep | 1,000 seeds, **50 simulated node-hours**, in 13.7 s wall clock | 60 s each, 3 nodes, crashes + partitions + clock jumps; 2.3 M records committed, 7,205 crashes survived |
| Durability trade-off | `acks=quorum+fsync` keeps all **1,882**, `acks=1` loses **18** | seed 2, 60 simulated s, a crash every 4 s; same seed, one flag |
| Bugs found | 3, each replayable from its seed | seeds 1, 3, 11; [the bug journal](docs/retrospective.md) |

## What the numbers mean

**The pipeline is one batch deep, and that explains everything else.** At the top clean
rate the cluster commits 312.5 batches/s and each batch takes 3.183 ms end to end.
Multiply them (Little's law) and you get 0.99: exactly one batch is ever in flight. Each
batch is proposed, flushed, replicated, and committed before the next one starts.
Nothing overlaps. So the flat p50 was never headroom, it was a service time, and the
maximum throughput did not have to be discovered by pushing until something broke: it is
just 1 ÷ latency, derivable from the first clean row of the table. One batch of 16
records per 3.2 ms is 5,000 records/s. The run at 6,000 was confirmation.

Two fixes follow, and they are independent ceilings that multiply rather than one:

1. **Group commit** (designed, not implemented). Every append currently costs one
   synchronous fsync, so the 3.2 ms is mostly one device flush. Batching flushes across
   in-flight appends shrinks the 3.2 ms. This gap is the main reason throughput is where
   it is, and no correctness test could ever have found it; it took a measurement.
2. **Pipelining AppendEntries.** Even with a faster flush, one-at-a-time serialization
   caps throughput at 1 ÷ latency. Letting batches overlap removes that limit.

**Past the limit it collapses rather than degrading.** At 6,000 records/s the leader is
so busy with synchronous disk flushes that it misses its own heartbeat deadlines, the
followers conclude it is dead, and the cluster spends the run electing instead of
working: 39 elections in 30 seconds, 56 at 8,000. A broker that loses its leadership
because it is working too hard is the argument for taking disk flushes off the thread
that owns consensus timing.

One platform note: p50 on Linux/pd-ssd is 3.2 ms against 8.5 ms for the same code on
macOS/APFS. Most of that gap is `fdatasync` versus `F_FULLFSYNC`, not the hardware.

### Caveats, which the numbers inherit

- **The load generator runs inside the leader process**, on the same event loop as the
  consensus code, because there is no client library yet. When it falls behind it
  catches up in bursts of up to 64 appends, each with its own synchronous fsync. The
  starvation effect is real; the severity above 6,000 records/s is partly the measuring
  instrument.
- **The saturation point is not a clean threshold.** Each rate starts a fresh cluster,
  which must finish its startup election before the producer's backlog builds. Near the
  knee that race is a coin flip: one sweep had 4,200 collapse while 4,400 through 5,000
  ran clean above it. Rates well below the knee are repeatable from one run; rates near
  it need several, and the honest output there is a failure rate.
- **Until week 8, every "cluster" test ran on one machine.** The server bound only
  loopback, so CI, the demos, and the benchmarks were all three processes talking to
  `127.0.0.1`. Everything passed, because a loopback cluster is a working cluster; it
  just has no network in it. Found while setting up the GCP run, fixed with
  `--bind-all`, and the sweep above was the first genuinely multi-machine run.

## Code layout

```
src/
  base/     slices, buffers, Result<T>, CRC32C, endian codecs — no dependencies
  io/       the seam: Clock, Network, Disk, Random + real/ and sim/ implementations
  runtime/  callback event loop with a timer heap
  wire/     frame codec, API keys, versioning
  storage/  segments, sparse index, batch format, crash recovery
  raft/     the consensus state machine (ticks in, decisions out), raft.state file
  server/   Broker — storage + raft + connections + the tick loop, the one driver
  sim/      virtual-time scheduler, trace hashing, fault injection, the oracle
  main/     logengine, the broker binary
tools/      sim (the simulator CLI), log-dump (read-only segment inspector)
bench/      run_all.sh, run_gcp.sh, the failover and echo benchmarks
scripts/    weekly demos, the ER-1 guard, GCP provisioning
```

Dependencies point strictly downward; `sim/` depends on `server/` and never the
reverse. Full structure and request paths: [`docs/architecture.md`](docs/architecture.md).

Living documents, written as the work happened rather than at the end:
[status](docs/project_status.md) ·
[changelog](docs/changelog.md) ·
[architecture](docs/architecture.md) ·
[retrospective and bug journal](docs/retrospective.md) ·
[benchmarking procedure](docs/benchmarking.md)
