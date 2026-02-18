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

### #2 — One cut link made leadership ping-pong for as long as the partition lasted · Week 4

```
seed:       3  (./sim --seed 3 --duration-s 40 --crash-s 0 --partition-s 30)
invariant:  none — and that is the entry. Every one of I1–I6 held throughout.
symptom:    28 elections in 40 simulated seconds. Leadership alternated between nodes 1
            and 2 roughly every 200 ms for the entire 30 s partition, and did not stop
            until it healed. Across a 30-seed sweep with one 60 s link cut: 4228
            elections, 6237 terms.
cause:      node 0 could reach both peers. It was receiving heartbeats from a healthy
            leader and still granted a vote to the node that could not reach that
            leader — deposing a leader it was, at that moment, talking to. The deposed
            node then timed out, campaigned, and deposed the other one back. Textbook
            Raft has no rule against this; the dissertation added one in §4.2.3.
fix:        src/raft/node.cpp — a follower that has heard from its leader inside a full
            election timeout drops a RequestVote without answering *and without adopting
            its term*. The ordering matters: the check has to run before the term rule,
            because adopting the term is what destroys the evidence it needs.
            After: 2 elections on the same seed, 53 across the sweep.
commit:     81ac1c2
```

**What I believed:** that the invariant checker was the thing that would catch Raft bugs,
and that a green sweep across 1000 seeds meant elections worked. I had spent the week
building I6 into the oracle and writing a test that deliberately breaks the metadata
fsync to prove I6 could go red. It does go red. It is a good check. It is also completely
blind to this.

**What was actually happening:** a single cut link between two of three nodes. Not an
isolated node, not a lost majority — the leader still had a quorum the whole time and
could have served every request. The third node, which could see everyone, kept handing
out votes to whichever peer had most recently lost contact with the leader. Both healthy
nodes spent the entire partition deposing each other, and the cluster committed nothing.

The trace made it obvious the moment I looked at the right thing:

```
4257111705   1 PARTITION_START    2      0
4520000000   2 CAMPAIGN           2      0
4521486861   0 VOTE               2      2     <- node 0 votes, while hearing from leader 1
4522786316   2 LEADER             2      0
4553234069   1 STEP_DOWN          2      -
4710000000   1 CAMPAIGN           3      0
4710935982   0 VOTE               3      1     <- and again, the other way
4711984100   1 LEADER             3      0
```

**How I found it:** not from a failing check — nothing failed. I had just added
`elections` and `highest_term` to the result line, mostly so the demo would have
something to print, and ran a sweep across fault configurations to see whether the
numbers looked sensible. Healthy clusters showed 1 election per run. Partitioned ones
showed 128. That ratio was the entire signal, and I only had it because a counter happened
to be printed next to the seed.

**Time lost:** about forty minutes, most of it spent confirming the mechanism rather than
finding it. The fix is nine lines.

**Why a reader cares:** this is the one in the set that is *not* a safety bug, and it is
the one I would talk about first. The whole apparatus — six invariants, a shadow oracle,
a deterministic scheduler, a thousand seeds — is built to answer "did the cluster do
something wrong". It has nothing to say about "did the cluster do anything at all". A
system can satisfy every safety property ever written down by simply never making
progress, and mine did exactly that for thirty seconds at a time while reporting green.

The counter that caught it is not a check and still is not one. What week 5 owes this
entry is a liveness invariant with teeth: over a window in which some majority was
continuously connected, a leader must exist for most of it. That is harder to state than
I1–I6 and it is the one that would have caught this on seed 1.

Two more notes worth keeping. It is **not** `[sim-only]` — `tc netem` on three real hosts
would reproduce it, and I expect it is sitting in a fair number of student Raft
implementations right now. And the fix is incomplete on purpose: the node behind the cut
still campaigns into the void, so its term climbs (81 terms in that 40 s run, against 1
election) and it disrupts the healthy leader once when the partition heals. That half is
Pre-Vote, and it is deferred to week 5 — see §3.

### #3 — A follower answered "I have all of that" when it had agreed to one line · Week 5

