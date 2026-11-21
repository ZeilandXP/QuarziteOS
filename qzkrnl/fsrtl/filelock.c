/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     File system runtime: byte-range locks
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * A FILE_LOCK's LockInformation is a list of granted locks and a queue of
 * lock requests waiting for a range (IRP_MN_LOCK without
 * FAIL_IMMEDIATELY). A lock's owner is its file object, process and key.
 * Shared locks keep everyone from writing the range; exclusive ones keep
 * other owners from reading or writing it, and from locking it.
 * FastIoIsQuestionable says whether any locks exist, which is what
 * FsRtlAreThereCurrentFileLocks reads.
 */

#include "qzio.h"

#define QZ_LOCK_TAG 'kLzQ'

typedef struct _QZ_LOCK
{
    LIST_ENTRY Link;
    FILE_LOCK_INFO Info;            /* StartingByte, Length, Key, ProcessId, FileObject, ExclusiveLock */
    LARGE_INTEGER End;              /* last byte, inclusive */
} QZ_LOCK, *PQZ_LOCK;

typedef struct _QZ_LOCK_STATE
{
    KSPIN_LOCK SpinLock;
    LIST_ENTRY Locks;
    LIST_ENTRY Waiting;             /* IRPs, through Tail.Overlay.ListEntry */
} QZ_LOCK_STATE, *PQZ_LOCK_STATE;

static PQZ_LOCK_STATE
QzLockState(PFILE_LOCK FileLock, BOOLEAN Create)
{
    PQZ_LOCK_STATE State = FileLock->LockInformation;
    if (!State && Create)
    {
        State = ExAllocatePoolWithTag(NonPagedPool, sizeof(*State), QZ_LOCK_TAG);
        if (!State)
            return NULL;
        KeInitializeSpinLock(&State->SpinLock);
        InitializeListHead(&State->Locks);
        InitializeListHead(&State->Waiting);
        if (InterlockedCompareExchangePointer(&FileLock->LockInformation, State, NULL))
        {
            ExFreePoolWithTag(State, QZ_LOCK_TAG);
            State = FileLock->LockInformation;
        }
    }
    return State;
}

VOID
NTAPI
FsRtlInitializeFileLock(IN PFILE_LOCK FileLock, IN PCOMPLETE_LOCK_IRP_ROUTINE CompleteLockIrpRoutine OPTIONAL,
                        IN PUNLOCK_ROUTINE UnlockRoutine OPTIONAL)
{
    RtlZeroMemory(FileLock, sizeof(*FileLock));
    FileLock->CompleteLockIrpRoutine = CompleteLockIrpRoutine;
    FileLock->UnlockRoutine = UnlockRoutine;
}

PFILE_LOCK
NTAPI
FsRtlAllocateFileLock(IN PCOMPLETE_LOCK_IRP_ROUTINE CompleteLockIrpRoutine OPTIONAL,
                      IN PUNLOCK_ROUTINE UnlockRoutine OPTIONAL)
{
    PFILE_LOCK FileLock = ExAllocatePoolWithTag(PagedPool, sizeof(FILE_LOCK), QZ_LOCK_TAG);
    if (FileLock)
        FsRtlInitializeFileLock(FileLock, CompleteLockIrpRoutine, UnlockRoutine);
    return FileLock;
}

