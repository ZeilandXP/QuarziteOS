/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Registry: the loader's SYSTEM hive, writable in memory, and
 *              the volatile in-memory keys around it
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Stage B5, the configuration manager proper:
 *   \REGISTRY\MACHINE\SYSTEM     the SYSTEM hive FreeLoader loaded, copied
 *                                into bins of its own (cmlib HINIT_MEMORY)
 *                                so keys and values can be created, changed
 *                                and deleted. Stable cells are marked dirty;
 *                                writing them back to SYSTEM on disk needs a
 *                                file system (stage B7). Volatile keys
 *                                (REG_OPTION_VOLATILE) live in the hive's
 *                                volatile storage.
 *   SYSTEM\CurrentControlSet     a volatile symbolic link key, created at
 *                                boot, pointing at the ControlSetNNN that
 *                                Select\Current names.
 *   \REGISTRY\MACHINE\HARDWARE   volatile and writable, as on NT; its
 *                                DESCRIPTION subtree is built from the
 *                                loader's ARC configuration tree (this is
 *                                where the HAL finds the PCI BIOS).
 *   everything else              volatile in-memory keys.
 * ZwNotifyChangeKey watches a key (or a subtree) for name, value and
 * attribute changes; an event or a work item signals the caller.
 */

#include "qzio.h"
#include <cmlib.h>

#define QZ_CM_TAG  'yeKQ'
#define QZ_REG_FULL_RESOURCE_DESCRIPTOR 9

/* ---------------------------------------------------------------------- */
/* In-memory keys                                                          */
/* ---------------------------------------------------------------------- */

typedef struct _QZ_MEM_VALUE
{
    struct _QZ_MEM_VALUE *Next;
    ULONG Type;
    ULONG DataLength;
    PUCHAR Data;
    USHORT NameLength;                 /* bytes */
    WCHAR Name[1];
} QZ_MEM_VALUE, *PQZ_MEM_VALUE;

typedef struct _QZ_MEM_KEY
{
    struct _QZ_MEM_KEY *Parent, *Child, *Sibling;
    PQZ_MEM_VALUE Values;
    ULONG SubKeys, ValueCount;
    struct _CMHIVE *MountedHive;       /* a hive's root goes here (\REGISTRY\MACHINE\SYSTEM, ...) */
    LARGE_INTEGER LastWriteTime;
    USHORT NameLength;                 /* bytes */
    WCHAR Name[1];
} QZ_MEM_KEY, *PQZ_MEM_KEY;

/* A key object: a cell of a hive or an in-memory key */
typedef struct _QZ_KEY
{
    PCMHIVE CmHive;                    /* the cell's hive */
    BOOLEAN IsCell;
    BOOLEAN Deleted;                   /* the key went away under this handle */
    HCELL_INDEX Cell;
    PQZ_MEM_KEY Mem;
    LIST_ENTRY Link;                   /* QzOpenKeys */
} QZ_KEY, *PQZ_KEY;

static LIST_ENTRY QzOpenKeys = { &QzOpenKeys, &QzOpenKeys };

typedef struct _QZ_KEY_PARSE
{
    CSHORT Type;                       /* 'K' */
    BOOLEAN Create;
    BOOLEAN Volatile;                  /* REG_OPTION_VOLATILE */
    ULONG Disposition;
} QZ_KEY_PARSE, *PQZ_KEY_PARSE;

POBJECT_TYPE CmKeyObjectType;
static PCMHIVE QzSystemCmHive;
/*
 * The hive the cell functions work on: chosen, under the registry lock,
 * from the key an operation is about (several hives are mounted).
 */
static PHHIVE QzCurHive;
#define QzHive QzCurHive
#define QzSelectHive(Key) do { if ((Key)->IsCell) QzCurHive = &(Key)->CmHive->Hive; } while (0)
#define QzInSystemHive() (QzSystemCmHive && QzCurHive == &QzSystemCmHive->Hive)
static BOOLEAN QzHiveReady;
static HCELL_INDEX QzControlSetCell = HCELL_NIL;
static PQZ_MEM_KEY QzRegistryRoot, QzHardwareKey;
static FAST_MUTEX QzRegistryLock;

/* What cmlib needs from its host */
PCMHIVE CmiVolatileHive;
ULONG CmpBootType;
BOOLEAN CmpSelfHeal = TRUE;

PVOID
NTAPI
CmpAllocate(IN SIZE_T Size, IN BOOLEAN Paged, IN ULONG Tag)
{
    UNREFERENCED_PARAMETER(Paged);
    return ExAllocatePoolWithTag(NonPagedPool, Size, Tag);
}

VOID
NTAPI
CmpFree(IN PVOID Ptr, IN ULONG Quota)
{
    UNREFERENCED_PARAMETER(Quota);
    ExFreePool(Ptr);
}

VOID
NTAPI
CmpLazyFlush(VOID)
{
}

BOOLEAN
NTAPI
IoSetThreadHardErrorMode(IN BOOLEAN HardErrorEnabled)
{
    UNREFERENCED_PARAMETER(HardErrorEnabled);
    return TRUE;
}

static BOOLEAN
QzCmNamesEqual(PCWSTR A, ULONG ABytes, PCWSTR B, ULONG BBytes)
{
    UNICODE_STRING Left, Right;
    Left.Buffer = (PWSTR)A;
    Left.Length = Left.MaximumLength = (USHORT)ABytes;
    Right.Buffer = (PWSTR)B;
    Right.Length = Right.MaximumLength = (USHORT)BBytes;
    return RtlEqualUnicodeString(&Left, &Right, TRUE);
}

static PQZ_MEM_KEY
QzMemFindKey(PQZ_MEM_KEY Parent, PCUNICODE_STRING Name)
{
    PQZ_MEM_KEY Child;
    for (Child = Parent->Child; Child; Child = Child->Sibling)
        if (QzCmNamesEqual(Child->Name, Child->NameLength, Name->Buffer, Name->Length))
            return Child;
    return NULL;
}

/* Parent NULL: a detached key nobody can find again */
static PQZ_MEM_KEY
QzMemCreateKey(PQZ_MEM_KEY Parent, PCWSTR Name, ULONG NameBytes)
{
    UNICODE_STRING String;
    PQZ_MEM_KEY Key, *Link;

    String.Buffer = (PWSTR)Name;
    String.Length = String.MaximumLength = (USHORT)NameBytes;
    if (Parent && (Key = QzMemFindKey(Parent, &String)))
        return Key;

    Key = ExAllocatePoolWithTag(NonPagedPool, sizeof(QZ_MEM_KEY) + NameBytes, QZ_CM_TAG);
    if (!Key)
        return NULL;
    RtlZeroMemory(Key, sizeof(QZ_MEM_KEY));
    RtlCopyMemory(Key->Name, Name, NameBytes);
    Key->NameLength = (USHORT)NameBytes;
    KeQuerySystemTime(&Key->LastWriteTime);
    Key->Parent = Parent;
    if (Parent)
    {
        for (Link = &Parent->Child; *Link; Link = &(*Link)->Sibling)
            ;
        *Link = Key;
        Parent->SubKeys++;
    }
    return Key;
}

static PQZ_MEM_KEY
QzMemCreatePath(PQZ_MEM_KEY Parent, PCWSTR Name)
{
    return QzMemCreateKey(Parent, Name, (ULONG)wcslen(Name) * sizeof(WCHAR));
}

static PQZ_MEM_VALUE
QzMemFindValue(PQZ_MEM_KEY Key, PCUNICODE_STRING Name)
{
    PQZ_MEM_VALUE Value;
    for (Value = Key->Values; Value; Value = Value->Next)
        if (QzCmNamesEqual(Value->Name, Value->NameLength, Name->Buffer, Name->Length))
            return Value;
    return NULL;
}

