/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel timers (KTIMER) and system/interrupt time
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Due times are kept as absolute interrupt time (100ns since boot) in
 * KTIMER.DueTime. The clock interrupt requests expiration processing, which
 * runs at DISPATCH_LEVEL as part of the DISPATCH software interrupt: expired
 * timers become signaled, satisfy their waiters, queue their DPC and, when
 * periodic, are re-armed.
 */

#include "qz.h"

#define QZ_TIMER_NOTIFICATION       8
#define QZ_TIMER_SYNCHRONIZATION    9

#undef KeQuerySystemTime
#undef KeQueryInterruptTime

ULONGLONG QzTimeoutToInterruptTime(LONGLONG Timeout);

static LIST_ENTRY QzTimerListHead;
static ULONG QzTimerCount;

/* System time at boot (100ns since 1601), from the CMOS clock */
static LONGLONG QzBootSystemTime;

VOID
QzTimerObjInit(VOID)
{
    TIME_FIELDS Now;

    InitializeListHead(&QzTimerListHead);

    QzReadRtc(&Now);
    if (!RtlTimeFieldsToTime(&Now, (PLARGE_INTEGER)&QzBootSystemTime))
        QzBootSystemTime = 0;
}

ULONG
QzActiveTimerCount(VOID)
{
    return QzTimerCount;
}

static VOID
QzInsertTimer(PKTIMER Timer, ULONGLONG DueInterruptTime)
{
    Timer->DueTime.QuadPart = DueInterruptTime;
    InsertTailList(&QzTimerListHead, &Timer->TimerListEntry);
    Timer->Header.Inserted = TRUE;
    QzTimerCount++;
}

static VOID
QzRemoveTimer(PKTIMER Timer)
{
    RemoveEntryList(&Timer->TimerListEntry);
    Timer->Header.Inserted = FALSE;
    QzTimerCount--;
}

/* Called at DISPATCH_LEVEL from the DISPATCH software interrupt */
VOID
QzExpireTimers(VOID)
{
    PLIST_ENTRY Entry, Next;
    PKTIMER Timer;
    LARGE_INTEGER SystemTime;

    for (Entry = QzTimerListHead.Flink; Entry != &QzTimerListHead; Entry = Next)
    {
        Next = Entry->Flink;
        Timer = CONTAINING_RECORD(Entry, KTIMER, TimerListEntry);
        if (Timer->DueTime.QuadPart > QzInterruptTime)
            continue;

        QzRemoveTimer(Timer);

        Timer->Header.SignalState = 1;
        QzWaitTest(&Timer->Header);

        if (Timer->Period)
            QzInsertTimer(Timer, Timer->DueTime.QuadPart + (ULONGLONG)Timer->Period * 10000);

        if (Timer->Dpc)
        {
            KeQuerySystemTime(&SystemTime);
            KeInsertQueueDpc(Timer->Dpc,
                             ULongToPtr(SystemTime.LowPart),
                             ULongToPtr(SystemTime.HighPart));
        }

        /* A re-armed periodic timer may now sit before Next: keep going */
    }
}

/* ---------------------------------------------------------------------- */
/* Timer exports                                                           */
/* ---------------------------------------------------------------------- */

VOID
NTAPI
KeInitializeTimerEx(OUT PKTIMER Timer,
                    IN TIMER_TYPE Type)
{
    RtlZeroMemory(&Timer->Header, sizeof(Timer->Header));
    Timer->Header.Type = (Type == SynchronizationTimer) ? QZ_TIMER_SYNCHRONIZATION
                                                        : QZ_TIMER_NOTIFICATION;
    Timer->Header.Size = sizeof(KTIMER) / sizeof(LONG);
    Timer->Header.SignalState = 0;
    InitializeListHead(&Timer->Header.WaitListHead);
    Timer->DueTime.QuadPart = 0;
    Timer->Dpc = NULL;
    Timer->Period = 0;
}

VOID
NTAPI
KeInitializeTimer(OUT PKTIMER Timer)
{
    KeInitializeTimerEx(Timer, NotificationTimer);
}

BOOLEAN
NTAPI
KeSetTimerEx(IN OUT PKTIMER Timer,
             IN LARGE_INTEGER DueTime,
             IN LONG Period,
             IN PKDPC Dpc OPTIONAL)
{
    KIRQL OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    BOOLEAN WasInserted = Timer->Header.Inserted;

    if (WasInserted)
        QzRemoveTimer(Timer);

    Timer->Header.SignalState = 0;
    Timer->Dpc = Dpc;
    Timer->Period = (Period > 0) ? (ULONG)Period : 0;
    QzInsertTimer(Timer, QzTimeoutToInterruptTime(DueTime.QuadPart));

    /* Already due (zero or past due time): expire at the next opportunity */
    if (Timer->DueTime.QuadPart <= QzInterruptTime)
    {
        QzPrcb->TimerPending = TRUE;
    }

    QzLowerIrql(OldIrql);
    return WasInserted;
}

BOOLEAN
NTAPI
KeSetTimer(IN OUT PKTIMER Timer,
           IN LARGE_INTEGER DueTime,
           IN PKDPC Dpc OPTIONAL)
{
    return KeSetTimerEx(Timer, DueTime, 0, Dpc);
}

BOOLEAN
NTAPI
KeCancelTimer(IN OUT PKTIMER Timer)
{
    KIRQL OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    BOOLEAN WasInserted = Timer->Header.Inserted;

    if (WasInserted)
        QzRemoveTimer(Timer);

    QzLowerIrql(OldIrql);
    return WasInserted;
}

BOOLEAN
NTAPI
KeReadStateTimer(IN PKTIMER Timer)
{
    return (BOOLEAN)Timer->Header.SignalState;
}

/* ---------------------------------------------------------------------- */
/* Time exports                                                            */
/* ---------------------------------------------------------------------- */

ULONGLONG
NTAPI
KeQueryInterruptTime(VOID)
{
    ULONGLONG Value;
    ULONG Flags = QzSaveFlagsAndDisable();
    Value = QzInterruptTime;
    QzRestoreFlags(Flags);
    return Value;
}

VOID
NTAPI
KeQuerySystemTime(OUT PLARGE_INTEGER CurrentTime)
{
    CurrentTime->QuadPart = QzBootSystemTime + (LONGLONG)KeQueryInterruptTime();
}
