# Nested-page transaction and measurement contract

## Publication and failure recovery

The control path is `hvm_nested_page.c`. It serializes against the HVM lifecycle
with the runtime lock; root-mode readers use the published pointer without taking
that lock. A successful map initializes its rule and 4 KiB WB replacement, publishes
the pointer, and invalidates the base and composed EPT contexts on participating
processors. Logical publication alone is not a successful commit.

If commit fails, the implementation withdraws the rule and attempts another
all-processor invalidation. It frees the rule and backing only after that drain
succeeds. Otherwise it retains the allocation in `retired`; another map is refused.
Removal retries drain the retained allocation. A fully stopped VMX lifecycle also
allows teardown to reclaim it. These rules prevent freeing memory while a processor
may still have a translation to it. The response retains the original commit error,
while the trace records the separate rollback result.

This is one override keyed by EPT12 root and descendant GPA. Callers must choose a
reserved ordinary RAM page and keep its owning VM alive. It does not establish
guest semantic consistency, device/DMA isolation, or an atomic 4 KiB read/write.
Changing a backing mapping cannot make an arbitrary application data structure
consistent. Root reuse across VM destruction is outside this contract.

## Request-local fault injection

The ordinary commands remain `nested-page-map` and `nested-page-remove`.
The main program's existing command catalog and standalone CLI use the same engine.

```text
hvm_ctl --json nested-page-map-test <EPT12> <reserved GPA> <fill byte> <1..4>
hvm_ctl --json nested-page-remove-test
hvm_ctl --json nested-page-remove
```

| Mode | Injected point | Expected control result |
| --- | --- | --- |
| 1 | Replacement allocation omitted after rule allocation | `STATUS_INSUFFICIENT_RESOURCES`; no publication; rule reclaimed |
| 2 | Cancel after allocation, before publication | `STATUS_CANCELLED`; both allocations reclaimed; generation unchanged |
| 3 | Cancel after successful publication and invalidation | `STATUS_CANCELLED`; withdraw, drain, reclaim |
| 4 | Commit invalidation call reports injected failure | `STATUS_HV_OPERATION_FAILED`; withdraw, perform real rollback drain, reclaim |
| 5 | Removal invalidation call reports injected failure | `active=0, retired=1`; normal remove retries real drain and reclamation |

Modes 4 and 5 simulate a failure at the invalidation call boundary by omitting that
call. They are **not physical INVEPT instruction failures**. Actual instruction
attempt/result counters must not increment for an omitted instruction. No fault
remains armed after the request. The ordinary map/remove path uses no fault flags.
This coverage does not reproduce arbitrary CPU loss or a hardware instruction fault.

## Machine-readable trace

`nested-page` responses include an `operationId` in the former reserved response
word. The request layout, response size, and version remain compatible. Event type
`NESTED_PAGE` (6) gives that id as `pageOperationId`, a `pageStage`, an operation,
fault mode, GPA, EPT12 pointer, backing PA and status. `timestampQpc` is the driver's
QPC reading; `metrics.qpcFrequency` supplies its units. The event ring is bounded:
missing required stages or overwritten evidence must be classified as incomplete.

| Stage | Meaning |
| --- | --- |
| 1 | Authorized operation begins |
| 2 / 3 | Allocation and initialization begin / end |
| 4 | Rule pointer published |
| 5 / 6 | Commit invalidation begin / end |
| 7 | Rule pointer withdrawn |
| 8 / 9 | Removal or rollback drain begin / end |
| 10 | Allocation actually reclaimed |
| 11 | Operation ends |

The generic event fields remain for existing consumers: `ruleId` is the operation
id, `exitReason` is the stage, `access` is the page operation, `qualification` holds
request flags, `guestLinearAddress` carries EPT12, and `guestRip` carries replacement
backing. For type 6 these are **not** CPU exit/access/RIP observations; new consumers
should use the typed aliases.

## Resource and continuity evidence

`metrics` reports actual successful rule/replacement allocations and actual frees,
independent of `active` and `retired`. They count only this single-page facility,
not all allocations in the driver or hypervisor. Per-run records must retain:

- Driver/control SHA256, the Windows boot identity and VMM PID/creation time.
- Before/after metrics and page state, the exact failed request and normal retry.
- Correlated event stages and raw guest observations on every participating CPU.
- A final zero active/retired count and balanced object/backing ledgers.

`Test-NestedPageFaults.ps1` saves each trial independently before mutation and after
every command. Its control assertions are separate from guest readback analysis;
an accepted command is not sufficient evidence that the target guest observed the
expected bytes or that no stale translation remained.

## Scope of intermediate-VMM independence

The page-control path invokes KSword's driver interface and resident hypercalls.
It does not call VMware/Hyper-V management APIs or patch/inject their code. Normal
guest hypercalls, nested VMX instructions and their reflection continue during the
experiment. Therefore, "Hyper-V was never called" is not a valid description.
The outer Hyper-V and Windows root with HVCI are trusted and remain outside the
monitor's authority; this work makes no confidentiality claim against them.
