/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Asynchronous procedure calls: kernel (special and normal) and user APCs
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Each thread has a kernel-mode and a user-mode APC list.
 *
 * Kernel APCs run in the thread they were queued to, at APC_LEVEL: when
 * it is the running thread, through the HAL's APC software interrupt as
 * soon as its IRQL drops below APC_LEVEL; when it waits at PASSIVE_LEVEL,
 * the wait is broken (STATUS_KERNEL_APC), the APCs run and the wait starts
 * again. Special kernel APCs (no normal routine) always run first; normal
 * ones wait for critical regions to end and do not nest. They are held
 * while the thread is attached to another process: they expect their
 * own address space (the I/O manager's completion writes the caller's
 * buffers from one).
 *
 * User APCs run only when the thread lets them: an alertable wait from
 * user mode returns STATUS_USER_APC, and on the way back to ring 3 the
 * thread's context goes onto its user stack with a frame for ntdll's
 * KiUserApcDispatcher, which calls the routine and NtContinue()s into
 * the saved context, asking for the next APC.
 */

#include "qz.h"
#include "qzps.h"

BOOLEAN QzPsIsObject(PVOID Body);
VOID QzUnwaitThread(PQZ_THREAD Thread, NTSTATUS WaitStatus);
ULONG_PTR QzPsUserApcDispatcher(VOID);

static __inline VOID
QzApcInitLists(PQZ_THREAD Thread)
{
    if (!Thread->ApcListHead[KernelMode].Flink)
    {
        InitializeListHead(&Thread->ApcListHead[KernelMode]);
        InitializeListHead(&Thread->ApcListHead[UserMode]);
    }
}

/* A PKTHREAD from what callers pass: the scheduler thread, or a Thread object */
PQZ_THREAD
QzKeThreadOf(PVOID Thread)
{
    if (Thread && QzPsIsObject(Thread))
        return ((PQZ_PS_THREAD)Thread)->Thread;
    return Thread;
}

static __inline BOOLEAN
QzApcAttached(PQZ_THREAD Thread)
{
    return Thread->Process && Thread->ApcProcess != Thread->Process;
}

/* Can a kernel APC at the head of the list run now (IRQL aside)? */
static BOOLEAN
QzApcKernelDeliverable(PQZ_THREAD Thread)
{
    PKAPC Apc;

    QzApcInitLists(Thread);
    if (IsListEmpty(&Thread->ApcListHead[KernelMode]) || QzApcAttached(Thread))
        return FALSE;
    Apc = CONTAINING_RECORD(Thread->ApcListHead[KernelMode].Flink, KAPC, ApcListEntry);
    if (!Apc->NormalRoutine)
        return TRUE;                                    /* special */
    return !Thread->KernelApcDisable && !Thread->KernelApcInProgress;
}

/* For the current thread: ask the HAL for the APC interrupt if one can run */
VOID
QzApcCheckPending(VOID)
{
    PQZ_THREAD Thread = QzCurrentThread;
    if (Thread && Thread->KernelApcPending && QzApcKernelDeliverable(Thread))
        HalRequestSoftwareInterrupt(APC_LEVEL);
}

VOID
NTAPI
KeInitializeApc(IN PRKAPC Apc, IN PRKTHREAD Thread, IN KAPC_ENVIRONMENT TargetEnvironment,
                IN PKKERNEL_ROUTINE KernelRoutine, IN PKRUNDOWN_ROUTINE RundownRoutine OPTIONAL,
                IN PKNORMAL_ROUTINE NormalRoutine OPTIONAL, IN KPROCESSOR_MODE ApcMode OPTIONAL,
                IN PVOID NormalContext OPTIONAL)
{
    RtlZeroMemory(Apc, sizeof(*Apc));
    Apc->Type = ApcObject;
    Apc->Size = sizeof(KAPC);
    Apc->Thread = (PKTHREAD)QzKeThreadOf(Thread);
    Apc->ApcStateIndex = (CCHAR)TargetEnvironment;
    Apc->KernelRoutine = KernelRoutine;
    Apc->RundownRoutine = RundownRoutine;
    Apc->NormalRoutine = NormalRoutine;
    if (NormalRoutine)
    {
        Apc->ApcMode = ApcMode;
        Apc->NormalContext = NormalContext;
    }
    else
    {
        Apc->ApcMode = KernelMode;
    }
}

