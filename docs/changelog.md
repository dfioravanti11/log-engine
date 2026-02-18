# Changelog

> **Living document.** Append at the top under `[Unreleased]` as work lands. This is not
> a git log — it records what *changed for the system*, plus the trials and tribulations
> that don't survive in commit messages: what broke, what got reversed, what took three
> days that should have taken one.
>
> **Conventions**
> - Newest first. Dated `YYYY-MM-DD`. Weekly headings match the milestones in `project_status.md`.
> - Sections: `Added` · `Changed` · `Fixed` · `Removed` · `Trials & tribulations`
> - Correctness bugs found by the simulator get a full entry in the bug journal
>   (`retrospective.md` §1) and a one-line pointer here. Don't duplicate the detail.
> - Reversed decisions are recorded, not deleted. "We tried X and it lost to Y" is the
>   single most useful thing in this file six months from now, and it feeds `retrospective.md`.
> - Numbers go in with their conditions attached. A throughput figure without hardware and
>   offered load is noise.

---

## [Unreleased]

### Week 8 — three VMs, one broker each · 2026-08-16

The benchmark harness could not produce a distributed number, and nothing in the repo
said so. `RealNetwork::listen()` bound `INADDR_LOOPBACK` — every cluster this project has
ever run, in CI, in the demos, and in `run_all.sh`'s throughput sweep, was three brokers
on one machine talking to themselves. Correct for a laptop and invisible for four weeks,
because a loopback cluster is a *working* cluster: it elects, replicates, commits and
survives `kill -9`. It just isn't distributed.

**Added**
- `--bind-all` on `logengine`, plumbed as a `RealNetwork` constructor flag rather than a
  new parameter on `Network::listen()`. Binding is a property of the deployment, not of a
  call; widening the interface would have pushed a detail of the real socket API through
  the seam into `io/sim/`, where it means nothing. Zero of the 16 existing `listen()` call
  sites changed, and `io/sim/` did not change at all. Defaults to loopback, so the laptop
  case cannot accidentally expose a broker.
- `bench/run_gcp.sh` — §19 #1 and #2 across three VMs in one zone. Nodes rendezvous on a
  shared wall-clock second before starting, because `gcloud ssh` connection skew is
  seconds and variable, and it would otherwise land inside the measurement window as
  throughput loss that has nothing to do with the code.
- `bench/run_node.sh` — the same sweep with the orchestration removed, for a shell on each
  VM and no gcloud on the laptop. Nodes derive an identical schedule from one `--start`
  epoch, so the three pastes need only happen before the first rate is due, not together.
  Testing it on one machine found a `pkill -f` scoped to the binary path rather than to
  the node's own invocation — harmless on three VMs, and it killed the other two brokers
  everywhere else. Now scoped by `--id` and `--port`; `run_gcp.sh` had the same line.
- `scripts/gcp_setup.sh`, `scripts/gcp_conditions.sh` — provisioning and the §19
  conditions block (kernel, CPU, filesystem, mount options, scheduler). `gcp_conditions.sh`
  **exits non-zero if the data directory is tmpfs**: fsync there is a no-op, so a
  throughput number measured on it is not optimistic, it is fictional.
- `docs/benchmarking.md` — the full procedure, ~$1/hour, ~30 minutes.

**Changed**
- `bench/run_all.sh` — `RATES=` now skips the throughput sweep, so the three sections that
  run on virtual time can be collected next to a real-hardware sweep without also running
  a meaningless loopback one. This needed `${RATES-...}` rather than `${RATES:-...}`: the
  colon form treats empty as unset, so the first version silently ran the default sweep
  while reporting that it had skipped it.
- `bench/run_all.sh` — the "Build first" hint said `--preset release` while the script
  looks for binaries under `build/dev/`. Following the error message could not fix the
  error.

**Trials & tribulations**
- The 500 GB disk in `docs/benchmarking.md` is not about space. GCP provisions PD
  performance *per gigabyte* — pd-ssd gives 30 write IOPS/GB — so a default 100 GB disk
  caps at 3,000 IOPS, and with §13.1's group commit still unimplemented (one fsync per
  append) the sweep would have measured a billing tier and published it as a throughput
  result. Found while reasoning about what the number would mean, not by running it.

### Week 8 — the benchmark suite · 2026-08-16

`bench/run_all.sh` produces every number in one command, each with its conditions attached.
**Criterion 7's harness is done**; the throughput numbers stay out of the README until they
come off real hardware, which is this project's own rule and not a technicality.

**Added**
- `bench/histogram.h` — exact percentiles by storing and sorting. A few million samples is
  tens of megabytes and one sort off the hot path; an approximate percentile would be one
  more thing a reader has to take on trust when the numbers are the whole point.
- `bench/failover.cpp` — **NFR-3, and it passes**: p50 178 ms, p99 489 ms, p99.9 657 ms over
  200 induced leader failures, against a 900 ms bound. Measured in the simulator so every
  failure lands where a seed put it and an outlier replays; the trade (it excludes process
  restart and scheduler latency) is stated in the output itself.
- **Open-loop load generation** in the broker binary (`--bench-rate`, `--record-bytes`).
  Latency is measured from when a record was *due* to be issued, so a stall stays in the
  histogram instead of vanishing exactly when it matters — coordinated omission, avoided
  by construction rather than by disclaimer.
- `bench/run_all.sh` — simulator totals, failover, a saturation *sweep* rather than a single
  rate (NFR-2 asks for a p99 at 70% of saturation, so saturation has to be measured first),
  and the `acks` trade-off. Prints hardware, filesystem, mount options and the exact command.

