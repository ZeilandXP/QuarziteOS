/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The kernel side of win32k: callouts, GUI threads, the Desktop
 *              and WindowStation object types, jobs and tokens it asks about
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * win32k registers its callouts (PsEstablishWin32Callouts) and its system
 * service table (KeAddSystemServiceTable, table 1). A thread becomes a
 * GUI thread on its first call into that table: the process callout runs
 * for its process if it has no Win32 process yet, then the thread
 * callout. Their exits run the callouts again. Desktops and window
 * stations are kernel object types whose procedures are win32k's.
 */

#include "qzio.h"
#include "qzps.h"

static WIN32_CALLOUTS_FPNS QzWin32;
static BOOLEAN QzWin32Ready;
POBJECT_TYPE ExDesktopObjectType;
POBJECT_TYPE ExWindowStationObjectType;
POBJECT_TYPE PsJobType;

PQZ_PS_THREAD
QzPsThreadOf(PVOID Thread)
{
    if (!Thread)
        return NULL;
    return QzPsIsObject(Thread) ? (PQZ_PS_THREAD)Thread : ((PQZ_THREAD)Thread)->PsThread;
}

VOID
NTAPI
PsEstablishWin32Callouts(IN PWIN32_CALLOUTS_FPNS CalloutData)
{
    QzWin32 = *CalloutData;
    QzWin32Ready = TRUE;
    DbgPrint(QZ_KERNEL_NAME ": win32k registered its callouts\n");
}

/* The first call into table 1: this thread (and its process) go GUI */
NTSTATUS
QzPsConvertToGuiThread(VOID)
{
    PQZ_PS_THREAD Thread = QzCurrentThread->PsThread;
    PQZ_PROCESS Process;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!QzWin32Ready || !Thread)
        return STATUS_INVALID_SYSTEM_SERVICE;
    if (Thread->GuiThread)
        return STATUS_SUCCESS;
    Process = Thread->Process;
    if (!Process->Win32Process && QzWin32.ProcessCallout)
    {
        Status = QzWin32.ProcessCallout((PEPROCESS)Process, TRUE);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    Thread->GuiThread = TRUE;
    if (QzWin32.ThreadCallout)
    {
        Status = QzWin32.ThreadCallout((PETHREAD)Thread, PsW32ThreadCalloutInitialize);
        if (!NT_SUCCESS(Status))
            Thread->GuiThread = FALSE;
    }
    return Status;
}

VOID
QzPsWin32ThreadExit(PQZ_PS_THREAD Thread)
{
    if (Thread->GuiThread && QzWin32.ThreadCallout)
        QzWin32.ThreadCallout((PETHREAD)Thread, PsW32ThreadCalloutExit);
    Thread->GuiThread = FALSE;
}

VOID
QzPsWin32ProcessExit(PQZ_PROCESS Process)
{
    if (Process->Win32Process && QzWin32.ProcessCallout)
        QzWin32.ProcessCallout((PEPROCESS)Process, FALSE);
}

/* ---------------------------------------------------------------------- */
/* What win32k keeps in processes and threads                              */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
PsSetProcessWin32Process(IN OUT PEPROCESS Process, IN PVOID Win32Process OPTIONAL,
                         IN PVOID OldWin32Process OPTIONAL)
{
    PQZ_PROCESS Body = (PQZ_PROCESS)Process;
    /* Set only if it still is what the caller saw */
    if (InterlockedCompareExchangePointer(&Body->Win32Process, Win32Process, OldWin32Process) != OldWin32Process)
        return STATUS_PROCESS_IS_TERMINATING;
    return STATUS_SUCCESS;
}

PVOID
NTAPI
PsSetThreadWin32Thread(IN OUT PETHREAD Thread, IN PVOID Win32Thread OPTIONAL, IN PVOID OldWin32Thread OPTIONAL)
{
    PQZ_PS_THREAD Body = QzPsThreadOf(Thread);
    return Body ? InterlockedCompareExchangePointer(&Body->Win32Thread, Win32Thread, OldWin32Thread) : NULL;
}

PVOID
NTAPI
PsGetProcessWin32WindowStation(IN PEPROCESS Process)
{
    return ((PQZ_PROCESS)Process)->Win32WindowStation;
}

VOID
NTAPI
PsSetProcessWindowStation(IN OUT PEPROCESS Process, IN PVOID WindowStation OPTIONAL)
{
    ((PQZ_PROCESS)Process)->Win32WindowStation = WindowStation;
}

