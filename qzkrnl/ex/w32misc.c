/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     What else win32k asks of the kernel: push locks, critical
 *              region helpers, FPU state, status codes as Win32 errors,
 *              stack walks, the user key path
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qzio.h"
#include "qzps.h"
#include <ndk/exfuncs.h>
#include <ndk/psfuncs.h>
#include <ndk/kdtypes.h>

/* ---------------------------------------------------------------------- */
/* Push locks: the exclusive acquire's slow path (the fast path is inline) */
/* ---------------------------------------------------------------------- */

/*
 * The inline acquire sets the lock bit; when it was set already, this
 * waits for it to clear. Waiters do not queue (no "waiting" bit is set),
 * so a release never has anyone to wake.
 */
VOID
FASTCALL
ExfAcquirePushLockExclusive(IN PEX_PUSH_LOCK PushLock)
{
    LARGE_INTEGER Pause;
    Pause.QuadPart = -10000;            /* a millisecond */
    while (InterlockedBitTestAndSet((PLONG)PushLock, EX_PUSH_LOCK_LOCK_V))
        KeDelayExecutionThread(KernelMode, FALSE, &Pause);
}

VOID
FASTCALL
ExfAcquirePushLockShared(IN PEX_PUSH_LOCK PushLock)
{
    ExfAcquirePushLockExclusive(PushLock);   /* shared holders are serialized: correct, if slower */
}

VOID
FASTCALL
ExfReleasePushLock(IN PEX_PUSH_LOCK PushLock)
{
    InterlockedAnd((PLONG)PushLock, ~(LONG)EX_PUSH_LOCK_LOCK);
}

VOID
FASTCALL
ExfReleasePushLockExclusive(IN PEX_PUSH_LOCK PushLock)
{
    ExfReleasePushLock(PushLock);
}

VOID
FASTCALL
ExfReleasePushLockShared(IN PEX_PUSH_LOCK PushLock)
{
    ExfReleasePushLock(PushLock);
}

VOID
FASTCALL
ExfTryToWakePushLock(IN PEX_PUSH_LOCK PushLock)
{
    UNREFERENCED_PARAMETER(PushLock);   /* nobody queues: nobody to wake */
}

/* ---------------------------------------------------------------------- */
/* Critical region helpers                                                 */
/* ---------------------------------------------------------------------- */

VOID
FASTCALL
ExEnterCriticalRegionAndAcquireFastMutexUnsafe(IN OUT PFAST_MUTEX FastMutex)
{
    QzKeEnterCriticalRegion();
    ExAcquireFastMutexUnsafe(FastMutex);
}

VOID
FASTCALL
ExReleaseFastMutexUnsafeAndLeaveCriticalRegion(IN OUT PFAST_MUTEX FastMutex)
{
    ExReleaseFastMutexUnsafe(FastMutex);
    QzKeLeaveCriticalRegion();
}

PVOID
NTAPI
ExEnterCriticalRegionAndAcquireResourceShared(IN OUT PERESOURCE Resource)
{
    QzKeEnterCriticalRegion();
    ExAcquireResourceSharedLite(Resource, TRUE);
    return PsGetCurrentThreadWin32Thread();
}

VOID
NTAPI
ExRaiseAccessViolation(VOID)
{
    ExRaiseStatus(STATUS_ACCESS_VIOLATION);
}

/* ---------------------------------------------------------------------- */
/* Ke                                                                      */
/* ---------------------------------------------------------------------- */

BOOLEAN
NTAPI
KeIsAttachedProcess(VOID)
{
    PQZ_THREAD Thread = QzCurrentThread;
    return Thread->ApcProcess && Thread->Process && Thread->ApcProcess != Thread->Process;
}

/* Kernel stacks are not swapped out */
BOOLEAN
NTAPI
KeSetKernelStackSwapEnable(IN BOOLEAN Enable)
{
    UNREFERENCED_PARAMETER(Enable);
    return TRUE;
}

/*
 * The FPU in kernel mode: the state of whoever had it is saved into the
 * caller's buffer (an FXSAVE image, 16-aligned inside it) and put back.
 */
typedef struct _QZ_FLOATING_SAVE
{
    UCHAR Area[512 + 16];
} QZ_FLOATING_SAVE;

C_ASSERT(sizeof(KFLOATING_SAVE) >= sizeof(ULONG) * 6);

