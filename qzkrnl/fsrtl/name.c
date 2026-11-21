/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     File system runtime: names, wildcards, legal characters
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Wildcard matching is the NT dialect: '*' and '?', and the DOS forms the
 * I/O manager substitutes for them: '<' (DOS_STAR: anything up to the last
 * dot), '>' (DOS_QM: one character, or none before a dot or the end) and
 * '"' (DOS_DOT: a dot, or none at the end). It runs as a set of positions
 * in the expression stepped once per character of the name, so it is
 * linear however many stars there are.
 */

#include "qz.h"

#define QZ_FAT   FSRTL_FAT_LEGAL
#define QZ_HPFS  FSRTL_HPFS_LEGAL
#define QZ_NTFS  FSRTL_NTFS_LEGAL
#define QZ_WILD  FSRTL_WILD_CHARACTER
#define QZ_OLE   FSRTL_OLE_LEGAL
#define QZ_ALL   (QZ_FAT | QZ_HPFS | QZ_NTFS | QZ_OLE)
#define QZ_NOFAT (QZ_HPFS | QZ_NTFS | QZ_OLE)

static UCHAR QzLegalCharacters[256] =
{
    /* 0x00-0x1F: control characters (NTFS stream names only) */
    0, QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE,
    QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE,
    QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE,
    QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE, QZ_OLE,
    /*   ! " # $ % & ' */
    QZ_ALL, QZ_ALL, QZ_WILD, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL,
    /* ( ) * + , - . / */
    QZ_ALL, QZ_ALL, QZ_WILD, QZ_NOFAT, QZ_NOFAT, QZ_ALL, QZ_FAT | QZ_HPFS | QZ_NTFS, 0,
    /* 0-7 */
    QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL,
    /* 8 9 : ; < = > ? */
    QZ_ALL, QZ_ALL, 0, QZ_NOFAT, QZ_WILD, QZ_NOFAT, QZ_WILD, QZ_WILD,
    /* @ A-G */
    QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL,
    /* H-O */
    QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL,
    /* P-W */
    QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL,
    /* X Y Z [ \ ] ^ _ */
    QZ_ALL, QZ_ALL, QZ_ALL, QZ_NOFAT, 0, QZ_NOFAT, QZ_ALL, QZ_ALL,
    /* ` a-g */
    QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL,
    /* h-o */
    QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL,
    /* p-w */
    QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL,
    /* x y z { | } ~ DEL */
    QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, 0, QZ_ALL, QZ_ALL, QZ_NOFAT,
    /* 0x80-0xFF: OEM characters, legal everywhere */
#define QZ_ROW QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL, QZ_ALL
    QZ_ROW, QZ_ROW, QZ_ROW, QZ_ROW, QZ_ROW, QZ_ROW, QZ_ROW, QZ_ROW,
    QZ_ROW, QZ_ROW, QZ_ROW, QZ_ROW, QZ_ROW, QZ_ROW, QZ_ROW, QZ_ROW,
#undef QZ_ROW
};

const UCHAR * const FsRtlLegalAnsiCharacterArray = QzLegalCharacters;

/* ---------------------------------------------------------------------- */
/* Wildcards                                                               */
/* ---------------------------------------------------------------------- */

static BOOLEAN
QzIsWild(WCHAR c)
{
    return c == L'*' || c == L'?' || c == DOS_STAR || c == DOS_QM || c == DOS_DOT;
}

BOOLEAN
NTAPI
FsRtlDoesNameContainWildCards(IN PUNICODE_STRING Name)
{
    ULONG i;
    for (i = Name->Length / sizeof(WCHAR); i; i--)
    {
        WCHAR c = Name->Buffer[i - 1];
        if (c == L'\\')
            break;                          /* only the last component counts */
        if (QzIsWild(c))
            return TRUE;
    }
    return FALSE;
}

BOOLEAN
NTAPI
FsRtlDoesDbcsContainWildCards(IN PANSI_STRING Name)
{
    ULONG i;
    for (i = 0; i < Name->Length; i++)
    {
        if (QzIsWild((UCHAR)Name->Buffer[i]))
            return TRUE;
    }
    return FALSE;
}

/*
 * The matcher, on characters of either width. States[p] says position p of
 * the expression is reachable; Upcase folds the name's characters (the
 * expression is upper case already when case does not matter).
 */
#define QZ_MAX_EXPRESSION 512

typedef WCHAR (*QZ_CHAR_AT)(const VOID *String, ULONG Index);
static WCHAR QzWideAt(const VOID *String, ULONG Index) { return ((PCWCH)String)[Index]; }
static WCHAR QzNarrowAt(const VOID *String, ULONG Index) { return (UCHAR)((PCCH)String)[Index]; }

