# HVM paper evidence tools

These collectors measure the shared HVM command engine. The `metrics` command
adds an independently versioned, read-only driver snapshot and appears in the
main program's existing command catalog. The checked-in pilot is documented in
[the evidence report](../../docs/next/hvm-paper-evidence.md).

## Recompute the archived pilot

```powershell
python tools/hvm_paper/analyze.py docs/next/paper-data/20260915-pilot
python tools/hvm_paper/plot.py docs/next/paper-data/20260915-pilot
python tools/hvm_paper/validate.py docs/next/paper-data/20260915-pilot
```

Analysis uses only the Python standard library; plotting needs Matplotlib.
`validate.py` checks this pilot's expected sample counts and preserved unsuccessful
trials. It is a dataset-integrity check, not a general runtime test or correctness proof.
`analyze.py` leaves raw records untouched and writes derived tables separately.

## Build workloads in the repository

```powershell
New-Item -ItemType Directory tools/hvm_paper/bin -Force | Out-Null
& $gcc -std=c11 -O2 -Wall -Wextra -static tools/hvm_paper/microbench.c -o tools/hvm_paper/bin/microbench.exe -lws2_32
& $gcc -std=c11 -O2 -Wall -Wextra -static tools/hvm_paper/transition_probe.c -o tools/hvm_paper/bin/transition_probe.exe
```

Use the installed MinGW compiler as `$gcc`; the pilot used GCC 13.1.0. Do not replace
binaries midway through matched blocks. Record their SHA256 and source hash.
Linux compilation is supported with `gcc -std=c11 -O2 -Wall -Wextra -static -pthread`,
but the pilot Linux binary was never transferred to or executed in TinyCore.

`microbench` exposes seven workloads: integer xorshift, CPUID, memcpy, pointer chase,
TCP loopback bulk, TCP loopback RTT, and a new 64 MiB direct-I/O scratch file. It pins
to CPU 0 and reports affinity outcome. Disk files use create-new semantics and only
the owned scratch path is removed. Loopback measurements are not physical-network
measurements. Outer virtualization and storage caching remain part of the result.

## Live collection

Run host scripts from the repository in an authorized Hyper-V lab. Supply its
`PSCredential` as `$credential`. The pilot target name is `KSword-HVM-Target`.
The existing HVM CLI is `C:\ksword\hvm_ctl.exe`; workload binaries belong in
`C:\ksword\paper`. Driver identity, EPTP switch readiness and residency must be
checked before each experiment. Target reset belongs outside measured transitions.

| Script | Execution surface | Purpose |
| --- | --- | --- |
| `Capture-Windows.ps1` | root; or PowerShell Direct with `-ArgumentList $true` | Environment, build, identities, HVM state, raw serial |
| `Watch-Stability.ps1` | root | One persistent session; 30-second samples for a 600-second monitor |
| `Run-WindowsBenchmarks.ps1` | PowerShell Direct `-FilePath` | Warmup + per-run durable JSON; captures identities before/after |
| `Run-WindowsAB.ps1` | root | Matched off/pre → on → off/post with VMware absent |
| `Run-Transition.ps1` | PowerShell Direct `-FilePath` | One start/stop; optional busy QPC observer |
| `Run-NestedPageCycles.ps1` | root | Repeated live mapping or invalid-request rejection |
| `Run-WriteIsolation.ps1` | root | One A5 → D1 → B2 → A5 effect record, using existing VNC helper |
| `Test-DescriptorContinuity.ps1` | root | One-shot, nested-probe, self-virtualization and stop hardware GDTR/IDTR readbacks per CPU |
| `Test-VmwareTeardown.ps1` | root | Deliberately stop the lab VMware VM while KSword remains resident; save pre-state before the potentially failing operation |

Example matched collection:

```powershell
& tools/hvm_paper/Run-WindowsAB.ps1 -Credential $credential -OutputDirectory $newRunDirectory
```

The existing target driver must be loaded, residency stopped, and VMware absent.
The script prepares and self-tests only if needed; otherwise it validates readiness.
Every measured block uses the same binary and Windows boot. All remote scripts run
through PowerShell Direct `-FilePath`. Results are copied back in `finally`.

The busy QPC probe is deliberately a different condition. It drives both CPUs and
records only their 16 largest execution gaps. In this condition the pilot observed
severe command latency and one 30-second command timeout, with stopped residency later observed.
The remaining six planned busy pairs were not run. For a single ordinary command:

```powershell
Invoke-Command -Session $session -FilePath tools/hvm_paper/Run-Transition.ps1 `
  -ArgumentList 'C:\ksword\paper','resident-nested-hidehv',1,$true
```

The final boolean disables the busy observer; this is now the default in
`Run-WindowsAB.ps1`. Command durations include process creation and CLI work.
New records also retain `metricsRaw`: driver QPC boundaries for resource setup,
EPT construction, IPI rendezvous, and each CPU's capture/control selection, VMCS
programming and entry continuation. Run `transition_metrics.py <directory>` to
validate ordering, completeness, command identity and CPU identity before deriving
intervals. The rendezvous envelope bounds disruption; it is not exact application
pause, and simultaneous entry is not claimed. INVEPT counters count actual wrapper
calls, whereas resource counters cover only nested-page objects/replacement pages.
The collector does not kill an in-flight HVM command at its timeout. Inspect saved
state and remaining command processes before further mutations.

## Nested page preconditions and guest observation

Use only the diagnostic page explicitly reserved at GPA `0x07000000`, seeded with
A5 in the guest. Query the current EPT12 root; do not reuse a root across guest boots.
The cycle script requires exactly two resident KSword CPUs, one known nested
root, and no active/retired mapping. The older pilot uses one guest CPU; the newer
SMP experiment pins one observer to each of two guest CPUs and checks all 4096 bytes
with MD5 as well as a four-byte read. See `analyze_smp.py` and its per-run tables.

Before cycles, start the bounded guest reader through the existing VNC helper:

```sh
(for i in $(seq 1 180); do echo paper-page; cat /proc/uptime; dd if=/dev/mem bs=4 skip=29360128 count=1 2>/dev/null | od -An -tx4; sleep 1; done) >/dev/ttyS0 2>&1 &
```

Then run `Run-NestedPageCycles.ps1`; it writes each run before mutation, records
map/query/remove, and retains the corresponding raw serial slice. Analysis requires
the distinct fill value followed by A5; an accepted IOCTL alone is insufficient.
`-RejectionsOnly` separately tests an above-52-bit GPA, unaligned GPA, and unknown root.
`-Condition guest-cpu-load` is a label: a bounded guest CPU workload must actually be
running and its observation retained. The pilot logs include `/proc/stat` brackets.

`Run-WriteIsolation.ps1` uses the repository's existing lab-specific VNC helper.
Its four-byte read/write proof does not validate all 4096 bytes or arbitrary workload
pages. Remove mappings and verify both active/retired clear before fixture teardown.

## Known evidence limits

- UTC, Windows QPC and guest uptime are distinct time domains. No direct subtraction
  across uncalibrated clocks. Physical-page addresses also retain their layer scope.
- Active/retired slots are not total outstanding-allocation or leak counters.
- Historical pilot counters omit early-reflected/handled L2 exits. The metrics-v1
  build counts every resident dispatch entry and actual INVEPT calls separately.
  Histogram 50/53 still counts nested instruction exits. Do not merge these scopes.
- `Test-NestedPageFaults.ps1` exercises five request-local fault modes, retains
  each attempted run, and paginates the event ring. `analyze_smp.py` requires fresh
  readback on both CPUs, complete operation traces and balanced allocation ledgers.
  The initial short-observation records remain incomplete instead of being dropped.
- Orchestration uses Hyper-V PowerShell Direct and VMware VNC. The EPT control path
  itself is separate; EPT12 A/D metadata writeback also prevents an absolute claim
  that no VMM-owned memory is ever modified.
- Win10 LTSC and further GUI tests were skipped at the user's request.

`Watch-Stability.ps1` schema 2 adds deployed-driver identity, actual INVEPT/resource
counters and bounded serial reads. `stability_v2.py` rejects stale per-CPU observers,
changed boot/process identity, reset counters and incomplete requested durations.
The default is a ten-minute sample; set `-Seconds 3600` for an hour. Monitoring itself
adds work, and total Windows pool usage cannot isolate a driver memory leak.

`Run-LinuxBenchmarks.sh` and `Serve-LinuxBench.ps1` are prepared transfer/collection
helpers. The native Linux executable compiled successfully, but the attempted
transfer was blocked by automatic approval review; it was **not executed** in
TinyCore. This is not additional Linux performance evidence.