static NTSTATUS
QzMemSetValue(PQZ_MEM_KEY Key, PCUNICODE_STRING Name, ULONG Type, const VOID *Data, ULONG Length)
{
    PQZ_MEM_VALUE Value = QzMemFindValue(Key, Name), *Link;
    PUCHAR Copy = ExAllocatePoolWithTag(NonPagedPool, max(Length, 1), QZ_CM_TAG);

    if (!Copy)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (Length)
        RtlCopyMemory(Copy, Data, Length);
    if (!Value)
    {
        Value = ExAllocatePoolWithTag(NonPagedPool, sizeof(QZ_MEM_VALUE) + Name->Length, QZ_CM_TAG);
        if (!Value)
        {
            ExFreePoolWithTag(Copy, QZ_CM_TAG);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(Value, sizeof(QZ_MEM_VALUE));
        RtlCopyMemory(Value->Name, Name->Buffer, Name->Length);
        Value->NameLength = Name->Length;
        for (Link = &Key->Values; *Link; Link = &(*Link)->Next)
            ;
        *Link = Value;
        Key->ValueCount++;
    }
    else if (Value->Data)
    {
        ExFreePoolWithTag(Value->Data, QZ_CM_TAG);
    }
    Value->Type = Type;
    Value->Data = Copy;
    Value->DataLength = Length;
    KeQuerySystemTime(&Key->LastWriteTime);
    return STATUS_SUCCESS;
}

static NTSTATUS
QzMemSetValueW(PQZ_MEM_KEY Key, PCWSTR Name, ULONG Type, const VOID *Data, ULONG Length)
{
    UNICODE_STRING String;
    RtlInitUnicodeString(&String, Name);
    return QzMemSetValue(Key, &String, Type, Data, Length);
}

/* ---------------------------------------------------------------------- */
/* Hive cells                                                              */
/* ---------------------------------------------------------------------- */

static PCM_KEY_NODE
QzCmNode(HCELL_INDEX Cell)
{
    return (PCM_KEY_NODE)HvGetCell(QzHive, Cell);
}

static ULONG
QzCmNodeName(PCM_KEY_NODE Node, PWCHAR Buffer, ULONG BufferBytes)
{
    ULONG Bytes = (Node->Flags & KEY_COMP_NAME) ? CmpCompressedNameSize(Node->Name, Node->NameLength)
                                                : Node->NameLength;
    if (Buffer)
    {
        if (Node->Flags & KEY_COMP_NAME)
            CmpCopyCompressedName(Buffer, min(Bytes, BufferBytes), Node->Name, Node->NameLength);
        else
            RtlCopyMemory(Buffer, Node->Name, min(Bytes, BufferBytes));
    }
    return Bytes;
}

static ULONG
QzCmValueName(PCM_KEY_VALUE Value, PWCHAR Buffer, ULONG BufferBytes)
{
    ULONG Bytes = (Value->Flags & VALUE_COMP_NAME) ? CmpCompressedNameSize(Value->Name, Value->NameLength)
                                                   : Value->NameLength;
    if (Buffer)
    {
        if (Value->Flags & VALUE_COMP_NAME)
            CmpCopyCompressedName(Buffer, min(Bytes, BufferBytes), Value->Name, Value->NameLength);
        else
            RtlCopyMemory(Buffer, Value->Name, min(Bytes, BufferBytes));
    }
    return Bytes;
}

static PCM_KEY_VALUE
QzCmFindValue(HCELL_INDEX KeyCell, PCUNICODE_STRING Name)
{
    PCM_KEY_NODE Node = QzCmNode(KeyCell);
    HCELL_INDEX Cell = CmpFindValueByName(QzHive, Node, (PUNICODE_STRING)Name);
    return (Cell == HCELL_NIL) ? NULL : (PCM_KEY_VALUE)HvGetCell(QzHive, Cell);
}

static PCM_KEY_VALUE
QzCmValueByIndex(HCELL_INDEX KeyCell, ULONG Index)
{
    PCM_KEY_NODE Node = QzCmNode(KeyCell);
    PHCELL_INDEX List;

    if (Index >= Node->ValueList.Count)
        return NULL;
    List = (PHCELL_INDEX)HvGetCell(QzHive, Node->ValueList.List);
    return (PCM_KEY_VALUE)HvGetCell(QzHive, List[Index]);
}

static PVOID
QzCmValueData(PCM_KEY_VALUE Value, PULONG Length)
{
    return CmpValueToData(QzHive, Value, Length);
}

static HCELL_INDEX
QzCmSubKey(HCELL_INDEX Parent, PCUNICODE_STRING Name)
{
    return CmpFindSubKeyByName(QzHive, QzCmNode(Parent), (PUNICODE_STRING)Name);
}

/* ControlSetNNN named by Select\Current */
static HCELL_INDEX
QzCmFindControlSet(VOID)
{
    UNICODE_STRING Select = RTL_CONSTANT_STRING(L"Select"), Current = RTL_CONSTANT_STRING(L"Current");
    HCELL_INDEX SelectCell = QzCmSubKey(QzHive->BaseBlock->RootCell, &Select);
    ULONG Number = 1, Length;
    WCHAR Buffer[16];
    UNICODE_STRING Name;

    if (SelectCell != HCELL_NIL)
    {
        PCM_KEY_VALUE Value = QzCmFindValue(SelectCell, &Current);
        if (Value && Value->Type == REG_DWORD)
            Number = *(PULONG)QzCmValueData(Value, &Length);
    }
    Name.Buffer = Buffer;
    Name.MaximumLength = sizeof(Buffer);
    Name.Length = (USHORT)(_snwprintf(Buffer, RTL_NUMBER_OF(Buffer), L"ControlSet%03lu", Number) *
                           sizeof(WCHAR));
    return QzCmSubKey(QzHive->BaseBlock->RootCell, &Name);
}

/* ---------------------------------------------------------------------- */
/* Writing the SYSTEM hive (in memory; registry lock held)                 */
/* ---------------------------------------------------------------------- */

static VOID QzCmReportChange(BOOLEAN IsCell, HCELL_INDEX Cell, PQZ_MEM_KEY Mem, ULONG Filter);

static NTSTATUS
QzCmCreateCellKey(HCELL_INDEX Parent, PCUNICODE_STRING Name, BOOLEAN Volatile, USHORT Flags,
                  PHCELL_INDEX Result)
{
    HSTORAGE_TYPE Storage = (Volatile || HvGetCellType(Parent) == Volatile) ? Volatile : Stable;
    ULONG NameSize = CmpNameSize(QzHive, (PUNICODE_STRING)Name);
    PCM_KEY_NODE Node, ParentNode;
    HCELL_INDEX Cell;

    Cell = HvAllocateCell(QzHive, FIELD_OFFSET(CM_KEY_NODE, Name) + NameSize, Storage, Parent);
    if (Cell == HCELL_NIL)
        return STATUS_INSUFFICIENT_RESOURCES;

    ParentNode = QzCmNode(Parent);
    Node = QzCmNode(Cell);
    RtlZeroMemory(Node, FIELD_OFFSET(CM_KEY_NODE, Name));
    Node->Signature = CM_KEY_NODE_SIGNATURE;
    Node->Flags = Flags | ((Storage == Volatile) ? KEY_IS_VOLATILE : 0) |
                  ((NameSize < Name->Length) ? KEY_COMP_NAME : 0);
    KeQuerySystemTime(&Node->LastWriteTime);
    Node->Parent = Parent;
    Node->SubKeyLists[Stable] = Node->SubKeyLists[Volatile] = HCELL_NIL;
    Node->ValueList.Count = 0;
    Node->ValueList.List = HCELL_NIL;
    Node->Class = HCELL_NIL;
    Node->NameLength = (USHORT)CmpCopyName(QzHive, Node->Name, (PUNICODE_STRING)Name);

    /* Keys share their parent's security descriptor cell */
    Node->Security = ParentNode->Security;
    if (Node->Security != HCELL_NIL)
    {
        PCM_KEY_SECURITY Security = (PCM_KEY_SECURITY)HvGetCell(QzHive, Node->Security);
        Security->ReferenceCount++;
        HvMarkCellDirty(QzHive, Node->Security, FALSE);
    }

    HvMarkCellDirty(QzHive, Parent, FALSE);
    if (!CmpAddSubKey(QzHive, Parent, Cell))
    {
        HvFreeCell(QzHive, Cell);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ParentNode = QzCmNode(Parent);
    if (ParentNode->MaxNameLen < Name->Length)
        ParentNode->MaxNameLen = Name->Length;
    KeQuerySystemTime(&ParentNode->LastWriteTime);
    HvMarkCellDirty(QzHive, Parent, FALSE);
    *Result = Cell;
    return STATUS_SUCCESS;
}

static NTSTATUS
QzCmSetCellValue(HCELL_INDEX KeyCell, PCUNICODE_STRING Name, ULONG Type, PVOID Data, ULONG Length)
{
    HSTORAGE_TYPE Storage = HvGetCellType(KeyCell);
    PCM_KEY_NODE Node = QzCmNode(KeyCell);
    HCELL_INDEX ValueCell = HCELL_NIL, DataCell = HCELL_NIL;
    PCM_KEY_VALUE Value;
    ULONG Index;
    NTSTATUS Status;

    if (Length > CM_KEY_VALUE_BIG)
        return STATUS_INSUFFICIENT_RESOURCES;   /* big data cells: later */

    if (!CmpFindNameInList(QzHive, &Node->ValueList, (PUNICODE_STRING)Name, &Index, &ValueCell))
        return STATUS_INSUFFICIENT_RESOURCES;

    if (ValueCell != HCELL_NIL)
    {
        /* Replace the data of an existing value */
        Value = (PCM_KEY_VALUE)HvGetCell(QzHive, ValueCell);
        HvMarkCellDirty(QzHive, ValueCell, FALSE);
        CmpMarkValueDataDirty(QzHive, Value);
        CmpFreeValueData(QzHive, Value->Data, Value->DataLength);
    }
    else
    {
        ULONG NameSize = CmpNameSize(QzHive, (PUNICODE_STRING)Name);
        ValueCell = HvAllocateCell(QzHive, FIELD_OFFSET(CM_KEY_VALUE, Name) + NameSize, Storage, KeyCell);
        if (ValueCell == HCELL_NIL)
            return STATUS_INSUFFICIENT_RESOURCES;
        Value = (PCM_KEY_VALUE)HvGetCell(QzHive, ValueCell);
        RtlZeroMemory(Value, FIELD_OFFSET(CM_KEY_VALUE, Name));
        Value->Signature = CM_KEY_VALUE_SIGNATURE;
        Value->NameLength = (USHORT)CmpCopyName(QzHive, Value->Name, (PUNICODE_STRING)Name);
        Value->Flags = (NameSize < Name->Length) ? VALUE_COMP_NAME : 0;
        Node = QzCmNode(KeyCell);
        HvMarkCellDirty(QzHive, KeyCell, FALSE);
        if (Node->ValueList.Count)
            HvMarkCellDirty(QzHive, Node->ValueList.List, FALSE);    /* it is reallocated */
        Status = CmpAddValueToList(QzHive, ValueCell, Node->ValueList.Count, Storage, &Node->ValueList);
        if (!NT_SUCCESS(Status))
        {
            HvFreeCell(QzHive, ValueCell);
            return Status;
        }
    }

    /* Up to four bytes live in the value cell itself */
    Value = (PCM_KEY_VALUE)HvGetCell(QzHive, ValueCell);
    Value->Type = Type;
    if (Length <= CM_KEY_VALUE_SMALL)
    {
        Value->Data = 0;
        if (Length)
            RtlCopyMemory(&Value->Data, Data, Length);
        Value->DataLength = Length | CM_KEY_VALUE_SPECIAL_SIZE;
    }
    else
    {
        Status = CmpSetValueDataNew(QzHive, Data, Length, Storage, ValueCell, &DataCell);
        if (!NT_SUCCESS(Status))
        {
            Value->Data = 0;
            Value->DataLength = CM_KEY_VALUE_SPECIAL_SIZE;
            return Status;
        }
        Value = (PCM_KEY_VALUE)HvGetCell(QzHive, ValueCell);
        Value->Data = DataCell;
        Value->DataLength = Length;
    }
    HvMarkCellDirty(QzHive, ValueCell, FALSE);

    Node = QzCmNode(KeyCell);
    if (Node->MaxValueNameLen < Name->Length)
        Node->MaxValueNameLen = Name->Length;
    if (Node->MaxValueDataLen < Length)
        Node->MaxValueDataLen = Length;
    KeQuerySystemTime(&Node->LastWriteTime);
    HvMarkCellDirty(QzHive, KeyCell, FALSE);
    return STATUS_SUCCESS;
}

static NTSTATUS
QzCmDeleteCellValue(HCELL_INDEX KeyCell, PCUNICODE_STRING Name)
{
    PCM_KEY_NODE Node = QzCmNode(KeyCell);
    HCELL_INDEX ValueCell = HCELL_NIL;
    ULONG Index;
    NTSTATUS Status;

    if (!CmpFindNameInList(QzHive, &Node->ValueList, (PUNICODE_STRING)Name, &Index, &ValueCell) ||
        ValueCell == HCELL_NIL)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    /* cmlib wants every cell it changes or frees marked dirty first */
    HvMarkCellDirty(QzHive, KeyCell, FALSE);
    HvMarkCellDirty(QzHive, Node->ValueList.List, FALSE);
    HvMarkCellDirty(QzHive, ValueCell, FALSE);
    CmpMarkValueDataDirty(QzHive, (PCM_KEY_VALUE)HvGetCell(QzHive, ValueCell));
    Node = QzCmNode(KeyCell);
    Status = CmpRemoveValueFromList(QzHive, Index, &Node->ValueList);
    if (!NT_SUCCESS(Status))
        return Status;
    CmpFreeValue(QzHive, ValueCell);
    Node = QzCmNode(KeyCell);
    KeQuerySystemTime(&Node->LastWriteTime);
    HvMarkCellDirty(QzHive, KeyCell, FALSE);
    return STATUS_SUCCESS;
}

/* A key without subkeys; its handles see STATUS_KEY_DELETED afterwards */
static NTSTATUS
QzCmDeleteCellKey(HCELL_INDEX Cell)
{
    PCM_KEY_NODE Node = QzCmNode(Cell);
    PLIST_ENTRY Entry;
    NTSTATUS Status;

    if (Cell == QzHive->BaseBlock->RootCell)
        return STATUS_CANNOT_DELETE;
    if (Node->SubKeyCounts[Stable] || Node->SubKeyCounts[Volatile])
        return STATUS_CANNOT_DELETE;
    Status = CmpFreeKeyByCell(QzHive, Cell, TRUE);
    if (!NT_SUCCESS(Status))
        return Status;
    for (Entry = QzOpenKeys.Flink; Entry != &QzOpenKeys; Entry = Entry->Flink)
    {
        PQZ_KEY Key = CONTAINING_RECORD(Entry, QZ_KEY, Link);
        if (Key->IsCell && Key->Cell == Cell && &Key->CmHive->Hive == QzCurHive)
            Key->Deleted = TRUE;
    }
    if (QzInSystemHive() && Cell == QzControlSetCell)
        QzControlSetCell = HCELL_NIL;
    return STATUS_SUCCESS;
}

/* SYSTEM\CurrentControlSet: a volatile symbolic link to ControlSetNNN */
static VOID
QzCmCreateCurrentControlSetLink(VOID)
{
    UNICODE_STRING Ccs = RTL_CONSTANT_STRING(L"CurrentControlSet");
    UNICODE_STRING LinkValue = RTL_CONSTANT_STRING(L"SymbolicLinkValue");
    PCM_KEY_NODE Target;
    HCELL_INDEX Link;
    WCHAR Buffer[64], Name[32];
    ULONG Chars, NameChars;

    if (QzControlSetCell == HCELL_NIL ||
        QzCmSubKey(QzHive->BaseBlock->RootCell, &Ccs) != HCELL_NIL)
        return;
    Target = QzCmNode(QzControlSetCell);
    NameChars = min(QzCmNodeName(Target, Name, sizeof(Name) - sizeof(WCHAR)),
                    sizeof(Name) - sizeof(WCHAR)) / sizeof(WCHAR);
    Name[NameChars] = UNICODE_NULL;
    Chars = _snwprintf(Buffer, RTL_NUMBER_OF(Buffer), L"\\Registry\\Machine\\System\\%ls", Name);
    if (!NT_SUCCESS(QzCmCreateCellKey(QzHive->BaseBlock->RootCell, &Ccs, TRUE, KEY_SYM_LINK, &Link)))
        return;
    QzCmSetCellValue(Link, &LinkValue, REG_LINK, Buffer, Chars * sizeof(WCHAR));
}

/* ---------------------------------------------------------------------- */
/* Change notification (ZwNotifyChangeKey); registry lock held            */
/* ---------------------------------------------------------------------- */

typedef struct _QZ_NOTIFY_BLOCK
{
    LIST_ENTRY Link;
    PQZ_KEY Key;                       /* watched key object (not referenced) */
    ULONG Filter;
    BOOLEAN WatchTree;
    PKEVENT Event;
    BOOLEAN EventReferenced;
    PWORK_QUEUE_ITEM WorkItem;         /* kernel callers may ask for a work item */
    WORK_QUEUE_TYPE WorkQueue;
    PIO_STATUS_BLOCK IoStatus;
} QZ_NOTIFY_BLOCK, *PQZ_NOTIFY_BLOCK;

static LIST_ENTRY QzNotifyBlocks = { &QzNotifyBlocks, &QzNotifyBlocks };
static ULONG QzNotifyFired;

static VOID
QzCmCompleteNotify(PQZ_NOTIFY_BLOCK Block, NTSTATUS Status)
{
    RemoveEntryList(&Block->Link);
    if (Block->IoStatus)
    {
        Block->IoStatus->Status = Status;
        Block->IoStatus->Information = 0;
    }
    if (Block->Event)
    {
        KeSetEvent(Block->Event, IO_NO_INCREMENT, FALSE);
        if (Block->EventReferenced)
            ObDereferenceObject(Block->Event);
    }
    if (Block->WorkItem)
        ExQueueWorkItem(Block->WorkItem, Block->WorkQueue);
    QzNotifyFired++;
    ExFreePoolWithTag(Block, QZ_CM_TAG);
}

/* Is the key at (IsCell, Cell, Mem) the watched key, or below it? */
static BOOLEAN
QzCmNotifyMatches(PQZ_NOTIFY_BLOCK Block, BOOLEAN IsCell, HCELL_INDEX Cell, PQZ_MEM_KEY Mem)
{
    ULONG Depth = 0;

    if (Block->Key->IsCell != IsCell)
        return FALSE;
    if (IsCell && &Block->Key->CmHive->Hive != QzCurHive)
        return FALSE;
    if (IsCell)
    {
        while (Cell != HCELL_NIL && Depth < 64)
        {
            if (Cell == Block->Key->Cell)
                return Depth == 0 || Block->WatchTree;
            if (Cell == QzHive->BaseBlock->RootCell)
                break;
            Cell = QzCmNode(Cell)->Parent;
            Depth++;
        }
    }
    else
    {
        for (; Mem && Depth < 64; Mem = Mem->Parent, Depth++)
            if (Mem == Block->Key->Mem)
                return Depth == 0 || Block->WatchTree;
    }
    return FALSE;
}

static VOID
QzCmReportChange(BOOLEAN IsCell, HCELL_INDEX Cell, PQZ_MEM_KEY Mem, ULONG Filter)
{
    PLIST_ENTRY Entry, Next;

    for (Entry = QzNotifyBlocks.Flink; Entry != &QzNotifyBlocks; Entry = Next)
    {
        PQZ_NOTIFY_BLOCK Block = CONTAINING_RECORD(Entry, QZ_NOTIFY_BLOCK, Link);
        Next = Entry->Flink;
        if ((Block->Filter & Filter) && QzCmNotifyMatches(Block, IsCell, Cell, Mem))
            QzCmCompleteNotify(Block, STATUS_NOTIFY_ENUM_DIR);
    }
}

/* The watched key's last handle closed (or the object died) */
static VOID
QzCmCancelNotifications(PQZ_KEY Key)
{
    PLIST_ENTRY Entry, Next;

    ExAcquireFastMutex(&QzRegistryLock);
    for (Entry = QzNotifyBlocks.Flink; Entry != &QzNotifyBlocks; Entry = Next)
    {
        PQZ_NOTIFY_BLOCK Block = CONTAINING_RECORD(Entry, QZ_NOTIFY_BLOCK, Link);
        Next = Entry->Flink;
        if (Block->Key == Key)
            QzCmCompleteNotify(Block, STATUS_NOTIFY_CLEANUP);
    }
    ExReleaseFastMutex(&QzRegistryLock);
}

static VOID NTAPI
QzCmCloseKey(PEPROCESS Process, PVOID Object, ACCESS_MASK GrantedAccess,
             ULONG ProcessHandleCount, ULONG SystemHandleCount)
{
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(GrantedAccess);
    UNREFERENCED_PARAMETER(ProcessHandleCount);
    if (SystemHandleCount == 1)
        QzCmCancelNotifications(Object);
}

static VOID NTAPI
QzCmDeleteKey(PVOID Object)
{
    PQZ_KEY Key = Object;
    QzCmCancelNotifications(Key);
    ExAcquireFastMutex(&QzRegistryLock);
    if (Key->Link.Flink)
        RemoveEntryList(&Key->Link);
    ExReleaseFastMutex(&QzRegistryLock);
}

ULONG
QzCmNotificationsFired(VOID)
{
    return QzNotifyFired;
}

/* ---------------------------------------------------------------------- */
/* Key objects and the parse procedure                                     */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzCmCreateKeyObject(PCMHIVE CmHive, BOOLEAN IsCell, HCELL_INDEX Cell, PQZ_MEM_KEY Mem, PQZ_KEY *Result)
{
    PQZ_KEY Key;
    NTSTATUS Status = ObCreateObject(KernelMode, CmKeyObjectType, NULL, KernelMode, NULL,
                                     sizeof(QZ_KEY), 0, 0, (PVOID *)&Key);
    if (!NT_SUCCESS(Status))
        return Status;
    Key->CmHive = IsCell ? CmHive : NULL;
    Key->IsCell = IsCell;
    Key->Deleted = FALSE;
    Key->Cell = Cell;
    Key->Mem = Mem;
    ExAcquireFastMutex(&QzRegistryLock);
    InsertTailList(&QzOpenKeys, &Key->Link);
    ExReleaseFastMutex(&QzRegistryLock);
    *Result = Key;
    return STATUS_SUCCESS;
}

/* Follow a registry symbolic link value inside the SYSTEM hive */
static HCELL_INDEX
QzCmFollowLink(HCELL_INDEX Cell)
{
    static const WCHAR Prefix[] = L"\\Registry\\Machine\\System\\";
    UNICODE_STRING LinkValue = RTL_CONSTANT_STRING(L"SymbolicLinkValue"), Target, Inner;
    PCM_KEY_VALUE Value = QzCmFindValue(Cell, &LinkValue);
    ULONG Length;

    if (!Value)
        return Cell;
    Target.Buffer = QzCmValueData(Value, &Length);
    Target.Length = Target.MaximumLength = (USHORT)Length;
    while (Target.Length && !Target.Buffer[Target.Length / sizeof(WCHAR) - 1])
        Target.Length -= sizeof(WCHAR);
    if (Target.Length <= sizeof(Prefix) - sizeof(WCHAR))
        return HCELL_NIL;
    Inner.Buffer = Target.Buffer + RTL_NUMBER_OF(Prefix) - 1;
    Inner.Length = Inner.MaximumLength = Target.Length - (sizeof(Prefix) - sizeof(WCHAR));
    return QzCmSubKey(QzHive->BaseBlock->RootCell, &Inner);
}

static NTSTATUS NTAPI
QzCmParseKey(PVOID ParseObject, PVOID ObjectType, PACCESS_STATE AccessState,
             KPROCESSOR_MODE AccessMode, ULONG Attributes, PUNICODE_STRING CompleteName,
             PUNICODE_STRING RemainingName, PVOID Context, PSECURITY_QUALITY_OF_SERVICE SecurityQos,
             PVOID *Object)
{
    PQZ_KEY_PARSE Parse = Context;
    PQZ_KEY Start = ParseObject;
    PCMHIVE CmHive = Start->IsCell ? Start->CmHive : NULL;
    BOOLEAN IsCell = Start->IsCell;
    HCELL_INDEX Cell = Start->Cell;
    PQZ_MEM_KEY Mem = Start->Mem;
    UNICODE_STRING Remaining = *RemainingName, Component;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ObjectType);
    UNREFERENCED_PARAMETER(AccessState);
    UNREFERENCED_PARAMETER(AccessMode);
    UNREFERENCED_PARAMETER(Attributes);
    UNREFERENCED_PARAMETER(CompleteName);
    UNREFERENCED_PARAMETER(SecurityQos);

    *Object = NULL;
    if (!Parse || Parse->Type != 'K')
        return STATUS_OBJECT_TYPE_MISMATCH;
    if (Start->Deleted)
        return STATUS_KEY_DELETED;
    Parse->Disposition = REG_OPENED_EXISTING_KEY;

    ExAcquireFastMutex(&QzRegistryLock);
    if (IsCell)
        QzCurHive = &CmHive->Hive;
    for (;;)
    {
        /* Entering a mount point (\REGISTRY\MACHINE\SYSTEM, ...) switches to its hive */
        if (!IsCell && Mem && Mem->MountedHive)
        {
            IsCell = TRUE;
            CmHive = Mem->MountedHive;
            QzCurHive = &CmHive->Hive;
            Cell = QzHive->BaseBlock->RootCell;
        }

        while (Remaining.Length && Remaining.Buffer[0] == L'\\')
        {
            Remaining.Buffer++;
            Remaining.Length -= sizeof(WCHAR);
        }
        if (!Remaining.Length)
            break;
        Component.Buffer = Remaining.Buffer;
        Component.Length = 0;
        while (Component.Length < Remaining.Length &&
               Component.Buffer[Component.Length / sizeof(WCHAR)] != L'\\')
            Component.Length += sizeof(WCHAR);
        Component.MaximumLength = Component.Length;
        Remaining.Buffer += Component.Length / sizeof(WCHAR);
        Remaining.Length -= Component.Length;

        if (IsCell)
        {
            UNICODE_STRING Ccs = RTL_CONSTANT_STRING(L"CurrentControlSet");
            HCELL_INDEX Next = QzCmSubKey(Cell, &Component);
            if (Next == HCELL_NIL && QzInSystemHive() && Cell == QzHive->BaseBlock->RootCell &&
                RtlEqualUnicodeString(&Component, &Ccs, TRUE))
                Next = QzControlSetCell;
            if (Next != HCELL_NIL && QzInSystemHive() && (QzCmNode(Next)->Flags & KEY_SYM_LINK))
                Next = QzCmFollowLink(Next);
            if (Next != HCELL_NIL)
            {
                Cell = Next;
                continue;
            }
            if (!Parse->Create)
            {
                ExReleaseFastMutex(&QzRegistryLock);
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
            /* A new key in the hive; below a volatile key only volatile ones */
            if (!Parse->Volatile && HvGetCellType(Cell) == Volatile)
            {
                ExReleaseFastMutex(&QzRegistryLock);
                return STATUS_CHILD_MUST_BE_VOLATILE;
            }
            Status = QzCmCreateCellKey(Cell, &Component, Parse->Volatile, 0, &Next);
            if (!NT_SUCCESS(Status))
            {
                ExReleaseFastMutex(&QzRegistryLock);
                return Status;
            }
            QzCmReportChange(TRUE, Cell, NULL, REG_NOTIFY_CHANGE_NAME);
            Parse->Disposition = REG_CREATED_NEW_KEY;
            Cell = Next;
            continue;
        }

        {
            PQZ_MEM_KEY Next = QzMemFindKey(Mem, &Component);
            if (!Next)
            {
                if (!Parse->Create)
                {
                    ExReleaseFastMutex(&QzRegistryLock);
                    return STATUS_OBJECT_NAME_NOT_FOUND;
                }
                Next = QzMemCreateKey(Mem, Component.Buffer, Component.Length);
                QzCmReportChange(FALSE, HCELL_NIL, Mem, REG_NOTIFY_CHANGE_NAME);
                Parse->Disposition = REG_CREATED_NEW_KEY;
                if (!Next)
                {
                    ExReleaseFastMutex(&QzRegistryLock);
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
            }
            Mem = Next;
        }
    }
    ExReleaseFastMutex(&QzRegistryLock);

    Status = QzCmCreateKeyObject(CmHive, IsCell, Cell, IsCell ? NULL : Mem, (PQZ_KEY *)Object);
    return Status;
}

/* ---------------------------------------------------------------------- */
/* Key services                                                            */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzCmOpen(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
         BOOLEAN Create, ULONG CreateOptions, PULONG Disposition)
{
    QZ_KEY_PARSE Parse;
    OBJECT_ATTRIBUTES Attributes = *ObjectAttributes;
    NTSTATUS Status;

    Parse.Type = 'K';
    Parse.Create = Create;
    Parse.Volatile = (CreateOptions & REG_OPTION_VOLATILE) != 0;
    Parse.Disposition = REG_OPENED_EXISTING_KEY;
    Attributes.Attributes |= OBJ_CASE_INSENSITIVE;
    if (ExGetPreviousMode() != KernelMode)
        Attributes.Attributes &= ~OBJ_KERNEL_HANDLE;

    /* An empty name relative to a key reopens that key */
    if (Attributes.RootDirectory && (!Attributes.ObjectName || !Attributes.ObjectName->Length))
    {
        PQZ_KEY Key, Copy;
        Status = ObReferenceObjectByHandle(Attributes.RootDirectory, 0, CmKeyObjectType, KernelMode,
                                           (PVOID *)&Key, NULL);
        if (!NT_SUCCESS(Status))
            return Status;
        Status = QzCmCreateKeyObject(Key->CmHive, Key->IsCell, Key->Cell, Key->Mem, &Copy);
        if (NT_SUCCESS(Status))
        {
            Status = ObOpenObjectByPointer(Copy, Attributes.Attributes & OBJ_KERNEL_HANDLE, NULL, DesiredAccess,
                                           CmKeyObjectType, KernelMode, KeyHandle);
            ObDereferenceObject(Copy);
        }
        ObDereferenceObject(Key);
        if (Disposition)
            *Disposition = REG_OPENED_EXISTING_KEY;
        return Status;
    }

    Status = ObOpenObjectByName(&Attributes, CmKeyObjectType, KernelMode, NULL, DesiredAccess,
                                &Parse, KeyHandle);
    if (Disposition)
        *Disposition = Parse.Disposition;
    return Status;
}

NTSTATUS
NTAPI
ZwOpenKey(OUT PHANDLE KeyHandle, IN ACCESS_MASK DesiredAccess, IN POBJECT_ATTRIBUTES ObjectAttributes)
{
    return QzCmOpen(KeyHandle, DesiredAccess, ObjectAttributes, FALSE, 0, NULL);
}

NTSTATUS
NTAPI
NtOpenKey(OUT PHANDLE KeyHandle, IN ACCESS_MASK DesiredAccess, IN POBJECT_ATTRIBUTES ObjectAttributes)
{
    return ZwOpenKey(KeyHandle, DesiredAccess, ObjectAttributes);
}

NTSTATUS
NTAPI
ZwOpenKeyEx(OUT PHANDLE KeyHandle, IN ACCESS_MASK DesiredAccess,
            IN POBJECT_ATTRIBUTES ObjectAttributes, IN ULONG OpenOptions)
{
    UNREFERENCED_PARAMETER(OpenOptions);
    return ZwOpenKey(KeyHandle, DesiredAccess, ObjectAttributes);
}

NTSTATUS
NTAPI
ZwCreateKey(OUT PHANDLE KeyHandle, IN ACCESS_MASK DesiredAccess,
            IN POBJECT_ATTRIBUTES ObjectAttributes, IN ULONG TitleIndex,
            IN PUNICODE_STRING Class OPTIONAL, IN ULONG CreateOptions,
            OUT PULONG Disposition OPTIONAL)
{
    UNREFERENCED_PARAMETER(TitleIndex);
    UNREFERENCED_PARAMETER(Class);
    return QzCmOpen(KeyHandle, DesiredAccess, ObjectAttributes, TRUE, CreateOptions, Disposition);
}

NTSTATUS
NTAPI
NtCreateKey(OUT PHANDLE KeyHandle, IN ACCESS_MASK DesiredAccess,
            IN POBJECT_ATTRIBUTES ObjectAttributes, IN ULONG TitleIndex,
            IN PUNICODE_STRING Class OPTIONAL, IN ULONG CreateOptions,
            OUT PULONG Disposition OPTIONAL)
{
    return ZwCreateKey(KeyHandle, DesiredAccess, ObjectAttributes, TitleIndex, Class,
                       CreateOptions, Disposition);
}

static NTSTATUS
QzCmReferenceKey(HANDLE KeyHandle, PQZ_KEY *Key)
{
    NTSTATUS Status = ObReferenceObjectByHandle(KeyHandle, 0, CmKeyObjectType, KernelMode,
                                                (PVOID *)Key, NULL);
    if (NT_SUCCESS(Status) && (*Key)->Deleted)
    {
        ObDereferenceObject(*Key);
        return STATUS_KEY_DELETED;
    }
    return Status;
}

/* Everything the KEY_*_INFORMATION classes report */
typedef struct _QZ_KEY_FACTS
{
    WCHAR Name[512];                   /* KeyNameInformation: the full path */
    ULONG NameBytes;
    ULONG SubKeys, Values, MaxNameLen, MaxValueNameLen, MaxValueDataLen;
    LARGE_INTEGER LastWriteTime;
} QZ_KEY_FACTS, *PQZ_KEY_FACTS;

static VOID
QzCmGetFacts(BOOLEAN IsCell, HCELL_INDEX Cell, PQZ_MEM_KEY Mem, PQZ_KEY_FACTS Facts)
{
    RtlZeroMemory(Facts, sizeof(*Facts));
    if (IsCell)
    {
        PCM_KEY_NODE Node = QzCmNode(Cell);
        Facts->NameBytes = min(QzCmNodeName(Node, NULL, 0), sizeof(Facts->Name));
        QzCmNodeName(Node, Facts->Name, Facts->NameBytes);
        Facts->SubKeys = Node->SubKeyCounts[Stable];
        Facts->Values = Node->ValueList.Count;
        Facts->MaxNameLen = Node->MaxNameLen;
        Facts->MaxValueNameLen = Node->MaxValueNameLen;
        Facts->MaxValueDataLen = Node->MaxValueDataLen;
        Facts->LastWriteTime = Node->LastWriteTime;
    }
    else
    {
        PQZ_MEM_KEY Child;
        PQZ_MEM_VALUE Value;
        Facts->NameBytes = min(Mem->NameLength, sizeof(Facts->Name));
        RtlCopyMemory(Facts->Name, Mem->Name, Facts->NameBytes);
        Facts->SubKeys = Mem->SubKeys;
        Facts->Values = Mem->ValueCount;
        Facts->LastWriteTime = Mem->LastWriteTime;
        for (Child = Mem->Child; Child; Child = Child->Sibling)
            Facts->MaxNameLen = max(Facts->MaxNameLen, Child->NameLength);
        for (Value = Mem->Values; Value; Value = Value->Next)
        {
            Facts->MaxValueNameLen = max(Facts->MaxValueNameLen, Value->NameLength);
            Facts->MaxValueDataLen = max(Facts->MaxValueDataLen, Value->DataLength);
        }
    }
}

static NTSTATUS
QzCmKeyInformation(PQZ_KEY_FACTS Facts, KEY_INFORMATION_CLASS Class, PVOID Buffer, ULONG Length,
                   PULONG ResultLength)
{
    ULONG Fixed, Needed;
    PWCHAR NameOut;

    switch (Class)
    {
        case KeyBasicInformation:
        {
            PKEY_BASIC_INFORMATION Info = Buffer;
            Fixed = FIELD_OFFSET(KEY_BASIC_INFORMATION, Name);
            *ResultLength = Needed = Fixed + Facts->NameBytes;
            if (Length < Fixed)
                return STATUS_BUFFER_TOO_SMALL;
            Info->LastWriteTime = Facts->LastWriteTime;
            Info->TitleIndex = 0;
            Info->NameLength = Facts->NameBytes;
            NameOut = Info->Name;
            break;
        }
        case KeyNodeInformation:
        {
            PKEY_NODE_INFORMATION Info = Buffer;
            Fixed = FIELD_OFFSET(KEY_NODE_INFORMATION, Name);
            *ResultLength = Needed = Fixed + Facts->NameBytes;
            if (Length < Fixed)
                return STATUS_BUFFER_TOO_SMALL;
            Info->LastWriteTime = Facts->LastWriteTime;
            Info->TitleIndex = 0;
            Info->ClassOffset = 0xFFFFFFFF;
            Info->ClassLength = 0;
            Info->NameLength = Facts->NameBytes;
            NameOut = Info->Name;
            break;
        }
        case KeyFullInformation:
        case KeyCachedInformation:
        {
            PKEY_FULL_INFORMATION Info = Buffer;
            *ResultLength = Fixed = FIELD_OFFSET(KEY_FULL_INFORMATION, Class);
            if (Length < Fixed)
                return STATUS_BUFFER_TOO_SMALL;
            RtlZeroMemory(Info, Fixed);
            Info->LastWriteTime = Facts->LastWriteTime;
            Info->ClassOffset = 0xFFFFFFFF;
            Info->SubKeys = Facts->SubKeys;
            Info->MaxNameLen = Facts->MaxNameLen;
            Info->Values = Facts->Values;
            Info->MaxValueNameLen = Facts->MaxValueNameLen;
            Info->MaxValueDataLen = Facts->MaxValueDataLen;
            return STATUS_SUCCESS;
        }
        case KeyNameInformation:
        {
            PKEY_NAME_INFORMATION Info = Buffer;
            Fixed = FIELD_OFFSET(KEY_NAME_INFORMATION, Name);
            *ResultLength = Needed = Fixed + Facts->NameBytes;
            if (Length < Fixed)
                return STATUS_BUFFER_TOO_SMALL;
            Info->NameLength = Facts->NameBytes;
            NameOut = Info->Name;
            break;
        }
        default:
            return STATUS_INVALID_PARAMETER;
    }

    RtlCopyMemory(NameOut, Facts->Name, min(Facts->NameBytes, Length - (ULONG)((PUCHAR)NameOut - (PUCHAR)Buffer)));
    return (Length < Needed) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

/* The in-memory key a hive is mounted at (the first, for DEFAULT's two) */
static PQZ_MEM_KEY
QzCmFindMount(PQZ_MEM_KEY Mem, PCMHIVE CmHive)
{
    PQZ_MEM_KEY Child, Found;
    if (Mem->MountedHive == CmHive)
        return Mem;
    for (Child = Mem->Child; Child; Child = Child->Sibling)
        if ((Found = QzCmFindMount(Child, CmHive)) != NULL)
            return Found;
    return NULL;
}

/* Put Name (Bytes long) in front of the path being built backwards in Buffer */
static BOOLEAN
QzCmPrepend(PWCHAR Buffer, PULONG Start, PCWSTR Name, ULONG Bytes)
{
    if (*Start < Bytes + sizeof(WCHAR))
        return FALSE;
    *Start -= Bytes;
    RtlCopyMemory((PUCHAR)Buffer + *Start, Name, Bytes);
    *Start -= sizeof(WCHAR);
    *(PWCHAR)((PUCHAR)Buffer + *Start) = L'\\';
    return TRUE;
}

/* "\REGISTRY\MACHINE\SOFTWARE\Classes\..." for a key (registry lock held, its hive selected) */
static ULONG
QzCmFullName(PQZ_KEY Key, PWCHAR Buffer, ULONG MaxBytes)
{
    ULONG Start = MaxBytes & ~1UL, Bytes;
    PQZ_MEM_KEY Mem = Key->Mem;
    WCHAR Name[128];

    if (Key->IsCell)
    {
        HCELL_INDEX Cell = Key->Cell;
        while (Cell != HCELL_NIL && Cell != QzHive->BaseBlock->RootCell)
        {
            PCM_KEY_NODE Node = QzCmNode(Cell);
            Bytes = min(QzCmNodeName(Node, NULL, 0), sizeof(Name));
            QzCmNodeName(Node, Name, Bytes);
            if (!QzCmPrepend(Buffer, &Start, Name, Bytes))
                break;
            Cell = Node->Parent;
        }
        Mem = QzCmFindMount(QzRegistryRoot, Key->CmHive);
    }
    for (; Mem && Mem != QzRegistryRoot; Mem = Mem->Parent)
        if (!QzCmPrepend(Buffer, &Start, Mem->Name, Mem->NameLength))
            break;
    QzCmPrepend(Buffer, &Start, L"REGISTRY", 8 * sizeof(WCHAR));
    Bytes = (MaxBytes & ~1UL) - Start;
    RtlMoveMemory(Buffer, (PUCHAR)Buffer + Start, Bytes);
    return Bytes;
}

NTSTATUS
NTAPI
ZwQueryKey(IN HANDLE KeyHandle, IN KEY_INFORMATION_CLASS KeyInformationClass,
           OUT PVOID KeyInformation, IN ULONG Length, OUT PULONG ResultLength)
{
    QZ_KEY_FACTS Facts;
    PQZ_KEY Key;
    NTSTATUS Status = QzCmReferenceKey(KeyHandle, &Key);

    if (!NT_SUCCESS(Status))
        return Status;
    ExAcquireFastMutex(&QzRegistryLock);
    QzSelectHive(Key);
    QzCmGetFacts(Key->IsCell, Key->Cell, Key->Mem, &Facts);
    /* NT names the key in full here (advapi32 finds HKCR's merge points by it) */
    if (KeyInformationClass == KeyNameInformation)
        Facts.NameBytes = QzCmFullName(Key, Facts.Name, sizeof(Facts.Name));
    ExReleaseFastMutex(&QzRegistryLock);
    Status = QzCmKeyInformation(&Facts, KeyInformationClass, KeyInformation, Length, ResultLength);
    ObDereferenceObject(Key);
    return Status;
}

NTSTATUS
NTAPI
ZwEnumerateKey(IN HANDLE KeyHandle, IN ULONG Index, IN KEY_INFORMATION_CLASS KeyInformationClass,
               OUT PVOID KeyInformation, IN ULONG Length, OUT PULONG ResultLength)
{
    QZ_KEY_FACTS Facts;
    PQZ_KEY Key;
    BOOLEAN Found = FALSE;
    NTSTATUS Status = QzCmReferenceKey(KeyHandle, &Key);

    if (!NT_SUCCESS(Status))
        return Status;
    ExAcquireFastMutex(&QzRegistryLock);
    QzSelectHive(Key);
    if (Key->IsCell)
    {
        HCELL_INDEX Child = CmpFindSubKeyByNumber(QzHive, QzCmNode(Key->Cell), Index);
        if (Child != HCELL_NIL)
        {
            QzCmGetFacts(TRUE, Child, NULL, &Facts);
            Found = TRUE;
        }
    }
    else
    {
        PQZ_MEM_KEY Child = Key->Mem->Child;
        while (Child && Index--)
            Child = Child->Sibling;
        if (Child)
        {
            QzCmGetFacts(FALSE, HCELL_NIL, Child, &Facts);
            Found = TRUE;
        }
    }
    ExReleaseFastMutex(&QzRegistryLock);
    Status = Found ? QzCmKeyInformation(&Facts, KeyInformationClass, KeyInformation, Length, ResultLength)
                   : STATUS_NO_MORE_ENTRIES;
    ObDereferenceObject(Key);
    return Status;
}

static NTSTATUS
QzCmValueInformation(PCWSTR Name, ULONG NameBytes, ULONG Type, const VOID *Data, ULONG DataBytes,
                     KEY_VALUE_INFORMATION_CLASS Class, PVOID Buffer, ULONG Length,
                     PULONG ResultLength)
{
    ULONG Fixed, Needed, DataOffset;
    BOOLEAN Overflow;

    switch (Class)
    {
        case KeyValueBasicInformation:
        {
            PKEY_VALUE_BASIC_INFORMATION Info = Buffer;
            Fixed = FIELD_OFFSET(KEY_VALUE_BASIC_INFORMATION, Name);
            *ResultLength = Needed = Fixed + NameBytes;
            if (Length < Fixed)
                return STATUS_BUFFER_TOO_SMALL;
            Info->TitleIndex = 0;
            Info->Type = Type;
            Info->NameLength = NameBytes;
            RtlCopyMemory(Info->Name, Name, min(NameBytes, Length - Fixed));
            return (Length < Needed) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
        }

        case KeyValuePartialInformation:
        case KeyValuePartialInformationAlign64:
        {
            PKEY_VALUE_PARTIAL_INFORMATION Info = Buffer;
            Fixed = FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data);
            *ResultLength = Needed = Fixed + DataBytes;
            if (Length < Fixed)
                return STATUS_BUFFER_TOO_SMALL;
            Info->TitleIndex = 0;
            Info->Type = Type;
            Info->DataLength = DataBytes;
            Overflow = Length < Needed;
            if (Data)
                RtlCopyMemory(Info->Data, Data, min(DataBytes, Length - Fixed));
            return Overflow ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
        }

        case KeyValueFullInformation:
        case KeyValueFullInformationAlign64:
        {
            PKEY_VALUE_FULL_INFORMATION Info = Buffer;
            Fixed = FIELD_OFFSET(KEY_VALUE_FULL_INFORMATION, Name);
            DataOffset = ALIGN_UP_BY(Fixed + NameBytes, sizeof(ULONG));
            *ResultLength = Needed = DataOffset + DataBytes;
            if (Length < Fixed)
                return STATUS_BUFFER_TOO_SMALL;
            Info->TitleIndex = 0;
            Info->Type = Type;
            Info->NameLength = NameBytes;
            Info->DataOffset = DataOffset;
            Info->DataLength = DataBytes;
            RtlCopyMemory(Info->Name, Name, min(NameBytes, Length - Fixed));
            Overflow = Length < Needed;
            if (!Overflow && Data)
                RtlCopyMemory((PUCHAR)Info + DataOffset, Data, DataBytes);
            return Overflow ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
        }

        default:
            return STATUS_INVALID_PARAMETER;
    }
}

static NTSTATUS
QzCmCellValueInformation(PCM_KEY_VALUE Value, KEY_VALUE_INFORMATION_CLASS Class, PVOID Buffer,
                         ULONG Length, PULONG ResultLength)
{
    WCHAR Name[256];
    ULONG NameBytes = min(QzCmValueName(Value, NULL, 0), sizeof(Name)), DataBytes = 0;
    PVOID Data = QzCmValueData(Value, &DataBytes);

    QzCmValueName(Value, Name, NameBytes);
    return QzCmValueInformation(Name, NameBytes, Value->Type, Data, DataBytes, Class, Buffer, Length,
                                ResultLength);
}

NTSTATUS
NTAPI
ZwQueryValueKey(IN HANDLE KeyHandle, IN PUNICODE_STRING ValueName,
                IN KEY_VALUE_INFORMATION_CLASS KeyValueInformationClass,
                OUT PVOID KeyValueInformation, IN ULONG Length, OUT PULONG ResultLength)
{
    PQZ_KEY Key;
    NTSTATUS Status = QzCmReferenceKey(KeyHandle, &Key);
    UNICODE_STRING Empty = RTL_CONSTANT_STRING(L"");

    if (!NT_SUCCESS(Status))
        return Status;
    if (!ValueName)
        ValueName = &Empty;
    *ResultLength = 0;
    ExAcquireFastMutex(&QzRegistryLock);
    QzSelectHive(Key);
    if (Key->IsCell)
    {
        PCM_KEY_VALUE Value = QzCmFindValue(Key->Cell, ValueName);
        Status = Value ? QzCmCellValueInformation(Value, KeyValueInformationClass, KeyValueInformation,
                                                  Length, ResultLength)
                       : STATUS_OBJECT_NAME_NOT_FOUND;
    }
    else
    {
        PQZ_MEM_VALUE Value = QzMemFindValue(Key->Mem, ValueName);
        Status = Value ? QzCmValueInformation(Value->Name, Value->NameLength, Value->Type, Value->Data,
                                              Value->DataLength, KeyValueInformationClass,
                                              KeyValueInformation, Length, ResultLength)
                       : STATUS_OBJECT_NAME_NOT_FOUND;
    }
    ExReleaseFastMutex(&QzRegistryLock);
    ObDereferenceObject(Key);
    return Status;
}

NTSTATUS
NTAPI
ZwEnumerateValueKey(IN HANDLE KeyHandle, IN ULONG Index,
                    IN KEY_VALUE_INFORMATION_CLASS KeyValueInformationClass,
                    OUT PVOID KeyValueInformation, IN ULONG Length, OUT PULONG ResultLength)
{
    PQZ_KEY Key;
    NTSTATUS Status = QzCmReferenceKey(KeyHandle, &Key);

    if (!NT_SUCCESS(Status))
        return Status;
    *ResultLength = 0;
    ExAcquireFastMutex(&QzRegistryLock);
    QzSelectHive(Key);
    if (Key->IsCell)
    {
        PCM_KEY_VALUE Value = QzCmValueByIndex(Key->Cell, Index);
        Status = Value ? QzCmCellValueInformation(Value, KeyValueInformationClass, KeyValueInformation,
                                                  Length, ResultLength)
                       : STATUS_NO_MORE_ENTRIES;
    }
    else
    {
        PQZ_MEM_VALUE Value = Key->Mem->Values;
        while (Value && Index--)
            Value = Value->Next;
        Status = Value ? QzCmValueInformation(Value->Name, Value->NameLength, Value->Type, Value->Data,
                                              Value->DataLength, KeyValueInformationClass,
                                              KeyValueInformation, Length, ResultLength)
                       : STATUS_NO_MORE_ENTRIES;
    }
    ExReleaseFastMutex(&QzRegistryLock);
    ObDereferenceObject(Key);
    return Status;
}

NTSTATUS
NTAPI
ZwSetValueKey(IN HANDLE KeyHandle, IN PUNICODE_STRING ValueName, IN ULONG TitleIndex,
              IN ULONG Type, IN PVOID Data, IN ULONG DataSize)
{
    UNICODE_STRING Empty = RTL_CONSTANT_STRING(L"");
    PQZ_KEY Key;
    NTSTATUS Status = QzCmReferenceKey(KeyHandle, &Key);

    UNREFERENCED_PARAMETER(TitleIndex);
    if (!NT_SUCCESS(Status))
        return Status;
    ExAcquireFastMutex(&QzRegistryLock);
    QzSelectHive(Key);
    if (Key->IsCell)
        Status = QzCmSetCellValue(Key->Cell, ValueName ? ValueName : &Empty, Type, Data, DataSize);
    else
        Status = QzMemSetValue(Key->Mem, ValueName ? ValueName : &Empty, Type, Data, DataSize);
    if (NT_SUCCESS(Status))
        QzCmReportChange(Key->IsCell, Key->Cell, Key->Mem, REG_NOTIFY_CHANGE_LAST_SET);
    ExReleaseFastMutex(&QzRegistryLock);
    ObDereferenceObject(Key);
    return Status;
}

NTSTATUS
NTAPI
ZwDeleteValueKey(IN HANDLE KeyHandle, IN PUNICODE_STRING ValueName)
{
    PQZ_KEY Key;
    NTSTATUS Status = QzCmReferenceKey(KeyHandle, &Key);

    if (!NT_SUCCESS(Status))
        return Status;
    if (Key->IsCell)
    {
        ExAcquireFastMutex(&QzRegistryLock);
        QzSelectHive(Key);
        Status = QzCmDeleteCellValue(Key->Cell, ValueName);
        if (NT_SUCCESS(Status))
            QzCmReportChange(TRUE, Key->Cell, NULL, REG_NOTIFY_CHANGE_LAST_SET);
        ExReleaseFastMutex(&QzRegistryLock);
    }
    else
    {
        PQZ_MEM_VALUE *Link, Value;
        Status = STATUS_OBJECT_NAME_NOT_FOUND;
        ExAcquireFastMutex(&QzRegistryLock);
        QzSelectHive(Key);
        for (Link = &Key->Mem->Values; (Value = *Link); Link = &Value->Next)
        {
            if (QzCmNamesEqual(Value->Name, Value->NameLength, ValueName->Buffer, ValueName->Length))
            {
                *Link = Value->Next;
                Key->Mem->ValueCount--;
                if (Value->Data)
                    ExFreePoolWithTag(Value->Data, QZ_CM_TAG);
                ExFreePoolWithTag(Value, QZ_CM_TAG);
                Status = STATUS_SUCCESS;
                break;
            }
        }
        ExReleaseFastMutex(&QzRegistryLock);
    }
    ObDereferenceObject(Key);
    return Status;
}

NTSTATUS
NTAPI
ZwDeleteKey(IN HANDLE KeyHandle)
{
    PQZ_KEY Key;
    NTSTATUS Status = QzCmReferenceKey(KeyHandle, &Key);

    if (!NT_SUCCESS(Status))
        return Status;
    if (Key->IsCell)
    {
        PCM_KEY_NODE Node;
        HCELL_INDEX Parent;
        ExAcquireFastMutex(&QzRegistryLock);
        QzSelectHive(Key);
        Node = QzCmNode(Key->Cell);
        Parent = Node->Parent;
        if (Node->SubKeyCounts[Stable] + Node->SubKeyCounts[Volatile] ||
            Key->Cell == QzHive->BaseBlock->RootCell)
        {
            Status = STATUS_CANNOT_DELETE;
        }
        else
        {
            /* Watchers of the key and of the trees above it hear first */
            QzCmReportChange(TRUE, Key->Cell, NULL, REG_NOTIFY_CHANGE_NAME);
            Status = QzCmDeleteCellKey(Key->Cell);
            if (NT_SUCCESS(Status))
                QzCmReportChange(TRUE, Parent, NULL, REG_NOTIFY_CHANGE_NAME);
        }
        ExReleaseFastMutex(&QzRegistryLock);
    }
    else if (Key->Mem->Child)
    {
        Status = STATUS_CANNOT_DELETE;
    }
    else if (Key->Mem->Parent)
    {
        /* Unlinked; open handles keep the memory (it is never freed) */
        PQZ_MEM_KEY *Link;
        ExAcquireFastMutex(&QzRegistryLock);
        QzSelectHive(Key);
        for (Link = &Key->Mem->Parent->Child; *Link; Link = &(*Link)->Sibling)
        {
            if (*Link == Key->Mem)
            {
                *Link = Key->Mem->Sibling;
                Key->Mem->Parent->SubKeys--;
                QzCmReportChange(FALSE, HCELL_NIL, Key->Mem, REG_NOTIFY_CHANGE_NAME);
                Key->Mem->Parent = NULL;
                break;
            }
        }
        ExReleaseFastMutex(&QzRegistryLock);
    }
    ObDereferenceObject(Key);
    return Status;
}

NTSTATUS
NTAPI
ZwNotifyChangeKey(IN HANDLE KeyHandle,
                  IN HANDLE Event OPTIONAL,
                  IN PIO_APC_ROUTINE ApcRoutine OPTIONAL,
                  IN PVOID ApcContext OPTIONAL,
                  OUT PIO_STATUS_BLOCK IoStatusBlock,
                  IN ULONG CompletionFilter,
                  IN BOOLEAN WatchTree,
                  OUT PVOID Buffer OPTIONAL,
                  IN ULONG BufferSize,
                  IN BOOLEAN Asynchronous)
{
    PQZ_NOTIFY_BLOCK Block;
    KEVENT LocalEvent;
    PQZ_KEY Key;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(BufferSize);

    Status = QzCmReferenceKey(KeyHandle, &Key);
    if (!NT_SUCCESS(Status))
        return Status;
    Block = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Block), QZ_CM_TAG);
    if (!Block)
    {
        ObDereferenceObject(Key);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Block, sizeof(*Block));
    Block->Key = Key;
    Block->Filter = CompletionFilter & REG_LEGAL_CHANGE_FILTER;
    Block->WatchTree = WatchTree;
    Block->IoStatus = IoStatusBlock;

    /* Kernel callers pass a WORK_QUEUE_ITEM and its queue type in place of
       an APC routine and context; real APCs arrive with stage B6 */
    if (ApcRoutine)
    {
        Block->WorkItem = (PWORK_QUEUE_ITEM)ApcRoutine;
        Block->WorkQueue = (WORK_QUEUE_TYPE)(ULONG_PTR)ApcContext;
    }
    if (Event)
    {
        Status = ObReferenceObjectByHandle(Event, EVENT_MODIFY_STATE, ExEventObjectType, KernelMode,
                                           (PVOID *)&Block->Event, NULL);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(Block, QZ_CM_TAG);
            ObDereferenceObject(Key);
            return Status;
        }
        Block->EventReferenced = TRUE;
        KeClearEvent(Block->Event);
    }
    else if (!Asynchronous)
    {
        KeInitializeEvent(&LocalEvent, NotificationEvent, FALSE);
        Block->Event = &LocalEvent;
    }

    IoStatusBlock->Status = STATUS_PENDING;
    ExAcquireFastMutex(&QzRegistryLock);
    QzSelectHive(Key);
    InsertTailList(&QzNotifyBlocks, &Block->Link);
    ExReleaseFastMutex(&QzRegistryLock);
    ObDereferenceObject(Key);

    if (Asynchronous)
        return STATUS_PENDING;
    KeWaitForSingleObject(Block->EventReferenced ? Block->Event : &LocalEvent, Executive, KernelMode,
                          FALSE, NULL);
    return IoStatusBlock->Status;
}

