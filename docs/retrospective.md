# Retrospective — raw material for the blog post

> **Living document.** Capture-as-you-go, not write-up-at-the-end. The good details
> decay within about 48 hours: what you believed before the bug, what the wrong theory
> was, how long you stared at it. Write them down *while confused*, not after.
>
> **This file is deliberately unpolished.** Fragments are fine. Drop things in the right
> bucket and move on — shaping happens in week 9.
>
> Rules:
> - Write the wrong belief, not just the fix. "I assumed the fsync completion ran on the
>   same path as the submit" is the story; "moved the ack to the completion handler" is not.
> - Keep the seed with the story. A reproducible bug is a demonstrable one.
> - Record numbers when you see them, with conditions. You will not remember them.
> - Nothing is too small. A one-line note in week 2 becomes a paragraph in week 9.

**Started:** 2026-08-12

---

## Post thesis (revise as it becomes true)

> Working draft: *Plenty of people write a Raft. Almost nobody writes one they can prove
> anything about. The interesting engineering wasn't consensus — it was building the
> machine that made consensus bugs reproducible.*

If by week 9 the post is actually about something else, change this line rather than
forcing the material into it.

---

## 1. Bug journal — the record *and* the story

**This is the bug journal.** Every simulator-found bug gets an entry here; there is no
separate `bugs.md`. The formal record and the narrative are the same entries because they
are the same bugs, and maintaining two files would mean writing each one twice and letting
them drift.

The top half of an entry is the record — seed, invariant, symptom, cause, fix. That half is
the evidence, it's what the README links to, and it must be complete. The bottom half is the
story — what you believed, how you found it, what it cost. That half is the post, and it is
the half that evaporates if you don't write it the same day.

Number entries sequentially. **The fix commit message references the entry number, and the
entry references the commit SHA** — the link runs both directions or the evidence is worthless.

Also tag entries the simulator caught that a real cluster plausibly never would (rare
interleavings, asymmetric partitions, unflushed-write loss at exactly the wrong moment)
with **`[sim-only]`**. One of those becomes the 60-second pitch's specific example:
*"that's how I caught [X], which I'd never have found by running the real thing."*

### #1 — Recovery deleted 320 acked records because one read failed · Week 3 · `[sim-only]`

```
seed:       1  (./sim --seed 1 --duration-s 30 --io-errors 0.02)
invariant:  I1 — an acked write is never lost
symptom:    node 1's log ended at offset 48; offset 368 had been acked and fsynced.
            24,960 durable bytes truncated at t=16.27s, 320 records gone.
cause:      Segment::recover() treated a failed pread and a short pread as the same
            thing. A short read is end-of-file — the tail was torn, truncate. An I/O
            error is the disk declining to answer, and says nothing about the bytes.
            One line, `if (!got || got.value() < kBatchHeaderBytes) break;`, meant a
            single transient EIO mid-scan silently deleted every batch after it.
fix:        src/storage/segment.cpp — split the two cases; an error aborts recovery and
            propagates, so the node stays down and retries rather than destroying data.
            Regression tests: SegmentTest.ATransientReadErrorDuringRecoveryNeverTruncates
            (fails the read at each of 8 scan positions) and
            .ATransientReadErrorOnFetchIsNotReportedAsCorruption.
commit:     4c196a8
```

**What I believed:** that recovery's stopping conditions were all the same kind of thing.
The scan walks forward until something stops looking like a valid batch, and I had a
tidy list of the ways that can happen — short read, bad magic, bad CRC, wrong offset —
and treated "the read failed" as one more entry on it. The comment I wrote at the time
said `// short tail`, which is exactly the mistake in miniature: I named the *benign*
case and then handled a second, entirely different case with the same code.

**What was actually happening:** the disk returned EIO in the middle of a scan over a
segment that was completely intact. Recovery concluded the file ended there, truncated
everything after it, and reported the truncation as a normal torn tail — at info level,
because a torn tail *is* normal. So the loudest signal the system produced while
destroying 320 acknowledged records was an informational log line saying everything was
fine.

**How I found it:** not on purpose. Week 3's fault menu had `io_error_probability` at 0
by default, and 500 seeds were green. That felt like too little resistance for code that
had never been run against a hostile disk, so I wired the knob to a CLI flag and swept
40 seeds at 2%. Seed 1 failed within 17 simulated seconds. The invariant checker printed
the seed, the offsets, and the last 64 trace events, and the line
`RECOVERED next_offset=48 bytes_truncated=24960` sitting directly under a `RESTART` made
the cause obvious before I opened the file.

