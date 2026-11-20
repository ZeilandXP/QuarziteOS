/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Dispatcher objects (events, mutants, semaphores) and waits
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Objects use the public NT layouts (DISPATCHER_HEADER, KEVENT, KMUTANT,
 * KSEMAPHORE, KWAIT_BLOCK) because drivers allocate them in their own
 * memory. The dispatcher database is protected by raising to
 * DISPATCH_LEVEL (single processor).
 *
 * Waits use direct handoff, as on NT: when an object becomes signaled,
 * QzWaitTest() satisfies waiters on their behalf (consuming the signal of
 * auto-reset objects) before making them ready, so a pulse or an auto-reset
 * event wakes exactly the right number of waiters.
 */

#include "qz.h"

/* DISPATCHER_HEADER.Type values (NT KOBJECTS) */
#define QZ_EVENT_NOTIFICATION       0
#define QZ_EVENT_SYNCHRONIZATION    1
#define QZ_MUTANT                   2
#define QZ_SEMAPHORE                5
#define QZ_TIMER_NOTIFICATION       8
#define QZ_TIMER_SYNCHRONIZATION    9

#undef KeWaitForMutexObject
#undef KeReadStateMutex

static BOOLEAN
QzIsObjectSignaled(PDISPATCHER_HEADER Object, PQZ_THREAD Thread)
{
    if (Object->Type == QZ_MUTANT)
    {
        PKMUTANT Mutant = (PKMUTANT)Object;
        /* Free, or already owned by this thread (recursive acquisition) */
        return Mutant->Header.SignalState > 0 ||
               Mutant->OwnerThread == (PKTHREAD)Thread;
    }
    return Object->SignalState > 0;
}

/* Consume the signal on behalf of Thread */
static VOID
QzSatisfyObject(PDISPATCHER_HEADER Object, PQZ_THREAD Thread)
{
    switch (Object->Type)
    {
        case QZ_EVENT_SYNCHRONIZATION:
        case QZ_TIMER_SYNCHRONIZATION:
            Object->SignalState = 0;
            break;

        case QZ_SEMAPHORE:
            Object->SignalState--;
            break;

        case QZ_MUTANT:
        {
            PKMUTANT Mutant = (PKMUTANT)Object;
            Mutant->Header.SignalState--;
            if (Mutant->Header.SignalState == 0)
                Mutant->OwnerThread = (PKTHREAD)Thread;
            break;
        }

        default:    /* notification event/timer: stays signaled */
            break;
    }
}

/* Remove a thread's wait blocks, set its wait status, make it ready */
VOID
QzUnwaitThread(PQZ_THREAD Thread, NTSTATUS WaitStatus)
{
    ULONG i;

    for (i = 0; i < Thread->WaitCount; i++)
        RemoveEntryList(&Thread->WaitBlockList[i].WaitListEntry);

    Thread->WaitCount = 0;
    Thread->WaitStatus = WaitStatus;
    Thread->TimeoutActive = FALSE;
    QzReadyThread(Thread);
}

static BOOLEAN
QzAllObjectsSignaled(PQZ_THREAD Thread)
{
    ULONG i;
    for (i = 0; i < Thread->WaitCount; i++)
    {
        if (!QzIsObjectSignaled(Thread->WaitBlockList[i].Object, Thread))
            return FALSE;
    }
    return TRUE;
}

/*
 * Object just became (more) signaled: satisfy as many waiters as it can.
 * Called at DISPATCH_LEVEL.
 */
VOID
QzWaitTest(PDISPATCHER_HEADER Object)
{
    PLIST_ENTRY Entry;
    PKWAIT_BLOCK WaitBlock;
    PQZ_THREAD Thread;
    BOOLEAN Progress;
    ULONG i;

    do
    {
        Progress = FALSE;
        for (Entry = Object->WaitListHead.Flink;
             Entry != &Object->WaitListHead && Object->SignalState > 0;
             Entry = Entry->Flink)
        {
            WaitBlock = CONTAINING_RECORD(Entry, KWAIT_BLOCK, WaitListEntry);
            Thread = (PQZ_THREAD)WaitBlock->Thread;

            if (WaitBlock->WaitType == WaitAny)
            {
                if (!QzIsObjectSignaled(Object, Thread))
                    continue;
                QzSatisfyObject(Object, Thread);
                QzUnwaitThread(Thread, STATUS_WAIT_0 + WaitBlock->WaitKey);
            }
            else
            {
                if (!QzAllObjectsSignaled(Thread))
                    continue;
                for (i = 0; i < Thread->WaitCount; i++)
                    QzSatisfyObject(Thread->WaitBlockList[i].Object, Thread);
                QzUnwaitThread(Thread, STATUS_WAIT_0);
            }

            /* The list changed under us: start over */
            Progress = TRUE;
            break;
        }
    } while (Progress && Object->SignalState > 0);
}

