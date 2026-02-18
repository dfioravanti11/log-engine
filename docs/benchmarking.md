# Benchmarking on GCP

The README's throughput and latency numbers come from three GCE VMs, one broker each.
This is the exact procedure. Total cost is about **$1/hour**; the whole run takes ~30
minutes including VM creation.

## Why three VMs and not one

`bench/run_all.sh` has four sections. Only one of them needs real hardware:

| Section | What it measures | Where it runs |
|---|---|---|
| 1. simulator totals | seeds, node-hours, records, crashes | virtual time — **any machine** |
| 2. failover time (NFR-3) | election + campaign + vote round trip | virtual time — **any machine** |
| 3. throughput / latency (NFR-1, NFR-2) | the real thing | **3 VMs** |
| 4. durability trade-off (§13.2) | acks=1 loses data, quorum doesn't | virtual time — **any machine** |

Sections 1, 2 and 4 run on the simulator's virtual clock, so they produce byte-identical
output on a laptop and on a GCE VM. Running them on GCP would be theatre.

Section 3 is different, and one VM is not good enough for it. The obvious objection to
three brokers on one machine is CPU and disk contention. The objection that matters more
is that **replication happens over loopback** — a quorum round trip costing ~20 µs
flatters precisely the thing this project is about. One broker per VM, same zone, is the
cheapest topology that isn't lying.

## 0. Push the code

The VMs get the code by cloning, so the branch has to be on the remote:

```bash
git push origin main
```

