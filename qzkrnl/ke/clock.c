/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The kernel side of the system clock (driven by the HAL)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * The HAL owns the clock hardware (8254 PIT on the PIC HAL). It tells the
 * kernel its tick length with KeSetTimeIncrement during HalInitSystem phase
 * 0, and from HalInitSystem phase 1 on, its clock interrupt calls
 * KeUpdateSystemTime, which never returns: the kernel ends the interrupt.
 */

#include "qz.h"
#include <ntimage.h>

#undef KeTickCount
#undef KeQueryTickCount
#undef KeQuerySystemTime

/* Exported: drivers read the tick count directly on x86 */
volatile KSYSTEM_TIME KeTickCount;
volatile ULONG QzTicks;
volatile ULONGLONG QzInterruptTime;

/* Exported data the HAL reads */
BOOLEAN KiEnableTimerWatchdog = FALSE;

/* Tick length in 100ns units; the HAL reports its own (about 10 ms) */
ULONG QzTimeIncrement = 100000;
static ULONG QzMinimumIncrement = 100000;

/* KSYSTEM_TIME update protocol: High2, Low, High1 */
static __inline VOID
QzStoreSystemTime(volatile KSYSTEM_TIME *Time, ULONGLONG Value)
{
    Time->High2Time = (LONG)(Value >> 32);
    Time->LowPart = (ULONG)Value;
    Time->High1Time = (LONG)(Value >> 32);
}

VOID
QzSharedUserDataInit(VOID)
{
    PKUSER_SHARED_DATA Shared = SharedUserData;   /* 0xFFDF0000, mapped by the loader */

    RtlZeroMemory((PVOID)Shared, sizeof(*Shared));
    Shared->TickCountMultiplier = (ULONG)(((ULONGLONG)QzTimeIncrement << 24) / 10000);
    Shared->NtMajorVersion = QZ_NT_MAJOR;
    Shared->NtMinorVersion = QZ_NT_MINOR;
    Shared->NtProductType = NtProductWinNt;
    Shared->ProductTypeIsValid = TRUE;
    Shared->ImageNumberLow = IMAGE_FILE_MACHINE_I386;
    Shared->ImageNumberHigh = IMAGE_FILE_MACHINE_I386;
}

/* ---------------------------------------------------------------------- */
/* Exports                                                                 */
/* ---------------------------------------------------------------------- */

VOID
NTAPI
KeSetTimeIncrement(IN ULONG MaxIncrement,
                   IN ULONG MinIncrement)
{
    QzTimeIncrement = MaxIncrement;
    QzMinimumIncrement = MinIncrement;
    SharedUserData->TickCountMultiplier =
        (ULONG)(((ULONGLONG)QzTimeIncrement << 24) / 10000);
}

ULONG
NTAPI
KeQueryTimeIncrement(VOID)
{
    return QzTimeIncrement;
}

/*
 * The HAL's clock interrupt, at CLOCK2_LEVEL with interrupts disabled.
 * Irql is the IRQL the interrupted code ran at.
 */
DECLSPEC_NORETURN
VOID
FASTCALL
KeUpdateSystemTime(IN PKTRAP_FRAME TrapFrame,
                   IN ULONG Increment,
                   IN KIRQL Irql)
{
    PKUSER_SHARED_DATA Shared = SharedUserData;
    LARGE_INTEGER SystemTime;
    ULONG Low = KeTickCount.LowPart + 1;
    LONG High = KeTickCount.High1Time;

    if (Low == 0)
        High++;

    KeTickCount.High2Time = High;
    KeTickCount.LowPart = Low;
    KeTickCount.High1Time = High;
    QzTicks++;
    QzPrcb->ClockInterrupts++;

    QzInterruptTime += Increment;

    /* x86 drivers read these straight from KUSER_SHARED_DATA */
    QzStoreSystemTime(&Shared->InterruptTime, QzInterruptTime);
    KeQuerySystemTime(&SystemTime);
    QzStoreSystemTime(&Shared->SystemTime, SystemTime.QuadPart);
    QzStoreSystemTime(&Shared->TickCount, ((ULONGLONG)(ULONG)High << 32) | Low);

    /* Timer expiration and quantum end run in the DISPATCH software interrupt */
    QzPrcb->TimerPending = TRUE;
    QzPrcb->DispatchPending = TRUE;
    HalRequestSoftwareInterrupt(DISPATCH_LEVEL);    /* pends: we are above it */

    /* End the interrupt; the HAL may run the dispatch interrupt right away */
    _disable();
    HalEndSystemInterrupt(Irql, TrapFrame);
    KiEoiHelper(TrapFrame);
}

/* Profiling is not implemented: the HAL keeps the profile interrupt off */
VOID
NTAPI
KeProfileInterrupt(IN PKTRAP_FRAME TrapFrame)
{
    UNREFERENCED_PARAMETER(TrapFrame);
}

VOID
NTAPI
KeQueryTickCount(OUT PLARGE_INTEGER TickCount)
{
    do
    {
        TickCount->HighPart = KeTickCount.High1Time;
        TickCount->LowPart = KeTickCount.LowPart;
    } while (TickCount->HighPart != KeTickCount.High2Time);
}
