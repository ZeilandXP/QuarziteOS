/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Executive resources (ERESOURCE), callback objects, system
 *              information, driver loading by service name, debugger
 *              status, pageable-section no-ops
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qzio.h"
#include "qzps.h"

/* ---------------------------------------------------------------------- */
/* ERESOURCE: shared/exclusive locks with recursion and hand-off           */
/* ---------------------------------------------------------------------- */

#define QZ_RESOURCE_TAG     'seRQ'
#define QZ_SHARED_OWNERS    16

typedef struct _QZ_RESOURCE
{
    KSPIN_LOCK Lock;
    KSEMAPHORE SharedWake;
    KSEMAPHORE ExclusiveWake;
    PVOID ExclusiveOwner;
    ULONG ExclusiveCount;
    ULONG SharedCount;                 /* shared acquisitions held */
    ULONG SharedWaiters;
    ULONG ExclusiveWaiters;
    struct { PVOID Thread; ULONG Count; } Shared[QZ_SHARED_OWNERS];
} QZ_RESOURCE, *PQZ_RESOURCE;

#define QzResource(R) ((PQZ_RESOURCE)(R)->OwnerTable)

NTSTATUS
NTAPI
ExInitializeResourceLite(OUT PERESOURCE Resource)
{
    PQZ_RESOURCE Private = ExAllocatePoolWithTag(NonPagedPool, sizeof(QZ_RESOURCE), QZ_RESOURCE_TAG);

    RtlZeroMemory(Resource, sizeof(ERESOURCE));
    if (!Private)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Private, sizeof(*Private));
    KeInitializeSemaphore(&Private->SharedWake, 0, MAXLONG);
    KeInitializeSemaphore(&Private->ExclusiveWake, 0, MAXLONG);
    Resource->OwnerTable = (POWNER_ENTRY)Private;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