NTSTATUS
NTAPI
ZwNotifyChangeMultipleKeys(IN HANDLE MasterKeyHandle,
                           IN ULONG Count,
                           IN POBJECT_ATTRIBUTES SubordinateObjects OPTIONAL,
                           IN HANDLE Event OPTIONAL,
                           IN PIO_APC_ROUTINE ApcRoutine OPTIONAL,
                           IN PVOID ApcContext OPTIONAL,
                           OUT PIO_STATUS_BLOCK IoStatusBlock,
                           IN ULONG CompletionFilter,
                           IN BOOLEAN WatchTree,
                           OUT PVOID Buffer OPTIONAL,
                           IN ULONG BufferSize,
                           IN BOOLEAN Asynchronous)
{
    if (Count)
        return STATUS_NOT_IMPLEMENTED;   /* subordinate keys: later */
    UNREFERENCED_PARAMETER(SubordinateObjects);
    return ZwNotifyChangeKey(MasterKeyHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
                             CompletionFilter, WatchTree, Buffer, BufferSize, Asynchronous);
}

NTSTATUS
NTAPI
ZwFlushKey(IN HANDLE KeyHandle)
{
    UNREFERENCED_PARAMETER(KeyHandle);
    return STATUS_SUCCESS;
}

/* A hive file, read whole and copied into bins of its own */
static NTSTATUS
QzCmLoadHiveFile(PCWSTR FileName, PCMHIVE *Result)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Iosb;
    FILE_STANDARD_INFORMATION Standard;
    PCMHIVE CmHive = NULL;
    PVOID Buffer = NULL;
    HANDLE File;
    ULONG Size;
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, FileName);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenFile(&File, FILE_READ_DATA | SYNCHRONIZE, &Attributes, &Iosb, FILE_SHARE_READ,
                        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = ZwQueryInformationFile(File, &Iosb, &Standard, sizeof(Standard), FileStandardInformation);
    if (NT_SUCCESS(Status) && (Standard.EndOfFile.HighPart || Standard.EndOfFile.LowPart < HBLOCK_SIZE ||
                               Standard.EndOfFile.LowPart > 32 * 1024 * 1024))
        Status = STATUS_REGISTRY_CORRUPT;
    if (NT_SUCCESS(Status))
    {
        Size = Standard.EndOfFile.LowPart;
        Buffer = ExAllocatePoolWithTag(NonPagedPool, Size, QZ_CM_TAG);
        Status = Buffer ? ZwReadFile(File, NULL, NULL, NULL, &Iosb, Buffer, Size, NULL, NULL)
                        : STATUS_INSUFFICIENT_RESOURCES;
        if (NT_SUCCESS(Status) && Iosb.Information != Size)
            Status = STATUS_REGISTRY_IO_FAILED;
    }
    ZwClose(File);
    if (NT_SUCCESS(Status))
    {
        CmHive = CmpAllocate(sizeof(CMHIVE), FALSE, QZ_CM_TAG);
        Status = CmHive ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
    }
    if (NT_SUCCESS(Status))
    {
        RtlZeroMemory(CmHive, sizeof(CMHIVE));
        ExAcquireFastMutex(&QzRegistryLock);
        Status = HvInitialize(&CmHive->Hive, HINIT_MEMORY, HIVE_NOLAZYFLUSH, HFILE_TYPE_PRIMARY, Buffer,
                              CmpAllocate, CmpFree, NULL, NULL, NULL, NULL, 1, NULL);
        ExReleaseFastMutex(&QzRegistryLock);
        if (!NT_SUCCESS(Status))
        {
            CmpFree(CmHive, 0);
            CmHive = NULL;
        }
    }
    if (Buffer)
        ExFreePoolWithTag(Buffer, QZ_CM_TAG);
    *Result = CmHive;
    return Status;
}