BOOLEAN
NTAPI
PsGetProcessExitProcessCalled(IN PEPROCESS Process)
{
    return ((PQZ_PROCESS)Process)->Exiting;
}

BOOLEAN
NTAPI
PsIsSystemProcess(IN PEPROCESS Process)
{
    return (PQZ_PROCESS)Process == QzSystemProcess;
}

ULONG
NTAPI
PsGetThreadFreezeCount(IN PETHREAD Thread)
{
    /* Suspension and freezing are one count here */
    PQZ_PS_THREAD PsThread = QzPsThreadOf(Thread);
    return PsThread ? (ULONG)PsThread->SuspendCount : 0;
}

PVOID
NTAPI
PsGetProcessSectionBaseAddress(IN PEPROCESS Process)
{
    return ((PQZ_PROCESS)Process)->SectionBaseAddress;
}

/* No jobs */
PEJOB
NTAPI
PsGetProcessJob(IN PEPROCESS Process)
{
    UNREFERENCED_PARAMETER(Process);
    return NULL;
}

ULONG
NTAPI
PsGetJobUIRestrictionsClass(IN PEJOB Job)
{
    UNREFERENCED_PARAMETER(Job);
    return 0;
}

/* Tokens: the system token for every process, no impersonation */
PACCESS_TOKEN
NTAPI
PsReferencePrimaryToken(IN PEPROCESS Process)
{
    PVOID Token = NULL;
    HANDLE Handle;
    UNREFERENCED_PARAMETER(Process);
    if (NT_SUCCESS(ZwOpenProcessTokenEx(NtCurrentProcess(), TOKEN_QUERY, OBJ_KERNEL_HANDLE, &Handle)))
    {
        ObReferenceObjectByHandle(Handle, 0, SeTokenObjectType, KernelMode, &Token, NULL);
        ZwClose(Handle);
    }
    return Token;
}

PACCESS_TOKEN
NTAPI
PsReferenceImpersonationToken(IN OUT PETHREAD Thread, OUT PBOOLEAN CopyOnOpen, OUT PBOOLEAN EffectiveOnly,
                              OUT PSECURITY_IMPERSONATION_LEVEL ImpersonationLevel)
{
    UNREFERENCED_PARAMETER(Thread);
    *CopyOnOpen = FALSE;
    *EffectiveOnly = FALSE;
    *ImpersonationLevel = SecurityAnonymous;
    return NULL;
}