/* Clock-driven: fail waits whose timeout has expired (DISPATCH_LEVEL) */
VOID
QzExpireThreadTimeouts(VOID)
{
    PQZ_THREAD First = QzFirstThread(), Thread = First;

    if (!Thread)
        return;
    do
    {
        if (Thread->State == QzThreadWaiting && Thread->TimeoutActive &&
            QzInterruptTime >= Thread->TimeoutInterruptTime)
        {
            /* A plain delay (no objects) completes successfully */
            QzUnwaitThread(Thread, Thread->WaitCount ? STATUS_TIMEOUT : STATUS_SUCCESS);
        }
        Thread = Thread->Next;
    } while (Thread != First);
}

/* Absolute deadline, in interrupt time, for an NT timeout value */
ULONGLONG
QzTimeoutToInterruptTime(LONGLONG Timeout)
{
    LARGE_INTEGER Now;

    if (Timeout < 0)
        return QzInterruptTime + (ULONGLONG)(-Timeout);

    /* Absolute system time */
    KeQuerySystemTime(&Now);
    if (Timeout <= Now.QuadPart)
        return QzInterruptTime;
    return QzInterruptTime + (ULONGLONG)(Timeout - Now.QuadPart);
}

static NTSTATUS
QzWaitCommon(ULONG Count,
             PVOID Object[],
             WAIT_TYPE WaitType,
             PLARGE_INTEGER Timeout,
             PKWAIT_BLOCK WaitBlockArray)
{
    PQZ_THREAD Thread = QzCurrentThread;
    PDISPATCHER_HEADER Header;
    NTSTATUS Status;
    KIRQL OldIrql;
    ULONG i;

    if (Thread->WaitNext)
    {
        /* A KeSetEvent/KeRelease*(..., Wait=TRUE) left us at DISPATCH */
        Thread->WaitNext = FALSE;
        OldIrql = Thread->WaitIrql;
    }
    else
    {
        OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    }

    /* Can the wait be satisfied right now? */
    if (WaitType == WaitAny)
    {
        for (i = 0; i < Count; i++)
        {
            Header = Object[i];
            if (QzIsObjectSignaled(Header, Thread))
            {
                QzSatisfyObject(Header, Thread);
                QzLowerIrql(OldIrql);
                return STATUS_WAIT_0 + i;
            }
        }
    }
    else if (Count > 0)
    {
        for (i = 0; i < Count && QzIsObjectSignaled(Object[i], Thread); i++);
        if (i == Count)
        {
            for (i = 0; i < Count; i++)
                QzSatisfyObject(Object[i], Thread);
            QzLowerIrql(OldIrql);
            return STATUS_WAIT_0;
        }
    }

    if (Timeout && Timeout->QuadPart == 0)
    {
        QzLowerIrql(OldIrql);
        return Count ? STATUS_TIMEOUT : STATUS_SUCCESS;
    }

    /* Blocking is only legal below DISPATCH_LEVEL */
    if (OldIrql >= DISPATCH_LEVEL)
        KeBugCheckEx(IRQL_NOT_LESS_OR_EQUAL, (ULONG_PTR)Object, OldIrql, 0, 0);

    /* Queue a wait block on every object */
    Thread->WaitBlockList = WaitBlockArray;
    Thread->WaitCount = Count;
    for (i = 0; i < Count; i++)
    {
        WaitBlockArray[i].Thread = (PKTHREAD)Thread;
        WaitBlockArray[i].Object = Object[i];
        WaitBlockArray[i].WaitKey = (USHORT)i;
        WaitBlockArray[i].WaitType = (UCHAR)WaitType;
        WaitBlockArray[i].NextWaitBlock = (i + 1 < Count) ? &WaitBlockArray[i + 1]
                                                         : &WaitBlockArray[0];
        InsertTailList(&((PDISPATCHER_HEADER)Object[i])->WaitListHead,
                       &WaitBlockArray[i].WaitListEntry);
    }

    Thread->TimeoutActive = (Timeout != NULL);
    if (Timeout)
        Thread->TimeoutInterruptTime = QzTimeoutToInterruptTime(Timeout->QuadPart);

    Thread->WaitStatus = STATUS_PENDING;
    Thread->State = QzThreadWaiting;

    /* Sleep; QzWaitTest or the timeout code makes us ready again */
    QzSwapThread();

    Status = Thread->WaitStatus;
    QzLowerIrql(OldIrql);
    return Status;
}