static VOID
QzCompleteLockIrp(PFILE_LOCK FileLock, PVOID Context, PIRP Irp, NTSTATUS Status)
{
    Irp->IoStatus.Status = Status;
    if (FileLock->CompleteLockIrpRoutine)
        FileLock->CompleteLockIrpRoutine(Context, Irp);
    else
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

VOID
NTAPI
FsRtlUninitializeFileLock(IN PFILE_LOCK FileLock)
{
    PQZ_LOCK_STATE State = FileLock->LockInformation;
    if (!State)
        return;
    while (!IsListEmpty(&State->Locks))
        ExFreePoolWithTag(CONTAINING_RECORD(RemoveHeadList(&State->Locks), QZ_LOCK, Link), QZ_LOCK_TAG);
    while (!IsListEmpty(&State->Waiting))
    {
        PIRP Irp = CONTAINING_RECORD(RemoveHeadList(&State->Waiting), IRP, Tail.Overlay.ListEntry);
        IoSetCancelRoutine(Irp, NULL);
        QzCompleteLockIrp(FileLock, NULL, Irp, STATUS_RANGE_NOT_LOCKED);
    }
    ExFreePoolWithTag(State, QZ_LOCK_TAG);
    FileLock->LockInformation = NULL;
    FileLock->FastIoIsQuestionable = FALSE;
}

VOID
NTAPI
FsRtlFreeFileLock(IN PFILE_LOCK FileLock)
{
    FsRtlUninitializeFileLock(FileLock);
    ExFreePoolWithTag(FileLock, QZ_LOCK_TAG);
}

static BOOLEAN
QzSameOwner(const FILE_LOCK_INFO *Lock, PFILE_OBJECT FileObject, PVOID ProcessId, ULONG Key)
{
    return Lock->FileObject == FileObject && Lock->ProcessId == ProcessId && Lock->Key == Key;
}

static BOOLEAN
QzOverlaps(PQZ_LOCK Lock, LONGLONG Start, LONGLONG End)
{
    return Lock->Info.Length.QuadPart && Start <= Lock->End.QuadPart && Lock->Info.StartingByte.QuadPart <= End;
}

/* Can this owner have a lock on the range? (state lock held) */
static BOOLEAN
QzCanLock(PQZ_LOCK_STATE State, LONGLONG Start, LONGLONG Length, BOOLEAN Exclusive, PFILE_OBJECT FileObject,
          PVOID ProcessId, ULONG Key)
{
    PLIST_ENTRY Entry;
    if (!Length)
        return TRUE;
    for (Entry = State->Locks.Flink; Entry != &State->Locks; Entry = Entry->Flink)
    {
        PQZ_LOCK Lock = CONTAINING_RECORD(Entry, QZ_LOCK, Link);
        if (!QzOverlaps(Lock, Start, Start + Length - 1))
            continue;
        if ((Lock->Info.ExclusiveLock || Exclusive) && !QzSameOwner(&Lock->Info, FileObject, ProcessId, Key))
            return FALSE;
    }
    return TRUE;
}

static VOID
QzUpdateQuestionable(PFILE_LOCK FileLock, PQZ_LOCK_STATE State)
{
    FileLock->FastIoIsQuestionable = !IsListEmpty(&State->Locks) || !IsListEmpty(&State->Waiting);
}

static VOID QzRetryWaiters(PFILE_LOCK FileLock);

BOOLEAN
NTAPI
FsRtlPrivateLock(IN PFILE_LOCK FileLock, IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER FileOffset,
                 IN PLARGE_INTEGER Length, IN PEPROCESS Process, IN ULONG Key, IN BOOLEAN FailImmediately,
                 IN BOOLEAN ExclusiveLock, OUT PIO_STATUS_BLOCK IoStatus, IN PIRP Irp OPTIONAL,
                 IN PVOID Context OPTIONAL, IN BOOLEAN AlreadySynchronized)
{
    PQZ_LOCK_STATE State = QzLockState(FileLock, TRUE);
    PVOID ProcessId = Process ? PsGetProcessId(Process) : NULL;
    PQZ_LOCK Lock;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(AlreadySynchronized);
    if (!State || Length->QuadPart < 0 || FileOffset->QuadPart < 0 ||
        (Length->QuadPart && FileOffset->QuadPart + Length->QuadPart - 1 < FileOffset->QuadPart))
    {
        IoStatus->Status = State ? STATUS_INVALID_LOCK_RANGE : STATUS_INSUFFICIENT_RESOURCES;
        IoStatus->Information = 0;
        if (Irp)
            QzCompleteLockIrp(FileLock, Context, Irp, IoStatus->Status);
        return TRUE;
    }
    Lock = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Lock), QZ_LOCK_TAG);
    if (!Lock)
    {
        IoStatus->Status = STATUS_INSUFFICIENT_RESOURCES;
        if (Irp)
            QzCompleteLockIrp(FileLock, Context, Irp, IoStatus->Status);
        return TRUE;
    }
    Lock->Info.StartingByte = *FileOffset;
    Lock->Info.Length = *Length;
    Lock->Info.EndingByte.QuadPart = FileOffset->QuadPart + Length->QuadPart - 1;
    Lock->Info.Key = Key;
    Lock->Info.ProcessId = ProcessId;
    Lock->Info.FileObject = FileObject;
    Lock->Info.ExclusiveLock = ExclusiveLock;
    Lock->End = Lock->Info.EndingByte;

    KeAcquireSpinLock(&State->SpinLock, &OldIrql);
    if (QzCanLock(State, FileOffset->QuadPart, Length->QuadPart, ExclusiveLock, FileObject, ProcessId, Key))
    {
        InsertTailList(&State->Locks, &Lock->Link);
        QzUpdateQuestionable(FileLock, State);
        KeReleaseSpinLock(&State->SpinLock, OldIrql);
        IoStatus->Status = STATUS_SUCCESS;
        IoStatus->Information = 0;
        if (Irp)
            QzCompleteLockIrp(FileLock, Context, Irp, STATUS_SUCCESS);
        return TRUE;
    }
    ExFreePoolWithTag(Lock, QZ_LOCK_TAG);
    if (FailImmediately || !Irp)
    {
        KeReleaseSpinLock(&State->SpinLock, OldIrql);
        if (!Irp && !FailImmediately)
            return FALSE;                   /* fast I/O cannot wait: take the IRP path */
        IoStatus->Status = STATUS_LOCK_NOT_GRANTED;
        IoStatus->Information = 0;
        if (Irp)
            QzCompleteLockIrp(FileLock, Context, Irp, STATUS_LOCK_NOT_GRANTED);
        return TRUE;
    }

    /* Wait for the range; the request is retried at every unlock */
    IoMarkIrpPending(Irp);
    Irp->Tail.Overlay.DriverContext[0] = Context;
    InsertTailList(&State->Waiting, &Irp->Tail.Overlay.ListEntry);
    QzUpdateQuestionable(FileLock, State);
    KeReleaseSpinLock(&State->SpinLock, OldIrql);
    IoStatus->Status = STATUS_PENDING;
    return TRUE;
}