/*
 * smss asks for the rest of the registry: the hives in System32\config,
 * mounted where NT mounts them. They stay in memory (the boot CD cannot
 * take them back; writing hives to a disk is still to come).
 */
static VOID
QzCmLoadSystemHives(VOID)
{
    static const struct { PCWSTR File; PCWSTR Mounts[2]; } Hives[] =
    {
        { L"\\SystemRoot\\System32\\config\\SOFTWARE", { L"MACHINE\\SOFTWARE", NULL } },
        { L"\\SystemRoot\\System32\\config\\SAM", { L"MACHINE\\SAM", NULL } },
        { L"\\SystemRoot\\System32\\config\\SECURITY", { L"MACHINE\\SECURITY", NULL } },
        { L"\\SystemRoot\\System32\\config\\DEFAULT", { L"USER\\.DEFAULT", L"USER\\S-1-5-18" } },
    };
    ULONG i, j;

    for (i = 0; i < RTL_NUMBER_OF(Hives); i++)
    {
        PCMHIVE CmHive;
        NTSTATUS Status = QzCmLoadHiveFile(Hives[i].File, &CmHive);
        DbgPrint(QZ_KERNEL_NAME ": registry: %ls: 0x%08lx\n", Hives[i].File, Status);
        if (!NT_SUCCESS(Status))
            continue;
        ExAcquireFastMutex(&QzRegistryLock);
        for (j = 0; j < RTL_NUMBER_OF(Hives[i].Mounts) && Hives[i].Mounts[j]; j++)
        {
            PQZ_MEM_KEY Mount = QzRegistryRoot;
            PCWSTR Part = Hives[i].Mounts[j];
            while (Mount && *Part)
            {
                PCWSTR End = Part;
                while (*End && *End != L'\\')
                    End++;
                Mount = QzMemCreateKey(Mount, Part, (ULONG)(End - Part) * sizeof(WCHAR));
                Part = *End ? End + 1 : End;
            }
            if (Mount && !Mount->Child && !Mount->Values)
                Mount->MountedHive = CmHive;
        }
        ExReleaseFastMutex(&QzRegistryLock);
    }
}