NTSTATUS
NTAPI
KeSaveFloatingPointState(OUT PKFLOATING_SAVE FloatSave)
{
    QZ_FLOATING_SAVE *Save = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Save), 'pFzQ');
    PUCHAR Area;
    if (!Save)
        return STATUS_INSUFFICIENT_RESOURCES;
    Area = (PUCHAR)(((ULONG_PTR)Save->Area + 15) & ~15UL);
    __asm__ __volatile__("fxsave (%0)\n\tfninit" : : "r"(Area) : "memory");
    *(PVOID *)FloatSave = Save;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KeRestoreFloatingPointState(IN PKFLOATING_SAVE FloatSave)
{
    QZ_FLOATING_SAVE *Save = *(PVOID *)FloatSave;
    PUCHAR Area;
    if (!Save)
        return STATUS_INVALID_PARAMETER;
    Area = (PUCHAR)(((ULONG_PTR)Save->Area + 15) & ~15UL);
    __asm__ __volatile__("fxrstor (%0)" : : "r"(Area) : "memory");
    ExFreePoolWithTag(Save, 'pFzQ');
    return STATUS_SUCCESS;
}

/* No kernel debugger to control */
NTSTATUS
NTAPI
KdSystemDebugControl(IN SYSDBG_COMMAND Command, IN PVOID InputBuffer, IN ULONG InputBufferLength,
                     OUT PVOID OutputBuffer, IN ULONG OutputBufferLength, IN OUT PULONG ReturnLength,
                     IN KPROCESSOR_MODE PreviousMode)
{
    UNREFERENCED_PARAMETER(Command);
    UNREFERENCED_PARAMETER(InputBuffer);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBuffer);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ReturnLength);
    UNREFERENCED_PARAMETER(PreviousMode);
    return STATUS_DEBUGGER_INACTIVE;
}

/* Signalled when the system shuts down (win32k's raw input thread waits on it); nothing sets it yet */
static KEVENT QzShutdownEvent;
static LONG QzShutdownEventReady;