NTSTATUS
QzDelay(LONGLONG Interval100ns)
{
    LARGE_INTEGER Timeout;
    Timeout.QuadPart = Interval100ns;
    return QzWaitCommon(0, NULL, WaitAny, &Timeout, QzCurrentThread->WaitBlock);
}

/* ---------------------------------------------------------------------- */
/* Wait exports                                                            */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
KeWaitForSingleObject(IN PVOID Object,
                      IN KWAIT_REASON WaitReason,
                      IN KPROCESSOR_MODE WaitMode,
                      IN BOOLEAN Alertable,
                      IN PLARGE_INTEGER Timeout OPTIONAL)
{
    UNREFERENCED_PARAMETER(WaitReason);
    UNREFERENCED_PARAMETER(WaitMode);
    UNREFERENCED_PARAMETER(Alertable);
    return QzWaitCommon(1, &Object, WaitAny, Timeout, QzCurrentThread->WaitBlock);
}

NTSTATUS
NTAPI
KeWaitForMultipleObjects(IN ULONG Count,
                         IN PVOID Object[],
                         IN WAIT_TYPE WaitType,
                         IN KWAIT_REASON WaitReason,
                         IN KPROCESSOR_MODE WaitMode,
                         IN BOOLEAN Alertable,
                         IN PLARGE_INTEGER Timeout OPTIONAL,
                         OUT PKWAIT_BLOCK WaitBlockArray OPTIONAL)
{
    UNREFERENCED_PARAMETER(WaitReason);
    UNREFERENCED_PARAMETER(WaitMode);
    UNREFERENCED_PARAMETER(Alertable);

    if (WaitBlockArray == NULL)
    {
        if (Count > THREAD_WAIT_OBJECTS)
            KeBugCheckEx(MAXIMUM_WAIT_OBJECTS_EXCEEDED, Count, 0, 0, 0);
        WaitBlockArray = QzCurrentThread->WaitBlock;
    }
    else if (Count > MAXIMUM_WAIT_OBJECTS)
    {
        KeBugCheckEx(MAXIMUM_WAIT_OBJECTS_EXCEEDED, Count, 0, 0, 0);
    }

    return QzWaitCommon(Count, Object, WaitType, Timeout, WaitBlockArray);
}

/* Shared tail of KeSetEvent/KeRelease*: honour the Wait parameter */
static VOID
QzExitSignal(KIRQL OldIrql, BOOLEAN Wait)
{
    if (Wait)
    {
        /* Stay at DISPATCH; the caller's next KeWaitXxx picks it up */
        QzCurrentThread->WaitNext = TRUE;
        QzCurrentThread->WaitIrql = OldIrql;
        return;
    }
    QzLowerIrql(OldIrql);
}

static VOID
QzInitializeHeader(PDISPATCHER_HEADER Header, UCHAR Type, ULONG Size, LONG SignalState)
{
    RtlZeroMemory(Header, sizeof(*Header));
    Header->Type = Type;
    Header->Size = (UCHAR)(Size / sizeof(LONG));
    Header->SignalState = SignalState;
    InitializeListHead(&Header->WaitListHead);
}

/* ---------------------------------------------------------------------- */
/* Events                                                                  */
/* ---------------------------------------------------------------------- */

VOID
NTAPI
KeInitializeEvent(OUT PRKEVENT Event,
                  IN EVENT_TYPE Type,
                  IN BOOLEAN State)
{
    QzInitializeHeader(&Event->Header,
                       Type == SynchronizationEvent ? QZ_EVENT_SYNCHRONIZATION
                                                    : QZ_EVENT_NOTIFICATION,
                       sizeof(KEVENT), State ? 1 : 0);
}

LONG
NTAPI
KeSetEvent(IN OUT PRKEVENT Event,
           IN KPRIORITY Increment,
           IN BOOLEAN Wait)
{
    KIRQL OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    LONG Previous = Event->Header.SignalState;

    UNREFERENCED_PARAMETER(Increment);

    Event->Header.SignalState = 1;
    if (Previous == 0)
        QzWaitTest(&Event->Header);

    QzExitSignal(OldIrql, Wait);
    return Previous;
}

LONG
NTAPI
KeResetEvent(IN OUT PRKEVENT Event)
{
    KIRQL OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    LONG Previous = Event->Header.SignalState;
    Event->Header.SignalState = 0;
    QzLowerIrql(OldIrql);
    return Previous;
}

VOID
NTAPI
KeClearEvent(IN OUT PRKEVENT Event)
{
    Event->Header.SignalState = 0;
}

LONG
NTAPI
KePulseEvent(IN OUT PRKEVENT Event,
             IN KPRIORITY Increment,
             IN BOOLEAN Wait)
{
    KIRQL OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    LONG Previous = Event->Header.SignalState;

    UNREFERENCED_PARAMETER(Increment);

    /* Release the current waiters, then return to non-signaled */
    if (Previous == 0)
    {
        Event->Header.SignalState = 1;
        QzWaitTest(&Event->Header);
    }
    Event->Header.SignalState = 0;

    QzExitSignal(OldIrql, Wait);
    return Previous;
}

