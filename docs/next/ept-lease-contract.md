# Descendant page lease: ownership and reclamation contract

The version 3 page-control response records the captured source EPT path,
the Windows 1 physical backing, and the first revocation reason. These are
implementation guarantees with explicit preconditions, not a general guarantee
that a guest-physical address identifies the same application object forever.

## Preconditions

- The authorized caller reserves/pins one ordinary 4 KiB WB RAM page in the
  descendant and supplies its current GPA and observed EPT12 root.
- The owner is bound to a referenced process object and its creation time.
  The caller removes the lease before resetting the VM, repurposing the GPA,
  changing the workload page's ownership, or migrating the workload.
- The intermediate VMM obeys architectural EPT invalidation requirements. The
  prototype is not a security boundary against a malicious intermediate VMM.

## Lifecycle

```text
capture source path + validate again
                 |
          allocate replacement
                 |
        bind process identity
                 |
      publish immutable lease
                 |
      all-CPU invalidation ------ failure ------+
                 |                              |
               active                           |
                 |                              |
       remove / owner exit /                    |
       observed source drift                    |
                 |                              |
     unpublish or mark revoked <----------------+
                 |
       retain rule + backing
                 |
       all-CPU drain succeeds
                 |
            reclaim memory
```

Publication, invalidation completion, and reclamation are separate events.
`active=0` alone does not prove stale CPU translations are gone. A nonzero
`retired` means backing remains retained. A failed drain must retain memory;
retrying removal performs the same all-CPU drain before freeing it.

## Source identity checks

Admission records the EPTP configuration, each entry address and normalized
entry value, effective permissions, and resolved source frame. Before nested
entry and before a shadow-EPT fill, matching leases are revalidated using the
per-CPU physical window. Address, permissions, page size, memory type, or parent
path changes revoke the lease. With EPT A/D enabled, only architecturally
maintained accessed bits and leaf dirty bits are ignored. Unreadable paths
revoke separately. Revocation is sticky until explicit drain and a new map.

This is a bounded sampled check. It cannot detect an ABA change that restores
all captured bits before observation, a VM reboot that preserves the identical
translation, or reuse of an application object at the same GPA. It does not
make changes on different vCPUs simultaneous. A source change observed by one
CPU prevents new authorized overrides; other CPUs' cached translations can
persist until their next invalidation point. Backing is retained throughout.
The synchronous remove response, rather than asynchronous revocation alone,
is the boundary at which successful all-CPU retirement may be claimed.

## Evidence and remaining validation

`test_ept_lease.c` directly compiles the same walker used by the driver. It
changes every bit at every level, exercises A/D modes, 4 KiB/2 MiB/1 GiB leaves,
address/root-path reuse, unreadable sources and malformed requests. This checks
the comparator and admission algorithm; it is not a hardware concurrency test.
Runtime remap/restore and fault results must additionally name the exact version
3 binary. Earlier version 2 results remain historical and cannot establish that
the new guard passed runtime validation.
