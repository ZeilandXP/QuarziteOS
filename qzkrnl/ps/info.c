/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Process and thread information services, and the small
 *              time/locale/identifier services every process starts with
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qzio.h"
#include "qzps.h"

static NTSTATUS
QzPsReturn(KPROCESSOR_MODE Mode, PVOID Buffer, ULONG Length, PULONG ReturnLength, const VOID *Data,
           ULONG Size)
{
    if (ReturnLength && !QzWriteArg(Mode, ReturnLength, &Size, sizeof(Size)))
        return STATUS_ACCESS_VIOLATION;
    if (Length < Size)
        return STATUS_INFO_LENGTH_MISMATCH;
    return QzWriteArg(Mode, Buffer, Data, Size) ? STATUS_SUCCESS : STATUS_ACCESS_VIOLATION;
}

/* ---------------------------------------------------------------------- */
/* Processes                                                               */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
NtQueryInformationProcess(IN HANDLE ProcessHandle,
                          IN PROCESSINFOCLASS ProcessInformationClass,
                          OUT PVOID ProcessInformation,
                          IN ULONG ProcessInformationLength,
                          OUT PULONG ReturnLength OPTIONAL)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PQZ_PROCESS Process;
    NTSTATUS Status;

    Status = ObReferenceObjectByHandle(ProcessHandle, PROCESS_QUERY_INFORMATION, PsProcessType, Mode,
                                       (PVOID *)&Process, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    switch (ProcessInformationClass)
    {
        case ProcessBasicInformation:
        {
            PROCESS_BASIC_INFORMATION Info;
            RtlZeroMemory(&Info, sizeof(Info));
            Info.ExitStatus = Process->ExitStatus;
            Info.PebBaseAddress = Process->Peb;
            Info.AffinityMask = Process->Affinity;
            Info.BasePriority = Process->BasePriority;
            Info.UniqueProcessId = Process->UniqueId;
            Info.InheritedFromUniqueProcessId = Process->InheritedFromUniqueId;
            Status = QzPsReturn(Mode, ProcessInformation, ProcessInformationLength, ReturnLength, &Info, sizeof(Info));
            break;
        }
        case ProcessDefaultHardErrorMode:
            Status = QzPsReturn(Mode, ProcessInformation, ProcessInformationLength, ReturnLength,
                                &Process->HardErrorMode, sizeof(ULONG));
            break;
        case ProcessDebugPort:
        case ProcessWow64Information:
        {
            ULONG_PTR Zero = 0;
            Status = QzPsReturn(Mode, ProcessInformation, ProcessInformationLength, ReturnLength, &Zero, sizeof(Zero));
            break;
        }
        case ProcessHandleCount:
        {
            ULONG Count = Process->HandleTable ? QzObTableHandleCount(Process->HandleTable) : 0;
            Status = QzPsReturn(Mode, ProcessInformation, ProcessInformationLength, ReturnLength, &Count, sizeof(Count));
            break;
        }
        case ProcessTimes:
        {
            KERNEL_USER_TIMES Times;
            RtlZeroMemory(&Times, sizeof(Times));
            Times.CreateTime = Process->CreateTime;
            Times.ExitTime = Process->ExitTime;
            Status = QzPsReturn(Mode, ProcessInformation, ProcessInformationLength, ReturnLength, &Times, sizeof(Times));
            break;
        }
        case ProcessVmCounters:
        {
            VM_COUNTERS Counters;
            RtlZeroMemory(&Counters, sizeof(Counters));
            Counters.WorkingSetSize = Counters.PeakWorkingSetSize = Process->PrivatePages * PAGE_SIZE;
            Counters.PagefileUsage = Counters.PeakPagefileUsage = Process->PrivatePages * PAGE_SIZE;
            Status = QzPsReturn(Mode, ProcessInformation, ProcessInformationLength, ReturnLength,
                                &Counters, sizeof(Counters));
            break;
        }
        case ProcessIoCounters:
        {
            IO_COUNTERS Counters;
            RtlZeroMemory(&Counters, sizeof(Counters));
            Status = QzPsReturn(Mode, ProcessInformation, ProcessInformationLength, ReturnLength,
                                &Counters, sizeof(Counters));
            break;
        }
        case ProcessSessionInformation:
        {
            PROCESS_SESSION_INFORMATION Info = { 0 };
            Status = QzPsReturn(Mode, ProcessInformation, ProcessInformationLength, ReturnLength, &Info, sizeof(Info));
            break;
        }
        case ProcessPriorityClass:
        {
            PROCESS_PRIORITY_CLASS Info = { FALSE, PROCESS_PRIORITY_CLASS_NORMAL };
            Status = QzPsReturn(Mode, ProcessInformation, ProcessInformationLength, ReturnLength, &Info, sizeof(Info));
            break;
        }
        case ProcessCookie:
        {
            if (!Process->Cookie)
            {
                LARGE_INTEGER Now;
                KeQuerySystemTime(&Now);
                Process->Cookie = (Now.LowPart ^ (Process->UniqueId << 16) ^ 0x9E3779B9) | 1;
            }
            Status = QzPsReturn(Mode, ProcessInformation, ProcessInformationLength, ReturnLength,
                                &Process->Cookie, sizeof(ULONG));
            break;
        }
        case ProcessExecuteFlags:
        {
            ULONG Flags = 0x2 /* MEM_EXECUTE_OPTION_ENABLE */;
            Status = QzPsReturn(Mode, ProcessInformation, ProcessInformationLength, ReturnLength, &Flags, sizeof(Flags));
            break;
        }
        case ProcessImageFileName:
        {
            /* A UNICODE_STRING followed by the name, in the caller's buffer */
            ULONG Needed = sizeof(UNICODE_STRING) + Process->ImagePathName.Length + sizeof(WCHAR);
            UNICODE_STRING Name;
            if (ReturnLength)
                QzWriteArg(Mode, ReturnLength, &Needed, sizeof(Needed));
            if (ProcessInformationLength < Needed)
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            Name.Length = Process->ImagePathName.Length;
            Name.MaximumLength = Name.Length + sizeof(WCHAR);
            Name.Buffer = (PWSTR)((PUCHAR)ProcessInformation + sizeof(UNICODE_STRING));
            if (!QzWriteArg(Mode, ProcessInformation, &Name, sizeof(Name)) ||
                (Name.Length && !QzWriteArg(Mode, Name.Buffer, Process->ImagePathName.Buffer, Name.Length)) ||
                !QzWriteArg(Mode, (PUCHAR)Name.Buffer + Name.Length, L"", sizeof(WCHAR)))
                Status = STATUS_ACCESS_VIOLATION;
            break;
        }
        default:
            DbgPrint(QZ_KERNEL_NAME ": NtQueryInformationProcess: class %lu not supported yet\n",
                     ProcessInformationClass);
            Status = STATUS_INVALID_INFO_CLASS;
            break;
    }
    ObDereferenceObject(Process);
    return Status;
}