**Time lost:** about ten minutes from failing seed to fix. That number is the whole
argument for the week — the same bug reaching a real cluster would present as "we're
missing some records after that flaky-disk incident", with no reproduction and nothing
in the logs but an info line.

**Why a reader cares:** the bug is invisible to every test week 2 shipped, and week 2 was
not lazy about testing recovery — there is a property test that cuts a log at *every
byte position* and checks the result is exactly the longest valid prefix. It passes.
It was always going to pass, because it corrupts the *file* and never the *disk*. The
fault it could not express is the one where the bytes are fine and the storage device
misbehaves, and that gap is precisely what a fault-injecting `io::Disk` exists to cover.
**A checksum tells you the bytes are wrong. Nothing in the file tells you the reader was
wrong.**

Also the cleanest example so far of why the simulator is the deliverable rather than the
log: this is a storage bug, found with no Raft, no cluster, and no network — by a machine
whose entire job is to be an unreasonable disk, reproducibly.

<!-- Copy the template below for entry #2. -->


> **Week 2 note on what does *not* go here.** Week 2 found a real correctness bug — the
> CRC did not cover the control-batch bit (§5) — and it is deliberately not an entry in
> this section. There is no seed, because there is no simulator yet; it was found by
> writing a property test and asking which bytes the checksum actually protects. This
> section is for bugs with a reproducible seed, and diluting it with bugs found by
> ordinary means would make the strongest artifact in the repo weaker. The story lives
> in §5 and the fix is in the week 2 changelog entry.

<!-- Copy this. The record half is mandatory; the story half is what makes it worth having.

### #N — <one-line symptom, not the fix> · Week N · `[sim-only]`
```
seed:       0x????????
invariant:  I1 — acked writes are never lost
symptom:    3 records lost after node 2 restart w/ unflushed-write-loss at t=41.2s
cause:      AppendEntries response sent from the write-submit path, not the completion path
fix:        commit a1b3f9e
```
**What I believed:** the submit path and the completion path were the same path.
**What was actually happening:** …
**How I found it:** which invariant fired, what the trace diff showed, what `sim-replay --break-at` revealed.
**Time lost:** …
**Why a reader cares:** …
-->

## 2. Decisions I'd defend

Design choices with a real alternative that lost. These are the post's substance —
a decision with no rejected alternative is not a decision, it's a default.

- **The user log *is* the Raft log.** Alternative: textbook layering, Raft as a command
  stream feeding a separate state machine. Lost on double-write amplification and a
  separate snapshot mechanism. Consequence: snapshot install degenerates into segment
  shipping, and four follow-on problems appear (consumer visibility, control-record
  offsets, epoch tracking, membership). *→ Does this still feel right in week 9?*
- **Control records occupy real offsets.** Alternative: a side channel. Lost because it
  reintroduces a second durable write path — it un-makes the decision above. Cost:
  consumer offsets are no longer dense, exactly like Kafka's.
- **Thread-per-core.** The honest version — it loses to a thread pool with few partitions,
  skewed load, or unavoidable blocking. *Needs a number from week 7 or it stays an opinion.*
- **Delay the response, not the write.** Ack after the group fsync rather than lying about
  durability. Alternative (VR/TigerBeetle-style learner recovery) documented but not built.
- **Callback event loop, not coroutines.** Ergonomics, not capability. *Did this hold, or
  did the callback code become unreadable by week 6?*
- **Only rewritable fields go in front of the CRC.** Week 2. The rule sounds obvious
  stated that way, which is why the original layout got it wrong — the CRC was placed
  "after the fields it doesn't cover" without anyone checking which fields those had
  ended up being. Alternative considered: keep the specced byte order and define the
  CRC over two disjoint ranges, skipping the CRC field itself. Lost because a checksum
  with a hole in it is a thing every future reader has to re-derive, and the hole is
  exactly where the next unprotected field will get added. Moving one `u32` was cheaper
  than explaining a discontiguous range forever. (§5 for what it would have cost.)
