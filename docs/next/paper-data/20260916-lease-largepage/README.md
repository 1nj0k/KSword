# Portable lease-walk results: leaves larger than 4 KiB

Regenerate both files from the repository root, with no VM and no driver load:

```sh
tools/hvm_paper/Test-EptLeaseMutation.sh
```

## What is here

`ept-lease-unit.txt` is the output of two suites, each compiling the same header
the driver compiles, against synthetic inputs:

* `tools/hvm_paper/test_ept_lease.c` over `hvm_nested_lease_walk.h` — capturing a
  source translation and detecting drift, including 2 MiB and 1 GiB source leaves.
* `tools/hvm_paper/test_ept_leaf_plan.c` over `hvm_nested_leaf_plan.h` — which
  override regions may be published as a leaf larger than 4 KiB, and which are
  refused. The rule that matters is that a large leaf applies one permission set
  to every page beneath it, so it is admitted only where EPT12's own leaf already
  covers the whole region.

`ept-lease-mutation.txt` measures what those suites can detect. Twenty defects
are injected one at a time into a copy of the relevant header, and each is
required to make its suite fail. The repository copies are never modified. Every
mutation is also run against the suite as committed at `HEAD`, so added coverage
is a count rather than a claim: fourteen of the twenty are not killed by the
committed suites — three because the large-page walker cases are new, eleven
because the planner itself is new.

The harness exits non-zero if any mutation survives.

## Scope, and what these files do not show

The inputs are synthetic. This measures the headers' logic and the suites'
detection power. It is not a hardware result:

* **No large-page override has ever been published on a real machine.** The
  driver path that installs a 2 MiB or 1 GiB shadow leaf, the contiguous aligned
  backing allocation, the copy-on-map of the original region and the per-page
  staging write are all unexercised outside these synthetic tests. They have been
  syntax- and type-checked with the WDK toolchain and nothing more.
* Every page intervention measured for the manuscript targets a 4 KiB page.
* Real source-EPT mutation by a running VMM, adversarial multicore, and actual
  hardware failures are not simulated here.
* A mutation score is a property of this fixed set of nine defects. It is not an
  upper bound on the defects the walker could contain.

The lease contract and its ABA, guest-reboot and same-GPA-reuse limits are in
[`docs/next/ept-lease-contract.md`](../../ept-lease-contract.md).