/* smss's call, once (NT refuses a second one from user mode): the other hives come in now */
NTSTATUS
NTAPI
NtInitializeRegistry(IN USHORT Flag)
{
    static LONG Done;
    UNREFERENCED_PARAMETER(Flag);
    if (InterlockedExchange(&Done, 1))
        return ExGetPreviousMode() != KernelMode ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
    QzCmLoadSystemHives();
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* HARDWARE\DESCRIPTION from the loader's ARC configuration tree           */
/* ---------------------------------------------------------------------- */

static const PCWSTR QzArcTypeNames[] =
{
    L"System", L"CentralProcessor", L"FloatingPointProcessor", L"PrimaryIcache",
    L"PrimaryDcache", L"SecondaryIcache", L"SecondaryDcache", L"SecondaryCache",
    L"EisaAdapter", L"TcAdapter", L"ScsiAdapter", L"DtiAdapter", L"MultifunctionAdapter",
    L"DiskController", L"TapeController", L"CdRomController", L"WormController",
    L"SerialController", L"NetworkController", L"DisplayController", L"ParallelController",
    L"PointerController", L"KeyboardController", L"AudioController", L"OtherController",
    L"DiskPeripheral", L"FloppyDiskPeripheral", L"TapePeripheral", L"ModemPeripheral",
    L"MonitorPeripheral", L"PrinterPeripheral", L"PointerPeripheral", L"KeyboardPeripheral",
    L"TerminalPeripheral", L"OtherPeripheral", L"LinePeripheral", L"NetworkPeripheral",
    L"SystemMemory", L"DockingInformation", L"RealModeIrqRoutingTable", L"RealModePCIEnumeration",
};

static ULONG QzArcNodes;

static VOID
QzCmImportArcNode(PQZ_MEM_KEY Parent, PCONFIGURATION_COMPONENT_DATA Node,
                  INTERFACE_TYPE Interface, ULONG BusNumber)
{
    PCONFIGURATION_COMPONENT Entry = &Node->ComponentEntry;
    PQZ_MEM_KEY Key, TypeKey;
    PCONFIGURATION_COMPONENT_DATA Child;
    ULONG Size, Info[4], AdapterIndex[MaximumType];
    PCM_FULL_RESOURCE_DESCRIPTOR Full;
    WCHAR Text[128];

    /* The loader's root (FreeLoader gives it MaximumType) is "System" */
    BOOLEAN IsRoot = (Node->Parent == NULL);

    if ((!IsRoot && (ULONG)Entry->Type >= RTL_NUMBER_OF(QzArcTypeNames)) || QzArcNodes > 512)
        return;
    QzArcNodes++;

    if (IsRoot || Entry->Type == ArcSystem)
    {
        Key = QzMemCreatePath(Parent, L"System");
    }
    else
    {
        WCHAR Index[12];
        TypeKey = QzMemCreatePath(Parent, QzArcTypeNames[Entry->Type]);
        if (!TypeKey)
            return;
        _snwprintf(Index, RTL_NUMBER_OF(Index), L"%lu", TypeKey->SubKeys);
        Key = QzMemCreatePath(TypeKey, Index);
    }
    if (!Key)
        return;

    /* Component Information: flags, version, revision, key, affinity */
    Info[0] = *(PULONG)&Entry->Flags;
    Info[1] = Entry->Version | ((ULONG)Entry->Revision << 16);
    Info[2] = Entry->Key;
    Info[3] = Entry->AffinityMask;
    QzMemSetValueW(Key, L"Component Information", REG_BINARY, Info, sizeof(Info));

    if (Entry->Identifier && Entry->IdentifierLength)
    {
        ULONG i;
        for (i = 0; i < RTL_NUMBER_OF(Text) - 1 && i < Entry->IdentifierLength && Entry->Identifier[i]; i++)
            Text[i] = (UCHAR)Entry->Identifier[i];
        Text[i] = UNICODE_NULL;
        QzMemSetValueW(Key, L"Identifier", REG_SZ, Text, (i + 1) * sizeof(WCHAR));
    }

    /* Configuration Data: the component's partial list behind a full
       descriptor header naming the bus it sits on */
    Size = FIELD_OFFSET(CM_FULL_RESOURCE_DESCRIPTOR, PartialResourceList) +
           max(Entry->ConfigurationDataLength, FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors));
    Full = ExAllocatePoolWithTag(NonPagedPool, Size, QZ_CM_TAG);
    if (Full)
    {
        RtlZeroMemory(Full, Size);
        Full->InterfaceType = Interface;
        Full->BusNumber = BusNumber;
        if (Node->ConfigurationData && Entry->ConfigurationDataLength)
            RtlCopyMemory(&Full->PartialResourceList, Node->ConfigurationData,
                          Entry->ConfigurationDataLength);
        QzMemSetValueW(Key, L"Configuration Data", QZ_REG_FULL_RESOURCE_DESCRIPTOR, Full, Size);
        ExFreePoolWithTag(Full, QZ_CM_TAG);
    }

    /* Children sit on this adapter's bus */
    RtlZeroMemory(AdapterIndex, sizeof(AdapterIndex));
    for (Child = Node->Child; Child; Child = Child->Sibling)
    {
        INTERFACE_TYPE ChildInterface = Interface;
        ULONG ChildBus = BusNumber;
        PCONFIGURATION_COMPONENT ChildEntry = &Child->ComponentEntry;

        if (ChildEntry->Type == EisaAdapter)
        {
            ChildInterface = Eisa;
            ChildBus = AdapterIndex[EisaAdapter]++;
        }
        else if (ChildEntry->Type == MultiFunctionAdapter && ChildEntry->Identifier)
        {
            ChildInterface = (!strcmp(ChildEntry->Identifier, "PCI")) ? PCIBus :
                             (!strcmp(ChildEntry->Identifier, "ISA")) ? Isa : Internal;
            ChildBus = AdapterIndex[MultiFunctionAdapter]++;
        }
        QzCmImportArcNode(Key, Child, ChildInterface, ChildBus);
    }
}

