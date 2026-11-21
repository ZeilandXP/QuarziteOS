/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Preemptive round-robin scheduler for kernel threads
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Context switching swaps kernel stacks: QzSwapContext saves the callee-
 * saved registers, EFLAGS and the SEH chain head on the old thread's stack
 * and resumes the new one where it last called QzSwapContext. Switches
 * only happen at DISPATCH_LEVEL:
 *  - in KiDispatchInterrupt, which the HAL calls for the DISPATCH software
 *    interrupt (clock quantum end, a thread became ready);
 *  - when a thread blocks in a wait, yields or exits.
 * Each thread's previous IRQL is kept by whoever raised it on that thread's
 * stack (the HAL's interrupt path, or the waiting code), so it comes back
 * on its own when the thread resumes.
 *
 * Single processor, no priorities yet -- see ROADMAP.md.
 */

#include "qz.h"
#include "qzps.h"

#define QZ_THREAD_STACK_SIZE   (16 * 1024)

/* Exported: drivers read the processor count directly */
CCHAR KeNumberProcessors = 1;

static PQZ_THREAD QzIdleThread;
static QZ_THREAD QzBootThread;
static ULONG QzNextThreadId = 1;

extern VOID QzThreadStartup(VOID);
VOID FASTCALL QzSwapContext(PQZ_THREAD OldThread, PQZ_THREAD NewThread);
VOID __cdecl QzExitCurrentThread(VOID);

C_ASSERT(FIELD_OFFSET(QZ_THREAD, SavedEsp) == 0);

__asm__(
    ".text\n"
    /* QzSwapContext(Old = ecx, New = edx) */
    ".globl @QzSwapContext@8\n"
    "@QzSwapContext@8:\n"
    "    pushl %ebp\n"
    "    pushl %ebx\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    pushfl\n"
    "    pushl %fs:0\n"
    "    movl %esp, (%ecx)\n"
    "    movl (%edx), %esp\n"
    "    popl %fs:0\n"
    "    popfl\n"
    "    popl %edi\n"
    "    popl %esi\n"
    "    popl %ebx\n"
    "    popl %ebp\n"
    "    ret\n"
    "\n"
    /* First switch into a new thread lands here: EBX = routine, ESI = context */
    ".globl _QzThreadStartup\n"
    "_QzThreadStartup:\n"
    "    pushl %esi\n"
    "    pushl %ebx\n"
    "    call _QzThreadMain@8\n"
    "1:  hlt\n"
    "    jmp 1b\n"
);

DECLSPEC_NORETURN VOID NTAPI
QzThreadMain(PQZ_THREAD_ROUTINE Routine, PVOID Context)
{
    /* We arrive at DISPATCH_LEVEL from the switch: drop to PASSIVE_LEVEL */
    QzLowerIrql(PASSIVE_LEVEL);
    Routine(Context);
    QzExitCurrentThread();
    for (;;);
}

static VOID NTAPI
QzIdleRoutine(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    for (;;)
    {
        __asm__ __volatile__("sti; hlt" : : : "memory");
    }
}

static VOID
QzInsertThread(PQZ_THREAD Thread)
{
    ULONG Flags = QzSaveFlagsAndDisable();
    Thread->Next = QzCurrentThread->Next;
    QzCurrentThread->Next = Thread;
    QzRestoreFlags(Flags);
}

PQZ_THREAD
QzCreateThread(PCSTR Name, PQZ_THREAD_ROUTINE Routine, PVOID Context)
{
    return QzCreateThreadEx(Name, Routine, Context, NULL, TRUE);
}

/* Process NULL: the System process (or none yet, early in boot) */
PQZ_THREAD
QzCreateThreadEx(PCSTR Name, PQZ_THREAD_ROUTINE Routine, PVOID Context,
                 struct _QZ_PROCESS *Process, BOOLEAN Ready)
{
    PQZ_THREAD Thread;
    PUCHAR Stack;

    Thread = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Thread), 'dhTQ');
    Stack = ExAllocatePoolWithTag(NonPagedPool, QZ_THREAD_STACK_SIZE, 'ktSQ');
    if (!Thread || !Stack)
    {
        if (Thread) ExFreePoolWithTag(Thread, 'dhTQ');
        if (Stack) ExFreePoolWithTag(Stack, 'ktSQ');
        return NULL;
    }

    RtlZeroMemory(Thread, sizeof(*Thread));
    Thread->StackBase = Stack;
    Thread->StackSize = QZ_THREAD_STACK_SIZE;
    Thread->Id = QzNextThreadId++;
    Thread->State = Ready ? QzThreadReady : QzThreadNew;
    Thread->InitialStack = (ULONG_PTR)Stack + QZ_THREAD_STACK_SIZE;
    if (!Process)
        Process = QzSystemProcess;   /* NULL until the System process exists */
    Thread->Process = Thread->ApcProcess = Process;
    strncpy(Thread->Name, Name, sizeof(Thread->Name) - 1);

    /* Fabricate what QzSwapContext pops: SEH head, EFLAGS, edi, esi, ebx, ebp, ret */
    {
        /* The top of the stack is kept for a ring-3 thread's KTRAP_FRAME */
        PULONG_PTR Sp = (PULONG_PTR)(Stack + QZ_THREAD_STACK_SIZE - sizeof(KTRAP_FRAME));
        *--Sp = (ULONG_PTR)QzThreadStartup;     /* return address */
        *--Sp = 0;                              /* ebp */
        *--Sp = (ULONG_PTR)Routine;             /* ebx */
        *--Sp = (ULONG_PTR)Context;             /* esi */
        *--Sp = 0;                              /* edi */
        *--Sp = 0x202;                          /* EFLAGS: IF */
        *--Sp = (ULONG_PTR)EXCEPTION_CHAIN_END; /* fs:[0] */
        Thread->SavedEsp = (ULONG_PTR)Sp;
    }

    QzInsertThread(Thread);
    return Thread;
}