/* After an unlock: grant what can be granted now */
static VOID
QzRetryWaiters(PFILE_LOCK FileLock)
{
    PQZ_LOCK_STATE State = FileLock->LockInformation;
    LIST_ENTRY Retry;
    KIRQL OldIrql;

    if (!State)
        return;
    InitializeListHead(&Retry);
    KeAcquireSpinLock(&State->SpinLock, &OldIrql);
    while (!IsListEmpty(&State->Waiting))
        InsertTailList(&Retry, RemoveHeadList(&State->Waiting));
    KeReleaseSpinLock(&State->SpinLock, OldIrql);

    while (!IsListEmpty(&Retry))
    {
        PIRP Irp = CONTAINING_RECORD(RemoveHeadList(&Retry), IRP, Tail.Overlay.ListEntry);
        PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
        IO_STATUS_BLOCK Io;
        FsRtlPrivateLock(FileLock, Stack->FileObject, &Stack->Parameters.LockControl.ByteOffset,
                         Stack->Parameters.LockControl.Length, IoGetRequestorProcess(Irp),
                         Stack->Parameters.LockControl.Key, FALSE,
                         (Stack->Flags & SL_EXCLUSIVE_LOCK) != 0, &Io, Irp, Irp->Tail.Overlay.DriverContext[0],
                         FALSE);
    }
}