**Found**
- **§13.1's group commit was specified and never implemented.** Every append costs one
  synchronous fsync, so throughput is pinned to the device flush rate. Visible only as a
  number: p50 append-ack latency of 8.5 ms is one `F_FULLFSYNC`, and the sweep flattens at
  ~1,228 records/s. No correctness test could ever have caught it — group commit is a
  throughput decision and every invariant passes identically either way.
  `project_spec.md` §13.1 now records the gap; `retrospective.md` §5 tells the story.
- **Past saturation the cluster burns Raft terms.** At 4× the knee the event loop is so busy
  fsyncing that it starves its own tick timer and followers time out. A broker losing an
  election because it is working too hard is a good argument for §15's thread-per-core split.

**Fixed**
- The first failover run reported p99 = 2.9 s and failed NFR-3. The bug was the measurement:
  it counted every leaderless stretch, including those where *two of three nodes were down*
  and having no leader is correct — so it was reporting `restart_delay_max` wearing a
  benchmark's clothes. Failover samples now require a majority to have been alive
  throughout, which is the same conditionality I8 is stated with, applied to a measurement.
- `run_all.sh` copied `disown` from the week-6 demo, which also makes `wait` return
  instantly — so every number was read out of a file nothing had been written to yet.

### Week 6 — the architectural bet, settled · 2026-08-16

Demo ran. `scripts/demo_week6.sh`: **three real processes, real TCP, real disks.** A leader
is elected, the cluster commits, `kill -9` takes the leader out with no shutdown and no
flush, a new leader is elected in the next term, and the commit index goes 548 → 1,116
without ever moving backwards. `log-dump` reads both survivors' segments back off disk.

**The extraction is the story.** Weeks 3–5 grew the driver — the code that implements
§13's persist-then-send — inside `sim/`. Week 6 moved it to `server::Broker` unchanged,
and `sim::NodeWorkload` now *owns* one instead of being one. That direction matters more
than it looks: if production had its own copy, the simulator would have been validating a
sibling of the shipped code rather than the shipped code, and every correctness claim in
the README would have been about the wrong binary.

The evidence that the move was faithful: the 1000-seed sweep produced **byte-identical
totals** before and after — 2,320,262 records, 7,205 crashes — and all 21 test binaries
stayed green.

**Added**
- `server::Broker` — storage + raft + connections + the tick loop, over injected `io::`
  interfaces. It holds no simulator types and no production types; which implementation
  arrives is the caller's business.
- `server::BrokerObserver` — how `sim/` watches without being linked into production.
  Every hook is an *observation*, never a decision, so a null observer changes nothing
  about what a broker does.
- `src/main/logengine.cpp` — the product. One broker process, `--id/--port/--dir/--peers`,
  with an optional built-in producer so a three-node cluster does something worth watching
  before there is a client library.
- `scripts/demo_week6.sh` — brings up three processes, waits for a leader, SIGKILLs it,
  and fails loudly if the commit index regresses across the failover.

**Changed**
- `sim::NodeWorkload` is now the load generator and the invariant checker, and nothing
  else. Roughly 300 lines of driver left this file for `server/`.

**Fixed**
- The I2 check sampled the oracle's commit index *after* proposing, which under `acks=1`
  is after the append has already committed itself — so every append looked like a
  regression. Caught on the first run after the extraction, by the checker, which is the
  checker doing its job on a refactor rather than on a bug.

### Week 5 — Raft replication · 2026-08-16

Demo ran. `scripts/demo_week5.sh`: a replicated log committing 2,390 records a simulated
minute on one term; **1000 seeds green** — 50 simulated node-hours, 2.3 M records
committed, 7,205 crashes survived, in 13.7 s of wall clock; and the headline, seed 2,
`acks=quorum+fsync` keeping all 1,882 records where `acks=1` loses 18.

**Acceptance criterion 4 is met**, and the bug journal reached 3 entries — the weeks 4–5
milestone in full.

Built in two halves, deliberately: the consensus logic first, with the driver untouched
so the build stayed green throughout, then the wiring.

**Added (raft/)**
- **The log, as metadata only.** `raft::Node` still holds no entries — it holds the log's
  exclusive end and an append-only `vector<Epoch>` of which term produced which range.
  That is the leader epoch cache of §12.3, and it is what lets the node answer Raft's two
  questions about a log ("how current is yours?", "what term is the entry at N?") while
  the entries live in `storage::Log` and nowhere else. A hundred entries from one leader
  is one epoch; ten million entries across four leader changes is five.
- `Ready` grew the replication instructions: `truncate_from`, `append_entry`,
  `commit_index`. The division of labour is that **the node owns decisions and the driver
  owns bytes** — the node names an index to send and the driver reads that batch out of
  storage; the node says an arriving entry is acceptable and the driver, already holding
  the decoded bytes, writes it.
- **One entry per AppendEntries**, because §16.2 already says a batch *is* an entry. No
  entry arrays, no per-entry length prefixes, and no ambiguity about how many terms a
  message spans: the batch header carries `base_offset` and `leader_epoch`, so an entry is
  self-describing on the wire and on disk.
- Log matching (§5.3) with truncate-on-conflict, `next`/`match` per follower with
  optimistic initialisation and one-round-trip backoff, and commit advancement under the
  **§5.4.2 rule** — a majority holding an entry is *not* enough to commit it if it came
  from an earlier term.