NTSTATUS
NTAPI
ZwQueryInformationProcess(IN HANDLE ProcessHandle, IN PROCESSINFOCLASS ProcessInformationClass,
                          OUT PVOID ProcessInformation, IN ULONG ProcessInformationLength,
                          OUT PULONG ReturnLength OPTIONAL)
{
    return QZ_AS_KERNEL(NtQueryInformationProcess(ProcessHandle, ProcessInformationClass, ProcessInformation,
                                                  ProcessInformationLength, ReturnLength));
}

NTSTATUS
NTAPI
NtSetInformationProcess(IN HANDLE ProcessHandle,
                        IN PROCESSINFOCLASS ProcessInformationClass,
                        IN PVOID ProcessInformation,
                        IN ULONG ProcessInformationLength)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PQZ_PROCESS Process;
    ULONG Value = 0;
    NTSTATUS Status;

    Status = ObReferenceObjectByHandle(ProcessHandle, PROCESS_SET_INFORMATION, PsProcessType, Mode,
                                       (PVOID *)&Process, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    if (ProcessInformationLength >= sizeof(ULONG))
        QzReadArg(Mode, &Value, ProcessInformation, sizeof(ULONG));

    switch (ProcessInformationClass)
    {
        case ProcessDefaultHardErrorMode:
            Process->HardErrorMode = Value;
            break;
        case ProcessAffinityMask:
            Status = (Value & 1) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
            break;
        case ProcessBasePriority:
            Process->BasePriority = (LONG)Value;
            break;
        case ProcessUserModeIOPL:
            Status = STATUS_PRIVILEGE_NOT_HELD;
            break;
        case ProcessExceptionPort:
        case ProcessAccessToken:
        case ProcessPriorityClass:
        case ProcessSessionInformation:
        case ProcessForegroundInformation:
        case ProcessExecuteFlags:
        case ProcessRaisePriority:
        case ProcessEnableAlignmentFaultFixup:
        case ProcessPriorityBoost:
        case ProcessQuotaLimits:
        case ProcessWx86Information:
        case ProcessDeviceMap:
        case ProcessBreakOnTermination:
            break;   /* accepted: nothing acts on these yet */
        default:
            DbgPrint(QZ_KERNEL_NAME ": NtSetInformationProcess: class %lu not supported yet\n",
                     ProcessInformationClass);
            Status = STATUS_INVALID_INFO_CLASS;
            break;
    }
    ObDereferenceObject(Process);
    return Status;
}

