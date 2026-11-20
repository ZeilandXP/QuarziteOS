/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Self-test of IRQL, DPCs, spinlocks, dispatcher objects,
 *              waits and timers through the exported NT interfaces
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Run with the shell command 'synctest'. Every main-thread wait has a
 * timeout, so a broken primitive shows up as FAIL instead of a hang.
 */

#include "qz.h"

static ULONG QzPassed, QzFailed;

static VOID
QzCheck(PCSTR Name, BOOLEAN Condition)
{
    if (Condition)
        QzPassed++;
    else
        QzFailed++;
    QzPrint("  [%s] %s\n", Condition ? "PASS" : "FAIL", Name);
}

static LARGE_INTEGER
QzMs(LONG Milliseconds)
{
    LARGE_INTEGER Interval;
    Interval.QuadPart = -(LONGLONG)Milliseconds * 10000;   /* relative */
    return Interval;
}

static VOID
QzSleepMs(LONG Milliseconds)
{
    LARGE_INTEGER Interval = QzMs(Milliseconds);
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

static ULONG
QzElapsedMs(ULONGLONG Start)
{
    return (ULONG)((KeQueryInterruptTime() - Start) / 10000);
}

/* Shared state for worker threads */
static KEVENT QzGo;
static KSEMAPHORE QzDone;
static volatile LONG QzWoken;

static VOID NTAPI
QzWaitOnGoWorker(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    KeWaitForSingleObject(&QzGo, Executive, KernelMode, FALSE, NULL);
    InterlockedIncrement(&QzWoken);
    KeReleaseSemaphore(&QzDone, 0, 1, FALSE);
}

static BOOLEAN
QzWaitDone(ULONG Count, LONG TimeoutMs)
{
    LARGE_INTEGER Timeout = QzMs(TimeoutMs);
    ULONG i;
    for (i = 0; i < Count; i++)
    {
        if (KeWaitForSingleObject(&QzDone, Executive, KernelMode, FALSE, &Timeout) != STATUS_WAIT_0)
            return FALSE;
    }
    return TRUE;
}

static VOID
QzSpawn(PCSTR Name, PQZ_THREAD_ROUTINE Routine, PVOID Context, ULONG Count)
{
    ULONG i;
    for (i = 0; i < Count; i++)
        QzCreateThread(Name, Routine, Context);
}

/* ---- DPC ---- */

static volatile LONG QzDpcRuns;
static volatile KIRQL QzDpcIrql;
static volatile BOOLEAN QzDpcSawExecuting;

static VOID NTAPI
QzTestDpcRoutine(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    QzDpcRuns++;
    QzDpcIrql = QzGetCurrentIrql();
    QzDpcSawExecuting = KeIsExecutingDpc();
}

static VOID
QzTestIrqlAndDpc(VOID)
{
    KDPC Dpc;
    KIRQL OldIrql;
    BOOLEAN First, Second;

    QzDpcRuns = 0;
    KeInitializeDpc(&Dpc, QzTestDpcRoutine, NULL);

    OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    QzCheck("raise IRQL to DISPATCH_LEVEL (PCR FS:0x24)",
            OldIrql == PASSIVE_LEVEL && QzGetCurrentIrql() == DISPATCH_LEVEL &&
            __readfsbyte(0x24) == DISPATCH_LEVEL);
    First = KeInsertQueueDpc(&Dpc, NULL, NULL);
    Second = KeInsertQueueDpc(&Dpc, NULL, NULL);
    QzCheck("DPC queued at DISPATCH is deferred; double insert refused",
            First && !Second && QzDpcRuns == 0);
    QzLowerIrql(OldIrql);
    QzCheck("lowering IRQL delivers the DPC at DISPATCH_LEVEL",
            QzDpcRuns == 1 && QzDpcIrql == DISPATCH_LEVEL && QzDpcSawExecuting &&
            QzGetCurrentIrql() == PASSIVE_LEVEL && !KeIsExecutingDpc());

    KeInsertQueueDpc(&Dpc, NULL, NULL);
    QzCheck("DPC queued at PASSIVE_LEVEL runs immediately", QzDpcRuns == 2);

    OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    KeInsertQueueDpc(&Dpc, NULL, NULL);
    First = KeRemoveQueueDpc(&Dpc);
    QzLowerIrql(OldIrql);
    QzCheck("KeRemoveQueueDpc cancels a queued DPC", First && QzDpcRuns == 2);
}

/* ---- Events ---- */

static VOID
QzTestEvents(VOID)
{
    BOOLEAN Ok;
    ULONG i;

    /* Notification event releases every waiter */
    KeInitializeEvent(&QzGo, NotificationEvent, FALSE);
    KeInitializeSemaphore(&QzDone, 0, 100);
    QzWoken = 0;
    QzSpawn("evwait", QzWaitOnGoWorker, NULL, 3);
    QzSleepMs(50);
    Ok = (QzWoken == 0);
    KeSetEvent(&QzGo, 0, FALSE);
    Ok = Ok && QzWaitDone(3, 1000);
    QzCheck("notification event wakes all 3 waiters and stays signaled",
            Ok && QzWoken == 3 && KeReadStateEvent(&QzGo) == 1);

    /* Synchronization event releases exactly one waiter per set */
    KeInitializeEvent(&QzGo, SynchronizationEvent, FALSE);
    QzWoken = 0;
    QzSpawn("evwait", QzWaitOnGoWorker, NULL, 3);
    QzSleepMs(50);
    Ok = TRUE;
    for (i = 1; i <= 3; i++)
    {
        KeSetEvent(&QzGo, 0, FALSE);
        Ok = Ok && QzWaitDone(1, 1000);
        QzSleepMs(30);
        Ok = Ok && QzWoken == (LONG)i && KeReadStateEvent(&QzGo) == 0;
    }
    QzCheck("synchronization event wakes one waiter per KeSetEvent, auto-resets", Ok);

    /* Pulse: releases current waiters, leaves the event non-signaled */
    KeInitializeEvent(&QzGo, NotificationEvent, FALSE);
    QzWoken = 0;
    QzSpawn("evwait", QzWaitOnGoWorker, NULL, 2);
    QzSleepMs(50);
    KePulseEvent(&QzGo, 0, FALSE);
    Ok = QzWaitDone(2, 1000);
    QzCheck("KePulseEvent releases both waiters and resets",
            Ok && QzWoken == 2 && KeReadStateEvent(&QzGo) == 0);
}

/* ---- Semaphores: bounded producer/consumer ---- */

static KSEMAPHORE QzItems, QzSpace;
static volatile LONG QzProduced;

static VOID NTAPI
QzProducer(PVOID Context)
{
    ULONG i;
    UNREFERENCED_PARAMETER(Context);
    for (i = 0; i < 20; i++)
    {
        KeWaitForSingleObject(&QzSpace, Executive, KernelMode, FALSE, NULL);
        InterlockedIncrement(&QzProduced);
        KeReleaseSemaphore(&QzItems, 0, 1, FALSE);
    }
}

static VOID
QzTestSemaphores(VOID)
{
    LARGE_INTEGER Timeout = QzMs(2000);
    ULONG Consumed = 0, i;
    LONG MaxInFlight = 0;

    KeInitializeSemaphore(&QzItems, 0, 4);
    KeInitializeSemaphore(&QzSpace, 4, 4);
    QzProduced = 0;
    QzCreateThread("producer", QzProducer, NULL);

    for (i = 0; i < 20; i++)
    {
        if (KeWaitForSingleObject(&QzItems, Executive, KernelMode, FALSE, &Timeout) != STATUS_WAIT_0)
            break;
        Consumed++;
        if (QzProduced - (LONG)Consumed > MaxInFlight)
            MaxInFlight = QzProduced - (LONG)Consumed;
        KeReleaseSemaphore(&QzSpace, 0, 1, FALSE);
        if (i % 5 == 0)
            QzSleepMs(10);          /* let the producer fill the buffer */
    }
    QzSleepMs(20);
    QzCheck("semaphores: 20 items through a 4-slot bounded buffer",
            Consumed == 20 && QzProduced == 20 && MaxInFlight <= 4 &&
            KeReadStateSemaphore(&QzItems) == 0 && KeReadStateSemaphore(&QzSpace) == 4);
}

/* ---- Mutexes ---- */

static KMUTEX QzMutex;
static volatile LONG QzShared;
static volatile BOOLEAN QzInSection, QzOverlap;

static VOID NTAPI
QzMutexWorker(PVOID Context)
{
    ULONG i;
    LONG Local;
    UNREFERENCED_PARAMETER(Context);
    for (i = 0; i < 5; i++)
    {
        KeWaitForSingleObject(&QzMutex, Executive, KernelMode, FALSE, NULL);
        if (QzInSection)
            QzOverlap = TRUE;
        QzInSection = TRUE;
        Local = QzShared;
        QzSleepMs(10);              /* get preempted while holding it */
        QzShared = Local + 1;
        QzInSection = FALSE;
        KeReleaseMutex(&QzMutex, FALSE);
    }
    KeReleaseSemaphore(&QzDone, 0, 1, FALSE);
}

static VOID
QzTestMutexes(VOID)
{
    BOOLEAN Ok;

    KeInitializeMutex(&QzMutex, 0);
    KeWaitForSingleObject(&QzMutex, Executive, KernelMode, FALSE, NULL);
    KeWaitForSingleObject(&QzMutex, Executive, KernelMode, FALSE, NULL);
    KeWaitForSingleObject(&QzMutex, Executive, KernelMode, FALSE, NULL);
    Ok = (KeReadStateMutant(&QzMutex) == -2);
    KeReleaseMutex(&QzMutex, FALSE);
    KeReleaseMutex(&QzMutex, FALSE);
    KeReleaseMutex(&QzMutex, FALSE);
    QzCheck("mutex: recursive acquisition x3, released x3",
            Ok && KeReadStateMutant(&QzMutex) == 1);

    KeInitializeSemaphore(&QzDone, 0, 100);
    QzShared = 0;
    QzOverlap = FALSE;
    QzSpawn("mutexwk", QzMutexWorker, NULL, 2);
    Ok = QzWaitDone(2, 3000);
    QzCheck("mutex: 2 threads x 5 critical sections, no overlap, no lost update",
            Ok && QzShared == 10 && !QzOverlap);
}

/* ---- Timeouts and multiple waits ---- */

static KEVENT QzE1, QzE2;

static VOID NTAPI
QzSetE2Later(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    QzSleepMs(30);
    KeSetEvent(&QzE2, 0, FALSE);
}

static VOID
QzTestWaits(VOID)
{
    KEVENT Never;
    LARGE_INTEGER Timeout;
    PVOID Objects[2] = { &QzE1, &QzE2 };
    ULONGLONG Start;
    NTSTATUS Status;
    ULONG Elapsed;

    KeInitializeEvent(&Never, NotificationEvent, FALSE);
    Timeout = QzMs(50);
    Start = KeQueryInterruptTime();
    Status = KeWaitForSingleObject(&Never, Executive, KernelMode, FALSE, &Timeout);
    Elapsed = QzElapsedMs(Start);
    QzCheck("wait with 50 ms timeout returns STATUS_TIMEOUT after ~50 ms",
            Status == STATUS_TIMEOUT && Elapsed >= 40 && Elapsed <= 90);

    Timeout.QuadPart = 0;
    QzCheck("zero timeout polls without blocking",
            KeWaitForSingleObject(&Never, Executive, KernelMode, FALSE, &Timeout) == STATUS_TIMEOUT);

    KeInitializeEvent(&QzE1, SynchronizationEvent, FALSE);
    KeInitializeEvent(&QzE2, SynchronizationEvent, FALSE);
    QzCreateThread("setter", QzSetE2Later, NULL);
    Timeout = QzMs(1000);
    Status = KeWaitForMultipleObjects(2, Objects, WaitAny, Executive, KernelMode,
                                      FALSE, &Timeout, NULL);
    QzCheck("WaitAny returns STATUS_WAIT_1 when the second object fires",
            Status == STATUS_WAIT_1 && KeReadStateEvent(&QzE2) == 0);

    KeSetEvent(&QzE1, 0, FALSE);
    Timeout = QzMs(30);
    Status = KeWaitForMultipleObjects(2, Objects, WaitAll, Executive, KernelMode,
                                      FALSE, &Timeout, NULL);
    QzCheck("WaitAll with one object signaled times out without consuming it",
            Status == STATUS_TIMEOUT && KeReadStateEvent(&QzE1) == 1);

    KeSetEvent(&QzE2, 0, FALSE);
    Status = KeWaitForMultipleObjects(2, Objects, WaitAll, Executive, KernelMode,
                                      FALSE, &Timeout, NULL);
    QzCheck("WaitAll succeeds once both are signaled and consumes both",
            Status == STATUS_WAIT_0 && KeReadStateEvent(&QzE1) == 0 &&
            KeReadStateEvent(&QzE2) == 0);

    /* KeSetEvent(..., Wait=TRUE) followed by a wait */
    KeInitializeEvent(&QzE1, NotificationEvent, FALSE);
    KeInitializeEvent(&QzE2, NotificationEvent, TRUE);
    KeSetEvent(&QzE1, 0, TRUE);
    Status = KeWaitForSingleObject(&QzE2, Executive, KernelMode, FALSE, NULL);
    QzCheck("KeSetEvent(Wait=TRUE) chains into the next wait, IRQL restored",
            Status == STATUS_WAIT_0 && QzGetCurrentIrql() == PASSIVE_LEVEL &&
            KeReadStateEvent(&QzE1) == 1);
}

/* ---- Timers ---- */

static volatile LONG QzTimerDpcRuns;

static VOID NTAPI
QzTimerDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    InterlockedIncrement(&QzTimerDpcRuns);
}

