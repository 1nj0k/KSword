## Abstract

Nested virtualization makes live memory control a coordination problem across independently managed address translations. We present a VMX monitor that can be inserted beneath a running Windows guest of Hyper-V and subsequently host an unmodified VMware instance and its Linux guest. The monitor composes extended page tables (EPT) to redirect a selected 4 KiB guest-physical page to private backing while preserving translation permissions. Publication and removal coordinate invalidation across resident CPUs; failed commits roll back, and backing remains retained until invalidation completes. A lease binds the intermediate VMM's process lifetime and the sampled source translation path. Observed ownership or translation changes revoke the lease. The page-control path requires no intermediate-VMM management API, code hook, or injected component.

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

- **Continuity and lifecycle.** On four Windows vCPUs and two TinyCore vCPUs, the current lease implementation completes three two-CPU remap–restore cycles, fifteen injected-control trials and three clean invalid-request rejections, with complete traces and balanced page-control allocation ledgers. A separate paced TCP experiment retains 74,145 valid responses across 24 insertion, removal and sham observations without changing Windows boot or key process identities.
- **Application value and comparison.** A real HTTP server and fixed-schema policy consumer observe valid pricing, injected corruption, and recovery. Three EPT trials complete with at least 90 validated responses; one observer timeout is retained as incomplete. Three guest-side direct-write trials reproduce the business effect. The comparison distinguishes authority and recovery requirements: EPT retains the original backing, while direct writing needs saved original bytes. It does not establish a performance advantage or eliminate privileged guest setup.
- **Measured cost.** In five measured insertions with descendant VMMs absent, median driver-body time is 2.60 ms and the rendezvous envelope is 111.6 µs. The largest command-overlapping TCP completion gap is 6.23 ms; pacing and scheduling contribute to that gap, so it is not an exact pause. A same-binary comparison still shows 8.76× CPUID cost and 30.7% additional loopback RTT against disabled residency. Removing two diagnostic VMREADs changes the CPUID median by only 0.99%, with overlapping ranges.

These single-machine results establish reversible control of the evaluated pages, with substantial workload-dependent costs. A normally booted Hyper-V/TinyCore intermediate-VMM configuration is also tested, but KSword admission is refused because VMX is unavailable in its Windows root partition. Insertion beneath an already-running VMM, identical-translation reuse detection, and arbitrary page-update atomicity remain open. The retained ten-minute stability observation belongs to an earlier two-by-two build. An anonymous evidence package preserves per-run raw records, incomplete trials, and reproducible analyses.
