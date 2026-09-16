# Prior work and claim boundaries

Author working notes, verified against the primary sources below on 2026-09-15.
These comparisons describe mechanisms and scope. We have not rerun these systems,
so their published performance numbers are not baselines for our experiment.

| Work | Established mechanism | Consequence for this paper |
| --- | --- | --- |
| [SubVirt, IEEE S&P 2006](https://www.microsoft.com/en-us/research/publication/subvirt-implementing-malware-with-virtual-machines/) | A VMM beneath an existing OS, demonstrated with Windows XP and Linux | Inserting a monitor below an OS is established prior art. Cite it; do not claim invention of OS virtualization from below. |
| [Blue Pill, Black Hat 2006, slides 24 and 29–33](https://blackhat.com/presentations/bh-usa-06/BH-US-06-Rutkowska.pdf) | AMD SVM used to place a running OS under a thin monitor; nested virtualization is discussed | Reboot-free OS interposition alone is not novel. Do not repeat the slides' undetectability claims as proven facts. |
| [Turtles, OSDI 2010](https://www.usenix.org/legacy/event/osdi10/tech/full_papers/Ben-Yehuda.pdf) | Nested VMX, execution of unmodified guest hypervisors, and compression of multiple translation tables | VMCS virtualization, exit reflection and composed EPT are established. Our implementation must credit this architecture. |
| [CloudVisor, SOSP 2011](https://sigops.org/s/conferences/sosp/2011/current/abstracts.html) | A small monitor below a commodity VMM separates resource management from protection of hosted VMs | Descendant memory control from a lower monitor is also established. Our prototype trusts the outer Hyper-V/root partition and does not supply CloudVisor's security guarantee. |
| [HyperFresh, VEE 2019](https://huilucs.github.io/pubs/doddamani19fast.pdf) | A pre-existing hyperplexor remaps VM memory to a replacement hypervisor on the same machine | Hypervisor replacement and memory remapping are prior art. Our Windows-only insertion followed by later VMware startup is not equivalent to relocating already-running descendant VMs. |
| [HyperTP, JPDC 2023](https://www.sciencedirect.com/science/article/pii/S074373152300103X) | Live migration and in-place microreboot for Xen/KVM replacement, including heterogeneous directions and application workloads | A topology-change claim must say which old VMM state is transferred. Our experiment does not transfer an existing VMware VMXON/VMCS ownership state. |
| [HyperTurtle, USENIX ATC 2025](https://www.usenix.org/conference/atc25/presentation/zur) | Nested critical paths run as eBPF in the outer hypervisor to reduce world switches | Our monitor requires no such outer-hypervisor extension, but a performance advantage over HyperTurtle has not been measured. |

## Defensible current contribution

The evaluated contribution is a Windows-hosted, late-starting VMX monitor inside
an existing Hyper-V guest, followed by ordinary startup of VMware/TinyCore and
transactional control of one reserved descendant page. The implementation exposes
publication, invalidation, rollback and reclamation as separate observable events.
The evaluation links these events to per-CPU guest readback and identity continuity.

This is a **candidate contribution**, not a claim that the novelty threshold is
already met. A port to a new platform plus a successful memory demonstration can
still be judged insufficient. The follow-up adds a real HTTP server, a fixed-schema
policy consumer, and a guest-side direct-write comparator. Both controls produce
a corrupted policy followed by recovery. EPT intervention retains the original
backing; direct writing requires saved original bytes and guest-side write authority.
This distinguishes control requirements, but does not establish superior speed
or a production reliability benefit. The oracle is a research fixture, and
privileged in-guest instrumentation still selects and pins its page. Marker
readback remains mechanism validation.

## Exact experimental contract

1. Start with a running Windows 1, no VMware VMX process and no existing guest VMX
   owner. Enter/exit the monitor without rebooting Windows. Test this independently.
2. Enter the monitor; then start VMware and TinyCore. While all three remain live,
   substitute and restore the reserved guest page. Track Windows boot identity,
   VMware PID/creation time, TinyCore boot ID and both CPU observers.

The full-tree transition in which an already-running VMware/TinyCore stack is moved
beneath a newly inserted monitor remains unimplemented. The existing CR4.VMXE
ownership check must remain; removing it would not implement state transfer.

## Trust and scope

The outer Hyper-V, Windows root partition and its HVCI are part of the trusted
environment. The guest Windows kernel and operator authorize monitor loading.
The monitor does not acquire VMX root ownership in the Windows 0 root partition.
Normal hypercalls, nested VMX operations and EPT12 accessed/dirty updates still
occur. Only the page-control path avoids intermediate-VMM management APIs, code
hooks and injected components; an absolute claim that Hyper-V/VMware are never
called or that no VMM-owned metadata changes would be false.

## Evaluation needed for a stronger paper

- A production workload beyond the new fixed-schema HTTP policy oracle, including
  a matched performance comparison with alternative fault-injection mechanisms.
- A matched guest baseline on a configuration where the same VMware backend starts
  without the monitor. WHP is a different backend and must be reported separately.
- Independent machines/boots and configurations beyond four Windows/two guest
  vCPUs. Windows 1 has now been upgraded to Pro. The inner Hyper-V/TinyCore
  baseline boots normally, but monitor admission is refused because VMX is not
  exposed to its Windows root partition. This is a measured negative result,
  not successful hot insertion; see [the experiment](../hyperv-descendant-results.md).
- Longer observed runs, page-table changes/root reuse, and failure cases involving
  partial CPU acknowledgement. Request-local omitted-drain tests do not reproduce
  arbitrary hardware failure.

The [EuroSys CFP](https://2027.eurosys.org/cfp.html) evaluates novelty, significance,
correctness and rigorous comparison. Better wording cannot replace these experiments.
