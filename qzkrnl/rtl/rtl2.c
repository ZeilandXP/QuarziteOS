/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     More RTL: generic tables, code page conversion, number and
 *              GUID strings, unwinding, debug output with va_list
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qzio.h"

/* ---------------------------------------------------------------------- */
/* Generic table ("AVL"): an ordinary binary search tree over the same     */
/* RTL_BALANCED_LINKS nodes. Callers only see the API, so balance is an    */
/* optimization for later.                                                 */
/* ---------------------------------------------------------------------- */

#define QzAvlData(Links) ((PVOID)((PRTL_BALANCED_LINKS)(Links) + 1))

VOID
NTAPI
RtlInitializeGenericTableAvl(OUT PRTL_AVL_TABLE Table,
                             IN PRTL_AVL_COMPARE_ROUTINE CompareRoutine,
                             IN PRTL_AVL_ALLOCATE_ROUTINE AllocateRoutine,
                             IN PRTL_AVL_FREE_ROUTINE FreeRoutine,
                             IN PVOID TableContext OPTIONAL)
{
    RtlZeroMemory(Table, sizeof(*Table));
    Table->BalancedRoot.Parent = &Table->BalancedRoot;
    Table->CompareRoutine = CompareRoutine;
    Table->AllocateRoutine = AllocateRoutine;
    Table->FreeRoutine = FreeRoutine;
    Table->TableContext = TableContext;
}

/* The real root hangs off BalancedRoot.RightChild, as on NT */
static PRTL_BALANCED_LINKS
QzAvlFind(PRTL_AVL_TABLE Table, PVOID Buffer, PRTL_BALANCED_LINKS *Parent, BOOLEAN *Left)
{
    PRTL_BALANCED_LINKS Node = Table->BalancedRoot.RightChild;

    *Parent = &Table->BalancedRoot;
    *Left = FALSE;
    while (Node)
    {
        RTL_GENERIC_COMPARE_RESULTS Result = Table->CompareRoutine(Table, Buffer, QzAvlData(Node));
        if (Result == GenericEqual)
            return Node;
        *Parent = Node;
        *Left = (Result == GenericLessThan);
        Node = *Left ? Node->LeftChild : Node->RightChild;
    }
    return NULL;
}

PVOID
NTAPI
RtlInsertElementGenericTableAvl(IN PRTL_AVL_TABLE Table,
                                IN PVOID Buffer,
                                IN CLONG BufferSize,
                                OUT PBOOLEAN NewElement OPTIONAL)
{
    PRTL_BALANCED_LINKS Parent, Node;
    BOOLEAN Left;

    Node = QzAvlFind(Table, Buffer, &Parent, &Left);
    if (Node)
    {
        if (NewElement)
            *NewElement = FALSE;
        return QzAvlData(Node);
    }
    Node = Table->AllocateRoutine(Table, sizeof(RTL_BALANCED_LINKS) + BufferSize);
    if (!Node)
        return NULL;
    RtlZeroMemory(Node, sizeof(RTL_BALANCED_LINKS));
    RtlCopyMemory(QzAvlData(Node), Buffer, BufferSize);
    Node->Parent = Parent;
    if (Left)
        Parent->LeftChild = Node;
    else
        Parent->RightChild = Node;
    Table->NumberGenericTableElements++;
    Table->WhichOrderedElement = 0;
    Table->OrderedPointer = NULL;
    if (NewElement)
        *NewElement = TRUE;
    return QzAvlData(Node);
}

PVOID
NTAPI
RtlLookupElementGenericTableAvl(IN PRTL_AVL_TABLE Table,
                                IN PVOID Buffer)
{
    PRTL_BALANCED_LINKS Parent;
    BOOLEAN Left;
    PRTL_BALANCED_LINKS Node = QzAvlFind(Table, Buffer, &Parent, &Left);
    return Node ? QzAvlData(Node) : NULL;
}

