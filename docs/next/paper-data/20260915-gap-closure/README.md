# Current-driver gap-closure evidence — 2026-09-15

Author evidence archive; local paths, machine names and process identities are
retained. This is not an anonymized submission package. Raw failed/incomplete
attempts are preserved. See [the report](../../hvm-paper-gap-closure.md) for claims.

## Version and environment

Research source: `a7e1842f2d4a9bb3d3e77c8c5e98a53c8b2f3263`.
Builds explicitly disabled variant signing and automatic test signing, then used
the existing ordinary test certificate. [source-manifest.json](source-manifest.json)
records the build properties, exact binaries and source-file hashes. It was made
before later documentation, collector refinements or unrelated remote changes;
those changes do not retroactively change the identity of the measured binary.

- Driver: `667D21B398B7C6A6FA9D00BAC203576C05E32B6F33F0536573C99011D3EFC407`.
- Control executable: `DEDD4598BD918B09C263D287195B307CC1BBD38998D0BF5DE7CA8DE37C1B2FF1`.
- Outer host: Intel i7-13700F, family 6/model 183/stepping 1, 16 cores/24 logical
  processors; Windows 11 26300.9022, Hyper-V, HVCI active.
- Windows 1: Home 22621.4317, 2 vCPUs; VMware 17.6.4 build 24832109.
- Descendant: TinyCore 17.1, Linux 6.18.35-tinycore64, normal init and 2 vCPUs.

[root-environment.json](root-environment.json) and
[smp/environment.json](smp/environment.json) preserve actual queries, firmware
identifiers and virtualization settings. Root WMI virtualization booleans are
capability visibility under an active hypervisor; they are not a firmware-menu
readback and must not be used to claim VT-x was disabled.

## Separate batches

| Directory / records | Tested state | Outcome and scope |
| --- | --- | --- |
| Root `windows1-*`, `transition-*` | Earlier measurement prototype, driver `35380F71…` | Development batch, 168 benchmark processes and 10 pairs; do not use its overheads for the final driver |
| `final-windows/` | Current `667D21B3…`; no VMware; one Windows boot | 168 benchmark processes, 192 metric rows; 10/10 insertion/removal pairs, internal per-CPU timings |
| `smp/nested-page-*` | Current driver; one normally booted TinyCore, one reserved page | 20/20 dual-CPU effect closures: idle 10, busy 10; 16 full traces and 4 incomplete traces; 3 clean rejects |
| `smp/page-fault-*` | Five request-local fault modes, 10 each | 50 expected control/resource outcomes; 44 complete evidence sets, 6 incomplete; includes a later complete 25-run batch |
| `smp/write-isolation-*` | Both CPU-pinned observers | Full-page A5 → D1 → B2 header / D1 remainder → A5; guest writes only four bytes |
| `smp/stability-*` | Empty override slot, active readback observers | One 600.6-second collection, 21 points, 599.43 seconds between first/last samples |
| `smp/vmware-teardown-*` | VMware stopped while KSword stays resident | One current-driver pass; no Windows reboot or triple-fault event |
| `baseline-*`, feature/diagnostic records | No KSword; original, WHP, WHP+VMP configurations | Three fresh VMware startup refusals; no matched nested baseline |

`serial.txt` preserves CR/LF bytes because collector offsets count both characters.
SMP guest boot ID is `13391ee4-65c6-4dd3-b3f7-17d181422f30`. The recorded EPT12 root
belongs to that destroyed fixture and must not be reused. HVM TARGET was rebooted
after SMP teardown, outside any measured transition, for the final Windows batch.
Final Windows batch ended with KSword stopped and VMware absent.

The matched performance baseline retains prepared resources; it isolates resident
execution within this configuration. Off/pre → on → off/post is sequential and
not randomized. Seven measured repetitions per block plus one warmup provide
conditional statistics, not independent machine/boot replications. No matched
nested-guest slowdown can be inferred from rejected baseline starts.

The Linux benchmark executable compiled but its transfer was rejected by automatic
approval review; [the blocked record](linux-benchmark-blocked.json) is not a result.
Full Hyper-V cannot be installed on this Home edition. Intermediate Hyper-V remains
unverified, and Windows 10 LTSC is deferred by the user.

## Recompute without changing raw evidence

From repository root, using Python standard library only:

```powershell
$data='docs/next/paper-data/20260915-gap-closure'
python tools/hvm_paper/analyze.py "$data/final-windows"
python tools/hvm_paper/transition_metrics.py "$data/final-windows"
python tools/hvm_paper/analyze_smp.py "$data/smp"
python -c "import sys; from pathlib import Path; sys.path.insert(0,'tools/hvm_paper'); from analyze import stability; p=Path(sys.argv[1]); stability(p,p/'derived')" "$data/smp"
python tools/hvm_paper/analyze.py $data
python tools/hvm_paper/transition_metrics.py $data
python tools/hvm_paper/verify_gap_closure.py $data
python -m unittest discover -s tools/hvm_paper -p 'test_*.py'
```

Root `analyze.py` regenerates the SHA256 raw index recursively while excluding
all derived directories. The root summary applies only to the older root batch;
use `final-windows/derived` and `smp/derived` for current-driver results. The
dataset-specific verifier checks byte identity, denominators and retention of
known incomplete trials. It does not prove the hypervisor implementation correct.

Sampling detects the expected contents on both CPUs but cannot prove atomic 4 KiB
switching, absence of every transient stale mapping, or absence of every memory
leak. Resource ledgers cover page-control objects/backing only. Counter rates
cover KSword resident dispatch and actual INVEPT wrapper calls, not every exit
of outer Hyper-V. CPU timing intervals overlap and must not be summed.