static VOID
QzTestTimers(VOID)
{
    KTIMER Timer;
    KDPC Dpc;
    LARGE_INTEGER Timeout = QzMs(1000);
    ULONGLONG Start;
    NTSTATUS Status;
    ULONG Elapsed;
    LONG Runs;
    BOOLEAN Cancelled;

    KeInitializeTimer(&Timer);
    KeInitializeDpc(&Dpc, QzTimerDpc, NULL);
    QzTimerDpcRuns = 0;

    Start = KeQueryInterruptTime();
    KeSetTimer(&Timer, QzMs(100), &Dpc);
    Status = KeWaitForSingleObject(&Timer, Executive, KernelMode, FALSE, &Timeout);
    Elapsed = QzElapsedMs(Start);
    QzCheck("one-shot timer: signaled after ~100 ms, DPC ran once",
            Status == STATUS_WAIT_0 && Elapsed >= 90 && Elapsed <= 150 &&
            QzTimerDpcRuns == 1 && KeReadStateTimer(&Timer) &&
            !KeCancelTimer(&Timer));

    QzTimerDpcRuns = 0;
    KeSetTimerEx(&Timer, QzMs(20), 20, &Dpc);
    QzSleepMs(210);
    Cancelled = KeCancelTimer(&Timer);
    Runs = QzTimerDpcRuns;
    QzSleepMs(60);
    QzCheck("periodic 20 ms timer fires ~10 times in 210 ms, stops when cancelled",
            Cancelled && Runs >= 8 && Runs <= 12 && QzTimerDpcRuns == Runs);

    KeInitializeTimerEx(&Timer, SynchronizationTimer);
    KeSetTimer(&Timer, QzMs(10), NULL);
    Status = KeWaitForSingleObject(&Timer, Executive, KernelMode, FALSE, &Timeout);
    QzCheck("synchronization timer auto-resets when a wait is satisfied",
            Status == STATUS_WAIT_0 && !KeReadStateTimer(&Timer));
}