If the repo is private, either make it public or skip ahead to
[private repos](#appendix-private-repos).

## 1. Set the variables

```bash
export PROJECT=your-gcp-project-id
export ZONE=europe-west4-a          # any zone; all three VMs must share it
export PREFIX=logengine-bench
export REPO=https://github.com/dfioravanti11/log-engine.git

gcloud config set project "$PROJECT"
```

All three VMs must be in **one zone**. Intra-zone RTT is ~0.1–0.3 ms; cross-zone is
~1 ms and cross-region is tens. A replicated log's commit latency is a network round
trip plus a flush, so the zone is part of the measurement, not a deployment detail.

## 2. Create the three VMs

```bash
for n in 0 1 2; do
  gcloud compute instances create "$PREFIX-$n" \
    --zone="$ZONE" \
    --machine-type=n2-standard-4 \
    --image-family=ubuntu-2404-lts \
    --image-project=ubuntu-os-cloud \
    --boot-disk-type=pd-ssd \
    --boot-disk-size=500GB &
done
wait
```

**The disk size is not about space — it is about IOPS.** Persistent Disk performance on
GCP is provisioned per gigabyte: pd-ssd gives 30 write IOPS and ~0.48 MB/s per GB. Since
§13.1's group commit is not implemented, every append costs one synchronous fsync, so
what the benchmark needs from the disk is flushes per second:

| Size | Write IOPS | Verdict |
|---|---|---|
| 100 GB | 3,000 | too close to the working point |
| **150 GB** | **4,500** | comfortable, and 3× fits under a default `SSD_TOTAL_GB` quota |
| 500 GB | 15,000 | generous headroom |

This design reaches roughly 1,000–2,000 flushes/sec — a network round trip plus a flush
per entry — so 150 GB clears the requirement about 3×. Below that the sweep starts
measuring a billing tier and reporting it as a throughput result.

`n2-standard-4` is four vCPUs. A broker is one event loop on one core, so this is
headroom rather than parallelism: it means the measurement is not competing with sshd
and the kernel.

### Quota, on a newer account

Three of these in one region needs `CPUS` 12, `N2_CPUS` 12, `SSD_TOTAL_GB` 1500,
`IN_USE_ADDRESSES` 3, and `CPUS_ALL_REGIONS` 12. The vCPU numbers are usually inside the
defaults; **`SSD_TOTAL_GB` often defaults to 500 per region**, which three 500 GB disks
blow through. Either request an increase (IAM & Admin → Quotas) or use
`--boot-disk-size=150GB`, which totals 450 GB and needs no increase. Check with:

```bash
gcloud compute regions describe "$ZONE_REGION" \
  --format="table(quotas.metric,quotas.limit,quotas.usage)"
```

## 3. Check the firewall

The default VPC ships with `default-allow-internal`, which permits all TCP between
instances on the internal range. Verify:

```bash
gcloud compute firewall-rules list --filter="name~'default-allow-internal'"
```

If it is missing (a custom VPC), open the broker port between the VMs:

```bash
gcloud compute firewall-rules create logengine-internal \
  --allow=tcp:9500 --source-ranges=10.128.0.0/9
```

Brokers bind loopback by default. `bench/run_gcp.sh` passes `--bind-all`, which is the
only reason the port is reachable at all — so the firewall and the flag both have to be
right before a cluster forms.

## 4. Provision each VM

Installs the toolchain, clones, builds the three binaries, creates `/var/lib/logengine`,
and prints the hardware conditions. Idempotent — re-run it after any `git push`.

```bash
for n in 0 1 2; do
  gcloud compute ssh "$PREFIX-$n" --zone="$ZONE" --quiet --command "
    sudo apt-get update -qq && sudo apt-get install -y -qq git &&
    (git clone -q $REPO ~/log-engine 2>/dev/null || true) &&
    ~/log-engine/scripts/gcp_setup.sh $REPO main
  " &
done
wait
```

Roughly 90 seconds on a cold VM. The last thing it prints is the conditions block —
kernel, CPU, filesystem, mount options. **It exits non-zero if the data directory is
tmpfs**, because fsync there is a no-op and every durability number would be fiction.

## 5. Run the sweep

From your laptop:

```bash
ZONE=$ZONE ./bench/run_gcp.sh | tee bench-gcp.txt
```

Defaults: 1024 B records, 16 per batch, `acks=quorum+fsync`, 30 s per rate, offered
rates 1000 → 32000 records/s. Override any of them:

```bash
ZONE=$ZONE RATES="2000 8000 32000" DURATION=60 ./bench/run_gcp.sh
```

The three nodes rendezvous on a shared wall-clock second before starting, so `gcloud
ssh` connection skew — seconds, and variable — stays out of the measurement window.

Read the `terms` column first. It should be `1` at every rate. A higher term means the
cluster was electing *under load*, which is a throughput result too: the event loop
starving its own Raft ticks. The rate where `terms` climbs and `achieved` falls away
from `offered` is saturation, and NFR-2's p99 is the one at 70% of it.

### 5b. Without gcloud — three browser SSH tabs

`run_gcp.sh` drives the machines over `gcloud compute ssh`, so it needs the CLI. If you
only have Console SSH, `bench/run_node.sh` is the same sweep with the orchestration
removed. Every node computes the same schedule from the same `--start` epoch, so the
three pastes do not need to be simultaneous — only done before the first rate is due.

Get the internal IPs from the VM list, pick an epoch two minutes out, and paste one
command per tab:

```bash
echo $(( $(date +%s) + 120 ))          # run once; give ALL THREE the same number

# tab 0
~/log-engine/bench/run_node.sh --id 0 --peers 1@IP1:9500,2@IP2:9500 --start EPOCH
# tab 1
~/log-engine/bench/run_node.sh --id 1 --peers 0@IP0:9500,2@IP2:9500 --start EPOCH
# tab 2
~/log-engine/bench/run_node.sh --id 2 --peers 0@IP0:9500,1@IP1:9500 --start EPOCH
```

Only the leader has samples, so exactly one tab prints a row per rate and the other two
say `(follower this round)`. Leadership can move between rates. Collect the rows from all
three tabs; together they are the table.

## 6. Run the hardware-independent sections

Anywhere — your laptop is fine, and that is the point:

```bash
RATES= ./bench/run_all.sh | tee bench-sim.txt
```

`RATES=` skips section 3, which `run_gcp.sh` just did properly.

## 7. Delete the VMs

```bash
gcloud compute instances delete "$PREFIX-0" "$PREFIX-1" "$PREFIX-2" --zone="$ZONE" --quiet
```

Do this. Three n2-standard-4 with 500 GB pd-ssd is ~$1/hour whether or not anything is
running on them.

## What goes in the README

Both files, with their conditions blocks attached. §19's rule is that a number ships
with hardware, kernel, filesystem, mount options, offered load and the exact command —
`gcp_conditions.sh` prints the first four and the benchmark prints the last two, so
between them nothing is unstated. A p99 without an offered load is not a weak
measurement; it is not a measurement.

State plainly what is still missing on this hardware:

- **§13.1 group commit is specified and not implemented.** Every append costs one
  synchronous fsync, so the p50 is one device flush plus one network round trip. This is
  the "before" of §19 #5's before/after; the "after" does not exist yet.
- **No Kafka comparison** (§19 #7). Same VMs and same offered load, or it is a press
  release.
- **No consumer numbers** (§19 #8, #10) — both need the client library.

## Appendix: private repos

`git clone` over HTTPS needs credentials the VM does not have. Ship a tarball instead of
step 0 and 4's clone:

```bash
git archive --format=tar.gz -o /tmp/le.tgz HEAD
for n in 0 1 2; do
  gcloud compute scp /tmp/le.tgz "$PREFIX-$n":~/ --zone="$ZONE" &
done
wait
for n in 0 1 2; do
  gcloud compute ssh "$PREFIX-$n" --zone="$ZONE" --quiet --command "
    mkdir -p ~/log-engine && tar xzf ~/le.tgz -C ~/log-engine &&
    sudo apt-get update -qq &&
    sudo apt-get install -y -qq cmake ninja-build build-essential &&
    cd ~/log-engine && cmake --preset dev >/dev/null &&
    cmake --build --preset dev -j\$(nproc) --target logengine tool_sim bench_failover &&
    sudo mkdir -p /var/lib/logengine && sudo chown \$(id -un) /var/lib/logengine &&
    ./scripts/gcp_conditions.sh" &
done
wait
```

`git archive HEAD` ships committed content only — uncommitted work in your tree will not
be in the build, which is usually what you want for a number you intend to publish.
