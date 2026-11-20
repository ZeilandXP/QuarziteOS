/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     printf-style formatting, console output, DbgPrint
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Supports the NT kernel format dialect: %s %S %ls %Z %wZ %c %C %d %i %u
 * %x %X %o %p %%, flags (- 0 + space #), width/precision (incl. *), and the
 * length modifiers h l ll I64 I32 I w.
 */

#include "qz.h"

VOID QzVgaSetAttribute(UCHAR Attr);

typedef struct _QZ_OUT
{
    char *Buffer;
    size_t Size;
    size_t Count;
} QZ_OUT;

static __inline VOID
QzOutChar(QZ_OUT *Out, char c)
{
    if (Out->Count + 1 < Out->Size)
        Out->Buffer[Out->Count] = c;
    Out->Count++;
}

static VOID
QzOutPad(QZ_OUT *Out, char c, int Count)
{
    while (Count-- > 0)
        QzOutChar(Out, c);
}

static VOID
QzOutNumber(QZ_OUT *Out, ULONGLONG Value, BOOLEAN Negative, ULONG Base,
            BOOLEAN Upper, int Width, int Precision, BOOLEAN Left,
            BOOLEAN Zero, char Sign, BOOLEAN Alt)
{
    char Digits[24];
    int Length = 0, Prefix, Total;
    const char *Set = Upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char SignChar = Negative ? '-' : Sign;

    do
    {
        Digits[Length++] = Set[Value % Base];
        Value /= Base;
    } while (Value);

    if (Precision == 0 && Length == 1 && Digits[0] == '0')
        Length = 0;

    Prefix = (SignChar ? 1 : 0) + ((Alt && Base == 16) ? 2 : 0) + ((Alt && Base == 8) ? 1 : 0);
    Total = (Precision > Length ? Precision : Length) + Prefix;

    if (!Left && !(Zero && Precision < 0))
        QzOutPad(Out, ' ', Width - Total);
    if (SignChar)
        QzOutChar(Out, SignChar);
    if (Alt && Base == 16)
    {
        QzOutChar(Out, '0');
        QzOutChar(Out, Upper ? 'X' : 'x');
    }
    else if (Alt && Base == 8)
    {
        QzOutChar(Out, '0');
    }
    if (!Left && Zero && Precision < 0)
        QzOutPad(Out, '0', Width - Total);
    QzOutPad(Out, '0', Precision - Length);
    while (Length)
        QzOutChar(Out, Digits[--Length]);
    if (Left)
        QzOutPad(Out, ' ', Width - Total);
}

static VOID
QzOutAnsi(QZ_OUT *Out, const char *s, int Length, int Width, int Precision, BOOLEAN Left)
{
    int i;

    if (!s)
    {
        s = "(null)";
        Length = -1;
    }
    if (Length < 0)
        for (Length = 0; s[Length]; Length++);
    if (Precision >= 0 && Length > Precision)
        Length = Precision;

    if (!Left) QzOutPad(Out, ' ', Width - Length);
    for (i = 0; i < Length; i++)
        QzOutChar(Out, s[i]);
    if (Left) QzOutPad(Out, ' ', Width - Length);
}

static VOID
QzOutWide(QZ_OUT *Out, const WCHAR *s, int Length, int Width, int Precision, BOOLEAN Left)
{
    int i;

    if (!s)
    {
        QzOutAnsi(Out, NULL, -1, Width, Precision, Left);
        return;
    }
    if (Length < 0)
        for (Length = 0; s[Length]; Length++);
    if (Precision >= 0 && Length > Precision)
        Length = Precision;

    if (!Left) QzOutPad(Out, ' ', Width - Length);
    for (i = 0; i < Length; i++)
        QzOutChar(Out, s[i] < 0x80 ? (char)s[i] : '?');   /* no NLS tables yet */
    if (Left) QzOutPad(Out, ' ', Width - Length);
}

int
QzVsnprintf(char *Buffer, size_t Size, const char *Format, va_list Args)
{
    QZ_OUT Out = { Buffer, Size, 0 };
    const char *p;

    for (p = Format; *p; p++)
    {
        BOOLEAN Left = FALSE, Zero = FALSE, Alt = FALSE, Wide = FALSE;
        char Sign = 0;
        int Width = 0, Precision = -1, LongCount = 0;
        BOOLEAN Is64 = FALSE;

        if (*p != '%')
        {
            QzOutChar(&Out, *p);
            continue;
        }
        p++;

        /* Flags */
        for (;; p++)
        {
            if (*p == '-') Left = TRUE;
            else if (*p == '0') Zero = TRUE;
            else if (*p == '+') Sign = '+';
            else if (*p == ' ') { if (!Sign) Sign = ' '; }
            else if (*p == '#') Alt = TRUE;
            else break;
        }

        /* Width */
        if (*p == '*')
        {
            Width = va_arg(Args, int);
            if (Width < 0) { Left = TRUE; Width = -Width; }
            p++;
        }
        else
        {
            while (*p >= '0' && *p <= '9')
                Width = Width * 10 + (*p++ - '0');
        }

        /* Precision */
        if (*p == '.')
        {
            p++;
            Precision = 0;
            if (*p == '*')
            {
                Precision = va_arg(Args, int);
                p++;
            }
            else
            {
                while (*p >= '0' && *p <= '9')
                    Precision = Precision * 10 + (*p++ - '0');
            }
        }

        /* Length modifiers */
        for (;;)
        {
            if (*p == 'h') { p++; }
            else if (*p == 'l') { LongCount++; Wide = TRUE; p++; }
            else if (*p == 'w') { Wide = TRUE; p++; }
            else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { Is64 = TRUE; p += 3; }
            else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') { p += 3; }
            else if (*p == 'I') { p++; }
            else break;
        }
        if (LongCount >= 2)
            Is64 = TRUE;

        switch (*p)
        {
            case 'd':
            case 'i':
            {
                LONGLONG v = Is64 ? va_arg(Args, LONGLONG) : (LONGLONG)va_arg(Args, int);
                QzOutNumber(&Out, v < 0 ? (ULONGLONG)-v : (ULONGLONG)v, v < 0, 10,
                            FALSE, Width, Precision, Left, Zero, Sign, FALSE);
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o':
            {
                ULONGLONG v = Is64 ? va_arg(Args, ULONGLONG) : (ULONGLONG)va_arg(Args, unsigned int);
                ULONG Base = (*p == 'u') ? 10 : (*p == 'o') ? 8 : 16;
                QzOutNumber(&Out, v, FALSE, Base, *p == 'X', Width, Precision,
                            Left, Zero, 0, Alt);
                break;
            }
            case 'p':
                QzOutNumber(&Out, (ULONG_PTR)va_arg(Args, void *), FALSE, 16, TRUE,
                            8, -1, FALSE, TRUE, 0, FALSE);
                break;
            case 'c':
            case 'C':
            {
                int c = va_arg(Args, int);
                if (!Left) QzOutPad(&Out, ' ', Width - 1);
                QzOutChar(&Out, (char)c);
                if (Left) QzOutPad(&Out, ' ', Width - 1);
                break;
            }
            case 's':
                if (Wide)
                    QzOutWide(&Out, va_arg(Args, const WCHAR *), -1, Width, Precision, Left);
                else
                    QzOutAnsi(&Out, va_arg(Args, const char *), -1, Width, Precision, Left);
                break;
            case 'S':
                QzOutWide(&Out, va_arg(Args, const WCHAR *), -1, Width, Precision, Left);
                break;
            case 'Z':
                if (Wide)
                {
                    PUNICODE_STRING us = va_arg(Args, PUNICODE_STRING);
                    if (us && us->Buffer)
                        QzOutWide(&Out, us->Buffer, us->Length / sizeof(WCHAR),
                                  Width, Precision, Left);
                    else
                        QzOutAnsi(&Out, NULL, -1, Width, Precision, Left);
                }
                else
                {
                    PANSI_STRING as = va_arg(Args, PANSI_STRING);
                    if (as && as->Buffer)
                        QzOutAnsi(&Out, as->Buffer, as->Length, Width, Precision, Left);
                    else
                        QzOutAnsi(&Out, NULL, -1, Width, Precision, Left);
                }
                break;
            case '%':
                QzOutChar(&Out, '%');
                break;
            case 0:
                p--;
                break;
            default:
                QzOutChar(&Out, '%');
                QzOutChar(&Out, *p);
                break;
        }
    }

    if (Size)
        Buffer[Out.Count < Size ? Out.Count : Size - 1] = 0;
    return (int)Out.Count;
}

int
QzSnprintf(char *Buffer, size_t Size, const char *Format, ...)
{
    va_list Args;
    int Result;

    va_start(Args, Format);
    Result = QzVsnprintf(Buffer, Size, Format, Args);
    va_end(Args);
    return Result;
}

VOID
QzPutString(PCSTR String)
{
    ULONG Flags = QzSaveFlagsAndDisable();
    while (*String)
    {
        QzSerialPutChar(*String);
        QzVgaPutChar(*String);
        String++;
    }
    QzRestoreFlags(Flags);
}

VOID
QzSetConsoleColor(UCHAR Attribute)
{
    QzVgaSetAttribute(Attribute);
}

static VOID
QzVPrint(PCSTR Format, va_list Args)
{
    CHAR Buffer[512];
    QzVsnprintf(Buffer, sizeof(Buffer), Format, Args);
    QzPutString(Buffer);
}

VOID
QzPrint(PCSTR Format, ...)
{
    va_list Args;
    va_start(Args, Format);
    QzVPrint(Format, Args);
    va_end(Args);
}

/* ---------------------------------------------------------------------- */
/* Exports                                                                 */
/* ---------------------------------------------------------------------- */

ULONG
__cdecl
DbgPrint(IN PCCH Format, ...)
{
    va_list Args;
    va_start(Args, Format);
    QzVPrint(Format, Args);
    va_end(Args);
    return STATUS_SUCCESS;
}

ULONG
__cdecl
DbgPrintEx(IN ULONG ComponentId,
           IN ULONG Level,
           IN PCCH Format,
           ...)
{
    va_list Args;

    UNREFERENCED_PARAMETER(ComponentId);

    /* Default NT filtering: only error-level messages are shown */
    if (!(Level == DPFLTR_ERROR_LEVEL || (Level > 31 && (Level & 1))))
        return STATUS_SUCCESS;

    va_start(Args, Format);
    QzVPrint(Format, Args);
    va_end(Args);
    return STATUS_SUCCESS;
}

int
__cdecl
_vsnprintf(char *Buffer, size_t Count, const char *Format, va_list Args)
{
    int Result = QzVsnprintf(Buffer, Count, Format, Args);
    /* MSVCRT semantics: -1 when the output did not fit */
    return ((size_t)Result >= Count) ? -1 : Result;
}

int
__cdecl
_snprintf(char *Buffer, size_t Count, const char *Format, ...)
{
    va_list Args;
    int Result;

    va_start(Args, Format);
    Result = _vsnprintf(Buffer, Count, Format, Args);
    va_end(Args);
    return Result;
}

int
__cdecl
sprintf(char *Buffer, const char *Format, ...)
{
    va_list Args;
    int Result;

    va_start(Args, Format);
    Result = QzVsnprintf(Buffer, 0x7FFFFFFF, Format, Args);
    va_end(Args);
    return Result;
}