- `tests/unit/test_raft_replication.cpp` — 16 tests, no storage, no disk, no event loop.
  A "log" is a `vector<Term>` and a "driver" is thirty lines of harness that carries out a
  `Ready` in the order `Ready` states it, asserting on every single one that the commit
  index never moves backwards and never passes the log end.

**Changed**
- Indices are storage offsets throughout, described by an **exclusive end**: `log_end == 0`
  is an empty log. The Raft paper starts at index 1 and uses 0 for "empty", which would
  have meant a translation at every `raft/`↔`storage/` boundary for no benefit.
  `set_log_state()` became `restore_log(end, epochs)` and `log_appended(index, term)`.

**Then wired end to end.** The simulator now runs a *replicated* log: one leader appends,
followers receive entries over the real wire codec and write them to the real
`storage::Log`, and a record is acked when it commits rather than when its writer fsynced
it. 500 seeds green, 1.16 M records committed, 3,591 crashes survived. With no faults:
one election, one term, 2,390 records committed in 60 simulated seconds.

**Added (storage)**
- `Log::append_replicated()` — `append()` with the offset decision *removed* rather than
  delegated. The batch carries the offset the leader assigned and must land at exactly
  that offset with exactly those bytes; re-framing would change the CRC and destroy the
  identity that makes "these two nodes hold the same entry" checkable. `Log` stays the
  offset authority for everything a client writes (I2); this is the one path where the
  authority sits upstream.
- `Log::truncate_to()` / `Segment::truncate_to()` — drops a divergent tail at a named
  batch boundary. Unlike recovery's truncation, which cuts at the first thing that fails
  to decode, this one cuts where Raft says to, because the bytes are perfectly valid.
- `Log::scan_epochs()` / `Segment::scan_headers()` — rebuilds the leader epoch cache
  (§12.3) by walking batch headers. Derived, never persisted: a second durable structure
  is a second thing that can disagree with the first.
- 10 new `LogTest` cases covering all three, including a replicated batch with a corrupted
  body (checked *before* it is written, not after it is read back).

**Added (wire)**
- The Raft message grew a 72-byte header plus an optional attached batch, copied verbatim
  with its CRC. `entry_end` is carried explicitly and is **not** `entry_index + 1` — an
  index is a record offset and an entry is a batch of several, which §12.2 already implies
  and which cost an afternoon to rediscover (see below).

**Fixed**
- **Bug journal #3** — a follower answered an AppendEntries with its own log *length*
  rather than the extent it had agreed to. Whenever a follower held a longer divergent
  tail than its leader, the leader recorded `match` past its own log end and committed
  entries that existed nowhere. Caught by a new I1 check — a leader must hold every
  committed entry (§5.4.1) — on its first simulated hour, seed 11.
- **Raft indices are record offsets, not entry numbers.** `log_appended()` advanced the
  log end by one per batch, so every later index landed mid-batch where no read can start
  and no append can land. The leader stopped sending anything at all — not even
  heartbeats — and the cluster spent its whole life electing: **89 elections in 30
  simulated seconds with no faults injected**.
- `entry_buf_` was declared and never sized, so every attempt to read a batch for
  replication failed silently and `send_raft()` returned without sending. Same symptom as
  above, and found at the same time.
- A follower's rejection hint must be a **batch boundary**. Classic Raft backs off one
  index per round trip; here that lands in the middle of a batch. The follower now answers
  with its own log end, or the first index of the epoch it disagreed in — the
  conflicting-term optimization, which also skips the whole disputed run in one round trip.

**Then the durability trade-off, made visible.** `scripts/demo_week5.sh` ran.

**Added (the `acks` knob — FR-2, §13.2)**
- `acks=quorum+fsync` (default): a record is promised once it is committed, i.e. a
  majority holds it durably. `acks=1`: the leader answers the moment its own fsync
  returns. **Acceptance criterion 4 is met** — seed 2, thirty crashes: quorum keeps every
  promise across 1,882 records; `acks=1` loses 18 of them at t=56.9 s, because a leader
  died before replicating and a new one was elected that had never seen them. Neither
  setting is a bug; they are different promises, and the simulator holds the system to
  whichever one it made.

**Added (I8 — liveness)**
- The invariant journal #2 said was missing, now in `project_spec.md` §6. The simulator
  measures the longest stretch with **no leader anywhere**, sampled at every instant the
  clock lands on. Healthy: **0.182 s** — the startup election and nothing else. Under
  crashes every 8 s: **2.156 s**.
- Stated conditionally on purpose. The unconditional version is false: during a partition
  that costs the majority, having no leader is *correct*. So the simulator reports the
  number and the caller decides what is tolerable given the faults it injected.
- `tools/sim` prints it on every run, and `--acks-1` selects the fast promise.

**Fixed (CI time)**
- Replication made a simulated second several times more expensive, and the two simulation
  binaries hit **29 and 36 minutes** under UBSan. Instrumented builds now scale run
  *durations* as well as seed counts (`tests/support/build_mode.h`), and the four
  seed-specific scenarios — which cannot be shortened without becoming different tests —
  step aside there and run in full in the optimized build. **40 minutes → 82 seconds**
  across dev/asan/ubsan/tsan, with every code path still covered.
- `Simulator.FaultsActuallyFire` caught that change making the fault tests vacuous: six
  second runs against a twenty second crash interval meant *no faults fired at all*. It now
  configures faults that fire promptly instead of inheriting a duration and hoping.
  `docs/retrospective.md` §5 — it is the best example yet of why that test exists.

