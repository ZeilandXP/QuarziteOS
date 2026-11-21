/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private kernel header
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * qzkrnl is a from-scratch NT-style kernel. It speaks the NT boot protocol
 * (FreeLoader hands it a LOADER_PARAMETER_BLOCK exactly like it does for
 * ntoskrnl.exe) and reuses the public SDK headers for binary-compatible
 * structures, but none of its code is derived from ntoskrnl.
 */

#pragma once

#include <ntifs.h>
#include <arc/arc.h>
#include <bugcodes.h>
#include <ndk/asm.h>
#include <ndk/ketypes.h>
#include <ndk/haltypes.h>
#include <ndk/halfuncs.h>
#include <ndk/obtypes.h>
#include <ndk/obfuncs.h>
#include "exportstats.h"

/* ExRaiseStatus is exported as an alias of RtlRaiseStatus */
DECLSPEC_NORETURN VOID NTAPI RtlRaiseStatus(NTSTATUS Status);
#define ExRaiseStatus RtlRaiseStatus

/* ---------------------------------------------------------------------- */
/* Product identity                                                        */
/* ---------------------------------------------------------------------- */

#define QZ_PRODUCT_NAME        "QuarziteOS"
#define QZ_KERNEL_NAME         "qzkrnl"
#define QZ_VERSION_STR         "0.1.0"

/* The Windows version QuarziteOS reports and targets: Windows 7 SP1 */
#define QZ_NT_MAJOR            6
#define QZ_NT_MINOR            1
#define QZ_NT_BUILD            7601
#define QZ_NT_CSD_VERSION      0x0100          /* SP 1.0 */
#define QZ_NT_CSD_STRING       L"Service Pack 1"

/* ---------------------------------------------------------------------- */
/* i386 memory layout (non-PAE, as set up by FreeLoader)                  */
/* ---------------------------------------------------------------------- */

#define QZ_PTE_BASE            0xC0000000UL    /* page-table self-map */
#define QZ_PDE_BASE            0xC0300000UL
#define QZ_POOL_BASE           0xA0000000UL    /* nonpaged pool window */
#define QZ_POOL_SIZE           (64UL * 1024 * 1024)
#define QZ_IOSPACE_BASE        0xB0000000UL    /* MmMapIoSpace window */
#define QZ_IOSPACE_SIZE        (64UL * 1024 * 1024)

#define QZ_PAGE_SIZE           4096UL
#define QZ_PAGE_SHIFT          12

#define QZ_PTE_PRESENT         0x001
#define QZ_PTE_WRITE           0x002
#define QZ_PTE_PWT             0x008
#define QZ_PTE_PCD             0x010
#define QZ_PTE_LARGE           0x080

/* GDT selectors -- identical to the NT x86 layout */
#define QZ_SEL_R0_CODE         0x08
#define QZ_SEL_R0_DATA         0x10
#define QZ_SEL_R3_CODE         0x18
#define QZ_SEL_R3_DATA         0x20
#define QZ_SEL_TSS             0x28
#define QZ_SEL_R0_PCR          0x30

/* Interrupt vectors: the HAL maps IRQ n to PRIMARY_VECTOR_BASE (0x30) + n */
#define QZ_VECTOR_IRQ_BASE     0x30
#define QZ_IRQ_KEYBOARD        1
#define QZ_IRQ_COM1            4

/* ---------------------------------------------------------------------- */
/* Threads                                                                 */
/* ---------------------------------------------------------------------- */

typedef VOID (NTAPI *PQZ_THREAD_ROUTINE)(PVOID Context);

typedef enum _QZ_THREAD_STATE
{
    QzThreadReady,
    QzThreadWaiting,
    QzThreadTerminated,
    QzThreadNew                    /* created, not yet made ready */
} QZ_THREAD_STATE;

#define QZ_THREAD_WAIT_BLOCKS  THREAD_WAIT_OBJECTS   /* 3, as on NT */

