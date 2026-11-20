/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     MC146818-compatible CMOS real-time clock (read only)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * NOTE: on NT this is HalQueryRealTimeClock; it moves to the HAL in B2.
 */

#include "qz.h"

static UCHAR
QzCmosRead(UCHAR Register)
{
    QzOutB(0x70, Register);
    return QzInB(0x71);
}

static UCHAR
QzFromBcd(UCHAR Value)
{
    return (UCHAR)((Value >> 4) * 10 + (Value & 0x0F));
}

VOID
QzReadRtc(PTIME_FIELDS TimeFields)
{
    UCHAR Sec, Min, Hour, Day, Month, Year, Century, StatusB;
    ULONG Spin;
    BOOLEAN Pm;

    /* Wait for the end of an update cycle (status A bit 7) */
    for (Spin = 0; Spin < 100000 && (QzCmosRead(0x0A) & 0x80); Spin++);

    Sec = QzCmosRead(0x00);
    Min = QzCmosRead(0x02);
    Hour = QzCmosRead(0x04);
    Day = QzCmosRead(0x07);
    Month = QzCmosRead(0x08);
    Year = QzCmosRead(0x09);
    Century = QzCmosRead(0x32);
    StatusB = QzCmosRead(0x0B);

    Pm = (Hour & 0x80) != 0;
    Hour &= 0x7F;

    if (!(StatusB & 0x04))      /* BCD mode */
    {
        Sec = QzFromBcd(Sec);
        Min = QzFromBcd(Min);
        Hour = QzFromBcd(Hour);
        Day = QzFromBcd(Day);
        Month = QzFromBcd(Month);
        Year = QzFromBcd(Year);
        Century = QzFromBcd(Century);
    }
    if (!(StatusB & 0x02))      /* 12-hour mode */
    {
        if (Hour == 12) Hour = 0;
        if (Pm) Hour += 12;
    }
    if (Century < 19 || Century > 21)
        Century = 20;

    TimeFields->Year = (CSHORT)(Century * 100 + Year);
    TimeFields->Month = Month;
    TimeFields->Day = Day;
    TimeFields->Hour = Hour;
    TimeFields->Minute = Min;
    TimeFields->Second = Sec;
    TimeFields->Milliseconds = 0;
    TimeFields->Weekday = 0;
}
