# Intermediate Hyper-V: baseline succeeds, live insertion is refused

## What was tested

On 2026-09-16 UTC, HVM-target was upgraded by its owner to Windows 11 Pro.
We enabled its full Hyper-V role, rebooted the lab target, and created a
generation-1 TinyCore fixture with two vCPUs and 768 MiB fixed memory. The outer
HVM-target retained four vCPUs and 8 GiB. Target VM definitions and ISO hashes
are in the [raw dataset](paper-data/20260916-followup/).

```text
Outer Hyper-V
├── Windows 0 root partition (existing HVCI)
└── HVM-target partition
    └── Inner Hyper-V
        ├── Windows 1 root partition
        │   └── KSword driver: loaded; VMX admission refused
        └── TinyCore 17.1: normal /init, two online CPUs
```

Hyper-V is a hypervisor below its root partition. Installing its role does not
turn it into a VMware-like VMX process inside an otherwise unchanged Windows
kernel. This distinction follows Microsoft's
[architecture description](https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/architecture).
The documented [nested virtualization setting](https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/enable-nested-virtualization)
exposes extensions to a managed child partition; it does not establish that a
driver in the inner root partition can acquire another VMX execution context.

## Observed results

| Observation | Evidence |
| --- | --- |
| Pro role usable | Inner `vmms.exe` 10.0.22621.1 running |
| Normal Linux boot | Kernel `6.18.35-tinycore64`, `INIT=init`, `CPU_ONLINE=0-1` |
| Guest identity | `c6c03f47-16bc-4d0f-9dae-14194fa93ea9`; repeated serial heartbeats |
| Windows identity across first admission attempt | Boot time `2026-09-16T00:53:15.158483Z` unchanged |
| Intermediate worker identity | `vmwp.exe` PID 2824, created `2026-09-16T01:17:54.101516Z`, unchanged |
| Child identity | VM GUID `74eb68a3-c16a-4e98-827a-05d363bdf3f6`; uptime increases from 203.659 to 203.784 s across first attempt |
| VMX availability in Windows 1 | `CPUID.1:ECX=0xFEFA3203`, VMX bit 5 clear |
| KSword admission | Query status 1, `STATUS_NOT_SUPPORTED (0xC00000BB)`; no resources or resident CPUs |
| Start requests | Both `prepare-eptpsw` and `resident-nested-hidehv` refused at the capability query; control IOCTL not submitted |

The first CLI version returned exit 2 with empty output on capability refusal.
The corrected shared engine emits the query result and `controlSubmitted=false`.
Both observations are retained; neither is a successful insertion. The baseline
used the prior ordinarily signed driver (SHA256 in each record), because this
admission boundary precedes the new EPT lease implementation.

## Fixture failures and scope

The first two rebuilt ISO attempts stopped in ISOLINUX with “Could not find
kernel image”. Replaying the original ISO's boot configuration and changing
the ISO9660 level were insufficient. The successful third image rebuilds its
El Torito boot information explicitly. We have not isolated a single responsible
image field. These are retained setup failures, not guest kernel hangs or
successful compatibility runs. The successful image changes the boot menu and
adds an evidence-only `bootlocal.sh` overlay; it retains the original kernel,
root filesystem and normal `/init` path. No `nosmp` or replacement `rdinit` is used.

This establishes **a working Hyper-V/TinyCore baseline and a reproducible
KSword admission limitation**. It does not establish descendant EPT control
under inner Hyper-V, successful hot insertion, support for all Hyper-V builds,
or an impossibility result about every alternative architecture. Forcing VMXON
or spoofing CPUID would not implement ownership transfer.

After collecting the records, we stopped this diskless fixture, retained its
definition, set only the target's `hypervisorlaunchtype` to `off`, and rebooted
HVM-target to run the VMX-dependent performance/lifecycle tests. That reboot is
outside every continuity interval. The inner Hyper-V role remains installed.

After those experiments, normal VMware teardown and KSword stop both succeeded.
We restored inner `hypervisorlaunchtype=auto` and rebooted HVM-target outside
the measurement intervals. `post-cleanup.json` confirms the new boot, running
`vmms`, stopped KSword service and retained two-vCPU TinyCore definition. Both
descendant fixtures are stopped; VMX remains unexposed in this inner root partition.
