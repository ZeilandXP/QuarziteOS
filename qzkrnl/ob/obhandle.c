/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Handles: the (system) handle table and handle-based services
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * There are no processes yet (stage B6), so there is a single handle table:
 * the one NT gives the System process for kernel handles. Handle values are
 * multiples of 4 with the kernel-handle bit (0x80000000) set; lookups
 * accept them with or without that bit.
 */

#include "qz.h"

#define QZ_HANDLE_TAG           'dHbO'
#define QZ_KERNEL_HANDLE_BIT    0x80000000UL
#define QZ_INITIAL_HANDLES      64

typedef struct _QZ_HANDLE_ENTRY
{
    PVOID Object;               /* NULL: free */
    ACCESS_MASK GrantedAccess;
    ULONG Attributes;
} QZ_HANDLE_ENTRY, *PQZ_HANDLE_ENTRY;

static PQZ_HANDLE_ENTRY QzHandleTable;
static ULONG QzHandleCapacity;
static ULONG QzHandlesInUse;
static ULONG QzHandleHint;
static KSPIN_LOCK QzHandleLock;

ULONG
QzObHandleCount(VOID)
{
    return QzHandlesInUse;
}

static __inline HANDLE
QzIndexToHandle(ULONG Index)
{
    return (HANDLE)(ULONG_PTR)(QZ_KERNEL_HANDLE_BIT | ((Index + 1) << 2));
}

static __inline LONG
QzHandleToIndex(HANDLE Handle)
{
    ULONG Value = (ULONG)(ULONG_PTR)Handle & ~QZ_KERNEL_HANDLE_BIT;
    if (Value == 0 || (Value & 3))
        return -1;
    return (LONG)(Value >> 2) - 1;
}

static BOOLEAN
QzIsPseudoHandle(HANDLE Handle)
{
    return Handle == NtCurrentProcess() || Handle == NtCurrentThread();
}

