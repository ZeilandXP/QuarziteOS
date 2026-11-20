/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     IRQL, the DISPATCH_LEVEL software interrupt, DPCs, spinlocks
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * IRQL model (single processor):
 *  - The current IRQL lives in the PCR (FS:0x24), where drivers and the HAL
 *    expect it.
 *  - Above DISPATCH_LEVEL, hardware interrupts are masked (IF=0).
 *  - At DISPATCH_LEVEL and above, the "DISPATCH software interrupt" is held
 *    pending: no DPC runs and no thread switch happens. Lowering the IRQL
 *    below DISPATCH_LEVEL delivers it -- pending timers and DPCs are retired
 *    and a pending reschedule takes place -- exactly like NT's KiDispatch-
 *    Interrupt, but without a hardware software-interrupt controller.
 *  - Hardware interrupts (ke/i386/trap.c) deliver it on their way out when
 *    the interrupted code ran below DISPATCH_LEVEL.
 *
 * NOTE: on x86 NT, KeRaiseIrql/KeLowerIrql/KeAcquireSpinLock are HAL.DLL
 * exports. qzkrnl keeps these internal until it hosts the HAL (stage B2).
 */

#include "qz.h"

#define QZ_DPC_OBJECT           0x13
#define QZ_THREADED_DPC_OBJECT  0x18

#undef KeAcquireSpinLockAtDpcLevel
#undef KeReleaseSpinLockFromDpcLevel
#undef KeInitializeSpinLock
#undef KeTestSpinLock

#define QZ_IF 0x200

KIRQL
QzGetCurrentIrql(VOID)
{
    return QzPcr.Pcr.Irql;
}

KIRQL
QzRaiseIrql(KIRQL NewIrql)
{
    KIRQL OldIrql = QzPcr.Pcr.Irql;

    if (NewIrql < OldIrql)
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL, OldIrql, NewIrql, 0, 0);

    if (NewIrql > DISPATCH_LEVEL && OldIrql <= DISPATCH_LEVEL)
        __asm__ __volatile__("cli" : : : "memory");

    QzPcr.Pcr.Irql = NewIrql;
    return OldIrql;
}

static BOOLEAN
QzSoftwareInterruptPending(VOID)
{
    return QzPrcb->DpcPending || QzPrcb->TimerPending;
}

VOID
QzLowerIrql(KIRQL NewIrql)
{
    KIRQL OldIrql = QzPcr.Pcr.Irql;
    ULONG Flags;

    if (NewIrql > OldIrql)
        KeBugCheckEx(IRQL_NOT_LESS_OR_EQUAL, OldIrql, NewIrql, 0, 0);

    /* Leaving the device levels re-enables hardware interrupts */
    if (OldIrql > DISPATCH_LEVEL && NewIrql <= DISPATCH_LEVEL)
    {
        QzPcr.Pcr.Irql = (NewIrql < DISPATCH_LEVEL) ? DISPATCH_LEVEL : NewIrql;
        __asm__ __volatile__("sti" : : : "memory");
    }

    if (NewIrql >= DISPATCH_LEVEL)
    {
        QzPcr.Pcr.Irql = NewIrql;
        return;
    }

    /* Deliver the DISPATCH software interrupt: retire timers and DPCs... */
    Flags = QzSaveFlagsAndDisable();
    while (QzSoftwareInterruptPending())
    {
        QzPcr.Pcr.Irql = DISPATCH_LEVEL;
        QzRestoreFlags(Flags);
        QzRetireDpcs();
        Flags = QzSaveFlagsAndDisable();
    }
    QzPcr.Pcr.Irql = NewIrql;

    /* ...then reschedule if asked to (needs interrupts on: it's an int) */
    if (QzPrcb->DispatchPending && QzCurrentThread && (Flags & QZ_IF))
    {
        QzRestoreFlags(Flags);
        QzSwapThread();
        return;
    }
    QzRestoreFlags(Flags);
}

/*
 * Interrupt epilogue: the interrupted context ran at Irql < DISPATCH_LEVEL.
 * Called with interrupts disabled; returns the frame to resume.
 */
PQZ_TRAP_FRAME
QzDispatchSoftwareInterrupt(PQZ_TRAP_FRAME Frame, KIRQL Irql)
{
    for (;;)
    {
        if (QzSoftwareInterruptPending())
        {
            QzPcr.Pcr.Irql = DISPATCH_LEVEL;
            __asm__ __volatile__("sti" : : : "memory");
            QzRetireDpcs();
            __asm__ __volatile__("cli" : : : "memory");
            continue;
        }
        if (QzPrcb->DispatchPending && QzCurrentThread)
        {
            Frame = QzSelectNextThread(Frame, Irql);
            Irql = QzPcr.Pcr.Irql;
            if (Irql >= DISPATCH_LEVEL)
                return Frame;      /* resumed a thread blocked at DISPATCH */
            continue;
        }
        break;
    }
    QzPcr.Pcr.Irql = Irql;
    return Frame;
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

        if (QzPcr.Pcr.Irql != DISPATCH_LEVEL)
            KeBugCheckEx(IRQL_UNEXPECTED_VALUE, QzPcr.Pcr.Irql, DISPATCH_LEVEL,
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

    /* Below DISPATCH_LEVEL the software interrupt fires right away */
    if (QzGetCurrentIrql() < DISPATCH_LEVEL)
        QzLowerIrql(QzRaiseIrql(DISPATCH_LEVEL));

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
        QzLowerIrql(QzRaiseIrql(DISPATCH_LEVEL));
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