```
seed:       11  (./sim --seed 11 --duration-s 3600)
invariant:  I1 — an acked write is never lost, in its replicated form: a leader is
            guaranteed to hold every committed entry (Raft §5.4.1)
symptom:    node 0 won an election with a log ending at 67,238 while the cluster had
            committed through 67,392. 154 committed records held by a leader that did
            not have them.
cause:      a follower accepting an AppendEntries replied with its own *log length*
            instead of the extent it had actually agreed to. Those are different numbers
            whenever a follower holds a divergent tail from a term that lost — it is
            longer than the leader, and a heartbeat agreeing about the entry at `prev`
            says nothing whatsoever about the entries after it. The leader recorded
            `match` past its own log end, counted entries it had never seen toward the
            quorum, and committed on the strength of them.
fix:        src/raft/node.cpp — the response reports what matched (`entry_end` when an
            entry was accepted, `prev + 1` for a bare heartbeat), and the leader clamps
            any `match` to its own log end besides. Two independent bounds, because this
            is arithmetic that decides durability.
commit:     81ac1c2
```

**What I believed:** that "how much of your log do you have?" and "how much of my log do
you have?" were the same question asked from two directions. For most of a run they give
the same answer, which is exactly why it survived every unit test I had written — in the
pure tests a follower is never *ahead* of its leader, because the harness only ever builds
logs by replicating from one.

**What was actually happening:** a follower that had been leader in an earlier term, been
deposed, and kept its uncommitted tail. It is longer than the new leader. The new leader
heartbeats about an index they agree on; the follower says "my log is 300 long"; the
leader writes down `match = 300` for a log that is 250 long, and two nodes reporting 300
is a quorum for entries 250–299 that exist nowhere.

**How I found it:** the checker I had added an hour earlier, on the run that was supposed
to be a speed test. `SimulatesAnHourFastEnoughToBeWorthRunning` runs a full simulated hour
under faults, and the leader-completeness check fired 12.8 seconds in. It reported both
numbers — the leader's log end and the committed end — and 67,238 against 67,392 named the
bug almost by itself: the leader was *behind*, so somebody had told it something false
about how far ahead everyone was.

**Time lost:** about fifteen minutes, nearly all of it deciding whether the checker or the
code was wrong. That is the standard doubt with a new invariant and it is worth budgeting
for: a check written today that fires today is more likely to be miscalibrated than right,
except this time it wasn't.

**Why a reader cares:** the invariant that caught it did not exist in week 4, and it is
the one I added because journal #2 had just finished teaching me that the existing set was
blind to whole categories of failure. I1–I6 as written are about *data*; this one is about
the *argument* — Raft's election safety proof says a leader holds every committed entry,
so checking it directly turns a paper theorem into a runtime assertion. It found a real
bug on the first hour it ran.

It is also a good example of a bug that unit tests structurally cannot find. Every pure
test builds follower logs by replicating from a leader, so no follower is ever longer than
its leader, so the two quantities are never different, so the confusion is invisible. The
state that exposes it — a deposed leader with an uncommitted tail rejoining under a new
one — takes a crash, an election, and a partition to construct. The simulator constructs
thousands of them an hour and did not have to be asked.

<!-- Copy the template below for entry #4. -->


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
- **`raft::Node` holds no `io::` interface except `Random`.** Week 4. ER-1 only asks that
  the OS be reached through an injected seam; this goes further and gives the state
  machine no seam to reach through at all — no clock, no disk, no network. It counts
  ticks and returns a `Ready` describing what it wants done. Alternative considered: pass
  it `io::Disk` and `io::Network` and let it persist and send for itself, which is what
  the Raft paper's pseudocode reads like. Lost on three counts, and the first is the one
  that keeps paying: a full three-node election is now three objects and a message queue,
  so `tests/unit/test_raft_election.cpp` runs 19 election tests in **1 ms** with no event
  loop, no disk, and no time. When the simulator later reported a failure, the algorithm
  was already ruled out. Second, §13's persist-before-you-respond rule becomes structural
  — the node *cannot* send, so there is exactly one function in the repo where the
  ordering could be wrong. Third, timeouts counted in ticks mean the clock-jump fault has
  no route into Raft at all (§17), which is now a test rather than a claim.
