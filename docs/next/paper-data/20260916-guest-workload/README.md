# Configured memory is not mapped memory

Everything here was measured on the live chain — Hyper-V root, a Windows VM, our
driver resident inside it, VMware Workstation inside that, and a TinyCore
descendant inside VMware. No synthetic inputs.

Two questions were open before these runs. Both are now answered, and one of the
answers is a refusal.

## 1. A large override needs the descendant to have touched the memory

Raising `memsize` does not create mappings. VMware populates the descendant's
EPT — the table our planner reads and clones, called EPT12 throughout — on
demand, so a descendant told it has 4 GiB has entries only where its guest has
actually been. Publishing an override at an untouched base therefore measures
the source table, not the planner.

`extent-4g-idle-21.json` and `extent-4g-idle-30.json` are that descendant at
4096 MiB, freshly booted and idle. Every base from 256 MiB to 3.75 GiB refuses
with `STATUS_ADDRESS_NOT_ASSOCIATED` and `scannedLeafCount` zero.

Read that carefully, because a later run corrected what it means.
`extent-4gplus.json` probes above the 4 GiB line on the same descendant and
finds four consecutive 2 MiB bases — `0x108000000` through `0x120000000` —
admitted with 512 agreeing entries each. **At 4096 MiB this descendant's bulk
memory is above `0x100000000`, not below it**, so the idle sweep was reporting
which bases the guest uses at all, not only which it had touched yet. The claim
that survives is the narrower and more useful one: a base carries an override
only where the guest has actually been, and guessing the base is how you get a
refusal that says nothing about the planner. The controlled demonstration of
that is the 768 MiB pair below, where the bases are held fixed and the workload
is the only variable.

The lever that changes this is a workload in the descendant, driven over its
VNC framebuffer into the shell on `tty1` (`docs/next/logs/Get-VmwareVnc.ps1`).
Its serial console carries kernel messages only — no getty is attached — so
`docs/next/logs/Invoke-TinyCoreConsole.ps1` connects, sends and records, and
comes back with an empty transcript, which is the evidence that the serial path
is not the one to use.

The clearest before/after is at 768 MiB, where a 400 MiB `dd` into tmpfs moves
three bases from *nothing is mapped* to *a 2 MiB leaf is admitted*:

| base | idle | after 400 MiB fill |
|---|---|---|
| `0x10000000` | refused, 1 entry scanned | **published**, 512 entries, uniform |
| `0x18000000` | `STATUS_ADDRESS_NOT_ASSOCIATED` | **published**, 512 entries, uniform |
| `0x20000000` | `STATUS_ADDRESS_NOT_ASSOCIATED` | **published**, 512 entries, uniform |

(`extent-768m-idle.json`, `extent-768m-after-400mib-fill.json`.)

This also says *where* uniformity lives. The low bases stay refused after the
fill — they stop at entry 24, 33, 146 — because that is kernel, DMA and firmware
memory with mixed attributes. Bulk anonymous memory the guest allocated in one
go is what scans uniform.

## 2. A 1 GiB leaf is refused here, and the refusal is the result

`extent-lp-fill-30.json` and `extent-4g-done-30.json` are the 1 GiB attempts,
under both admission rules, on a descendant that had written 2.7 GiB. Both
refuse with `scannedLeafCount` zero.

That is the designed behaviour and it is worth stating precisely, because
`scannedLeafCount` zero next to a *mapped* base is the signature of the scan
bound and not of an unmapped region:

* Every mapped base, under every configuration tried, reports
  `sourceLeafShift = 12`. VMware maps this descendant at 4 KiB — at 768 MiB and
  at 4096 MiB, idle and filled, and with `MemTrimRate`, page sharing,
  `prefvmx.minVmMemPct` and `mainMem.useNamedFile` set to the combination that
  pins guest memory and permits MMU large pages (`extent-lp-boot-21.json` and
  after).
* A 1 GiB region over 4 KiB source leaves is 262,144 entries. The scan bound is
  512, and it is a bound on the *drift check*, not just on the initial pass —
  every one of those entries would have to be re-read on each check. So the
  scanner refuses before reading anything, which is why the count is zero.
* The coarse-source rule refuses for the other reason: it needs EPT12's own leaf
  to be at least 1 GiB, and here it is 4 KiB.

So a 1 GiB leaf is **not** unexercised on hardware any more: it is exercised and
refused, by policy, for a measured reason. Admitting one needs an intermediate
VMM whose own EPT uses leaves of 2 MiB or coarser. That is a property of the
VMM, not of this planner, and no `.vmx` setting tried here changes it.

The pinning settings were not wasted, but they are not a cure either. Comparing
the same two bases at the same 4096 MiB after the same kind of fill:

| base | before pinning | after pinning |
|---|---|---|
| `0x40000000` | 139 entries, then an absent one | 151 entries, then an absent one |
| `0x60000000` | 93 entries, then an absent one | **512 entries, uniform, published** |

(`extent-4g-done-21.json` versus `extent-lp-fill-21.json`.) The shared bits read
back as zero on both refusals, which distinguishes them: a disagreeing entry
would have reported the bits agreed on up to that point, so these are holes in
EPT12, not permission conflicts.

## 3. One 2 MiB leaf above the 1 GiB line, end to end

`large-leaf-above-1gib.json` is the full sequence at guest-physical
`0x60000000` — 1.5 GiB, higher than anything previously published — regenerated
by `docs/next/logs/Measure-LargeLeafAboveOneGiB.ps1`:

| step | what it shows |
|---|---|
| publish | admitted by scanning 512 source entries sharing `0x77`; backing `0x1DBA00000`, 2 MiB aligned |
| t0 | source digest == backing digest over 2,097,152 bytes — the clone is faithful |
| t1 (+30 s) | both unchanged |
| stage page 300 | backing digest moves, **source digest does not** |
| stage page 512 | `STATUS_INVALID_PARAMETER`, staged count unchanged — refused, not wrapped |
| remove | `active=0`, `retired=0`, four processors invalidated |

`leaseRevocationReason` stays 0 throughout.

Under a 90-second guest-wide read of a 2.7 GiB file with that leaf published,
`composedCount` went from 0 to **1**: one composition covered all 512 pages of
the region. A 4 KiB policy needs one composition per page to cover the same
ground. That is the saving, measured — though note the denominator is what the
region contains, not what this particular workload touched, and no attempt was
made here to steer the workload at a chosen guest-physical address.

## 4. A published region revoked by a real source change

`region-revocation.json` (`../../logs/Measure-RegionRevocation.ps1`) publishes a
2 MiB region at `0x110000000`, then snapshots the descendant while polling the
lease. The snapshot drops the intermediate VMM's entries for that region
entirely — visible here as the same base answering
`STATUS_ADDRESS_NOT_ASSOCIATED` while the snapshot exists — and the lease is
revoked within the first five-second sample, with the region then removed
cleanly (`status=0`, `retired=0`).

The reason it reports is `2`, translation-changed, not `4`, region-drifted, and
that is a result rather than a disappointment. The lease's ordinary check runs
at every shadow fill and covers the path the lease was captured on; the region
recheck samples one *other* page per 4096 fills. A whole-VM event changes the
captured path too, so the ordinary check always sees it first. Reason `4` can
only win when a page of the region other than the captured one changes while the
captured path stays identical — a per-page revocation such as page sharing
collapsing one page, or a balloon reclaiming one — and nothing we can ask this
VMM to do produces that. The reason-4 decision itself is covered by eight
injected defects in `../20260916-lease-largepage/`, in both directions.

## 5. An incremental driver build that broke the descendant's VM entry

`vmware-monitor-panics.txt` is the descendant VMM's own log history, summarised.
It contains a clean A/B that was not planned:

| log | driver | `MONITOR PANIC` |
|---|---|---|
| vmware-4, vmware-3 | `9b7771ca…` | 0 |
| vmware-2, vmware-1, vmware-0 | `088c44a0…` | 1 each, 3 of 3 runs |
| vmware | `ea6965f0…` | 0 |

`088c44a0` and `ea6965f0` are the **same sources**. The first was produced by
`msbuild /t:Build`, the second by `/t:Rebuild`. Under the incremental build,
every run ended in

```
MONITOR PANIC: vcpu-N:VM-entry failed; VMCS valid (error code 7)
```

which is the intermediate VMM's VM entry being rejected for invalid control
fields — the descendant stops, its VMM posts an unrecoverable error that is
invisible when it was started from session 0, and the process stays alive
serving its framebuffer, so from outside it looks like a hang. The clean rebuild
ran a 800 MiB allocation, a snapshot cycle and a further 400 MiB allocation with
none. Nothing here explains *why* an incremental build differs; it records that
it did, and that a driver binary must come from a clean rebuild before any
reading taken with it means anything.

## Scope

* One descendant, one VMM, one host. `sourceLeafShift = 12` is a fact about
  VMware Workstation on this machine, not about VMMs in general.
* The region drift sampler is present and was not triggered in any run here;
  `leaseRevocationReason` was 0 every time. It remains unobserved revoking a
  lease on hardware.
* TinyCore booted cleanly at 4096 MiB in both runs on this page. With the three
  earlier controlled boots at 4096/2048/768, the kernel panic seen once after a
  simultaneous driver-and-memory change has now failed to reproduce five times
  and is attributed to neither.
