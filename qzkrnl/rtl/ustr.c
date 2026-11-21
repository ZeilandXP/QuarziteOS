/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Counted string routines (UNICODE_STRING / ANSI_STRING)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Case folding covers ASCII and Latin-1 until NLS tables are loaded
 * (configuration manager stage); ANSI <-> Unicode is Latin-1 for now.
 */

#include "qz.h"

#define QZ_STRING_TAG 'rtSQ'

WCHAR
NTAPI
RtlUpcaseUnicodeChar(IN WCHAR Source)
{
    if (Source >= L'a' && Source <= L'z')
        return Source - (L'a' - L'A');
    /* Latin-1 lowercase letters, except the multiplication sign and y-diaeresis */
    if (Source >= 0xE0 && Source <= 0xFE && Source != 0xF7)
        return Source - 0x20;
    return Source;
}

LONG
NTAPI
RtlCompareUnicodeString(IN PCUNICODE_STRING String1,
                        IN PCUNICODE_STRING String2,
                        IN BOOLEAN CaseInSensitive)
{
    USHORT Len1 = String1->Length / sizeof(WCHAR), Len2 = String2->Length / sizeof(WCHAR);
    USHORT i, Min = min(Len1, Len2);
    WCHAR a, b;

    for (i = 0; i < Min; i++)
    {
        a = String1->Buffer[i];
        b = String2->Buffer[i];
        if (CaseInSensitive)
        {
            a = RtlUpcaseUnicodeChar(a);
            b = RtlUpcaseUnicodeChar(b);
        }
        if (a != b)
            return (LONG)a - (LONG)b;
    }
    return (LONG)Len1 - (LONG)Len2;
}

BOOLEAN
NTAPI
RtlEqualUnicodeString(IN PCUNICODE_STRING String1,
                      IN PCUNICODE_STRING String2,
                      IN BOOLEAN CaseInSensitive)
{
    if (String1->Length != String2->Length)
        return FALSE;
    return RtlCompareUnicodeString(String1, String2, CaseInSensitive) == 0;
}

BOOLEAN
NTAPI
RtlPrefixUnicodeString(IN PCUNICODE_STRING String1,
                       IN PCUNICODE_STRING String2,
                       IN BOOLEAN CaseInSensitive)
{
    UNICODE_STRING Head;

    if (String1->Length > String2->Length)
        return FALSE;
    Head.Buffer = String2->Buffer;
    Head.Length = Head.MaximumLength = String1->Length;
    return RtlEqualUnicodeString(String1, &Head, CaseInSensitive);
}

VOID
NTAPI
RtlCopyUnicodeString(IN OUT PUNICODE_STRING DestinationString,
                     IN PCUNICODE_STRING SourceString OPTIONAL)
{
    USHORT Length;

    if (!SourceString)
    {
        DestinationString->Length = 0;
        return;
    }
    Length = min(SourceString->Length, DestinationString->MaximumLength);
    RtlMoveMemory(DestinationString->Buffer, SourceString->Buffer, Length);
    DestinationString->Length = Length;
    if (Length + sizeof(WCHAR) <= DestinationString->MaximumLength)
        DestinationString->Buffer[Length / sizeof(WCHAR)] = UNICODE_NULL;
}

