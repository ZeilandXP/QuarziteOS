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
#define QZ_QUEUE                    4
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
        case QZ_QUEUE:                  /* the waiter takes an entry: KeRemoveQueue */
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

NTSTATUS QzApcWaitCheck(PQZ_THREAD Thread, BOOLEAN Alertable, KPROCESSOR_MODE WaitMode, KIRQL OldIrql);

static NTSTATUS
QzWaitCommon(ULONG Count,
             PVOID Object[],
             WAIT_TYPE WaitType,
             BOOLEAN Alertable,
             KPROCESSOR_MODE WaitMode,
             PLARGE_INTEGER Timeout,
             PKWAIT_BLOCK WaitBlockArray)
{
    PQZ_THREAD Thread = QzCurrentThread;
    PDISPATCHER_HEADER Header;
    ULONGLONG Deadline = 0;
    LARGE_INTEGER Remaining;
    NTSTATUS Status;
    KIRQL OldIrql;
    ULONG i;

    /* A kernel APC breaks the wait and it starts again: keep the deadline */
    if (Timeout && Timeout->QuadPart != 0)
        Deadline = QzTimeoutToInterruptTime(Timeout->QuadPart);

Restart:
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

    /* Pending APCs or an alert end the wait before it starts */
    Status = QzApcWaitCheck(Thread, Alertable, WaitMode, OldIrql);
    if (Status == STATUS_KERNEL_APC)
    {
        HalRequestSoftwareInterrupt(APC_LEVEL);
        QzLowerIrql(OldIrql);           /* they run here */
        goto Restart;
    }
    if (Status != STATUS_SUCCESS)
    {
        QzLowerIrql(OldIrql);
        return Status;
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
        Thread->TimeoutInterruptTime = Deadline;

    Thread->WaitStatus = STATUS_PENDING;
    Thread->WaitAlertable = Alertable;
    Thread->WaitMode = WaitMode;
    Thread->WaitOldIrql = OldIrql;
    Thread->State = QzThreadWaiting;

    /* Sleep; QzWaitTest, the timeout code or an APC makes us ready again */
    QzSwapThread();

    Status = Thread->WaitStatus;
    Thread->WaitAlertable = FALSE;
    if (Status == STATUS_KERNEL_APC)
    {
        HalRequestSoftwareInterrupt(APC_LEVEL);
        QzLowerIrql(OldIrql);           /* the APCs run here, then the wait again */
        if (Timeout)
        {
            if (QzInterruptTime >= Deadline)
                return Count ? STATUS_TIMEOUT : STATUS_SUCCESS;
            Remaining.QuadPart = -(LONGLONG)(Deadline - QzInterruptTime);
            Timeout = &Remaining;
        }
        goto Restart;
    }
    QzLowerIrql(OldIrql);
    return Status;
}

NTSTATUS
QzDelay(LONGLONG Interval100ns)
{
    LARGE_INTEGER Timeout;
    Timeout.QuadPart = Interval100ns;
    return QzWaitCommon(0, NULL, WaitAny, FALSE, KernelMode, &Timeout, QzCurrentThread->WaitBlock);
}

NTSTATUS
QzDelayEx(LONGLONG Interval100ns, BOOLEAN Alertable, KPROCESSOR_MODE WaitMode)
{
    LARGE_INTEGER Timeout;
    Timeout.QuadPart = Interval100ns;
    return QzWaitCommon(0, NULL, WaitAny, Alertable, WaitMode, &Timeout, QzCurrentThread->WaitBlock);
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
    return QzWaitCommon(1, &Object, WaitAny, Alertable, WaitMode, Timeout, QzCurrentThread->WaitBlock);
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

    return QzWaitCommon(Count, Object, WaitType, Alertable, WaitMode, Timeout, WaitBlockArray);
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

/* ---------------------------------------------------------------------- */
/* Queues (I/O completion ports, worker queues)                            */
/* ---------------------------------------------------------------------- */

/*
 * The signal state counts the entries. A waiter woken by an insert has
 * already taken one (QzSatisfyObject), so the count and the list agree.
 * The concurrency limit is not enforced: one processor.
 */
VOID
NTAPI
KeInitializeQueue(OUT PKQUEUE Queue, IN ULONG Count OPTIONAL)
{
    QzInitializeHeader(&Queue->Header, QZ_QUEUE, sizeof(KQUEUE), 0);
    InitializeListHead(&Queue->EntryListHead);
    InitializeListHead(&Queue->ThreadListHead);
    Queue->CurrentCount = 0;
    Queue->MaximumCount = Count ? Count : 1;
}

static LONG
QzInsertQueue(PKQUEUE Queue, PLIST_ENTRY Entry, BOOLEAN Head)
{
    KIRQL OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    LONG Previous = Queue->Header.SignalState;

    if (Head)
        InsertHeadList(&Queue->EntryListHead, Entry);
    else
        InsertTailList(&Queue->EntryListHead, Entry);
    Queue->Header.SignalState++;
    QzWaitTest(&Queue->Header);
    QzLowerIrql(OldIrql);
    return Previous;
}

LONG
NTAPI
KeInsertQueue(IN PKQUEUE Queue, IN PLIST_ENTRY Entry)
{
    return QzInsertQueue(Queue, Entry, FALSE);
}

LONG
NTAPI
KeInsertHeadQueue(IN PKQUEUE Queue, IN PLIST_ENTRY Entry)
{
    return QzInsertQueue(Queue, Entry, TRUE);
}

PLIST_ENTRY
NTAPI
KeRemoveQueue(IN PKQUEUE Queue, IN KPROCESSOR_MODE WaitMode, IN PLARGE_INTEGER Timeout OPTIONAL)
{
    PLIST_ENTRY Entry;
    NTSTATUS Status;
    KIRQL OldIrql = QzRaiseIrql(DISPATCH_LEVEL);

    if (Queue->Header.SignalState > 0)
    {
        Queue->Header.SignalState--;
        Entry = RemoveHeadList(&Queue->EntryListHead);
        QzLowerIrql(OldIrql);
        Entry->Flink = NULL;
        return Entry;
    }
    QzLowerIrql(OldIrql);

    Status = KeWaitForSingleObject(Queue, WrQueue, WaitMode, FALSE, Timeout);
    if (Status != STATUS_WAIT_0)
        return (PLIST_ENTRY)(ULONG_PTR)Status;      /* STATUS_TIMEOUT, STATUS_USER_APC, ... */
    OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    Entry = RemoveHeadList(&Queue->EntryListHead);
    QzLowerIrql(OldIrql);
    Entry->Flink = NULL;
    return Entry;
}

LONG
NTAPI
KeReadStateQueue(IN PKQUEUE Queue)
{
    return Queue->Header.SignalState;
}

/* Empty the queue: the entries, still linked to each other, or NULL */
PLIST_ENTRY
NTAPI
KeRundownQueue(IN PKQUEUE Queue)
{
    KIRQL OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    PLIST_ENTRY First = NULL;

    if (!IsListEmpty(&Queue->EntryListHead))
    {
        First = Queue->EntryListHead.Flink;
        RemoveEntryList(&Queue->EntryListHead);
    }
    InitializeListHead(&Queue->EntryListHead);
    Queue->Header.SignalState = 0;
    QzLowerIrql(OldIrql);
    return First;
}
