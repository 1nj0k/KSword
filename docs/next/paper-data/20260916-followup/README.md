# Pro follow-up, 2026-09-16 UTC

This dataset contains new measurements after the target's Pro upgrade and a new
driver build. Do not pool them with `20260915-4x2` or the older two-CPU soak.
The [results report](../../hvm-followup-results.md) explains interpretation.

## Configuration and binary identity

- Outer CPU: Intel Core i7-13700F, family 6 / model 183 / stepping 1 as reported
  in `outer-cpu-identity.json`, 16 cores / 24 logical processors; outer
  Hyper-V binary 10.0.26100.9022, Windows build 26300. See `outer-environment.json`.
- Windows 1: Pro 22621.4317, four vCPUs, 8 GiB, nested extensions exposed.
  The inner Hyper-V experiment and later VMX tests use **different boots**.
- VMware: Workstation 17.6.4 build 24832109; TinyCore 17.1,
  kernel 6.18.35-tinycore64, two vCPUs, normal `/init`, 768 MiB.
- Inner Hyper-V child: generation 1, two vCPUs, 768 MiB, diskless evidence ISO.
  `hyperv-postboot.json` and fixture/admission records retain exact identities.
- Outer HVCI stays active (`outer-deviceguard.json`, running service code 2).
  Inner Hyper-V startup is disabled only for the VMX experiments, outside their
  continuity intervals. Final cleanup state is recorded separately.
- Main tested driver SHA256:
  `3B44132F82FEBCE3FCDF059B697D938479B9F791A628C00B55BC2CF397D5D500`.
  Controller SHA256:
  `4DD0D8DABB3019503E15D70692759FF0502CAC46635D53F266A577CAAD5972AE`.
  The Hyper-V admission baseline uses the previous driver, explicitly recorded
  in its files; the refusal occurs before resource preparation.
- `provenance.json` records the source parent, current source hashes and binary
  hashes. Measurements were made from the working tree before its commit, so
  the parent commit alone is not the measured implementation identity.
- BIOS version and Windows-reported CPU feature fields are retained literally.
  False VMX/SLAT firmware fields under an active hypervisor are not proof that
  firmware virtualization is disabled. Unavailable fields remain null.

The driver uses standard MSVC/WDK and the existing ordinary test certificate.
Both automatic variant signing and automatic test signing were disabled during
the build. Signing was a separate step; target loading succeeded. Host
Authenticode trust verification reports an untrusted root and is **not** a pass.
The main program and both CLIs were rebuilt. GUI runtime tests were not run.

## Raw evidence map

| Directory / record | Scope and retained outcomes |
| --- | --- |
| `hyperv-*`, `inner-hyperv-install.json` | Two ISO boot failures, one successful normal-init baseline, reproducible capability refusal with unchanged child/worker identities |
| `attribution/` | 72 runs: 6 modes × 2 workloads × (1 warmup + 5 measured); all successful; setup and cleanup records separate |
| `latency/` | 24 runs: 4 conditions × (1 warmup + 5 measured), 74,145 valid TCP responses; every request retained in JSONL |
| `exit-cost-*.json`, `tsc-calibration.jsonl` | Bounded diagnostic profiles and invariant-TSC calibration; the first short sample did not reach the reporting threshold |
| `lifecycle/` | 3 complete two-CPU cycles, 15 injected-control trials, 3 invalid-request rejections; per-run records, full serial and phase traces |
| `application/http-page-*.json` | 4 EPT trials: 3 complete, 1 incomplete observer window with successful removal/cleanup |
| `application/inplace-{1,2,3}.{json,log}` | 3 direct-write trials, per-run excerpts from durable serial, persisted after execution; whole-series Windows/VMM snapshots separate |
| `application/*failure.json` | Read-only collector stall and holder create-new rejection, neither counted as a successful application trial |
| `application/measured-*` | Exact guest-side scripts used in the experiment, captured afterwards; MD5/SHA256 checked |
| `preflight-serialization-error.json` | An initial PowerShell remoting serialization failure, retained rather than treated as an environment result |
| `validation/` | Build outputs and analysis/static-check results; scope of each check stated separately |

## Application comparison contract

The EPT trials pin `policy.json` at GPA `0x2479000`; original Windows 1 backing
is `0xCCF89000`. The comparator pins a separate `policy-inplace.json` at GPA
`0x170d1000`, with identical initial contents and the same HTTP server. Original
pin expiry and the holder's `O_EXCL` guard are handled outside measured trials.
Both readbacks require complete 4096-byte responses, current PFN observations,
and a business decision (`quote-1250` or `reject-corrupt-policy`).

The strict HTTP analyzer validates 90 responses in the three complete EPT trials.
The interleaving-tolerant policy join validates 91 oracle responses; one extra
response is separated from its PFN line by unrelated serial output and is not
counted by the strict contiguous HTTP join. The comparator validates 78 responses.
Its six observations in stage settling windows are counted as exclusions and
remain in raw serial. The 0.5-second rule is relative to the first observation
after a marker, not a timestamp for the physical write. No atomicity or matched
recovery-latency conclusion follows.

The observer window was increased from 120 to 360 iterations before the final
EPT trial. The measured comparator script substitutes its own file/PFN-log paths.
The checked-in wrapper later exposes these as `POLICY_FILE` and `POLICY_PFN_LOG`
environment variables; the archived measured copies are authoritative for hashes.
Use `POLICY_FILE=policy-inplace.json POLICY_PFN_LOG=pfn-inplace.jsonl`
and `POLICY_HOLDER_PID_FILE=holder-inplace.pid` for the
equivalent named-file setup. Neither intervention invokes a VMM management API
during its measured window; ordinary nested VMX, hypercalls and A/D updates continue.

Post-run source review found that the measured policy observer printed the holder
`alive` field as constant one. That field is **not independent liveness evidence**.
The current analyses expose `independentHolderLiveness=false`; their accepted
evidence is advancing PFN observations no more than five seconds old, together
with per-response server creation time and guest boot ID. This does not prove
holder liveness at every response instant. The checked-in script now probes
`kill -0`, but this fix was not rerun on the target. Raw records remain unchanged.

## Reproduction

From the repository root:

```powershell
python tools/hvm_paper/analyze_followup.py docs/next/paper-data/20260916-followup
python tools/hvm_paper/analyze_smp.py docs/next/paper-data/20260916-followup/lifecycle
python tools/hvm_paper/analyze_http_page.py docs/next/paper-data/20260916-followup/application
python tools/hvm_paper/analyze_policy_comparison.py docs/next/paper-data/20260916-followup/application
```

Python 3.10+ and the standard library suffice. Raw records remain unchanged;
derived summaries are written separately. Successful analysis means the reported
checks passed, not that all experiments succeeded: incomplete trials are retained
in the tables. The anonymous package independently reruns these analyses after
length-preserving redaction and checks file hashes and ZIP integrity.

The experiment does not establish Hyper-V hot insertion, a substantial new speedup,
cross-machine compatibility, arbitrary page atomicity, or same-bits guest reboot
detection. No new soak, Windows 10 LTSC, or GUI runtime test is claimed.
