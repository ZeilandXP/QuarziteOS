/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     File system runtime: pool, status codes, oplocks, tunnelling,
 *              change notification, fast I/O, stack overflow posting
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * What is simplified, and why it is safe for the file systems we run:
 *  - Oplocks are never granted (FsRtlOplockFsctrl says so), so there is
 *    nothing to break: FsRtlCheckOplock lets every request through.
 *  - The tunnel cache keeps nothing: a file deleted and recreated gets a
 *    new short name and creation time, which is what a cache miss does.
 *  - Change notification completes a waiting request with
 *    STATUS_NOTIFY_ENUM_DIR ("something changed, list it again") rather
 *    than a list of changes; callers handle that status by design.
 */

#include "qzio.h"
#include "qzps.h"

#define QZ_FSRTL_TAG 'tRzQ'

/* The kernel's own names for KeEnter/LeaveCriticalRegion (ke/sched.c) */
VOID NTAPI QzKeEnterCriticalRegion(VOID);
VOID NTAPI QzKeLeaveCriticalRegion(VOID);
#undef FsRtlEnterFileSystem
#undef FsRtlExitFileSystem
#define FsRtlEnterFileSystem() QzKeEnterCriticalRegion()
#define FsRtlExitFileSystem() QzKeLeaveCriticalRegion()

/* ---------------------------------------------------------------------- */
/* Pool and status codes                                                   */
/* ---------------------------------------------------------------------- */

PVOID
NTAPI
FsRtlAllocatePoolWithTag(IN POOL_TYPE PoolType, IN ULONG NumberOfBytes, IN ULONG Tag)
{
    PVOID Block = ExAllocatePoolWithTag(PoolType, NumberOfBytes, Tag);
    if (!Block)
        ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    return Block;
}

PVOID
NTAPI
FsRtlAllocatePoolWithQuotaTag(IN POOL_TYPE PoolType, IN ULONG NumberOfBytes, IN ULONG Tag)
{
    return FsRtlAllocatePoolWithTag(PoolType, NumberOfBytes, Tag);
}

PVOID
NTAPI
FsRtlAllocatePool(IN POOL_TYPE PoolType, IN ULONG NumberOfBytes)
{
    return FsRtlAllocatePoolWithTag(PoolType, NumberOfBytes, 'eliF');
}

/* Exceptions a file system expects (and handles) versus genuine bugs */
BOOLEAN
NTAPI
FsRtlIsNtstatusExpected(IN NTSTATUS Status)
{
    return Status != STATUS_DATATYPE_MISALIGNMENT && Status != STATUS_ACCESS_VIOLATION &&
           Status != STATUS_ILLEGAL_INSTRUCTION && Status != STATUS_INSTRUCTION_MISALIGNMENT;
}

NTSTATUS
NTAPI
FsRtlNormalizeNtstatus(IN NTSTATUS NtStatusToNormalize, IN NTSTATUS NormalizedNtStatus)
{
    return FsRtlIsNtstatusExpected(NtStatusToNormalize) ? NtStatusToNormalize : NormalizedNtStatus;
}

BOOLEAN
NTAPI
FsRtlIsTotalDeviceFailure(IN NTSTATUS NtStatus)
{
    return !(NT_SUCCESS(NtStatus) || NtStatus == STATUS_CRC_ERROR || NtStatus == STATUS_DEVICE_DATA_ERROR);
}