typedef struct _QZ_THREAD
{
    ULONG_PTR SavedEsp;            /* kernel stack pointer while switched out */
    PVOID StackBase;               /* lowest address of the stack */
    ULONG StackSize;
    ULONG Id;
    QZ_THREAD_STATE State;
    ULONG Switches;
    CHAR Name[16];
    struct _QZ_THREAD *Next;       /* circular run list */

    /* Dispatcher wait state (ke/wait.c) */
    PKWAIT_BLOCK WaitBlockList;    /* blocks of the current wait */
    ULONG WaitCount;
    NTSTATUS WaitStatus;
    BOOLEAN TimeoutActive;
    ULONGLONG TimeoutInterruptTime;/* absolute, in interrupt time */
    BOOLEAN WaitNext;              /* a KeSet/Release(..., Wait=TRUE) is pending */
    KIRQL WaitIrql;                /* IRQL to return to after that wait */
    KWAIT_BLOCK WaitBlock[QZ_THREAD_WAIT_BLOCKS];

    SHORT KernelApcDisable;        /* KeEnter/LeaveCriticalRegion */
    PVOID PsThread;                /* ps/thread.c: the Thread object, if any */

    /* Processes and user mode (stage B6) */
    struct _QZ_PROCESS *Process;   /* the owning process */
    struct _QZ_PROCESS *ApcProcess;/* whose address space is loaded (attach) */
    struct _QZ_PROCESS *SavedApcProcess; /* KeAttachProcess */
    ULONG_PTR InitialStack;        /* top of the kernel stack, TSS.Esp0 */
    PKTRAP_FRAME TrapFrame;        /* the user-mode frame at the top of the stack */
    KPROCESSOR_MODE PreviousMode;  /* of the system call being served */
    ULONG_PTR FaultRecovery;       /* EIP to resume at on a bad user access */
    ULONG_PTR SyscallRecovery;     /* ... or, failing that, abort the system call */
    ULONG_PTR SyscallEsp;
    PVOID SyscallExceptionList;
    PVOID FxSaveArea;              /* user threads: FPU/SSE state, 512 bytes, 16-aligned */
    BOOLEAN Alerted;
} QZ_THREAD, *PQZ_THREAD;

/* ---------------------------------------------------------------------- */
/* Processor control region: the genuine NT x86 KIPCR (with its KPRCB at   */
/* 0x120), because the HAL and drivers read it directly through FS.        */
/* qzkrnl's own per-processor state lives next to it in QZ_PRCB_EXT.       */
/* ---------------------------------------------------------------------- */

typedef struct _QZ_PRCB_EXT
{
    LIST_ENTRY DpcListHead;
    ULONG DpcQueueDepth;
    ULONG DpcCount;
    volatile BOOLEAN DpcPending;      /* DPCs queued */
    volatile BOOLEAN TimerPending;    /* clock tick wants timer expiration */
    volatile BOOLEAN DispatchPending; /* reschedule requested */
    volatile BOOLEAN DpcRoutineActive;
    ULONG InterruptCount;
    ULONG ClockInterrupts;
    ULONG DispatchInterrupts;
} QZ_PRCB_EXT, *PQZ_PRCB_EXT;

/*
 * On uniprocessor NT 5.x x86, KeGetPcr() is the fixed address KIP0PCRADDRESS
 * (0xFFDFF000), which FreeLoader maps for the kernel; the HAL is compiled
 * that way. The PCR must live there (FS points at the same page).
 */
#define QzPcr (*(PKIPCR)KIP0PCRADDRESS)
extern QZ_PRCB_EXT QzPrcbExt;
#define QzPrcb (&QzPrcbExt)

#ifndef EXCEPTION_CHAIN_END
#define EXCEPTION_CHAIN_END ((PEXCEPTION_REGISTRATION_RECORD)-1)
#endif

/* ---------------------------------------------------------------------- */
/* Globals                                                                 */
/* ---------------------------------------------------------------------- */

