# Paper evidence and correctness worklist

This worklist follows the review of 2026-09-15. A passing command is not a
passing experiment. Every runtime result must identify the tested binaries,
configuration, time domain, continuity checks and retained raw record.

| Item | Required result | Current state |
| --- | --- | --- |
| Running descendant insertion | Already-running Windows, VMM and descendant remain live across insertion | Deferred by user on 2026-09-15; current start rejects pre-existing CR4.VMXE ownership, and native VMCS import is not implemented. No confirmed attribution to a VMware defect |
| Novelty and application | Prior-work comparison and a useful measured application | Primary-source comparison plus three complete HTTP data-fault/recovery trials added; comparative advantage over alternative instrumentation remains unmeasured |
| Transition timing and performance | Internal phase timestamps, per-CPU timing, matched current-binary baselines | Current 4x2 driver: 10 Windows start/stop pairs, 168 Windows benchmark processes, old/new nested implementation comparison; memcpy time down 94.53%, CPUID down 29.05% in TinyCore. Windows CPUID remains 9.05x and RTT +34.82%; exact application pause, matched nested no-monitor baseline, nested persistent disk and multiple machines remain open |
| Architectural state and stability | State invariants, timer regression, monitored long runs and teardown coverage | Current four-CPU Windows/two-CPU TinyCore boots normally; current owner-exit/reclamation passes. Existing ten-minute soak belongs to older 2x2 only. No new soak requested; timer-specific and broader state regressions remain open |
| EPT concurrency and failure recovery | Multicore effects, deterministic failures, reclamation and stale-translation checks | Current 4x2: 20/20 complete two-CPU cycles, 15/15 complete injected-control trials, 3/3 rejections and write isolation. Process-exit lease added/tested. Historical incomplete traces retained separately; hardware faults, same-process reboot/root reuse and general workload-page atomicity remain open |
| Compatibility | Separate intermediate-VMM and guest-OS configurations | VMware/TinyCore normal two-vCPU boot and EPT control verified; Windows Home lacks full Hyper-V role, so intermediate Hyper-V remains unverified; Win10 LTSC deferred by user |
| Manuscript and artifact | Version-consistent claims, reproducible analysis, anonymized submission copy | Markdown abstract updated for 4x2 and HTTP application; anonymous evidence generator includes integrity checks, private provenance and offline reproduction. Full anonymous buildable source artifact and full paper remain separate work |

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