**Still deferred to keep the endgame short:** Pre-Vote, check-quorum, and I3–I5 in the
oracle. All three are real and none of them blocks an acceptance criterion — see
`project_status.md` for what that costs and why the call was made.

### README · 2026-08-15

Written to the §20 outline, five weeks before it is due, because four of the seven
acceptance criteria are "it is in the README" and none of them can be satisfied by code.
It now accumulates instead of being written in one sitting in week 8.

Everything unbuilt is marked `[planned — week N]` rather than described in the present
tense, and every number carries its conditions — including the ones that are *not*
comparable to a production system (loopback echo, fsync-per-batch on a laptop) and say so.
The three build/test/sweep commands are verified against a clean configure.

### Week 4 — Raft leader election · 2026-08-15

Demo ran. `scripts/demo_week4.sh`: a healthy cluster elects one leader and holds it for
two simulated minutes on one term; **1000 seeds green** under crashes, partitions and
clock jumps with I6 checked after every event; and the headline — one seed, one knob, the
`raft.state` fsync on versus off, clean versus *two leaders in term 18*.

**The simulator found its second bug**, and this one is not a safety bug at all: see
*Fixed* and `retrospective.md` §1 entry #2.

**Added**
- `raft/types` — `HardState`, `Message`, `Config`, and `Ready`. `Ready` is where §13
  lives: if `persist_hard_state` is set, the state must be durable *before* any message
  in the same batch is sent. Bundling them makes the ordering reviewable in one place.
- `raft/node` — the election state machine: follower/candidate/leader, randomized
  timeouts, one vote per term, the §5.4.1 up-to-date check, step-down on a higher term,
  and heartbeats. **It holds no `io::` interface except `Random`** — no clock, no disk, no
  socket. It counts ticks and asks; the driver acts. See `retrospective.md` §2.
- `raft/state_file` — `raft.state` as two alternating 32-byte slots, each CRC'd and
  sequence-numbered. A torn write can only damage the half being written, so the previous
  half is still there and still valid. A file whose *both* halves are gone is an error
  that keeps the node down, not a fresh start — coming back as a term-0 voter is exactly
  the amnesia the file exists to prevent.
- `raft/codec` — fixed 48-byte message payloads under api keys 100/101, versioned at v0.
  Week 5 grows a variable tail for log entries, which is what the version is for.
- `sim/workload` is now the **driver**: it loads `raft.state`, ticks the node, and carries
  out `Ready` in one function, `drive()`, which persists and then sends. There is no
  second code path that sends a Raft message.
- `Oracle::record_leader()` — **I6**, held by the simulator rather than derived by asking
  nodes who the leader is. A node that wins a term and is destroyed by a crash before
  anyone observes it still won.
- `tools/sim` — `--crash-s`, `--restart-ms`, `--partition-s`, and `--unsafe-metadata`.
  The result line now reports elections, terms, and `raft.state` fsyncs.
- `tests/unit/test_raft_election.cpp` — 19 tests, **1 ms**, no infrastructure at all.
  Includes the amnesia test that makes the fsync argument by construction, and
  `WithoutJitterLockstepNodesSplitTheVoteForever`, which shows what the jitter is for.
- `tests/simulation/test_raft_cluster.cpp` — elections under faults, clock jumps that
  must not disturb them, the asymmetric-partition test §17 asks for by name, and
  `OneKnobDecidesWhetherATermCanElectTwoLeaders` — the test that proves the I6 checker can
  go red, because a check nobody has watched fail is not evidence. It pins the seed the
  demo script uses, so the demo cannot go stale quietly;
  `TheAmnesiaFailureIsNotOneCherryPickedSeed` carries the breadth.
- `tests/support/build_mode.h` — instrumented builds run reduced sweeps. The sanitizer
  presets are `Debug` (`-O0`), roughly 100× slower than `dev`, and seed *breadth* is a
  logic argument that a deterministic simulation makes build-independent; what ASan and
  UBSan need is the code *path*, which one seed reaches as well as forty.

**Changed**
- Ping/pong is gone from the simulated workload; Raft messages exercise the transport
  instead, and better. Each node still appends to its own log independently — those
  appends are not proposals and are not replicated yet — which keeps I1 and I2 under
  continuous test while elections stabilize. Turning them into Raft entries is week 5.
- Raft messages always leave over the sender's *own* connection to the target, never as a
  reply on the connection a request arrived on. Replying inbound would have quietly made
  every partition symmetric in practice while looking asymmetric in the config.

**Fixed**
- **TSAN no longer runs the simulation tests** — ER-5 enforced rather than merely written
  down. The `tsan` configure preset has said "real runtime only; useless in sim" in its
  display name since week 1, but nothing implemented it, so TSAN had been spending its
  time on a simulator that is single-threaded by construction and has no data race to
  find. Week 3 absorbed that as a 248 s run and raised a timeout; week 4 added ticks,
  heartbeats and an fsync per vote, and it became two 600 s timeouts and a 30-minute job.
  The simulation tests now carry a `simulation` label that the `tsan` test preset
  excludes. **TSAN: 1828 s with 2 timeouts → 7.6 s, 18/18.** They remain fully covered by
  ASAN and UBSAN, which are the sanitizers that can find something there.
