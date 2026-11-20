/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     8259 PIC, 8254 PIT and the system clock
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * NOTE: On real NT this belongs to the HAL. qzkrnl drives the legacy
 * platform devices itself until it is able to host HAL.DLL.
 */

#include "qz.h"

#undef KeTickCount
#undef KeQueryTickCount

/* Exported: drivers read the tick count directly on x86 */
volatile KSYSTEM_TIME KeTickCount;
volatile ULONG QzTicks;

static UCHAR QzPicMask[2] = { 0xFF, 0xFF };

VOID
QzPicInit(VOID)
{
    /* ICW1: edge triggered, cascade, ICW4 needed */
    QzOutB(0x20, 0x11); QzIoWait();
    QzOutB(0xA0, 0x11); QzIoWait();
    /* ICW2: vector bases (NT's PRIMARY_VECTOR_BASE is 0x30) */
    QzOutB(0x21, QZ_VECTOR_IRQ_BASE); QzIoWait();
    QzOutB(0xA1, QZ_VECTOR_IRQ_BASE + 8); QzIoWait();
    /* ICW3: slave on IRQ2 */
    QzOutB(0x21, 0x04); QzIoWait();
    QzOutB(0xA1, 0x02); QzIoWait();
    /* ICW4: 8086 mode */
    QzOutB(0x21, 0x01); QzIoWait();
    QzOutB(0xA1, 0x01); QzIoWait();

    /* Everything masked except the cascade line */
    QzPicMask[0] = 0xFB;
    QzPicMask[1] = 0xFF;
    QzOutB(0x21, QzPicMask[0]);
    QzOutB(0xA1, QzPicMask[1]);
}

VOID
QzPicUnmask(ULONG Irq)
{
    ULONG Flags = QzSaveFlagsAndDisable();
    if (Irq < 8)
    {
        QzPicMask[0] &= (UCHAR)~(1 << Irq);
        QzOutB(0x21, QzPicMask[0]);
    }
    else
    {
        QzPicMask[1] &= (UCHAR)~(1 << (Irq - 8));
        QzOutB(0xA1, QzPicMask[1]);
    }
    QzRestoreFlags(Flags);
}

VOID
QzPicEoi(ULONG Irq)
{
    if (Irq >= 8)
        QzOutB(0xA0, 0x20);
    QzOutB(0x20, 0x20);
}

VOID
QzTimerInit(VOID)
{
    ULONG Divisor = 1193182 / QZ_TIMER_HZ;

    /* Channel 0, lobyte/hibyte, mode 2 (rate generator) */
    QzOutB(0x43, 0x34);
    QzOutB(0x40, (UCHAR)(Divisor & 0xFF));
    QzOutB(0x40, (UCHAR)(Divisor >> 8));
    QzPicUnmask(QZ_IRQ_TIMER);
}

VOID
QzClockTick(VOID)
{
    ULONG Low = KeTickCount.LowPart + 1;
    LONG High = KeTickCount.High1Time;

    if (Low == 0)
        High++;

    /* KSYSTEM_TIME update protocol: High2, Low, High1 */
    KeTickCount.High2Time = High;
    KeTickCount.LowPart = Low;
    KeTickCount.High1Time = High;
    QzTicks++;
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

ULONG
NTAPI
KeQueryTimeIncrement(VOID)
{
    return QZ_TIME_INCREMENT;
}