NTSTATUS
NTAPI
RtlAppendUnicodeStringToString(IN OUT PUNICODE_STRING Destination,
                               IN PCUNICODE_STRING Source)
{
    USHORT NewLength = Destination->Length + Source->Length;

    if (NewLength > Destination->MaximumLength)
        return STATUS_BUFFER_TOO_SMALL;

    RtlMoveMemory((PUCHAR)Destination->Buffer + Destination->Length, Source->Buffer,
                  Source->Length);
    Destination->Length = NewLength;
    if (NewLength + sizeof(WCHAR) <= Destination->MaximumLength)
        Destination->Buffer[NewLength / sizeof(WCHAR)] = UNICODE_NULL;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlAppendUnicodeToString(IN OUT PUNICODE_STRING Destination,
                         IN PCWSTR Source OPTIONAL)
{
    UNICODE_STRING Temp;

    if (!Source)
        return STATUS_SUCCESS;
    RtlInitUnicodeString(&Temp, Source);
    return RtlAppendUnicodeStringToString(Destination, &Temp);
}

BOOLEAN
NTAPI
RtlCreateUnicodeString(OUT PUNICODE_STRING DestinationString,
                       IN PCWSTR SourceString)
{
    SIZE_T Size = (wcslen(SourceString) + 1) * sizeof(WCHAR);

    if (Size > MAXUSHORT)
        return FALSE;
    DestinationString->Buffer = ExAllocatePoolWithTag(PagedPool, Size, QZ_STRING_TAG);
    if (!DestinationString->Buffer)
        return FALSE;
    RtlCopyMemory(DestinationString->Buffer, SourceString, Size);
    DestinationString->MaximumLength = (USHORT)Size;
    DestinationString->Length = (USHORT)(Size - sizeof(WCHAR));
    return TRUE;
}

VOID
NTAPI
RtlFreeUnicodeString(IN PUNICODE_STRING UnicodeString)
{
    if (UnicodeString->Buffer)
        ExFreePoolWithTag(UnicodeString->Buffer, QZ_STRING_TAG);
    UnicodeString->Buffer = NULL;
    UnicodeString->Length = UnicodeString->MaximumLength = 0;
}

NTSTATUS
NTAPI
RtlUpcaseUnicodeString(IN OUT PUNICODE_STRING DestinationString,
                       IN PCUNICODE_STRING SourceString,
                       IN BOOLEAN AllocateDestinationString)
{
    USHORT i;

    if (AllocateDestinationString)
    {
        DestinationString->MaximumLength = SourceString->Length;
        DestinationString->Buffer = ExAllocatePoolWithTag(PagedPool,
                                                          max(SourceString->Length, 1),
                                                          QZ_STRING_TAG);
        if (!DestinationString->Buffer)
            return STATUS_NO_MEMORY;
    }
    else if (SourceString->Length > DestinationString->MaximumLength)
    {
        return STATUS_BUFFER_OVERFLOW;
    }

    for (i = 0; i < SourceString->Length / sizeof(WCHAR); i++)
        DestinationString->Buffer[i] = RtlUpcaseUnicodeChar(SourceString->Buffer[i]);
    DestinationString->Length = SourceString->Length;
    return STATUS_SUCCESS;
}

VOID
NTAPI
RtlInitString(IN OUT PSTRING DestinationString,
              IN PCSZ SourceString)
{
    RtlInitAnsiString(DestinationString, SourceString);
}

BOOLEAN
NTAPI
RtlEqualString(IN const STRING *String1,
               IN const STRING *String2,
               IN BOOLEAN CaseInSensitive)
{
    USHORT i;
    CHAR a, b;

    if (String1->Length != String2->Length)
        return FALSE;
    for (i = 0; i < String1->Length; i++)
    {
        a = String1->Buffer[i];
        b = String2->Buffer[i];
        if (CaseInSensitive)
        {
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
        }
        if (a != b)
            return FALSE;
    }
    return TRUE;
}

VOID
NTAPI
RtlCopyString(OUT PSTRING DestinationString,
              IN const STRING *SourceString OPTIONAL)
{
    USHORT Length;

    if (!SourceString)
    {
        DestinationString->Length = 0;
        return;
    }
    Length = min(SourceString->Length, DestinationString->MaximumLength);
    RtlMoveMemory(DestinationString->Buffer, SourceString->Buffer, Length);
    DestinationString->Length = Length;
}

WCHAR
NTAPI
RtlAnsiCharToUnicodeChar(IN OUT PUCHAR *SourceCharacter)
{
    /* Latin-1 until the ANSI code page is loaded */
    return (WCHAR)*(*SourceCharacter)++;
}

NTSTATUS
NTAPI
RtlAnsiStringToUnicodeString(IN OUT PUNICODE_STRING DestinationString,
                             IN PCANSI_STRING SourceString,
                             IN BOOLEAN AllocateDestinationString)
{
    ULONG Size = SourceString->Length * sizeof(WCHAR);
    USHORT i;

    if (Size > MAXUSHORT - sizeof(WCHAR))
        return STATUS_INVALID_PARAMETER_2;

    if (AllocateDestinationString)
    {
        DestinationString->MaximumLength = (USHORT)(Size + sizeof(WCHAR));
        DestinationString->Buffer = ExAllocatePoolWithTag(PagedPool, Size + sizeof(WCHAR),
                                                          QZ_STRING_TAG);
        if (!DestinationString->Buffer)
            return STATUS_NO_MEMORY;
    }
    else if (Size > DestinationString->MaximumLength)
    {
        return STATUS_BUFFER_OVERFLOW;
    }

    for (i = 0; i < SourceString->Length; i++)
        DestinationString->Buffer[i] = (UCHAR)SourceString->Buffer[i];
    DestinationString->Length = (USHORT)Size;
    if (Size + sizeof(WCHAR) <= DestinationString->MaximumLength)
        DestinationString->Buffer[i] = UNICODE_NULL;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlUnicodeStringToAnsiString(IN OUT PANSI_STRING DestinationString,
                             IN PCUNICODE_STRING SourceString,
                             IN BOOLEAN AllocateDestinationString)
{
    USHORT Chars = SourceString->Length / sizeof(WCHAR), i;
    NTSTATUS Status = STATUS_SUCCESS;

    if (AllocateDestinationString)
    {
        DestinationString->MaximumLength = Chars + 1;
        DestinationString->Buffer = ExAllocatePoolWithTag(PagedPool, Chars + 1, QZ_STRING_TAG);
        if (!DestinationString->Buffer)
            return STATUS_NO_MEMORY;
    }
    else if (Chars >= DestinationString->MaximumLength)
    {
        if (DestinationString->MaximumLength == 0)
            return STATUS_BUFFER_OVERFLOW;
        Chars = DestinationString->MaximumLength - 1;
        Status = STATUS_BUFFER_OVERFLOW;
    }

    for (i = 0; i < Chars; i++)
    {
        WCHAR c = SourceString->Buffer[i];
        DestinationString->Buffer[i] = (c < 0x100) ? (CHAR)c : '?';
    }
    DestinationString->Buffer[Chars] = 0;
    DestinationString->Length = Chars;
    return Status;
}

VOID
NTAPI
RtlFreeAnsiString(IN PANSI_STRING AnsiString)
{
    if (AnsiString->Buffer)
        ExFreePoolWithTag(AnsiString->Buffer, QZ_STRING_TAG);
    AnsiString->Buffer = NULL;
    AnsiString->Length = AnsiString->MaximumLength = 0;
}

NTSTATUS
NTAPI
RtlIntegerToUnicodeString(IN ULONG Value,
                          IN ULONG Base OPTIONAL,
                          IN OUT PUNICODE_STRING String)
{
    WCHAR Digits[33];
    ULONG Count = 0, i;

    if (Base == 0)
        Base = 10;
    if (Base != 2 && Base != 8 && Base != 10 && Base != 16)
        return STATUS_INVALID_PARAMETER;

    do
    {
        ULONG d = Value % Base;
        Digits[Count++] = (WCHAR)(d < 10 ? L'0' + d : L'A' + d - 10);
        Value /= Base;
    } while (Value);

    if ((Count + 1) * sizeof(WCHAR) > String->MaximumLength)
        return STATUS_BUFFER_OVERFLOW;
    for (i = 0; i < Count; i++)
        String->Buffer[i] = Digits[Count - 1 - i];
    String->Buffer[Count] = UNICODE_NULL;
    String->Length = (USHORT)(Count * sizeof(WCHAR));
    return STATUS_SUCCESS;
}