static BOOLEAN
QzMatch(const VOID *Expression, ULONG ExpressionLength, const VOID *Name, ULONG NameLength, QZ_CHAR_AT At,
        BOOLEAN IgnoreCase, PCWCH UpcaseTable)
{
    BOOLEAN StackA[QZ_MAX_EXPRESSION + 1], StackB[QZ_MAX_EXPRESSION + 1];
    PBOOLEAN Current = StackA, Next = StackB, Swap;
    ULONG i, p, LastDot = MAXULONG;
    BOOLEAN Any;

    if (ExpressionLength > QZ_MAX_EXPRESSION)
        return FALSE;
    for (i = 0; i < NameLength; i++)
        if (At(Name, i) == L'.')
            LastDot = i;

    RtlZeroMemory(Current, ExpressionLength + 1);
    Current[0] = TRUE;
    for (i = 0; ; i++)
    {
        BOOLEAN AtEnd = (i == NameLength);
        WCHAR c = AtEnd ? 0 : At(Name, i);
        BOOLEAN BeforeDotOrEnd = AtEnd || c == L'.';

        if (!AtEnd && IgnoreCase)
            c = UpcaseTable ? UpcaseTable[c] : RtlUpcaseUnicodeChar(c);

        /* Zero-length matches: a star or DOS_STAR may match nothing, DOS_QM
           nothing before a dot or the end, DOS_DOT nothing at the end */
        for (p = 0; p < ExpressionLength; p++)
        {
            WCHAR e;
            if (!Current[p])
                continue;
            e = At(Expression, p);
            if (e == L'*' || e == DOS_STAR || (e == DOS_QM && BeforeDotOrEnd) || (e == DOS_DOT && AtEnd))
                Current[p + 1] = TRUE;
        }
        if (AtEnd)
            return Current[ExpressionLength];

        RtlZeroMemory(Next, ExpressionLength + 1);
        Any = FALSE;
        for (p = 0; p < ExpressionLength; p++)
        {
            WCHAR e;
            if (!Current[p])
                continue;
            e = At(Expression, p);
            if (e == L'*')
                Next[p] = TRUE;
            else if (e == DOS_STAR)
            {
                if (!(c == L'.' && i == LastDot))
                    Next[p] = TRUE;         /* anything but the last dot */
            }
            else if (e == L'?' || e == DOS_QM)
                Next[p + 1] = TRUE;
            else if (e == DOS_DOT)
            {
                if (c == L'.')
                    Next[p + 1] = TRUE;
            }
            else if (e == c)
                Next[p + 1] = TRUE;
        }
        for (p = 0; p <= ExpressionLength; p++)
            Any |= Next[p];
        if (!Any)
            return FALSE;
        Swap = Current;
        Current = Next;
        Next = Swap;
    }
}

BOOLEAN
NTAPI
RtlIsNameInExpression(IN PUNICODE_STRING Expression, IN PUNICODE_STRING Name, IN BOOLEAN IgnoreCase,
                        IN PWCHAR UpcaseTable OPTIONAL)
{
    return QzMatch(Expression->Buffer, Expression->Length / sizeof(WCHAR), Name->Buffer, Name->Length / sizeof(WCHAR),
                   QzWideAt, IgnoreCase, UpcaseTable);
}

BOOLEAN
NTAPI
FsRtlIsDbcsInExpression(IN PANSI_STRING Expression, IN PANSI_STRING Name)
{
    /* File systems pass upper-case OEM names */
    return QzMatch(Expression->Buffer, Expression->Length, Name->Buffer, Name->Length, QzNarrowAt, FALSE, NULL);
}