static VOID
QzCpuid(ULONG Leaf, ULONG Regs[4])
{
    __asm__ __volatile__("cpuid" : "=a"(Regs[0]), "=b"(Regs[1]), "=c"(Regs[2]), "=d"(Regs[3]) : "a"(Leaf), "c"(0));
}

/*
 * The loader's tree has no processors on x86: NT describes them itself
 * from CPUID. CentralProcessor\0 and FloatingPointProcessor\0 (one
 * processor, as the scheduler has).
 */
static VOID
QzCmDescribeProcessors(PQZ_MEM_KEY Description)
{
    PQZ_MEM_KEY System = QzMemCreatePath(Description, L"System"), Cpu, Fpu;
    ULONG Regs[4], Family, Model, Stepping, MaxExtended, i, Mhz = 0, Feature = 0;
    CHAR Vendor[13], Brand[49];
    WCHAR Text[64];
    ULONG Info[4] = { 0, 0, 0, 1 };
    ULONGLONG Start, End;

    if (!System || QzMemCreatePath(System, L"CentralProcessor")->SubKeys)
        return;
    Cpu = QzMemCreatePath(QzMemCreatePath(System, L"CentralProcessor"), L"0");
    Fpu = QzMemCreatePath(QzMemCreatePath(System, L"FloatingPointProcessor"), L"0");
    if (!Cpu || !Fpu)
        return;

    QzCpuid(0, Regs);
    RtlCopyMemory(Vendor, &Regs[1], 4);
    RtlCopyMemory(Vendor + 4, &Regs[3], 4);
    RtlCopyMemory(Vendor + 8, &Regs[2], 4);
    Vendor[12] = 0;
    QzCpuid(1, Regs);
    Family = (Regs[0] >> 8) & 0xF;
    Model = (Regs[0] >> 4) & 0xF;
    Stepping = Regs[0] & 0xF;
    if (Family == 0xF)
        Family += (Regs[0] >> 20) & 0xFF;
    if (Family == 0x6 || Family >= 0xF)
        Model |= ((Regs[0] >> 16) & 0xF) << 4;
    if (Regs[3] & (1 << 24)) Feature |= KF_FXSR;
    if (Regs[3] & (1 << 25)) Feature |= KF_XMMI;
    if (Regs[3] & (1 << 26)) Feature |= KF_XMMI64;
    if (Regs[3] & (1 << 23)) Feature |= KF_MMX;
    if (Regs[3] & (1 << 15)) Feature |= KF_CMOV;
    if (Regs[3] & (1 << 4))  Feature |= KF_RDTSC;
    if (Regs[3] & (1 << 8))  Feature |= KF_CMPXCHG8B;

    Brand[0] = 0;
    QzCpuid(0x80000000, Regs);
    MaxExtended = Regs[0];
    if (MaxExtended >= 0x80000004)
    {
        for (i = 0; i < 3; i++)
        {
            QzCpuid(0x80000002 + i, Regs);
            RtlCopyMemory(Brand + i * 16, Regs, 16);
        }
        Brand[48] = 0;
    }

    /* The clock: time stamps over 10 ms */
    if (Feature & KF_RDTSC)
    {
        Start = __rdtsc();
        KeStallExecutionProcessor(10000);
        End = __rdtsc();
        Mhz = (ULONG)((End - Start + 5000) / 10000);
    }

    _snwprintf(Text, RTL_NUMBER_OF(Text), L"x86 Family %lu Model %lu Stepping %lu", Family, Model, Stepping);
    QzMemSetValueW(Cpu, L"Identifier", REG_SZ, Text, (ULONG)(wcslen(Text) + 1) * sizeof(WCHAR));
    QzMemSetValueW(Fpu, L"Identifier", REG_SZ, Text, (ULONG)(wcslen(Text) + 1) * sizeof(WCHAR));
    _snwprintf(Text, RTL_NUMBER_OF(Text), L"%hs", Vendor);
    QzMemSetValueW(Cpu, L"VendorIdentifier", REG_SZ, Text, (ULONG)(wcslen(Text) + 1) * sizeof(WCHAR));
    if (Brand[0])
    {
        PCSTR Name = Brand;
        while (*Name == ' ')
            Name++;
        _snwprintf(Text, RTL_NUMBER_OF(Text), L"%hs", Name);
        QzMemSetValueW(Cpu, L"ProcessorNameString", REG_SZ, Text, (ULONG)(wcslen(Text) + 1) * sizeof(WCHAR));
    }
    QzMemSetValueW(Cpu, L"FeatureSet", REG_DWORD, &Feature, sizeof(Feature));
    QzMemSetValueW(Cpu, L"~MHz", REG_DWORD, &Mhz, sizeof(Mhz));
    QzMemSetValueW(Cpu, L"Component Information", REG_BINARY, Info, sizeof(Info));
    QzMemSetValueW(Fpu, L"Component Information", REG_BINARY, Info, sizeof(Info));
}

