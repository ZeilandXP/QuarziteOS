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

NTSTATUS
NTAPI
PsTerminateSystemThread(IN NTSTATUS ExitStatus)
{
    PQZ_PS_THREAD PsThread = QzCurrentThread->PsThread;

    if (!PsThread)
        return STATUS_INVALID_PARAMETER;   /* not a thread made by PsCreateSystemThread */
    PsThread->ExitStatus = ExitStatus;
    KeQuerySystemTime(&PsThread->ExitTime);
    KeSetEvent(&PsThread->Terminated, IO_NO_INCREMENT, FALSE);
    if (PsThread->Process)
        InterlockedDecrement((PLONG)&PsThread->Process->ActiveThreads);
    QzCurrentThread->PsThread = NULL;
    ObDereferenceObject(PsThread);   /* the thread's own reference */
    QzExitCurrentThread();
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
}
