/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Processes: objects, IDs, attaching to address spaces
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * A process is a "Process" object whose body is QZ_PROCESS (EPROCESS is
 * opaque to drivers). Its body starts with a notification event that is
 * signaled when the process ends, so waits on processes work as on NT.
 * Process and thread IDs come from one client ID table, multiples of 4;
 * the System process is 4.
 */

#include "qzio.h"
#include "qzps.h"

#define QZ_CID_TAG  'diCQ'

PQZ_PROCESS QzSystemProcess;
static LIST_ENTRY QzProcessList = { &QzProcessList, &QzProcessList };
static FAST_MUTEX QzProcessListLock;

/* ---------------------------------------------------------------------- */
/* Client IDs                                                              */
/* ---------------------------------------------------------------------- */

static PVOID *QzCidTable;
static ULONG QzCidCapacity, QzCidHint = 1;
static KSPIN_LOCK QzCidLock;

ULONG
QzPsAllocateId(PVOID Object)
{
    KIRQL OldIrql = QzAcquireSpinLock(&QzCidLock);
    ULONG Index;

    for (;;)
    {
        for (Index = QzCidHint; Index < QzCidCapacity; Index++)
        {
            if (!QzCidTable[Index])
            {
                QzCidTable[Index] = Object;
                QzCidHint = Index + 1;
                QzReleaseSpinLock(&QzCidLock, OldIrql);
                return Index * 4;
            }
        }
        if (QzCidHint > 1)
        {
            QzCidHint = 1;
            continue;
        }
        /* Grow */
        {
            ULONG NewCapacity = QzCidCapacity ? QzCidCapacity * 2 : 256;
            PVOID *NewTable = ExAllocatePoolWithTag(NonPagedPool, NewCapacity * sizeof(PVOID), QZ_CID_TAG);
            if (!NewTable)
            {
                QzReleaseSpinLock(&QzCidLock, OldIrql);
                return 0;
            }
            RtlZeroMemory(NewTable, NewCapacity * sizeof(PVOID));
            if (QzCidTable)
            {
                RtlCopyMemory(NewTable, QzCidTable, QzCidCapacity * sizeof(PVOID));
                ExFreePoolWithTag(QzCidTable, QZ_CID_TAG);
            }
            QzCidHint = QzCidCapacity ? QzCidCapacity : 1;
            QzCidTable = NewTable;
            QzCidCapacity = NewCapacity;
        }
    }
}

VOID
QzPsFreeId(ULONG Id)
{
    KIRQL OldIrql = QzAcquireSpinLock(&QzCidLock);
    if (Id / 4 < QzCidCapacity)
        QzCidTable[Id / 4] = NULL;
    QzReleaseSpinLock(&QzCidLock, OldIrql);
}

/* The object for an ID, referenced */
PVOID
QzPsLookupId(ULONG Id)
{
    KIRQL OldIrql = QzAcquireSpinLock(&QzCidLock);
    PVOID Object = NULL;

    if (!(Id & 3) && Id / 4 < QzCidCapacity && QzCidTable[Id / 4])
    {
        Object = QzCidTable[Id / 4];
        ObfReferenceObject(Object);
    }
    QzReleaseSpinLock(&QzCidLock, OldIrql);
    return Object;
}

/* ---------------------------------------------------------------------- */
/* Attaching: running in another process's address space                  */
/* ---------------------------------------------------------------------- */

VOID
QzPsAttach(PQZ_PROCESS Process, PQZ_PROCESS *Saved)
{
    PQZ_THREAD Thread = QzCurrentThread;
    ULONG Flags = QzSaveFlagsAndDisable();

    *Saved = Thread->ApcProcess;
    Thread->ApcProcess = Process;
    QzMmSwitchAddressSpace(Process);
    QzRestoreFlags(Flags);
}

VOID
QzPsDetach(PQZ_PROCESS Saved)
{
    PQZ_THREAD Thread = QzCurrentThread;
    ULONG Flags = QzSaveFlagsAndDisable();

    Thread->ApcProcess = Saved;
    QzMmSwitchAddressSpace(Saved ? Saved : QzSystemProcess);
    QzRestoreFlags(Flags);
    QzApcCheckPending();                /* kernel APCs held while attached */
}

VOID
NTAPI
KeStackAttachProcess(IN PKPROCESS Process,
                     OUT PRKAPC_STATE ApcState)
{
    PQZ_PROCESS Saved;
    QzPsAttach((PQZ_PROCESS)Process, &Saved);
    ApcState->Process = (PKPROCESS)Saved;
}

