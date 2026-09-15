# Paper evidence and correctness worklist

This worklist follows the review of 2026-09-15. A passing command is not a
passing experiment. Every runtime result must identify the tested binaries,
configuration, time domain, continuity checks and retained raw record.

| Item | Required result | Current state |
| --- | --- | --- |
| Running descendant insertion | Already-running Windows, VMM and descendant remain live across insertion | Deferred by user on 2026-09-15; current start rejects pre-existing CR4.VMXE ownership, and native VMCS import is not implemented. No confirmed attribution to a VMware defect |
| Novelty and application | Prior-work comparison and a useful measured application | Primary-source comparison added; useful application and measured advantage remain open |
| Transition timing and performance | Internal phase timestamps, per-CPU timing, matched current-binary baselines | Current driver: 10 start/stop pairs, 168 Windows benchmark processes, per-CPU timestamps complete; exact application pause, matched nested baseline and multiple machines remain open |
| Architectural state and stability | State invariants, timer regression, monitored long runs and teardown coverage | Descriptor fix verified; normal two-vCPU TinyCore boot, 10-minute monitored run and one current-driver VMware teardown pass; timer-specific, hour-scale and broader state regressions remain open |
| EPT concurrency and failure recovery | Multicore effects, deterministic failures, reclamation and stale-translation checks | 20 dual-CPU effect closures, 16 complete traces; 50 injected-control trials, 44 complete evidence sets and 6 retained incomplete sets; hardware failures, root reuse and general workload-page atomicity remain open |
| Compatibility | Separate intermediate-VMM and guest-OS configurations | VMware/TinyCore normal two-vCPU boot and EPT control verified; Windows Home lacks full Hyper-V role, so intermediate Hyper-V remains unverified; Win10 LTSC deferred by user |
| Manuscript and artifact | Version-consistent claims, reproducible analysis, anonymized submission copy | Current Markdown abstract and author evidence archive updated; full paper and anonymized artifact remain open |

Target resets belong outside measured transitions. The outer Windows root's
HVCI remains enabled. Existing unrelated GUI/theme changes are out of scope.
For this conversation, driver builds disable variant signing explicitly with
`KswordArkSkipAutoVariantSign=true` and `KswordArkSkipAutoTestSign=true`.
The lab uses the existing ordinary test certificate for target loading.

The requested two-step sequence is verified: enter/leave KSword with VMware absent;
then enter KSword, start VMware/TinyCore, and remap/restore its reserved page.
This does not resolve the separate already-running-descendant insertion item.
Results, incomplete records and remaining limits are in
[the current evidence report](hvm-paper-gap-closure.md) and
[the versioned dataset](paper-data/20260915-gap-closure/README.md).
