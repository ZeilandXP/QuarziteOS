/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Preemptive round-robin scheduler for kernel threads
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Context switching is done entirely through trap frames: every interrupt
 * saves the full register state on the current thread's stack, and the
 * dispatcher may return a different thread's frame to resume.
 *
 * A switch happens in exactly two places:
 *  - the DISPATCH_LEVEL software interrupt (ke/irql.c), when a reschedule is
 *    pending and the interrupted code ran below DISPATCH_LEVEL (preemption);
 *  - the swap vector (int 0x2F), used by a thread to give up the processor
 *    voluntarily -- to yield, or to block in a wait (at DISPATCH_LEVEL).
 * Each thread keeps its own IRQL across switches (SavedIrql).
 *
 * Single processor, no priorities yet -- see ROADMAP.md.
 */

#include "qz.h"

#define QZ_THREAD_STACK_SIZE   (16 * 1024)

/* Exported: drivers read the processor count directly */
CCHAR KeNumberProcessors = 1;

static PQZ_THREAD QzIdleThread;
static QZ_THREAD QzBootThread;
static ULONG QzNextThreadId = 1;

extern VOID QzThreadTrampoline(VOID);
VOID __cdecl QzExitCurrentThread(VOID);

/* New threads start here at PASSIVE_LEVEL: EBX = routine, ESI = context */
__asm__(
    ".text\n"
    ".globl _QzThreadTrampoline\n"
    "_QzThreadTrampoline:\n"
    "    pushl %esi\n"
    "    call *%ebx\n"            /* NTAPI routine pops its argument */
    "    call _QzExitCurrentThread\n"
    "1:  hlt\n"
    "    jmp 1b\n"
);

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
    PQZ_THREAD Thread;
    PQZ_TRAP_FRAME Frame;
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
    Thread->State = QzThreadReady;
    Thread->SavedIrql = PASSIVE_LEVEL;
    strncpy(Thread->Name, Name, sizeof(Thread->Name) - 1);

    /* Fabricate the frame an interrupt would have left behind */
    Frame = (PQZ_TRAP_FRAME)(Stack + QZ_THREAD_STACK_SIZE - sizeof(QZ_TRAP_FRAME));
    RtlZeroMemory(Frame, sizeof(*Frame));
    Frame->Eip = (ULONG)(ULONG_PTR)QzThreadTrampoline;
    Frame->Cs = QZ_SEL_R0_CODE;
    Frame->EFlags = 0x202;  /* IF set */
    Frame->Ds = Frame->Es = Frame->Gs = QZ_SEL_R0_DATA;
    Frame->Fs = QZ_SEL_R0_PCR;
    Frame->Ebx = (ULONG)(ULONG_PTR)Routine;
    Frame->Esi = (ULONG)(ULONG_PTR)Context;
    Thread->Frame = Frame;

    QzInsertThread(Thread);
    return Thread;
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
    QzBootThread.SavedIrql = PASSIVE_LEVEL;
    strncpy(QzBootThread.Name, "main", sizeof(QzBootThread.Name) - 1);
    QzBootThread.Next = &QzBootThread;
    QzCurrentThread = &QzBootThread;

    QzIdleThread = QzCreateThread("idle", QzIdleRoutine, NULL);
    QzPrcb->IdleThread = QzIdleThread;
}

PQZ_THREAD
QzFirstThread(VOID)
{
    return QzCurrentThread;
}

/*
 * Pick the thread to run next. Called with interrupts disabled from the
 * swap vector or the dispatch software interrupt; CurrentIrql is the IRQL
 * the current thread must get back when it is resumed.
 */
PQZ_TRAP_FRAME
QzSelectNextThread(PQZ_TRAP_FRAME Frame, KIRQL CurrentIrql)
{
    PQZ_THREAD Current = QzCurrentThread, Thread, Next = NULL;

    if (!Current)
        return Frame;

    Current->Frame = Frame;
    Current->SavedIrql = CurrentIrql;
    QzPrcb->DispatchPending = FALSE;

    /* Round robin: first ready, non-idle thread after the current one */
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
        Next->Switches++;
    QzCurrentThread = Next;
    QzPcr.Pcr.Irql = Next->SavedIrql;
    return Next->Frame;
}

/* Make a waiting thread runnable again (dispatcher database held) */
VOID
QzReadyThread(PQZ_THREAD Thread)
{
    Thread->State = QzThreadReady;
    QzPrcb->DispatchPending = TRUE;
}

/* Give up the processor; returns when this thread is scheduled again */
VOID
QzSwapThread(VOID)
{
    __asm__ __volatile__("int $0x2F" : : : "memory");
}

VOID
QzYield(VOID)
{
    QzSwapThread();
}

VOID
QzSleepTicks(ULONG Ticks)
{
    if (!QzCurrentThread)
        return;
    QzDelay(-(LONGLONG)Ticks * QZ_TIME_INCREMENT);
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
    static PCSTR States[] = { "ready", "waiting", "dead" };
    PQZ_THREAD Thread = QzCurrentThread;

    QzPrint("  TID  NAME            STATE     SWITCHES  IRQL  STACK\n");
    do
    {
        QzPrint("  %3lu  %-15s %-8s %9lu  %4u  %p%s\n",
                Thread->Id, Thread->Name, States[Thread->State],
                Thread->Switches,
                Thread == QzCurrentThread ? QzGetCurrentIrql() : Thread->SavedIrql,
                Thread->StackBase,
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

KPROCESSOR_MODE
NTAPI
KeGetPreviousMode(VOID)
{
    /* No user mode yet */
    return KernelMode;
}

KPROCESSOR_MODE
NTAPI
ExGetPreviousMode(VOID)
{
    return KernelMode;
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