extern PLOADER_PARAMETER_BLOCK QzLoaderBlock;
extern volatile ULONG QzTicks;
#define QzCurrentThread (*(PQZ_THREAD *)&QzPcr.PrcbData.CurrentThread)
extern ULONG NtBuildNumber;
extern volatile ULONGLONG QzInterruptTime;   /* 100ns since boot */
extern ULONG QzTimeIncrement;                /* 100ns per clock tick, set by the HAL */

/* ---------------------------------------------------------------------- */
/* CPU helpers                                                             */
/* ---------------------------------------------------------------------- */

static __inline UCHAR QzInB(USHORT Port)
{
    UCHAR v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(Port));
    return v;
}

static __inline VOID QzOutB(USHORT Port, UCHAR Value)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(Value), "Nd"(Port));
}

static __inline VOID QzIoWait(VOID)
{
    QzOutB(0x80, 0);
}

static __inline ULONG QzSaveFlagsAndDisable(VOID)
{
    ULONG Flags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(Flags) : : "memory");
    return Flags;
}

static __inline VOID QzRestoreFlags(ULONG Flags)
{
    if (Flags & 0x200)
        __asm__ __volatile__("sti" : : : "memory");
}

static __inline VOID QzHalt(VOID)
{
    __asm__ __volatile__("hlt" : : : "memory");
}

static __inline VOID QzInvlpg(ULONG_PTR Va)
{
    __asm__ __volatile__("invlpg (%0)" : : "r"(Va) : "memory");
}

static __inline ULONG QzReadCr2(VOID)
{
    ULONG v;
    __asm__ __volatile__("movl %%cr2, %0" : "=r"(v));
    return v;
}

static __inline ULONG QzReadCr3(VOID)
{
    ULONG v;
    __asm__ __volatile__("movl %%cr3, %0" : "=r"(v));
    return v;
}

/* ---------------------------------------------------------------------- */
/* Subsystems                                                              */
/* ---------------------------------------------------------------------- */

/* ke/i386/cpu.c */
VOID QzCpuInit(VOID);
VOID QzSetIdtGate(ULONG Vector, PVOID Handler);
VOID QzCpuGetVendor(PCHAR Vendor13, PCHAR Brand49);

/* ke/i386/trap.c */
VOID QzInstallTrapStubs(VOID);
VOID QzDumpTrapFrame(PKTRAP_FRAME TrapFrame);
DECLSPEC_NORETURN VOID FASTCALL KiEoiHelper(PKTRAP_FRAME TrapFrame);

/* ke/clock.c -- the kernel side of the HAL clock */
VOID QzSharedUserDataInit(VOID);

/* ke/intobj.c -- interrupt objects */
VOID FASTCALL QzDispatchDeviceInterrupt(PKTRAP_FRAME TrapFrame, ULONG Vector);
ULONG QzConnectedInterruptCount(VOID);

/* ke/halif.c -- data the HAL expects from the kernel */
VOID QzHalInterfaceInit(VOID);

/* ke/sched.c */
VOID QzSchedInit(VOID);
PQZ_THREAD QzCreateThread(PCSTR Name, PQZ_THREAD_ROUTINE Routine, PVOID Context);
PQZ_THREAD QzCreateThreadEx(PCSTR Name, PQZ_THREAD_ROUTINE Routine, PVOID Context,
                            struct _QZ_PROCESS *Process, BOOLEAN Ready);
VOID QzReadyNewThread(PQZ_THREAD Thread);
VOID QzReadyThread(PQZ_THREAD Thread);
VOID QzSwapThread(VOID);
VOID QzYield(VOID);
VOID QzSleepTicks(ULONG Ticks);
VOID QzDumpThreads(VOID);
ULONG QzThreadCount(VOID);
PQZ_THREAD QzFirstThread(VOID);
VOID QzRequestDispatch(VOID);
VOID QzRequestSoftwareInterrupt(VOID);