/* No fault-tolerant mirror drivers */
NTSTATUS
NTAPI
FsRtlBalanceReads(IN PDEVICE_OBJECT TargetDevice)
{
    UNREFERENCED_PARAMETER(TargetDevice);
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* ---------------------------------------------------------------------- */
/* Oplocks: none are granted                                               */
/* ---------------------------------------------------------------------- */

VOID
NTAPI
FsRtlInitializeOplock(IN OUT POPLOCK Oplock)
{
    *Oplock = NULL;
}

VOID
NTAPI
FsRtlUninitializeOplock(IN POPLOCK Oplock)
{
    *Oplock = NULL;
}

NTSTATUS
NTAPI
FsRtlCheckOplock(IN POPLOCK Oplock, IN PIRP Irp, IN PVOID Context, IN POPLOCK_WAIT_COMPLETE_ROUTINE CompletionRoutine,
                 IN POPLOCK_FS_PREPOST_IRP PostIrpRoutine)
{
    UNREFERENCED_PARAMETER(Oplock);
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(CompletionRoutine);
    UNREFERENCED_PARAMETER(PostIrpRoutine);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
FsRtlOplockFsctrl(IN POPLOCK Oplock, IN PIRP Irp, IN ULONG OpenCount)
{
    UNREFERENCED_PARAMETER(Oplock);
    UNREFERENCED_PARAMETER(OpenCount);
    Irp->IoStatus.Status = STATUS_OPLOCK_NOT_GRANTED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_OPLOCK_NOT_GRANTED;
}

BOOLEAN
NTAPI
FsRtlOplockIsFastIoPossible(IN POPLOCK Oplock)
{
    UNREFERENCED_PARAMETER(Oplock);
    return TRUE;
}

BOOLEAN
NTAPI
FsRtlCurrentBatchOplock(IN POPLOCK Oplock)
{
    UNREFERENCED_PARAMETER(Oplock);
    return FALSE;
}

/* ---------------------------------------------------------------------- */
/* Tunnel cache: empty                                                     */
/* ---------------------------------------------------------------------- */

VOID
NTAPI
FsRtlInitializeTunnelCache(IN PTUNNEL Cache)
{
    RtlZeroMemory(Cache, sizeof(*Cache));
}

VOID
NTAPI
FsRtlDeleteTunnelCache(IN PTUNNEL Cache)
{
    UNREFERENCED_PARAMETER(Cache);
}

VOID
NTAPI
FsRtlAddToTunnelCache(IN PTUNNEL Cache, IN ULONGLONG DirectoryKey, IN PUNICODE_STRING ShortName,
                      IN PUNICODE_STRING LongName, IN BOOLEAN KeyByShortName, IN ULONG DataLength, IN PVOID Data)
{
    UNREFERENCED_PARAMETER(Cache);
    UNREFERENCED_PARAMETER(DirectoryKey);
    UNREFERENCED_PARAMETER(ShortName);
    UNREFERENCED_PARAMETER(LongName);
    UNREFERENCED_PARAMETER(KeyByShortName);
    UNREFERENCED_PARAMETER(DataLength);
    UNREFERENCED_PARAMETER(Data);
}

BOOLEAN
NTAPI
FsRtlFindInTunnelCache(IN PTUNNEL Cache, IN ULONGLONG DirectoryKey, IN PUNICODE_STRING Name,
                       OUT PUNICODE_STRING ShortName, OUT PUNICODE_STRING LongName, IN OUT PULONG DataLength,
                       OUT PVOID Data)
{
    UNREFERENCED_PARAMETER(Cache);
    UNREFERENCED_PARAMETER(DirectoryKey);
    UNREFERENCED_PARAMETER(Name);
    UNREFERENCED_PARAMETER(ShortName);
    UNREFERENCED_PARAMETER(LongName);
    UNREFERENCED_PARAMETER(DataLength);
    UNREFERENCED_PARAMETER(Data);
    return FALSE;
}

VOID
NTAPI
FsRtlDeleteKeyFromTunnelCache(IN PTUNNEL Cache, IN ULONGLONG DirectoryKey)
{
    UNREFERENCED_PARAMETER(Cache);
    UNREFERENCED_PARAMETER(DirectoryKey);
}

/* ---------------------------------------------------------------------- */
/* Directory change notification                                           */
/* ---------------------------------------------------------------------- */

typedef struct _QZ_NOTIFY
{
    LIST_ENTRY Link;                /* on the file system's NotifyList */
    PVOID FsContext;                /* the handle's context: one entry per open directory */
    PSTRING Directory;              /* its full name (the FS keeps it alive) */
    BOOLEAN Unicode;
    BOOLEAN WatchTree;
    BOOLEAN Changed;                /* a change nobody was waiting for */
    ULONG Filter;
    LIST_ENTRY Irps;
} QZ_NOTIFY, *PQZ_NOTIFY;

static KSPIN_LOCK QzNotifyLock;
static BOOLEAN QzNotifyLockReady;

static VOID
QzNotifyAcquire(PKIRQL OldIrql)
{
    if (!QzNotifyLockReady)
    {
        KeInitializeSpinLock(&QzNotifyLock);
        QzNotifyLockReady = TRUE;
    }
    KeAcquireSpinLock(&QzNotifyLock, OldIrql);
}

VOID
NTAPI
FsRtlNotifyInitializeSync(IN PNOTIFY_SYNC *NotifySync)
{
    PFAST_MUTEX Mutex = ExAllocatePoolWithTag(NonPagedPool, sizeof(FAST_MUTEX), QZ_FSRTL_TAG);
    if (!Mutex)
        ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    ExInitializeFastMutex(Mutex);
    *NotifySync = (PNOTIFY_SYNC)Mutex;
}

VOID
NTAPI
FsRtlNotifyUninitializeSync(IN PNOTIFY_SYNC *NotifySync)
{
    if (*NotifySync)
        ExFreePoolWithTag(*NotifySync, QZ_FSRTL_TAG);
    *NotifySync = NULL;
}

static VOID
NTAPI
QzNotifyCancel(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    KIRQL OldIrql;
    UNREFERENCED_PARAMETER(DeviceObject);
    IoReleaseCancelSpinLock(Irp->CancelIrql);
    QzNotifyAcquire(&OldIrql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&QzNotifyLock, OldIrql);
    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static PQZ_NOTIFY
QzNotifyFind(PLIST_ENTRY NotifyList, PVOID FsContext)
{
    PLIST_ENTRY Entry;
    for (Entry = NotifyList->Flink; Entry != NotifyList; Entry = Entry->Flink)
    {
        PQZ_NOTIFY Notify = CONTAINING_RECORD(Entry, QZ_NOTIFY, Link);
        if (Notify->FsContext == FsContext)
            return Notify;
    }
    return NULL;
}

static VOID
QzNotifyComplete(PIRP Irp, NTSTATUS Status)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_DISK_INCREMENT);
}

VOID
NTAPI
FsRtlNotifyFilterChangeDirectory(IN PNOTIFY_SYNC NotifySync, IN PLIST_ENTRY NotifyList, IN PVOID FsContext,
                                 IN PSTRING FullDirectoryName, IN BOOLEAN WatchTree, IN BOOLEAN IgnoreBuffer,
                                 IN ULONG CompletionFilter, IN PIRP NotifyIrp,
                                 IN PCHECK_FOR_TRAVERSE_ACCESS TraverseCallback OPTIONAL,
                                 IN PSECURITY_SUBJECT_CONTEXT SubjectContext OPTIONAL,
                                 IN PFILTER_REPORT_CHANGE FilterCallback OPTIONAL)
{
    PQZ_NOTIFY Notify;
    KIRQL OldIrql;
    BOOLEAN CompleteNow = FALSE;

    UNREFERENCED_PARAMETER(IgnoreBuffer);
    UNREFERENCED_PARAMETER(TraverseCallback);
    UNREFERENCED_PARAMETER(SubjectContext);
    UNREFERENCED_PARAMETER(FilterCallback);
    if (!NotifyIrp)
        return;

    ExAcquireFastMutex((PFAST_MUTEX)NotifySync);
    QzNotifyAcquire(&OldIrql);
    Notify = QzNotifyFind(NotifyList, FsContext);
    KeReleaseSpinLock(&QzNotifyLock, OldIrql);
    if (!Notify)
    {
        Notify = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Notify), QZ_FSRTL_TAG);
        if (!Notify)
        {
            ExReleaseFastMutex((PFAST_MUTEX)NotifySync);
            QzNotifyComplete(NotifyIrp, STATUS_INSUFFICIENT_RESOURCES);
            return;
        }
        RtlZeroMemory(Notify, sizeof(*Notify));
        Notify->FsContext = FsContext;
        Notify->Directory = FullDirectoryName;
        Notify->Unicode = FullDirectoryName && FullDirectoryName->Length > 1 &&
                          ((PWCHAR)FullDirectoryName->Buffer)[0] == L'\\';
        Notify->WatchTree = WatchTree;
        Notify->Filter = CompletionFilter;
        InitializeListHead(&Notify->Irps);
        QzNotifyAcquire(&OldIrql);
        InsertTailList(NotifyList, &Notify->Link);
        KeReleaseSpinLock(&QzNotifyLock, OldIrql);
    }

    QzNotifyAcquire(&OldIrql);
    if (Notify->Changed)
    {
        Notify->Changed = FALSE;
        CompleteNow = TRUE;
    }
    else
    {
        IoMarkIrpPending(NotifyIrp);
        InsertTailList(&Notify->Irps, &NotifyIrp->Tail.Overlay.ListEntry);
        IoSetCancelRoutine(NotifyIrp, QzNotifyCancel);
        if (NotifyIrp->Cancel && IoSetCancelRoutine(NotifyIrp, NULL))
        {
            RemoveEntryList(&NotifyIrp->Tail.Overlay.ListEntry);
            KeReleaseSpinLock(&QzNotifyLock, OldIrql);
            ExReleaseFastMutex((PFAST_MUTEX)NotifySync);
            QzNotifyComplete(NotifyIrp, STATUS_CANCELLED);
            return;
        }
    }
    KeReleaseSpinLock(&QzNotifyLock, OldIrql);
    ExReleaseFastMutex((PFAST_MUTEX)NotifySync);
    if (CompleteNow)
        QzNotifyComplete(NotifyIrp, STATUS_NOTIFY_ENUM_DIR);
}