- **The simulated network does not drop, reorder, or duplicate bytes.** Week 3, and it
  contradicts this project's own spec (§14.1) on purpose. That fault menu is written for
  message passing; `io::Network` is a **byte stream**. TCP does not deliver byte 40
  before byte 39 and does not lose byte 41 while delivering byte 42, so a simulator that
  did those things would manufacture failures no production system can produce — and
  every hour spent chasing one would be an hour spent on a bug that cannot happen. What
  the wires model instead is what actually goes wrong on a stream: latency, backpressure,
  short writes, partitions, and connections that die. Message-level loss reappears one
  layer up, where it belongs — a partition kills the connection, the RPC never gets its
  reply, the caller retries into a new one. **A fault model that is more adversarial than
  reality is not more rigorous, it is just wrong in the expensive direction.**
- **The scheduler never draws from the RNG.** Week 3. It is a pure priority queue over
  `(time, id)`. Alternative considered: pick randomly among events ready at the same
  instant, the way some simulators explore interleavings. Lost because the randomness has
  to enter *somewhere physical* to mean anything — it enters as the latency each message
  and each restart draws, plus a rotating order in which nodes are drained. Randomizing
  the queue on top of that explores nothing new and makes every failing trace harder to
  reason about, because the order of two events stops corresponding to anything real.
- **`log-dump` is read-only, even though the recovery code was right there.** Week 2.
  Alternative: open the segment through `storage::Segment` and print what it holds — ten
  lines instead of a hundred. Lost because `Segment::open()` repairs as a side effect of
  opening, so the tool would have destroyed the torn tail it was run to inspect. General
  form: **a diagnostic tool must not share a code path with a repair tool**, however
  convenient the reuse looks.

## 3. Decisions I'd reverse

The most valuable section, and the hardest to write honestly. "What would you do
differently if you started over" needs a real answer — a design you'd undo, not a
humblebrag. Add candidates the moment you feel the friction, not in week 9.

*(empty)*

## 4. Numbers worth quoting

Fill in as measured. Every number carries its conditions or it's noise.

| Number | Value | Conditions | When |
|---|---|---|---|
| Sustained throughput | — | 1 KB records, `acks=quorum`, 3 nodes, hw/kernel/fs | wk 8 |
| p99 append latency | — | at stated offered load, **open-loop** | wk 8 |
| p99.9 append latency | — | and the *reason* it's worse than p99 | wk 8 |
| Failover p50 / p99 | — | ≥ 50 induced failures | wk 8 |
| Ratio vs Kafka | — | identical hardware — and the explanation of the gap | wk 8 |
| Simulated node-hours | 12.5 | 500 seeds × 30 s × 3 nodes, one thread, laptop | wk 3 |
| Distinct bugs found by the simulator | 1 | entry #1, replays from seed 1 | wk 3 |
| Simulation speed | **3 node-hours in ~2 s** | 1 simulated hour of 3 nodes, ~1.08 M trace events, Apple M1, single-threaded, idle machine. NFR-4 asks for 1 cluster-hour per 5 s | wk 3 |
| Optimization delta | — | before/after with a flamegraph behind it | wk 7 |
| fsync-bound append rate | ~100–200 batches/s | Apple M1 laptop, APFS, `F_FULLFSYNC`, fsync **per batch**, 4×64 B records. A durability-latency measurement, not throughput. **Never goes in the README** | wk 2 |
| Batch header size | 52 bytes | fixed; amortized across every record in the batch | wk 2 |
| Seed sweep throughput | 500 seeds in ~0.6 s | 30 simulated seconds each, 3 nodes, no I/O errors | wk 3 |
| Records acked under fault injection | 1,700,730 | 500 seeds, 1,645 crashes survived, zero I1 violations. Was 1,324,798 before the clock-frame fix (§5) — the instrument had been under-reporting its own coverage by 28% | wk 3 |
| Produce→consume vs append-ack gap | — | the replication-latency delta | wk 8 |

## 5. Things that surprised me

Where reality diverged from what the papers or the docs implied. Reader gold —
this is where a post stops sounding like a tutorial.

### Week 1 · The safety check that couldn't fail
The ER-1 grep — the one enforcing that `storage/`/`raft/`/`server/`/`client/` never
touch the OS — passed on its first run. It had to: those directories didn't exist yet,
so it globbed nothing and printed OK. I'd have carried that green check to week 4 and
trusted it. **A check that cannot fail is not a check, and "it passes" is the exact
signal that hides it.** Fixed by making the guard plant a violation against itself
(`--self-test`) before every real run. Generalizes past this project: any invariant
enforcer needs a test that the enforcer fires, not just a test that it's quiet.