static VOID
QzCmBuildVolatileTree(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PQZ_MEM_KEY Machine, Description;

    QzRegistryRoot = QzMemCreateKey(NULL, L"REGISTRY", 8 * sizeof(WCHAR));
    Machine = QzMemCreatePath(QzRegistryRoot, L"MACHINE");
    QzMemCreatePath(QzRegistryRoot, L"USER");
    QzHardwareKey = QzMemCreatePath(Machine, L"HARDWARE");
    QzMemCreatePath(Machine, L"SAM");
    QzMemCreatePath(Machine, L"SECURITY");
    QzMemCreatePath(Machine, L"SOFTWARE");
    QzMemCreatePath(Machine, L"SYSTEM")->MountedHive = QzSystemCmHive;

    Description = QzMemCreatePath(QzHardwareKey, L"DESCRIPTION");
    QzMemCreatePath(QzHardwareKey, L"DEVICEMAP");
    QzMemCreatePath(QzHardwareKey, L"RESOURCEMAP");
    QzMemCreatePath(QzHardwareKey, L"OWNERMAP");
    if (LoaderBlock->ConfigurationRoot)
    {
        DbgPrint(QZ_KERNEL_NAME ": registry: ARC tree at %p, root type %lu\n",
                 LoaderBlock->ConfigurationRoot, LoaderBlock->ConfigurationRoot->ComponentEntry.Type);
        QzCmImportArcNode(Description, LoaderBlock->ConfigurationRoot, Internal, 0);
    }
    QzCmDescribeProcessors(Description);
}

/* ---------------------------------------------------------------------- */
/* Rtl registry helpers                                                    */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzRtlOpenRegistryKey(ULONG RelativeTo, PCWSTR Path, BOOLEAN Create, PHANDLE Handle)
{
    static const PCWSTR Bases[] =
    {
        L"",
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\",
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\",
        L"\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion\\",
        L"\\Registry\\Machine\\Hardware\\DeviceMap\\",
        L"\\Registry\\User\\.Default\\",
    };
    WCHAR Buffer[256];
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    ULONG Base = RelativeTo & 0xFFFF;

    if (RelativeTo & RTL_REGISTRY_HANDLE)
    {
        *Handle = (HANDLE)Path;
        return STATUS_SUCCESS;
    }
    if (Base >= RTL_NUMBER_OF(Bases))
        return STATUS_INVALID_PARAMETER;

    Name.Buffer = Buffer;
    Name.Length = 0;
    Name.MaximumLength = sizeof(Buffer);
    RtlAppendUnicodeToString(&Name, Bases[Base]);
    if (Path)
        RtlAppendUnicodeToString(&Name, Path);
    while (Name.Length && Name.Buffer[Name.Length / sizeof(WCHAR) - 1] == L'\\')
        Name.Length -= sizeof(WCHAR);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (Create)
        return ZwCreateKey(Handle, KEY_ALL_ACCESS, &Attributes, 0, NULL, 0, NULL);
    return ZwOpenKey(Handle, KEY_READ, &Attributes);
}

static NTSTATUS
QzRtlCallQueryRoutine(PRTL_QUERY_REGISTRY_TABLE Entry, PWSTR Name, ULONG Type, PVOID Data,
                      ULONG Length, PVOID Context)
{
    NTSTATUS Status;

    if (Entry->Flags & RTL_QUERY_REGISTRY_DIRECT)
    {
        if (Type == REG_SZ || Type == REG_EXPAND_SZ || Type == REG_MULTI_SZ)
        {
            PUNICODE_STRING Out = Entry->EntryContext;
            ULONG Bytes = Length;
            if (Type != REG_MULTI_SZ)
                while (Bytes >= sizeof(WCHAR) && !((PWCHAR)Data)[Bytes / sizeof(WCHAR) - 1])
                    Bytes -= sizeof(WCHAR);
            if (!Out->Buffer)
            {
                Out->Buffer = ExAllocatePoolWithTag(PagedPool, Bytes + sizeof(WCHAR), 'rtSQ');
                if (!Out->Buffer)
                    return STATUS_NO_MEMORY;
                Out->MaximumLength = (USHORT)(Bytes + sizeof(WCHAR));
            }
            else if (Out->MaximumLength < Bytes)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }
            RtlCopyMemory(Out->Buffer, Data, Bytes);
            Out->Length = (USHORT)Bytes;
            if (Bytes + sizeof(WCHAR) <= Out->MaximumLength)
                Out->Buffer[Bytes / sizeof(WCHAR)] = UNICODE_NULL;
        }
        else if (Length <= sizeof(ULONG))
        {
            RtlCopyMemory(Entry->EntryContext, Data, Length);
        }
        else
        {
            /* Larger data: the first LONG holds -(buffer size) */
            PLONG Out = Entry->EntryContext;
            if (*Out < 0 && (ULONG)-*Out >= Length)
                RtlCopyMemory(Out, Data, Length);
            else
                return STATUS_BUFFER_TOO_SMALL;
        }
        return STATUS_SUCCESS;
    }

    /* Routine: REG_MULTI_SZ is handed over one string at a time */
    if (Type == REG_MULTI_SZ && !(Entry->Flags & RTL_QUERY_REGISTRY_NOEXPAND))
    {
        PWCHAR String = Data, End = (PWCHAR)((PUCHAR)Data + Length);
        while (String < End && *String)
        {
            ULONG Bytes = (ULONG)(wcslen(String) + 1) * sizeof(WCHAR);
            Status = Entry->QueryRoutine(Name, REG_SZ, String, Bytes, Context, Entry->EntryContext);
            if (!NT_SUCCESS(Status))
                return Status;
            String += Bytes / sizeof(WCHAR);
        }
        return STATUS_SUCCESS;
    }
    return Entry->QueryRoutine(Name, Type == REG_EXPAND_SZ && !(Entry->Flags & RTL_QUERY_REGISTRY_NOEXPAND)
                                         ? REG_SZ : Type,
                               Data, Length, Context, Entry->EntryContext);
}

NTSTATUS
NTAPI
RtlQueryRegistryValues(IN ULONG RelativeTo, IN PCWSTR Path,
                       IN PRTL_QUERY_REGISTRY_TABLE QueryTable, IN PVOID Context,
                       IN PVOID Environment OPTIONAL)
{
    HANDLE Top, Current, Sub = NULL;
    PRTL_QUERY_REGISTRY_TABLE Entry;
    PKEY_VALUE_FULL_INFORMATION Info;
    ULONG InfoSize = 1024, Result;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Environment);

    Status = QzRtlOpenRegistryKey(RelativeTo, Path, FALSE, &Top);
    if (!NT_SUCCESS(Status))
        return Status;
    Current = Top;
    Info = ExAllocatePoolWithTag(PagedPool, InfoSize, 'rtSQ');
    if (!Info)
        Status = STATUS_NO_MEMORY;

    for (Entry = QueryTable; NT_SUCCESS(Status) && (Entry->QueryRoutine || Entry->Name); Entry++)
    {
        if (Entry->Flags & (RTL_QUERY_REGISTRY_TOPKEY | RTL_QUERY_REGISTRY_SUBKEY))
        {
            if (Sub)
            {
                ZwClose(Sub);
                Sub = NULL;
            }
            Current = Top;
        }
        if (Entry->Flags & RTL_QUERY_REGISTRY_SUBKEY)
        {
            UNICODE_STRING Name;
            OBJECT_ATTRIBUTES Attributes;
            RtlInitUnicodeString(&Name, Entry->Name);
            InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                       Top, NULL);
            Status = ZwOpenKey(&Sub, KEY_READ, &Attributes);
            if (!NT_SUCCESS(Status))
            {
                Sub = NULL;
                break;
            }
            Current = Sub;
            if (!Entry->QueryRoutine)
                continue;
        }

        if (Entry->Name && !(Entry->Flags & RTL_QUERY_REGISTRY_SUBKEY))
        {
            UNICODE_STRING Name;
            RtlInitUnicodeString(&Name, Entry->Name);
            Status = ZwQueryValueKey(Current, &Name, KeyValueFullInformation, Info, InfoSize, &Result);
            if (NT_SUCCESS(Status))
            {
                Status = QzRtlCallQueryRoutine(Entry, (PWSTR)Entry->Name, Info->Type,
                                               (PUCHAR)Info + Info->DataOffset, Info->DataLength,
                                               Context);
            }
            else if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
            {
                if ((Entry->DefaultType & 0xFFFFFF) != REG_NONE)
                {
                    ULONG DefaultLength = Entry->DefaultLength;
                    if (!DefaultLength && (Entry->DefaultType == REG_SZ ||
                                           Entry->DefaultType == REG_EXPAND_SZ))
                        DefaultLength = Entry->DefaultData
                                        ? (ULONG)(wcslen(Entry->DefaultData) + 1) * sizeof(WCHAR) : 0;
                    Status = QzRtlCallQueryRoutine(Entry, (PWSTR)Entry->Name, Entry->DefaultType & 0xFFFFFF,
                                                   Entry->DefaultData, DefaultLength, Context);
                }
                else if (!(Entry->Flags & RTL_QUERY_REGISTRY_REQUIRED))
                {
                    Status = STATUS_SUCCESS;
                }
            }
        }
        else if (Entry->QueryRoutine)
        {
            /* All values of the current key */
            ULONG Index;
            for (Index = 0; NT_SUCCESS(Status); Index++)
            {
                Status = ZwEnumerateValueKey(Current, Index, KeyValueFullInformation, Info, InfoSize,
                                             &Result);
                if (Status == STATUS_NO_MORE_ENTRIES)
                {
                    Status = STATUS_SUCCESS;
                    break;
                }
                if (NT_SUCCESS(Status))
                {
                    WCHAR Name[64];
                    ULONG Chars = min(Info->NameLength / sizeof(WCHAR), RTL_NUMBER_OF(Name) - 1);
                    RtlCopyMemory(Name, Info->Name, Chars * sizeof(WCHAR));
                    Name[Chars] = UNICODE_NULL;
                    Status = QzRtlCallQueryRoutine(Entry, Name, Info->Type,
                                                   (PUCHAR)Info + Info->DataOffset,
                                                   Info->DataLength, Context);
                }
            }
        }
    }

    if (Info)
        ExFreePoolWithTag(Info, 'rtSQ');
    if (Sub)
        ZwClose(Sub);
    if (!(RelativeTo & RTL_REGISTRY_HANDLE))
        ZwClose(Top);
    return Status;
}