VOID
NTAPI
KeUnstackDetachProcess(IN PRKAPC_STATE ApcState)
{
    QzPsDetach((PQZ_PROCESS)ApcState->Process);
}

VOID
NTAPI
KeAttachProcess(IN PKPROCESS Process)
{
    PQZ_PROCESS Saved;
    QzPsAttach((PQZ_PROCESS)Process, &Saved);
    QzCurrentThread->SavedApcProcess = Saved;
}

VOID
NTAPI
KeDetachProcess(VOID)
{
    PQZ_PROCESS Saved = QzCurrentThread->SavedApcProcess;
    QzCurrentThread->SavedApcProcess = NULL;
    QzPsDetach(Saved ? Saved : QzCurrentThread->Process);
}

/* ---------------------------------------------------------------------- */
/* Process objects                                                         */
/* ---------------------------------------------------------------------- */

/* The Process type's delete procedure: the last reference is gone */
VOID NTAPI
QzPsDeleteProcess(PVOID Object)
{
    PQZ_PROCESS Process = Object;

    if (Process == QzSystemProcess)
        KeBugCheckEx(PROCESS_HAS_LOCKED_PAGES, 0, (ULONG_PTR)Process, 0, 0);
    if (Process->ProcessLink.Flink)
    {
        ExAcquireFastMutex(&QzProcessListLock);
        RemoveEntryList(&Process->ProcessLink);
        ExReleaseFastMutex(&QzProcessListLock);
    }
    QzObKillHandleTable(Process->HandleTable);
    Process->HandleTable = NULL;
    QzMmDeleteAddressSpace(Process);
    if (Process->UniqueId)
        QzPsFreeId(Process->UniqueId);
    if (Process->ImagePathName.Buffer)
        ExFreePool(Process->ImagePathName.Buffer);
}

static VOID
QzPsInitBody(PQZ_PROCESS Process, PCSTR Name, PQZ_PROCESS Parent)
{
    RtlZeroMemory(Process, sizeof(*Process));
    KeInitializeEvent(&Process->Exited, NotificationEvent, FALSE);
    InitializeListHead(&Process->VadList);
    InitializeListHead(&Process->ThreadList);
    ExInitializeFastMutex(&Process->AddressLock);
    strncpy(Process->ImageFileName, Name, sizeof(Process->ImageFileName) - 1);
    Process->InheritedFromUniqueId = Parent ? Parent->UniqueId : 0;
    Process->Affinity = 1;
    Process->BasePriority = 8;
    Process->ExitStatus = STATUS_PENDING;
    KeQuerySystemTime(&Process->CreateTime);
}

NTSTATUS
QzPsCreateProcess(PCSTR Name, PQZ_PROCESS Parent, PQZ_PROCESS *Result)
{
    PQZ_PROCESS Process;
    NTSTATUS Status;

    *Result = NULL;
    Status = ObCreateObject(KernelMode, PsProcessType, NULL, KernelMode, NULL, sizeof(QZ_PROCESS),
                            0, 0, (PVOID *)&Process);
    if (!NT_SUCCESS(Status))
        return Status;
    QzPsInitBody(Process, Name, Parent);

    Process->HandleTable = QzObCreateHandleTable();
    Process->UniqueId = QzPsAllocateId(Process);
    if (!Process->HandleTable || !Process->UniqueId)
    {
        ObDereferenceObject(Process);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = QzMmCreateAddressSpace(Process);
    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(Process);
        return Status;
    }

    ExAcquireFastMutex(&QzProcessListLock);
    InsertTailList(&QzProcessList, &Process->ProcessLink);
    ExReleaseFastMutex(&QzProcessListLock);
    *Result = Process;
    return STATUS_SUCCESS;
}

PQZ_PROCESS
QzPsLookupProcess(ULONG UniqueId)
{
    PVOID Object = QzPsLookupId(UniqueId);
    if (Object && OBJECT_TO_OBJECT_HEADER(Object)->Type != PsProcessType)
    {
        ObDereferenceObject(Object);
        return NULL;
    }
    return Object;
}

