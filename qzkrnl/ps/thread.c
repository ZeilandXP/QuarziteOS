/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     System threads and the System process as objects
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Processes are stage B6. Until then a system thread created by a driver
 * is a scheduler thread plus a "Thread" object: its body starts with a
 * notification event (in the place of KTHREAD's dispatcher header) that
 * is signaled when the thread exits, so drivers can wait for their
 * threads by handle or by object as on NT. The System process is a
 * "Process" object with a zeroed body.
 */

#include "qzio.h"

typedef struct _QZ_PS_THREAD
{
    KEVENT Terminated;             /* must be first: waits use it */
    PQZ_THREAD Thread;
    PKSTART_ROUTINE StartRoutine;
    PVOID StartContext;
    NTSTATUS ExitStatus;
} QZ_PS_THREAD, *PQZ_PS_THREAD;

POBJECT_TYPE PsThreadType;
POBJECT_TYPE PsProcessType;
PEPROCESS PsInitialSystemProcess;

#define QZ_PROCESS_BODY 0x400

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
    KeSetEvent(&PsThread->Terminated, IO_NO_INCREMENT, FALSE);
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
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ProcessHandle);   /* everything runs in System */

    Status = ObCreateObject(KernelMode, PsThreadType, ObjectAttributes, KernelMode, NULL,
                            sizeof(QZ_PS_THREAD), 0, 0, (PVOID *)&PsThread);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlZeroMemory(PsThread, sizeof(*PsThread));
    KeInitializeEvent(&PsThread->Terminated, NotificationEvent, FALSE);
    PsThread->StartRoutine = StartRoutine;
    PsThread->StartContext = StartContext;
    PsThread->ExitStatus = STATUS_PENDING;

    /* One reference for the handle (the creation one), one for the thread */
    ObReferenceObject(PsThread);
    Status = ObInsertObject(PsThread, NULL, DesiredAccess, 0, NULL, ThreadHandle);
    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(PsThread);
        return Status;
    }

    Thread = QzCreateThread("system", QzPsThreadStart, PsThread);
    if (!Thread)
    {
        ZwClose(*ThreadHandle);
        ObDereferenceObject(PsThread);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    PsThread->Thread = Thread;
    if (ClientId)
    {
        ClientId->UniqueProcess = (HANDLE)4;
        ClientId->UniqueThread = (HANDLE)(ULONG_PTR)(Thread->Id * 4);
    }
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

static POBJECT_TYPE
QzPsCreateType(PCWSTR Name, PGENERIC_MAPPING Mapping, ULONG ValidAccess)
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
    RtlInitUnicodeString(&TypeName, Name);
    if (!NT_SUCCESS(ObCreateObjectType(&TypeName, &Init, NULL, &Type)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, 1, 0, 0, 0);
    return Type;
}

VOID
QzPsInit(VOID)
{
    PVOID Process;

    PsThreadType = QzPsCreateType(L"Thread", &QzThreadMapping, THREAD_ALL_ACCESS);
    PsProcessType = QzPsCreateType(L"Process", &QzProcessMapping, PROCESS_ALL_ACCESS);

    if (!NT_SUCCESS(ObCreateObject(KernelMode, PsProcessType, NULL, KernelMode, NULL,
                                   QZ_PROCESS_BODY, 0, 0, &Process)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, 2, 0, 0, 0);
    RtlZeroMemory(Process, QZ_PROCESS_BODY);
    ObInsertObject(Process, NULL, 0, 0, NULL, NULL);
    PsInitialSystemProcess = Process;
}
