/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     IRQL (through the HAL), the DISPATCH software interrupt,
 *              DPCs and spinlocks
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * IRQL belongs to the HAL, as on NT x86: KfRaiseIrql/KfLowerIrql keep it
 * in the PCR (FS:0x24) and, on the PIC HAL, mask device interrupts lazily.
 * The kernel asks for the DISPATCH_LEVEL software interrupt with
 * HalRequestSoftwareInterrupt; the HAL delivers it -- immediately if the
 * IRQL is low enough, otherwise when it drops -- by calling the kernel's
 * KiDispatchInterrupt at DISPATCH_LEVEL with interrupts enabled. That is
 * where timers expire, DPCs run and threads are switched.
 */

#include "qz.h"

#pragma GCC diagnostic ignored "-Wframe-address"

#define QZ_DPC_OBJECT           0x13
#define QZ_THREADED_DPC_OBJECT  0x18

#undef KeAcquireSpinLockAtDpcLevel
#undef KeReleaseSpinLockFromDpcLevel
#undef KeInitializeSpinLock
#undef KeTestSpinLock
#undef KeGetCurrentIrql

KIRQL
QzGetCurrentIrql(VOID)
{
    return QzPcr.Irql;
}

KIRQL
QzRaiseIrql(KIRQL NewIrql)
{
    if (NewIrql < QzPcr.Irql)
    {
        DbgPrint("qzkrnl: raise %u -> %u on thread '%s' from %p <- %p\n",
                 QzPcr.Irql, NewIrql, QzCurrentThread ? QzCurrentThread->Name : "?",
                 __builtin_return_address(0), __builtin_return_address(1));
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL, QzPcr.Irql, NewIrql,
                     (ULONG_PTR)__builtin_return_address(0), 0);
    }
    return KfRaiseIrql(NewIrql);
}

VOID
QzLowerIrql(KIRQL NewIrql)
{
    if (NewIrql > QzPcr.Irql)
        KeBugCheckEx(IRQL_NOT_LESS_OR_EQUAL, QzPcr.Irql, NewIrql,
                     (ULONG_PTR)__builtin_return_address(0), 0);
    KfLowerIrql(NewIrql);
}

/*
 * The DISPATCH_LEVEL software interrupt, called by the HAL at
 * DISPATCH_LEVEL with interrupts enabled: retire timers and DPCs, then
 * switch threads if a reschedule is pending.
 */
VOID
NTAPI
KiDispatchInterrupt(VOID)
{
    QzPrcb->DispatchInterrupts++;
    QzRetireDpcs();

    if (QzPrcb->DispatchPending)
    {
        QzPrcb->DispatchPending = FALSE;
        QzSwapThread();
    }
}

/* APC software interrupt: no APCs yet (they arrive with processes, B6) */
VOID
NTAPI
KiDeliverApc(IN KPROCESSOR_MODE DeliveryMode,
             IN PKEXCEPTION_FRAME ExceptionFrame,
             IN PKTRAP_FRAME TrapFrame)
{
    UNREFERENCED_PARAMETER(DeliveryMode);
    UNREFERENCED_PARAMETER(ExceptionFrame);
    UNREFERENCED_PARAMETER(TrapFrame);
}