### Week 1 · A determinism bug that couldn't reproduce yet
The timer heap ordered on deadline alone, so two timers sharing a deadline fired in
whatever order the heap happened to hold them. Under `CLOCK_MONOTONIC` an exact
nanosecond tie basically never happens, so this would have looked perfect for weeks.
Under **virtual time, ties are the common case** — the simulator advances the clock to
the next deadline, so everything scheduled for that instant is exactly simultaneous.
The bug would have surfaced in week 3 as two runs of one seed disagreeing on the trace
hash, with nothing in the diff pointing at a timer comparator written in week 1.

Worth making the general point in the post: **determinism is not a property you add to
the simulator, it's a property every layer underneath it has to already have.** Week 1
code decided whether week 3 was possible. The fix was six characters and a test.

### Week 2 · The checksum protected the wrong bytes
This is the best story of the week and probably a section of the post on its own.

The batch header ends with a CRC32C, and the spec said — following Kafka, and for
Kafka's actual reason — that the CRC covers everything *after* the CRC field, so that
`base_offset` and `batch_length` can be assigned or rewritten without recomputing it.
Sound reasoning. I wrote the header out in the order the spec listed the fields:

```
base_offset · batch_length · leader_epoch · magic · attributes · crc32c · ...
```

**What I believed:** the two fields outside the CRC were `base_offset` and
`batch_length`, because those are the two the rationale names.

**What was actually true:** *five* fields were outside it. Whatever the rationale said,
the coverage was determined by where the CRC field physically landed, and three more
fields had drifted in front of it — `leader_epoch`, `magic`, and `attributes`.

`attributes` is the one that matters. Bit 1 is the control-batch flag (§12.2): control
batches take real offsets and the fetch path filters them out as internal records. So a
single flipped bit in that byte turns an ordinary data batch into an internal one. The
CRC still verifies, because the CRC never covered that byte. Recovery accepts the batch.
`log-dump` shows it as valid. And a record that was acked to a producer becomes
permanently invisible to every consumer — a violation of I1, *acked writes are never
lost*, with no error raised anywhere in the system and nothing in any log to find later.

**How I found it:** not by inspection. I was writing the corruption property test —
flip a random byte in a random batch, assert recovery stops at or before it — and had to
work out which byte positions the assertion could legally cover. Answering that question
*is* the audit. The test would have failed on any seed that picked byte 17, and I would
have found it anyway on the first run, which is a decent argument for writing the
adversarial test before trusting the format. (A subagent reviewing the same code
independently flagged the identical gap from the constant `kCrcCoveredFrom = 22` alone.)

**The fix:** move `crc32c` to byte 12, immediately after `batch_length`. Same 52-byte
header, four fields reordered, and the rule becomes something you can state and check —
*only fields that must stay rewritable go in front of the CRC.* `project_spec.md` §16.2
was updated too; the spec was wrong, not just the code.

**Why a reader cares:** the bug is invisible to every test that doesn't corrupt bytes on
purpose. Round-trip tests pass. Recovery tests pass. The demo passes. It survives until
a cosmic ray, a bad cable, or a firmware bug flips one bit in production, and then it
manifests as "a record we definitely wrote isn't there" with a valid checksum sitting
next to it. **A checksum's value is entirely determined by its coverage, and coverage is
a property of byte layout, not of intent.**

### Week 2 · `kill -9` cannot tear a write
The week 2 demo is "SIGKILL mid-append, restart, prove nothing acked was lost." I wrote
it, ran it, and it passed five rounds in a row reporting `0 bytes truncated` every time.
I nearly filed that as success.

**What I believed:** killing a process mid-write leaves a half-written batch on disk.

**What is actually true:** it does not, and cannot. `kill -9` destroys a process; it does
not touch the page cache. Every byte the process handed to `pwrite` is still in the
kernel and still visible to the next process that opens the file. A torn tail requires
losing *unflushed* data, which means power loss, a drive that lies about `fsync`, or a
`pwrite` large enough to be interrupted partway. A local `kill -9` loop tests process
restart. It does not test crash recovery, and the two get conflated constantly —
including in a lot of "chaos testing" that amounts to restarting things.