BOOLEAN
NTAPI
RtlDeleteElementGenericTableAvl(IN PRTL_AVL_TABLE Table,
                                IN PVOID Buffer)
{
    PRTL_BALANCED_LINKS Parent, Node, Replace;
    BOOLEAN Left;

    Node = QzAvlFind(Table, Buffer, &Parent, &Left);
    if (!Node)
        return FALSE;

    if (Node->LeftChild && Node->RightChild)
    {
        /* Swap with the in-order successor, which has no left child */
        PRTL_BALANCED_LINKS Successor = Node->RightChild;
        while (Successor->LeftChild)
            Successor = Successor->LeftChild;

        /* Unlink the successor */
        Replace = Successor->RightChild;
        if (Successor->Parent->LeftChild == Successor)
            Successor->Parent->LeftChild = Replace;
        else
            Successor->Parent->RightChild = Replace;
        if (Replace)
            Replace->Parent = Successor->Parent;

        /* Put it where Node was */
        Successor->LeftChild = Node->LeftChild;
        Successor->RightChild = Node->RightChild;
        Successor->Parent = Node->Parent;
        if (Successor->LeftChild)
            Successor->LeftChild->Parent = Successor;
        if (Successor->RightChild)
            Successor->RightChild->Parent = Successor;
        Replace = Successor;
    }
    else
    {
        Replace = Node->LeftChild ? Node->LeftChild : Node->RightChild;
        if (Replace)
            Replace->Parent = Node->Parent;
    }

    if (Node->Parent->LeftChild == Node)
        Node->Parent->LeftChild = Replace;
    else
        Node->Parent->RightChild = Replace;

    Table->NumberGenericTableElements--;
    Table->DeleteCount++;
    Table->WhichOrderedElement = 0;
    Table->OrderedPointer = NULL;
    Table->FreeRoutine(Table, Node);
    return TRUE;
}

ULONG
NTAPI
RtlNumberGenericTableElementsAvl(IN PRTL_AVL_TABLE Table)
{
    return Table->NumberGenericTableElements;
}

BOOLEAN
NTAPI
RtlIsGenericTableEmptyAvl(IN PRTL_AVL_TABLE Table)
{
    return Table->NumberGenericTableElements == 0;
}

static PRTL_BALANCED_LINKS
QzAvlFirst(PRTL_AVL_TABLE Table)
{
    PRTL_BALANCED_LINKS Node = Table->BalancedRoot.RightChild;
    while (Node && Node->LeftChild)
        Node = Node->LeftChild;
    return Node;
}

static PRTL_BALANCED_LINKS
QzAvlNext(PRTL_AVL_TABLE Table, PRTL_BALANCED_LINKS Node)
{
    if (Node->RightChild)
    {
        Node = Node->RightChild;
        while (Node->LeftChild)
            Node = Node->LeftChild;
        return Node;
    }
    while (Node->Parent != &Table->BalancedRoot && Node->Parent->RightChild == Node)
        Node = Node->Parent;
    return (Node->Parent == &Table->BalancedRoot) ? NULL : Node->Parent;
}

PVOID
NTAPI
RtlEnumerateGenericTableWithoutSplayingAvl(IN PRTL_AVL_TABLE Table,
                                           IN OUT PVOID *RestartKey)
{
    PRTL_BALANCED_LINKS Node = *RestartKey ? QzAvlNext(Table, *RestartKey) : QzAvlFirst(Table);
    *RestartKey = Node;
    return Node ? QzAvlData(Node) : NULL;
}

PVOID
NTAPI
RtlEnumerateGenericTableAvl(IN PRTL_AVL_TABLE Table,
                            IN BOOLEAN Restart)
{
    PVOID Key = Restart ? NULL : Table->RestartKey;
    PVOID Data = RtlEnumerateGenericTableWithoutSplayingAvl(Table, &Key);
    Table->RestartKey = Key;
    return Data;
}

PVOID
NTAPI
RtlGetElementGenericTableAvl(IN PRTL_AVL_TABLE Table,
                             IN ULONG I)
{
    PRTL_BALANCED_LINKS Node = QzAvlFirst(Table);
    while (Node && I--)
        Node = QzAvlNext(Table, Node);
    return Node ? QzAvlData(Node) : NULL;
}