/* Run expired timers and queued DPCs. Called at DISPATCH_LEVEL. */
VOID
QzRetireDpcs(VOID)
{
    PLIST_ENTRY Entry;
    PKDPC Dpc;
    PKDEFERRED_ROUTINE Routine;
    PVOID Context, Arg1, Arg2;
    ULONG Flags;

    for (;;)
    {
        Flags = QzSaveFlagsAndDisable();

        if (QzPrcb->TimerPending)
        {
            QzPrcb->TimerPending = FALSE;
            QzRestoreFlags(Flags);
            QzExpireTimers();
            QzExpireThreadTimeouts();
            continue;
        }

        if (IsListEmpty(&QzPrcb->DpcListHead))
        {
            QzPrcb->DpcPending = FALSE;
            QzRestoreFlags(Flags);
            return;
        }

        Entry = RemoveHeadList(&QzPrcb->DpcListHead);
        Dpc = CONTAINING_RECORD(Entry, KDPC, DpcListEntry);
        Routine = Dpc->DeferredRoutine;
        Context = Dpc->DeferredContext;
        Arg1 = Dpc->SystemArgument1;
        Arg2 = Dpc->SystemArgument2;
        Dpc->DpcData = NULL;
        QzPrcb->DpcQueueDepth--;
        QzRestoreFlags(Flags);

        QzPrcb->DpcRoutineActive = TRUE;
        Routine(Dpc, Context, Arg1, Arg2);
        QzPrcb->DpcRoutineActive = FALSE;
        QzPrcb->DpcCount++;

        if (QzPcr.Irql != DISPATCH_LEVEL)
            KeBugCheckEx(IRQL_UNEXPECTED_VALUE, QzPcr.Irql, DISPATCH_LEVEL,
                         (ULONG_PTR)Routine, 0);
    }
}

KIRQL
QzAcquireSpinLock(PKSPIN_LOCK SpinLock)
{
    KIRQL OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    QzKeAcquireSpinLockAtDpcLevel(SpinLock);
    return OldIrql;
}

VOID
QzReleaseSpinLock(PKSPIN_LOCK SpinLock, KIRQL OldIrql)
{
    QzKeReleaseSpinLockFromDpcLevel(SpinLock);
    QzLowerIrql(OldIrql);
}

/* ---------------------------------------------------------------------- */
/* DPC exports                                                             */
/* ---------------------------------------------------------------------- */

VOID
NTAPI
KeInitializeDpc(IN PKDPC Dpc,
                IN PKDEFERRED_ROUTINE DeferredRoutine,
                IN PVOID DeferredContext)
{
    Dpc->Type = QZ_DPC_OBJECT;
    Dpc->Importance = MediumImportance;
    Dpc->Number = 0;
    Dpc->DeferredRoutine = DeferredRoutine;
    Dpc->DeferredContext = DeferredContext;
    Dpc->DpcData = NULL;
}

VOID
NTAPI
KeInitializeThreadedDpc(IN PKDPC Dpc,
                        IN PKDEFERRED_ROUTINE DeferredRoutine,
                        IN PVOID DeferredContext)
{
    /* Threaded DPCs run as normal DPCs (allowed by the DDK contract) */
    KeInitializeDpc(Dpc, DeferredRoutine, DeferredContext);
    Dpc->Type = QZ_THREADED_DPC_OBJECT;
}

BOOLEAN
NTAPI
KeInsertQueueDpc(IN PKDPC Dpc,
                 IN PVOID SystemArgument1,
                 IN PVOID SystemArgument2)
{
    ULONG Flags = QzSaveFlagsAndDisable();

    if (Dpc->DpcData != NULL)
    {
        QzRestoreFlags(Flags);
        return FALSE;               /* already queued */
    }

    Dpc->SystemArgument1 = SystemArgument1;
    Dpc->SystemArgument2 = SystemArgument2;
    Dpc->DpcData = &QzPrcb->DpcListHead;   /* non-NULL: queued */
    if (Dpc->Importance == HighImportance)
        InsertHeadList(&QzPrcb->DpcListHead, &Dpc->DpcListEntry);
    else
        InsertTailList(&QzPrcb->DpcListHead, &Dpc->DpcListEntry);
    QzPrcb->DpcQueueDepth++;
    QzPrcb->DpcPending = TRUE;
    QzRestoreFlags(Flags);

    /* Runs right away below DISPATCH_LEVEL, otherwise when the IRQL drops */
    QzRequestSoftwareInterrupt();
    return TRUE;
}

BOOLEAN
NTAPI
KeRemoveQueueDpc(IN PKDPC Dpc)
{
    ULONG Flags = QzSaveFlagsAndDisable();
    BOOLEAN Queued = (Dpc->DpcData != NULL);

    if (Queued)
    {
        RemoveEntryList(&Dpc->DpcListEntry);
        Dpc->DpcData = NULL;
        QzPrcb->DpcQueueDepth--;
    }
    QzRestoreFlags(Flags);
    return Queued;
}