- **Two alternating slots in `raft.state`, not write-and-rename.** Week 4. The file is
  rewritten on every term change and every vote, so a torn write there is routine, not
  exotic — and a node that cannot read its own vote is the amnesiac that elects two
  leaders. Alternative: write a temp file, fsync, rename. Standard and correct on a real
  filesystem, but it leans on rename atomicity and directory-fsync semantics, which the
  simulated disk would then have to model faithfully or the test would be testing the
  model. Two 32-byte slots with a sequence number need nothing but `pwrite` and `fsync`,
  both of which are already modelled honestly. A torn write damages only the half being
  written; load takes the newest half that still passes its CRC.
- **Pre-Vote deferred to week 5, with the number written down.** Week 4, and it is a
  deferral rather than a rejection. The §4.2.3 lease rule (bug journal #2) stopped the
  livelock — 28 elections down to 2 on the seed that showed it — but a node behind a cut
  link still campaigns into the void, so its term climbs (81 terms against 1 election in a
  40 s run) and it disrupts the healthy leader once when the partition heals. Pre-Vote
  fixes that half by not incrementing the term until the candidate knows it could win.
  Deferred because the livelock was the availability bug and the inflation costs a single
  election on heal, and because week 5's critical path is replication. **The point of
  writing it here is that "we knew and chose" and "we didn't notice" look identical in a
  codebase six months later.**
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
| Simulated node-hours | 50.0 | 1000 seeds × 60 s × 3 nodes, one thread, laptop, **9 s wall clock** | wk 4 |
| Distinct bugs found by the simulator | 3 | entries #1, #2 and #3 — replay from seeds 1, 3 and 11 | wk 5 |
| acks=quorum+fsync vs acks=1 | **1,882 kept vs 18 lost** | seed 2, 60 simulated s, a crash every 4 s. Same seed, one knob (§13.2) | wk 5 |
| Longest leaderless stretch (I8) | 0.182 s healthy · 2.156 s under crashes | 3 nodes, 120 simulated s; crashes every 8 s in the second. No safety invariant can see this number | wk 5 |
| Replication under fault injection | 1.16 M records committed | 500 seeds x 60 s x 3 nodes, 3,591 crashes survived, zero violations | wk 5 |
| Election churn through one cut link | **28 → 2** | seed 3, 40 s, one 30 s symmetric link cut. Sweep of 30 seeds with a 60 s cut: 4228 → 53 | wk 4 |
| Term inflation still unfixed | 81 terms / 1 election | same seed and cut. The node behind the cut campaigns into the void; Pre-Vote is the fix, deferred (§2) | wk 4 |
| Raft election unit tests | 19 tests in **1 ms** | no event loop, no disk, no network — the payoff of a pure state machine (§2) | wk 4 |
| `raft.state` write | 32 B + fsync | one slot of two, per term change or vote. Not batched, not tunable (§13) | wk 4 |
| Simulation speed | **3 node-hours in ~2 s** | 1 simulated hour of 3 nodes, ~1.08 M trace events, Apple M1, single-threaded, idle machine. NFR-4 asks for 1 cluster-hour per 5 s | wk 3 |
| Optimization delta | — | before/after with a flamegraph behind it | wk 7 |
| fsync-bound append rate | ~100–200 batches/s | Apple M1 laptop, APFS, `F_FULLFSYNC`, fsync **per batch**, 4×64 B records. A durability-latency measurement, not throughput. **Never goes in the README** | wk 2 |
| Batch header size | 52 bytes | fixed; amortized across every record in the batch | wk 2 |
| Seed sweep throughput | 500 seeds in ~0.6 s | 30 simulated seconds each, 3 nodes, no I/O errors | wk 3 |
| Records acked under fault injection | 1,700,730 | 500 seeds, 1,645 crashes survived, zero I1 violations. Was 1,324,798 before the clock-frame fix (§5) — the instrument had been under-reporting its own coverage by 28% | wk 3 |
| Produce→consume vs append-ack gap | — | the replication-latency delta | wk 8 |
| Failover time (NFR-3) | **p50 178 ms · p99 489 ms · p99.9 657 ms** | 200 induced leader failures over 10 seeds, 3 nodes, crash every 5 s. Simulated, so virtual time: election + campaign + vote round trip, excluding process restart. Bound is 900 ms (3× election timeout) — **PASS** | wk 8 |
| Saturation, local | ~1,228 records/s ≈ 1.2 MB/s | `BENCH_LOCAL` — 3 brokers on one M1 laptop, APFS, `F_FULLFSYNC`, 1 KB records, 16/batch, acks=quorum+fsync. **Never goes in the README** | wk 8 |
| Append-ack latency, local | p50 8.5 ms · p99 12.6 ms @ 1,000 rec/s | same run, ~81% of saturation. p50 ≈ **one F_FULLFSYNC** — the number that exposed the missing group commit | wk 8 |
| Latency above the knee | p50 1,474 ms @ 2,000 rec/s offered | same run. The queue *is* the latency past saturation, which is what open-loop measurement is for — a closed-loop producer would have reported none of this | wk 8 |

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

### Week 8 · The benchmark found a feature that was never built

§13.1 is one of the decisions I was proudest of in week 0. The question is how to get
throughput out of a system that must fsync before acking, and the answer written down is:
**delay the response, not the write** — append to the page cache immediately, respond after
the next group fsync, amortize one device flush across many in-flight appends. The spec
even says why the alternative is dishonest.

It is not implemented. `Broker::propose()` calls `log_->fsync()` per batch, one flush per
entry, and it has done since the day the driver was written. I read that line dozens of
times across three weeks without registering that it was the thing §13.1 exists to avoid.

What made it visible was a number. p50 append-ack latency came out at **8.5 ms**, and 8.5 ms
is not a number a program produces — it is a number a *device* produces. It is one
`F_FULLFSYNC` on APFS, sitting under every single append, exactly as the spec predicted it
would if you did the naive thing.

The saturation sweep says the rest: throughput flattens at ~1,228 records/s, which is the
fsync rate times the batch size and nothing to do with CPU, network, or the log format.
Every line of careful work on batch encoding and zero-allocation buffers is invisible
behind one synchronous flush per entry.

Three things worth keeping from this.

**A design document is not an implementation, and the gap is invisible from the inside.**
The spec said group commit, the code said fsync-per-batch, both were in front of me the
whole time, and no test could tell the difference — group commit is a *throughput*
decision, and every correctness test I have would pass identically either way. It took a
measurement to see it, which is the entire argument for §19 existing at all.

**It is the ideal candidate for §19 #5** ("one before/after optimization with a percentage
and a flamegraph behind it"). I now have the before number, measured, with conditions
attached, and a specced design to implement against. That is a much better story than
optimizing something I picked because it looked slow.

**The overload behaviour is its own finding.** Past saturation the cluster does not just
get slower: at 4× the knee it starts burning Raft terms, because the event loop is so busy
fsyncing that it starves its own tick timer and followers time out. A broker that
fails an election because it is *working too hard* is a nice illustration of why §15's
thread-per-core split matters, and it argues for taking the fsync off the loop that owns
consensus timing.

### Week 8 · Four weeks of "cluster" tests, and not one crossed a machine

Setting up the GCP run, I went looking for how to point three brokers at three internal
IPs and found that there is no how. `RealNetwork::listen()` binds `INADDR_LOOPBACK`. It
has since week 1, when it was obviously correct and nobody was ever going to run this
anywhere else.

The wrong belief was not "the code binds all interfaces" — I had no belief about it at
all. It was one level up: **I thought "distributed" was a property the system had, when
it was a property nothing had ever tested.** CI runs a three-node cluster. The week 6 demo
kills a real leader with `kill -9` and watches a real failover. `run_all.sh` sweeps a real
three-broker cluster under real load. Every one of those is three processes on one machine
talking to `127.0.0.1`, and every one of them passes, because a loopback cluster is a
*working* cluster. It elects, replicates, commits, recovers. It is missing exactly one
thing, and that thing is the network.

This is the same shape as week 5's "the check passed because it wasn't checking", and it
is getting to be a habit worth naming. The failure mode is never a test that goes red for
the wrong reason. It is a test that goes green while quietly not exercising the axis you
believe it covers — recovery that never read a corrupt page, an invariant set with no
liveness property in it, a fault injector whose faults had stopped firing, and now a
distributed systems project with no network in the loop. In every case the artifact
*looked* more complete than it was, and in every case the thing that exposed it was
changing the conditions rather than adding an assertion.

The fix is ten lines and uninteresting. What I want to keep is the reason it stayed hidden
for four weeks: **loopback is not a degraded network, it is a different one.** A latency of
~20 µs and a loss rate of zero means every timing constant in Raft has enormous slack, so
the parts of the design that exist *because* the network is slow and unreliable — the
election timeout range, the rejection backoff, the pipelining of AppendEntries — were
never under any pressure. The simulator did cover that axis, with delays and partitions and
reordering, which is why the algorithm is in good shape. But the simulator drives
`io/sim/`. The claim "the real binary is the same object" was proven at the seam in week 6,
and I let it quietly extend to "and therefore the real binary has been tested over a real
network", which does not follow and was not true.

Cheap lesson, and only cheap because it surfaced before the numbers were published rather
than after.

### Week 6 · The bet paid, and the payment was a diff

Three weeks of this project rested on one claim: everything above the `io/` seam runs
identically in production and in the simulator. Week 6 is where that either holds or is
revealed as a story I had been telling myself.

The honest test is not "does the real binary work". It is **which object does the
simulator test?** Weeks 3–5 grew the driver — the persist-then-send code, the part §13 is
about — inside `sim/workload.cpp`. If `server/` had been written fresh alongside it, every
correctness claim in the README would have been about a *sibling* of the shipped code, and
the sentence "1000 seeds, 2.3 M records, 7,205 crashes" would have been quietly false in a
way no test could detect.

So the work was a move, not a write: about 300 lines went from `sim/` to `server::Broker`,
and `sim::NodeWorkload` went from *being* the driver to *owning* one. The simulator watches
through an observer interface where every hook is an observation and none is a decision —
a null observer changes nothing about what a broker does, which is what makes the claim
checkable rather than rhetorical.

**The measurement that mattered:** the 1000-seed sweep produced byte-identical totals
before and after the move — 2,320,262 records, 7,205 crashes, same numbers. That is the
whole proof. Then the same object, with `io/real/` constructed instead of `io/sim/`, ran
three processes over loopback and survived a `kill -9` on the leader.

Two things I did not expect.

**The first real-hardware run worked.** Not "worked after a day of debugging" — the first
three-process cluster elected a leader and committed 1,216 records. After three weeks of
being told by a simulator that this code was correct, that should not be surprising, and it
was anyway, because the usual experience of moving code to real I/O is a week of
socket-level surprises. There were none, and the reason is that the socket-level surprises
had all been simulated already: partial writes, backpressure, connections dying mid-request.

**The extraction introduced exactly one bug, and the checker caught it.** The I2 check
sampled the oracle's commit index *after* calling propose — which is fine when the ack
happens later, and wrong under `acks=1`, where the append commits itself before returning.
Every single append then looked like an offset regression. Caught on the first run, named
precisely, on a seed. A refactor of the most safety-critical code in the project cost about
ninety seconds of debugging, and that — rather than any individual bug — is what the
simulator bought.

### Week 5 · The test that caught me sabotaging the tests

Replication made a simulated second several times more expensive, and the two simulation
binaries went from seconds to **29 and 36 minutes** under UBSan. So I shortened the runs
for instrumented builds — the same trick that worked in week 4 for seed counts.

`Simulator.FaultsActuallyFire` immediately went red with four zeroes: no crashes, no
partitions, no resets, no unflushed bytes lost. I had shortened the runs to six simulated
seconds and the default crash interval is twenty. Every one of those runs had been a
completely fault-free run wearing the label of a fault test.

That test exists for exactly one reason — week 1's lesson that a check which cannot fail is
worse than no check — and it caught the person who wrote it, doing the thing it was written
to prevent, from a direction I did not anticipate. **Not by changing the check, but by
changing something else entirely and letting the check go quiet.** The other simulation
tests would have kept passing indefinitely, on runs where nothing ever went wrong.

The fix is a small design principle worth keeping: a test about faults should *configure*
faults that fire promptly, not inherit a global duration and hope it is long enough. It
now sets a two-second crash interval and does not care how long the run is. The one
exception is unflushed-write loss, which genuinely needs runway — the window between a
write and its fsync got narrow in week 5, because now everything is fsynced — so that one
keeps its full twenty seconds in every build, with a comment saying why.

Final numbers: 40 minutes to 82 seconds across all four presets, with every code path still
covered. The seed breadth is what got cut, and seed breadth is a logic argument that a
deterministic simulation makes build-independent.

### Week 5 · The invariant that had to be conditional to be true

I8 took three attempts to state, and the failures are more interesting than the result.

*"A leader always exists"* — false, and obviously so: every election has a moment with no
leader, and that moment is the algorithm working.

*"A leader exists at least 95% of the time"* — false in a way that took longer to see. A
cluster partitioned so that no majority can communicate is **supposed** to have no leader,
indefinitely, and a checker that flags that is demanding the system violate Raft. Any
threshold I picked was really a statement about my fault injector's settings, not about
the system.

What is actually true is conditional: *once a majority can communicate again, a leader
appears within a bounded time*. And that is awkward to check, because "a majority can
communicate" is a fact about the fault injector, not about the cluster — so the invariant
needs to reach into the adversary to evaluate itself.

The shape I settled on: the simulator measures the longest leaderless stretch and reports
it; the *test* supplies the bound, because the test is the thing that knows what faults it
injected. A healthy cluster gets 1 second and uses 0.182. A cluster being crashed every 8
seconds gets 15 and uses 2.156.

That is weaker than a real liveness proof and I want to be honest about it in the post:
it is a smoke alarm, not a theorem. But it is the difference between noticing bug journal
#2 in ten minutes and not noticing it for a week, and the general lesson is one I had not
appreciated before writing it — **safety invariants are absolute and liveness invariants
are always relative to an assumption about the environment.** That is why every paper
states liveness "under a synchrony assumption" and why I had never had to think about it
until I tried to write the check down.

### Week 5 · Two number lines that looked like one

The cost of §12's central decision, and the bug I would most expect a reader to have
already made themselves.

Raft's log is a sequence of **entries**. Kafka's log is a sequence of **records** with
offsets. This project's whole premise is that they are the same log — so an index and an
offset are the same number, right up until a batch holds more than one record. A batch of
two at offset 12 means the next entry starts at 14, not 13.

I wrote `log_end_ = index + 1`. Every subsequent index then pointed into the middle of a
batch, where no read can start and no append can land, so the leader silently failed to
send *anything* — not even heartbeats, because a heartbeat to a follower that needs an
entry is an entry-carrying message. The symptom was **89 elections in 30 simulated
seconds with no faults injected at all**, which is a spectacular way for an off-by-one to
present.

Two things worth keeping. First, the fix is not "add one correctly" but "carry the extent
explicitly" — `entry_end` is on the wire because only the *sender's storage layer* knows
how many records a batch holds, and the consensus layer must be told. Second, the same
confusion had a second head: the rejection hint. Classic Raft backs off one index per
round trip, which here lands mid-batch, so the follower has to answer with a place it
knows is a boundary. §12.2 says offsets are "monotonic, not dense" and I had written that
sentence myself in week 0; knowing it and having it in my fingers turned out to be
different things.

### Week 4 · Six invariants, and not one of them is about the system working

This is bug journal #2 stated as a principle, and it is the thing I most want to say in
the post. I1–I6 are all *safety* properties: nothing is lost, nothing is reordered,
nothing is overwritten, no two leaders in a term. Every one of them is satisfied, trivially
and permanently, by a cluster that does nothing at all.

So a thousand seeds ran green while a single cut link had two healthy nodes deposing each
other every 200 ms, committing nothing, for as long as the partition lasted. The trace hash
was stable. The oracle was happy. The system was safe, available on paper — there was
always a leader! — and useless.

What caught it was a counter I had added to make the demo output look better. Healthy runs
showed one election; partitioned runs showed 128. Nobody wrote a check; somebody looked at
a number that happened to be printed next to the seed.

The general form is worth more than the bug: **a safety-only invariant set cannot
distinguish a working system from a stopped one**, and the liveness properties that could
are genuinely harder to state, because every honest version needs a hypothesis about what
the network was doing. "A leader exists" is false during any legitimate election. The one
worth building is conditional — over a window in which some majority stayed connected,
a leader existed for most of it — and it is week 5's debt.

### Week 4 · The bug where every layer behaves exactly as documented

Found by reading, not by the simulator, so it is not in §1 — same category as week 2's CRC
placement. It is the best example so far of a fault with no faulty component.

`take_ready()` handed the driver the pending hard state and cleared the "needs persisting"
flag in the same breath. The driver persists, then sends; if the fsync fails it drops the
whole batch and sends nothing. Read either half on its own and both are correct. Read them
together:

1. The node grants a vote in term 5. Flag set, response queued.
2. The driver takes the batch. **Flag cleared** — nothing has been written yet.
3. The fsync fails. The driver drops the response. Correct so far: nobody was told.
4. The vote now exists only in memory, and nothing anywhere records that it owes a write.
5. The candidate retries. The node grants again — correctly; the vote is already committed
   to that candidate. But `votedFor` is *already* set, so nothing changed, so no flag, so
   the driver has nothing to persist and sends the grant straight out.
6. A vote that never reached the platter has now been announced. Crash, and it votes again
   in term 5.

Nothing misbehaved. The state machine followed Raft, the driver followed §13, and the
composition violated it. The fix is to make the debt **sticky**: `take_ready()` no longer
clears anything and the driver calls `hard_state_persisted()` once the fsync returns.

What makes this worth writing down is the asymmetry that picks the design. Both versions
can be got wrong; they just fail in different directions. Forget to acknowledge, and the
node rewrites 32 bytes it had already written. Clear it optimistically, and you get the
above. **When a default has to be picked for a rule that cannot be enforced, pick the one
whose failure is a wasted write rather than a lost promise** — and it is worth noticing
that I had written the optimistic version first without pausing, because it reads more
naturally.

A second, smaller thing fell out of the test I wrote for it: granting a repeat vote to the
same candidate was marking the state dirty even though nothing had changed, so a candidate
retrying every election timeout was costing an fsync per retry — a stall on the durability
path at exactly the moment the cluster is already in trouble. Now only an actual change
owes a write.

### Week 4 · The state machine that could not be told what time it is

`raft::Node` counts ticks. It has no clock, and the driver's tick timer runs on the
monotonic clock, so the simulator's wall-clock-jump fault has no path into Raft whatsoever
— I wrote `ClockJumpsDoNotDisturbElections` expecting to have to fix something and it
passed on the first run, jumping the wall clock by a minute every five seconds.

That is `project_spec.md` §17's line "correctness never depends on wall clock" turning out
to be free rather than expensive, and only because of a decision made for a different
reason. Ticks were chosen for testability — you can drive 400 of them in a loop with no
time source at all — and immunity to clock skew fell out of it. Worth noting that the
usual framing has this backwards: clock-independence is normally presented as discipline
you maintain, when here it is a property of a type that has no way to express a deadline.

### Week 4 · Writing the amnesia test made the fsync argument for me

I wrote `ANodeThatForgetsItsVoteElectsASecondLeaderInTheSameTerm` before the driver
existed: construct a node, have it grant a vote in term 5, then construct a *second* node
with a default `HardState` — a restart that lost the file — and offer it a different
candidate in the same term. It votes. Of course it votes; nothing is wrong with it.

Twelve lines, no infrastructure, and it turns "fsync before responding" from a rule I was
following because §13 says so into something I could see. The message the amnesiac sends
is perfectly well-formed. Nothing downstream can detect it. The only place that bug can be
prevented is the one function that decides whether to wait for the platter — which is why
the driver has exactly one such function and the state machine cannot send at all.

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

### Week 4 · The sanitizer presets are `-O0`, and I had not priced that in

`test_raft_cluster` runs in 2.2 s under `dev` and I left it at 40 seeds without thinking
about it. Under UBSan the same binary was still going after four minutes. Not hung —
the simulation is deterministic, so it was doing exactly the same work — just doing it in
a build that is `Debug` *and* instrumented. `dev` is `RelWithDebInfo`; `asan`, `ubsan` and
`tsan` are all `Debug`. That is roughly a hundredfold on a compute-bound simulator, and I
had been reading week 3's "TSan takes 248 s" as a TSan fact rather than an `-O0` fact.

The fix was to stop conflating two things a sweep does. Breadth over seeds is a **logic**
argument, and the logic is build-independent because the simulation is deterministic —
seed 37 under ASan proves nothing seed 37 under `dev` did not. What the sanitizers
actually need is **coverage of a code path**, and the fortieth seed walks the same lines
as the first. So instrumented builds now run a handful of seeds and the optimized build
runs the full sweep, with the one seed the demo depends on pinned and run everywhere
(`tests/support/build_mode.h`).

Worth noting what that pinning caught immediately. I had written the test's fault config
by hand rather than copying the demo's command, zeroing partitions and clock jumps "to
isolate the variable" — and seed 4 stopped violating, because its violation depends on the
partitions being there. **A seed is only evidence together with its entire configuration**,
which is obvious stated plainly and was not obvious while typing.

Then TSAN timed out on both simulation tests — 600 s each, a thirty-minute job — and the
fix turned out to be a rule this project wrote down in week 1 and never implemented. ER-5
says TSAN runs on the real runtime only, because the simulator is single-threaded by
construction and there is no data race in it to find. The `tsan` preset's *display name*
literally reads "real runtime only; useless in sim". Nothing enforced it. So TSAN had been
grinding through the simulator since week 3, finding nothing, and week 3 had absorbed that
as a 248-second run and raised a timeout rather than asking why the number was large.

One label and one preset filter later: **1828 s with two timeouts → 7.6 s, 18/18.**

The lesson is not about TSAN. A rule that exists only in prose is a rule that is being
violated somewhere and nobody knows it — which is precisely the argument for ER-1's grep,
made by the one engineering rule in this project that had no mechanical check. The
timeout raise in week 3 is the tell: a limit was hit, the limit got raised, and the
question of what the work was *for* never came up.

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

- **#2 — one cut link made leadership ping-pong for the whole partition.** Not
  `[sim-only]` — `tc netem` reproduces it — but it is the better *story*, and it may end
  up being the one the post is built around, because it is the only one so far where the
  entire correctness apparatus was working perfectly and reporting green. The 60-second
  version: *"a thousand seeds green, six invariants checked after every event, and the
  cluster was spending thirty seconds at a time electing a new leader every two hundred
  milliseconds and committing nothing. All six invariants are safety properties. A cluster
  that does nothing satisfies every one of them. What caught it was a counter I'd added to
  make the demo output look nicer — healthy runs showed one election, partitioned runs
  showed a hundred and twenty-eight."*

  The two make a good pair, and the pairing is probably the shape of the post: #1 is what
  the simulator is *for*, #2 is what it still could not see.

## 9. Quotes, snippets, and images to reuse

Diagrams, a flamegraph before/after, the leader-kill GIF, a trace diff that localized a
nondeterminism bug in one glance, a short and genuinely elegant piece of code. Note the
file path so week 9 isn't an archaeology expedition.

*(empty)*

## 10. Interview answers, drafted from real material

Draft these from what actually happened, not from the spec. Each one needs an anecdote.

- Walk me through leader election. What happens in a split vote? *(Have the anecdote:
  `WithoutJitterLockstepNodesSplitTheVoteForever` — set the jitter to zero and three
  lockstep nodes campaign, self-vote, reject each other, and climb the term forever
  without ever electing anyone. It is the jitter, not the timeout, that resolves it.)*
- How do you test consensus? *(The pure state machine: no clock, no disk, no socket. 19
  election tests in 3 ms. Then the same code under a fault simulator. The point is that
  "the algorithm is wrong" and "the driver is wrong" became separately answerable.)*
- Where does the fsync go, and why there? *(§13 — and the twelve-line test that builds an
  amnesiac node and watches it hand out a second vote in the same term. Then the harder
  version: the composition bug in §5, where the state machine and the driver were each
  correct and the pair was not.)*
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