The demo now runs the SIGKILL rounds (which do prove the ack ordering is right) *and* a
second phase that injects a torn tail by hand, with a comment saying plainly that it is
a stand-in. The real fault injection arrives in week 3 with the simulator, where
unflushed-write loss is a first-class fault on virtual time (§14.1) — and this is a
concrete answer to "why build a simulator when you can just kill processes?"

### Week 2 · Every corruption test I wrote corrupted bytes at the wrong time
The read path shipped without a CRC check and I didn't notice, because the test suite
made it look covered. There were four tests about corrupted batches. All four wrote the
bad byte, *then* opened the segment — so all four exercised recovery, and recovery is
exactly the one path that did validate. The read path had a hole shaped precisely like
the gap between "corrupt then open" and "open then corrupt", and the whole suite was on
one side of it.

**What I believed:** recovery validates the segment on open, so the bytes in a segment
are known-good afterwards.

**What is actually true:** they were known-good *at that instant*. Recovery is a
one-time statement about the past. Silent disk corruption (§14.1) is a fault that
happens to data at rest, after every check that would have caught it has already run —
which is the entire reason §17 says "CRC on read" as a separate line item from recovery.
An `ok` status carrying wrong bytes is the worst failure this layer can produce, and it
was one `if` away.

**How I found it:** I didn't. A review pass over the finished week did, by reading the
read path against §17 rather than against the tests. Worth noting for the post — the
week's own property test was excellent at the thing it was pointed at and completely
blind one inch to the left. **Coverage of a code path is not coverage of a fault, and
"when did the fault happen" is a dimension test suites forget to vary.**

The fix costs a CRC pass over every byte served, which makes week 7's zero-copy fetch a
genuine trade-off instead of a free optimization: sending straight from the page cache
means nothing checksums the bytes on the way out. That gets decided with benchmark #8's
numbers, not by default.

### Week 3 · A simulated crash that runs destructors is not a crash
The simulator kills a node by destroying its event loop and its workload — which in C++
means running their destructors. And `storage::Log::~Log()` writes the sparse index on
the way out.

**What I believed:** destroying the objects models the process dying.

**What is actually true:** it models the process *shutting down cleanly*, which is close
to the opposite. A real machine losing power does not get to flush anything; the
simulated one was tidying up on its way down and leaving the disk in a state no crash
produces. Recovery would then be tested against a neater disk than reality ever hands
it — and the sparse index, the one structure whose entire design rests on being
untrustworthy after a crash, would have arrived trustworthy every single time.

The fix is `SimDisk::crash()` leaving the disk **powered off**: every operation fails
until `power_on()`. The dying process still runs its destructors, and every one of them
gets EIO, which is exactly what a process running on a machine that just lost power
would get if it somehow kept executing.

Generalizes past this project, and I suspect it is a common bug in home-grown fault
injectors: **in a garbage-collected or RAII language, "destroy the object" is a graceful
shutdown, not a crash.** If a fault injector is built out of destructors, it is testing
the clean path with extra steps.

### Week 3 · Simulation speed is a correctness feature, not a nicety
`SimDisk::fsync()` originally copied the whole file image to make it durable — one line,
obviously correct, `durable = visible`. It also made fsync cost O(file size), so a log
that fsyncs every append pays quadratically in the segment it is filling. A one-hour run
would have taken minutes instead of two seconds.

The fix is to replay the pending write list onto the durable image instead: O(bytes
written), same result, because `durable + pending == visible` holds by construction.

Worth writing down because the framing matters more than the fix. It looks like a
micro-optimization and it is not: NFR-4 (a simulated cluster-hour per five wall seconds)
exists because **seed count is the correctness budget**. A simulator that is 30× slower
finds 30× fewer bugs in the same CI minute, and the bugs it misses are the rare
interleavings — which are the only ones worth having a simulator for. Slow simulator,
weak evidence.

### Week 3 · Two thirds of the cluster was quietly running at two thirds speed
Every simulated node gets a per-node offset on its monotonic clock, because real machines
boot at different moments and their monotonic clocks share no origin. The point of the
offset is to make cross-node monotonic comparisons visibly wrong, since any code that
does one is broken.

Then the simulator did exactly that comparison itself.

`EventLoop` computes a timer deadline as `clock.monotonic_now() + delay`, which is in
*that node's* frame. The simulator asks every loop for its next deadline, takes the
minimum, and advances the shared clock to it — mixing two coordinate systems. A node with
a 137 ms offset reports every deadline 137 ms later than it really is, so it is never the
earliest, never drives the clock, and only gets to run when some *other* node's timer
happens to drag time past its due point.

