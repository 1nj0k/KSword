# Four Windows vCPUs and two TinyCore vCPUs

Collected 2026-09-15 to 2026-09-16 UTC on one physical machine. The directory
name identifies the experiment start date. Each measured operation retains its
own JSON record; serial files are raw observations, not screenshots.

## Configuration and provenance

- `topology-change.json`: configuration reboot changing the target from two to
  four vCPUs, outside every measured live insertion interval.
- `outer-environment.json`, `platform-details.json`, `smp/environment.json`:
  physical CPU/BIOS, Hyper-V binaries and settings, Windows and VMware versions.
- `build-provenance.json`: exact tested driver/source identities and build scope.
  The driver is ordinarily test signed; variant signing was disabled.
- `application-v2-payload.json`: later ISO revision changing only the HTTP
  observer script; performance measurements use the earlier recorded payload.
- `completion-cleanup.json`: Windows target still running with four vCPUs,
  VMware configured for two, VMware and residency stopped, mapping resources
  reclaimed, outer root HVCI still active.

## Independent evidence groups

| Directory | Purpose and denominator |
| --- | --- |
| `before` | Previous driver, 4 Windows vCPUs, 168 Windows benchmark process runs, 10 insertion/removal pairs |
| `after` | Current driver, same 4-vCPU configuration, same workload/repetition counts; primary current Windows comparison |
| `after-host-build-confounded` | Earlier current-driver collection overlapping a root-host build; retained and excluded from primary performance results |
| `before-nested` / `after-nested` | Same static benchmark binary, 4×2 configuration, CPU 0 affinity, one warmup and three measured repetitions per six workloads; different driver and guest boots |
| `smp` | 20 complete two-CPU remap/restore trials, 15 complete injected-control trials, three clean request rejections, full-page write-isolation observations |
| `application` | HTTP observer development, including setup failure, no-observer timeout, one truncated final PFN pair, two content-only trials with startup identity snapshots, and active-mapping owner-exit test |
| `application-v2` | Three complete HTTP trials, 30 responses per trial; server creation time and guest boot ID re-read per response; live holder and fresh PFN required |

The first HTTP observer version does not prove per-response process identity.
Its raw data is retained and is not pooled with the three schema-2 trials.
The setup and timeout failures remain part of the failure inventory. An analyzer
finishing successfully means it generated classifications, not that every run passed.

## Recompute

From the repository root, with Python 3.10 or newer:

```powershell
python tools/hvm_paper/analyze.py docs/next/paper-data/20260915-4x2/before
python tools/hvm_paper/analyze.py docs/next/paper-data/20260915-4x2/after
python tools/hvm_paper/transition_metrics.py docs/next/paper-data/20260915-4x2/before
python tools/hvm_paper/transition_metrics.py docs/next/paper-data/20260915-4x2/after
python tools/hvm_paper/validate.py docs/next/paper-data/20260915-4x2/after --windows-only
python tools/hvm_paper/analyze_nested_bench.py docs/next/paper-data/20260915-4x2
python tools/hvm_paper/analyze_smp.py docs/next/paper-data/20260915-4x2/smp
python tools/hvm_paper/analyze_http_page.py docs/next/paper-data/20260915-4x2/application
python tools/hvm_paper/analyze_http_page.py docs/next/paper-data/20260915-4x2/application-v2
python tools/hvm_paper/analyze_lifecycle.py docs/next/paper-data/20260915-4x2
```

## Measurement boundaries

- Windows overhead compares residency off/pre → on → off/post, with VMware
  absent. Seven measured repetitions per block follow one warmup. Off samples
  are pooled (n=14); on has n=7. Bootstrap intervals are descriptive within this
  machine/boot, not cross-machine confidence intervals.
- Nested performance compares old versus new implementations, not a matched
  no-monitor baseline. A process timeout includes initialization; it cannot be
  substituted for the timed workload latency. The old pointer-chase trials all
  timed out; the new trials completed.
- Network tests use TCP loopback. Windows disk tests use a file on the virtual
  disk. No new persistent-disk benchmark inside TinyCore is claimed.
- Per-CPU QPC intervals overlap. Do not add their durations or subtract guest
  uptime from Windows UTC/QPC. The rendezvous envelope is not application pause.
- GPA is in the TinyCore address space; backing PAs are in Windows 1's visible
  physical-address domain. Hyper-V still controls final machine translation.
- The HTTP page is selected by guest root pagemap/mlock instrumentation.
  Neither arbitrary page atomicity nor a guest-agent-free technique is claimed.
- Page-control allocation counters cover rule/replacement objects only. Owner
  process exit is tested; in-process guest reboot/snapshot/root reuse is not.
- There is no new stability soak. The separate `20260915-gap-closure/smp`
  ten-minute record belongs to an older 2×2 build. Do not relabel it as 4×2.
- Already-running full-chain insertion, intermediate Hyper-V, Windows 10 LTSC,
  other CPUs/Windows builds and overnight stability are not established here.

Raw author data contains identifiable paths and machine labels. Submission must
use the separately generated anonymous archive and its own SHA256 manifest.