VOID
NTAPI
FsRtlNotifyFullChangeDirectory(IN PNOTIFY_SYNC NotifySync, IN PLIST_ENTRY NotifyList, IN PVOID FsContext,
                               IN PSTRING FullDirectoryName, IN BOOLEAN WatchTree, IN BOOLEAN IgnoreBuffer,
                               IN ULONG CompletionFilter, IN PIRP NotifyIrp,
                               IN PCHECK_FOR_TRAVERSE_ACCESS TraverseCallback OPTIONAL,
                               IN PSECURITY_SUBJECT_CONTEXT SubjectContext OPTIONAL)
{
    FsRtlNotifyFilterChangeDirectory(NotifySync, NotifyList, FsContext, FullDirectoryName, WatchTree, IgnoreBuffer,
                                     CompletionFilter, NotifyIrp, TraverseCallback, SubjectContext, NULL);
}

/* Does a change at Target (whose parent is the first ParentLength bytes) concern this watch? */
static BOOLEAN
QzNotifyConcerns(PQZ_NOTIFY Notify, PSTRING Target, USHORT ParentLength)
{
    USHORT Length;
    if (!Notify->Directory)
        return FALSE;
    Length = Notify->Directory->Length;
    if (Length == ParentLength)
        return RtlCompareMemory(Notify->Directory->Buffer, Target->Buffer, Length) == Length;
    if (Notify->WatchTree && Length < ParentLength)
    {
        if (RtlCompareMemory(Notify->Directory->Buffer, Target->Buffer, Length) != Length)
            return FALSE;
        /* the root ("\") watches everything; others end at a separator */
        if (Notify->Unicode)
            return Length == sizeof(WCHAR) || ((PWCHAR)Target->Buffer)[Length / sizeof(WCHAR)] == L'\\';
        return Length == 1 || Target->Buffer[Length] == '\\';
    }
    return FALSE;
}