LONG
NTAPI
KeReadStateEvent(IN PRKEVENT Event)
{
    return Event->Header.SignalState;
}

/* ---------------------------------------------------------------------- */
/* Mutants / mutexes                                                       */
/* ---------------------------------------------------------------------- */

VOID
NTAPI
KeInitializeMutant(IN PKMUTANT Mutant,
                   IN BOOLEAN InitialOwner)
{
    QzInitializeHeader(&Mutant->Header, QZ_MUTANT, sizeof(KMUTANT), 1);
    Mutant->OwnerThread = NULL;
    if (InitialOwner)
    {
        Mutant->Header.SignalState = 0;
        Mutant->OwnerThread = (PKTHREAD)QzCurrentThread;
    }
    Mutant->Abandoned = FALSE;
    Mutant->ApcDisable = 0;
    InitializeListHead(&Mutant->MutantListEntry);
}

VOID
NTAPI
KeInitializeMutex(IN PKMUTEX Mutex,
                  IN ULONG Level)
{
    UNREFERENCED_PARAMETER(Level);
    KeInitializeMutant(Mutex, FALSE);
    Mutex->ApcDisable = 1;
}

LONG
NTAPI
KeReleaseMutant(IN PKMUTANT Mutant,
                IN KPRIORITY Increment,
                IN BOOLEAN Abandon,
                IN BOOLEAN Wait)
{
    KIRQL OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    LONG Previous = Mutant->Header.SignalState;

    UNREFERENCED_PARAMETER(Increment);

    if (Abandon)
    {
        Mutant->Header.SignalState = 1;
        Mutant->Abandoned = TRUE;
    }
    else
    {
        if (Mutant->OwnerThread != (PKTHREAD)QzCurrentThread)
        {
            QzLowerIrql(OldIrql);
            KeBugCheckEx(THREAD_NOT_MUTEX_OWNER, (ULONG_PTR)Mutant->OwnerThread,
                         (ULONG_PTR)Mutant, (ULONG_PTR)QzCurrentThread, 0);
        }
        Mutant->Header.SignalState++;
    }

    if (Mutant->Header.SignalState == 1)
    {
        Mutant->OwnerThread = NULL;
        QzWaitTest(&Mutant->Header);
    }

    QzExitSignal(OldIrql, Wait);
    return Previous;
}

LONG
NTAPI
KeReleaseMutex(IN PKMUTEX Mutex,
               IN BOOLEAN Wait)
{
    return KeReleaseMutant(Mutex, 1, FALSE, Wait);
}

LONG
NTAPI
KeReadStateMutant(IN PKMUTANT Mutant)
{
    return Mutant->Header.SignalState;
}

/* ---------------------------------------------------------------------- */
/* Semaphores                                                              */
/* ---------------------------------------------------------------------- */

VOID
NTAPI
KeInitializeSemaphore(IN PRKSEMAPHORE Semaphore,
                      IN LONG Count,
                      IN LONG Limit)
{
    QzInitializeHeader(&Semaphore->Header, QZ_SEMAPHORE, sizeof(KSEMAPHORE), Count);
    Semaphore->Limit = Limit;
}

LONG
NTAPI
KeReleaseSemaphore(IN OUT PRKSEMAPHORE Semaphore,
                   IN KPRIORITY Increment,
                   IN LONG Adjustment,
                   IN BOOLEAN Wait)
{
    KIRQL OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    LONG Previous = Semaphore->Header.SignalState;

    UNREFERENCED_PARAMETER(Increment);

    if (Adjustment <= 0 ||
        Previous + Adjustment > Semaphore->Limit ||
        Previous + Adjustment < Previous)
    {
        /* NT raises STATUS_SEMAPHORE_LIMIT_EXCEEDED; no SEH here yet */
        QzLowerIrql(OldIrql);
        KeBugCheckEx(KMODE_EXCEPTION_NOT_HANDLED,
                     (ULONG_PTR)STATUS_SEMAPHORE_LIMIT_EXCEEDED,
                     (ULONG_PTR)Semaphore, Previous, Adjustment);
    }

    Semaphore->Header.SignalState += Adjustment;
    if (Previous == 0)
        QzWaitTest(&Semaphore->Header);

    QzExitSignal(OldIrql, Wait);
    return Previous;
}

LONG
NTAPI
KeReadStateSemaphore(IN PRKSEMAPHORE Semaphore)
{
    return Semaphore->Header.SignalState;
}