/* Called by QzPsInit once the Process type exists */
VOID
QzPsInitProcesses(VOID)
{
    PQZ_THREAD Thread, First;
    PVOID Body;

    ExInitializeFastMutex(&QzProcessListLock);
    if (!NT_SUCCESS(ObCreateObject(KernelMode, PsProcessType, NULL, KernelMode, NULL,
                                   sizeof(QZ_PROCESS), 0, 0, &Body)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, 2, 0, 0, 0);
    QzSystemProcess = Body;
    QzPsInitBody(QzSystemProcess, "System", NULL);
    QzSystemProcess->DirectoryTableBase = QzReadCr3();
    QzSystemProcess->HandleTable = QzObKernelHandleTable();

    /* ID 0 is the idle process; System is 4 */
    QzPsAllocateId(QzSystemProcess);     /* index 1 -> ID 4 */
    QzSystemProcess->UniqueId = 4;
    InsertTailList(&QzProcessList, &QzSystemProcess->ProcessLink);
    ObInsertObject(QzSystemProcess, NULL, 0, 0, NULL, NULL);
    PsInitialSystemProcess = (PEPROCESS)QzSystemProcess;

    /* The threads that exist already all belong to System */
    First = Thread = QzFirstThread();
    do
    {
        if (!Thread->Process)
            Thread->Process = Thread->ApcProcess = QzSystemProcess;
        Thread = Thread->Next;
    } while (Thread != First);
}

VOID
QzPsDumpProcesses(VOID)
{
    PLIST_ENTRY Entry;

    QzPrint("   PID  PPID  NAME             THREADS  HANDLES  PAGES  CR3\n");
    ExAcquireFastMutex(&QzProcessListLock);
    for (Entry = QzProcessList.Flink; Entry != &QzProcessList; Entry = Entry->Flink)
    {
        PQZ_PROCESS Process = CONTAINING_RECORD(Entry, QZ_PROCESS, ProcessLink);
        QzPrint("  %4lu  %4lu  %-16s %7lu  %7lu  %5lu  %08lx%s\n", Process->UniqueId,
                Process->InheritedFromUniqueId, Process->ImageFileName, Process->ActiveThreads,
                Process->HandleTable ? QzObTableHandleCount(Process->HandleTable) : 0,
                Process->PrivatePages, Process->DirectoryTableBase,
                Process->Exiting ? "  (exiting)" : "");
    }
    ExReleaseFastMutex(&QzProcessListLock);
}

/* ---------------------------------------------------------------------- */
/* Exports: process and thread queries                                     */
/* ---------------------------------------------------------------------- */

PEPROCESS
NTAPI
IoGetCurrentProcess(VOID)
{
    return (PEPROCESS)(QzCurrentThread->ApcProcess ? QzCurrentThread->ApcProcess : QzSystemProcess);
}

HANDLE
NTAPI
PsGetCurrentProcessId(VOID)
{
    PQZ_PROCESS Process = QzCurrentThread->Process;
    return (HANDLE)(ULONG_PTR)(Process ? Process->UniqueId : 4);
}

HANDLE
NTAPI
PsGetCurrentThreadId(VOID)
{
    PQZ_PS_THREAD PsThread = QzCurrentThread->PsThread;
    if (PsThread && PsThread->UniqueId)
        return (HANDLE)(ULONG_PTR)PsThread->UniqueId;
    return (HANDLE)(ULONG_PTR)(0x10000 + QzCurrentThread->Id * 4);   /* a kernel-internal thread */
}

HANDLE
NTAPI
PsGetProcessId(IN PEPROCESS Process)
{
    return (HANDLE)(ULONG_PTR)((PQZ_PROCESS)Process)->UniqueId;
}

HANDLE
NTAPI
PsGetProcessInheritedFromUniqueProcessId(IN PEPROCESS Process)
{
    return (HANDLE)(ULONG_PTR)((PQZ_PROCESS)Process)->InheritedFromUniqueId;
}

LPSTR
NTAPI
PsGetProcessImageFileName(IN PEPROCESS Process)
{
    return ((PQZ_PROCESS)Process)->ImageFileName;
}

PPEB
NTAPI
PsGetProcessPeb(IN PEPROCESS Process)
{
    return ((PQZ_PROCESS)Process)->Peb;
}

NTSTATUS
NTAPI
PsGetProcessExitStatus(IN PEPROCESS Process)
{
    return ((PQZ_PROCESS)Process)->ExitStatus;
}

LONGLONG
NTAPI
PsGetProcessCreateTimeQuadPart(IN PEPROCESS Process)
{
    return ((PQZ_PROCESS)Process)->CreateTime.QuadPart;
}

ULONG
NTAPI
PsGetProcessSessionId(IN PEPROCESS Process)
{
    UNREFERENCED_PARAMETER(Process);
    return 0;
}

ULONG
NTAPI
PsGetCurrentProcessSessionId(VOID)
{
    return 0;
}

PVOID
NTAPI
PsGetProcessDebugPort(IN PEPROCESS Process)
{
    return ((PQZ_PROCESS)Process)->DebugPort;
}

BOOLEAN
NTAPI
PsIsProcessBeingDebugged(IN PEPROCESS Process)
{
    return ((PQZ_PROCESS)Process)->DebugPort != NULL;
}

PVOID
NTAPI
PsGetProcessWin32Process(IN PEPROCESS Process)
{
    return ((PQZ_PROCESS)Process)->Win32Process;
}

PVOID
NTAPI
PsGetCurrentProcessWin32Process(VOID)
{
    PQZ_PROCESS Process = QzCurrentThread->Process;
    return Process ? Process->Win32Process : NULL;
}

NTSTATUS
NTAPI
PsLookupProcessByProcessId(IN HANDLE ProcessId,
                           OUT PEPROCESS *Process)
{
    PQZ_PROCESS Found = QzPsLookupProcess((ULONG)(ULONG_PTR)ProcessId);
    *Process = (PEPROCESS)Found;
    return Found ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

/* ---- threads ---- */

BOOLEAN
QzPsIsObject(PVOID Body)
{
    /* A scheduler thread is plain pool, not an object */
    return MmIsAddressValid(OBJECT_TO_OBJECT_HEADER(Body)) &&
           OBJECT_TO_OBJECT_HEADER(Body)->Type == PsThreadType;
}

static PQZ_THREAD
QzPsKernelThreadOf(PETHREAD Thread)
{
    if (QzPsIsObject(Thread))
        return ((PQZ_PS_THREAD)Thread)->Thread;
    return (PQZ_THREAD)Thread;
}

HANDLE
NTAPI
PsGetThreadId(IN PETHREAD Thread)
{
    PQZ_THREAD KThread = QzPsKernelThreadOf(Thread);
    PQZ_PS_THREAD PsThread = QzPsIsObject(Thread) ? (PQZ_PS_THREAD)Thread : KThread->PsThread;
    if (PsThread && PsThread->UniqueId)
        return (HANDLE)(ULONG_PTR)PsThread->UniqueId;
    return (HANDLE)(ULONG_PTR)(0x10000 + KThread->Id * 4);
}

PEPROCESS
NTAPI
PsGetThreadProcess(IN PETHREAD Thread)
{
    PQZ_THREAD KThread = QzPsKernelThreadOf(Thread);
    if (QzPsIsObject(Thread))
        return (PEPROCESS)((PQZ_PS_THREAD)Thread)->Process;
    return (PEPROCESS)(KThread->Process ? KThread->Process : QzSystemProcess);
}

HANDLE
NTAPI
PsGetThreadProcessId(IN PETHREAD Thread)
{
    return PsGetProcessId(PsGetThreadProcess(Thread));
}

PEPROCESS
NTAPI
PsGetCurrentThreadProcess(VOID)
{
    return (PEPROCESS)(QzCurrentThread->Process ? QzCurrentThread->Process : QzSystemProcess);
}

HANDLE
NTAPI
PsGetCurrentThreadProcessId(VOID)
{
    return PsGetCurrentProcessId();
}

BOOLEAN
NTAPI
PsIsSystemThread(IN PETHREAD Thread)
{
    return PsGetThreadProcess(Thread) == (PEPROCESS)QzSystemProcess;
}

PVOID
NTAPI
PsGetThreadTeb(IN PETHREAD Thread)
{
    PQZ_PS_THREAD PsThread = QzPsIsObject(Thread) ? (PQZ_PS_THREAD)Thread
                                                  : ((PQZ_THREAD)Thread)->PsThread;
    return PsThread ? PsThread->Teb : NULL;
}

PVOID
NTAPI
PsGetCurrentThreadTeb(VOID)
{
    PQZ_PS_THREAD PsThread = QzCurrentThread->PsThread;
    return PsThread ? PsThread->Teb : NULL;
}

PVOID
NTAPI
PsGetThreadWin32Thread(IN PETHREAD Thread)
{
    PQZ_PS_THREAD PsThread = QzPsThreadOf(Thread);
    return PsThread ? PsThread->Win32Thread : NULL;
}

PVOID
NTAPI
PsGetCurrentThreadWin32Thread(VOID)
{
    PQZ_PS_THREAD PsThread = QzCurrentThread->PsThread;
    return PsThread ? PsThread->Win32Thread : NULL;
}

NTSTATUS
NTAPI
PsLookupThreadByThreadId(IN HANDLE ThreadId,
                         OUT PETHREAD *Thread)
{
    PVOID Object = QzPsLookupId((ULONG)(ULONG_PTR)ThreadId);
    if (Object && OBJECT_TO_OBJECT_HEADER(Object)->Type != PsThreadType)
    {
        ObDereferenceObject(Object);
        Object = NULL;
    }
    *Thread = Object;
    return Object ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

/* ---------------------------------------------------------------------- */
/* Opening processes and threads                                           */
/* ---------------------------------------------------------------------- */

/*
 * By client ID (a thread ID, if given, names its process) or by name.
 * Access is not checked yet: the handle gets what was asked for.
 */
static NTSTATUS
QzPsOpen(PHANDLE Handle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PCLIENT_ID ClientId,
         BOOLEAN WantThread)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    POBJECT_TYPE Type = WantThread ? PsThreadType : PsProcessType;
    OBJECT_ATTRIBUTES Attributes;
    CLIENT_ID Cid;
    PVOID Object = NULL;
    HANDLE Result;
    NTSTATUS Status;

    if (!QzReadArg(Mode, &Attributes, ObjectAttributes, sizeof(Attributes)))
        return STATUS_ACCESS_VIOLATION;
    if (ClientId && !QzReadArg(Mode, &Cid, ClientId, sizeof(Cid)))
        return STATUS_ACCESS_VIOLATION;
    if (ClientId && Attributes.ObjectName)
        return STATUS_INVALID_PARAMETER_MIX;

    if (!ClientId)
    {
        if (!Attributes.ObjectName)
            return STATUS_INVALID_PARAMETER_MIX;
        Status = ObOpenObjectByName(ObjectAttributes, Type, Mode, NULL, DesiredAccess, NULL, &Result);
    }
    else
    {
        if (Cid.UniqueThread)
        {
            PETHREAD Thread;
            Status = PsLookupThreadByThreadId(Cid.UniqueThread, &Thread);
            if (NT_SUCCESS(Status))
            {
                if (Cid.UniqueProcess && PsGetThreadProcessId(Thread) != Cid.UniqueProcess)
                {
                    ObDereferenceObject(Thread);
                    return STATUS_INVALID_CID;
                }
                if (WantThread)
                {
                    Object = Thread;
                }
                else
                {
                    Object = PsGetThreadProcess(Thread);
                    ObReferenceObject(Object);
                    ObDereferenceObject(Thread);
                }
            }
        }
        else if (!WantThread)
        {
            PEPROCESS Process;
            Status = PsLookupProcessByProcessId(Cid.UniqueProcess, &Process);
            Object = Process;
        }
        else
        {
            return STATUS_INVALID_CID;
        }
        if (!NT_SUCCESS(Status))
            return STATUS_INVALID_CID;
        Status = ObOpenObjectByPointer(Object, Attributes.Attributes, NULL, DesiredAccess, Type, Mode, &Result);
        ObDereferenceObject(Object);
    }
    if (NT_SUCCESS(Status) && !QzWriteArg(Mode, Handle, &Result, sizeof(Result)))
    {
        ObCloseHandle(Result, Mode);
        Status = STATUS_ACCESS_VIOLATION;
    }
    return Status;
}

NTSTATUS
NTAPI
NtOpenProcess(OUT PHANDLE ProcessHandle, IN ACCESS_MASK DesiredAccess, IN POBJECT_ATTRIBUTES ObjectAttributes,
              IN PCLIENT_ID ClientId OPTIONAL)
{
    return QzPsOpen(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId, FALSE);
}

NTSTATUS
NTAPI
NtOpenThread(OUT PHANDLE ThreadHandle, IN ACCESS_MASK DesiredAccess, IN POBJECT_ATTRIBUTES ObjectAttributes,
             IN PCLIENT_ID ClientId OPTIONAL)
{
    return QzPsOpen(ThreadHandle, DesiredAccess, ObjectAttributes, ClientId, TRUE);
}

NTSTATUS
NTAPI
ZwOpenProcess(OUT PHANDLE ProcessHandle, IN ACCESS_MASK DesiredAccess, IN POBJECT_ATTRIBUTES ObjectAttributes,
              IN PCLIENT_ID ClientId OPTIONAL)
{
    return QZ_AS_KERNEL(NtOpenProcess(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId));
}

NTSTATUS
NTAPI
ZwOpenThread(OUT PHANDLE ThreadHandle, IN ACCESS_MASK DesiredAccess, IN POBJECT_ATTRIBUTES ObjectAttributes,
             IN PCLIENT_ID ClientId OPTIONAL)
{
    return QZ_AS_KERNEL(NtOpenThread(ThreadHandle, DesiredAccess, ObjectAttributes, ClientId));
}