/* ke/irql.c -- IRQL (through the HAL), the DISPATCH software interrupt, DPCs */
KIRQL QzGetCurrentIrql(VOID);
KIRQL QzRaiseIrql(KIRQL NewIrql);
VOID QzLowerIrql(KIRQL NewIrql);
VOID QzRetireDpcs(VOID);
KIRQL QzAcquireSpinLock(PKSPIN_LOCK SpinLock);
VOID QzReleaseSpinLock(PKSPIN_LOCK SpinLock, KIRQL OldIrql);

/* ke/wait.c -- dispatcher objects and waits */
VOID QzWaitTest(PDISPATCHER_HEADER Object);
VOID QzUnwaitThread(PQZ_THREAD Thread, NTSTATUS WaitStatus);
VOID QzExpireThreadTimeouts(VOID);
NTSTATUS QzDelay(LONGLONG Interval100ns);

/* ke/timerobj.c -- kernel timers */
VOID QzTimerObjInit(VOID);
VOID QzExpireTimers(VOID);
ULONG QzActiveTimerCount(VOID);

/* Interrupt object exports (declared by the NDK, not the DDK) */
VOID NTAPI KeInitializeInterrupt(PKINTERRUPT Interrupt, PKSERVICE_ROUTINE ServiceRoutine,
                                 PVOID ServiceContext, PKSPIN_LOCK SpinLock, ULONG Vector,
                                 KIRQL Irql, KIRQL SynchronizeIrql,
                                 KINTERRUPT_MODE InterruptMode, BOOLEAN ShareVector,
                                 CHAR ProcessorNumber, BOOLEAN FloatingSave);
BOOLEAN NTAPI KeConnectInterrupt(PKINTERRUPT Interrupt);
BOOLEAN NTAPI KeDisconnectInterrupt(PKINTERRUPT Interrupt);

/* Exports implemented under internal names (see exports.spec) */
BOOLEAN NTAPI KeIsExecutingDpc(VOID);
VOID NTAPI QzKeAcquireSpinLockAtDpcLevel(PKSPIN_LOCK SpinLock);
VOID NTAPI QzKeReleaseSpinLockFromDpcLevel(PKSPIN_LOCK SpinLock);
VOID NTAPI QzKeEnterCriticalRegion(VOID);
VOID NTAPI QzKeLeaveCriticalRegion(VOID);

/* ob/ -- object manager */
extern POBJECT_TYPE ObpTypeObjectType;
extern POBJECT_TYPE ObpDirectoryObjectType;
extern POBJECT_TYPE ObpSymbolicLinkObjectType;
extern POBJECT_DIRECTORY QzObRootDirectory;
VOID QzObCoreInit(VOID);
VOID QzObInit(PLOADER_PARAMETER_BLOCK LoaderBlock);
VOID QzObInitPhase1(VOID);
VOID QzObAcquireNamespace(VOID);
VOID QzObReleaseNamespace(VOID);
NTSTATUS QzObpLookupObjectName(HANDLE RootHandle, PUNICODE_STRING ObjectName,
                               ULONG Attributes, POBJECT_TYPE ObjectType,
                               KPROCESSOR_MODE AccessMode, PVOID ParseContext,
                               PVOID InsertObject, PVOID *FoundObject);
VOID QzObpDeleteNameCheck(PVOID Object);
NTSTATUS QzObpCreateHandle(PVOID Object, ACCESS_MASK GrantedAccess,
                           ULONG HandleAttributes, PHANDLE Handle);
ACCESS_MASK QzObpMapDesiredAccess(POBJECT_TYPE Type, ACCESS_MASK DesiredAccess);
ULONG QzObHandleCount(VOID);
ULONG QzObObjectCount(VOID);
PCUNICODE_STRING QzObTypeName(PVOID Object);
VOID QzObDumpDirectory(PCWSTR Path);

