/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NT time <-> calendar conversion
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * NT time counts 100ns intervals since 1601-01-01 00:00 UTC (a Monday).
 * Conversions go through a day count using the proleptic Gregorian
 * "days from civil" arithmetic (eras of 400 years = 146097 days).
 */

#include "qz.h"

#define TICKS_PER_SECOND    10000000LL
#define TICKS_PER_DAY       (TICKS_PER_SECOND * 86400LL)
#define DAYS_1601_TO_1970   134774LL

/* Days since 1970-01-01 for a Gregorian date */
static LONGLONG
QzDaysFromCivil(LONG y, LONG m, LONG d)
{
    LONG era, yoe, doy, doe;

    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;                                   /* [0, 399] */
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  /* [0, 365] */
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           /* [0, 146096] */
    return (LONGLONG)era * 146097 + doe - 719468;
}

static VOID
QzCivilFromDays(LONGLONG z, LONG *y, LONG *m, LONG *d)
{
    LONG era, doe, yoe, doy, mp;

    z += 719468;
    era = (LONG)((z >= 0 ? z : z - 146096) / 146097);
    doe = (LONG)(z - (LONGLONG)era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = yoe + era * 400 + (*m <= 2);
}

static BOOLEAN
QzIsLeapYear(LONG y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

BOOLEAN
NTAPI
RtlTimeFieldsToTime(IN PTIME_FIELDS TimeFields,
                    OUT PLARGE_INTEGER Time)
{
    static const UCHAR DaysInMonth[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    LONG MaxDay;
    LONGLONG Days;

    if (TimeFields->Year < 1601 || TimeFields->Month < 1 || TimeFields->Month > 12 ||
        TimeFields->Hour < 0 || TimeFields->Hour > 23 ||
        TimeFields->Minute < 0 || TimeFields->Minute > 59 ||
        TimeFields->Second < 0 || TimeFields->Second > 59 ||
        TimeFields->Milliseconds < 0 || TimeFields->Milliseconds > 999)
    {
        return FALSE;
    }

    MaxDay = DaysInMonth[TimeFields->Month - 1] +
             (TimeFields->Month == 2 && QzIsLeapYear(TimeFields->Year));
    if (TimeFields->Day < 1 || TimeFields->Day > MaxDay)
        return FALSE;

    Days = QzDaysFromCivil(TimeFields->Year, TimeFields->Month, TimeFields->Day) +
           DAYS_1601_TO_1970;
    Time->QuadPart = Days * TICKS_PER_DAY +
                     ((LONGLONG)TimeFields->Hour * 3600 +
                      TimeFields->Minute * 60 + TimeFields->Second) * TICKS_PER_SECOND +
                     (LONGLONG)TimeFields->Milliseconds * 10000;
    return TRUE;
}

VOID
NTAPI
RtlTimeToTimeFields(IN PLARGE_INTEGER Time,
                    OUT PTIME_FIELDS TimeFields)
{
    LONGLONG Days = Time->QuadPart / TICKS_PER_DAY;
    LONGLONG Rest = Time->QuadPart % TICKS_PER_DAY;
    LONG y, m, d;

    QzCivilFromDays(Days - DAYS_1601_TO_1970, &y, &m, &d);

    TimeFields->Year = (CSHORT)y;
    TimeFields->Month = (CSHORT)m;
    TimeFields->Day = (CSHORT)d;
    TimeFields->Weekday = (CSHORT)((Days + 1) % 7);   /* 1601-01-01 was a Monday */
    TimeFields->Hour = (CSHORT)(Rest / (3600 * TICKS_PER_SECOND));
    Rest %= 3600 * TICKS_PER_SECOND;
    TimeFields->Minute = (CSHORT)(Rest / (60 * TICKS_PER_SECOND));
    Rest %= 60 * TICKS_PER_SECOND;
    TimeFields->Second = (CSHORT)(Rest / TICKS_PER_SECOND);
    TimeFields->Milliseconds = (CSHORT)((Rest % TICKS_PER_SECOND) / 10000);
}