/* ---------------------------------------------------------------------- */
/* Threads                                                                 */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
NtQueryInformationThread(IN HANDLE ThreadHandle,
                         IN THREADINFOCLASS ThreadInformationClass,
                         OUT PVOID ThreadInformation,
                         IN ULONG ThreadInformationLength,
                         OUT PULONG ReturnLength OPTIONAL)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PQZ_PS_THREAD Thread;
    NTSTATUS Status;

    Status = ObReferenceObjectByHandle(ThreadHandle, THREAD_QUERY_INFORMATION, PsThreadType, Mode,
                                       (PVOID *)&Thread, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    switch (ThreadInformationClass)
    {
        case ThreadBasicInformation:
        {
            THREAD_BASIC_INFORMATION Info;
            RtlZeroMemory(&Info, sizeof(Info));
            Info.ExitStatus = Thread->ExitStatus;
            Info.TebBaseAddress = Thread->Teb;
            Info.ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)Thread->Process->UniqueId;
            Info.ClientId.UniqueThread = (HANDLE)(ULONG_PTR)Thread->UniqueId;
            Info.AffinityMask = 1;
            Info.Priority = 8;
            Info.BasePriority = 0;
            Status = QzPsReturn(Mode, ThreadInformation, ThreadInformationLength, ReturnLength, &Info, sizeof(Info));
            break;
        }
        case ThreadTimes:
        {
            KERNEL_USER_TIMES Times;
            RtlZeroMemory(&Times, sizeof(Times));
            Times.CreateTime = Thread->CreateTime;
            Times.ExitTime = Thread->ExitTime;
            Status = QzPsReturn(Mode, ThreadInformation, ThreadInformationLength, ReturnLength, &Times, sizeof(Times));
            break;
        }
        case ThreadAmILastThread:
        {
            ULONG Last = Thread->Process->ActiveThreads <= 1;
            Status = QzPsReturn(Mode, ThreadInformation, ThreadInformationLength, ReturnLength, &Last, sizeof(Last));
            break;
        }
        case ThreadQuerySetWin32StartAddress:
            Status = QzPsReturn(Mode, ThreadInformation, ThreadInformationLength, ReturnLength,
                                &Thread->Win32StartAddress, sizeof(PVOID));
            break;
        case ThreadIsIoPending:
        {
            ULONG Pending = 0;
            Status = QzPsReturn(Mode, ThreadInformation, ThreadInformationLength, ReturnLength, &Pending,
                                sizeof(Pending));
            break;
        }
        default:
            DbgPrint(QZ_KERNEL_NAME ": NtQueryInformationThread: class %lu not supported yet\n",
                     ThreadInformationClass);
            Status = STATUS_INVALID_INFO_CLASS;
            break;
    }
    ObDereferenceObject(Thread);
    return Status;
}

NTSTATUS
NTAPI
NtSetInformationThread(IN HANDLE ThreadHandle,
                       IN THREADINFOCLASS ThreadInformationClass,
                       IN PVOID ThreadInformation,
                       IN ULONG ThreadInformationLength)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PQZ_PS_THREAD Thread;
    NTSTATUS Status;

    Status = ObReferenceObjectByHandle(ThreadHandle, THREAD_SET_INFORMATION, PsThreadType, Mode,
                                       (PVOID *)&Thread, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    switch (ThreadInformationClass)
    {
        case ThreadQuerySetWin32StartAddress:
            if (ThreadInformationLength == sizeof(PVOID))
                QzReadArg(Mode, &Thread->Win32StartAddress, ThreadInformation, sizeof(PVOID));
            break;
        case ThreadZeroTlsCell:
        {
            /* Clear one TLS slot in every thread of the process (ours: this one) */
            ULONG Index = 0;
            QzReadArg(Mode, &Index, ThreadInformation, sizeof(ULONG));
            if (Index < 64 && Thread->Teb && Thread->Process == QzCurrentThread->ApcProcess)
            {
                PVOID Zero = NULL;
                QzCopyToUser(&((PTEB)Thread->Teb)->TlsSlots[Index], &Zero, sizeof(Zero));
            }
            break;
        }
        case ThreadPriority:
        case ThreadBasePriority:
        case ThreadAffinityMask:
        case ThreadImpersonationToken:
        case ThreadHideFromDebugger:
        case ThreadIdealProcessor:
        case ThreadPriorityBoost:
        case ThreadSetTlsArrayAddress:
        case ThreadBreakOnTermination:
            break;   /* accepted: one processor, no priorities, no impersonation yet */
        default:
            DbgPrint(QZ_KERNEL_NAME ": NtSetInformationThread: class %lu not supported yet\n",
                     ThreadInformationClass);
            Status = STATUS_INVALID_INFO_CLASS;
            break;
    }
    ObDereferenceObject(Thread);
    return Status;
}