ExDeleteResourceLite(IN OUT PERESOURCE Resource)
{
    if (Resource->OwnerTable)
        ExFreePoolWithTag(Resource->OwnerTable, QZ_RESOURCE_TAG);
    Resource->OwnerTable = NULL;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
ExReinitializeResourceLite(IN OUT PERESOURCE Resource)
{
    ExDeleteResourceLite(Resource);
    return ExInitializeResourceLite(Resource);
}

static VOID
QzResourceSync(PERESOURCE Resource, PQZ_RESOURCE Private)
{
    Resource->ActiveCount = (SHORT)(Private->ExclusiveCount ? 1 : Private->SharedCount);
    Resource->Flag = Private->ExclusiveOwner ? ResourceOwnedExclusive : 0;
    Resource->OwnerEntry.OwnerThread = (ERESOURCE_THREAD)Private->ExclusiveOwner;
    Resource->NumberOfSharedWaiters = Private->SharedWaiters;
    Resource->NumberOfExclusiveWaiters = Private->ExclusiveWaiters;
}

static LONG
QzResourceFindShared(PQZ_RESOURCE Private, PVOID Thread)
{
    LONG i;
    for (i = 0; i < QZ_SHARED_OWNERS; i++)
        if (Private->Shared[i].Thread == Thread)
            return i;
    return -1;
}

BOOLEAN
NTAPI
ExAcquireResourceExclusiveLite(IN OUT PERESOURCE Resource, IN BOOLEAN Wait)
{
    PQZ_RESOURCE Private = QzResource(Resource);
    PVOID Me = KeGetCurrentThread();
    KIRQL OldIrql = QzAcquireSpinLock(&Private->Lock);

    if (Private->ExclusiveOwner == Me)
    {
        Private->ExclusiveCount++;
    }
    else if (!Private->ExclusiveOwner && !Private->SharedCount)
    {
        Private->ExclusiveOwner = Me;
        Private->ExclusiveCount = 1;
    }
    else if (!Wait)
    {
        QzReleaseSpinLock(&Private->Lock, OldIrql);
        return FALSE;
    }
    else
    {
        /* The releaser hands the resource over before waking us */
        Private->ExclusiveWaiters++;
        Resource->ContentionCount++;
        QzResourceSync(Resource, Private);
        QzReleaseSpinLock(&Private->Lock, OldIrql);
        KeWaitForSingleObject(&Private->ExclusiveWake, WrResource, KernelMode, FALSE, NULL);
        OldIrql = QzAcquireSpinLock(&Private->Lock);
        Private->ExclusiveOwner = Me;
    }
    QzResourceSync(Resource, Private);
    QzReleaseSpinLock(&Private->Lock, OldIrql);
    return TRUE;
}

static BOOLEAN
QzAcquireShared(PERESOURCE Resource, BOOLEAN Wait, BOOLEAN StarveExclusive)
{
    PQZ_RESOURCE Private = QzResource(Resource);
    PVOID Me = KeGetCurrentThread();
    KIRQL OldIrql = QzAcquireSpinLock(&Private->Lock);
    LONG Slot;

    /* An exclusive owner may also acquire shared (recursively) */
    if (Private->ExclusiveOwner == Me)
    {
        Private->ExclusiveCount++;
        QzReleaseSpinLock(&Private->Lock, OldIrql);
        return TRUE;
    }

    Slot = QzResourceFindShared(Private, Me);
    if (!Private->ExclusiveOwner &&
        (Slot >= 0 || StarveExclusive || !Private->ExclusiveWaiters))
    {
        /* Granted (a thread already sharing always gets in, avoiding a
           deadlock against waiting exclusive requests) */
    }
    else if (!Wait)
    {
        QzReleaseSpinLock(&Private->Lock, OldIrql);
        return FALSE;
    }
    else
    {
        Private->SharedWaiters++;
        Resource->ContentionCount++;
        QzResourceSync(Resource, Private);
        QzReleaseSpinLock(&Private->Lock, OldIrql);
        KeWaitForSingleObject(&Private->SharedWake, WrResource, KernelMode, FALSE, NULL);
        OldIrql = QzAcquireSpinLock(&Private->Lock);
        Private->SharedCount--;   /* the releaser counted us in already */
    }

    Private->SharedCount++;
    if (Slot < 0)
        Slot = QzResourceFindShared(Private, NULL);
    if (Slot >= 0)
    {
        Private->Shared[Slot].Thread = Me;
        Private->Shared[Slot].Count++;
    }
    QzResourceSync(Resource, Private);
    QzReleaseSpinLock(&Private->Lock, OldIrql);
    return TRUE;
}

BOOLEAN
NTAPI
ExAcquireResourceSharedLite(IN OUT PERESOURCE Resource, IN BOOLEAN Wait)
{
    return QzAcquireShared(Resource, Wait, FALSE);
}

BOOLEAN
NTAPI
ExAcquireSharedStarveExclusive(IN OUT PERESOURCE Resource, IN BOOLEAN Wait)
{
    return QzAcquireShared(Resource, Wait, TRUE);
}

BOOLEAN
NTAPI
ExAcquireSharedWaitForExclusive(IN OUT PERESOURCE Resource, IN BOOLEAN Wait)
{
    return QzAcquireShared(Resource, Wait, FALSE);
}

static VOID
QzResourceRelease(PERESOURCE Resource, PVOID Thread)
{
    PQZ_RESOURCE Private = QzResource(Resource);
    KIRQL OldIrql = QzAcquireSpinLock(&Private->Lock);

    if (Private->ExclusiveOwner && Private->ExclusiveOwner == Thread)
    {
        if (--Private->ExclusiveCount)
            goto Done;
        Private->ExclusiveOwner = NULL;
    }
    else
    {
        LONG Slot = QzResourceFindShared(Private, Thread);
        if (!Private->SharedCount)
            KeBugCheckEx(RESOURCE_NOT_OWNED, (ULONG_PTR)Resource, (ULONG_PTR)Thread, 0, 0);
        Private->SharedCount--;
        if (Slot >= 0 && !--Private->Shared[Slot].Count)
            Private->Shared[Slot].Thread = NULL;
        if (Private->SharedCount)
            goto Done;
    }

    /* Now free: an exclusive waiter first, else every shared waiter */
    if (Private->ExclusiveWaiters)
    {
        Private->ExclusiveWaiters--;
        Private->ExclusiveOwner = (PVOID)-1;   /* reserved; the waiter claims it */
        Private->ExclusiveCount = 1;
        KeReleaseSemaphore(&Private->ExclusiveWake, IO_NO_INCREMENT, 1, FALSE);
    }
    else if (Private->SharedWaiters)
    {
        ULONG Count = Private->SharedWaiters;
        Private->SharedWaiters = 0;
        Private->SharedCount += Count;
        KeReleaseSemaphore(&Private->SharedWake, IO_NO_INCREMENT, Count, FALSE);
    }

Done:
    QzResourceSync(Resource, Private);
    QzReleaseSpinLock(&Private->Lock, OldIrql);
}

VOID
FASTCALL
ExReleaseResourceLite(IN OUT PERESOURCE Resource)
{
    QzResourceRelease(Resource, KeGetCurrentThread());
}

VOID
NTAPI
ExReleaseResourceForThreadLite(IN OUT PERESOURCE Resource, IN ERESOURCE_THREAD ResourceThreadId)
{
    QzResourceRelease(Resource, (PVOID)ResourceThreadId);
}

VOID
NTAPI
ExConvertExclusiveToSharedLite(IN OUT PERESOURCE Resource)
{
    PQZ_RESOURCE Private = QzResource(Resource);
    PVOID Me = KeGetCurrentThread();
    KIRQL OldIrql = QzAcquireSpinLock(&Private->Lock);
    LONG Slot = QzResourceFindShared(Private, NULL);

    Private->SharedCount = Private->ExclusiveCount;
    if (Slot >= 0)
    {
        Private->Shared[Slot].Thread = Me;
        Private->Shared[Slot].Count = Private->ExclusiveCount;
    }
    Private->ExclusiveOwner = NULL;
    Private->ExclusiveCount = 0;
    if (Private->SharedWaiters)
    {
        ULONG Count = Private->SharedWaiters;
        Private->SharedWaiters = 0;
        Private->SharedCount += Count;
        KeReleaseSemaphore(&Private->SharedWake, IO_NO_INCREMENT, Count, FALSE);
    }
    QzResourceSync(Resource, Private);
    QzReleaseSpinLock(&Private->Lock, OldIrql);
}

BOOLEAN
NTAPI
ExIsResourceAcquiredExclusiveLite(IN PERESOURCE Resource)
{
    return QzResource(Resource)->ExclusiveOwner == KeGetCurrentThread();
}

ULONG
NTAPI
ExIsResourceAcquiredSharedLite(IN PERESOURCE Resource)
{
    PQZ_RESOURCE Private = QzResource(Resource);
    LONG Slot;

    if (Private->ExclusiveOwner == KeGetCurrentThread())
        return Private->ExclusiveCount;
    Slot = QzResourceFindShared(Private, KeGetCurrentThread());
    return (Slot >= 0) ? Private->Shared[Slot].Count : 0;
}

ULONG
NTAPI
ExGetExclusiveWaiterCount(IN PERESOURCE Resource)
{
    return QzResource(Resource)->ExclusiveWaiters;
}

ULONG
NTAPI
ExGetSharedWaiterCount(IN PERESOURCE Resource)
{
    return QzResource(Resource)->SharedWaiters;
}

PVOID
NTAPI
ExEnterCriticalRegionAndAcquireResourceExclusive(IN PERESOURCE Resource)
{
    QzKeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(Resource, TRUE);
    return KeGetCurrentThread();
}

VOID
FASTCALL
ExReleaseResourceAndLeaveCriticalRegion(IN PERESOURCE Resource)
{
    ExReleaseResourceLite(Resource);
    QzKeLeaveCriticalRegion();
}

/* ---------------------------------------------------------------------- */
/* Callback objects (\Callback\...)                                         */
/* ---------------------------------------------------------------------- */

typedef struct _QZ_CALLBACK_REGISTRATION
{
    LIST_ENTRY Link;
    struct _CALLBACK_OBJECT *Owner;
    PCALLBACK_FUNCTION Function;
    PVOID Context;
} QZ_CALLBACK_REGISTRATION, *PQZ_CALLBACK_REGISTRATION;

/* The NDK's CALLBACK_OBJECT layout */
typedef CALLBACK_OBJECT QZ_CALLBACK_OBJECT, *PQZ_CALLBACK_OBJECT;

POBJECT_TYPE ExCallbackObjectType;

static VOID
QzExCallbackInit(VOID)
{
    OBJECT_TYPE_INITIALIZER Init;
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"Callback");

    RtlZeroMemory(&Init, sizeof(Init));
    Init.Length = sizeof(Init);
    Init.GenericMapping.GenericRead = STANDARD_RIGHTS_READ;
    Init.GenericMapping.GenericWrite = STANDARD_RIGHTS_WRITE;
    Init.GenericMapping.GenericExecute = STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE;
    Init.GenericMapping.GenericAll = CALLBACK_ALL_ACCESS;
    Init.ValidAccessMask = CALLBACK_ALL_ACCESS;
    Init.PoolType = NonPagedPool;
    Init.UseDefaultObject = TRUE;
    Init.CaseInsensitive = TRUE;
    ObCreateObjectType(&Name, &Init, NULL, &ExCallbackObjectType);
}