/* ex/exobj.c -- executive objects */
extern POBJECT_TYPE ExEventObjectType, ExSemaphoreObjectType, ExTimerObjectType;
VOID QzExObjectsInit(VOID);
NTSTATUS NTAPI NtCreateSemaphore(PHANDLE SemaphoreHandle, ACCESS_MASK DesiredAccess,
                                 POBJECT_ATTRIBUTES ObjectAttributes,
                                 LONG InitialCount, LONG MaximumCount);
NTSTATUS NTAPI NtReleaseSemaphore(HANDLE SemaphoreHandle, LONG ReleaseCount,
                                  PLONG PreviousCount);
/* Exported services the DDK headers do not declare */
NTSTATUS NTAPI ZwResetEvent(HANDLE EventHandle, PLONG PreviousState);
NTSTATUS NTAPI ZwPulseEvent(HANDLE EventHandle, PLONG PreviousState);
NTSTATUS NTAPI ZwClearEvent(HANDLE EventHandle);

/* ob/obtest.c */
VOID QzRunObjectTests(VOID);

/* ke/synctest.c */
VOID QzRunSyncTests(VOID);

/* ke/bugcheck.c */
DECLSPEC_NORETURN VOID QzBugCheckWithText(ULONG Code, ULONG_PTR P1, ULONG_PTR P2,
                                          ULONG_PTR P3, ULONG_PTR P4, PCSTR Text);

/* mm/mm.c */
VOID QzMmInit(PLOADER_PARAMETER_BLOCK LoaderBlock);
ULONG QzAllocPhysicalPage(VOID);
VOID QzFreePhysicalPage(ULONG Pfn);
VOID QzMapPage(ULONG_PTR Va, ULONG Pfn, ULONG Flags);
VOID QzUnmapPage(ULONG_PTR Va);
PVOID QzMapPhysical(ULONG PhysicalAddress, ULONG Size, BOOLEAN Uncached);
VOID QzMmDumpInfo(VOID);
VOID QzMmGetStats(PULONG TotalPages, PULONG FreePages);

/* mm/mdl.c */
PVOID QzMapPages(PPFN_NUMBER Pages, ULONG Count, BOOLEAN Uncached);

/* mm/pool.c */
VOID QzPoolInit(VOID);
VOID QzPoolDumpInfo(VOID);

/* drv/serial.c */
VOID QzSerialInit(VOID);
VOID QzSerialPutChar(CHAR c);
VOID QzSerialConnectInterrupt(VOID);

/* drv/vga.c */
VOID QzVgaInit(VOID);
VOID QzVgaPutChar(CHAR c);
VOID QzVgaSetStatus(PCSTR Text);
VOID QzVgaBlueScreen(VOID);
BOOLEAN QzVgaAvailable(VOID);

/* drv/kbd.c */
VOID QzKbdInit(VOID);

/* drv/input.c */
VOID QzInputPush(CHAR c);
CHAR QzInputGet(VOID);                 /* blocks */
VOID QzReadLine(PCHAR Buffer, ULONG Size);

/* rtl/print.c */
int QzVsnprintf(char *Buffer, size_t Size, const char *Format, va_list Args);
int QzSnprintf(char *Buffer, size_t Size, const char *Format, ...);
VOID QzPrint(PCSTR Format, ...);       /* console (VGA + serial) */
VOID QzPutString(PCSTR String);
VOID QzSetConsoleColor(UCHAR Attribute);

/* ex/shell.c */
DECLSPEC_NORETURN VOID QzShell(VOID);

/* rtl/rtl.c */
typedef struct _QZ_LDR_ENTRY          /* the leading part of LDR_DATA_TABLE_ENTRY */
{
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
} QZ_LDR_ENTRY, *PQZ_LDR_ENTRY;
PVOID QzFindKernelExport(PCSTR Name);
PVOID QzFindImageExport(PVOID ImageBase, PCSTR Name);