/* ---------------------------------------------------------------------- */
/* Code page conversion: Latin-1 until NLS tables load                     */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
RtlMultiByteToUnicodeN(OUT PWCH UnicodeString, IN ULONG MaxBytesInUnicodeString,
                       OUT PULONG BytesInUnicodeString OPTIONAL, IN PCSTR MultiByteString,
                       IN ULONG BytesInMultiByteString)
{
    ULONG Count = min(MaxBytesInUnicodeString / sizeof(WCHAR), BytesInMultiByteString), i;
    for (i = 0; i < Count; i++)
        UnicodeString[i] = (UCHAR)MultiByteString[i];
    if (BytesInUnicodeString)
        *BytesInUnicodeString = Count * sizeof(WCHAR);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlMultiByteToUnicodeSize(OUT PULONG BytesInUnicodeString, IN PCSTR MultiByteString,
                          IN ULONG BytesInMultiByteString)
{
    UNREFERENCED_PARAMETER(MultiByteString);
    *BytesInUnicodeString = BytesInMultiByteString * sizeof(WCHAR);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlUnicodeToMultiByteN(OUT PCHAR MultiByteString, IN ULONG MaxBytesInMultiByteString,
                       OUT PULONG BytesInMultiByteString OPTIONAL, IN PCWCH UnicodeString,
                       IN ULONG BytesInUnicodeString)
{
    ULONG Count = min(MaxBytesInMultiByteString, BytesInUnicodeString / sizeof(WCHAR)), i;
    for (i = 0; i < Count; i++)
        MultiByteString[i] = (UnicodeString[i] < 0x100) ? (CHAR)UnicodeString[i] : '?';
    if (BytesInMultiByteString)
        *BytesInMultiByteString = Count;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlUnicodeToMultiByteSize(OUT PULONG BytesInMultiByteString, IN PCWCH UnicodeString,
                          IN ULONG BytesInUnicodeString)
{
    UNREFERENCED_PARAMETER(UnicodeString);
    *BytesInMultiByteString = BytesInUnicodeString / sizeof(WCHAR);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlUnicodeStringToInteger(IN PCUNICODE_STRING String, IN ULONG Base OPTIONAL, OUT PULONG Value)
{
    ULONG i = 0, Length = String->Length / sizeof(WCHAR), Result = 0;
    BOOLEAN Negative = FALSE;

    while (i < Length && String->Buffer[i] == L' ')
        i++;
    if (i < Length && (String->Buffer[i] == L'-' || String->Buffer[i] == L'+'))
        Negative = (String->Buffer[i++] == L'-');
    if (!Base)
    {
        Base = 10;
        if (i + 1 < Length && String->Buffer[i] == L'0')
        {
            switch (String->Buffer[i + 1])
            {
                case L'x': case L'X': Base = 16; i += 2; break;
                case L'o': case L'O': Base = 8; i += 2; break;
                case L'b': case L'B': Base = 2; i += 2; break;
            }
        }
    }
    else if (Base != 2 && Base != 8 && Base != 10 && Base != 16)
    {
        return STATUS_INVALID_PARAMETER;
    }
    for (; i < Length; i++)
    {
        WCHAR c = String->Buffer[i];
        ULONG Digit;
        if (c >= L'0' && c <= L'9')
            Digit = c - L'0';
        else if (c >= L'a' && c <= L'f')
            Digit = c - L'a' + 10;
        else if (c >= L'A' && c <= L'F')
            Digit = c - L'A' + 10;
        else
            break;
        if (Digit >= Base)
            break;
        Result = Result * Base + Digit;
    }
    *Value = Negative ? (ULONG)-(LONG)Result : Result;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlStringFromGUID(IN REFGUID Guid, OUT PUNICODE_STRING GuidString)
{
    WCHAR Buffer[39];
    _snwprintf(Buffer, RTL_NUMBER_OF(Buffer),
               L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               Guid->Data1, Guid->Data2, Guid->Data3, Guid->Data4[0], Guid->Data4[1],
               Guid->Data4[2], Guid->Data4[3], Guid->Data4[4], Guid->Data4[5], Guid->Data4[6],
               Guid->Data4[7]);
    Buffer[38] = UNICODE_NULL;
    return RtlCreateUnicodeString(GuidString, Buffer) ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

static BOOLEAN
QzHexValue(PCWSTR Text, ULONG Digits, PULONG Value)
{
    ULONG i, v = 0;
    for (i = 0; i < Digits; i++)
    {
        WCHAR c = Text[i];
        v <<= 4;
        if (c >= L'0' && c <= L'9') v |= c - L'0';
        else if (c >= L'a' && c <= L'f') v |= c - L'a' + 10;
        else if (c >= L'A' && c <= L'F') v |= c - L'A' + 10;
        else return FALSE;
    }
    *Value = v;
    return TRUE;
}

NTSTATUS
NTAPI
RtlGUIDFromString(IN PUNICODE_STRING GuidString, OUT GUID *Guid)
{
    PCWSTR s = GuidString->Buffer;
    ULONG v, i;
    static const UCHAR Offsets[8] = { 20, 22, 25, 27, 29, 31, 33, 35 };

    if (GuidString->Length < 38 * sizeof(WCHAR) || s[0] != L'{' || s[9] != L'-' || s[14] != L'-' ||
        s[19] != L'-' || s[24] != L'-' || s[37] != L'}')
        return STATUS_INVALID_PARAMETER;
    if (!QzHexValue(s + 1, 8, &v)) return STATUS_INVALID_PARAMETER;
    Guid->Data1 = v;
    if (!QzHexValue(s + 10, 4, &v)) return STATUS_INVALID_PARAMETER;
    Guid->Data2 = (USHORT)v;
    if (!QzHexValue(s + 15, 4, &v)) return STATUS_INVALID_PARAMETER;
    Guid->Data3 = (USHORT)v;
    for (i = 0; i < 8; i++)
    {
        if (!QzHexValue(s + Offsets[i], 2, &v))
            return STATUS_INVALID_PARAMETER;
        Guid->Data4[i] = (UCHAR)v;
    }
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Unwinding and stack traces                                              */
/* ---------------------------------------------------------------------- */

/*
 * Exceptions are never dispatched yet, so nobody unwinds through a
 * handler. RtlUnwind still does its basic job: drop the FS:[0]
 * registrations above TargetFrame.
 */
VOID
NTAPI
RtlUnwind(IN PVOID TargetFrame OPTIONAL, IN PVOID TargetIp OPTIONAL,
          IN PEXCEPTION_RECORD ExceptionRecord OPTIONAL, IN PVOID ReturnValue)
{
    PEXCEPTION_REGISTRATION_RECORD Frame = (PEXCEPTION_REGISTRATION_RECORD)__readfsdword(0);

    UNREFERENCED_PARAMETER(TargetIp);
    UNREFERENCED_PARAMETER(ExceptionRecord);
    UNREFERENCED_PARAMETER(ReturnValue);

    while (Frame != EXCEPTION_CHAIN_END && Frame != TargetFrame)
        Frame = Frame->Next;
    __writefsdword(0, (ULONG)(ULONG_PTR)Frame);
}

USHORT
NTAPI
RtlCaptureStackBackTrace(IN ULONG FramesToSkip, IN ULONG FramesToCapture,
                         OUT PVOID *BackTrace, OUT PULONG BackTraceHash OPTIONAL)
{
    UNREFERENCED_PARAMETER(FramesToSkip);
    UNREFERENCED_PARAMETER(FramesToCapture);
    UNREFERENCED_PARAMETER(BackTrace);
    if (BackTraceHash)
        *BackTraceHash = 0;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Debug output with a va_list                                             */
/* ---------------------------------------------------------------------- */

extern BOOLEAN QzDebugVerbose;

ULONG
NTAPI
vDbgPrintEx(IN ULONG ComponentId, IN ULONG Level, IN PCCH Format, IN va_list Args)
{
    CHAR Buffer[512];
    UNREFERENCED_PARAMETER(ComponentId);
    /* The same filter as DbgPrintEx: error-level messages only */
    if (!QzDebugVerbose && !(Level == DPFLTR_ERROR_LEVEL || (Level > 31 && (Level & 1))))
        return STATUS_SUCCESS;
    QzVsnprintf(Buffer, sizeof(Buffer), Format, Args);
    DbgPrint("%s", Buffer);
    return STATUS_SUCCESS;
}

ULONG
NTAPI
vDbgPrintExWithPrefix(IN PCCH Prefix, IN ULONG ComponentId, IN ULONG Level, IN PCCH Format,
                      IN va_list Args)
{
    CHAR Buffer[512];
    UNREFERENCED_PARAMETER(ComponentId);
    if (!QzDebugVerbose && !(Level == DPFLTR_ERROR_LEVEL || (Level > 31 && (Level & 1))))
        return STATUS_SUCCESS;
    QzVsnprintf(Buffer, sizeof(Buffer), Format, Args);
    DbgPrint("%s%s", Prefix, Buffer);
    return STATUS_SUCCESS;
}
