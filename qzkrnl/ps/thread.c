/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     System threads and the System process as objects
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 A thread is a scheduler thread plus a "Thread" object (QZ_PS_THREAD):
 * its body starts with a notification event (in the place of KTHREAD's
 * dispatcher header) that is signaled when the thread exits, so threads
 * can be waited for by handle or by object as on NT. Processes are in
 * ps/process.c.
 */

#include "qzio.h"
#include "qzps.h"

POBJECT_TYPE PsThreadType;
POBJECT_TYPE PsProcessType;
PEPROCESS PsInitialSystemProcess;

VOID NTAPI QzPsDeleteProcess(PVOID Object);   /* ps/process.c */
VOID QzLpcInit(VOID);                          /* lpc/port.c */
VOID QzSeInitTokens(VOID);                     /* se/token.c */
VOID QzExInitKeyedEvents(VOID);                /* ex/keyed.c */

static GENERIC_MAPPING QzThreadMapping =
{
    STANDARD_RIGHTS_READ | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
    STANDARD_RIGHTS_WRITE | THREAD_TERMINATE | THREAD_SUSPEND_RESUME | THREAD_SET_INFORMATION,
    STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
    THREAD_ALL_ACCESS
};

static GENERIC_MAPPING QzProcessMapping =
{
    STANDARD_RIGHTS_READ | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
    STANDARD_RIGHTS_WRITE | PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE,
    STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
    PROCESS_ALL_ACCESS
};

DECLSPEC_NORETURN VOID __cdecl QzExitCurrentThread(VOID);

/* The process's last thread is gone: it has exited */
static VOID
QzPsProcessExited(PQZ_PROCESS Process, NTSTATUS ExitStatus)
{
    PQZ_HANDLE_TABLE Table;

    if (Process->ExitStatus == STATUS_PENDING)
        Process->ExitStatus = ExitStatus;
    Process->Exiting = TRUE;
    KeQuerySystemTime(&Process->ExitTime);
    DbgPrint(QZ_KERNEL_NAME ": process %s (pid %lu) exited with 0x%08lx\n", Process->ImageFileName,
             Process->UniqueId, Process->ExitStatus);

    /* Its handles go now (they may keep the process itself alive) */
    Table = Process->HandleTable;
    Process->HandleTable = NULL;
    QzObKillHandleTable(Table);
    KeSetEvent(&Process->Exited, IO_NO_INCREMENT, FALSE);
}

/* The way out for every thread with a Thread object */
DECLSPEC_NORETURN
VOID
QzPsTerminateCurrentThread(NTSTATUS ExitStatus)
{
    PQZ_THREAD Thread = QzCurrentThread;
    PQZ_PS_THREAD PsThread = Thread->PsThread;
    PQZ_PROCESS Process = PsThread->Process;

    if (QzGetCurrentIrql() != PASSIVE_LEVEL)
        QzLowerIrql(PASSIVE_LEVEL);
    PsThread->ExitStatus = ExitStatus;
    PsThread->ExitedThread = TRUE;
    KeQuerySystemTime(&PsThread->ExitTime);

    /* win32k lets go of the thread (and, with the last one, the process)
       while the process's address space is still loaded */
    if (PsThread->GuiThread)
        QzPsWin32ThreadExit(PsThread);
    if (Process && Process != QzSystemProcess && Process->ActiveThreads == 1 && Process->Win32Process)
        QzPsWin32ProcessExit(Process);
    KeSetEvent(&PsThread->Terminated, IO_NO_INCREMENT, FALSE);

    /* Leave the process's address space before it can go away */
    if (Thread->ApcProcess != QzSystemProcess)
    {
        PQZ_PROCESS Ignored;
        QzPsAttach(QzSystemProcess, &Ignored);
    }
    if (Process && InterlockedDecrement((PLONG)&Process->ActiveThreads) == 0 && Process != QzSystemProcess)
        QzPsProcessExited(Process, ExitStatus);

    Thread->PsThread = NULL;
    PsThread->Thread = NULL;         /* the scheduler frees it once we are off its stack */
    ObDereferenceObject(PsThread);   /* the thread's own reference */
    QzExitCurrentThread();
    for (;;);
}