NTSTATUS
NTAPI
ZwSetInformationThread(IN HANDLE ThreadHandle, IN THREADINFOCLASS ThreadInformationClass,
                       IN PVOID ThreadInformation, IN ULONG ThreadInformationLength)
{
    return QZ_AS_KERNEL(NtSetInformationThread(ThreadHandle, ThreadInformationClass, ThreadInformation,
                                               ThreadInformationLength));
}

/* ---------------------------------------------------------------------- */
/* Time, scheduling, identifiers, locale                                   */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
NtDelayExecution(IN BOOLEAN Alertable,
                 IN PLARGE_INTEGER DelayInterval)
{
    LARGE_INTEGER Interval;
    if (!QzReadArg(ExGetPreviousMode(), &Interval, DelayInterval, sizeof(Interval)))
        return STATUS_ACCESS_VIOLATION;
    return KeDelayExecutionThread(ExGetPreviousMode(), Alertable, &Interval);
}

NTSTATUS
NTAPI
NtYieldExecution(VOID)
{
    QzYield();
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NtQuerySystemTime(OUT PLARGE_INTEGER SystemTime)
{
    LARGE_INTEGER Now;
    KeQuerySystemTime(&Now);
    return QzWriteArg(ExGetPreviousMode(), SystemTime, &Now, sizeof(Now)) ? STATUS_SUCCESS : STATUS_ACCESS_VIOLATION;
}

NTSTATUS
NTAPI
NtQueryPerformanceCounter(OUT PLARGE_INTEGER PerformanceCounter,
                          OUT PLARGE_INTEGER PerformanceFrequency OPTIONAL)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    LARGE_INTEGER Frequency, Counter = KeQueryPerformanceCounter(&Frequency);
    if (!QzWriteArg(Mode, PerformanceCounter, &Counter, sizeof(Counter)) ||
        (PerformanceFrequency && !QzWriteArg(Mode, PerformanceFrequency, &Frequency, sizeof(Frequency))))
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NtGetTickCount(VOID)
{
    return (NTSTATUS)SharedUserData->TickCountLowDeprecated;
}

NTSTATUS
NTAPI
NtAllocateLocallyUniqueId(OUT LUID *LocallyUniqueId)
{
    static LONG Next = 0x1000;
    LUID Luid;
    Luid.LowPart = (ULONG)InterlockedIncrement(&Next);
    Luid.HighPart = 0;
    return QzWriteArg(ExGetPreviousMode(), LocallyUniqueId, &Luid, sizeof(Luid)) ? STATUS_SUCCESS
                                                                                  : STATUS_ACCESS_VIOLATION;
}

NTSTATUS
NTAPI
NtQueryDefaultLocale(IN BOOLEAN UserProfile,
                     OUT PLCID DefaultLocaleId)
{
    LCID Lcid = 0x0409;
    UNREFERENCED_PARAMETER(UserProfile);
    return QzWriteArg(ExGetPreviousMode(), DefaultLocaleId, &Lcid, sizeof(Lcid)) ? STATUS_SUCCESS
                                                                                 : STATUS_ACCESS_VIOLATION;
}

NTSTATUS
NTAPI
NtQueryDefaultUILanguage(OUT LANGID *LanguageId)
{
    LANGID Id = 0x0409;
    return QzWriteArg(ExGetPreviousMode(), LanguageId, &Id, sizeof(Id)) ? STATUS_SUCCESS : STATUS_ACCESS_VIOLATION;
}

NTSTATUS
NTAPI
NtQueryInstallUILanguage(OUT LANGID *LanguageId)
{
    return NtQueryDefaultUILanguage(LanguageId);
}

NTSTATUS
NTAPI
NtSetDefaultHardErrorPort(IN HANDLE PortHandle)
{
    UNREFERENCED_PARAMETER(PortHandle);
    return STATUS_SUCCESS;
}

/* No security tokens yet: everything runs as the system */
NTSTATUS
NTAPI
NtOpenThreadToken(IN HANDLE ThreadHandle, IN ACCESS_MASK DesiredAccess, IN BOOLEAN OpenAsSelf,
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
NtOpenThreadTokenEx(IN HANDLE ThreadHandle, IN ACCESS_MASK DesiredAccess, IN BOOLEAN OpenAsSelf,
                    IN ULONG HandleAttributes, OUT PHANDLE TokenHandle)
{
    UNREFERENCED_PARAMETER(HandleAttributes);
    return NtOpenThreadToken(ThreadHandle, DesiredAccess, OpenAsSelf, TokenHandle);
}