VOID
NTAPI
FsRtlNotifyFilterReportChange(IN PNOTIFY_SYNC NotifySync, IN PLIST_ENTRY NotifyList, IN PSTRING FullTargetName,
                              IN USHORT TargetNameOffset, IN PSTRING StreamName OPTIONAL,
                              IN PSTRING NormalizedParentName OPTIONAL, IN ULONG FilterMatch, IN ULONG Action,
                              IN PVOID TargetContext, IN PVOID FilterContext)
{
    LIST_ENTRY Complete;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;
    USHORT ParentLength;

    UNREFERENCED_PARAMETER(StreamName);
    UNREFERENCED_PARAMETER(NormalizedParentName);
    UNREFERENCED_PARAMETER(Action);
    UNREFERENCED_PARAMETER(TargetContext);
    UNREFERENCED_PARAMETER(FilterContext);
    if (!FullTargetName)
        return;
    /* The parent: the name up to the separator before the last component */
    ParentLength = TargetNameOffset;
    if (ParentLength > 2)
    {
        BOOLEAN Wide = FullTargetName->Length > 1 && ((PWCHAR)FullTargetName->Buffer)[0] == L'\\';
        ParentLength -= Wide ? sizeof(WCHAR) : 1;
    }

    InitializeListHead(&Complete);
    ExAcquireFastMutex((PFAST_MUTEX)NotifySync);
    QzNotifyAcquire(&OldIrql);
    for (Entry = NotifyList->Flink; Entry != NotifyList; Entry = Entry->Flink)
    {
        PQZ_NOTIFY Notify = CONTAINING_RECORD(Entry, QZ_NOTIFY, Link);
        if (!(Notify->Filter & FilterMatch) || !QzNotifyConcerns(Notify, FullTargetName, ParentLength))
            continue;
        if (IsListEmpty(&Notify->Irps))
        {
            Notify->Changed = TRUE;
            continue;
        }
        while (!IsListEmpty(&Notify->Irps))
        {
            PLIST_ENTRY IrpEntry = RemoveHeadList(&Notify->Irps);
            PIRP Irp = CONTAINING_RECORD(IrpEntry, IRP, Tail.Overlay.ListEntry);
            if (!IoSetCancelRoutine(Irp, NULL))
            {
                InitializeListHead(IrpEntry);   /* being cancelled: its routine unlinks it */
                continue;
            }
            InsertTailList(&Complete, IrpEntry);
        }
    }
    KeReleaseSpinLock(&QzNotifyLock, OldIrql);
    ExReleaseFastMutex((PFAST_MUTEX)NotifySync);
    while (!IsListEmpty(&Complete))
        QzNotifyComplete(CONTAINING_RECORD(RemoveHeadList(&Complete), IRP, Tail.Overlay.ListEntry),
                         STATUS_NOTIFY_ENUM_DIR);
}

