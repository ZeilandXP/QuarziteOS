/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Handles: the (system) handle table and handle-based services
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Every process has a handle table. Kernel handles (OBJ_KERNEL_HANDLE, or
 * any handle made while the System process is current) live in the System
 * process's table and carry the kernel-handle bit (0x80000000), so they
 * work from any process context, as on NT. Other handles are multiples of
 * 4 in the current process's table; user mode cannot use kernel handles.
 */

#include "qz.h"
#include "qzps.h"

#define QZ_HANDLE_TAG           'dHbO'
#define QZ_KERNEL_HANDLE_BIT    0x80000000UL
#define QZ_INITIAL_HANDLES      64

typedef struct _QZ_HANDLE_ENTRY
{
    PVOID Object;               /* NULL: free */
    ACCESS_MASK GrantedAccess;
    ULONG Attributes;
} QZ_HANDLE_ENTRY, *PQZ_HANDLE_ENTRY;

typedef struct _QZ_HANDLE_TABLE
{
    PQZ_HANDLE_ENTRY Entries;
    ULONG Capacity;
    ULONG InUse;
    ULONG Hint;
    BOOLEAN Kernel;
    KSPIN_LOCK Lock;
} QZ_HANDLE_TABLE;

static QZ_HANDLE_TABLE QzKernelHandleTable = { NULL, 0, 0, 0, TRUE, 0 };

PQZ_HANDLE_TABLE
QzObKernelHandleTable(VOID)
{
    return &QzKernelHandleTable;
}

PQZ_HANDLE_TABLE
QzObCreateHandleTable(VOID)
{
    PQZ_HANDLE_TABLE Table = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Table), QZ_HANDLE_TAG);
    if (Table)
        RtlZeroMemory(Table, sizeof(*Table));
    return Table;
}

ULONG
QzObTableHandleCount(PQZ_HANDLE_TABLE Table)
{
    return Table->InUse;
}

/* The table of the process whose context we are in */
static PQZ_HANDLE_TABLE
QzObpCurrentTable(VOID)
{
    PQZ_PROCESS Process = QzCurrentThread ? QzCurrentThread->ApcProcess : NULL;
    return (Process && Process->HandleTable) ? Process->HandleTable : &QzKernelHandleTable;
}

ULONG
QzObHandleCount(VOID)
{
    return QzKernelHandleTable.InUse;
}

static __inline HANDLE
QzIndexToHandle(PQZ_HANDLE_TABLE Table, ULONG Index)
{
    return (HANDLE)(ULONG_PTR)((Table->Kernel ? QZ_KERNEL_HANDLE_BIT : 0) | ((Index + 1) << 2));
}

/* Which table a handle value names, from AccessMode; NULL if none */
static PQZ_HANDLE_TABLE
QzObpTableOf(HANDLE Handle, KPROCESSOR_MODE AccessMode, PLONG Index)
{
    ULONG Value = (ULONG)(ULONG_PTR)Handle;
    PQZ_HANDLE_TABLE Table;

    if (Value & QZ_KERNEL_HANDLE_BIT)
    {
        if (AccessMode != KernelMode)
            return NULL;
        Table = &QzKernelHandleTable;
        Value &= ~QZ_KERNEL_HANDLE_BIT;
    }
    else
    {
        Table = QzObpCurrentTable();
    }
    if (Value == 0 || (Value & 3))
        return NULL;
    *Index = (LONG)(Value >> 2) - 1;
    return Table;
}

static BOOLEAN
QzIsPseudoHandle(HANDLE Handle)
{
    return Handle == NtCurrentProcess() || Handle == NtCurrentThread();
}

