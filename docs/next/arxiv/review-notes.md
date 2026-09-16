# Author review map

This is an initial manuscript, not a declaration that human review or external
peer review has finished. All author identity fields were supplied by the author.

## Substantive claims

| Claim | Review entry point | Boundary retained in the paper |
| --- | --- | --- |
| Windows-only live insertion | `latency/`, historical `after/` transition records | No intermediate VMM running at insertion |
| Current performance | `analysis-followup.json`, `attribution-runs.csv` | Five measured repetitions, one machine/boot, no descendant VMM |
| Application disruption | Every request in `latency/*.jsonl` | Paced closed loop; gap is not exact transition pause |
| ABI 3 lease | `hvm_nested_page.c`, `hvm_nested_lease_walk.h`, lease contract | Sampled translation identity; no same-bits ABA guarantee |
| Remap/rollback/reclaim | `lifecycle/` raw records and derived summary | Page-control ledger only; injected software boundary faults |
| HTTP policy behavior | `application/`, archived measured scripts | Three complete EPT trials, one incomplete; constant holder-alive defect |
| Direct-write comparator | `application/inplace-*`, policy summary | Different pinned file, six settling exclusions, no timing comparison |
| Historical optimization | `20260915-4x2/derived/`, `after/derived/` | Different implementation and guest boot; not no-monitor descendant overhead |
| Inner Hyper-V | `hyperv-*`, normal-init serial | Positive baseline, negative KSword admission |
| Source identity | Current `provenance.json`, historical build provenance | Source parent alone is insufficient for a dirty-tree build |

Paths without a dataset prefix above refer to
`docs/next/paper-data/20260916-followup/`.

## Primary bibliography audit

- SubVirt: Microsoft Research publication page and original paper.
- Blue Pill: author presentation hosted by Black Hat; used for the historical
  on-the-fly interposition concept, not its undetectability assertions.
- Turtles: original OSDI paper on USENIX.
- CloudVisor: SOSP proceedings abstract and author-hosted paper metadata.
- HyperFresh: author-hosted VEE paper and its DOI.
- HyperTP: publisher article metadata/abstract and DOI.
- HyperTurtle: USENIX proceedings page and bibliography.
- Intel VMX/EPT and Microsoft Hyper-V: vendor documentation.

The manuscript does not import published performance numbers as local baselines.
The DOI fields in references belong to those published works; this preprint has
no DOI or journal-reference claim.

## Deferred scientific work

- Live handoff of an already-running VMM and descendant stack.
- Actual source-EPT mutation and current-build owner-exit/high-load stress.
- Rerun with the corrected independent holder-liveness probe.
- External application observers and current active-lease performance.
- Independent boots/machines and meaningful production recovery workloads.
- Current-build ten-minute/overnight stability and whole-driver allocation trends.

Before public submission, the author should complete their data/claim review and
choose the submission category and license. No additional experiments are implied
by successful typesetting or source-package validation.