VOID
NTAPI
FsRtlNotifyFullReportChange(IN PNOTIFY_SYNC NotifySync, IN PLIST_ENTRY NotifyList, IN PSTRING FullTargetName,
                            IN USHORT TargetNameOffset, IN PSTRING StreamName OPTIONAL,
                            IN PSTRING NormalizedParentName OPTIONAL, IN ULONG FilterMatch, IN ULONG Action,
                            IN PVOID TargetContext)
{
    FsRtlNotifyFilterReportChange(NotifySync, NotifyList, FullTargetName, TargetNameOffset, StreamName,
                                  NormalizedParentName, FilterMatch, Action, TargetContext, NULL);
}

/* The handle closes: its waiting requests end with STATUS_NOTIFY_CLEANUP */
VOID
NTAPI
FsRtlNotifyCleanup(IN PNOTIFY_SYNC NotifySync, IN PLIST_ENTRY NotifyList, IN PVOID FsContext)
{
    LIST_ENTRY Complete;
    PQZ_NOTIFY Notify;
    KIRQL OldIrql;

    InitializeListHead(&Complete);
    ExAcquireFastMutex((PFAST_MUTEX)NotifySync);
    QzNotifyAcquire(&OldIrql);
    Notify = QzNotifyFind(NotifyList, FsContext);
    if (Notify)
    {
        RemoveEntryList(&Notify->Link);
        while (!IsListEmpty(&Notify->Irps))
        {
            PLIST_ENTRY IrpEntry = RemoveHeadList(&Notify->Irps);
            PIRP Irp = CONTAINING_RECORD(IrpEntry, IRP, Tail.Overlay.ListEntry);
            if (!IoSetCancelRoutine(Irp, NULL))
            {
                InitializeListHead(IrpEntry);
                continue;
            }
            InsertTailList(&Complete, IrpEntry);
        }
    }
    KeReleaseSpinLock(&QzNotifyLock, OldIrql);
    ExReleaseFastMutex((PFAST_MUTEX)NotifySync);
    while (!IsListEmpty(&Complete))
        QzNotifyComplete(CONTAINING_RECORD(RemoveHeadList(&Complete), IRP, Tail.Overlay.ListEntry),
                         STATUS_NOTIFY_CLEANUP);
    if (Notify)
        ExFreePoolWithTag(Notify, QZ_FSRTL_TAG);
}

