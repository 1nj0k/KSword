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
with `STATUS_ADDRESS_NOT_ASSOCIATED` and `scannedLeafCount` zero: nothing is
mapped up there at all.

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