- **The hard-state debt is sticky.** `take_ready()` used to clear the needs-persisting
  flag as it handed the state over, i.e. before anything was on disk. A failed fsync then
  left the node with a vote it believed it had recorded: the candidate's retry is granted,
  nothing changed so nothing asks for a write, and the grant goes out over a vote that
  never reached the platter. Both halves were individually correct; the composition
  violated §13. Now only `hard_state_persisted()` clears it, and the driver calls that
  after the fsync returns. Found by review, not by the simulator — `retrospective.md` §5.
- **A repeat vote for the same candidate no longer re-fsyncs.** Granting used to mark the
  state dirty unconditionally, so a candidate retrying every election timeout cost one
  `raft.state` fsync per retry — a stall on the durability path exactly when the cluster
  is already struggling. Only an actual change owes a write.
- **Bug journal #2** — one cut link between two of three nodes made leadership ping-pong
  every ~200 ms for the entire partition: 28 elections in 40 simulated seconds, 4228
  across a 30-seed sweep. A follower that could still hear the leader was granting votes
  to the node that could not, deposing a leader it was talking to. Fixed with the Raft
  dissertation's §4.2.3 rule; 28 → 2 on the same seed. **No invariant fired** — I1–I6 all
  held throughout, which is the interesting part.

**Known and deferred**
- **Pre-Vote is not implemented.** The lease rule stops the livelock but a node behind a
  cut link still campaigns into the void, so its term climbs — 81 terms against 1 election
  in a 40 s run — and it disrupts the healthy leader once on heal. Week 5.
- **No liveness invariant.** I1–I6 are all safety properties and are satisfied by a
  cluster that does nothing. This is what let bug #2 hide behind 1000 green seeds, and
  week 5 owes a conditional liveness check: over a window in which some majority stayed
  connected, a leader existed for most of it.
- **No check-quorum.** A leader isolated from its followers keeps believing it is leader.
  Harmless while it cannot commit; it matters in week 5, when a stale leader could serve
  a read.

### Week 3 — Simulator core · 2026-08-14

Demo ran. `scripts/demo_week3.sh`: one seed twice → byte-identical traces; one simulated
hour of a 3-node cluster in **1.9 s**; a seed sweep reporting node-hours and faults
survived. **The simulator found its first real bug on its first hostile sweep** — see
*Fixed* and `retrospective.md` §1 entry #1.

**Added**
- `sim/scheduler` — virtual time and the global event queue. Time only ever moves
  forward and only onto the next event, so idle time is free; ordering is `(when, id)`
  so ties are total and reproducible rather than heap-dependent.
- `sim/trace` — one line per event, folded into an FNV-1a rolling hash, plus a ring
  buffer of the last 64 events for violation reports. Hashes fields one at a time and
  never a pointer or a whole struct, because a trace hash that depended on an address or
  a padding byte would fail on every comparison and be worth nothing.
- `io/sim/SimClock` — virtual monotonic time with a per-node boot offset, and a wall
  clock that faults may jump in either direction. Nothing may move the monotonic clock.
- `io/sim/SimDisk` — in-memory files with a durable image and a visible image. `crash()`
  walks each file's unflushed writes and keeps, tears, or drops them, stopping at the
  first casualty. Also silent bit-flip corruption and per-operation I/O errors.
- `io/sim/SimNetwork` + `Fabric` — the full readiness-based `io::Network` over in-memory
  wires: listen/accept/connect, a send window with real backpressure, short writes,
  per-link latency, and directional partitions that reset the connections crossing them.
- `sim/workload` — each node runs a real `storage::Log` on the simulated disk and framed
  ping/pong over the simulated network. The `Oracle` is the shadow model: it lives in the
  simulator, not in the nodes, and survives the crashes they do not.
- `sim/simulation` — wires it together and injects the faults: crash/restart, symmetric
  and asymmetric partitions, and wall-clock jumps.
- `tools/sim` — `--seeds N`, `--seed X`, `--dump-trace`, `--io-errors P`. Never prints a
  result without its seed.
- `tests/simulation/test_determinism.cpp` — **I7, the required check**. Same seed twice →
  identical hash, event count, and every observable total; different seeds differ;
  determinism holds with faults off and at other cluster sizes; the captured traces are
  compared line by line, not just by hash. Plus `FaultsActuallyFire`, because a fault
  injector that never injects is a check that cannot fail.
- `runtime::EventLoop::next_timer_deadline()` / `has_pending_tasks()` — the only change
  the week-1 event loop needed to run unmodified on virtual time.

**Changed**
- `SimDisk::fsync()` replays the pending write list onto the durable image instead of
  copying the whole file. The copy made fsync O(file size), so a log that fsyncs every
  append paid quadratically — one simulated hour went from minutes to 1.9 s.
- Segment size in the simulated workload is 32 KB, so an hour of virtual life rolls
  segments hundreds of times. A fault injector that never crosses a roll boundary is not
  testing the interesting half of recovery.