BOOLEAN
NTAPI
KeInsertQueueApc(IN PRKAPC Apc, IN PVOID SystemArgument1, IN PVOID SystemArgument2, IN KPRIORITY PriorityBoost)
{
    PQZ_THREAD Thread = (PQZ_THREAD)Apc->Thread;
    PLIST_ENTRY Head, Entry;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(PriorityBoost);
    OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    if (Apc->Inserted || !Thread || Thread->State == QzThreadTerminated || Thread->ApcQueueingDisabled)
    {
        QzLowerIrql(OldIrql);
        return FALSE;
    }
    QzApcInitLists(Thread);
    Apc->SystemArgument1 = SystemArgument1;
    Apc->SystemArgument2 = SystemArgument2;
    Apc->Inserted = TRUE;
    Head = &Thread->ApcListHead[(UCHAR)Apc->ApcMode];

    if (Apc->ApcMode == KernelMode && !Apc->NormalRoutine)
    {
        /* Special APCs go after the special ones already queued, before the normal ones */
        for (Entry = Head->Flink; Entry != Head; Entry = Entry->Flink)
            if (CONTAINING_RECORD(Entry, KAPC, ApcListEntry)->NormalRoutine)
                break;
        InsertTailList(Entry, &Apc->ApcListEntry);
    }
    else
    {
        InsertTailList(Head, &Apc->ApcListEntry);
    }

    if (Apc->ApcMode == KernelMode)
    {
        Thread->KernelApcPending = TRUE;
        if (Thread == QzCurrentThread)
        {
            if (QzApcKernelDeliverable(Thread))
                HalRequestSoftwareInterrupt(APC_LEVEL);
        }
        else if (Thread->State == QzThreadWaiting &&
                 Thread->WaitOldIrql < APC_LEVEL && QzApcKernelDeliverable(Thread))
        {
            QzUnwaitThread(Thread, STATUS_KERNEL_APC);
        }
    }
    else if (Thread->State == QzThreadWaiting && Thread->WaitAlertable && Thread->WaitMode == UserMode)
    {
        Thread->UserApcPending = TRUE;
        QzUnwaitThread(Thread, STATUS_USER_APC);
    }
    QzLowerIrql(OldIrql);
    return TRUE;
}

BOOLEAN
NTAPI
KeRemoveQueueApc(IN PKAPC Apc)
{
    KIRQL OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    BOOLEAN Inserted = Apc->Inserted;

    if (Inserted)
    {
        RemoveEntryList(&Apc->ApcListEntry);
        Apc->Inserted = FALSE;
    }
    QzLowerIrql(OldIrql);
    return Inserted;
}

/* The HAL's APC software interrupt, at APC_LEVEL: the current thread's kernel APCs */
VOID
NTAPI
KiDeliverApc(IN KPROCESSOR_MODE DeliveryMode, IN PKEXCEPTION_FRAME ExceptionFrame, IN PKTRAP_FRAME TrapFrame)
{
    PQZ_THREAD Thread = QzCurrentThread;

    UNREFERENCED_PARAMETER(DeliveryMode);
    UNREFERENCED_PARAMETER(ExceptionFrame);
    UNREFERENCED_PARAMETER(TrapFrame);
    if (!Thread)
        return;

    for (;;)
    {
        PKKERNEL_ROUTINE KernelRoutine;
        PKNORMAL_ROUTINE NormalRoutine;
        PVOID NormalContext, Argument1, Argument2;
        KIRQL OldIrql;
        PKAPC Apc;

        OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
        QzApcInitLists(Thread);
        if (IsListEmpty(&Thread->ApcListHead[KernelMode]))
        {
            Thread->KernelApcPending = FALSE;
            QzLowerIrql(OldIrql);
            return;
        }
        if (!QzApcKernelDeliverable(Thread))
        {
            QzLowerIrql(OldIrql);       /* still pending: leaving the region or detaching asks again */
            return;
        }
        Apc = CONTAINING_RECORD(Thread->ApcListHead[KernelMode].Flink, KAPC, ApcListEntry);
        RemoveEntryList(&Apc->ApcListEntry);
        Apc->Inserted = FALSE;
        KernelRoutine = Apc->KernelRoutine;
        NormalRoutine = Apc->NormalRoutine;
        NormalContext = Apc->NormalContext;
        Argument1 = Apc->SystemArgument1;
        Argument2 = Apc->SystemArgument2;
        QzLowerIrql(OldIrql);           /* back to APC_LEVEL */

        /* The kernel routine may free the APC, and may cancel the normal routine */
        KernelRoutine(Apc, &NormalRoutine, &NormalContext, &Argument1, &Argument2);
        if (NormalRoutine)
        {
            Thread->KernelApcInProgress = TRUE;
            QzLowerIrql(PASSIVE_LEVEL);
            NormalRoutine(NormalContext, Argument1, Argument2);
            QzRaiseIrql(APC_LEVEL);
            Thread->KernelApcInProgress = FALSE;
        }
    }
}

