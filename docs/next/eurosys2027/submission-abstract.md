## Abstract

Nested virtualization makes live memory control a coordination problem across
independently managed address translations. We present a VMX monitor that can
be inserted beneath a running Windows guest of Hyper-V and subsequently host
an unmodified VMware instance and its Linux guest. The monitor composes extended
page tables (EPT) and redirects a selected 4 KiB guest-physical page to private
backing while retaining the original translation permissions. Publication and
removal coordinate invalidation across resident CPUs; failed commits roll back,
and backing is reclaimed only after every resident CPU acknowledges invalidation.
The page-control path uses no intermediate-VMM management API, code hook, or
injected component.

```text
Hyper-V
├── Root partition
│   └── Windows 0 · HVCI active
│
└── Guest partition
    └── VMX monitor                     ← inserted into running Windows
        ├── Windows 1 · 2 vCPUs          ← same Windows boot
        │   └── VMware                  ← started after insertion
        │       └── TinyCore · 2 vCPUs   ← normal init and shell
        │           └── Reserved 4 KiB page
        │
        └── Composed EPT
            ├── Original ──▶ Original backing
            ├── Remap ─────▶ Private backing · guest writes isolated here
            └── Restore ───▶ Original backing · original content retained
```

## Evidence

- **Continuity:** Ten insertion–removal pairs preserve Windows boot and key
  process identities with VMware absent. The median internal insertion body
  takes 1.257 ms; its CPU rendezvous spans 90.55 µs. This interval is an internal
  synchronization measurement, not a direct measurement of application pause.
- **Cross-layer control:** After insertion, a normally booted two-vCPU TinyCore
  guest completes 20 remap–restore readback cycles, including ten with both guest
  CPUs above 99% busy. Each CPU observes the expected full-page content. Windows,
  VMware and guest identities remain stable. Sixteen cycles have complete phase
  traces; four earlier traces are incomplete. Idle map and restore bodies have
  medians of 132.9 µs and 115.85 µs.
- **Recovery:** A separate write-isolation experiment recovers the original
  full page after writes to replacement backing. Fifty injected-control trials
  satisfy their state and resource assertions; 44 also have complete trace and
  dual-CPU readback evidence. All incomplete records are retained. A ten-minute
  monitored run preserves identities and reports no outstanding page-control
  allocations at its sampling points.
- **Cost:** Matched Windows-only microbenchmarks measure 36.9% additional TCP
  loopback round-trip time and a 10.30× CPUID cost, showing substantial workload
  sensitivity.

These single-machine results establish reversible control of a reserved page
in the evaluated stack. Windows insertion and descendant-page operations are
separate experiments. Insertion beneath an already-running intermediate VMM,
Hyper-V as that intermediate VMM, and matched descendant performance remain
unverified.