BOOLEAN
NTAPI
FsRtlAreNamesEqual(IN PCUNICODE_STRING Name1, IN PCUNICODE_STRING Name2, IN BOOLEAN IgnoreCase,
                   IN PCWCH UpcaseTable OPTIONAL)
{
    ULONG i;

    if (Name1->Length != Name2->Length)
        return FALSE;
    for (i = 0; i < Name1->Length / sizeof(WCHAR); i++)
    {
        WCHAR a = Name1->Buffer[i], b = Name2->Buffer[i];
        if (IgnoreCase)
        {
            a = UpcaseTable ? UpcaseTable[a] : RtlUpcaseUnicodeChar(a);
            b = UpcaseTable ? UpcaseTable[b] : RtlUpcaseUnicodeChar(b);
        }
        if (a != b)
            return FALSE;
    }
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Path components                                                         */
/* ---------------------------------------------------------------------- */

/* "\a\b\c" -> "a" and "b\c" */
VOID
NTAPI
FsRtlDissectName(IN UNICODE_STRING Path, OUT PUNICODE_STRING FirstName, OUT PUNICODE_STRING RemainingName)
{
    ULONG Length = Path.Length / sizeof(WCHAR), Start = 0, End;

    FirstName->Length = FirstName->MaximumLength = 0;
    FirstName->Buffer = NULL;
    RemainingName->Length = RemainingName->MaximumLength = 0;
    RemainingName->Buffer = NULL;
    if (!Length)
        return;
    if (Path.Buffer[0] == L'\\')
        Start = 1;
    for (End = Start; End < Length && Path.Buffer[End] != L'\\'; End++)
        ;
    FirstName->Buffer = Path.Buffer + Start;
    FirstName->Length = FirstName->MaximumLength = (USHORT)((End - Start) * sizeof(WCHAR));
    if (End + 1 < Length)
    {
        RemainingName->Buffer = Path.Buffer + End + 1;
        RemainingName->Length = RemainingName->MaximumLength = (USHORT)((Length - End - 1) * sizeof(WCHAR));
    }
}

VOID
NTAPI
FsRtlDissectDbcs(IN ANSI_STRING Name, OUT PANSI_STRING FirstPart, OUT PANSI_STRING RemainingPart)
{
    ULONG Start = 0, End;

    FirstPart->Length = FirstPart->MaximumLength = 0;
    FirstPart->Buffer = NULL;
    RemainingPart->Length = RemainingPart->MaximumLength = 0;
    RemainingPart->Buffer = NULL;
    if (!Name.Length)
        return;
    if (Name.Buffer[0] == '\\')
        Start = 1;
    for (End = Start; End < Name.Length && Name.Buffer[End] != '\\'; End++)
        ;
    FirstPart->Buffer = Name.Buffer + Start;
    FirstPart->Length = FirstPart->MaximumLength = (USHORT)(End - Start);
    if (End + 1 < Name.Length)
    {
        RemainingPart->Buffer = Name.Buffer + End + 1;
        RemainingPart->Length = RemainingPart->MaximumLength = (USHORT)(Name.Length - End - 1);
    }
}

/* ---------------------------------------------------------------------- */
/* FAT (8.3) and HPFS legality of OEM names                                */
/* ---------------------------------------------------------------------- */

static BOOLEAN
QzIsFatComponentLegal(PCCH Name, ULONG Length, BOOLEAN WildCards)
{
    ULONG i, Dot = MAXULONG;

    if (!Length)
        return FALSE;
    if ((Length == 1 && Name[0] == '.') || (Length == 2 && Name[0] == '.' && Name[1] == '.'))
        return TRUE;
    for (i = 0; i < Length; i++)
    {
        UCHAR c = (UCHAR)Name[i];
        if (c == '.')
        {
            if (Dot != MAXULONG && !WildCards)
                return FALSE;
            Dot = i;
            continue;
        }
        if (!(QzLegalCharacters[c] & (QZ_FAT | (WildCards ? QZ_WILD : 0))))
            return FALSE;
    }
    if (WildCards)
        return TRUE;                        /* an expression: lengths do not apply */
    if (Dot == MAXULONG)
        return Length <= 8 && Name[Length - 1] != ' ';
    if (Dot == 0 || Dot > 8 || Length - Dot - 1 > 3 || Dot == Length - 1)
        return FALSE;
    return Name[Dot - 1] != ' ' && Name[Length - 1] != ' ';
}

BOOLEAN
NTAPI
FsRtlIsFatDbcsLegal(IN ANSI_STRING DbcsName, IN BOOLEAN WildCardsPermissible, IN BOOLEAN PathNamePermissible,
                    IN BOOLEAN LeadingBackslashPermissible)
{
    ULONG Start = 0, i;

    if (!DbcsName.Length)
        return FALSE;
    if (DbcsName.Buffer[0] == '\\')
    {
        if (!LeadingBackslashPermissible)
            return FALSE;
        Start = 1;
        if (DbcsName.Length == 1)
            return TRUE;
    }
    for (i = Start; i <= DbcsName.Length; i++)
    {
        if (i == DbcsName.Length || DbcsName.Buffer[i] == '\\')
        {
            if (i < DbcsName.Length && !PathNamePermissible)
                return FALSE;
            if (!QzIsFatComponentLegal(DbcsName.Buffer + Start, i - Start, WildCardsPermissible))
                return FALSE;
            Start = i + 1;
        }
    }
    return TRUE;
}

BOOLEAN
NTAPI
FsRtlIsHpfsDbcsLegal(IN ANSI_STRING DbcsName, IN BOOLEAN WildCardsPermissible, IN BOOLEAN PathNamePermissible,
                     IN BOOLEAN LeadingBackslashPermissible)
{
    ULONG Start = 0, i;

    if (!DbcsName.Length)
        return FALSE;
    if (DbcsName.Buffer[0] == '\\')
    {
        if (!LeadingBackslashPermissible)
            return FALSE;
        Start = 1;
    }
    for (i = Start; i < DbcsName.Length; i++)
    {
        UCHAR c = (UCHAR)DbcsName.Buffer[i];
        if (c == '\\')
        {
            if (!PathNamePermissible || i == Start)
                return FALSE;
            Start = i + 1;
            continue;
        }
        if (!(QzLegalCharacters[c] & (QZ_HPFS | (WildCardsPermissible ? QZ_WILD : 0))))
            return FALSE;
    }
    return TRUE;
}
