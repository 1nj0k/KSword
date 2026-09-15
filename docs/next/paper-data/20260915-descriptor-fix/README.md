# Descriptor restoration and TinyCore follow-up

Source commit: `767259d8` (full identity and source hashes in
[source-manifest.json](source-manifest.json)). This is a new dataset; the earlier
`20260915-pilot` archive remains unchanged. Explanation and limitations:
[HVM descriptor continuity](../../hvm-descriptor-continuity.md).

## Results

| Check | Result | Raw evidence |
| --- | --- | --- |
| Three rounds of five VMX lifecycle/probe commands | 15 / 15 completed successfully | `verified/descriptor-*.json` |
| GDTR/IDTR hardware verification | 42 / 42 base-and-limit matches; both Windows vCPUs represented | Same records, `descriptorReadbacks` plus raw event pages |
| Kernel GDT dump | CPUs 0 and 1: 88 complete bytes, limit `0x57` | `verified/gdt-query-{0,1}.json` |
| Invalid CPU requests | CPU 2 absent and CPU 64 out of range: both rejected | `verified/gdt-query-{2,64}.json` |
| VMware hard stop while KSword remained resident on two CPUs | 3 / 3; no Windows restart or Hyper-V 18560 event | `teardown/vmware-teardown-*.json` |
| Diagnostic-shell EPT write isolation | A5 → D1 → B2 → A5 | `teardown/write-isolation-20260915T184354Z.json` |
| Normal TinyCore `/init` | Reached the text shell with PID 1 = init | `normal-init/shell-capture.json` |
| EPT write isolation after normal `/init` | A5 → D1 → B2 → A5 | `normal-init/write-isolation-20260915T190653Z.json` |
| Ten-minute Windows/VMware observation | 21 samples; one Windows boot and VMware process identity; two resident CPUs throughout | `normal-init/stability-20260915T185445Z.jsonl` |

The observer ran for 600.8 seconds including capture time. It includes guest boot
and shell instrumentation, not ten minutes of steady-state guest benchmarking.
The final target state has two resident KSword CPUs, an armed EPTP-switch backend,
and no active or retired replacement mapping; the normally booted TinyCore is
still running. See [final-state.json](final-state.json).

## Configuration and scope

- Windows 1 has two vCPUs; the VMware guest has **one vCPU**. Root Windows HVCI
  remained enabled; its configuration is in [host-environment.json](host-environment.json).
- Normal TinyCore 17.1 text boot uses its normal `/init`, without `nosmp`,
  `hpet=disable`, or `rdinit=/bin/sh`. Console/video options and the reserved test
  page remain; the full command line is recorded.
- Its clockevent is `lapic-deadline`. An earlier HPET-present diagnostic boot also
  completed a sleep, but this is not a direct HPET-clockevent regression.
- The EPT test covers four observed bytes within one reserved 4 KiB WB RAM page.
  Active/retired mapping counters are not total allocation or leak counters.
- Repeated teardown success does not establish which patch caused the old
  triple-fault behavior to disappear. Hour-scale crash elimination, multicore
  TinyCore boot, and an intermediate Hyper-V VMM remain unverified.
- Main-program GUI tests were not performed.

## Binary identities

Driver throughout the verified dataset:
`E087E968FF83EBC91C83801AC62692FB3C9AC0C5FA2157F90D3AADC2948DA58C`.

The 15 descriptor cases, three teardown cases, diagnostic-shell isolation and
ten-minute observer used CLI
`987DD0EAE124BC74EED0E76D83DADBC4FF1AAE32B04C2C46159FD8DBAB6A476D`.
After those intervals completed, the GDT query fix was deployed as CLI
`D44B16577F0F4A3334F04908F38BDA4C975FFBD737FB97113D93AFE68363195D`.
The four GDT queries and normal-init isolation use this latter CLI; the driver was unchanged.

The 15 `descriptor-*.json` files directly in this directory are preliminary runs
from before the CPU-attribution and CLI-timestamp correction. They are retained
with their original metadata and excluded from the 42 verified readbacks.

## Recompute and verify

```powershell
python tools/hvm_paper/analyze_descriptor.py docs/next/paper-data/20260915-descriptor-fix
```

The analyzer reads the individual runs and writes [summary.json](summary.json),
including unsuccessful request outcomes and observation limits. Raw records are
not rewritten. [raw-index.json](raw-index.json) records byte sizes and SHA256 for
the raw JSON/JSONL files. Large crash dumps remain local; their identities are
listed in [forensics-manifest.json](forensics-manifest.json), and debugger text
is referenced by the main report.