**What I believed:** an offset on a monotonic clock is a label. Which it is, in reality —
a machine whose clock starts at an arbitrary epoch still elapses at the same rate as
every other machine. That is exactly why the bug is not one a real deployment can have,
and exactly the kind of thing a simulator can get wrong on its own.

**What was actually happening:** 3 nodes, 30 s, no faults. Node 0 acked 595 batches; nodes
1 and 2 managed 406 and 421. A third of the cluster's intended work simply never happened.

**How I found it:** a review pass, not a test — and nothing would have caught it. The
totals looked plausible. Every seed was green. And critically, **the trace hash matched
perfectly on every run**, because a node running at the wrong rate is still running at
exactly the same wrong rate every time. The I7 canary was doing its job and its job does
not include this: determinism is not correctness, it only makes correctness *checkable*.

**The fix** is one subtraction — `next_node_deadline()` converts each node's deadline out
of its own frame before comparing — plus the test that was missing:
`Simulator.EveryNodeDoesItsShareOfTheWork` asserts the nodes' batch counts stay within
15% of each other. Records acked over 500 seeds went from 1.32 M to 1.70 M.

**Why a reader cares:** it is a bug *in the measuring instrument*, and those are the
worst kind, because every result the instrument produces looks fine. The green sweeps
before the fix were real runs of real code that genuinely survived their faults — they
were just quietly exercising a third less of it than the report claimed. Worth saying in
the post: **a simulator is a piece of software too, and nothing is checking it.**

### Week 3 · The green sweep was the suspicious part
500 seeds, 1.3 million acked records, 1,633 crashes survived, zero violations. That is
the result I wanted and it should have felt good.

