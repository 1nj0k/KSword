#pragma once

#include <ntddk.h>

EXTERN_C_START

#define KSW_OBJECT_HEADER_FALLBACK_FIELD_POINTER_COUNT 0x00000001UL
#define KSW_OBJECT_HEADER_FALLBACK_FIELD_HANDLE_COUNT  0x00000002UL

typedef struct _KSW_OBJECT_HEADER_FALLBACK_RESULT
{
    ULONG ValidFields;
    ULONG BodyOffset;
    ULONG PointerCountOffset;
    ULONG HandleCountOffset;
    ULONG PointerCount;
    ULONG HandleCount;
} KSW_OBJECT_HEADER_FALLBACK_RESULT, *PKSW_OBJECT_HEADER_FALLBACK_RESULT;

NTSTATUS
KswordARKObjectHeaderResolveBodyOffsetFallback(
    _Out_ ULONG* BodyOffsetOut
    );

NTSTATUS
KswordARKObjectHeaderQueryFallback(
    _In_ PVOID Object,
    _Out_ KSW_OBJECT_HEADER_FALLBACK_RESULT* Result
    );

//
// Take a reference on an object body the caller holds no reference into, such
// as a pointer decoded out of ActiveProcessLinks or PspCidTable.
//
// ObReferenceObjectByPointer and ObfReferenceObject both raise bugcheck 0x18
// (REFERENCE_BY_POINTER) when the pointer count has already reached zero, and
// that is a bugcheck rather than an exception, so no __except around them can
// hold it.  This routine increments the count only while it is still positive,
// and refuses the object otherwise.
//
// STATUS_SUCCESS means a reference was taken and the caller must release it
// with ObDereferenceObject.  STATUS_DELETE_PENDING means the object is already
// being deleted.  STATUS_NOT_SUPPORTED means the header layout is unresolved,
// in which case no reference may be taken at all.
//
NTSTATUS
KswordARKObjectHeaderReferenceObjectSafe(
    _In_ PVOID Object
    );

EXTERN_C_END
