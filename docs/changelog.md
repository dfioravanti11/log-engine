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