NTSTATUS
NTAPI
RtlWriteRegistryValue(IN ULONG RelativeTo, IN PCWSTR Path, IN PCWSTR ValueName,
                      IN ULONG ValueType, IN PVOID ValueData, IN ULONG ValueLength)
{
    UNICODE_STRING Name;
    HANDLE Key;
    NTSTATUS Status = QzRtlOpenRegistryKey(RelativeTo, Path, TRUE, &Key);

    if (!NT_SUCCESS(Status))
        return Status;
    RtlInitUnicodeString(&Name, ValueName);
    Status = ZwSetValueKey(Key, &Name, 0, ValueType, ValueData, ValueLength);
    if (!(RelativeTo & RTL_REGISTRY_HANDLE))
        ZwClose(Key);
    return Status;
}

NTSTATUS
NTAPI
RtlDeleteRegistryValue(IN ULONG RelativeTo, IN PCWSTR Path, IN PCWSTR ValueName)
{
    UNICODE_STRING Name;
    HANDLE Key;
    NTSTATUS Status = QzRtlOpenRegistryKey(RelativeTo, Path, FALSE, &Key);

    if (!NT_SUCCESS(Status))
        return Status;
    RtlInitUnicodeString(&Name, ValueName);
    Status = ZwDeleteValueKey(Key, &Name);
    if (!(RelativeTo & RTL_REGISTRY_HANDLE))
        ZwClose(Key);
    return Status;
}

NTSTATUS
NTAPI
RtlCheckRegistryKey(IN ULONG RelativeTo, IN PWSTR Path)
{
    HANDLE Handle;
    NTSTATUS Status = QzRtlOpenRegistryKey(RelativeTo, Path, FALSE, &Handle);
    if (NT_SUCCESS(Status) && !(RelativeTo & RTL_REGISTRY_HANDLE))
        ZwClose(Handle);
    return Status;
}

NTSTATUS
NTAPI
RtlCreateRegistryKey(IN ULONG RelativeTo, IN PWSTR Path)
{
    HANDLE Handle;
    NTSTATUS Status = QzRtlOpenRegistryKey(RelativeTo, Path, TRUE, &Handle);
    if (NT_SUCCESS(Status) && !(RelativeTo & RTL_REGISTRY_HANDLE))
        ZwClose(Handle);
    return Status;
}

/* ---------------------------------------------------------------------- */
/* Internal reads for the PnP manager                                      */
/* ---------------------------------------------------------------------- */

/* Read a value under an absolute path into Buffer; returns its type */
NTSTATUS
QzCmReadValue(PCWSTR KeyPath, PCWSTR ValueName, PVOID Buffer, ULONG Length, PULONG Type,
              PULONG DataLength)
{
    UNICODE_STRING Path, Name;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Key;
    UCHAR Small[256];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Small;
    ULONG InfoSize = sizeof(Small), Result;
    NTSTATUS Status;

    RtlInitUnicodeString(&Path, KeyPath);
    RtlInitUnicodeString(&Name, ValueName);
    InitializeObjectAttributes(&Attributes, &Path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenKey(&Key, KEY_READ, &Attributes);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Length + FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) > sizeof(Small))
    {
        InfoSize = Length + FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data);
        Info = ExAllocatePoolWithTag(PagedPool, InfoSize, QZ_CM_TAG);
        if (!Info)
        {
            ZwClose(Key);
            return STATUS_NO_MEMORY;
        }
    }
    Status = ZwQueryValueKey(Key, &Name, KeyValuePartialInformation, Info, InfoSize, &Result);
    if (NT_SUCCESS(Status))
    {
        if (Info->DataLength > Length)
        {
            Status = STATUS_BUFFER_OVERFLOW;
        }
        else
        {
            RtlCopyMemory(Buffer, Info->Data, Info->DataLength);
            if (Type)
                *Type = Info->Type;
            if (DataLength)
                *DataLength = Info->DataLength;
        }
    }
    if (Info != (PKEY_VALUE_PARTIAL_INFORMATION)Small)
        ExFreePoolWithTag(Info, QZ_CM_TAG);
    ZwClose(Key);
    return Status;
}

VOID
QzCmInit(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    UNICODE_STRING TypeName = RTL_CONSTANT_STRING(L"Key"), RootName = RTL_CONSTANT_STRING(L"\\REGISTRY");
    OBJECT_TYPE_INITIALIZER Init;
    OBJECT_ATTRIBUTES Attributes;
    PQZ_KEY Root;
    NTSTATUS Status;

    ExInitializeFastMutex(&QzRegistryLock);

    RtlZeroMemory(&Init, sizeof(Init));
    Init.Length = sizeof(Init);
    Init.GenericMapping.GenericRead = KEY_READ;
    Init.GenericMapping.GenericWrite = KEY_WRITE;
    Init.GenericMapping.GenericExecute = KEY_EXECUTE;
    Init.GenericMapping.GenericAll = KEY_ALL_ACCESS;
    Init.ValidAccessMask = KEY_ALL_ACCESS | ACCESS_SYSTEM_SECURITY;
    Init.PoolType = NonPagedPool;
    Init.UseDefaultObject = TRUE;
    Init.CaseInsensitive = TRUE;
    Init.ParseProcedure = QzCmParseKey;
    Init.CloseProcedure = QzCmCloseKey;
    Init.DeleteProcedure = QzCmDeleteKey;
    Status = ObCreateObjectType(&TypeName, &Init, NULL, &CmKeyObjectType);
    if (!NT_SUCCESS(Status))
        KeBugCheckEx(CONFIG_INITIALIZATION_FAILED, 1, Status, 0, 0);

    /* The loader's SYSTEM hive, read in place */
    if (LoaderBlock->RegistryBase && LoaderBlock->RegistryLength)
    {
        /* Copied into bins of our own so it can be written */
        QzSystemCmHive = CmpAllocate(sizeof(CMHIVE), FALSE, QZ_CM_TAG);
        if (QzSystemCmHive)
        {
            RtlZeroMemory(QzSystemCmHive, sizeof(CMHIVE));
            QzCurHive = &QzSystemCmHive->Hive;
            Status = HvInitialize(QzHive, HINIT_MEMORY, HIVE_NOLAZYFLUSH, HFILE_TYPE_PRIMARY,
                                  LoaderBlock->RegistryBase, CmpAllocate, CmpFree,
                                  NULL, NULL, NULL, NULL, 1, NULL);
            QzHiveReady = NT_SUCCESS(Status);
        }
        if (QzHiveReady)
        {
            QzControlSetCell = QzCmFindControlSet();
            QzCmCreateCurrentControlSetLink();
        }
    }

    /* The volatile keys, HARDWARE\DESCRIPTION from the loader's tree */
    QzCmBuildVolatileTree(LoaderBlock);
    DbgPrint(QZ_KERNEL_NAME ": registry: SYSTEM hive %s (%lu KB), %lu hardware description node(s)\n",
             QzHiveReady ? "mounted" : "unreadable", LoaderBlock->RegistryLength / 1024, QzArcNodes);

    /* \REGISTRY */
    InitializeObjectAttributes(&Attributes, &RootName, OBJ_PERMANENT | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ObCreateObject(KernelMode, CmKeyObjectType, &Attributes, KernelMode, NULL,
                            sizeof(QZ_KEY), 0, 0, (PVOID *)&Root);
    if (NT_SUCCESS(Status))
    {
        Root->IsCell = FALSE;
        Root->Cell = HCELL_NIL;
        Root->Mem = QzRegistryRoot;
        Status = ObInsertObject(Root, NULL, KEY_READ, 0, NULL, NULL);
    }
    if (!NT_SUCCESS(Status) || !QzRegistryRoot)
        KeBugCheckEx(CONFIG_INITIALIZATION_FAILED, 2, Status, 0, 0);
}

BOOLEAN
QzCmHiveReady(VOID)
{
    return QzHiveReady;
}

/* The shell's "reg" command */
VOID
QzCmDumpKey(PCWSTR Path)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Key;
    UCHAR Buffer[512];
    ULONG Index, Result, Keys = 0, Values = 0;
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenKey(&Key, KEY_READ, &Attributes);
    if (!NT_SUCCESS(Status))
    {
        QzPrint("  reg: cannot open %wZ (0x%08lx)\n", &Name, Status);
        return;
    }
    QzPrint("  Key %wZ\n", &Name);
    for (Index = 0; Keys < 40; Index++)
    {
        PKEY_BASIC_INFORMATION Info = (PKEY_BASIC_INFORMATION)Buffer;
        UNICODE_STRING Sub;
        if (!NT_SUCCESS(ZwEnumerateKey(Key, Index, KeyBasicInformation, Info, sizeof(Buffer), &Result)))
            break;
        Sub.Buffer = Info->Name;
        Sub.Length = Sub.MaximumLength = (USHORT)Info->NameLength;
        QzPrint("    [%wZ]\n", &Sub);
        Keys++;
    }
    for (Index = 0; Values < 40; Index++)
    {
        PKEY_VALUE_FULL_INFORMATION Info = (PKEY_VALUE_FULL_INFORMATION)Buffer;
        UNICODE_STRING ValueName;
        PUCHAR Data;
        if (!NT_SUCCESS(ZwEnumerateValueKey(Key, Index, KeyValueFullInformation, Info,
                                            sizeof(Buffer) - sizeof(WCHAR), &Result)))
            break;
        ValueName.Buffer = Info->Name;
        ValueName.Length = ValueName.MaximumLength = (USHORT)Info->NameLength;
        Data = (PUCHAR)Info + Info->DataOffset;
        if (Info->Type == REG_SZ || Info->Type == REG_EXPAND_SZ || Info->Type == REG_MULTI_SZ)
        {
            UNICODE_STRING Text;
            ULONG i;
            Text.Buffer = (PWCHAR)Data;
            Text.Length = Text.MaximumLength = (USHORT)Info->DataLength;
            while (Text.Length && !Text.Buffer[Text.Length / sizeof(WCHAR) - 1])
                Text.Length -= sizeof(WCHAR);
            /* MULTI_SZ: show the strings separated by "|" */
            for (i = 0; i < Text.Length / sizeof(WCHAR); i++)
                if (!Text.Buffer[i])
                    Text.Buffer[i] = L'|';
            QzPrint("    %-24wZ %s %wZ\n", &ValueName,
                    Info->Type == REG_MULTI_SZ ? "MULTI_SZ " : Info->Type == REG_EXPAND_SZ ? "EXPAND_SZ" : "REG_SZ   ",
                    &Text);
        }
        else if (Info->Type == REG_DWORD && Info->DataLength == 4)
        {
            QzPrint("    %-24wZ REG_DWORD 0x%08lx\n", &ValueName, *(PULONG)Data);
        }
        else
        {
            QzPrint("    %-24wZ type %lu, %lu bytes\n", &ValueName, Info->Type, Info->DataLength);
        }
        Values++;
    }
    QzPrint("  %lu subkey(s), %lu value(s)\n", Keys, Values);
    ZwClose(Key);
}