VOID
QzReadyNewThread(PQZ_THREAD Thread)
{
    KIRQL OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    Thread->State = QzThreadReady;
    QzRequestDispatch();
    QzLowerIrql(OldIrql);
}

VOID
QzSchedInit(VOID)
{
    extern UCHAR QzBootStack[];

    /* The boot thread adopts the context we are running on right now */
    QzBootThread.Id = 0;
    QzBootThread.StackBase = QzBootStack;
    QzBootThread.StackSize = 16 * 1024;
    QzBootThread.State = QzThreadReady;
    strncpy(QzBootThread.Name, "main", sizeof(QzBootThread.Name) - 1);
    QzBootThread.Next = &QzBootThread;
    QzCurrentThread = &QzBootThread;

    QzIdleThread = QzCreateThread("idle", QzIdleRoutine, NULL);
    QzPcr.PrcbData.IdleThread = (PKTHREAD)QzIdleThread;
}

PQZ_THREAD
QzFirstThread(VOID)
{
    return QzCurrentThread;
}

/*
 * Request the DISPATCH software interrupt. NT semantics: below
 * DISPATCH_LEVEL it is delivered at once. The PIC HAL only fires APC
 * requests immediately (it checks IRR & 3), so -- like ntoskrnl -- raise and
 * lower around the request and let KfLowerIrql deliver it.
 */
VOID
QzRequestSoftwareInterrupt(VOID)
{
    HalRequestSoftwareInterrupt(DISPATCH_LEVEL);
    if (QzPcr.Irql < DISPATCH_LEVEL)
        KfLowerIrql(KfRaiseIrql(DISPATCH_LEVEL));
}

/* Ask for the DISPATCH software interrupt to reschedule */
VOID
QzRequestDispatch(VOID)
{
    QzPrcb->DispatchPending = TRUE;
    QzRequestSoftwareInterrupt();
}

/* Make a waiting thread runnable again (dispatcher database held) */
VOID
QzReadyThread(PQZ_THREAD Thread)
{
    Thread->State = QzThreadReady;
    QzRequestDispatch();
}

/*
 * Switch to the next ready thread (round robin), or to the idle thread.
 * Must be called at DISPATCH_LEVEL. Returns when this thread runs again;
 * if it is still ready and alone, returns immediately.
 */
VOID
QzSwapThread(VOID)
{
    PQZ_THREAD Current = QzCurrentThread, Thread, Next = NULL;
    ULONG Flags;

    if (!Current)
        return;
    if (QzPcr.Irql < DISPATCH_LEVEL)
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL, QzPcr.Irql, DISPATCH_LEVEL, 0, 1);

    Flags = QzSaveFlagsAndDisable();

    /* Unlink threads that have ended (not the one running: we are on its stack) */
    {
        PQZ_THREAD Prev = Current, Dead = NULL;
        for (Thread = Current->Next; Thread != Current; Thread = Prev->Next)
        {
            if (Thread->State == QzThreadTerminated && Thread != QzIdleThread && Thread != &QzBootThread)
            {
                Prev->Next = Thread->Next;
                Thread->Next = Dead;
                Dead = Thread;
            }
            else
            {
                Prev = Thread;
            }
        }
        while (Dead)
        {
            Thread = Dead;
            Dead = Dead->Next;
            if (Thread->FxSaveArea)
                ExFreePool(((PVOID *)Thread->FxSaveArea)[-1]);
            ExFreePoolWithTag(Thread->StackBase, 'ktSQ');
            ExFreePoolWithTag(Thread, 'dhTQ');
        }
    }

    for (Thread = Current->Next; ; Thread = Thread->Next)
    {
        if (Thread->State == QzThreadReady && Thread != QzIdleThread)
        {
            Next = Thread;
            break;
        }
        if (Thread == Current)
            break;
    }
    if (!Next)
        Next = QzIdleThread;

    if (Next != Current)
    {
        Next->Switches++;
        QzCurrentThread = Next;
        /* The next thread's address space, and its stack for ring 3 -> 0 */
        if (Next->ApcProcess)
            QzMmSwitchAddressSpace(Next->ApcProcess);
        /* Ring 3 entries build their frame below Esp0; NT keeps 16 bytes
           (the V86 segment slots) above it */
        if (Next->InitialStack)
            ((PULONG)QzPcr.TSS)[1] = (ULONG)Next->InitialStack - 0x10;
        if (Next->PsThread && ((PQZ_PS_THREAD)Next->PsThread)->Teb)
            QzSetTebSelector(((PQZ_PS_THREAD)Next->PsThread)->Teb);
        if (Current->FxSaveArea)
            __asm__ __volatile__("fxsave (%0)" : : "r"(Current->FxSaveArea) : "memory");
        if (Next->FxSaveArea)
            __asm__ __volatile__("fxrstor (%0)" : : "r"(Next->FxSaveArea) : "memory");
        QzSwapContext(Current, Next);
    }

    QzRestoreFlags(Flags);
}

