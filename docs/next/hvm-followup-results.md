# Pro, intermediate Hyper-V, and measured remaining overhead

This follow-up uses a new Windows 1 boot and an updated driver. Do not pool its
samples with the earlier Home/4x2 experiments. Raw records and reproduction
instructions are in [the follow-up dataset](paper-data/20260916-followup/README.md).

## Compatibility boundary

The [intermediate Hyper-V experiment](hyperv-descendant-results.md) now has a
working normal-init, two-vCPU TinyCore baseline. KSword remains refused in the
inner Hyper-V root partition: VMX bit 5 is clear and the capability query returns
`STATUS_NOT_SUPPORTED`. Windows boot, VM GUID and worker identity remain stable
during the refusal. This is a negative compatibility result, not a successful
hot insertion. The later VMX experiments disable only inner Hyper-V startup and
reboot the target outside measured intervals. Outer HVCI remains active.

## Same-binary performance comparison

Each mode has five measured repetitions plus one excluded warmup. The mode
order is deterministically shuffled within each round. Every mode uses the same
driver/control/workload binaries and Windows boot, with descendant VMMs absent.

| Mode | CPUID leaf 0 median | Loopback RTT median |
| --- | ---: | ---: |
| Residency off | 332.792 ns | 13.100 μs |
| Ordinary residency | 2,905.918 ns | 16.778 μs |
| Nested residency, full snapshot reference | 2,944.212 ns | 16.742 μs |
| Nested residency, sparse CPUID snapshot | 2,915.062 ns | 17.121 μs |
| Ordinary residency + 256 VMREADs per exit | 4,942.857 ns | 16.842 μs |
| Ordinary residency + 512 VMREADs per exit | 6,875.552 ns | 16.935 μs |

