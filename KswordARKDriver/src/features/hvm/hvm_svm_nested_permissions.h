/* AMD APM 15.10/15.11: capture, combine and route nested permission maps. */
#pragma once
#include "hvm_svm_arch.h"

#define KSW_NSVM_MSRPM_BYTES 8192U
#define KSW_NSVM_IOPM_BYTES 12288U
#define KSW_NSVM_IOIO_PROT (1U << 27)
#define KSW_NSVM_MSR_PROT (1U << 28)
#define KSW_NSVM_PERMISSION_FLAGS (KSW_NSVM_IOIO_PROT | KSW_NSVM_MSR_PROT)
/* Ownership is a bit mask; BOTH still requires reflection before L0 emulation. */
#define KSW_NSVM_OWNER_NONE 0U
#define KSW_NSVM_OWNER_L0 1U
#define KSW_NSVM_OWNER_L1 2U
#define KSW_NSVM_OWNER_INVALID 4U

/* The caller holds these immutable, nonpageable snapshots until L2 exits. */
typedef struct _KSW_NSVM_PERMISSION_VIEW {
    /* Only the IOIO_PROT/MSR_PROT bits affect map semantics. */
    unsigned int Flags;
    /* Disabled maps may be NULL and must not be dereferenced. */
    const unsigned char* Msr;
    const unsigned char* Io;
} KSW_NSVM_PERMISSION_VIEW;

/* Preallocated destination; Ready is cleared before any capture attempt. */
typedef struct _KSW_NSVM_PERMISSION_IMAGE {
    /* Publication is CPU-local; external readers must use the runtime sequence. */
    unsigned int Ready, Flags;
    /* Reserved map bytes are retained, but never interpreted as MSR bits. */
    unsigned char Msr[KSW_NSVM_MSRPM_BYTES];
    unsigned char Io[KSW_NSVM_IOPM_BYTES];
} KSW_NSVM_PERMISSION_IMAGE;

/* Reads one full page of L1 physical RAM, after NPT01/cache/ownership validation.
   No direct physical cast, MMIO, allocation or blocking is allowed in this callback. */
typedef int (*KSW_NSVM_PERMISSION_READ)(void* Context, KSW_SVM_U64 GuestPa,
    unsigned char* Page);

/* APM ignores low twelve base bits; validate the entire hardware allocation. */
unsigned int KswSvmNestedMapAddress(KSW_SVM_U64 Address, unsigned int Bytes,
    unsigned int PhysicalBits, KSW_SVM_U64* Base);
/* Capture is not atomic with an unsynchronized L1 writer; entry owns publication. */
unsigned int KswSvmNestedCapturePermissions(KSW_NSVM_PERMISSION_IMAGE* Image,
    unsigned int Flags, KSW_SVM_U64 MsrPa, KSW_SVM_U64 IoPa,
    unsigned int PhysicalBits, KSW_NSVM_PERMISSION_READ Read, void* Context);
/* Failed capture images cannot be turned into an executable permission view. */
unsigned int KswSvmNestedPermissionView(const KSW_NSVM_PERMISSION_IMAGE* Image,
    KSW_NSVM_PERMISSION_VIEW* View);
/* Destination maps must be distinct preallocated hardware allocations, not sources.
   On failure they remain untouched and must not be published to VMCB02. */
unsigned int KswSvmNestedMergePermissions(const KSW_NSVM_PERMISSION_VIEW* Outer,
    const KSW_NSVM_PERMISSION_VIEW* Inner, unsigned char* Msr, unsigned char* Io);
/* Only MSR/IOIO exits are accepted; malformed metadata returns INVALID, never NONE. */
unsigned int KswSvmNestedPermissionOwners(const KSW_NSVM_PERMISSION_VIEW* Outer,
    const KSW_NSVM_PERMISSION_VIEW* Inner, KSW_SVM_U64 ExitCode,
    KSW_SVM_U64 ExitInfo1, unsigned int MsrNumber);