NTSTATUS
NTAPI
ExCreateCallback(OUT PCALLBACK_OBJECT *CallbackObject,
                 IN POBJECT_ATTRIBUTES ObjectAttributes,
                 IN BOOLEAN Create,
                 IN BOOLEAN AllowMultipleCallbacks)
{
    PQZ_CALLBACK_OBJECT Callback = NULL;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    HANDLE Handle;

    if (!ExCallbackObjectType)
        QzExCallbackInit();

    if (ObjectAttributes->ObjectName)
    {
        Status = ObOpenObjectByName(ObjectAttributes, ExCallbackObjectType, KernelMode, NULL, 0,
                                    NULL, &Handle);
        if (NT_SUCCESS(Status))
        {
            Status = ObReferenceObjectByHandle(Handle, 0, ExCallbackObjectType, KernelMode,
                                               (PVOID *)&Callback, NULL);
            ZwClose(Handle);
        }
    }

    if (!NT_SUCCESS(Status) && Create)
    {
        Status = ObCreateObject(KernelMode, ExCallbackObjectType, ObjectAttributes, KernelMode, NULL,
                                sizeof(QZ_CALLBACK_OBJECT), 0, 0, (PVOID *)&Callback);
        if (NT_SUCCESS(Status))
        {
            Callback->Signature = 'llaC';
            KeInitializeSpinLock(&Callback->Lock);
            InitializeListHead(&Callback->RegisteredCallbacks);
            Callback->AllowMultipleCallbacks = AllowMultipleCallbacks;
            /* ObInsertObject keeps our creation reference for the caller */
            Status = ObInsertObject(Callback, NULL, 0, 0, (PVOID *)&Callback, NULL);
        }
    }

    if (NT_SUCCESS(Status))
        *CallbackObject = (PCALLBACK_OBJECT)Callback;
    return Status;
}