NTSTATUS
NTAPI
FsRtlNotifyVolumeEvent(IN PFILE_OBJECT FileObject, IN ULONG EventCode)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(EventCode);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Filters and per-stream contexts: no file system filters yet             */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
FsRtlRegisterFileSystemFilterCallbacks(IN PDRIVER_OBJECT FilterDriverObject,
                                       IN PFS_FILTER_CALLBACKS Callbacks)
{
    UNREFERENCED_PARAMETER(FilterDriverObject);
    UNREFERENCED_PARAMETER(Callbacks);
    return STATUS_SUCCESS;
}

VOID
NTAPI
FsRtlTeardownPerStreamContexts(IN PFSRTL_ADVANCED_FCB_HEADER AdvancedHeader)
{
    UNREFERENCED_PARAMETER(AdvancedHeader);
}

/* ---------------------------------------------------------------------- */
/* Stack overflow posting: the work runs on a worker thread's fresh stack  */
/* ---------------------------------------------------------------------- */

typedef struct _QZ_STACK_OVERFLOW
{
    WORK_QUEUE_ITEM Item;
    PFSRTL_STACK_OVERFLOW_ROUTINE Routine;
    PVOID Context;
    PKEVENT Event;
} QZ_STACK_OVERFLOW, *PQZ_STACK_OVERFLOW;

static VOID
NTAPI
QzStackOverflowWorker(PVOID Parameter)
{
    PQZ_STACK_OVERFLOW Work = Parameter;
    Work->Routine(Work->Context, Work->Event);
    ExFreePoolWithTag(Work, QZ_FSRTL_TAG);
}

VOID
NTAPI
FsRtlPostStackOverflow(IN PVOID Context, IN PKEVENT Event, IN PFSRTL_STACK_OVERFLOW_ROUTINE StackOverflowRoutine)
{
    PQZ_STACK_OVERFLOW Work = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Work), QZ_FSRTL_TAG);
    if (!Work)
    {
        StackOverflowRoutine(Context, Event);
        return;
    }
    Work->Routine = StackOverflowRoutine;
    Work->Context = Context;
    Work->Event = Event;
    ExInitializeWorkItem(&Work->Item, QzStackOverflowWorker, Work);
    ExQueueWorkItem(&Work->Item, CriticalWorkQueue);
}

VOID
NTAPI
FsRtlPostPagingFileStackOverflow(IN PVOID Context, IN PKEVENT Event,
                                 IN PFSRTL_STACK_OVERFLOW_ROUTINE StackOverflowRoutine)
{
    FsRtlPostStackOverflow(Context, Event, StackOverflowRoutine);
}

/* ---------------------------------------------------------------------- */
/* Fast I/O through the cache                                              */
/* ---------------------------------------------------------------------- */