static NTSTATUS
QzUnlock(PFILE_LOCK FileLock, PFILE_OBJECT FileObject, PLARGE_INTEGER Offset, PLARGE_INTEGER Length,
         PVOID ProcessId, BOOLEAN AnyKey, ULONG Key, BOOLEAN All, PVOID Context)
{
    PQZ_LOCK_STATE State = FileLock->LockInformation;
    LIST_ENTRY Removed;
    PLIST_ENTRY Entry, Next;
    KIRQL OldIrql;
    BOOLEAN Found = FALSE;

    if (!State)
        return All ? STATUS_SUCCESS : STATUS_RANGE_NOT_LOCKED;
    InitializeListHead(&Removed);
    KeAcquireSpinLock(&State->SpinLock, &OldIrql);
    for (Entry = State->Locks.Flink; Entry != &State->Locks; Entry = Next)
    {
        PQZ_LOCK Lock = CONTAINING_RECORD(Entry, QZ_LOCK, Link);
        Next = Entry->Flink;
        if (Lock->Info.FileObject != FileObject || Lock->Info.ProcessId != ProcessId ||
            (!AnyKey && Lock->Info.Key != Key))
            continue;
        if (!All && (Lock->Info.StartingByte.QuadPart != Offset->QuadPart ||
                     Lock->Info.Length.QuadPart != Length->QuadPart))
            continue;
        RemoveEntryList(Entry);
        InsertTailList(&Removed, Entry);
        Found = TRUE;
        if (!All)
            break;
    }
    QzUpdateQuestionable(FileLock, State);
    KeReleaseSpinLock(&State->SpinLock, OldIrql);

    while (!IsListEmpty(&Removed))
    {
        PQZ_LOCK Lock = CONTAINING_RECORD(RemoveHeadList(&Removed), QZ_LOCK, Link);
        if (FileLock->UnlockRoutine)
            FileLock->UnlockRoutine(Context, &Lock->Info);
        ExFreePoolWithTag(Lock, QZ_LOCK_TAG);
    }
    if (Found)
        QzRetryWaiters(FileLock);
    return (Found || All) ? STATUS_SUCCESS : STATUS_RANGE_NOT_LOCKED;
}

NTSTATUS
NTAPI
FsRtlFastUnlockSingle(IN PFILE_LOCK FileLock, IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER FileOffset,
                      IN PLARGE_INTEGER Length, IN PEPROCESS Process, IN ULONG Key, IN PVOID Context OPTIONAL,
                      IN BOOLEAN AlreadySynchronized)
{
    UNREFERENCED_PARAMETER(AlreadySynchronized);
    return QzUnlock(FileLock, FileObject, FileOffset, Length, Process ? PsGetProcessId(Process) : NULL, FALSE, Key,
                    FALSE, Context);
}

/* Every lock of this file object and process; waiting requests of theirs fail */
static NTSTATUS
QzUnlockAll(PFILE_LOCK FileLock, PFILE_OBJECT FileObject, PEPROCESS Process, BOOLEAN AnyKey, ULONG Key,
            PVOID Context)
{
    PQZ_LOCK_STATE State = FileLock->LockInformation;
    PVOID ProcessId = Process ? PsGetProcessId(Process) : NULL;
    NTSTATUS Status = QzUnlock(FileLock, FileObject, NULL, NULL, ProcessId, AnyKey, Key, TRUE, Context);
    LIST_ENTRY Failed;
    PLIST_ENTRY Entry, Next;
    KIRQL OldIrql;

    if (!State || !AnyKey)
        return Status;
    InitializeListHead(&Failed);
    KeAcquireSpinLock(&State->SpinLock, &OldIrql);
    for (Entry = State->Waiting.Flink; Entry != &State->Waiting; Entry = Next)
    {
        PIRP Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        Next = Entry->Flink;
        if (IoGetCurrentIrpStackLocation(Irp)->FileObject == FileObject)
        {
            RemoveEntryList(Entry);
            InsertTailList(&Failed, Entry);
        }
    }
    QzUpdateQuestionable(FileLock, State);
    KeReleaseSpinLock(&State->SpinLock, OldIrql);
    while (!IsListEmpty(&Failed))
    {
        PIRP Irp = CONTAINING_RECORD(RemoveHeadList(&Failed), IRP, Tail.Overlay.ListEntry);
        QzCompleteLockIrp(FileLock, Irp->Tail.Overlay.DriverContext[0], Irp, STATUS_RANGE_NOT_LOCKED);
    }
    return Status;
}