NTSTATUS
QzObpCreateHandleEx(PQZ_HANDLE_TABLE Table, PVOID Object, ACCESS_MASK GrantedAccess,
                    ULONG HandleAttributes, PHANDLE Handle)
{
    POBJECT_HEADER Header = OBJECT_TO_OBJECT_HEADER(Object);
    PQZ_HANDLE_ENTRY NewTable;
    ULONG Index, NewCapacity;
    KIRQL OldIrql;

    OldIrql = QzAcquireSpinLock(&Table->Lock);

    if (Table->InUse == Table->Capacity)
    {
        NewCapacity = Table->Capacity ? Table->Capacity * 2 : QZ_INITIAL_HANDLES;
        NewTable = ExAllocatePoolWithTag(NonPagedPool, NewCapacity * sizeof(QZ_HANDLE_ENTRY),
                                         QZ_HANDLE_TAG);
        if (!NewTable)
        {
            QzReleaseSpinLock(&Table->Lock, OldIrql);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(NewTable, NewCapacity * sizeof(QZ_HANDLE_ENTRY));
        if (Table->Entries)
        {
            RtlCopyMemory(NewTable, Table->Entries, Table->Capacity * sizeof(QZ_HANDLE_ENTRY));
            ExFreePoolWithTag(Table->Entries, QZ_HANDLE_TAG);
        }
        Table->Hint = Table->Capacity;
        Table->Entries = NewTable;
        Table->Capacity = NewCapacity;
    }

    for (Index = Table->Hint; ; Index = (Index + 1) % Table->Capacity)
    {
        if (!Table->Entries[Index].Object)
            break;
    }
    Table->Entries[Index].Object = Object;
    Table->Entries[Index].GrantedAccess = GrantedAccess;
    Table->Entries[Index].Attributes = HandleAttributes & ~OBJ_KERNEL_HANDLE;
    Table->InUse++;
    Table->Hint = (Index + 1) % Table->Capacity;

    InterlockedIncrement((PLONG)&Header->HandleCount);
    InterlockedIncrement((PLONG)&Header->Type->TotalNumberOfHandles);
    if (Header->Type->TotalNumberOfHandles > Header->Type->HighWaterNumberOfHandles)
        Header->Type->HighWaterNumberOfHandles = Header->Type->TotalNumberOfHandles;

    QzReleaseSpinLock(&Table->Lock, OldIrql);
    *Handle = QzIndexToHandle(Table, Index);
    return STATUS_SUCCESS;
}

/* OBJ_KERNEL_HANDLE goes to the kernel table, the rest to the current process */
NTSTATUS
QzObpCreateHandle(PVOID Object, ACCESS_MASK GrantedAccess, ULONG HandleAttributes,
                  PHANDLE Handle)
{
    PQZ_HANDLE_TABLE Table = (HandleAttributes & OBJ_KERNEL_HANDLE) ? &QzKernelHandleTable
                                                                    : QzObpCurrentTable();
    return QzObpCreateHandleEx(Table, Object, GrantedAccess, HandleAttributes, Handle);
}

/* Copy out an entry (the object is referenced if Reference is set) */
static BOOLEAN
QzObpLookupHandleEx(HANDLE Handle, KPROCESSOR_MODE AccessMode, PQZ_HANDLE_ENTRY Out, BOOLEAN Reference)
{
    LONG Index = -1;
    PQZ_HANDLE_TABLE Table = QzObpTableOf(Handle, AccessMode, &Index);
    BOOLEAN Found = FALSE;
    KIRQL OldIrql;

    if (!Table || Index < 0)
        return FALSE;

    OldIrql = QzAcquireSpinLock(&Table->Lock);
    if ((ULONG)Index < Table->Capacity && Table->Entries[Index].Object)
    {
        *Out = Table->Entries[Index];
        if (Reference)
            ObfReferenceObject(Out->Object);
        Found = TRUE;
    }
    QzReleaseSpinLock(&Table->Lock, OldIrql);
    return Found;
}

static BOOLEAN
QzObpLookupHandle(HANDLE Handle, PQZ_HANDLE_ENTRY Out, BOOLEAN Reference)
{
    return QzObpLookupHandleEx(Handle, KernelMode, Out, Reference);
}

/* Pseudo-handles: the current process and thread */
static PVOID
QzObpPseudoObject(HANDLE Handle)
{
    if (Handle == NtCurrentProcess())
        return QzCurrentThread->ApcProcess;
    if (Handle == NtCurrentThread())
        return QzCurrentThread->PsThread;
    return NULL;
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

    if (QzIsPseudoHandle(Handle))
    {
        PVOID Pseudo = QzObpPseudoObject(Handle);
        if (!Pseudo)
            return STATUS_INVALID_HANDLE;
        if (ObjectType && OBJECT_TO_OBJECT_HEADER(Pseudo)->Type != ObjectType)
            return STATUS_OBJECT_TYPE_MISMATCH;
        ObfReferenceObject(Pseudo);
        if (HandleInformation)
        {
            HandleInformation->HandleAttributes = 0;
            HandleInformation->GrantedAccess = OBJECT_TO_OBJECT_HEADER(Pseudo)->Type->TypeInfo.ValidAccessMask;
        }
        *Object = Pseudo;
        return STATUS_SUCCESS;
    }
    if (!QzObpLookupHandleEx(Handle, AccessMode, &Entry, TRUE))
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

static NTSTATUS
QzObpCloseEntry(PQZ_HANDLE_TABLE Table, LONG Index)
{
    QZ_HANDLE_ENTRY Entry;
    POBJECT_HEADER Header;
    ULONG_PTR HandleCount;
    KIRQL OldIrql;

    OldIrql = QzAcquireSpinLock(&Table->Lock);
    if (Index < 0 || (ULONG)Index >= Table->Capacity || !Table->Entries[Index].Object)
    {
        QzReleaseSpinLock(&Table->Lock, OldIrql);
        return STATUS_INVALID_HANDLE;
    }
    Entry = Table->Entries[Index];
    Table->Entries[Index].Object = NULL;
    Table->InUse--;
    QzReleaseSpinLock(&Table->Lock, OldIrql);

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
ObCloseHandle(IN HANDLE Handle,
              IN KPROCESSOR_MODE AccessMode)
{
    LONG Index = -1;
    PQZ_HANDLE_TABLE Table;

    if (QzIsPseudoHandle(Handle))
        return STATUS_INVALID_HANDLE;
    Table = QzObpTableOf(Handle, AccessMode, &Index);
    if (!Table)
        return STATUS_INVALID_HANDLE;
    return QzObpCloseEntry(Table, Index);
}

/* A process is going away: close every handle in its table, free it */
VOID
QzObKillHandleTable(PQZ_HANDLE_TABLE Table)
{
    ULONG Index;

    if (!Table || Table == &QzKernelHandleTable)
        return;
    for (Index = 0; Index < Table->Capacity; Index++)
        if (Table->Entries[Index].Object)
            QzObpCloseEntry(Table, (LONG)Index);
    if (Table->Entries)
        ExFreePoolWithTag(Table->Entries, QZ_HANDLE_TAG);
    ExFreePoolWithTag(Table, QZ_HANDLE_TAG);
}

/* A new process inherits its parent's OBJ_INHERIT handles, with the same values */
NTSTATUS
QzObInheritHandles(PQZ_HANDLE_TABLE Parent, PQZ_HANDLE_TABLE Child)
{
    ULONG Index;
    KIRQL OldIrql;

    if (!Parent || !Child || Parent == &QzKernelHandleTable)
        return STATUS_SUCCESS;
    if (Parent->Capacity)
    {
        Child->Entries = ExAllocatePoolWithTag(NonPagedPool, Parent->Capacity * sizeof(QZ_HANDLE_ENTRY),
                                               QZ_HANDLE_TAG);
        if (!Child->Entries)
            return STATUS_INSUFFICIENT_RESOURCES;
        RtlZeroMemory(Child->Entries, Parent->Capacity * sizeof(QZ_HANDLE_ENTRY));
        Child->Capacity = Parent->Capacity;
    }
    OldIrql = QzAcquireSpinLock(&Parent->Lock);
    for (Index = 0; Index < Parent->Capacity; Index++)
    {
        QZ_HANDLE_ENTRY *Entry = &Parent->Entries[Index];
        POBJECT_HEADER Header;
        if (!Entry->Object || !(Entry->Attributes & OBJ_INHERIT))
            continue;
        Header = OBJECT_TO_OBJECT_HEADER(Entry->Object);
        ObfReferenceObject(Entry->Object);
        InterlockedIncrement((PLONG)&Header->HandleCount);
        InterlockedIncrement((PLONG)&Header->Type->TotalNumberOfHandles);
        Child->Entries[Index] = *Entry;
        Child->InUse++;
    }
    QzReleaseSpinLock(&Parent->Lock, OldIrql);
    return STATUS_SUCCESS;
}

/* Look a handle up in another process's table (NtDuplicateObject) */
static BOOLEAN
QzObpLookupInTable(PQZ_HANDLE_TABLE Table, HANDLE Handle, PQZ_HANDLE_ENTRY Out)
{
    ULONG Value = (ULONG)(ULONG_PTR)Handle;
    LONG Index;
    BOOLEAN Found = FALSE;
    KIRQL OldIrql;

    if (Value & QZ_KERNEL_HANDLE_BIT)
    {
        Table = &QzKernelHandleTable;
        Value &= ~QZ_KERNEL_HANDLE_BIT;
    }
    if (!Value || (Value & 3))
        return FALSE;
    Index = (LONG)(Value >> 2) - 1;
    OldIrql = QzAcquireSpinLock(&Table->Lock);
    if ((ULONG)Index < Table->Capacity && Table->Entries[Index].Object)
    {
        *Out = Table->Entries[Index];
        ObfReferenceObject(Out->Object);
        Found = TRUE;
    }
    QzReleaseSpinLock(&Table->Lock, OldIrql);
    return Found;
}

NTSTATUS
NTAPI
NtClose(IN HANDLE Handle)
{
    return ObCloseHandle(Handle, ExGetPreviousMode());
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
    PQZ_PROCESS Source, Target = NULL;
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    NTSTATUS Status;

    Status = ObReferenceObjectByHandle(SourceProcessHandle, PROCESS_DUP_HANDLE, PsProcessType, Mode,
                                       (PVOID *)&Source, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    if (TargetProcessHandle)
    {
        Status = ObReferenceObjectByHandle(TargetProcessHandle, PROCESS_DUP_HANDLE, PsProcessType, Mode,
                                           (PVOID *)&Target, NULL);
        if (!NT_SUCCESS(Status))
        {
            ObDereferenceObject(Source);
            return Status;
        }
    }

    if (QzIsPseudoHandle(SourceHandle) && Source == QzCurrentThread->ApcProcess)
    {
        Entry.Object = QzObpPseudoObject(SourceHandle);
        if (!Entry.Object)
            Status = STATUS_INVALID_HANDLE;
        else
        {
            ObfReferenceObject(Entry.Object);
            Entry.GrantedAccess = OBJECT_TO_OBJECT_HEADER(Entry.Object)->Type->TypeInfo.ValidAccessMask;
            Entry.Attributes = 0;
        }
    }
    else if (!QzObpLookupInTable(Source->HandleTable, SourceHandle, &Entry))
    {
        Status = STATUS_INVALID_HANDLE;
    }
    if (!NT_SUCCESS(Status))
        goto Done;

    if (Target)
    {
        PQZ_HANDLE_TABLE Table = Target->HandleTable;
        Access = (Options & DUPLICATE_SAME_ACCESS)
                     ? Entry.GrantedAccess
                     : QzObpMapDesiredAccess(OBJECT_TO_OBJECT_HEADER(Entry.Object)->Type,
                                             DesiredAccess);
        if (!(Options & DUPLICATE_SAME_ATTRIBUTES) && (HandleAttributes & OBJ_KERNEL_HANDLE) &&
            Mode == KernelMode)
            Table = &QzKernelHandleTable;
        if (TargetHandle)
            Status = QzObpCreateHandleEx(Table, Entry.Object, Access,
                                         (Options & DUPLICATE_SAME_ATTRIBUTES) ? Entry.Attributes
                                                                               : HandleAttributes,
                                         TargetHandle);
        else
            Status = STATUS_INVALID_PARAMETER;
        if (!NT_SUCCESS(Status))
            ObfDereferenceObject(Entry.Object);
    }
    else
    {
        ObfDereferenceObject(Entry.Object);
    }

    if ((Options & DUPLICATE_CLOSE_SOURCE) && !QzIsPseudoHandle(SourceHandle))
    {
        ULONG Value = (ULONG)(ULONG_PTR)SourceHandle & ~QZ_KERNEL_HANDLE_BIT;
        PQZ_HANDLE_TABLE Table = ((ULONG_PTR)SourceHandle & QZ_KERNEL_HANDLE_BIT) ? &QzKernelHandleTable
                                                                                 : Source->HandleTable;
        if (Value && !(Value & 3))
            QzObpCloseEntry(Table, (LONG)(Value >> 2) - 1);
    }
Done:
    if (Target)
        ObDereferenceObject(Target);
    ObDereferenceObject(Source);
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