/*
 * On the way back to ring 3 (KiEoiHelper, interrupts off): a pending
 * user APC becomes a KiUserApcDispatcher frame on the user stack.
 */
VOID
QzApcDeliverUser(PKTRAP_FRAME TrapFrame)
{
    PQZ_THREAD Thread = QzCurrentThread;
    PKNORMAL_ROUTINE NormalRoutine;
    PVOID NormalContext, Argument1, Argument2;
    PKKERNEL_ROUTINE KernelRoutine;
    ULONG_PTR Dispatcher = QzPsUserApcDispatcher(), Stack, Frame[4];
    CONTEXT Context;
    KIRQL OldIrql;
    PKAPC Apc;

    if (!Thread->UserApcPending)
        return;
    _enable();
    OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    Thread->UserApcPending = FALSE;
    QzApcInitLists(Thread);
    if (IsListEmpty(&Thread->ApcListHead[UserMode]) || !Dispatcher)
    {
        QzLowerIrql(OldIrql);
        _disable();
        return;
    }
    Apc = CONTAINING_RECORD(Thread->ApcListHead[UserMode].Flink, KAPC, ApcListEntry);
    RemoveEntryList(&Apc->ApcListEntry);
    Apc->Inserted = FALSE;
    KernelRoutine = Apc->KernelRoutine;
    NormalRoutine = Apc->NormalRoutine;
    NormalContext = Apc->NormalContext;
    Argument1 = Apc->SystemArgument1;
    Argument2 = Apc->SystemArgument2;
    QzLowerIrql(APC_LEVEL);
    KernelRoutine(Apc, &NormalRoutine, &NormalContext, &Argument1, &Argument2);
    QzLowerIrql(OldIrql);

    if (NormalRoutine)
    {
        RtlZeroMemory(&Context, sizeof(Context));
        QzTrapFrameToContext(TrapFrame, &Context);
        Context.ContextFlags = CONTEXT_FULL;
        /* Under the interrupted stack: 8 bytes KiUserApcDispatcher uses for its SEH
           registration (above the context), the context, then the dispatcher's frame */
        Stack = ((((Context.Esp & ~3UL) - 8) & ~3UL) - (CONTEXT_ALIGNED_SIZE + sizeof(Frame)));
        Frame[0] = (ULONG_PTR)NormalRoutine;
        Frame[1] = (ULONG_PTR)NormalContext;
        Frame[2] = (ULONG_PTR)Argument1;
        Frame[3] = (ULONG_PTR)Argument2;
        if (QzCopyToUser((PVOID)(Stack + sizeof(Frame)), &Context, sizeof(Context)) &&
            QzCopyToUser((PVOID)Stack, Frame, sizeof(Frame)))
        {
            TrapFrame->Eip = Dispatcher;
            TrapFrame->HardwareEsp = Stack;
            TrapFrame->DbgEip = Dispatcher;
        }
        else
        {
            DbgPrint(QZ_KERNEL_NAME ": [%s] no room on the user stack for an APC\n", Thread->Name);
        }
    }
    _disable();
}

/* NtContinue(..., TestAlert) and NtTestAlert: the next user APC, if any */
BOOLEAN
QzApcTestAlert(VOID)
{
    PQZ_THREAD Thread = QzCurrentThread;
    KIRQL OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    BOOLEAN Alerted = Thread->Alerted;

    QzApcInitLists(Thread);
    Thread->Alerted = FALSE;
    if (!IsListEmpty(&Thread->ApcListHead[UserMode]))
        Thread->UserApcPending = TRUE;
    QzLowerIrql(OldIrql);
    return Alerted;
}

