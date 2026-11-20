/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Preemptive round-robin scheduler for kernel threads
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Context switching is done entirely through trap frames: every interrupt
 * saves the full register state on the current thread's stack, and the
 * dispatcher may return a different thread's frame to resume. The clock
 * interrupt preempts; int 0x2F yields voluntarily.
 *
 * This is deliberately simple (single CPU, no priorities, no IRQL model yet)
 * and will grow into a KTHREAD/KPRCB based dispatcher -- see ROADMAP.md.
 */

#include "qz.h"

#define QZ_THREAD_STACK_SIZE   (16 * 1024)

PQZ_THREAD QzCurrentThread;
static PQZ_THREAD QzIdleThread;
static QZ_THREAD QzBootThread;
static ULONG QzNextThreadId = 1;

extern VOID QzThreadTrampoline(VOID);
VOID __cdecl QzExitCurrentThread(VOID);

/* New threads start here: EBX = routine, ESI = context */
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
    /* The boot thread adopts the context we are running on right now */
    QzBootThread.Id = 0;
    QzBootThread.State = QzThreadReady;
    strncpy(QzBootThread.Name, "main", sizeof(QzBootThread.Name) - 1);
    QzBootThread.Next = &QzBootThread;
    QzCurrentThread = &QzBootThread;

    QzIdleThread = QzCreateThread("idle", QzIdleRoutine, NULL);
}

PQZ_TRAP_FRAME
QzSchedule(PQZ_TRAP_FRAME Frame)
{
    PQZ_THREAD Current = QzCurrentThread, Thread, Next = NULL;

    if (!Current)
        return Frame;

    Current->Frame = Frame;

    /* Wake sleepers whose deadline has passed */
    Thread = Current;
    do
    {
        if (Thread->State == QzThreadWaiting &&
            (LONG)(QzTicks - Thread->WakeTick) >= 0)
        {
            Thread->State = QzThreadReady;
        }
        Thread = Thread->Next;
    } while (Thread != Current);

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
    return Next->Frame;
}

VOID
QzYield(VOID)
{
    __asm__ __volatile__("int $0x2F" : : : "memory");
}

VOID
QzSleepTicks(ULONG Ticks)
{
    if (!QzCurrentThread)
        return;
    QzCurrentThread->WakeTick = QzTicks + Ticks;
    QzCurrentThread->State = QzThreadWaiting;
    QzYield();
}

VOID __cdecl
QzExitCurrentThread(VOID)
{
    /* TODO: reap the stack from another thread's context */
    QzCurrentThread->State = QzThreadTerminated;
    for (;;)
        QzYield();
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

NTSTATUS
NTAPI
KeDelayExecutionThread(IN KPROCESSOR_MODE WaitMode,
                       IN BOOLEAN Alertable,
                       IN PLARGE_INTEGER Interval)
{
    LONGLONG Delay;
    ULONG Ticks;

    UNREFERENCED_PARAMETER(WaitMode);
    UNREFERENCED_PARAMETER(Alertable);

    /* Negative = relative (100ns units); absolute times are not supported yet */
    Delay = Interval->QuadPart < 0 ? -Interval->QuadPart : 0;
    Ticks = (ULONG)((ULONGLONG)(Delay + QZ_TIME_INCREMENT - 1) / QZ_TIME_INCREMENT);

    if (Ticks == 0)
        QzYield();
    else
        QzSleepTicks(Ticks);
    return STATUS_SUCCESS;
}