NTSTATUS
NTAPI
FsRtlFastUnlockAll(IN PFILE_LOCK FileLock, IN PFILE_OBJECT FileObject, IN PEPROCESS Process,
                   IN PVOID Context OPTIONAL)
{
    return QzUnlockAll(FileLock, FileObject, Process, TRUE, 0, Context);
}

NTSTATUS
NTAPI
FsRtlFastUnlockAllByKey(IN PFILE_LOCK FileLock, IN PFILE_OBJECT FileObject, IN PEPROCESS Process, IN ULONG Key,
                        IN PVOID Context OPTIONAL)
{
    return QzUnlockAll(FileLock, FileObject, Process, FALSE, Key, Context);
}

/* IRP_MJ_LOCK_CONTROL */
NTSTATUS
NTAPI
FsRtlProcessFileLock(IN PFILE_LOCK FileLock, IN PIRP Irp, IN PVOID Context OPTIONAL)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PEPROCESS Process = IoGetRequestorProcess(Irp);
    IO_STATUS_BLOCK Io;
    NTSTATUS Status;

    switch (Stack->MinorFunction)
    {
        case IRP_MN_LOCK:
            FsRtlPrivateLock(FileLock, Stack->FileObject, &Stack->Parameters.LockControl.ByteOffset,
                             Stack->Parameters.LockControl.Length, Process, Stack->Parameters.LockControl.Key,
                             (Stack->Flags & SL_FAIL_IMMEDIATELY) != 0, (Stack->Flags & SL_EXCLUSIVE_LOCK) != 0,
                             &Io, Irp, Context, FALSE);
            return Io.Status;
        case IRP_MN_UNLOCK_SINGLE:
            Status = FsRtlFastUnlockSingle(FileLock, Stack->FileObject, &Stack->Parameters.LockControl.ByteOffset,
                                           Stack->Parameters.LockControl.Length, Process,
                                           Stack->Parameters.LockControl.Key, Context, FALSE);
            break;
        case IRP_MN_UNLOCK_ALL:
            Status = FsRtlFastUnlockAll(FileLock, Stack->FileObject, Process, Context);
            break;
        case IRP_MN_UNLOCK_ALL_BY_KEY:
            Status = FsRtlFastUnlockAllByKey(FileLock, Stack->FileObject, Process,
                                             Stack->Parameters.LockControl.Key, Context);
            break;
        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }
    Irp->IoStatus.Information = 0;
    QzCompleteLockIrp(FileLock, Context, Irp, Status);
    return Status;
}

/* ---------------------------------------------------------------------- */
/* Checks for reads and writes                                             */
/* ---------------------------------------------------------------------- */