NTSTATUS
NTAPI
PoRequestShutdownEvent(OUT PVOID *Event)
{
    if (!InterlockedExchange(&QzShutdownEventReady, 1))
        KeInitializeEvent(&QzShutdownEvent, NotificationEvent, FALSE);
    *Event = &QzShutdownEvent;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Services for kernel callers                                             */
/* ---------------------------------------------------------------------- */

NTSTATUS NTAPI NtQueryDefaultLocale(IN BOOLEAN UserProfile, OUT PLCID DefaultLocaleId);
NTSTATUS NTAPI NtYieldExecution(VOID);

NTSTATUS
NTAPI
ZwQueryDefaultLocale(IN BOOLEAN UserProfile, OUT PLCID DefaultLocaleId)
{
    return QZ_AS_KERNEL(NtQueryDefaultLocale(UserProfile, DefaultLocaleId));
}

NTSTATUS
NTAPI
ZwYieldExecution(VOID)
{
    return NtYieldExecution();
}

NTSTATUS
NTAPI
NtQuerySystemInformation(IN SYSTEM_INFORMATION_CLASS SystemInformationClass, OUT PVOID SystemInformation,
                         IN ULONG Length, OUT PULONG ResultLength OPTIONAL)
{
    return ZwQuerySystemInformation(SystemInformationClass, SystemInformation, Length, ResultLength);
}

/* Pending I/O of this thread on the file: requests here complete before returning */
NTSTATUS
NTAPI
ZwCancelIoFile(IN HANDLE FileHandle, OUT PIO_STATUS_BLOCK IoStatusBlock)
{
    UNREFERENCED_PARAMETER(FileHandle);
    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = 0;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Rtl                                                                     */
/* ---------------------------------------------------------------------- */

VOID
NTAPI
RtlGetDefaultCodePage(OUT PUSHORT AnsiCodePage, OUT PUSHORT OemCodePage)
{
    extern USHORT NlsAnsiCodePage, NlsOemCodePage;
    *AnsiCodePage = NlsAnsiCodePage;
    *OemCodePage = NlsOemCodePage;
}

/* Every process is LocalSystem: its user key is .DEFAULT */
NTSTATUS
NTAPI
RtlFormatCurrentUserKeyPath(OUT PUNICODE_STRING KeyPath)
{
    return RtlCreateUnicodeString(KeyPath, L"\\Registry\\User\\.Default") ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

/* Return addresses up the EBP chain (Flags 1: user mode, of which none here) */
ULONG
NTAPI
RtlWalkFrameChain(OUT PVOID *Callers, IN ULONG Count, IN ULONG Flags)
{
    PULONG_PTR Frame = __builtin_frame_address(0);
    PQZ_THREAD Thread = QzCurrentThread;
    ULONG_PTR Low = (ULONG_PTR)Thread->StackBase, High = Low + Thread->StackSize;
    ULONG Found = 0;

    if (Flags & 1)
        return 0;
    while (Found < Count && (ULONG_PTR)Frame >= Low && (ULONG_PTR)Frame + 2 * sizeof(PVOID) <= High)
    {
        if (!Frame[1])
            break;
        Callers[Found++] = (PVOID)Frame[1];
        if ((PULONG_PTR)Frame[0] <= Frame)
            break;
        Frame = (PULONG_PTR)Frame[0];
    }
    return Found;
}

/* The common status codes as Win32 errors (what GetLastError reports) */
static const struct { NTSTATUS Status; ULONG Error; } QzStatusErrors[] =
{
    { STATUS_SUCCESS, 0 }, { STATUS_PENDING, 997 }, { STATUS_TIMEOUT, 258 }, { STATUS_MORE_ENTRIES, 234 },
    { STATUS_BUFFER_OVERFLOW, 234 }, { STATUS_NO_MORE_FILES, 18 }, { STATUS_NO_MORE_ENTRIES, 259 },
    { STATUS_UNSUCCESSFUL, 31 }, { STATUS_NOT_IMPLEMENTED, 1 }, { STATUS_INVALID_INFO_CLASS, 87 },
    { STATUS_INFO_LENGTH_MISMATCH, 24 }, { STATUS_ACCESS_VIOLATION, 998 }, { STATUS_INVALID_HANDLE, 6 },
    { STATUS_INVALID_PARAMETER, 87 }, { STATUS_NO_SUCH_DEVICE, 2 }, { STATUS_NO_SUCH_FILE, 2 },
    { STATUS_INVALID_DEVICE_REQUEST, 1 }, { STATUS_END_OF_FILE, 38 }, { STATUS_NO_MEDIA_IN_DEVICE, 21 },
    { STATUS_NO_MEMORY, 8 }, { STATUS_CONFLICTING_ADDRESSES, 487 }, { STATUS_ACCESS_DENIED, 5 },
    { STATUS_BUFFER_TOO_SMALL, 122 }, { STATUS_OBJECT_TYPE_MISMATCH, 6 }, { STATUS_OBJECT_NAME_INVALID, 123 },
    { STATUS_OBJECT_NAME_NOT_FOUND, 2 }, { STATUS_OBJECT_NAME_COLLISION, 183 }, { STATUS_OBJECT_PATH_INVALID, 161 },
    { STATUS_OBJECT_PATH_NOT_FOUND, 3 }, { STATUS_OBJECT_PATH_SYNTAX_BAD, 161 }, { STATUS_SHARING_VIOLATION, 32 },
    { STATUS_DELETE_PENDING, 5 }, { STATUS_INSUFFICIENT_RESOURCES, 1450 }, { STATUS_DISK_FULL, 112 },
    { STATUS_NOT_SUPPORTED, 50 }, { STATUS_INVALID_SYSTEM_SERVICE, 1 }, { STATUS_NOT_FOUND, 1168 },
    { STATUS_CANCELLED, 995 }, { STATUS_INVALID_ADDRESS, 487 }, { STATUS_PROCEDURE_NOT_FOUND, 127 },
    { STATUS_DLL_NOT_FOUND, 126 }, { STATUS_INVALID_IMAGE_FORMAT, 193 }, { STATUS_PRIVILEGE_NOT_HELD, 1314 },
    { STATUS_INVALID_SECURITY_DESCR, 1338 }, { STATUS_NO_TOKEN, 1008 }, { STATUS_FILE_IS_A_DIRECTORY, 5 },
    { STATUS_NOT_A_DIRECTORY, 267 }, { STATUS_DIRECTORY_NOT_EMPTY, 145 }, { STATUS_INVALID_PAGE_PROTECTION, 87 },
    { STATUS_MEDIA_WRITE_PROTECTED, 19 }, { STATUS_DEVICE_NOT_READY, 21 }, { STATUS_IO_DEVICE_ERROR, 1117 },
    { STATUS_FILE_CLOSED, 6 }, { STATUS_NOT_MAPPED_VIEW, 487 }, { STATUS_SECTION_TOO_BIG, 87 },
    { STATUS_INVALID_DEVICE_STATE, 31 }, { STATUS_DEVICE_DOES_NOT_EXIST, 433 }, { STATUS_ILLEGAL_INSTRUCTION, 1 },
    { STATUS_INTEGER_DIVIDE_BY_ZERO, 1 }, { STATUS_STACK_OVERFLOW, 1001 }, { STATUS_INVALID_PARAMETER_1, 87 },
    { STATUS_INVALID_PARAMETER_2, 87 }, { STATUS_INVALID_PARAMETER_3, 87 }, { STATUS_INVALID_PARAMETER_4, 87 },
    { STATUS_INVALID_PARAMETER_5, 87 }, { STATUS_INVALID_PARAMETER_6, 87 }, { STATUS_INVALID_CID, 87 },
    { STATUS_PROCESS_IS_TERMINATING, 5 }, { STATUS_THREAD_IS_TERMINATING, 5 }, { STATUS_PORT_DISCONNECTED, 6 },
    { STATUS_INVALID_PORT_HANDLE, 6 }, { STATUS_ALREADY_COMMITTED, 5 }, { STATUS_MUTANT_NOT_OWNED, 288 },
    { STATUS_SEMAPHORE_LIMIT_EXCEEDED, 298 }, { STATUS_IMAGE_ALREADY_LOADED, 1056 }, { STATUS_NAME_TOO_LONG, 206 },
    { STATUS_BAD_NETWORK_PATH, 53 }, { STATUS_INVALID_LEVEL, 124 }, { STATUS_INVALID_ACL, 1336 },
    { STATUS_INVALID_SID, 1337 }, { STATUS_WAIT_1, 1 },
};

ULONG
NTAPI
RtlNtStatusToDosError(IN NTSTATUS Status)
{
    ULONG i;
    if (Status >= STATUS_WAIT_0 + 1 && Status <= STATUS_WAIT_63)
        return (ULONG)Status;           /* waits: the index */
    for (i = 0; i < RTL_NUMBER_OF(QzStatusErrors); i++)
        if (QzStatusErrors[i].Status == Status)
            return QzStatusErrors[i].Error;
    if ((Status & 0xFFFF0000) == 0x80070000 || (Status & 0xFFFF0000) == 0xC0070000)
        return Status & 0xFFFF;         /* a Win32 error carried in a status */
    return NT_SUCCESS(Status) ? 0 : 317;   /* ERROR_MR_MID_NOT_FOUND */
}

ULONG
NTAPI
RtlNtStatusToDosErrorNoTeb(IN NTSTATUS Status)
{
    return RtlNtStatusToDosError(Status);
}

/*
 * V86 mode for video BIOS calls (videoprt's VideoPortInt10, which VBE
 * miniports use): not supported. The PCI miniports used here program
 * their adapters directly.
 */
NTSTATUS
NTAPI
Ke386CallBios(IN ULONG Int, OUT PCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Context);
    DbgPrint(QZ_KERNEL_NAME ": Ke386CallBios(0x%02lx): no V86 support yet\n", Int);
    return STATUS_NOT_IMPLEMENTED;
}

/* VdmInitialize: the real-mode interrupt table and BIOS data area (videoprt's int 10h setup) */
NTSTATUS
NTAPI
NtVdmControl(IN ULONG ControlCode, IN PVOID ControlData)
{
    PHYSICAL_ADDRESS Zero;
    PVOID Low;

    if (ControlCode != VdmInitialize)
        return STATUS_NOT_IMPLEMENTED;
    Zero.QuadPart = 0;
    Low = MmMapIoSpace(Zero, 1024 + 256, MmCached);
    if (!Low)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(ControlData, Low, 1024 + 256);
    MmUnmapIoSpace(Low, 1024 + 256);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* For the network stack (ndis.sys, tcpip.sys)                             */
/* ---------------------------------------------------------------------- */

NTSTATUS NTAPI NtQueryDebugFilterState(IN ULONG ComponentId, IN ULONG Level);

NTSTATUS
NTAPI
DbgQueryDebugFilterState(IN ULONG ComponentId, IN ULONG Level)
{
    return NtQueryDebugFilterState(ComponentId, Level);
}

/* No per-object security yet: accepted, as NtSetSecurityObject is */
NTSTATUS
NTAPI
ObSetSecurityObjectByPointer(IN PVOID Object, IN SECURITY_INFORMATION SecurityInformation,
                             IN PSECURITY_DESCRIPTOR SecurityDescriptor)
{
    UNREFERENCED_PARAMETER(Object);
    UNREFERENCED_PARAMETER(SecurityInformation);
    UNREFERENCED_PARAMETER(SecurityDescriptor);
    return STATUS_SUCCESS;
}

/* The processor's idle and busy time, in clock ticks (ndis's load balancing): idle time is not counted yet */
VOID
NTAPI
ExGetCurrentProcessorCounts(OUT PULONG IdleTime, OUT PULONG KernelAndUserTime, OUT PULONG ProcessorNumber)
{
    *IdleTime = 0;
    *KernelAndUserTime = (ULONG)(QzInterruptTime / (QzTimeIncrement ? QzTimeIncrement : 1));
    *ProcessorNumber = 0;
}

VOID
NTAPI
ExGetCurrentProcessorCpuUsage(OUT PULONG CpuUsage)
{
    *CpuUsage = 0;
}

/* Spread data shared between processors over cache lines */
ULONG
NTAPI
KeGetRecommendedSharedDataAlignment(VOID)
{
    return 64;
}