PVOID
NTAPI
ExRegisterCallback(IN PCALLBACK_OBJECT CallbackObject,
                   IN PCALLBACK_FUNCTION CallbackFunction,
                   IN PVOID CallbackContext OPTIONAL)
{
    PQZ_CALLBACK_OBJECT Callback = (PQZ_CALLBACK_OBJECT)CallbackObject;
    PQZ_CALLBACK_REGISTRATION Registration;
    KIRQL OldIrql;

    Registration = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Registration), 'lbCQ');
    if (!Registration)
        return NULL;
    Registration->Owner = Callback;
    Registration->Function = CallbackFunction;
    Registration->Context = CallbackContext;
    OldIrql = QzAcquireSpinLock(&Callback->Lock);
    if (!Callback->AllowMultipleCallbacks && !IsListEmpty(&Callback->RegisteredCallbacks))
    {
        QzReleaseSpinLock(&Callback->Lock, OldIrql);
        ExFreePoolWithTag(Registration, 'lbCQ');
        return NULL;
    }
    InsertTailList(&Callback->RegisteredCallbacks, &Registration->Link);
    QzReleaseSpinLock(&Callback->Lock, OldIrql);
    ObReferenceObject(Callback);
    return Registration;
}

VOID
NTAPI
ExUnregisterCallback(IN PVOID CallbackRegistration)
{
    PQZ_CALLBACK_REGISTRATION Registration = CallbackRegistration;
    PQZ_CALLBACK_OBJECT Callback = Registration->Owner;
    KIRQL OldIrql = QzAcquireSpinLock(&Callback->Lock);

    RemoveEntryList(&Registration->Link);
    QzReleaseSpinLock(&Callback->Lock, OldIrql);
    ObDereferenceObject(Callback);
    ExFreePoolWithTag(Registration, 'lbCQ');
}

VOID
NTAPI
ExNotifyCallback(IN PCALLBACK_OBJECT CallbackObject,
                 IN PVOID Argument1 OPTIONAL,
                 IN PVOID Argument2 OPTIONAL)
{
    PQZ_CALLBACK_OBJECT Callback = CallbackObject;
    PLIST_ENTRY Entry;

    for (Entry = Callback->RegisteredCallbacks.Flink; Entry != &Callback->RegisteredCallbacks; Entry = Entry->Flink)
    {
        PQZ_CALLBACK_REGISTRATION Registration = CONTAINING_RECORD(Entry, QZ_CALLBACK_REGISTRATION, Link);
        Registration->Function(Registration->Context, Argument1, Argument2);
    }
}

/* ---------------------------------------------------------------------- */
/* System information                                                      */
/* ---------------------------------------------------------------------- */

extern BOOLEAN QzSysenterAvailable;         /* ke/i386/cpu.c */

/* Family, model/stepping and KF_* bits of the boot processor, from CPUID */
static VOID
QzExProcessorIdentity(PUSHORT Level, PUSHORT Revision, PULONG FeatureBits)
{
    ULONG Eax, Ebx, Ecx, Edx, Family, Model, Bits = 0;

    __asm__ __volatile__("cpuid" : "=a"(Eax), "=b"(Ebx), "=c"(Ecx), "=d"(Edx) : "a"(1), "c"(0));
    Family = (Eax >> 8) & 0xF;
    Model = (Eax >> 4) & 0xF;
    if (Family == 0xF)
        Family += (Eax >> 20) & 0xFF;
    if (Family == 0x6 || Family >= 0xF)
        Model |= ((Eax >> 16) & 0xF) << 4;
    *Level = (USHORT)Family;
    *Revision = (USHORT)((Model << 8) | (Eax & 0xF));

    if (Edx & (1 << 4))  Bits |= KF_RDTSC;
    if (Edx & (1 << 3))  Bits |= KF_LARGE_PAGE;
    if (Edx & (1 << 13)) Bits |= KF_GLOBAL_PAGE;
    if (Edx & (1 << 15)) Bits |= KF_CMOV;
    if (Edx & (1 << 12)) Bits |= KF_MTRR;
    if (Edx & (1 << 8))  Bits |= KF_CMPXCHG8B;
    if (Edx & (1 << 23)) Bits |= KF_MMX;
    if (Edx & (1 << 16)) Bits |= KF_PAT;
    if (Edx & (1 << 24)) Bits |= KF_FXSR;
    if (Edx & (1 << 25)) Bits |= KF_XMMI;
    if (Edx & (1 << 26)) Bits |= KF_XMMI64;
    if (QzSysenterAvailable)
        Bits |= KF_FAST_SYSCALL;
    *FeatureBits = Bits | KF_CR4 | KF_WORKING_PTE;
}