static BOOLEAN
QzCheckAccess(PFILE_LOCK FileLock, LONGLONG Start, ULONG Length, ULONG Key, PFILE_OBJECT FileObject, PVOID ProcessId,
              BOOLEAN Write)
{
    PQZ_LOCK_STATE State = FileLock->LockInformation;
    PLIST_ENTRY Entry;
    BOOLEAN Ok = TRUE;
    KIRQL OldIrql;

    if (!State || !Length)
        return TRUE;
    KeAcquireSpinLock(&State->SpinLock, &OldIrql);
    for (Entry = State->Locks.Flink; Entry != &State->Locks; Entry = Entry->Flink)
    {
        PQZ_LOCK Lock = CONTAINING_RECORD(Entry, QZ_LOCK, Link);
        BOOLEAN Mine;
        if (!QzOverlaps(Lock, Start, Start + Length - 1))
            continue;
        Mine = QzSameOwner(&Lock->Info, FileObject, ProcessId, Key);
        if (Lock->Info.ExclusiveLock ? !Mine : Write)
        {
            Ok = FALSE;
            break;
        }
    }
    KeReleaseSpinLock(&State->SpinLock, OldIrql);
    return Ok;
}

BOOLEAN
NTAPI
FsRtlFastCheckLockForRead(IN PFILE_LOCK FileLock, IN PLARGE_INTEGER FileOffset, IN PLARGE_INTEGER Length,
                          IN ULONG Key, IN PFILE_OBJECT FileObject, IN PVOID Process)
{
    return QzCheckAccess(FileLock, FileOffset->QuadPart, Length->LowPart, Key, FileObject,
                         Process ? PsGetProcessId(Process) : NULL, FALSE);
}

BOOLEAN
NTAPI
FsRtlFastCheckLockForWrite(IN PFILE_LOCK FileLock, IN PLARGE_INTEGER FileOffset, IN PLARGE_INTEGER Length,
                           IN ULONG Key, IN PFILE_OBJECT FileObject, IN PVOID Process)
{
    return QzCheckAccess(FileLock, FileOffset->QuadPart, Length->LowPart, Key, FileObject,
                         Process ? PsGetProcessId(Process) : NULL, TRUE);
}

BOOLEAN
NTAPI
FsRtlCheckLockForReadAccess(IN PFILE_LOCK FileLock, IN PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PEPROCESS Process = IoGetRequestorProcess(Irp);
    return QzCheckAccess(FileLock, Stack->Parameters.Read.ByteOffset.QuadPart, Stack->Parameters.Read.Length,
                         Stack->Parameters.Read.Key, Stack->FileObject, Process ? PsGetProcessId(Process) : NULL,
                         FALSE);
}

BOOLEAN
NTAPI
FsRtlCheckLockForWriteAccess(IN PFILE_LOCK FileLock, IN PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PEPROCESS Process = IoGetRequestorProcess(Irp);
    return QzCheckAccess(FileLock, Stack->Parameters.Write.ByteOffset.QuadPart, Stack->Parameters.Write.Length,
                         Stack->Parameters.Write.Key, Stack->FileObject, Process ? PsGetProcessId(Process) : NULL,
                         TRUE);
}

/* The locks one by one (Restart starts over) */
PFILE_LOCK_INFO
NTAPI
FsRtlGetNextFileLock(IN PFILE_LOCK FileLock, IN BOOLEAN Restart)
{
    PQZ_LOCK_STATE State = FileLock->LockInformation;
    PLIST_ENTRY Entry;
    PQZ_LOCK Lock = NULL;
    KIRQL OldIrql;

    if (!State)
        return NULL;
    KeAcquireSpinLock(&State->SpinLock, &OldIrql);
    Entry = (Restart || !FileLock->LastReturnedLock) ? State->Locks.Flink
                                                    : ((PQZ_LOCK)FileLock->LastReturnedLock)->Link.Flink;
    if (Entry != &State->Locks)
    {
        Lock = CONTAINING_RECORD(Entry, QZ_LOCK, Link);
        FileLock->LastReturnedLockInfo = Lock->Info;
    }
    FileLock->LastReturnedLock = Lock;
    KeReleaseSpinLock(&State->SpinLock, OldIrql);
    return Lock ? &FileLock->LastReturnedLockInfo : NULL;
}

BOOLEAN
NTAPI
FsRtlAreThereCurrentOrInProgressFileLocks(IN PFILE_LOCK FileLock)
{
    return FileLock->FastIoIsQuestionable;
}
