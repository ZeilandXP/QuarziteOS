/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     OEM code page strings, counted strings, 8.3 names
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * The OEM code page (437 on an English system: what FAT short names and
 * CD-ROM file identifiers are in) comes from the loader's NLS data, the
 * same c_437.nls user mode gets. Only single-byte code pages are
 * supported: NlsMbOemCodePageTag stays FALSE.
 */

#include "qz.h"

BOOLEAN NlsMbCodePageTag = FALSE;
BOOLEAN NlsMbOemCodePageTag = FALSE;
static USHORT QzNoLeadBytes[256];
PUSHORT NlsOemLeadByteInfo = QzNoLeadBytes;
PUSHORT NlsLeadByteInfo = QzNoLeadBytes;
USHORT NlsAnsiCodePage = 1252;
USHORT NlsOemCodePage = 437;

static PUSHORT QzOemToUnicode;      /* 256 entries */
static PUCHAR QzUnicodeToOem;       /* 65536 entries */
static UCHAR QzOemDefault = '?';

/* The NLS file: a header, the byte -> WCHAR table, then WCHAR -> byte */
typedef struct _QZ_NLS_HEADER
{
    USHORT HeaderSize;              /* in USHORTs */
    USHORT CodePage;
    USHORT MaximumCharacterSize;
    USHORT DefaultChar;
    USHORT UniDefaultChar;
    USHORT TransDefaultChar;
    USHORT TransUniDefaultChar;
    UCHAR LeadByte[12];
} QZ_NLS_HEADER, *PQZ_NLS_HEADER;

static VOID
QzRtlInitOem(VOID)
{
    PNLS_DATA_BLOCK Nls = QzLoaderBlock ? QzLoaderBlock->NlsData : NULL;
    PQZ_NLS_HEADER Header;
    PUSHORT Base;

    if (QzOemToUnicode || !Nls || !Nls->OemCodePageData)
        return;
    Header = Nls->OemCodePageData;
    if (Header->MaximumCharacterSize != 1)
        return;                         /* a DBCS code page: Latin-1 instead */
    /* Base: the size (in USHORTs, counted from after itself) of what
       comes before the WCHAR -> byte table */
    Base = (PUSHORT)Header + Header->HeaderSize;
    QzUnicodeToOem = (PUCHAR)(Base + 1 + *Base);
    QzOemDefault = (UCHAR)Header->DefaultChar;
    NlsOemCodePage = Header->CodePage;
    QzOemToUnicode = Base + 1;
}

static WCHAR
QzOemToWide(UCHAR c)
{
    QzRtlInitOem();
    return QzOemToUnicode ? QzOemToUnicode[c] : (WCHAR)c;
}

static UCHAR
QzWideToOem(WCHAR c)
{
    QzRtlInitOem();
    if (QzUnicodeToOem)
        return QzUnicodeToOem[c];
    return c < 0x100 ? (UCHAR)c : QzOemDefault;
}

/* A character that survives the round trip through the OEM code page */
static BOOLEAN
QzIsOemCharacter(WCHAR c)
{
    return QzOemToWide(QzWideToOem(c)) == c;
}

WCHAR
NTAPI
RtlDowncaseUnicodeChar(IN WCHAR Source)
{
    if (Source >= L'A' && Source <= L'Z')
        return Source + (L'a' - L'A');
    if (Source >= 0xC0 && Source <= 0xDE && Source != 0xD7)
        return Source + 0x20;
    return Source;
}

CHAR
NTAPI
RtlUpperChar(IN CHAR Source)
{
    return (Source >= 'a' && Source <= 'z') ? Source - ('a' - 'A') : Source;
}

