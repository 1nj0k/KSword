# Paper evidence and correctness worklist

This worklist follows the review of 2026-09-15. A passing command is not a
passing experiment. Every runtime result must identify the tested binaries,
configuration, time domain, continuity checks and retained raw record.

| Item | Required result | Current state |
| --- | --- | --- |
| Running descendant insertion | Already-running Windows, VMM and descendant remain live across insertion | Deferred by user on 2026-09-15; current start rejects pre-existing CR4.VMXE ownership, and native VMCS import is not implemented. No confirmed attribution to a VMware defect |
| Novelty and application | Prior-work comparison and a useful measured application | Real HTTP server plus policy oracle: 3 complete EPT trials / 4 attempts and 3 direct-write comparator trials. Control requirements differ; production value and comparative performance remain unproven. Measured holder-alive field was constant; fresh PFN evidence is separately qualified |
| Transition timing and performance | Internal phases, per-CPU timing, matched binary baselines | New Pro boot: 72 attribution runs, 24 TCP observations, 74,145 valid responses; insertion body/rendezvous medians 2.5953 ms / 111.6 us. CPUID remains 8.76x, RTT +30.7%. Two-read optimization gives only 0.99% median change with overlapping ranges; main overhead unresolved |
| Architectural state and stability | State invariants, monitored runs and teardown coverage | Current 4x2 boot, EPT control and normal teardown pass. Owner-exit regression and ten-minute soak belong to previous versions; no new soak requested. Broader state/timer regressions remain open |
| EPT concurrency and failure recovery | Multicore effects, deterministic failures, reclamation and stale translations | ABI v3: 3 complete two-CPU cycles, 15 injected-control trials, 3 rejections, balanced resource ledgers; sampled source-path guard unit-tested. Actual source-table mutation, hardware faults, identical-bits reuse and application-page atomicity remain open |
| Compatibility | Separate intermediate-VMM and guest-OS configurations | VMware/TinyCore EPT control passes. Pro/inner Hyper-V/TinyCore normal boot passes; KSword admission is refused with VMX unavailable in the inner root. Hyper-V hot insertion remains unsupported. Win10 LTSC deferred |
| Manuscript and artifact | Version-consistent claims, reproducible analysis, anonymized submission copy | Complete [arXiv preprint draft](arxiv/README.md), with author identity, LaTeX source, PDF and evidence-derived tables; author review pending. Final anonymous evidence reruns analyses and checks redaction, manifests and ZIP integrity. A full anonymous buildable source artifact remains open |

## 2026-09-16 follow-up details

- **Intermediate Hyper-V:** Pro now runs a normal-init, two-vCPU TinyCore child.
  Live KSword admission is reproducibly refused in the inner Windows root
  partition (`CPUID.VMX=0`, `STATUS_NOT_SUPPORTED`). No successful insertion or
  descendant EPT control is claimed. The earlier Home edition blocker is resolved;
  the VMX ownership/capability blocker remains.
- **Application comparison:** the new HTTP policy oracle distinguishes successful
  transport from a valid business response. EPT remapping and guest-side direct
  writing are compared by fault/recovery effect and required authority. The
  comparison does not establish production value or a latency advantage.
- **Attribution:** 72 same-binary benchmark runs include 60 measured samples and
  12 warmups. Skipping two CPUID telemetry VMREADs changes the median by -0.99%,
  with overlapping ranges. Remaining Windows CPUID cost is 8.76x and loopback RTT
  is +30.7%. These new-boot values must not be pooled with the earlier ones.
- **Application disruption:** 24 paced TCP observations retain 74,145 valid
  responses. Measured insertion's largest command-overlapping completion gap is
  6.23 ms; the gap includes scheduler/pacing/socket effects and is not an exact
  pause. Driver-body/rendezvous medians are 2.5953 ms / 111.6 us (five insertions).
- **Lifecycle:** ABI v3 captures and revalidates the source EPT path. Three two-CPU
  remap cycles, 15 injected-control trials and three invalid requests pass on the
  new binary. Hardware source-path mutation, identical-bits ABA/reboot reuse,
  arbitrary application-page atomicity and the current-build soak remain open.

See [the follow-up report](hvm-followup-results.md),
[Hyper-V admission evidence](hyperv-descendant-results.md) and
[the lease contract](ept-lease-contract.md). Previous results below remain
version-specific history, not acceptance of every newer binary.

Target resets belong outside measured transitions. The outer Windows root's
HVCI remains enabled. Existing unrelated GUI/theme changes are out of scope.
For this conversation, driver builds disable variant signing explicitly with
`KswordArkSkipAutoVariantSign=true` and `KswordArkSkipAutoTestSign=true`.
The lab uses the existing ordinary test certificate for target loading.

The requested two-step sequence is verified: enter/leave KSword with VMware absent;
then enter KSword, start VMware/TinyCore, and remap/restore its reserved page.
This does not resolve the separate already-running-descendant insertion item.
Results, incomplete records and remaining limits are in
[the current 4x2 evidence report](hvm-4x2-results.md) and
[the versioned dataset](paper-data/20260915-4x2/README.md).
The [older 2x2 report](hvm-paper-gap-closure.md) remains historical evidence.