/* At the start of a wait (DISPATCH_LEVEL): what ends it before it blocks */
NTSTATUS
QzApcWaitCheck(PQZ_THREAD Thread, BOOLEAN Alertable, KPROCESSOR_MODE WaitMode, KIRQL OldIrql)
{
    QzApcInitLists(Thread);
    if (Thread->KernelApcPending && OldIrql < APC_LEVEL && QzApcKernelDeliverable(Thread))
        return STATUS_KERNEL_APC;
    if (Alertable)
    {
        if (Thread->Alerted)
        {
            Thread->Alerted = FALSE;
            return STATUS_ALERTED;
        }
        if (WaitMode == UserMode && !IsListEmpty(&Thread->ApcListHead[UserMode]))
        {
            Thread->UserApcPending = TRUE;
            return STATUS_USER_APC;
        }
    }
    return STATUS_SUCCESS;
}

/* A thread ends: its APCs that never ran get their rundown routines */
VOID
QzApcRundown(PQZ_THREAD Thread)
{
    ULONG Mode;

    if (!Thread->ApcListHead[KernelMode].Flink)
        return;
    for (Mode = 0; Mode < 2; Mode++)
    {
        while (!IsListEmpty(&Thread->ApcListHead[Mode]))
        {
            PKAPC Apc = CONTAINING_RECORD(RemoveHeadList(&Thread->ApcListHead[Mode]), KAPC, ApcListEntry);
            Apc->Inserted = FALSE;
            if (Apc->RundownRoutine)
                Apc->RundownRoutine(Apc);
            else if (Mode == UserMode)
                ExFreePool(Apc);        /* NtQueueApcThread's, and others with no rundown */
        }
    }
}

/* ---------------------------------------------------------------------- */
/* User APCs from user mode                                                */
/* ---------------------------------------------------------------------- */

static VOID NTAPI
QzApcFreeKernelRoutine(PKAPC Apc, PKNORMAL_ROUTINE *NormalRoutine, PVOID *NormalContext,
                       PVOID *SystemArgument1, PVOID *SystemArgument2)
{
    UNREFERENCED_PARAMETER(NormalRoutine);
    UNREFERENCED_PARAMETER(NormalContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    ExFreePool(Apc);
}

NTSTATUS
NTAPI
NtQueueApcThread(IN HANDLE ThreadHandle, IN PKNORMAL_ROUTINE ApcRoutine, IN PVOID NormalContext OPTIONAL,
                 IN PVOID SystemArgument1 OPTIONAL, IN PVOID SystemArgument2 OPTIONAL)
{
    PQZ_PS_THREAD Target;
    PKAPC Apc;
    NTSTATUS Status;

    Status = ObReferenceObjectByHandle(ThreadHandle, THREAD_SET_CONTEXT, PsThreadType, ExGetPreviousMode(),
                                       (PVOID *)&Target, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    Apc = ExAllocatePoolWithTag(NonPagedPool, sizeof(KAPC), 'pAzQ');
    if (!Apc)
    {
        ObDereferenceObject(Target);
        return STATUS_NO_MEMORY;
    }
    KeInitializeApc(Apc, (PRKTHREAD)Target->Thread, OriginalApcEnvironment, QzApcFreeKernelRoutine, NULL,
                    ApcRoutine, UserMode, NormalContext);
    if (!Target->UserThread || !KeInsertQueueApc(Apc, SystemArgument1, SystemArgument2, IO_NO_INCREMENT))
    {
        ExFreePool(Apc);
        Status = STATUS_UNSUCCESSFUL;
    }
    ObDereferenceObject(Target);
    return Status;
}

NTSTATUS
NTAPI
NtAlertThread(IN HANDLE ThreadHandle)
{
    PQZ_PS_THREAD Target;
    PQZ_THREAD Thread;
    KIRQL OldIrql;
    NTSTATUS Status = ObReferenceObjectByHandle(ThreadHandle, THREAD_ALERT, PsThreadType, ExGetPreviousMode(),
                                                (PVOID *)&Target, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    Thread = Target->Thread;
    OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    if (Thread && Thread->State == QzThreadWaiting && Thread->WaitAlertable)
        QzUnwaitThread(Thread, STATUS_ALERTED);
    else if (Thread)
        Thread->Alerted = TRUE;
    QzLowerIrql(OldIrql);
    ObDereferenceObject(Target);
    return STATUS_SUCCESS;
}
