/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Splay trees, generic tables and Unicode prefix tables
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * RTL_SPLAY_LINKS is the public node: a root is its own parent. File
 * systems link their nodes with the RtlInsertAsLeft/RightChild macros and
 * call RtlSplay and RtlDelete directly (cdfs's prefix tables); generic
 * tables are built on the same trees. A generic table entry is a
 * TABLE_ENTRY_HEADER (splay links, the insertion-order list entry) with
 * the caller's data after it, allocated through the table's routines.
 */

#include "qz.h"

/* ---------------------------------------------------------------------- */
/* Splay links                                                             */
/* ---------------------------------------------------------------------- */

/* Make Child take Node's place under Node's parent (or as the root) */
static VOID
QzReplaceInParent(PRTL_SPLAY_LINKS Node, PRTL_SPLAY_LINKS Child)
{
    PRTL_SPLAY_LINKS Parent = Node->Parent;

    if (Parent == Node)
    {
        if (Child)
            Child->Parent = Child;
        return;
    }
    if (Parent->LeftChild == Node)
        Parent->LeftChild = Child;
    else
        Parent->RightChild = Child;
    if (Child)
        Child->Parent = Parent;
}

/* One rotation: Node goes up over its parent */
static VOID
QzRotateUp(PRTL_SPLAY_LINKS Node)
{
    PRTL_SPLAY_LINKS Parent = Node->Parent;
    PRTL_SPLAY_LINKS Grand = Parent->Parent;
    BOOLEAN ParentIsRoot = (Grand == Parent);

    if (Parent->LeftChild == Node)
    {
        Parent->LeftChild = Node->RightChild;
        if (Node->RightChild)
            Node->RightChild->Parent = Parent;
        Node->RightChild = Parent;
    }
    else
    {
        Parent->RightChild = Node->LeftChild;
        if (Node->LeftChild)
            Node->LeftChild->Parent = Parent;
        Node->LeftChild = Parent;
    }
    if (ParentIsRoot)
    {
        Node->Parent = Node;
    }
    else
    {
        if (Grand->LeftChild == Parent)
            Grand->LeftChild = Node;
        else
            Grand->RightChild = Node;
        Node->Parent = Grand;
    }
    Parent->Parent = Node;
}

PRTL_SPLAY_LINKS
NTAPI
RtlSplay(IN PRTL_SPLAY_LINKS Links)
{
    while (!RtlIsRoot(Links))
    {
        PRTL_SPLAY_LINKS Parent = Links->Parent;
        if (RtlIsRoot(Parent))
        {
            QzRotateUp(Links);                          /* zig */
        }
        else if ((Parent->LeftChild == Links) == (Parent->Parent->LeftChild == Parent))
        {
            QzRotateUp(Parent);                         /* zig-zig */
            QzRotateUp(Links);
        }
        else
        {
            QzRotateUp(Links);                          /* zig-zag */
            QzRotateUp(Links);
        }
    }
    return Links;
}

PRTL_SPLAY_LINKS
NTAPI
RtlSubtreeSuccessor(IN PRTL_SPLAY_LINKS Links)
{
    PRTL_SPLAY_LINKS Node = Links->RightChild;
    if (!Node)
        return NULL;
    while (Node->LeftChild)
        Node = Node->LeftChild;
    return Node;
}

PRTL_SPLAY_LINKS
NTAPI
RtlSubtreePredecessor(IN PRTL_SPLAY_LINKS Links)
{
    PRTL_SPLAY_LINKS Node = Links->LeftChild;
    if (!Node)
        return NULL;
    while (Node->RightChild)
        Node = Node->RightChild;
    return Node;
}

PRTL_SPLAY_LINKS
NTAPI
RtlRealSuccessor(IN PRTL_SPLAY_LINKS Links)
{
    PRTL_SPLAY_LINKS Node = RtlSubtreeSuccessor(Links);
    if (Node)
        return Node;
    /* Up until we come from a left child */
    Node = Links;
    while (!RtlIsRoot(Node) && Node->Parent->RightChild == Node)
        Node = Node->Parent;
    return RtlIsRoot(Node) ? NULL : Node->Parent;
}

PRTL_SPLAY_LINKS
NTAPI
RtlRealPredecessor(IN PRTL_SPLAY_LINKS Links)
{
    PRTL_SPLAY_LINKS Node = RtlSubtreePredecessor(Links);
    if (Node)
        return Node;
    Node = Links;
    while (!RtlIsRoot(Node) && Node->Parent->LeftChild == Node)
        Node = Node->Parent;
    return RtlIsRoot(Node) ? NULL : Node->Parent;
}

/* Unlink Links; *Root becomes the tree that remains (or NULL), without splaying */
static PRTL_SPLAY_LINKS
QzDeleteNoSplay(PRTL_SPLAY_LINKS Links, PRTL_SPLAY_LINKS *Root)
{
    PRTL_SPLAY_LINKS Child;

    if (Links->LeftChild && Links->RightChild)
    {
        /* Put the in-order predecessor in its place */
        PRTL_SPLAY_LINKS Swap = RtlSubtreePredecessor(Links);
        PRTL_SPLAY_LINKS SwapChild = Swap->LeftChild;

        if (Swap->Parent != Links)
        {
            QzReplaceInParent(Swap, SwapChild);
            Swap->LeftChild = Links->LeftChild;
            Swap->LeftChild->Parent = Swap;
        }
        Swap->RightChild = Links->RightChild;
        Swap->RightChild->Parent = Swap;
        if (RtlIsRoot(Links))
        {
            Swap->Parent = Swap;
            *Root = Swap;
        }
        else
        {
            QzReplaceInParent(Links, Swap);
        }
        return *Root;
    }

    Child = Links->LeftChild ? Links->LeftChild : Links->RightChild;
    if (RtlIsRoot(Links))
    {
        if (Child)
            Child->Parent = Child;
        *Root = Child;
    }
    else
    {
        QzReplaceInParent(Links, Child);
    }
    return *Root;
}

VOID
NTAPI
RtlDeleteNoSplay(IN PRTL_SPLAY_LINKS Links, IN OUT PRTL_SPLAY_LINKS *Root)
{
    QzDeleteNoSplay(Links, Root);
}

/* Unlink Links and return the new root, splayed at Links's parent */
PRTL_SPLAY_LINKS
NTAPI
RtlDelete(IN PRTL_SPLAY_LINKS Links)
{
    PRTL_SPLAY_LINKS Root = Links, Parent;

    while (!RtlIsRoot(Root))
        Root = Root->Parent;
    Parent = RtlIsRoot(Links) ? NULL : Links->Parent;
    if (Links->LeftChild && Links->RightChild)
        Parent = NULL;                  /* the predecessor took its place */
    QzDeleteNoSplay(Links, &Root);
    if (Parent)
        return RtlSplay(Parent);
    return Root;
}

/* ---------------------------------------------------------------------- */
/* Generic tables                                                          */
/* ---------------------------------------------------------------------- */

typedef struct _QZ_TABLE_ENTRY
{
    RTL_SPLAY_LINKS SplayLinks;
    LIST_ENTRY ListEntry;
    LONGLONG UserData;                  /* the caller's buffer starts here */
} QZ_TABLE_ENTRY, *PQZ_TABLE_ENTRY;

#define QZ_TABLE_DATA_OFFSET FIELD_OFFSET(QZ_TABLE_ENTRY, UserData)
#define QzTableData(Node) ((PVOID)((PUCHAR)(Node) + QZ_TABLE_DATA_OFFSET))

VOID
NTAPI
RtlInitializeGenericTable(OUT PRTL_GENERIC_TABLE Table,
                          IN PRTL_GENERIC_COMPARE_ROUTINE CompareRoutine,
                          IN PRTL_GENERIC_ALLOCATE_ROUTINE AllocateRoutine,
                          IN PRTL_GENERIC_FREE_ROUTINE FreeRoutine,
                          IN PVOID TableContext)
{
    Table->TableRoot = NULL;
    InitializeListHead(&Table->InsertOrderList);
    Table->OrderedPointer = &Table->InsertOrderList;
    Table->WhichOrderedElement = 0;
    Table->NumberGenericTableElements = 0;
    Table->CompareRoutine = CompareRoutine;
    Table->AllocateRoutine = AllocateRoutine;
    Table->FreeRoutine = FreeRoutine;
    Table->TableContext = TableContext;
}

/* Where Buffer is or would go: the node, and on which side of it */
static TABLE_SEARCH_RESULT
QzTableFind(PRTL_GENERIC_TABLE Table, PVOID Buffer, PRTL_SPLAY_LINKS *Node)
{
    PRTL_SPLAY_LINKS Current = Table->TableRoot;

    *Node = NULL;
    if (!Current)
        return TableEmptyTree;
    for (;;)
    {
        RTL_GENERIC_COMPARE_RESULTS Result = Table->CompareRoutine(Table, Buffer, QzTableData(Current));
        *Node = Current;
        if (Result == GenericLessThan)
        {
            if (!Current->LeftChild)
                return TableInsertAsLeft;
            Current = Current->LeftChild;
        }
        else if (Result == GenericGreaterThan)
        {
            if (!Current->RightChild)
                return TableInsertAsRight;
            Current = Current->RightChild;
        }
        else
        {
            return TableFoundNode;
        }
    }
}

PVOID
NTAPI
RtlInsertElementGenericTableFull(IN PRTL_GENERIC_TABLE Table,
                                 IN PVOID Buffer,
                                 IN CLONG BufferSize,
                                 OUT PBOOLEAN NewElement OPTIONAL,
                                 IN PVOID NodeOrParent,
                                 IN TABLE_SEARCH_RESULT SearchResult)
{
    PQZ_TABLE_ENTRY Entry;

    if (SearchResult == TableFoundNode)
    {
        if (NewElement)
            *NewElement = FALSE;
        Table->TableRoot = RtlSplay(NodeOrParent);
        return QzTableData(NodeOrParent);
    }
    Entry = Table->AllocateRoutine(Table, BufferSize + QZ_TABLE_DATA_OFFSET);
    if (!Entry)
    {
        if (NewElement)
            *NewElement = FALSE;
        return NULL;
    }
    RtlInitializeSplayLinks(&Entry->SplayLinks);
    InsertTailList(&Table->InsertOrderList, &Entry->ListEntry);
    Table->NumberGenericTableElements++;
    Table->OrderedPointer = &Table->InsertOrderList;
    Table->WhichOrderedElement = 0;
    if (SearchResult == TableEmptyTree)
    {
        Table->TableRoot = &Entry->SplayLinks;
    }
    else if (SearchResult == TableInsertAsLeft)
    {
        RtlInsertAsLeftChild((PRTL_SPLAY_LINKS)NodeOrParent, &Entry->SplayLinks);
    }
    else
    {
        RtlInsertAsRightChild((PRTL_SPLAY_LINKS)NodeOrParent, &Entry->SplayLinks);
    }
    RtlCopyMemory(QzTableData(Entry), Buffer, BufferSize);
    Table->TableRoot = RtlSplay(&Entry->SplayLinks);
    if (NewElement)
        *NewElement = TRUE;
    return QzTableData(Entry);
}

PVOID
NTAPI
RtlInsertElementGenericTable(IN PRTL_GENERIC_TABLE Table,
                             IN PVOID Buffer,
                             IN CLONG BufferSize,
                             OUT PBOOLEAN NewElement OPTIONAL)
{
    PRTL_SPLAY_LINKS Node;
    TABLE_SEARCH_RESULT Result = QzTableFind(Table, Buffer, &Node);
    return RtlInsertElementGenericTableFull(Table, Buffer, BufferSize, NewElement, Node, Result);
}

PVOID
NTAPI
RtlLookupElementGenericTableFull(IN PRTL_GENERIC_TABLE Table,
                                 IN PVOID Buffer,
                                 OUT PVOID *NodeOrParent,
                                 OUT TABLE_SEARCH_RESULT *SearchResult)
{
    PRTL_SPLAY_LINKS Node;
    *SearchResult = QzTableFind(Table, Buffer, &Node);
    *NodeOrParent = Node;
    if (*SearchResult != TableFoundNode)
        return NULL;
    Table->TableRoot = RtlSplay(Node);
    return QzTableData(Node);
}

PVOID
NTAPI
RtlLookupElementGenericTable(IN PRTL_GENERIC_TABLE Table, IN PVOID Buffer)
{
    PVOID Node;
    TABLE_SEARCH_RESULT Result;
    return RtlLookupElementGenericTableFull(Table, Buffer, &Node, &Result);
}

BOOLEAN
NTAPI
RtlDeleteElementGenericTable(IN PRTL_GENERIC_TABLE Table, IN PVOID Buffer)
{
    PRTL_SPLAY_LINKS Node;
    PQZ_TABLE_ENTRY Entry;

    if (QzTableFind(Table, Buffer, &Node) != TableFoundNode)
        return FALSE;
    Entry = CONTAINING_RECORD(Node, QZ_TABLE_ENTRY, SplayLinks);
    Table->TableRoot = RtlDelete(Node);
    RemoveEntryList(&Entry->ListEntry);
    Table->NumberGenericTableElements--;
    Table->OrderedPointer = &Table->InsertOrderList;
    Table->WhichOrderedElement = 0;
    Table->FreeRoutine(Table, Entry);
    return TRUE;
}

/* In order: *RestartKey NULL starts at the smallest */
PVOID
NTAPI
RtlEnumerateGenericTableWithoutSplaying(IN PRTL_GENERIC_TABLE Table, IN OUT PVOID *RestartKey)
{
    PRTL_SPLAY_LINKS Node;

    if (!Table->TableRoot)
        return NULL;
    if (!*RestartKey)
    {
        Node = Table->TableRoot;
        while (Node->LeftChild)
            Node = Node->LeftChild;
    }
    else
    {
        Node = RtlRealSuccessor(*RestartKey);
    }
    *RestartKey = Node;
    return Node ? QzTableData(Node) : NULL;
}

PVOID
NTAPI
RtlEnumerateGenericTable(IN PRTL_GENERIC_TABLE Table, IN BOOLEAN Restart)
{
    PRTL_SPLAY_LINKS Node;

    if (!Table->TableRoot)
        return NULL;
    if (Restart)
    {
        Node = Table->TableRoot;
        while (Node->LeftChild)
            Node = Node->LeftChild;
    }
    else
    {
        Node = RtlRealSuccessor(Table->TableRoot);
        if (!Node)
            return NULL;
    }
    Table->TableRoot = RtlSplay(Node);
    return QzTableData(Node);
}

ULONG
NTAPI
RtlNumberGenericTableElements(IN PRTL_GENERIC_TABLE Table)
{
    return Table->NumberGenericTableElements;
}

BOOLEAN
NTAPI
RtlIsGenericTableEmpty(IN PRTL_GENERIC_TABLE Table)
{
    return Table->TableRoot == NULL;
}

/* The I'th element in insertion order */
PVOID
NTAPI
RtlGetElementGenericTable(IN PRTL_GENERIC_TABLE Table, IN ULONG I)
{
    PLIST_ENTRY Entry;
    ULONG Index = 0;

    if (I >= Table->NumberGenericTableElements)
        return NULL;
    for (Entry = Table->InsertOrderList.Flink; Entry != &Table->InsertOrderList; Entry = Entry->Flink, Index++)
    {
        if (Index == I)
            return QzTableData(CONTAINING_RECORD(Entry, QZ_TABLE_ENTRY, ListEntry));
    }
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Unicode prefix tables (mup, npfs)                                       */
/* ---------------------------------------------------------------------- */

/*
 * The entries are kept in a plain list through NextPrefixTree (the table
 * is a list head of the same shape). A lookup is the longest entry that
 * is a whole-component prefix of the name: "\a\b" matches "\a\b" and
 * "\a\b\c", not "\a\bc".
 */
#define QZ_PREFIX_TABLE 0x0800
#define QZ_PREFIX_ENTRY 0x0801

VOID
NTAPI
RtlInitializeUnicodePrefix(OUT PUNICODE_PREFIX_TABLE PrefixTable)
{
    PrefixTable->NodeTypeCode = QZ_PREFIX_TABLE;
    PrefixTable->NameLength = 0;
    PrefixTable->NextPrefixTree = (PUNICODE_PREFIX_TABLE_ENTRY)PrefixTable;
    PrefixTable->LastNextEntry = NULL;
}

static BOOLEAN
QzIsComponentPrefix(PCUNICODE_STRING Prefix, PCUNICODE_STRING Name, BOOLEAN CaseInsensitive)
{
    UNICODE_STRING Head;
    USHORT Length = Prefix->Length;

    if (Length > Name->Length)
        return FALSE;
    Head.Buffer = Name->Buffer;
    Head.Length = Head.MaximumLength = Length;
    if (!RtlEqualUnicodeString(Prefix, &Head, CaseInsensitive))
        return FALSE;
    return Length == Name->Length || Name->Buffer[Length / sizeof(WCHAR)] == L'\\' ||
           (Length && Prefix->Buffer[Length / sizeof(WCHAR) - 1] == L'\\');
}

BOOLEAN
NTAPI
RtlInsertUnicodePrefix(IN PUNICODE_PREFIX_TABLE PrefixTable,
                       IN PUNICODE_STRING Prefix,
                       IN PUNICODE_PREFIX_TABLE_ENTRY PrefixTableEntry)
{
    PUNICODE_PREFIX_TABLE_ENTRY Entry;

    for (Entry = PrefixTable->NextPrefixTree; Entry != (PVOID)PrefixTable; Entry = Entry->NextPrefixTree)
    {
        if (RtlEqualUnicodeString(Entry->Prefix, Prefix, TRUE))
            return FALSE;
    }
    PrefixTableEntry->NodeTypeCode = QZ_PREFIX_ENTRY;
    PrefixTableEntry->NameLength = Prefix->Length;
    PrefixTableEntry->Prefix = Prefix;
    PrefixTableEntry->CaseMatch = PrefixTableEntry;
    RtlInitializeSplayLinks(&PrefixTableEntry->Links);
    PrefixTableEntry->NextPrefixTree = PrefixTable->NextPrefixTree;
    PrefixTable->NextPrefixTree = PrefixTableEntry;
    return TRUE;
}

VOID
NTAPI
RtlRemoveUnicodePrefix(IN PUNICODE_PREFIX_TABLE PrefixTable, IN PUNICODE_PREFIX_TABLE_ENTRY PrefixTableEntry)
{
    PUNICODE_PREFIX_TABLE_ENTRY *Link = &PrefixTable->NextPrefixTree;

    while (*Link != (PVOID)PrefixTable)
    {
        if (*Link == PrefixTableEntry)
        {
            *Link = PrefixTableEntry->NextPrefixTree;
            break;
        }
        Link = &(*Link)->NextPrefixTree;
    }
    if (PrefixTable->LastNextEntry == PrefixTableEntry)
        PrefixTable->LastNextEntry = NULL;
}

PUNICODE_PREFIX_TABLE_ENTRY
NTAPI
RtlFindUnicodePrefix(IN PUNICODE_PREFIX_TABLE PrefixTable, IN PUNICODE_STRING FullName,
                     IN ULONG CaseInsensitiveIndex)
{
    PUNICODE_PREFIX_TABLE_ENTRY Entry, Best = NULL;

    for (Entry = PrefixTable->NextPrefixTree; Entry != (PVOID)PrefixTable; Entry = Entry->NextPrefixTree)
    {
        /* The first CaseInsensitiveIndex bytes must match exactly */
        UNICODE_STRING Exact = *Entry->Prefix;
        UNICODE_STRING Name = *FullName;
        if (CaseInsensitiveIndex > Exact.Length)
            CaseInsensitiveIndex = Exact.Length;
        if (CaseInsensitiveIndex > Name.Length)
            continue;
        if (CaseInsensitiveIndex &&
            RtlCompareMemory(Exact.Buffer, Name.Buffer, CaseInsensitiveIndex) != CaseInsensitiveIndex)
            continue;
        if (QzIsComponentPrefix(Entry->Prefix, FullName, TRUE) &&
            (!Best || Entry->Prefix->Length > Best->Prefix->Length))
            Best = Entry;
    }
    return Best;
}

PUNICODE_PREFIX_TABLE_ENTRY
NTAPI
RtlNextUnicodePrefix(IN PUNICODE_PREFIX_TABLE PrefixTable, IN BOOLEAN Restart)
{
    PUNICODE_PREFIX_TABLE_ENTRY Next;

    if (Restart || !PrefixTable->LastNextEntry)
        Next = PrefixTable->NextPrefixTree;
    else
        Next = PrefixTable->LastNextEntry->NextPrefixTree;
    if (Next == (PVOID)PrefixTable)
    {
        PrefixTable->LastNextEntry = NULL;
        return NULL;
    }
    PrefixTable->LastNextEntry = Next;
    return Next;
}