NTSTATUS
NTAPI
SeQueryAuthenticationIdToken(IN PACCESS_TOKEN Token, OUT PLUID LogonId)
{
    UNREFERENCED_PARAMETER(Token);
    LogonId->LowPart = 0x3E7;           /* SYSTEM_LUID */
    LogonId->HighPart = 0;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
ZwOpenThreadToken(IN HANDLE ThreadHandle, IN ACCESS_MASK DesiredAccess, IN BOOLEAN OpenAsSelf,
                  OUT PHANDLE TokenHandle)
{
    UNREFERENCED_PARAMETER(ThreadHandle);
    UNREFERENCED_PARAMETER(DesiredAccess);
    UNREFERENCED_PARAMETER(OpenAsSelf);
    UNREFERENCED_PARAMETER(TokenHandle);
    return STATUS_NO_TOKEN;
}

NTSTATUS
NTAPI
ZwOpenThreadTokenEx(IN HANDLE ThreadHandle, IN ACCESS_MASK DesiredAccess, IN BOOLEAN OpenAsSelf,
                    IN ULONG HandleAttributes, OUT PHANDLE TokenHandle)
{
    UNREFERENCED_PARAMETER(HandleAttributes);
    return ZwOpenThreadToken(ThreadHandle, DesiredAccess, OpenAsSelf, TokenHandle);
}

/* ---------------------------------------------------------------------- */
/* Desktops and window stations: kernel types, win32k procedures           */
/* ---------------------------------------------------------------------- */

static NTSTATUS
NTAPI
QzWinObjOpen(PKWIN32_SESSION_CALLOUT Callout, OB_OPEN_REASON Reason, PEPROCESS Process, PVOID Object,
             PACCESS_MASK GrantedAccess, ULONG HandleCount)
{
    WIN32_OPENMETHOD_PARAMETERS Parameters;
    if (!Callout)
        return STATUS_SUCCESS;
    Parameters.OpenReason = Reason;
    Parameters.Process = Process;
    Parameters.Object = Object;
    Parameters.GrantedAccess = *GrantedAccess;
    Parameters.HandleCount = HandleCount;
    return Callout(&Parameters);
}

static NTSTATUS
NTAPI
QzDesktopOpen(OB_OPEN_REASON Reason, KPROCESSOR_MODE AccessMode, PEPROCESS Process, PVOID Object,
              PACCESS_MASK GrantedAccess, ULONG HandleCount)
{
    UNREFERENCED_PARAMETER(AccessMode);
    return QzWinObjOpen(QzWin32.DesktopOpenProcedure, Reason, Process, Object, GrantedAccess, HandleCount);
}

static NTSTATUS
NTAPI
QzWinStaOpen(OB_OPEN_REASON Reason, KPROCESSOR_MODE AccessMode, PEPROCESS Process, PVOID Object,
             PACCESS_MASK GrantedAccess, ULONG HandleCount)
{
    UNREFERENCED_PARAMETER(AccessMode);
    return QzWinObjOpen(QzWin32.WindowStationOpenProcedure, Reason, Process, Object, GrantedAccess, HandleCount);
}

static BOOLEAN
QzWinObjOkToClose(PKWIN32_SESSION_CALLOUT Callout, PEPROCESS Process, PVOID Object, HANDLE Handle,
                  KPROCESSOR_MODE AccessMode)
{
    WIN32_OKAYTOCLOSEMETHOD_PARAMETERS Parameters;
    if (!Callout)
        return TRUE;
    Parameters.Process = Process;
    Parameters.Object = Object;
    Parameters.Handle = Handle;
    Parameters.PreviousMode = AccessMode;
    return NT_SUCCESS(Callout(&Parameters));
}

static BOOLEAN
NTAPI
QzDesktopOkToClose(PEPROCESS Process, PVOID Object, HANDLE Handle, KPROCESSOR_MODE AccessMode)
{
    return QzWinObjOkToClose(QzWin32.DesktopOkToCloseProcedure, Process, Object, Handle, AccessMode);
}

static BOOLEAN
NTAPI
QzWinStaOkToClose(PEPROCESS Process, PVOID Object, HANDLE Handle, KPROCESSOR_MODE AccessMode)
{
    return QzWinObjOkToClose(QzWin32.WindowStationOkToCloseProcedure, Process, Object, Handle, AccessMode);
}

static VOID
QzWinObjClose(PKWIN32_SESSION_CALLOUT Callout, PEPROCESS Process, PVOID Object, ACCESS_MASK GrantedAccess,
              ULONG_PTR ProcessHandleCount, ULONG_PTR SystemHandleCount)
{
    WIN32_CLOSEMETHOD_PARAMETERS Parameters;
    if (!Callout)
        return;
    Parameters.Process = Process;
    Parameters.Object = Object;
    Parameters.AccessMask = GrantedAccess;
    Parameters.ProcessHandleCount = (ULONG)ProcessHandleCount;
    Parameters.SystemHandleCount = (ULONG)SystemHandleCount;
    Callout(&Parameters);
}

static VOID
NTAPI
QzDesktopClose(PEPROCESS Process, PVOID Object, ACCESS_MASK GrantedAccess, ULONG_PTR ProcessHandleCount,
               ULONG_PTR SystemHandleCount)
{
    QzWinObjClose(QzWin32.DesktopCloseProcedure, Process, Object, GrantedAccess, ProcessHandleCount,
                  SystemHandleCount);
}

static VOID
NTAPI
QzWinStaClose(PEPROCESS Process, PVOID Object, ACCESS_MASK GrantedAccess, ULONG_PTR ProcessHandleCount,
              ULONG_PTR SystemHandleCount)
{
    QzWinObjClose(QzWin32.WindowStationCloseProcedure, Process, Object, GrantedAccess, ProcessHandleCount,
                  SystemHandleCount);
}

static VOID
NTAPI
QzDesktopDelete(PVOID Object)
{
    WIN32_DELETEMETHOD_PARAMETERS Parameters;
    Parameters.Object = Object;
    if (QzWin32.DesktopDeleteProcedure)
        QzWin32.DesktopDeleteProcedure(&Parameters);
}

static VOID
NTAPI
QzWinStaDelete(PVOID Object)
{
    WIN32_DELETEMETHOD_PARAMETERS Parameters;
    Parameters.Object = Object;
    if (QzWin32.WindowStationDeleteProcedure)
        QzWin32.WindowStationDeleteProcedure(&Parameters);
}

/* "WinSta0\Default": the window station parses its desktops' names */
static NTSTATUS
NTAPI
QzWinStaParse(PVOID ParseObject, PVOID ObjectType, PACCESS_STATE AccessState, KPROCESSOR_MODE AccessMode,
              ULONG Attributes, PUNICODE_STRING CompleteName, PUNICODE_STRING RemainingName, PVOID Context,
              PSECURITY_QUALITY_OF_SERVICE SecurityQos, PVOID *Object)
{
    WIN32_PARSEMETHOD_PARAMETERS Parameters;
    if (!QzWin32.WindowStationParseProcedure)
        return STATUS_OBJECT_TYPE_MISMATCH;
    Parameters.ParseObject = ParseObject;
    Parameters.ObjectType = ObjectType;
    Parameters.AccessState = AccessState;
    Parameters.AccessMode = AccessMode;
    Parameters.Attributes = Attributes;
    Parameters.CompleteName = CompleteName;
    Parameters.RemainingName = RemainingName;
    Parameters.Context = Context;
    Parameters.SecurityQos = SecurityQos;
    Parameters.Object = Object;
    return QzWin32.WindowStationParseProcedure(&Parameters);
}

static POBJECT_TYPE
QzExCreateWinType(PCWSTR TypeName, ULONG AllAccess, OB_OPEN_METHOD Open, OB_OKAYTOCLOSE_METHOD OkToClose,
                  OB_CLOSE_METHOD Close, OB_DELETE_METHOD Delete, OB_PARSE_METHOD Parse)
{
    static GENERIC_MAPPING Mapping = { STANDARD_RIGHTS_READ, STANDARD_RIGHTS_WRITE, STANDARD_RIGHTS_EXECUTE, 0x37F };
    OBJECT_TYPE_INITIALIZER Init;
    UNICODE_STRING Name;
    POBJECT_TYPE Type = NULL;

    RtlInitUnicodeString(&Name, TypeName);
    RtlZeroMemory(&Init, sizeof(Init));
    Init.Length = sizeof(Init);
    Init.GenericMapping = Mapping;
    Init.ValidAccessMask = AllAccess | STANDARD_RIGHTS_REQUIRED;
    Init.PoolType = NonPagedPool;
    Init.UseDefaultObject = TRUE;
    Init.OpenProcedure = Open;
    Init.OkayToCloseProcedure = OkToClose;
    Init.CloseProcedure = Close;
    Init.DeleteProcedure = Delete;
    Init.ParseProcedure = Parse;
    ObCreateObjectType(&Name, &Init, NULL, &Type);
    return Type;
}

VOID
QzExInitWin32Types(VOID)
{
    ExWindowStationObjectType = QzExCreateWinType(L"WindowStation", 0x37F, QzWinStaOpen, QzWinStaOkToClose,
                                                  QzWinStaClose, QzWinStaDelete, QzWinStaParse);
    ExDesktopObjectType = QzExCreateWinType(L"Desktop", 0x1FF, QzDesktopOpen, QzDesktopOkToClose, QzDesktopClose,
                                            QzDesktopDelete, NULL);
    PsJobType = QzExCreateWinType(L"Job", 0x1F, NULL, NULL, NULL, NULL, NULL);
}

/* win32k's GDI batch: flushed before win32k calls and after callbacks, when the TEB holds some */
VOID
QzPsWin32FlushBatch(VOID)
{
    PQZ_PS_THREAD Thread = QzCurrentThread->PsThread;
    ULONG Count = 0;

    if (!Thread || !Thread->Teb || !Thread->GuiThread || !QzWin32.BatchFlushRoutine)
        return;
    if (QzCopyFromUser(&Count, &((PTEB)Thread->Teb)->GdiBatchCount, sizeof(Count)) && Count)
        QzWin32.BatchFlushRoutine();
}

/* No jobs: a process is never in one */
NTSTATUS
NTAPI
NtIsProcessInJob(IN HANDLE ProcessHandle, IN HANDLE JobHandle OPTIONAL)
{
    PVOID Process;
    NTSTATUS Status = ObReferenceObjectByHandle(ProcessHandle, PROCESS_QUERY_INFORMATION, PsProcessType,
                                                ExGetPreviousMode(), &Process, NULL);
    UNREFERENCED_PARAMETER(JobHandle);
    if (!NT_SUCCESS(Status))
        return Status;
    ObDereferenceObject(Process);
    return STATUS_PROCESS_NOT_IN_JOB;
}
