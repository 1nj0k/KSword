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
  refused, and what re-reading one page of a published region concludes. The
  rule that matters is that a large leaf applies one permission set to every
  page beneath it, so it is admitted only where EPT12's own leaf already covers
  the whole region, or where every source entry under the region was read and
  agreed — and in that second case the agreement is rechecked a page at a time
  for as long as the region is published.

`ept-lease-mutation.txt` measures what those suites can detect. Thirty-six
defects are injected one at a time into a copy of the relevant header, and each
is required to make its suite fail. The repository copies are never modified.
Every mutation is also run against the suite as committed at `HEAD`, so added
coverage is a count rather than a claim: eight of the thirty-six are not killed
by the committed suites, and all eight are the recheck cases, because the
recheck is what this revision added. Both directions are covered, because both
are defects: a recheck that never revokes leaves the hole the scan exists to
close, and one that revokes on a transient read failure tears down a live region
for no proven change.

The harness exits non-zero if any mutation survives.

## Scope, and what these files do not show

The inputs are synthetic. This measures the headers' logic and the suites'
detection power. It is not a hardware result:

* Nothing here runs on hardware. A 2 MiB override has since been published and
  removed on a live descendant, and a page of it staged; those results are in
  `../20260916-large-leaf/` and are a separate measurement, not evidence about
  these suites. A 1 GiB leaf has since been requested on hardware under both
  rules and refused by both, for reasons measured in `../20260916-guest-workload/`;
  no 1 GiB leaf has been published on hardware, and on that stack none can be.
* Real source-EPT mutation by a running VMM, adversarial multicore, and actual
  hardware failures are not simulated here.
* A mutation score is a property of this fixed set of thirty-six defects. It
  is not an upper bound on the defects these headers could contain.

The lease contract and its ABA, guest-reboot and same-GPA-reuse limits are in
[`docs/next/ept-lease-contract.md`](../../ept-lease-contract.md).