NTSTATUS
NTAPI
ZwQuerySystemInformation(IN SYSTEM_INFORMATION_CLASS SystemInformationClass,
                         OUT PVOID SystemInformation,
                         IN ULONG Length,
                         OUT PULONG ResultLength OPTIONAL)
{
    ULONG Needed = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    switch (SystemInformationClass)
    {
        case SystemBasicInformation:
        {
            PSYSTEM_BASIC_INFORMATION Info = SystemInformation;
            ULONG Total, Free;
            Needed = sizeof(*Info);
            if (Length < Needed)
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            QzMmGetStats(&Total, &Free);
            RtlZeroMemory(Info, sizeof(*Info));
            Info->TimerResolution = KeQueryTimeIncrement();
            Info->PageSize = PAGE_SIZE;
            Info->NumberOfPhysicalPages = Total;
            Info->LowestPhysicalPageNumber = 1;
            Info->HighestPhysicalPageNumber = Total;
            Info->AllocationGranularity = 64 * 1024;
            Info->MinimumUserModeAddress = 0x10000;
            Info->MaximumUserModeAddress = 0x7FFEFFFF;
            Info->ActiveProcessorsAffinityMask = 1;
            Info->NumberOfProcessors = 1;
            break;
        }

        case SystemModuleInformation:
        {
            PRTL_PROCESS_MODULES Modules = SystemInformation;
            PLIST_ENTRY Entry;
            ULONG Count = 0, i = 0;

            for (Entry = QzLoaderBlock->LoadOrderListHead.Flink;
                 Entry != &QzLoaderBlock->LoadOrderListHead; Entry = Entry->Flink)
                Count++;
            Needed = FIELD_OFFSET(RTL_PROCESS_MODULES, Modules) + Count * sizeof(RTL_PROCESS_MODULE_INFORMATION);
            if (Length < Needed)
            {
                if (Length >= sizeof(ULONG))
                    Modules->NumberOfModules = Count;
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            Modules->NumberOfModules = Count;
            for (Entry = QzLoaderBlock->LoadOrderListHead.Flink;
                 Entry != &QzLoaderBlock->LoadOrderListHead; Entry = Entry->Flink, i++)
            {
                PQZ_LDR_ENTRY Module = CONTAINING_RECORD(Entry, QZ_LDR_ENTRY, InLoadOrderLinks);
                PRTL_PROCESS_MODULE_INFORMATION Out = &Modules->Modules[i];
                ULONG k, Chars;

                RtlZeroMemory(Out, sizeof(*Out));
                Out->ImageBase = Module->DllBase;
                Out->ImageSize = Module->SizeOfImage;
                Out->LoadOrderIndex = (USHORT)i;
                Out->LoadCount = 1;
                Chars = min(Module->FullDllName.Length / sizeof(WCHAR), sizeof(Out->FullPathName) - 1);
                for (k = 0; k < Chars; k++)
                {
                    Out->FullPathName[k] = (UCHAR)Module->FullDllName.Buffer[k];
                    if (Out->FullPathName[k] == '\\')
                        Out->OffsetToFileName = (USHORT)(k + 1);
                }
            }
            break;
        }

        case SystemProcessorInformation:
        {
            PSYSTEM_PROCESSOR_INFORMATION Info = SystemInformation;
            Needed = sizeof(*Info);
            if (Length < Needed)
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            RtlZeroMemory(Info, sizeof(*Info));
            Info->ProcessorArchitecture = PROCESSOR_ARCHITECTURE_INTEL;
            QzExProcessorIdentity(&Info->ProcessorLevel, &Info->ProcessorRevision, &Info->ProcessorFeatureBits);
            break;
        }

        case SystemPerformanceInformation:
        {
            PSYSTEM_PERFORMANCE_INFORMATION Info = SystemInformation;
            ULONG Total, Free;
            Needed = sizeof(*Info);
            if (Length < Needed)
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            QzMmGetStats(&Total, &Free);
            RtlZeroMemory(Info, sizeof(*Info));
            Info->AvailablePages = Free;
            Info->CommittedPages = Total - Free;
            Info->CommitLimit = Total;
            Info->PeakCommitment = Total - Free;
            break;
        }

        case SystemProcessorPerformanceInformation:
        {
            PSYSTEM_PROCESSOR_PERFORMANCE_INFORMATION Info = SystemInformation;
            Needed = sizeof(*Info);
            if (Length < Needed)
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            RtlZeroMemory(Info, sizeof(*Info));
            KeQueryTickCount(&Info->KernelTime);
            Info->KernelTime.QuadPart *= KeQueryTimeIncrement();
            break;
        }

        case SystemFlagsInformation:
        {
            PSYSTEM_FLAGS_INFORMATION Info = SystemInformation;
            Needed = sizeof(*Info);
            if (Length < Needed)
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            Info->Flags = 0;                    /* NtGlobalFlag */
            break;
        }

        case SystemRangeStartInformation:
            Needed = sizeof(ULONG_PTR);
            if (Length < Needed)
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            *(PULONG_PTR)SystemInformation = (ULONG_PTR)MmSystemRangeStart;
            break;

        case SystemKernelDebuggerInformation:
        {
            PSYSTEM_KERNEL_DEBUGGER_INFORMATION Info = SystemInformation;
            Needed = sizeof(*Info);
            if (Length < Needed)
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            Info->KernelDebuggerEnabled = FALSE;
            Info->KernelDebuggerNotPresent = TRUE;
            break;
        }

        case SystemTimeOfDayInformation:
        {
            PSYSTEM_TIMEOFDAY_INFORMATION Info = SystemInformation;
            Needed = sizeof(*Info);
            if (Length < Needed)
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            RtlZeroMemory(Info, sizeof(*Info));
            KeQuerySystemTime(&Info->CurrentTime);
            break;
        }

        default:
            Status = STATUS_INVALID_INFO_CLASS;
            break;
    }

    if (ResultLength)
        *ResultLength = Needed;
    return Status;
}

/*
 * win32k.sys: loaded like a driver, but it is no driver: DriverEntry gets
 * a driver object of its own that the I/O manager does not know, and it
 * extends the system service table (KeAddSystemServiceTable).
 */
static NTSTATUS
QzExLoadServiceTableImage(PCUNICODE_STRING Path)
{
    static DRIVER_OBJECT Win32k;
    PQZ_LDR_ENTRY Module;
    PCSTR FirstMissing = NULL;
    ULONG Missing;
    NTSTATUS Status;

    Status = QzMmLoadSystemImage(Path, (PVOID *)&Module);
    if (Status == STATUS_IMAGE_ALREADY_LOADED)
    {
        QzMmUnloadSystemImage(Module);          /* drop the reference just taken */
        return Status;
    }
    if (!NT_SUCCESS(Status))
        return Status;

    Missing = QzIoCheckImports(Module->DllBase, &FirstMissing, 0);
    if (Missing)
    {
        DbgPrint(QZ_KERNEL_NAME ": %wZ is loaded but not started: it imports %lu export(s) qzkrnl does not "
                 "implement yet (%s, ...)\n", &Module->BaseDllName, Missing, FirstMissing);
        QzMmUnloadSystemImage(Module);
        return STATUS_PROCEDURE_NOT_FOUND;
    }
    Status = QzIoInitializeImports(Module->DllBase, 0);
    if (NT_SUCCESS(Status) && Module->EntryPoint)
    {
        RtlZeroMemory(&Win32k, sizeof(Win32k));
        Win32k.Type = IO_TYPE_DRIVER;
        Win32k.Size = sizeof(Win32k);
        Win32k.DriverStart = Module->DllBase;
        Win32k.DriverSize = Module->SizeOfImage;
        Win32k.DriverSection = Module;
        Win32k.DriverInit = (PDRIVER_INITIALIZE)Module->EntryPoint;
        DbgPrint(QZ_KERNEL_NAME ": DriverEntry %wZ\n", &Module->BaseDllName);
        Status = Win32k.DriverInit(&Win32k, NULL);
        DbgPrint(QZ_KERNEL_NAME ": DriverEntry %wZ -> 0x%08lx\n", &Module->BaseDllName, Status);
    }
    if (!NT_SUCCESS(Status))
        QzMmUnloadSystemImage(Module);
    return Status;
}

/*
 * Setting system information. Sessions: there is one session space (the
 * system's), so a new session only gets the next number.
 */
NTSTATUS
NTAPI
NtSetSystemInformation(IN SYSTEM_INFORMATION_CLASS SystemInformationClass,
                       IN PVOID SystemInformation,
                       IN ULONG Length)
{
    static LONG NextSession = -1;
    KPROCESSOR_MODE Mode = ExGetPreviousMode();

    switch (SystemInformationClass)
    {
        case SystemSessionCreate:
        {
            ULONG Id;
            if (Length != sizeof(ULONG))
                return STATUS_INFO_LENGTH_MISMATCH;
            Id = (ULONG)InterlockedIncrement(&NextSession);
            return QzWriteArg(Mode, SystemInformation, &Id, sizeof(Id)) ? STATUS_SUCCESS : STATUS_ACCESS_VIOLATION;
        }

        case SystemSessionDetach:
            return Length == sizeof(ULONG) ? STATUS_SUCCESS : STATUS_INFO_LENGTH_MISMATCH;

        case SystemExtendServiceTableInformation:
        {
            WCHAR Buffer[128];
            UNICODE_STRING Name;
            if (Length != sizeof(UNICODE_STRING))
                return STATUS_INFO_LENGTH_MISMATCH;
            if (!QzReadArg(Mode, &Name, SystemInformation, sizeof(Name)) || Name.Length > sizeof(Buffer) ||
                !QzReadArg(Mode, Buffer, Name.Buffer, Name.Length))
                return STATUS_ACCESS_VIOLATION;
            Name.Buffer = Buffer;
            Name.MaximumLength = Name.Length;
            return QzExLoadServiceTableImage(&Name);
        }

        case SystemFlagsInformation:
        case SystemTimeAdjustmentInformation:
        case SystemFileCacheInformation:
        case SystemDpcBehaviorInformation:
        case SystemRegistryQuotaInformation:
            return STATUS_SUCCESS;              /* noted, not acted on */

        default:
            return STATUS_INVALID_INFO_CLASS;
    }
}

NTSTATUS
NTAPI
ZwSetSystemInformation(IN SYSTEM_INFORMATION_CLASS SystemInformationClass,
                       IN PVOID SystemInformation,
                       IN ULONG Length)
{
    return QZ_AS_KERNEL(NtSetSystemInformation(SystemInformationClass, SystemInformation, Length));
}

/* ---------------------------------------------------------------------- */
/* Loading drivers by service key                                         */
/* ---------------------------------------------------------------------- */

/* "\Registry\Machine\System\CurrentControlSet\Services\Name" -> "Name" */
static VOID
QzExServiceName(PCUNICODE_STRING Path, PUNICODE_STRING Service)
{
    USHORT i = Path->Length / sizeof(WCHAR);

    *Service = *Path;
    while (i && Service->Buffer[i - 1] != L'\\')
        i--;
    Service->Buffer += i;
    Service->Length -= i * sizeof(WCHAR);
    Service->MaximumLength = Service->Length;
}

NTSTATUS
NTAPI
ZwLoadDriver(IN PUNICODE_STRING DriverServiceName)
{
    UNICODE_STRING Service;
    PDRIVER_OBJECT Driver;

    QzExServiceName(DriverServiceName, &Service);
    Driver = QzIoFindDriver(&Service);
    if (Driver)
    {
        if (NT_SUCCESS(QzDriverExtension(Driver)->EntryStatus) || !QzDriverExtension(Driver)->EntryCalled)
            return STATUS_IMAGE_ALREADY_LOADED;
        QzIoUnloadDriver(&Service);           /* it failed before: try again */
    }
    return QzIoLoadDriver(&Service, &Driver);
}

NTSTATUS
NTAPI
ZwUnloadDriver(IN PUNICODE_STRING DriverServiceName)
{
    UNICODE_STRING Service;
    QzExServiceName(DriverServiceName, &Service);
    return QzIoUnloadDriver(&Service);
}

/* The services from user mode: the name is copied; every caller holds SeLoadDriverPrivilege */
static NTSTATUS
QzExCaptureServiceKey(PUNICODE_STRING User, PUNICODE_STRING Captured, PWCHAR Buffer, ULONG Size)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    UNICODE_STRING Name;

    if (!QzReadArg(Mode, &Name, User, sizeof(Name)))
        return STATUS_ACCESS_VIOLATION;
    if (!Name.Length || Name.Length > Size || (Name.Length & 1))
        return STATUS_INVALID_PARAMETER;
    if (!QzReadArg(Mode, Buffer, Name.Buffer, Name.Length))
        return STATUS_ACCESS_VIOLATION;
    Captured->Buffer = Buffer;
    Captured->Length = Captured->MaximumLength = Name.Length;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NtLoadDriver(IN PUNICODE_STRING DriverServiceName)
{
    WCHAR Buffer[256];
    UNICODE_STRING Name;
    NTSTATUS Status = QzExCaptureServiceKey(DriverServiceName, &Name, Buffer, sizeof(Buffer));
    return NT_SUCCESS(Status) ? QZ_AS_KERNEL(ZwLoadDriver(&Name)) : Status;
}

NTSTATUS
NTAPI
NtUnloadDriver(IN PUNICODE_STRING DriverServiceName)
{
    WCHAR Buffer[256];
    UNICODE_STRING Name;
    NTSTATUS Status = QzExCaptureServiceKey(DriverServiceName, &Name, Buffer, sizeof(Buffer));
    return NT_SUCCESS(Status) ? QZ_AS_KERNEL(ZwUnloadDriver(&Name)) : Status;
}

/* ---------------------------------------------------------------------- */
/* Debugger status: no kernel debugger is attached to qzkrnl                */
/* ---------------------------------------------------------------------- */

/* KdDebuggerEnabled / KdDebuggerNotPresent live in ke/halif.c */

BOOLEAN
NTAPI
KdRefreshDebuggerNotPresent(VOID)
{
    return TRUE;
}

VOID
NTAPI
DbgBreakPoint(VOID)
{
    DbgPrint(QZ_KERNEL_NAME ": DbgBreakPoint from %p (no debugger, continuing)\n",
             __builtin_return_address(0));
}

VOID
NTAPI
DbgBreakPointWithStatus(IN ULONG Status)
{
    DbgPrint(QZ_KERNEL_NAME ": DbgBreakPointWithStatus(0x%08lx) from %p (no debugger, continuing)\n",
             Status, __builtin_return_address(0));
}

/* ---------------------------------------------------------------------- */
/* Paging controls: everything is resident                                 */
/* ---------------------------------------------------------------------- */

LOGICAL
NTAPI
MmIsDriverVerifying(IN PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    return FALSE;
}

PVOID
NTAPI
MmLockPageableDataSection(IN PVOID AddressWithinSection)
{
    return AddressWithinSection;
}

VOID
NTAPI
MmLockPageableSectionByHandle(IN PVOID ImageSectionHandle)
{
    UNREFERENCED_PARAMETER(ImageSectionHandle);
}

VOID
NTAPI
MmUnlockPageableImageSection(IN PVOID ImageSectionHandle)
{
    UNREFERENCED_PARAMETER(ImageSectionHandle);
}

PVOID
NTAPI
MmPageEntireDriver(IN PVOID AddressWithinSection)
{
    return AddressWithinSection;
}

VOID
NTAPI
MmResetDriverPaging(IN PVOID AddressWithinSection)
{
    UNREFERENCED_PARAMETER(AddressWithinSection);
}

SIZE_T
NTAPI
MmSizeOfMdl(IN PVOID Base, IN SIZE_T Length)
{
    return sizeof(MDL) + ADDRESS_AND_SIZE_TO_SPAN_PAGES(Base, Length) * sizeof(PFN_NUMBER);
}

/* ---------------------------------------------------------------------- */
/* Small odds and ends                                                     */
/* ---------------------------------------------------------------------- */

PVOID
NTAPI
ExAllocatePoolWithQuotaTag(IN POOL_TYPE PoolType, IN SIZE_T NumberOfBytes, IN ULONG Tag)
{
    /* No quotas yet; the flag only changes failure behaviour */
    return ExAllocatePoolWithTag(PoolType & ~POOL_QUOTA_FAIL_INSTEAD_OF_RAISE, NumberOfBytes, Tag);
}

PVOID
NTAPI
ExAllocatePoolWithQuota(IN POOL_TYPE PoolType, IN SIZE_T NumberOfBytes)
{
    return ExAllocatePoolWithQuotaTag(PoolType, NumberOfBytes, 'enoN');
}

PVOID
NTAPI
RtlImageDirectoryEntryToData(IN PVOID BaseAddress, IN BOOLEAN MappedAsImage,
                             IN USHORT Directory, OUT PULONG Size)
{
    PIMAGE_NT_HEADERS Nt = (PIMAGE_NT_HEADERS)((PUCHAR)BaseAddress +
                                               ((PIMAGE_DOS_HEADER)BaseAddress)->e_lfanew);
    UNREFERENCED_PARAMETER(MappedAsImage);   /* loaded images are always mapped */

    *Size = 0;
    if (((PIMAGE_DOS_HEADER)BaseAddress)->e_magic != IMAGE_DOS_SIGNATURE ||
        Nt->Signature != IMAGE_NT_SIGNATURE ||
        Directory >= Nt->OptionalHeader.NumberOfRvaAndSizes ||
        !Nt->OptionalHeader.DataDirectory[Directory].VirtualAddress)
        return NULL;
    *Size = Nt->OptionalHeader.DataDirectory[Directory].Size;
    return (PUCHAR)BaseAddress + Nt->OptionalHeader.DataDirectory[Directory].VirtualAddress;
}

PIMAGE_NT_HEADERS
NTAPI
RtlImageNtHeader(IN PVOID Base)
{
    PIMAGE_DOS_HEADER Dos = Base;
    PIMAGE_NT_HEADERS Nt;

    if (!Base || Dos->e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;
    Nt = (PIMAGE_NT_HEADERS)((PUCHAR)Base + Dos->e_lfanew);
    return (Nt->Signature == IMAGE_NT_SIGNATURE) ? Nt : NULL;
}

NTSTATUS
NTAPI
IoSetDevicePropertyData(IN PDEVICE_OBJECT Pdo, IN CONST DEVPROPKEY *PropertyKey, IN LCID Lcid,
                        IN ULONG Flags, IN DEVPROPTYPE Type, IN ULONG Size, IN PVOID Data OPTIONAL)
{
    /* Unified device properties need the writable registry (B5) */
    UNREFERENCED_PARAMETER(Pdo);
    UNREFERENCED_PARAMETER(PropertyKey);
    UNREFERENCED_PARAMETER(Lcid);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(Type);
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(Data);
    return STATUS_SUCCESS;
}

BOOLEAN
NTAPI
PsIsThreadTerminating(IN PETHREAD Thread)
{
    UNREFERENCED_PARAMETER(Thread);
    return FALSE;
}

NTSTATUS
NTAPI
ZwPowerInformation(IN POWER_INFORMATION_LEVEL PowerInformationLevel,
                   IN PVOID InputBuffer OPTIONAL, IN ULONG InputBufferLength,
                   OUT PVOID OutputBuffer OPTIONAL, IN ULONG OutputBufferLength)
{
    UNREFERENCED_PARAMETER(PowerInformationLevel);
    UNREFERENCED_PARAMETER(InputBuffer);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBuffer);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    return STATUS_NOT_SUPPORTED;
}