/* Cached reads without an IRP; FALSE sends the caller down the IRP path */
BOOLEAN
NTAPI
FsRtlCopyRead(IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER FileOffset, IN ULONG Length, IN BOOLEAN Wait,
              IN ULONG LockKey, OUT PVOID Buffer, OUT PIO_STATUS_BLOCK IoStatus, IN PDEVICE_OBJECT DeviceObject)
{
    PFSRTL_COMMON_FCB_HEADER Header = FileObject->FsContext;
    PFAST_IO_DISPATCH FastIo;
    LARGE_INTEGER End;
    BOOLEAN Result = TRUE;

    if (!Length)
    {
        IoStatus->Status = STATUS_SUCCESS;
        IoStatus->Information = 0;
        return TRUE;
    }
    if (!Header || !FileObject->PrivateCacheMap || Length > MAXLONG ||
        FileOffset->QuadPart + Length < FileOffset->QuadPart)
        return FALSE;

    FsRtlEnterFileSystem();
    if (!ExAcquireResourceSharedLite(Header->Resource, Wait))
    {
        FsRtlExitFileSystem();
        return FALSE;
    }
    if (Header->IsFastIoPossible == FastIoIsNotPossible)
    {
        Result = FALSE;
        goto Done;
    }
    if (Header->IsFastIoPossible == FastIoIsQuestionable)
    {
        FastIo = IoGetRelatedDeviceObject(FileObject)->DriverObject->FastIoDispatch;
        if (!FastIo || !FastIo->FastIoCheckIfPossible ||
            !FastIo->FastIoCheckIfPossible(FileObject, FileOffset, Length, Wait, LockKey, TRUE, IoStatus,
                                           DeviceObject))
        {
            Result = FALSE;
            goto Done;
        }
    }

    /* Past the end: nothing; across it: up to it */
    if (FileOffset->QuadPart >= Header->FileSize.QuadPart)
    {
        IoStatus->Status = STATUS_END_OF_FILE;
        IoStatus->Information = 0;
        goto Done;
    }
    End.QuadPart = FileOffset->QuadPart + Length;
    if (End.QuadPart > Header->FileSize.QuadPart)
        Length = (ULONG)(Header->FileSize.QuadPart - FileOffset->QuadPart);

    Result = CcCopyRead(FileObject, FileOffset, Length, Wait, Buffer, IoStatus);
    if (Result)
    {
        FileObject->Flags |= FO_FILE_FAST_IO_READ;
        if (FileObject->Flags & FO_SYNCHRONOUS_IO)
            FileObject->CurrentByteOffset.QuadPart = FileOffset->QuadPart + IoStatus->Information;
    }

Done:
    ExReleaseResourceLite(Header->Resource);
    FsRtlExitFileSystem();
    return Result;
}

/* Cached writes inside the file as it is; growing it takes the IRP path */
BOOLEAN
NTAPI
FsRtlCopyWrite(IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER FileOffset, IN ULONG Length, IN BOOLEAN Wait,
               IN ULONG LockKey, IN PVOID Buffer, OUT PIO_STATUS_BLOCK IoStatus, IN PDEVICE_OBJECT DeviceObject)
{
    PFSRTL_COMMON_FCB_HEADER Header = FileObject->FsContext;
    PFAST_IO_DISPATCH FastIo;
    LARGE_INTEGER Offset = *FileOffset;
    BOOLEAN Result = TRUE;

    if (!Header || !FileObject->PrivateCacheMap || (FileObject->Flags & FO_WRITE_THROUGH) || Length > MAXLONG)
        return FALSE;
    if (!Length)
    {
        IoStatus->Status = STATUS_SUCCESS;
        IoStatus->Information = 0;
        return TRUE;
    }
    if (!CcCanIWrite(FileObject, Length, Wait, FALSE))
        return FALSE;

    FsRtlEnterFileSystem();
    if (!ExAcquireResourceSharedLite(Header->Resource, Wait))
    {
        FsRtlExitFileSystem();
        return FALSE;
    }
    if (Offset.LowPart == FILE_WRITE_TO_END_OF_FILE && Offset.HighPart == -1)
        Offset = Header->FileSize;
    if (Header->IsFastIoPossible == FastIoIsNotPossible ||
        Offset.QuadPart + Length > Header->ValidDataLength.QuadPart ||
        Offset.QuadPart + Length > Header->FileSize.QuadPart)
    {
        Result = FALSE;
        goto Done;
    }
    if (Header->IsFastIoPossible == FastIoIsQuestionable)
    {
        FastIo = IoGetRelatedDeviceObject(FileObject)->DriverObject->FastIoDispatch;
        if (!FastIo || !FastIo->FastIoCheckIfPossible ||
            !FastIo->FastIoCheckIfPossible(FileObject, &Offset, Length, Wait, LockKey, FALSE, IoStatus,
                                           DeviceObject))
        {
            Result = FALSE;
            goto Done;
        }
    }
    Result = CcCopyWrite(FileObject, &Offset, Length, Wait, Buffer);
    if (Result)
    {
        IoStatus->Status = STATUS_SUCCESS;
        IoStatus->Information = Length;
        FileObject->Flags |= FO_FILE_MODIFIED;
        if (FileObject->Flags & FO_SYNCHRONOUS_IO)
            FileObject->CurrentByteOffset.QuadPart = Offset.QuadPart + Length;
    }

Done:
    ExReleaseResourceLite(Header->Resource);
    FsRtlExitFileSystem();
    return Result;
}

