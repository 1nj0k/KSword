## Abstract

Nested virtualization makes live memory control a coordination problem across independently managed address translations. We present a VMX monitor that can be inserted beneath a running Windows guest of Hyper-V and subsequently host an unmodified VMware instance and its Linux guest. The monitor composes extended page tables (EPT) to redirect a selected 4 KiB guest-physical page to private backing while preserving the original translation permissions. Publication and removal coordinate invalidation across resident CPUs; failed commits roll back, and backing remains pinned until invalidation completes. A process-lifetime lease revokes a mapping when its intermediate VMM exits. The page-control path requires no intermediate-VMM management API, code hook, or injected component.

```text
Hyper-V
├── Root partition
│   └── Windows 0 · HVCI active
│
└── Guest partition
    └── VMX monitor                         ← live Windows insertion
        ├── Windows 1 · 4 vCPUs              ← same boot during insertion
        │   └── VMware                      ← started after insertion
        │       └── TinyCore · 2 vCPUs
        │           ├── CPU 0 + CPU 1        ← concurrent page observers
        │           └── HTTP server         ← actual file-cache page
        │
        └── Composed EPT
            ├── Original ──▶ Original backing ──▶ Valid JSON response
            ├── Remap ─────▶ Private backing  ──▶ Injected data fault
            └── Restore ──▶ Original backing ──▶ Original response
```

## Evidence

- **Continuity and multicore control.** Ten insertion–removal pairs preserve Windows boot and key process identities with VMware absent. After insertion, a normally booted two-vCPU Linux guest completes 20 remap–restore cycles, including ten under concurrent CPU load. Both guest CPUs observe the expected full-page contents, and all 20 cycles retain complete phase traces. Fifteen injected-control trials and three invalid requests satisfy their recovery or rejection checks; a separate trial verifies write isolation.
- **Application and lifetime.** Three HTTP experiments reversibly replace a resident file-cache page with zero-filled backing and restore the original response, verifying 90 HTTP responses. Server creation time and guest boot ID are re-read for every response, while Windows and VMM identities remain stable. A separate VMM-exit experiment verifies mapping revocation, retained backing, and subsequent reclamation.
- **Cost and optimization.** In the evaluated four-by-two configuration, the revised EPT tracking implementation reduces nested memory-copy time by 94.5% and CPUID time by 29.0% relative to the previous implementation. Ten Windows insertions have a median internal body of 2.80 ms and a CPU rendezvous envelope of 149.5 µs; the latter is not a direct application-pause measurement. Windows-only comparisons against disabled residency still show a 9.05× CPUID cost and 34.8% additional TCP loopback round-trip time. Nested round-trip time also regresses in this small implementation comparison.

These single-machine results establish reversible control of the evaluated reserved and application pages, with substantial workload-dependent costs. They do not establish insertion beneath an already-running intermediate VMM, arbitrary page-update atomicity, or compatibility with Hyper-V as the intermediate VMM. The retained ten-minute stability observation belongs to an earlier two-by-two build. An anonymous evidence package preserves per-run raw records, incomplete trials, and reproducible analyses.