VOID
QzYield(VOID)
{
    KIRQL OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    QzSwapThread();
    QzLowerIrql(OldIrql);
}

VOID
QzSleepTicks(ULONG Ticks)
{
    if (!QzCurrentThread)
        return;
    QzDelay(-(LONGLONG)Ticks * QzTimeIncrement);
}

VOID __cdecl
QzExitCurrentThread(VOID)
{
    /* TODO: reap the stack from another thread's context */
    (VOID)QzRaiseIrql(DISPATCH_LEVEL);
    QzCurrentThread->State = QzThreadTerminated;
    for (;;)
        QzSwapThread();
}

ULONG
QzThreadCount(VOID)
{
    PQZ_THREAD Thread = QzCurrentThread;
    ULONG Count = 0;

    if (!Thread)
        return 0;
    do
    {
        if (Thread->State != QzThreadTerminated)
            Count++;
        Thread = Thread->Next;
    } while (Thread != QzCurrentThread);
    return Count;
}

VOID
QzDumpThreads(VOID)
{
    static PCSTR States[] = { "ready", "waiting", "dead", "new" };
    PQZ_THREAD Thread = QzCurrentThread;

    QzPrint("  TID  NAME            STATE     SWITCHES  STACK\n");
    do
    {
        QzPrint("  %3lu  %-15s %-8s %9lu  %p%s\n",
                Thread->Id, Thread->Name, States[Thread->State],
                Thread->Switches, Thread->StackBase,
                Thread == QzCurrentThread ? "  <- running" : "");
        Thread = Thread->Next;
    } while (Thread != QzCurrentThread);
}

/* ---------------------------------------------------------------------- */
/* Exports                                                                 */
/* ---------------------------------------------------------------------- */

#undef KeGetCurrentThread

PKTHREAD
NTAPI
KeGetCurrentThread(VOID)
{
    /* Opaque to drivers: they only pass it back to the kernel */
    return (PKTHREAD)QzCurrentThread;
}

/* UserMode while serving a system call made from ring 3 */
KPROCESSOR_MODE
NTAPI
KeGetPreviousMode(VOID)
{
    return QzCurrentThread ? QzCurrentThread->PreviousMode : KernelMode;
}

KPROCESSOR_MODE
NTAPI
ExGetPreviousMode(VOID)
{
    return QzCurrentThread ? QzCurrentThread->PreviousMode : KernelMode;
}

VOID
NTAPI
QzKeEnterCriticalRegion(VOID)
{
    QzCurrentThread->KernelApcDisable--;
}

VOID
NTAPI
QzKeLeaveCriticalRegion(VOID)
{
    QzCurrentThread->KernelApcDisable++;
}

BOOLEAN
NTAPI
KeAreApcsDisabled(VOID)
{
    return QzCurrentThread->KernelApcDisable != 0 ||
           QzGetCurrentIrql() >= APC_LEVEL;
}

KAFFINITY
NTAPI
KeQueryActiveProcessors(VOID)
{
    return 1;
}

NTSTATUS
NTAPI
KeDelayExecutionThread(IN KPROCESSOR_MODE WaitMode,
                       IN BOOLEAN Alertable,
                       IN PLARGE_INTEGER Interval)
{
    UNREFERENCED_PARAMETER(WaitMode);
    UNREFERENCED_PARAMETER(Alertable);
    return QzDelay(Interval->QuadPart);
}

/* NT 6.0 / 6.1 processor and group queries: one processor, one group */

ULONG
NTAPI
QzKeQueryActiveProcessorCount(OUT PKAFFINITY ActiveProcessors OPTIONAL)
{
    if (ActiveProcessors)
        *ActiveProcessors = 1;
    return 1;
}

ULONG
NTAPI
QzKeQueryProcessorCountEx(IN USHORT GroupNumber)
{
    return (GroupNumber == 0 || GroupNumber == ALL_PROCESSOR_GROUPS) ? 1 : 0;
}

ULONG
NTAPI
QzKeQueryMaximumProcessorCount(VOID)
{
    return 1;
}

USHORT
NTAPI
QzKeQueryGroupCount(VOID)
{
    return 1;
}

ULONG
NTAPI
QzKeGetCurrentProcessorNumberEx(OUT PPROCESSOR_NUMBER ProcNumber OPTIONAL)
{
    if (ProcNumber)
    {
        ProcNumber->Group = 0;
        ProcNumber->Number = 0;
        ProcNumber->Reserved = 0;
    }
    return 0;
}