/* MDL fast I/O: not offered, the IRP path (IRP_MN_MDL) does it */
BOOLEAN
NTAPI
FsRtlMdlReadDev(IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER FileOffset, IN ULONG Length, IN ULONG LockKey,
                OUT PMDL *MdlChain, OUT PIO_STATUS_BLOCK IoStatus, IN PDEVICE_OBJECT DeviceObject)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(LockKey);
    UNREFERENCED_PARAMETER(MdlChain);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);
    return FALSE;
}

BOOLEAN
NTAPI
FsRtlPrepareMdlWriteDev(IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER FileOffset, IN ULONG Length, IN ULONG LockKey,
                        OUT PMDL *MdlChain, OUT PIO_STATUS_BLOCK IoStatus, IN PDEVICE_OBJECT DeviceObject)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(LockKey);
    UNREFERENCED_PARAMETER(MdlChain);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);
    return FALSE;
}

BOOLEAN
NTAPI
FsRtlMdlReadCompleteDev(IN PFILE_OBJECT FileObject, IN PMDL MemoryDescriptorList, IN PDEVICE_OBJECT DeviceObject)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    CcMdlReadComplete(FileObject, MemoryDescriptorList);
    return TRUE;
}

BOOLEAN
NTAPI
FsRtlMdlWriteCompleteDev(IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER FileOffset, IN PMDL MdlChain,
                         IN PDEVICE_OBJECT DeviceObject)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    CcMdlWriteComplete(FileObject, FileOffset, MdlChain);
    return TRUE;
}

BOOLEAN
NTAPI
FsRtlMdlReadComplete(IN PFILE_OBJECT FileObject, IN PMDL MdlChain)
{
    return FsRtlMdlReadCompleteDev(FileObject, MdlChain, NULL);
}

BOOLEAN
NTAPI
FsRtlMdlWriteComplete(IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER FileOffset, IN PMDL MdlChain)
{
    return FsRtlMdlWriteCompleteDev(FileObject, FileOffset, MdlChain, NULL);
}

/* ---------------------------------------------------------------------- */
/* Asking a file system about a file                                       */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
FsRtlGetFileSize(IN PFILE_OBJECT FileObject, IN OUT PLARGE_INTEGER FileSize)
{
    FILE_STANDARD_INFORMATION Info;
    ULONG Length;
    NTSTATUS Status = IoQueryFileInformation(FileObject, FileStandardInformation, sizeof(Info), &Info, &Length);
    if (NT_SUCCESS(Status))
    {
        if (Info.Directory)
            return STATUS_FILE_IS_A_DIRECTORY;
        *FileSize = Info.EndOfFile;
    }
    return Status;
}