NTSTATUS
QzObpCreateHandle(PVOID Object, ACCESS_MASK GrantedAccess, ULONG HandleAttributes,
                  PHANDLE Handle)
{
    POBJECT_HEADER Header = OBJECT_TO_OBJECT_HEADER(Object);
    PQZ_HANDLE_ENTRY NewTable;
    ULONG Index, NewCapacity;
    KIRQL OldIrql;

    OldIrql = QzAcquireSpinLock(&QzHandleLock);

    if (QzHandlesInUse == QzHandleCapacity)
    {
        NewCapacity = QzHandleCapacity ? QzHandleCapacity * 2 : QZ_INITIAL_HANDLES;
        NewTable = ExAllocatePoolWithTag(NonPagedPool, NewCapacity * sizeof(QZ_HANDLE_ENTRY),
                                         QZ_HANDLE_TAG);
        if (!NewTable)
        {
            QzReleaseSpinLock(&QzHandleLock, OldIrql);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(NewTable, NewCapacity * sizeof(QZ_HANDLE_ENTRY));
        if (QzHandleTable)
        {
            RtlCopyMemory(NewTable, QzHandleTable, QzHandleCapacity * sizeof(QZ_HANDLE_ENTRY));
            ExFreePoolWithTag(QzHandleTable, QZ_HANDLE_TAG);
        }
        QzHandleHint = QzHandleCapacity;
        QzHandleTable = NewTable;
        QzHandleCapacity = NewCapacity;
    }

    for (Index = QzHandleHint; ; Index = (Index + 1) % QzHandleCapacity)
    {
        if (!QzHandleTable[Index].Object)
            break;
    }
    QzHandleTable[Index].Object = Object;
    QzHandleTable[Index].GrantedAccess = GrantedAccess;
    QzHandleTable[Index].Attributes = HandleAttributes;
    QzHandlesInUse++;
    QzHandleHint = (Index + 1) % QzHandleCapacity;

    InterlockedIncrement((PLONG)&Header->HandleCount);
    InterlockedIncrement((PLONG)&Header->Type->TotalNumberOfHandles);
    if (Header->Type->TotalNumberOfHandles > Header->Type->HighWaterNumberOfHandles)
        Header->Type->HighWaterNumberOfHandles = Header->Type->TotalNumberOfHandles;

    QzReleaseSpinLock(&QzHandleLock, OldIrql);
    *Handle = QzIndexToHandle(Index);
    return STATUS_SUCCESS;
}

/* Copy out an entry (the object is referenced if Reference is set) */
static BOOLEAN
QzObpLookupHandle(HANDLE Handle, PQZ_HANDLE_ENTRY Out, BOOLEAN Reference)
{
    LONG Index = QzHandleToIndex(Handle);
    BOOLEAN Found = FALSE;
    KIRQL OldIrql;

    if (Index < 0)
        return FALSE;

    OldIrql = QzAcquireSpinLock(&QzHandleLock);
    if ((ULONG)Index < QzHandleCapacity && QzHandleTable[Index].Object)
    {
        *Out = QzHandleTable[Index];
        if (Reference)
            ObfReferenceObject(Out->Object);
        Found = TRUE;
    }
    QzReleaseSpinLock(&QzHandleLock, OldIrql);
    return Found;
}

/* ---------------------------------------------------------------------- */
/* Exports                                                                 */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
ObReferenceObjectByHandle(IN HANDLE Handle,
                          IN ACCESS_MASK DesiredAccess,
                          IN POBJECT_TYPE ObjectType OPTIONAL,
                          IN KPROCESSOR_MODE AccessMode,
                          OUT PVOID *Object,
                          OUT POBJECT_HANDLE_INFORMATION HandleInformation OPTIONAL)
{
    QZ_HANDLE_ENTRY Entry;
    POBJECT_TYPE Type;

    *Object = NULL;

    /* NtCurrentProcess()/NtCurrentThread() need process objects (B6) */
    if (QzIsPseudoHandle(Handle) || !QzObpLookupHandle(Handle, &Entry, TRUE))
        return STATUS_INVALID_HANDLE;

    Type = OBJECT_TO_OBJECT_HEADER(Entry.Object)->Type;
    if (ObjectType && Type != ObjectType)
    {
        ObfDereferenceObject(Entry.Object);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }

    if (AccessMode != KernelMode &&
        (QzObpMapDesiredAccess(Type, DesiredAccess) & ~Entry.GrantedAccess))
    {
        ObfDereferenceObject(Entry.Object);
        return STATUS_ACCESS_DENIED;
    }

    if (HandleInformation)
    {
        HandleInformation->HandleAttributes = Entry.Attributes;
        HandleInformation->GrantedAccess = Entry.GrantedAccess;
    }
    *Object = Entry.Object;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
ObOpenObjectByPointer(IN PVOID Object,
                      IN ULONG HandleAttributes,
                      IN PACCESS_STATE PassedAccessState OPTIONAL,
                      IN ACCESS_MASK DesiredAccess,
                      IN POBJECT_TYPE ObjectType OPTIONAL,
                      IN KPROCESSOR_MODE AccessMode,
                      OUT PHANDLE Handle)
{
    POBJECT_TYPE Type = OBJECT_TO_OBJECT_HEADER(Object)->Type;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(PassedAccessState);
    UNREFERENCED_PARAMETER(AccessMode);

    if (ObjectType && Type != ObjectType)
        return STATUS_OBJECT_TYPE_MISMATCH;

    ObfReferenceObject(Object);
    Status = QzObpCreateHandle(Object, QzObpMapDesiredAccess(Type, DesiredAccess),
                               HandleAttributes & (OBJ_INHERIT | OBJ_KERNEL_HANDLE), Handle);
    if (!NT_SUCCESS(Status))
        ObfDereferenceObject(Object);
    return Status;
}

NTSTATUS
NTAPI
ObCloseHandle(IN HANDLE Handle,
              IN KPROCESSOR_MODE AccessMode)
{
    LONG Index = QzHandleToIndex(Handle);
    QZ_HANDLE_ENTRY Entry;
    POBJECT_HEADER Header;
    ULONG_PTR HandleCount;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(AccessMode);

    if (Index < 0 || QzIsPseudoHandle(Handle))
        return STATUS_INVALID_HANDLE;

    OldIrql = QzAcquireSpinLock(&QzHandleLock);
    if ((ULONG)Index >= QzHandleCapacity || !QzHandleTable[Index].Object)
    {
        QzReleaseSpinLock(&QzHandleLock, OldIrql);
        return STATUS_INVALID_HANDLE;
    }
    Entry = QzHandleTable[Index];
    QzHandleTable[Index].Object = NULL;
    QzHandlesInUse--;
    QzReleaseSpinLock(&QzHandleLock, OldIrql);

    Header = OBJECT_TO_OBJECT_HEADER(Entry.Object);
    HandleCount = Header->HandleCount;

    if (Header->Type->TypeInfo.CloseProcedure)
        Header->Type->TypeInfo.CloseProcedure(NULL, Entry.Object, Entry.GrantedAccess,
                                              HandleCount, HandleCount);

    InterlockedDecrement((PLONG)&Header->Type->TotalNumberOfHandles);
    if (InterlockedDecrement((PLONG)&Header->HandleCount) == 0)
        QzObpDeleteNameCheck(Entry.Object);

    ObfDereferenceObject(Entry.Object);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NtClose(IN HANDLE Handle)
{
    return ObCloseHandle(Handle, KernelMode);
}

NTSTATUS
NTAPI
ZwClose(IN HANDLE Handle)
{
    return ObCloseHandle(Handle, KernelMode);
}

NTSTATUS
NTAPI
NtDuplicateObject(IN HANDLE SourceProcessHandle,
                  IN HANDLE SourceHandle,
                  IN HANDLE TargetProcessHandle OPTIONAL,
                  OUT PHANDLE TargetHandle OPTIONAL,
                  IN ACCESS_MASK DesiredAccess,
                  IN ULONG HandleAttributes,
                  IN ULONG Options)
{
    QZ_HANDLE_ENTRY Entry;
    ACCESS_MASK Access;
    NTSTATUS Status = STATUS_SUCCESS;

    /* Only within the one handle table we have */
    if (SourceProcessHandle != NtCurrentProcess() ||
        (TargetProcessHandle && TargetProcessHandle != NtCurrentProcess()))
    {
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!QzObpLookupHandle(SourceHandle, &Entry, TRUE))
        return STATUS_INVALID_HANDLE;

    if (TargetHandle)
    {
        Access = (Options & DUPLICATE_SAME_ACCESS)
                     ? Entry.GrantedAccess
                     : QzObpMapDesiredAccess(OBJECT_TO_OBJECT_HEADER(Entry.Object)->Type,
                                             DesiredAccess);
        Status = QzObpCreateHandle(Entry.Object, Access,
                                   (Options & DUPLICATE_SAME_ATTRIBUTES) ? Entry.Attributes
                                                                         : HandleAttributes,
                                   TargetHandle);
        if (!NT_SUCCESS(Status))
            ObfDereferenceObject(Entry.Object);
    }
    else
    {
        ObfDereferenceObject(Entry.Object);
    }

    if (Options & DUPLICATE_CLOSE_SOURCE)
        ObCloseHandle(SourceHandle, KernelMode);
    return Status;
}

NTSTATUS
NTAPI
ZwDuplicateObject(IN HANDLE SourceProcessHandle,
                  IN HANDLE SourceHandle,
                  IN HANDLE TargetProcessHandle OPTIONAL,
                  OUT PHANDLE TargetHandle OPTIONAL,
                  IN ACCESS_MASK DesiredAccess,
                  IN ULONG HandleAttributes,
                  IN ULONG Options)
{
    return NtDuplicateObject(SourceProcessHandle, SourceHandle, TargetProcessHandle,
                             TargetHandle, DesiredAccess, HandleAttributes, Options);
}

NTSTATUS
NTAPI
ZwMakeTemporaryObject(IN HANDLE Handle)
{
    PVOID Object;
    NTSTATUS Status = ObReferenceObjectByHandle(Handle, DELETE, NULL, KernelMode, &Object, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    ObMakeTemporaryObject(Object);
    ObfDereferenceObject(Object);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NtMakePermanentObject(IN HANDLE Handle)
{
    PVOID Object;
    NTSTATUS Status = ObReferenceObjectByHandle(Handle, 0, NULL, KernelMode, &Object, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    OBJECT_TO_OBJECT_HEADER(Object)->Flags |= OB_FLAG_PERMANENT;
    ObfDereferenceObject(Object);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
ZwQueryObject(IN HANDLE Handle,
              IN OBJECT_INFORMATION_CLASS ObjectInformationClass,
              OUT PVOID ObjectInformation,
              IN ULONG Length,
              OUT PULONG ResultLength OPTIONAL)
{
    QZ_HANDLE_ENTRY Entry;
    POBJECT_HEADER Header;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Needed = 0;

    if (!QzObpLookupHandle(Handle, &Entry, TRUE))
        return STATUS_INVALID_HANDLE;
    Header = OBJECT_TO_OBJECT_HEADER(Entry.Object);

    switch (ObjectInformationClass)
    {
        case ObjectBasicInformation:
        {
            POBJECT_BASIC_INFORMATION Info = ObjectInformation;
            Needed = sizeof(*Info);
            if (Length < Needed)
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            RtlZeroMemory(Info, sizeof(*Info));
            Info->Attributes = Entry.Attributes |
                               ((Header->Flags & OB_FLAG_PERMANENT) ? OBJ_PERMANENT : 0);
            Info->GrantedAccess = Entry.GrantedAccess;
            Info->HandleCount = (ULONG)Header->HandleCount;
            Info->PointerCount = (ULONG)Header->PointerCount - 1;  /* minus our lookup */
            break;
        }

        case ObjectNameInformation:
            Status = ObQueryNameString(Entry.Object, ObjectInformation, Length, &Needed);
            break;

        case ObjectTypeInformation:
        {
            POBJECT_TYPE_INFORMATION Info = ObjectInformation;
            POBJECT_TYPE Type = Header->Type;
            Needed = sizeof(*Info) + Type->Name.MaximumLength;
            if (Length < Needed)
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            RtlZeroMemory(Info, sizeof(*Info));
            Info->TypeName.Buffer = (PWCHAR)(Info + 1);
            Info->TypeName.Length = Type->Name.Length;
            Info->TypeName.MaximumLength = Type->Name.MaximumLength;
            RtlCopyMemory(Info->TypeName.Buffer, Type->Name.Buffer, Type->Name.MaximumLength);
            Info->TotalNumberOfObjects = Type->TotalNumberOfObjects;
            Info->TotalNumberOfHandles = Type->TotalNumberOfHandles;
            Info->GenericMapping = Type->TypeInfo.GenericMapping;
            Info->ValidAccessMask = Type->TypeInfo.ValidAccessMask;
            Info->PoolType = Type->TypeInfo.PoolType;
            break;
        }

        default:
            Status = STATUS_INVALID_INFO_CLASS;
            break;
    }

    ObfDereferenceObject(Entry.Object);
    if (ResultLength)
        *ResultLength = Needed;
    return Status;
}