/* ---------------------------------------------------------------------- */
/* OEM <-> Unicode                                                         */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
RtlOemToUnicodeN(OUT PWCHAR UnicodeString, IN ULONG MaxBytesInUnicodeString, OUT PULONG BytesInUnicodeString OPTIONAL,
                 IN PCCH OemString, IN ULONG BytesInOemString)
{
    ULONG Count = min(MaxBytesInUnicodeString / sizeof(WCHAR), BytesInOemString), i;

    for (i = 0; i < Count; i++)
        UnicodeString[i] = QzOemToWide((UCHAR)OemString[i]);
    if (BytesInUnicodeString)
        *BytesInUnicodeString = Count * sizeof(WCHAR);
    return Count < BytesInOemString ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlUnicodeToOemN(OUT PCHAR OemString, IN ULONG MaxBytesInOemString, OUT PULONG BytesInOemString OPTIONAL,
                 IN PCWCH UnicodeString, IN ULONG BytesInUnicodeString)
{
    ULONG Count = min(MaxBytesInOemString, BytesInUnicodeString / sizeof(WCHAR)), i;

    for (i = 0; i < Count; i++)
        OemString[i] = (CHAR)QzWideToOem(UnicodeString[i]);
    if (BytesInOemString)
        *BytesInOemString = Count;
    return Count < BytesInUnicodeString / sizeof(WCHAR) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlUpcaseUnicodeToOemN(OUT PCHAR OemString, IN ULONG MaxBytesInOemString, OUT PULONG BytesInOemString OPTIONAL,
                       IN PCWCH UnicodeString, IN ULONG BytesInUnicodeString)
{
    ULONG Count = min(MaxBytesInOemString, BytesInUnicodeString / sizeof(WCHAR)), i;

    for (i = 0; i < Count; i++)
        OemString[i] = (CHAR)QzWideToOem(RtlUpcaseUnicodeChar(UnicodeString[i]));
    if (BytesInOemString)
        *BytesInOemString = Count;
    return Count < BytesInUnicodeString / sizeof(WCHAR) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

ULONG
NTAPI
RtlxOemStringToUnicodeSize(IN PCOEM_STRING OemString)
{
    return (OemString->Length + 1) * sizeof(WCHAR);
}

ULONG
NTAPI
RtlxUnicodeStringToOemSize(IN PCUNICODE_STRING UnicodeString)
{
    return UnicodeString->Length / sizeof(WCHAR) + 1;
}

ULONG
NTAPI
RtlxAnsiStringToUnicodeSize(IN PCANSI_STRING AnsiString)
{
    return (AnsiString->Length + 1) * sizeof(WCHAR);
}

ULONG
NTAPI
RtlxUnicodeStringToAnsiSize(IN PCUNICODE_STRING UnicodeString)
{
    return UnicodeString->Length / sizeof(WCHAR) + 1;
}

static NTSTATUS
QzOemToUnicodeString(PUNICODE_STRING Destination, PCOEM_STRING Source, BOOLEAN Allocate, BOOLEAN Counted)
{
    ULONG Bytes = Source->Length * sizeof(WCHAR), Room = Bytes + (Counted ? 0 : sizeof(WCHAR));

    if (Room > MAXUSHORT)
        return STATUS_INVALID_PARAMETER_2;
    if (Allocate)
    {
        Destination->Buffer = ExAllocatePoolWithTag(PagedPool, max(Room, sizeof(WCHAR)), 'mOzQ');
        if (!Destination->Buffer)
            return STATUS_NO_MEMORY;
        Destination->MaximumLength = (USHORT)Room;
    }
    else if (Destination->MaximumLength < Room)
    {
        return STATUS_BUFFER_OVERFLOW;
    }
    RtlOemToUnicodeN(Destination->Buffer, Bytes, NULL, Source->Buffer, Source->Length);
    Destination->Length = (USHORT)Bytes;
    if (!Counted)
        Destination->Buffer[Bytes / sizeof(WCHAR)] = UNICODE_NULL;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlOemStringToUnicodeString(IN OUT PUNICODE_STRING DestinationString, IN PCOEM_STRING SourceString,
                            IN BOOLEAN AllocateDestinationString)
{
    return QzOemToUnicodeString(DestinationString, SourceString, AllocateDestinationString, FALSE);
}

NTSTATUS
NTAPI
RtlOemStringToCountedUnicodeString(IN OUT PUNICODE_STRING DestinationString, IN PCOEM_STRING SourceString,
                                   IN BOOLEAN AllocateDestinationString)
{
    return QzOemToUnicodeString(DestinationString, SourceString, AllocateDestinationString, TRUE);
}

static NTSTATUS
QzUnicodeToOemString(POEM_STRING Destination, PCUNICODE_STRING Source, BOOLEAN Allocate, BOOLEAN Counted,
                     BOOLEAN Upcase)
{
    ULONG Chars = Source->Length / sizeof(WCHAR), Room = Chars + (Counted ? 0 : 1);

    if (Room > MAXUSHORT)
        return STATUS_INVALID_PARAMETER_2;
    if (Allocate)
    {
        Destination->Buffer = ExAllocatePoolWithTag(PagedPool, max(Room, 1), 'mOzQ');
        if (!Destination->Buffer)
            return STATUS_NO_MEMORY;
        Destination->MaximumLength = (USHORT)Room;
    }
    else if (Destination->MaximumLength < Room)
    {
        return STATUS_BUFFER_OVERFLOW;
    }
    if (Upcase)
        RtlUpcaseUnicodeToOemN(Destination->Buffer, Chars, NULL, Source->Buffer, Source->Length);
    else
        RtlUnicodeToOemN(Destination->Buffer, Chars, NULL, Source->Buffer, Source->Length);
    Destination->Length = (USHORT)Chars;
    if (!Counted)
        Destination->Buffer[Chars] = 0;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlUnicodeStringToOemString(IN OUT POEM_STRING DestinationString, IN PCUNICODE_STRING SourceString,
                            IN BOOLEAN AllocateDestinationString)
{
    return QzUnicodeToOemString(DestinationString, SourceString, AllocateDestinationString, FALSE, FALSE);
}

NTSTATUS
NTAPI
RtlUnicodeStringToCountedOemString(IN OUT POEM_STRING DestinationString, IN PCUNICODE_STRING SourceString,
                                   IN BOOLEAN AllocateDestinationString)
{
    return QzUnicodeToOemString(DestinationString, SourceString, AllocateDestinationString, TRUE, FALSE);
}

NTSTATUS
NTAPI
RtlUpcaseUnicodeStringToOemString(IN OUT POEM_STRING DestinationString, IN PCUNICODE_STRING SourceString,
                                  IN BOOLEAN AllocateDestinationString)
{
    return QzUnicodeToOemString(DestinationString, SourceString, AllocateDestinationString, FALSE, TRUE);
}

NTSTATUS
NTAPI
RtlUpcaseUnicodeStringToCountedOemString(IN OUT POEM_STRING DestinationString, IN PCUNICODE_STRING SourceString,
                                         IN BOOLEAN AllocateDestinationString)
{
    return QzUnicodeToOemString(DestinationString, SourceString, AllocateDestinationString, TRUE, TRUE);
}

VOID
NTAPI
RtlFreeOemString(IN POEM_STRING OemString)
{
    if (OemString->Buffer)
        ExFreePool(OemString->Buffer);
    OemString->Buffer = NULL;
    OemString->Length = OemString->MaximumLength = 0;
}

/* ---------------------------------------------------------------------- */
/* Counted 8-bit strings, case                                             */
/* ---------------------------------------------------------------------- */

LONG
NTAPI
RtlCompareString(IN const STRING *String1, IN const STRING *String2, IN BOOLEAN CaseInSensitive)
{
    USHORT Length = min(String1->Length, String2->Length), i;

    for (i = 0; i < Length; i++)
    {
        CHAR a = String1->Buffer[i], b = String2->Buffer[i];
        if (CaseInSensitive)
        {
            a = RtlUpperChar(a);
            b = RtlUpperChar(b);
        }
        if (a != b)
            return (LONG)(UCHAR)a - (LONG)(UCHAR)b;
    }
    return (LONG)String1->Length - (LONG)String2->Length;
}

VOID
NTAPI
RtlUpperString(OUT PSTRING DestinationString, IN const STRING *SourceString)
{
    USHORT Length = min(SourceString->Length, DestinationString->MaximumLength), i;

    for (i = 0; i < Length; i++)
        DestinationString->Buffer[i] = RtlUpperChar(SourceString->Buffer[i]);
    DestinationString->Length = Length;
}

NTSTATUS
NTAPI
RtlDowncaseUnicodeString(IN OUT PUNICODE_STRING DestinationString, IN PCUNICODE_STRING SourceString,
                         IN BOOLEAN AllocateDestinationString)
{
    ULONG i;

    if (AllocateDestinationString)
    {
        DestinationString->Buffer = ExAllocatePoolWithTag(PagedPool, max(SourceString->Length, 1), 'mOzQ');
        if (!DestinationString->Buffer)
            return STATUS_NO_MEMORY;
        DestinationString->MaximumLength = SourceString->Length;
    }
    else if (SourceString->Length > DestinationString->MaximumLength)
    {
        return STATUS_BUFFER_OVERFLOW;
    }
    for (i = 0; i < SourceString->Length / sizeof(WCHAR); i++)
        DestinationString->Buffer[i] = RtlDowncaseUnicodeChar(SourceString->Buffer[i]);
    DestinationString->Length = SourceString->Length;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* 8.3 names                                                               */
/* ---------------------------------------------------------------------- */

/* Characters a FAT short name may hold besides letters and digits */
static BOOLEAN
QzIsShortNameChar(WCHAR c)
{
    static const WCHAR Bad[] = L"\"*+,./:;<=>?[\\]| ";
    ULONG i;
    if (c < 0x20 || c == 0x7F)
        return FALSE;
    for (i = 0; Bad[i]; i++)
    {
        if (c == Bad[i])
            return FALSE;
    }
    return TRUE;
}

/* Is Name already a valid upper-case 8.3 name (as OEM, in OemName)? */
BOOLEAN
NTAPI
RtlIsNameLegalDOS8Dot3(IN PCUNICODE_STRING Name, IN OUT POEM_STRING OemName OPTIONAL,
                       OUT PBOOLEAN NameContainsSpaces OPTIONAL)
{
    CHAR Buffer[12];
    OEM_STRING Oem;
    ULONG i, Dot = MAXULONG, Length = Name->Length / sizeof(WCHAR);
    BOOLEAN Spaces = FALSE;

    if (!Length || Length > 12)
        return FALSE;
    if (!OemName)
    {
        Oem.Buffer = Buffer;
        Oem.MaximumLength = sizeof(Buffer);
        OemName = &Oem;
    }
    if (!NT_SUCCESS(RtlUpcaseUnicodeStringToCountedOemString(OemName, Name, FALSE)))
        return FALSE;

    /* "." and ".." */
    if ((Length == 1 && Name->Buffer[0] == L'.') ||
        (Length == 2 && Name->Buffer[0] == L'.' && Name->Buffer[1] == L'.'))
        return TRUE;

    for (i = 0; i < Length; i++)
    {
        WCHAR c = Name->Buffer[i];
        if (c == L'.')
        {
            if (Dot != MAXULONG || i == 0 || i > 8 || Length - i - 1 > 3)
                return FALSE;
            Dot = i;
            continue;
        }
        if (c == L' ')
        {
            Spaces = TRUE;
            continue;
        }
        if (!QzIsShortNameChar(c) || !QzIsOemCharacter(c) || RtlUpcaseUnicodeChar(c) != c)
            return FALSE;
    }
    if (Dot == MAXULONG && Length > 8)
        return FALSE;
    if (Name->Buffer[Length - 1] == L' ' || Name->Buffer[0] == L' ' || (Dot != MAXULONG && Dot == Length - 1))
        return FALSE;
    if (NameContainsSpaces)
        *NameContainsSpaces = Spaces;
    return TRUE;
}

/*
 * A short name for Name: the first characters of the base (legal ones,
 * upper case), "~N", and the first three of the extension. The context
 * keeps the base between calls; each call gives the next N. After four,
 * the base becomes two characters and a checksum of the long name, as
 * Windows does, so that many similar names do not all crowd one base.
 */
VOID
NTAPI
RtlGenerate8dot3Name(IN PCUNICODE_STRING Name, IN BOOLEAN AllowExtendedCharacters,
                     IN OUT PGENERATE_NAME_CONTEXT Context, OUT PUNICODE_STRING Name8dot3)
{
    ULONG Length = Name->Length / sizeof(WCHAR), i, Last = MAXULONG, Out, Digits, Value;
    WCHAR Number[8];

    if (!Context->NameLength)
    {
        /* The extension: after the last dot */
        for (i = 0; i < Length; i++)
            if (Name->Buffer[i] == L'.')
                Last = i;
        Context->ExtensionLength = 0;
        if (Last != MAXULONG)
        {
            for (i = Last + 1; i < Length && Context->ExtensionLength < 4; i++)
            {
                WCHAR c = RtlUpcaseUnicodeChar(Name->Buffer[i]);
                if (!QzIsShortNameChar(c))
                    continue;
                if (!QzIsOemCharacter(c) || (!AllowExtendedCharacters && c > 0x7F))
                    c = L'_';
                if (!Context->ExtensionLength)
                    Context->ExtensionBuffer[Context->ExtensionLength++] = L'.';
                Context->ExtensionBuffer[Context->ExtensionLength++] = c;
            }
        }
        /* The base: up to six characters before it */
        for (i = 0; i < (Last == MAXULONG ? Length : Last) && Context->NameLength < 6; i++)
        {
            WCHAR c = RtlUpcaseUnicodeChar(Name->Buffer[i]);
            if (!QzIsShortNameChar(c))
                continue;
            if (!QzIsOemCharacter(c) || (!AllowExtendedCharacters && c > 0x7F))
                c = L'_';
            Context->NameBuffer[Context->NameLength++] = c;
        }
        if (!Context->NameLength)
            Context->NameBuffer[Context->NameLength++] = L'_';
        Context->LastIndexValue = 0;
        Context->CheckSumInserted = FALSE;
    }

    Context->LastIndexValue++;
    if (Context->LastIndexValue > 4 && !Context->CheckSumInserted)
    {
        USHORT Checksum = 0;
        for (i = 0; i < Length; i++)
            Checksum = (USHORT)(((Checksum & 1) ? 0x8000 : 0) + (Checksum >> 1) + Name->Buffer[i]);
        Context->Checksum = Checksum;
        if (Context->NameLength > 2)
            Context->NameLength = 2;
        for (i = 0; i < 4; i++)
            Context->NameBuffer[Context->NameLength + i] = L"0123456789ABCDEF"[(Checksum >> (12 - 4 * i)) & 0xF];
        Context->NameLength += 4;
        Context->CheckSumInserted = TRUE;
        Context->LastIndexValue = 1;
    }

    /* "~N", N without leading zeros */
    Value = Context->LastIndexValue;
    Digits = 0;
    do
    {
        Number[Digits++] = (WCHAR)(L'0' + Value % 10);
        Value /= 10;
    } while (Value && Digits < RTL_NUMBER_OF(Number));

    Out = 0;
    for (i = 0; i < Context->NameLength && i < 8 - Digits - 1; i++)
        Name8dot3->Buffer[Out++] = Context->NameBuffer[i];
    Name8dot3->Buffer[Out++] = L'~';
    while (Digits)
        Name8dot3->Buffer[Out++] = Number[--Digits];
    for (i = 0; i < Context->ExtensionLength; i++)
        Name8dot3->Buffer[Out++] = Context->ExtensionBuffer[i];
    Name8dot3->Length = (USHORT)(Out * sizeof(WCHAR));
}