NTSTATUS
NTAPI
PsTerminateSystemThread(IN NTSTATUS ExitStatus)
{
    PQZ_PS_THREAD PsThread = QzCurrentThread->PsThread;

    if (!PsThread || PsThread->UserThread)
        return STATUS_INVALID_PARAMETER;   /* not a thread made by PsCreateSystemThread */
    QzPsTerminateCurrentThread(ExitStatus);
}

/* Ask every thread of Process (but the current one) to end */
VOID
QzPsTerminateProcess(PQZ_PROCESS Process, NTSTATUS ExitStatus)
{
    PLIST_ENTRY Entry;

    ExAcquireFastMutex(&Process->AddressLock);
    if (Process->ExitStatus == STATUS_PENDING)
        Process->ExitStatus = ExitStatus;
    Process->Exiting = TRUE;
    for (Entry = Process->ThreadList.Flink; Entry != &Process->ThreadList; Entry = Entry->Flink)
    {
        PQZ_PS_THREAD Other = CONTAINING_RECORD(Entry, QZ_PS_THREAD, ProcessLink);
        if (Other->Thread == QzCurrentThread || Other->ExitedThread)
            continue;
        Other->PendingExitStatus = ExitStatus;
        Other->TerminatePending = TRUE;
        /* A thread that never ran, or is suspended, goes when it is next scheduled */
        if (Other->Thread && Other->Thread->State == QzThreadNew)
            QzReadyNewThread(Other->Thread);
        else if (Other->SuspendCount)
        {
            Other->SuspendCount = 0;
            KeReleaseSemaphore(&Other->SuspendSemaphore, IO_NO_INCREMENT, 1, FALSE);
        }
    }
    ExReleaseFastMutex(&Process->AddressLock);
    if (Process->ActiveThreads == 0 && !KeReadStateEvent(&Process->Exited))
        QzPsProcessExited(Process, ExitStatus);
}