/* ---- Spinlocks, critical regions ---- */

static VOID
QzTestSpinLocks(VOID)
{
    KSPIN_LOCK Lock;
    KLOCK_QUEUE_HANDLE Handle;
    KIRQL OldIrql;
    BOOLEAN Ok;

    KeInitializeSpinLock(&Lock);
    OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    Ok = KeTestSpinLock(&Lock);
    Ok = Ok && KeTryToAcquireSpinLockAtDpcLevel(&Lock);
    Ok = Ok && !KeTestSpinLock(&Lock);
    Ok = Ok && !KeTryToAcquireSpinLockAtDpcLevel(&Lock);
    QzKeReleaseSpinLockFromDpcLevel(&Lock);
    Ok = Ok && KeTestSpinLock(&Lock);
    KeAcquireInStackQueuedSpinLockAtDpcLevel(&Lock, &Handle);
    Ok = Ok && !KeTestSpinLock(&Lock);
    KeReleaseInStackQueuedSpinLockFromDpcLevel(&Handle);
    Ok = Ok && KeTestSpinLock(&Lock);
    QzLowerIrql(OldIrql);
    QzCheck("spinlocks: try/test/release, in-stack queued", Ok);

    QzKeEnterCriticalRegion();
    Ok = KeAreApcsDisabled();
    QzKeLeaveCriticalRegion();
    QzCheck("critical region disables/enables kernel APCs", Ok && !KeAreApcsDisabled());
}

