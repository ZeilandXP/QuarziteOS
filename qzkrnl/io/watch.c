/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Watchdog for driver calls that do not come back
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * On a real PC there is no serial port to read the log from, and a driver
 * that never answers leaves a screen that just stops. Calls into drivers
 * (DriverEntry, AddDevice, PnP requests) note what they are doing here; a
 * thread names every one still running after QZ_WATCH_FIRST seconds, and
 * again every QZ_WATCH_AGAIN seconds, so a photo of the screen says which
 * driver, which request and which device.
 */

#include "qzio.h"

#define QZ_WATCH_SLOTS      16
#define QZ_WATCH_FIRST      30      /* seconds before the first report */
#define QZ_WATCH_AGAIN      30      /* seconds between reports */

typedef struct _QZ_WATCH_SLOT
{
    BOOLEAN InUse;
    PCSTR What;
    UNICODE_STRING Driver;          /* points into the driver object */
    WCHAR Device[112];
    ULONGLONG Start;                /* interrupt time, 100 ns units */
    ULONG Reported;                 /* seconds at the last report */
} QZ_WATCH_SLOT, *PQZ_WATCH_SLOT;

static QZ_WATCH_SLOT QzWatchSlots[QZ_WATCH_SLOTS];
static KSPIN_LOCK QzWatchLock;
static LONG QzWatchStarted;

static VOID NTAPI
QzWatchThread(PVOID Context)
{
    LARGE_INTEGER Interval;
    UNREFERENCED_PARAMETER(Context);

    Interval.QuadPart = -5LL * 10 * 1000 * 1000;
    for (;;)
    {
        ULONGLONG Now;
        ULONG i;

        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
        Now = KeQueryInterruptTime();
        for (i = 0; i < QZ_WATCH_SLOTS; i++)
        {
            QZ_WATCH_SLOT Copy;
            ULONG Seconds;
            KIRQL OldIrql;

            KeAcquireSpinLock(&QzWatchLock, &OldIrql);
            Copy = QzWatchSlots[i];
            Seconds = Copy.InUse ? (ULONG)((Now - Copy.Start) / (10 * 1000 * 1000)) : 0;
            if (Copy.InUse && Seconds >= QZ_WATCH_FIRST &&
                (!Copy.Reported || Seconds - Copy.Reported >= QZ_WATCH_AGAIN))
                QzWatchSlots[i].Reported = Seconds;
            else
                Copy.InUse = FALSE;
            KeReleaseSpinLock(&QzWatchLock, OldIrql);

            if (Copy.InUse)
                DbgPrint(QZ_KERNEL_NAME ": watchdog: %wZ has not finished %s%s%ls after %lu s\n",
                         &Copy.Driver, Copy.What, Copy.Device[0] ? " for " : "", Copy.Device, Seconds);
        }
    }
}

/* Note a driver call; returns the slot for QzWatchEnd (or -1: not watched) */
LONG
QzWatchBegin(PCSTR What, PDRIVER_OBJECT Driver, PCUNICODE_STRING Device)
{
    KIRQL OldIrql;
    LONG i;

    if (InterlockedCompareExchange(&QzWatchStarted, 1, 0) == 0)
    {
        KeInitializeSpinLock(&QzWatchLock);
        if (!QzCreateThread("watchdog", QzWatchThread, NULL))
            DbgPrint(QZ_KERNEL_NAME ": watchdog: thread not started\n");
    }

    KeAcquireSpinLock(&QzWatchLock, &OldIrql);
    for (i = 0; i < QZ_WATCH_SLOTS; i++)
    {
        PQZ_WATCH_SLOT Slot = &QzWatchSlots[i];
        if (Slot->InUse)
            continue;
        Slot->InUse = TRUE;
        Slot->What = What;
        RtlInitEmptyUnicodeString(&Slot->Driver, NULL, 0);
        if (Driver)
            Slot->Driver = Driver->DriverName;
        Slot->Device[0] = UNICODE_NULL;
        if (Device && Device->Buffer)
        {
            USHORT Chars = min(Device->Length / sizeof(WCHAR), RTL_NUMBER_OF(Slot->Device) - 1);
            RtlCopyMemory(Slot->Device, Device->Buffer, Chars * sizeof(WCHAR));
            Slot->Device[Chars] = UNICODE_NULL;
        }
        Slot->Start = KeQueryInterruptTime();
        Slot->Reported = 0;
        break;
    }
    KeReleaseSpinLock(&QzWatchLock, OldIrql);
    return i < QZ_WATCH_SLOTS ? i : -1;
}

VOID
QzWatchEnd(LONG Slot)
{
    KIRQL OldIrql;
    ULONG Seconds = 0;
    QZ_WATCH_SLOT Copy;

    if (Slot < 0 || Slot >= QZ_WATCH_SLOTS)
        return;
    KeAcquireSpinLock(&QzWatchLock, &OldIrql);
    Copy = QzWatchSlots[Slot];
    QzWatchSlots[Slot].InUse = FALSE;
    KeReleaseSpinLock(&QzWatchLock, OldIrql);

    /* A call the watchdog reported: say that it came back after all */
    if (Copy.Reported)
    {
        Seconds = (ULONG)((KeQueryInterruptTime() - Copy.Start) / (10 * 1000 * 1000));
        DbgPrint(QZ_KERNEL_NAME ": watchdog: %wZ finished %s after %lu s\n", &Copy.Driver, Copy.What, Seconds);
    }
}