NTSTATUS
NTAPI
NtTerminateThread(IN HANDLE ThreadHandle OPTIONAL,
                  IN NTSTATUS ExitStatus)
{
    PQZ_PS_THREAD PsThread;
    NTSTATUS Status;

    if (!ThreadHandle || ThreadHandle == NtCurrentThread())
    {
        if (!QzCurrentThread->PsThread)
            return STATUS_INVALID_HANDLE;
        QzPsTerminateCurrentThread(ExitStatus);
    }
    Status = ObReferenceObjectByHandle(ThreadHandle, THREAD_TERMINATE, PsThreadType, ExGetPreviousMode(),
                                       (PVOID *)&PsThread, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    if (PsThread->Thread == QzCurrentThread)
    {
        ObDereferenceObject(PsThread);
        QzPsTerminateCurrentThread(ExitStatus);
    }
    if (!PsThread->ExitedThread)
    {
        PsThread->PendingExitStatus = ExitStatus;
        PsThread->TerminatePending = TRUE;
        if (PsThread->Thread && PsThread->Thread->State == QzThreadNew)
            QzReadyNewThread(PsThread->Thread);
    }
    ObDereferenceObject(PsThread);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NtTerminateProcess(IN HANDLE ProcessHandle OPTIONAL,
                   IN NTSTATUS ExitStatus)
{
    PQZ_PROCESS Process;
    BOOLEAN Self;
    NTSTATUS Status;

    /* NULL: every thread of the current process but this one */
    if (!ProcessHandle)
    {
        PQZ_PROCESS Current = QzCurrentThread->Process;
        NTSTATUS Saved = Current->ExitStatus;
        QzPsTerminateProcess(Current, ExitStatus);
        Current->Exiting = FALSE;   /* the caller goes on */
        Current->ExitStatus = Saved;
        return STATUS_SUCCESS;
    }
    Status = ObReferenceObjectByHandle(ProcessHandle, PROCESS_TERMINATE, PsProcessType, ExGetPreviousMode(),
                                       (PVOID *)&Process, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    if (Process == QzSystemProcess)
    {
        ObDereferenceObject(Process);
        return STATUS_ACCESS_DENIED;
    }
    Self = (Process == QzCurrentThread->Process);
    QzPsTerminateProcess(Process, ExitStatus);
    ObDereferenceObject(Process);
    if (Self)
        QzPsTerminateCurrentThread(ExitStatus);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
ZwTerminateProcess(IN HANDLE ProcessHandle OPTIONAL, IN NTSTATUS ExitStatus)
{
    return QZ_AS_KERNEL(NtTerminateProcess(ProcessHandle, ExitStatus));
}

/* A suspended thread that never ran starts when resumed */
NTSTATUS
NTAPI
NtResumeThread(IN HANDLE ThreadHandle,
               OUT PULONG SuspendCount OPTIONAL)
{
    PQZ_PS_THREAD PsThread;
    ULONG Previous;
    NTSTATUS Status;

    Status = ObReferenceObjectByHandle(ThreadHandle, THREAD_SUSPEND_RESUME, PsThreadType, ExGetPreviousMode(),
                                       (PVOID *)&PsThread, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    Previous = (ULONG)PsThread->SuspendCount;
    if (PsThread->SuspendCount > 0 && --PsThread->SuspendCount == 0)
    {
        if (PsThread->Thread && PsThread->Thread->State == QzThreadNew)
            QzReadyNewThread(PsThread->Thread);
        else
            KeReleaseSemaphore(&PsThread->SuspendSemaphore, IO_NO_INCREMENT, 1, FALSE);
    }
    ObDereferenceObject(PsThread);
    if (SuspendCount)
        QzWriteArg(ExGetPreviousMode(), SuspendCount, &Previous, sizeof(Previous));
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NtSuspendThread(IN HANDLE ThreadHandle,
                OUT PULONG PreviousSuspendCount OPTIONAL)
{
    PQZ_PS_THREAD PsThread;
    ULONG Previous;
    NTSTATUS Status;

    /* Suspension takes effect at the next return to user mode */
    Status = ObReferenceObjectByHandle(ThreadHandle, THREAD_SUSPEND_RESUME, PsThreadType, ExGetPreviousMode(),
                                       (PVOID *)&PsThread, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    Previous = (ULONG)PsThread->SuspendCount++;
    ObDereferenceObject(PsThread);
    if (PreviousSuspendCount)
        QzWriteArg(ExGetPreviousMode(), PreviousSuspendCount, &Previous, sizeof(Previous));
    return STATUS_SUCCESS;
}

static VOID NTAPI
QzPsThreadStart(PVOID Context)
{
    PQZ_PS_THREAD PsThread = Context;
    QzCurrentThread->PsThread = PsThread;
    PsThread->StartRoutine(PsThread->StartContext);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS
NTAPI
PsCreateSystemThread(OUT PHANDLE ThreadHandle,
                     IN ULONG DesiredAccess,
                     IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL,
                     IN HANDLE ProcessHandle OPTIONAL,
                     OUT PCLIENT_ID ClientId OPTIONAL,
                     IN PKSTART_ROUTINE StartRoutine,
                     IN PVOID StartContext)
{
    PQZ_PS_THREAD PsThread;
    PQZ_THREAD Thread;
    PQZ_PROCESS Process = QzSystemProcess;
    NTSTATUS Status;

    /* A system thread may run in another process (drivers do this to own
       handles in that process); it is still a kernel-mode thread */
    if (ProcessHandle)
    {
        Status = ObReferenceObjectByHandle(ProcessHandle, PROCESS_CREATE_THREAD, PsProcessType,
                                           KernelMode, (PVOID *)&Process, NULL);
        if (!NT_SUCCESS(Status))
            return Status;
        ObDereferenceObject(Process);   /* the thread's reference is taken below */
    }

    Status = ObCreateObject(KernelMode, PsThreadType, ObjectAttributes, KernelMode, NULL,
                            sizeof(QZ_PS_THREAD), 0, 0, (PVOID *)&PsThread);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlZeroMemory(PsThread, sizeof(*PsThread));
    KeInitializeEvent(&PsThread->Terminated, NotificationEvent, FALSE);
    PsThread->StartRoutine = StartRoutine;
    PsThread->StartContext = StartContext;
    PsThread->ExitStatus = STATUS_PENDING;
    PsThread->Process = Process;
    ObReferenceObject(Process);
    KeQuerySystemTime(&PsThread->CreateTime);
    PsThread->UniqueId = QzPsAllocateId(PsThread);

    /* One reference for the handle (the creation one), one for the thread */
    ObReferenceObject(PsThread);
    Status = ObInsertObject(PsThread, NULL, DesiredAccess, 0, NULL, ThreadHandle);
    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(PsThread);
        return Status;
    }

    Thread = QzCreateThreadEx("system", QzPsThreadStart, PsThread, Process, FALSE);
    if (!Thread)
    {
        ZwClose(*ThreadHandle);
        ObDereferenceObject(PsThread);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    PsThread->Thread = Thread;
    ExAcquireFastMutex(&Process->AddressLock);
    InsertTailList(&Process->ThreadList, &PsThread->ProcessLink);
    ExReleaseFastMutex(&Process->AddressLock);
    InterlockedIncrement((PLONG)&Process->ActiveThreads);
    if (ClientId)
    {
        ClientId->UniqueProcess = (HANDLE)(ULONG_PTR)Process->UniqueId;
        ClientId->UniqueThread = (HANDLE)(ULONG_PTR)PsThread->UniqueId;
    }
    QzReadyNewThread(Thread);
    return STATUS_SUCCESS;
}

BOOLEAN
NTAPI
PsGetThreadHardErrorsAreDisabled(IN PETHREAD Thread)
{
    UNREFERENCED_PARAMETER(Thread);
    return TRUE;   /* there is nobody to show a hard error to */
}

VOID
NTAPI
PsSetThreadHardErrorsAreDisabled(IN PETHREAD Thread, IN BOOLEAN HardErrorsAreDisabled)
{
    UNREFERENCED_PARAMETER(Thread);
    UNREFERENCED_PARAMETER(HardErrorsAreDisabled);
}

static VOID NTAPI
QzPsDeleteThread(PVOID Object)
{
    PQZ_PS_THREAD PsThread = Object;

    if (PsThread->UniqueId)
        QzPsFreeId(PsThread->UniqueId);
    if (PsThread->Process)
    {
        if (PsThread->ProcessLink.Flink)
        {
            ExAcquireFastMutex(&PsThread->Process->AddressLock);
            RemoveEntryList(&PsThread->ProcessLink);
            ExReleaseFastMutex(&PsThread->Process->AddressLock);
        }
        ObDereferenceObject(PsThread->Process);
    }
}

static POBJECT_TYPE
QzPsCreateType(PCWSTR Name, PGENERIC_MAPPING Mapping, ULONG ValidAccess, OB_DELETE_METHOD Delete)
{
    OBJECT_TYPE_INITIALIZER Init;
    UNICODE_STRING TypeName;
    POBJECT_TYPE Type;

    RtlZeroMemory(&Init, sizeof(Init));
    Init.Length = sizeof(Init);
    Init.GenericMapping = *Mapping;
    Init.ValidAccessMask = ValidAccess;
    Init.PoolType = NonPagedPool;
    Init.CaseInsensitive = TRUE;
    Init.DeleteProcedure = Delete;
    RtlInitUnicodeString(&TypeName, Name);
    if (!NT_SUCCESS(ObCreateObjectType(&TypeName, &Init, NULL, &Type)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, 1, 0, 0, 0);
    return Type;
}

VOID
QzPsInit(VOID)
{
    PsThreadType = QzPsCreateType(L"Thread", &QzThreadMapping, THREAD_ALL_ACCESS, QzPsDeleteThread);
    PsProcessType = QzPsCreateType(L"Process", &QzProcessMapping, PROCESS_ALL_ACCESS, QzPsDeleteProcess);
    QzPsInitProcesses();
    QzMmSectionInit();
    QzLpcInit();
    QzSeInitTokens();
    QzExInitKeyedEvents();
    QzExInitWin32Types();
}