VOID
NTAPI
KeSetImportanceDpc(IN PKDPC Dpc,
                   IN KDPC_IMPORTANCE Importance)
{
    Dpc->Importance = (UCHAR)Importance;
}

VOID
NTAPI
KeSetTargetProcessorDpc(IN PKDPC Dpc,
                        IN CCHAR Number)
{
    /* Single processor: remember it, run it here */
    Dpc->Number = (USHORT)(Number + 32);
}

BOOLEAN
NTAPI
KeIsExecutingDpc(VOID)
{
    return QzPrcb->DpcRoutineActive;
}

VOID
NTAPI
KeFlushQueuedDpcs(VOID)
{
    if (QzPrcb->DpcPending && QzGetCurrentIrql() < DISPATCH_LEVEL)
        QzRequestSoftwareInterrupt();
}

/* ---------------------------------------------------------------------- */
/* Spinlock exports. Single processor: a lock that is already held when    */
/* acquired at DISPATCH_LEVEL can never be released -- report it.          */
/* ---------------------------------------------------------------------- */

static __inline BOOLEAN
QzTrySetLock(PKSPIN_LOCK SpinLock)
{
    return (__atomic_fetch_or((volatile ULONG_PTR *)SpinLock, 1, __ATOMIC_ACQUIRE) & 1) == 0;
}

static __inline VOID
QzClearLock(PKSPIN_LOCK SpinLock)
{
    __atomic_store_n((volatile ULONG_PTR *)SpinLock, 0, __ATOMIC_RELEASE);
}

VOID
NTAPI
QzKeInitializeSpinLock(OUT PKSPIN_LOCK SpinLock)
{
    *SpinLock = 0;
}

VOID
NTAPI
QzKeAcquireSpinLockAtDpcLevel(IN OUT PKSPIN_LOCK SpinLock)
{
    if (!QzTrySetLock(SpinLock))
        KeBugCheckEx(SPIN_LOCK_ALREADY_OWNED, (ULONG_PTR)SpinLock, 0, 0, 0);
}

VOID
NTAPI
QzKeReleaseSpinLockFromDpcLevel(IN OUT PKSPIN_LOCK SpinLock)
{
    if (!(*SpinLock & 1))
        KeBugCheckEx(SPIN_LOCK_NOT_OWNED, (ULONG_PTR)SpinLock, 0, 0, 0);
    QzClearLock(SpinLock);
}

VOID
FASTCALL
KefAcquireSpinLockAtDpcLevel(IN OUT PKSPIN_LOCK SpinLock)
{
    QzKeAcquireSpinLockAtDpcLevel(SpinLock);
}

VOID
FASTCALL
KefReleaseSpinLockFromDpcLevel(IN OUT PKSPIN_LOCK SpinLock)
{
    QzKeReleaseSpinLockFromDpcLevel(SpinLock);
}

BOOLEAN
FASTCALL
KeTryToAcquireSpinLockAtDpcLevel(IN OUT PKSPIN_LOCK SpinLock)
{
    return QzTrySetLock(SpinLock);
}

BOOLEAN
FASTCALL
KeTestSpinLock(IN PKSPIN_LOCK SpinLock)
{
    /* TRUE if the lock is free */
    return (*(volatile ULONG_PTR *)SpinLock & 1) == 0;
}

VOID
FASTCALL
KeAcquireInStackQueuedSpinLockAtDpcLevel(IN OUT PKSPIN_LOCK SpinLock,
                                         IN PKLOCK_QUEUE_HANDLE LockHandle)
{
    LockHandle->LockQueue.Next = NULL;
    LockHandle->LockQueue.Lock = SpinLock;
    QzKeAcquireSpinLockAtDpcLevel(SpinLock);
}

VOID
FASTCALL
KeReleaseInStackQueuedSpinLockFromDpcLevel(IN PKLOCK_QUEUE_HANDLE LockHandle)
{
    QzKeReleaseSpinLockFromDpcLevel(LockHandle->LockQueue.Lock);
}