It didn't, because `io_error_probability` was still 0 — every fault I had actually
exercised was one week 2 had already been hardened against. The storage code had never
once been run against a disk that *refuses*, only against one that loses. Ten minutes
later it had a CLI flag, and seed 1 failed inside 17 simulated seconds (§1, entry #1).

Third time this pattern has appeared in three weeks: the vacuous ER-1 grep in week 1, the
`kill -9` demo that couldn't tear a write in week 2, and now a fault sweep with a fault
disabled. Same shape every time — **the check passed because it wasn't checking**. I now
think the right instinct on any green result is to ask what it would have taken for it to
be red, and if there isn't a satisfying answer, the result isn't evidence yet. That is
probably the through-line of the post.

### Week 2 · A sparse index that is usually useless is still the right design
The sparse index is never fsynced, which means after a real crash it is typically absent
or stale, and recovery falls back to scanning the active segment from byte zero. My first
reaction was that the index was pulling its weight only on clean restarts — i.e. exactly
when nothing needs recovering.

That reaction was wrong, but usefully so. The index's job on the read path is to bound a
scan for *every fetch*, forever; the recovery path is a bonus it earns only sometimes.
And because recovery scans the active segment, recovery time is bounded by **segment
size**, not log size — which turns "how big should a segment be?" from a throughput
question into a recovery-latency question. That is the real argument for 32 MB over
Kafka's 128 MB default, and it is a better answer than the one in `project_spec.md` §25.

## 6. Things that were harder than expected

With the actual reason, not "it was tricky." Time cost included.

### Week 2 · Writing a demo that could actually fail
The code took an afternoon. Making the demo *prove* something took longer, and three
separate times it looked like it worked when it didn't:

1. It reported `0 bytes truncated` on every run, because `kill -9` can't tear a write
   (§5). Green, and testing the happy path.
2. The shell glob that finds the newest segment picked up the appender's own stderr
   redirect, `append.log`, and `log-dump` faithfully reported a 103-byte file with an
   undecodable header — an extremely convincing false alarm assembled from two
   individually correct pieces.
3. The verifier's record count was computed from the last ack line rather than by
   summing, which is only wrong once a crash has left durable-but-unacked records and
   the ack list has a hole in it — i.e. only on the second run after a crash, which is
   the only run that matters.

The pattern in all three: **the failure mode of a demo is looking like it passed.** Same
lesson as week 1's vacuous ER-1 grep, from a completely different direction, which is
starting to feel like the actual theme of this project rather than a coincidence.

## 7. Things that were easier than expected

Equally worth recording, and usually more surprising. Often the payoff of an early
investment — note *which* investment.

### Week 2 · Storage never touched the OS, and nobody had to try
`storage/` is ~700 lines that read, write, truncate, and fsync files, and it does not
contain a single `#include <fcntl.h>`, `open()`, or `std::filesystem`. The ER-1 guard
passed on the first real run — its first *non-vacuous* run, week 1's story — with no
back-and-forth at all. The one thing that had to be added was `Disk::list_directory()`,
because recovery has to enumerate segment files, and adding it to the interface took
about ten minutes.

The investment being repaid: week 1 spent time on `io/` interfaces with no
implementation to justify them yet, which felt like ceremony at the time. It meant week
2 never faced a "just this once" temptation, because the seam was already the path of
least resistance. **A constraint enforced from day one is free; the same constraint
applied in week 4 is a refactor.**

### Week 3 · The event loop ran on virtual time without being touched
`runtime::EventLoop` was written in week 1 against `io::Clock` and `io::Network`
references, with a docstring claiming the identical loop would run in simulation. Week 3
is where that claim got tested, and the total change required to make three simulated
nodes run their real event loops on virtual time was **one accessor** —
`next_timer_deadline()`, so the simulator can ask each loop when it next wants to wake
and move the clock to the earliest.

Not zero, and the shape of the one thing needed is the interesting part. Under a real
clock a loop decides for itself how long to block; under virtual time it cannot, because
the next thing to happen in the cluster may belong to a different node. So the seam is
exactly "who decides how long to wait" — and `poll()` being the only blocking call in the
whole design (week 1's decision) is what made that a single question with a single
answer.

The `-fno-exceptions`-style discipline of taking every dependency by reference is what
paid here, and it is worth saying plainly in the post: **dependency injection is not a
testing convenience in this project, it is the entire architecture.**

### Week 2 · The property test was the cheapest thing in the week
`test_segment_recovery.cpp` is ~180 lines and covers "cut the log at byte N" for every N
across a hundred randomized logs, plus a bit flip at a random byte of a random batch.
Writing the *question* it had to answer — which bytes may recovery legitimately accept
after corruption? — is what surfaced the CRC-coverage bug (§5). The test found the bug
before it ran once.

## 8. The simulator's greatest hits

Not a separate list — the **`[sim-only]`** entries in §1. Keep a shortlist of entry numbers
here once there are enough to choose from, and note which one is the pitch example.

- **#1 — recovery deleted 320 acked records because one read failed.** Current pitch
  example by default, being the only one, but it is a genuinely good one and may survive
  the competition from weeks 4–5: it is a *storage* bug, found with no Raft, no cluster,
  and no network, by a simulator whose whole job was to be an unreasonable disk. The
  60-second version: *"week 2 had a property test that cut the log at every byte position
  and checked recovery returned exactly the longest valid prefix — it passed, and it was
  always going to pass, because it corrupted the file and never the disk. The fault it
  couldn't express is the one where the bytes are fine and the device misbehaves. Two
  percent read errors, seed 1, seventeen simulated seconds."*

## 9. Quotes, snippets, and images to reuse

Diagrams, a flamegraph before/after, the leader-kill GIF, a trace diff that localized a
nondeterminism bug in one glance, a short and genuinely elegant piece of code. Note the
file path so week 9 isn't an archaeology expedition.

*(empty)*

## 10. Interview answers, drafted from real material

Draft these from what actually happened, not from the spec. Each one needs an anecdote.

- Walk me through leader election. What happens in a split vote?
- The leader acks a write and crashes before replicating. What did the client see, and was that correct?
- Your user log *is* the Raft log. What happens to a consumer that read a record on a follower that then truncates?
- Why thread-per-core? When would a thread pool beat it?
- Why didn't you use an existing Raft library?
- How do you know your Raft is correct? What bug did the simulator find that you'd never have found otherwise?
- Why is p99.9 so much worse than p99?
- What would you do differently if you started over? *(→ §3)*

## 11. Post structure (draft — expect this to change)

1. The hook: a bug that only a simulator could have found, told as a scene
2. What the system is, in one diagram
3. The decision the whole project hangs on — log-as-Raft-log — and its four consequences
4. Building the simulator: virtual time, the seeded scheduler, and why determinism is a
   *tested property* rather than an aspiration
5. The durability section: two fsync knobs, and the `acks=1` vs `quorum+fsync` result
   under the same seed
6. Numbers, with the methodology first and the honest comparison
7. What I'd do differently