/* ---- Time ---- */

static BOOLEAN
QzRoundTrip(CSHORT Y, CSHORT M, CSHORT D, CSHORT h, CSHORT m, CSHORT s, CSHORT ms)
{
    TIME_FIELDS In = { Y, M, D, h, m, s, ms, 0 }, Out;
    LARGE_INTEGER Time;
    if (!RtlTimeFieldsToTime(&In, &Time))
        return FALSE;
    RtlTimeToTimeFields(&Time, &Out);
    return Out.Year == Y && Out.Month == M && Out.Day == D && Out.Hour == h &&
           Out.Minute == m && Out.Second == s && Out.Milliseconds == ms;
}

static VOID
QzTestTime(VOID)
{
    TIME_FIELDS Fields = { 1601, 1, 1, 0, 0, 0, 0, 0 };
    TIME_FIELDS Bad = { 2026, 2, 29, 0, 0, 0, 0, 0 };
    LARGE_INTEGER T1, T2, Epoch;
    ULONGLONG Shared;
    LONGLONG Diff;

    KeQuerySystemTime(&T1);
    QzSleepMs(100);
    KeQuerySystemTime(&T2);
    Diff = (T2.QuadPart - T1.QuadPart) / 10000;
    QzCheck("KeQuerySystemTime advances with the clock", Diff >= 90 && Diff <= 150);

    RtlTimeFieldsToTime(&Fields, &Epoch);
    RtlTimeToTimeFields(&Epoch, &Fields);
    QzCheck("time fields: 1601-01-01 is time 0 and a Monday",
            Epoch.QuadPart == 0 && Fields.Weekday == 1);
    QzCheck("time fields: round trips (2026-11-20 23:59:59.999, 2000-02-29)",
            QzRoundTrip(2026, 11, 20, 23, 59, 59, 999) && QzRoundTrip(2000, 2, 29, 12, 0, 0, 0));
    QzCheck("time fields: 2026-02-29 rejected", !RtlTimeFieldsToTime(&Bad, &T1));

    Shared = ((ULONGLONG)SharedUserData->InterruptTime.High1Time << 32) |
             SharedUserData->InterruptTime.LowPart;
    QzCheck("KUSER_SHARED_DATA interrupt time and NT 6.1 version are published",
            (LONGLONG)(KeQueryInterruptTime() - Shared) <= 2 * QZ_TIME_INCREMENT &&
            SharedUserData->NtMajorVersion == 6 && SharedUserData->NtMinorVersion == 1);
}

VOID
QzRunSyncTests(VOID)
{
    QzPassed = QzFailed = 0;
    QzTestIrqlAndDpc();
    QzTestEvents();
    QzTestSemaphores();
    QzTestMutexes();
    QzTestWaits();
    QzTestTimers();
    QzTestSpinLocks();
    QzTestTime();
    QzPrint("  synctest: %lu passed, %lu failed\n", QzPassed, QzFailed);
}