The small optimization skips exit-qualification and VM-instruction-error reads
on ordinary CPUID exits. VM-entry failures and other exits retain the full
snapshot. The reference mode restores these reads without changing the nested
or CPUID visibility policy. These fields' roles are described in Intel's
[VMX system programming manual](https://cdrdv2-public.intel.com/671506/326019-sdm-vol-3c.pdf).

The observed CPUID median falls 0.99%, but ranges overlap; this experiment does
not establish a robust speedup. TCP RTT does not improve. The current nested
mode still costs **8.76×** the off-mode CPUID time and **30.7%** more loopback RTT.
These are measurements of particular synthetic workloads, not a general
application slowdown or an apples-to-apples comparison with the older boot.

The 0/256/512-VMREAD experiment estimates approximately 7.8 ns per added VMREAD
(including loop overhead), so removing two such accesses cannot explain or
eliminate the remaining microsecond-scale cost. A separate million-exit profile
records 382 TSC cycles per C dispatcher invocation on CPU 0, 97 for the initial
VMCS reads and 25 for an empty timestamp pair. A measured invariant TSC near
2.112 GHz converts 382 cycles to approximately 181 ns. This average includes
the observed exit mixture; it is not an isolated CPUID-handler measurement.

The profiler excludes entry/exit assembly and outer-hypervisor work. The
difference from end-to-end CPUID timing points to those unmeasured paths as the
next attribution target; it does not uniquely assign the difference to Hyper-V.
The measured RTT windows are dominated by HLT, MSR and hypercall exits, so a
CPUID-only optimization should not be advertised as a networking fix.

## Application-visible disruption

A paced TCP client and echo service run on separate Windows vCPUs, using one
QPC clock domain. The control process runs on CPU 0. The observer records every
request's sequence, begin/end timestamps and previous completion, rather than
only the largest scheduling gaps. One warmup and five measured rounds contain
off-mode sham query, insertion, on-mode sham query, and removal.

All 24 runs complete with **74,145 correctly sequenced responses**, unchanged
Windows boot and key process identities. The 20 measured runs give:

| Condition | Median of run p99 RTT | Median command-overlap max completion gap | Largest command-overlap gap |
| --- | ---: | ---: | ---: |
| Sham, off | 166.99 μs | 2.1034 ms | 2.8122 ms |
| Insert | 212.22 μs | 2.1217 ms | 6.2300 ms |
| Sham, on | 209.61 μs | 2.1547 ms | 3.8944 ms |
| Remove | 187.47 μs | 2.0873 ms | 4.7701 ms |

Insertion's driver-body median is 2.5953 ms (maximum 3.0539 ms), and its
rendezvous median is 111.6 μs (maximum 148.2 μs). Removal medians are 1.1074 ms
and 55.8 μs respectively. Driver boundaries lie inside the measured control-call
window in the same QPC domain. Process creation and command work make the
application-level control calls roughly 20 ms.

The completion gaps include `Sleep(1)`, scheduling, socket work and observation
overhead. The on-mode sham has a 16.18 ms RTT outlier outside its command window.
Therefore neither a maximum gap nor its difference from a sham maximum is an
exact hypervisor pause. Closed-loop pacing also limits the offered load; these
results do not characterize overload or an external production client.

## EPT lease and resource regression

The [version 3 lease](ept-lease-contract.md) now records the source translation
path and revokes on observed semantic drift. The same walker passes portable
adversarial path/bit tests. On four Windows vCPUs and two TinyCore vCPUs, the new
binary passes three complete remap/restore cycles, fifteen request-local fault
trials and three clean invalid-request rejections. Both CPU observers verify
the complete 4 KiB page; all traces and allocation ledgers pass analysis.

The three map-body times have a 102.4 μs median (91.1–141.5 μs); restore-body
median is 104.3 μs (89.3–137.0 μs). Commit/retirement drain medians are 74.4/80.2 μs.
The measured control slot is empty and its allocations balanced after recovery.
This regression does not recreate arbitrary hardware failure, prove atomic
4 KiB application updates, or detect identical-bit-pattern GPA/root reuse.

## HTTP policy fault and direct-write comparison

A real BusyBox HTTP server serves a 4 KiB file-cache page containing a price
policy. A separate fixed-schema consumer checks the entire response and returns
`quote-1250` or `reject-corrupt-policy`; HTTP 200 alone is not a passing result.
The current driver completes three EPT fault/recovery trials out of four
attempts, with at least 90 fully joined HTTP/PFN/identity observations. One
attempt's bounded observer expires after five mapped samples; it remains
incomplete, and its successful cleanup is retained.

Three guest-side direct-write trials also produce the expected
`quote-1250 → reject-corrupt-policy → quote-1250` sequence, with 78 analyzed
responses. Six observations in the first 0.5 seconds after stage markers are
excluded by the stated settling rule and remain in raw serial. This rule does
not establish atomic page replacement. All three trials use a second pinned
file containing identical bytes, served by the same HTTP process; they do not
reuse the EPT trial's expired pin or claim a matched timing comparison.

| Property | EPT intervention | Guest-side direct write |
| --- | --- | --- |
| Measured fault and recovery | Three complete trials; one incomplete observer window | Three complete trials |
| Control authority after setup | Authorized Windows 1 KSword control | Guest-side permission to write the file |
| Original backing | Retained while private zero backing is served | Original file contents overwritten |
| Recovery source | Remove override and drain CPU translations | Saved original bytes |
| Intermediate VMM control in intervention | No management API, code hook or injected component | No VMM operation; normal guest file I/O |

Windows boot, VMware PID/creation time, guest boot, and HTTP server PID/creation
time remain unchanged across the respective intervals. Within each trial,
fresh pagemap observations confirm its pinned PFN. EPT rule/replacement ledgers
balance after removal. A read-only pre-comparator collector stalled, and an
attempt to reopen the old fixture hit the holder's create-new guard; these
setup failures are retained separately from the six complete application trials.

Post-run review also found that the policy collector's holder-alive field was a
constant one. The analyses explicitly report the lack of an independent liveness
probe. Advancing, fresh PFN observations are retained evidence; that constant
field is not. The checked-in collector now uses `kill -0`, with no new runtime
result claimed for that fix.

The application value demonstrated here is reversible data-fault injection
from the containing Windows guest without a VMM extension. Both mechanisms
work in this fixture. The experiment does not establish that this authority is
always preferable, that no guest instrumentation is needed, or that the oracle
models production business logic. It measures correctness effects and control
requirements, not comparative throughput or recovery latency.

## Still open

- Hot insertion beneath already-running intermediate VMMs, including Hyper-V.
- A substantial reduction in end-to-end exit/steady-state overhead.
- Hardware fault and cross-machine validation; Windows 10 LTSC is deferred.
- Overnight and current-build ten-minute soak: not newly run at the user's request.
- A full anonymous buildable source artifact and the full manuscript.

## Final lab state

All page overrides are removed, their tracked allocations balance, VMware has
stopped normally and KSword residency has stopped. Inner Hyper-V startup is
restored to `auto`; HVM-target has completed an out-of-experiment reboot.
The inner TinyCore definition remains available and stopped. This final cleanup
does not extend any earlier continuity interval or stability result.