**Fixed**
- **Recovery deleted 320 acked records because one read failed** (bug journal #1, seed 1).
  `Segment::recover()` treated a failed `pread` and a short `pread` identically. A short
  read is end-of-file — the tail was torn, truncate it. An I/O error is the disk
  declining to answer and says nothing about the bytes, so truncating on it destroys
  durable, acknowledged data because a read hiccuped once. Worse, it then reported the
  truncation as a normal torn tail, at info level. Now an error aborts recovery and
  propagates: the node stays down and retries rather than deleting data.
- The same conflation one level down in `Segment::read()`, which reported a transient
  I/O error as `CORRUPT_RECORD` — a *terminal* code that tells a client to give up
  forever on records that were never damaged.
- Use-after-free in the workload's read path, caught by review rather than by a test.
  `on_readable()` held a `Stream&` across the frame loop; replying to a ping can fail to
  write, a failed write calls `forget()`, and `forget()` erases that connection from the
  map — so the next line dereferenced a destroyed object. It needs a peer to die
  mid-request to fire, which is exactly the interleaving the fault injector produces and
  exactly the one no unit test was aiming at. The stream is now re-looked-up on every
  pass.
- Bounded the "run every event due at this instant" loop. Nothing schedules with zero
  delay today, so it cannot spin — but a future zero-delay event that reschedules itself
  would freeze the clock and present as *"the simulator is slow"* rather than as a bug.
- **Two thirds of the cluster was running at two thirds speed.** Timer deadlines are
  computed from each node's `monotonic_now()`, which carries that node's boot offset —
  and the simulator compared those raw values against shared time and advanced the shared
  clock to them, mixing two coordinate systems. A node with a 137 ms offset reported every
  deadline 137 ms late, was never the earliest, and only ran when another node's timer
  dragged the clock past its due point. Measured: 3 nodes, 30 s, no faults — node 0 acked
  595 batches, nodes 1 and 2 acked 406 and 421. Fixed by translating each deadline out of
  its node's frame; records acked over 500 seeds went from 1.32 M to 1.70 M. New test
  `Simulator.EveryNodeDoesItsShareOfTheWork` asserts the nodes stay within 15% of each
  other. Note what did *not* catch it: the trace hash matched perfectly on every run,
  because a node running at the wrong rate runs at exactly the same wrong rate every time.
- A node whose boot failed was never retried. `boot()` leaving `up == false` still armed
  the next *crash*, and `crash_node()` returns at its `!up` guard without scheduling
  anything — so the retry chain ended silently and the cluster shrank for the rest of the
  run with no signal in the result. Reachable whenever `--io-errors` makes `Log::open()`
  fail. A failed boot now schedules another boot.

**Trials & tribulations**
- **A green sweep was the suspicious part.** 500 seeds, 1.3 M acked records, 1,633
  crashes survived, zero violations — with `io_error_probability` still at 0. The storage
  code had never been run against a disk that *refuses*, only one that loses. Ten minutes
  and a CLI flag later, seed 1 failed inside 17 simulated seconds. Third time in three
  weeks that a green result turned out not to be checking anything (see the vacuous ER-1
  grep in week 1 and the `kill -9` demo in week 2).
- **A simulated crash that runs destructors is not a crash.** Killing a node meant
  destroying its event loop and workload — which in C++ runs their destructors, and
  `storage::Log::~Log()` writes the sparse index on the way out. The simulator was
  tidying up on its way down and handing recovery a neater disk than any real power cut
  produces. Fixed by leaving the disk **powered off** after `crash()`: every operation
  fails until `power_on()`, so the dying process's destructors get EIO, exactly as they
  would on a machine that just lost power.
- **The spec's fault menu was wrong for a byte stream, and it is this project's own
  spec.** §14.1 lists drop, reorder, and duplicate — sound for message passing, fiction
  for TCP. Simulating them would manufacture failures no production system can produce.
  The wires model latency, backpressure, short writes, partitions, and resets instead;
  message loss reappears one layer up as "the connection died and the RPC never got its
  reply", which is how a message actually gets lost. Recorded in `retrospective.md` §2
  rather than silently diverging.
- **Two namespaces called `sim`.** `io::sim` is the seam's other implementation, `sim` is
  the machine that drives it, and inside the former the latter has to be spelled
  `::sim::`. Both names are right for their directory; the papercut was cheaper than
  either rename.

### Week 2 — Single-partition storage · 2026-08-13

Demo ran. `scripts/demo_week2.sh` SIGKILLs the appender mid-write five times over,
reopens the log after each, and checks every offset it ever acked. Then it tears the
tail of the newest segment by hand and does it again.

**Added**
- `storage/record_batch` — the on-disk batch header (§16.2), CRC32C, a reusable
  `BatchBuilder`, and a `RecordIterator` that stops at its own batch's declared length
  rather than the end of the buffer it was handed.
- `storage/sparse_index` — one entry per ~4 KB, binary searched. Never fsynced, so
  `decode()` distrusts every byte: a partial trailing entry, a non-monotonic pair, or a
  position past the end of the log truncates the index at that point and the rest is
  rebuilt by scanning.
- `storage/segment` — one `.log` + `.index` pair, with recovery: read the index, verify
  the last entry actually points at a batch header, scan forward validating every CRC,
  and truncate at the first failure. Carries a `RecoveryReport` out of `open()` rather
  than logging it, so recovery behavior is testable instead of merely observable.
- `storage/log` — the offset authority. Rolls segments at a size bound, fsyncs before
  each roll so recovery only ever distrusts the last segment, and discards any segment
  stranded behind a truncation.
- `io::Disk::list_directory()` — sorted entry names, because `readdir()` order is
  filesystem-dependent and recovery order has to be reproducible (ER-2).
- `tools/log-dump` — batch headers, CRC status, epoch boundaries, record payloads with
  non-printable bytes escaped. **Read-only by construction** (below).
- `tools/crash-demo` — the appender/verifier pair the demo drives.
- 5 new test binaries: batch codec, sparse index, segment, log, and a seeded recovery
  property test that cuts a log at every byte position and at random bytes inside it.
  13 binaries total, green under dev, asan, ubsan, and tsan.
- `tests/fuzz/fuzz_batch_decode.cpp` + the `LOGENGINE_BUILD_FUZZERS` option the `fuzz`
  preset has been referencing since week 1 without it existing.

**Changed**
- **Batch header field order.** `crc32c` moved from byte 18 to byte 12, ahead of
  `leader_epoch`, `magic`, and `attributes`. See *Fixed*. `project_spec.md` §16.2 was
  updated to match — the spec was wrong, not the code.
- Segment size default is 32 MB, not Kafka's 128 MB (`project_spec.md` §25 Q1). A
  benchmark window that never rolls a segment never tests rolling.

**Fixed**
- **The CRC did not cover the control-batch bit.** The header layout as originally
  specced put `attributes` at byte 17, ahead of the CRC field and therefore outside the
  CRC's coverage. One flipped bit there turns an ordinary data batch into a control
  batch; the fetch path filters control batches out as internal records (§12.2); the
  checksum still verifies, recovery still accepts it, and a record that was acked to a
  producer is silently invisible to every consumer forever. That is a loss of an acked
  write (I1) with no error anywhere in the system. `leader_epoch` was unprotected for
  the same reason, which would have shown up in week 4 as inexplicable log divergence.
  Only fields that must stay rewritable — `base_offset` and `batch_length` — belong in
  front of the CRC.
- **The read path never checked a CRC.** `Segment::read()` decoded batch headers and
  copied bytes out without ever calling `validate_batch()`. Recovery validates a segment
  once, at open — so a bit that flips *after* that (silent disk corruption, §14.1) was
  never seen again by anything, and a fetch would have returned the corrupted bytes with
  an ok status. §17 is unambiguous: *CRC on read → refuse to serve.* Found by a review
  pass over the finished week, not by a test, because every existing test corrupted
  bytes *before* an open and so only ever exercised the recovery path.
- `SparseIndex::maybe_add()` computed `file_pos - last.file_pos` before establishing
  that `file_pos` was the larger of the two. On unsigned types a decreasing position
  wrapped to a huge number and disabled the interval check entirely.

**Trials & tribulations**
- **`kill -9` cannot tear a write, and the first version of the demo quietly proved
  nothing.** Five rounds of SIGKILL, five clean recoveries, `0 bytes truncated` every
  time. Killing a process does not drop the page cache — everything handed to `pwrite`
  is still there for the next process to read. A genuinely half-written batch needs
  power loss, a disk that lies about `fsync`, or a kill landing inside a `pwrite` big
  enough to be interrupted. The demo now has an explicit second phase that appends 37
  bytes of garbage to the newest segment and says out loud that it is a stand-in for
  the real fault, which week 3's simulator injects properly. The alternative — leaving
  the original demo in place — would have been a green check that tested the happy path
  and called it crash recovery.
- **`log-dump` was almost destructive.** The obvious implementation opens the segment
  through `storage::Segment` and prints what it finds. But `Segment::open()` *recovers*:
  it truncates the torn tail as a side effect of being asked to look at it. The forensic
  tool would have destroyed the evidence it was run to examine — and worse, it would
  have looked like it worked. It now walks the bytes itself and never writes.
- **The first demo run dumped the wrong file.** The script globbed `$DATA_DIR/*.log` to
  find the newest segment and picked up `append.log`, the appender's own stderr
  redirect. `log-dump` dutifully reported a 103-byte file with an undecodable header and
  zero batches, which is exactly what a catastrophically broken segment would look like.
  Two unrelated things — a shell glob and a tool that reports damage clearly — combined
  into a convincing false alarm.
- **The ack accounting was wrong in a way that only a crash could expose.** The verifier
  computed "records acked" from the last ack line's offset plus its count, which assumes
  the acked set is contiguous. It isn't: a crash leaves durable-but-unacked records
  behind, the next run starts appending *after* them, and the ack list legitimately
  contains a hole. Summing the counts is right; deriving from the last entry is a bug
  that only appears on the second run after a crash.
- **Recovery normally scans the whole active segment, and that is not a bug.** The
  sparse index is never fsynced, so after a real crash it is usually absent or stale and
  recovery starts from byte zero. The index only earns its keep on a clean restart. This
  is Kafka's behavior too, and it is the reason recovery time is bounded by segment size
  rather than by log size — which is the actual argument for keeping segments small.
- **Throughput on this laptop is ~100–200 acked batches/s**, and that number is honest:
  macOS `F_FULLFSYNC` really does wait for the drive. It is a fsync-latency measurement,
  not a throughput measurement, and it does not belong in the README.

### Week 1 — Transport stack · 2026-08-12

Demo ran. `bench/echo` does request/response over a real loopback socket through the
real event loop and the real codec.

**Added**
- Build system: CMake 3.24+ with 7 presets (`dev`, `release`, `asan`, `ubsan`, `tsan`,
  `msan`, `fuzz`). Warnings and sanitizers are applied per-target, never globally, so
  `-Werror` never reaches GoogleTest.
- CI: sanitizer matrix × two compiler frontends, plus the ER-1 guard as its own job.
- `scripts/check_one_rule.sh` — the ER-1 grep, with a `--self-test` mode (below).
- `base/`: `Slice`/`MutSlice`, `Buffer`, `Result<T>`/`ErrorCode`, little-endian codecs,
  CRC32C with an ARMv8/SSE4.2 hardware path validated against the software table.
- `io/` seam: `Clock`, `Network`, `Disk`, `Random` + real implementations.
  `RealNetwork` carries **both** kqueue and epoll backends; `RealDisk` uses
  `F_FULLFSYNC` on macOS. `SeededRandom` is xoshiro256\*\* with SplitMix64 expansion.
- `runtime/EventLoop`: callback loop, min-heap timers, posted tasks.
- `wire/`: framed codec (`u32 length | u16 api_key | u16 api_version | u32 correlation_id`),
  API-key registry, max-frame enforcement.
- 8 test binaries / 66 cases. Green under dev, asan, ubsan, and tsan.

**Changed**
- Sanitizer/warning flags moved from global to a `logengine_flags` INTERFACE target
  after `-Wconversion -Werror` broke the GoogleTest build.

**Trials & tribulations**
- **The ER-1 guard passed vacuously and I nearly shipped it that way.** `src/storage/`
  and friends don't exist yet, so the script looped over nothing and printed OK. A
  check that cannot fail is not a check — it would have sat green in CI until week 4
  and then been trusted. Added `--self-test`, which plants a `<chrono>` violation,
  asserts the guard catches it, and cleans up. CI runs it before the real check.
- **`FrameDecoder::next()` originally consumed the frame it returned.** That hands the
  caller a payload view into a buffer that the very next append can reallocate — a
  use-after-free that would have shown up as corrupted records, not as a crash, and
  probably not until Raft was replicating. Split into `next()` (peek) +
  `consume_frame()`. The docstring now says why, because the API looks gratuitously
  two-step otherwise.
- **Timer ties.** The heap comparator originally ordered on deadline alone, so two
  timers with the same deadline fired in heap-layout order. Under `CLOCK_MONOTONIC`
  exact ties are rare; under virtual time they will be *constant*. That is a
  determinism bug (ER-2) shipped in week 1 and cashed in week 3 as an unreproducible
  trace hash. Comparator now breaks ties on insertion id, with a test.
- **Known shortcut: `connect()` blocks.** Fine now — it happens once at setup, before
  the loop spins — and wrong from week 6, when a client must reconnect during a leader
  failover. A blocking call inside an event loop stalls every partition on that core.
  Marked in the source at the call site.
- macOS `fsync()` does not reach the platter; `F_FULLFSYNC` does. A durability
  benchmark taken on plain `fsync()` here would simply have been false.
- Numbers, with conditions: **1.46M RPC/s, p50 22 µs, p99 38 µs, p99.9 93 µs, max
  5.7 ms** — Apple M1 (8 cores), macOS 26.3, loopback TCP, 128 B payload, pipeline
  depth 32, closed-loop, server and client in one process on two threads. Closed-loop
  means coordinated omission, so these are a transport floor and a smoke test, not a
  result. They do not go in the README.

### Week 0 — Planning · 2026-08-12

**Added**
- `project_spec.md` — full specification in three parts: product requirements
  (goals, FR-1…FR-12, invariants I1…I7, NFRs, acceptance criteria), engineering &
  technical design (stack, engineering requirements, architecture, system design,
  formats, testing, benchmarks), and execution (milestones, risks, cut list).
- `CLAUDE.md` — always-in-context project memory: non-negotiable rules, conventions,
  commands, working agreements. Capped at ~80 lines by its own instruction.
- `.env.example` + `.gitignore` — environment for the layers *outside* the binary
  (scripts, deploy, benchmarks, CI). The broker itself takes flags + TOML, never
  `getenv()`, because ER-1 forbids `src/` from touching the OS.
- `docs/architecture.md`, `docs/changelog.md`, `docs/project_status.md`,
  `docs/retrospective.md` — living documents, updated as the project evolves.

**Changed**
- Multi-partition promoted from week-9 stretch into **week 7**, paying for it by cutting
  io_uring. Reason: thread-per-core is the strongest concurrency claim in the project and
  is completely unexercised with a single partition — it would have stayed a design
  paragraph instead of becoming a benchmark. Once single-partition Raft works, a second
  partition is mostly plumbing.
- Cap'n Proto / FlatBuffers dropped from the dependency list. The hand-written byte layout
  *is* an artifact here — CRC placed after the fields it doesn't cover is a real design
  decision worth defending. A schema compiler adds a dependency without adding evidence.
- Bug journal folded into `retrospective.md` §1; the planned standalone `docs/bugs.md` is
  gone. The record (seed, invariant, symptom, cause, fix commit) and the story (what I
  believed, how I found it, what it cost) describe the same bugs — two files would have
  meant writing each entry twice and letting them drift. Entries carry a `[sim-only]` tag
  where a real cluster plausibly never would have surfaced them.
- Cloud VM provisioning deferred to **end of week 5** with `BENCH_LOCAL=true` as the
  interim default. Weeks 1–5 need no cloud account; week 6's deploy script needs a target,
  and the student-pack credit takes days to approve.

**Trials & tribulations**
- *(none yet — this section is where the interesting content will accumulate)*

---

<!--
Template for a weekly entry — copy, don't improvise:

## Week N — <theme> · YYYY-MM-DD → YYYY-MM-DD

**Added**
- …

**Changed**
- … (if a decision reversed, say what lost and why)

**Fixed**
- #<n> <one line> — see `retrospective.md` §1

**Trials & tribulations**
- What cost more than it should have, and what the actual cause turned out to be.
- Dead ends worth remembering. Include the thing you believed at the time that was wrong.
- Demo status: did the week's demo run? If not, say so plainly and say what's missing.
-->
