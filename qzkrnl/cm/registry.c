/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Registry: the loader's SYSTEM hive (read-only) and the
 *              volatile in-memory keys around it
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * The configuration manager proper is stage B5. Stage B4 needs a registry
 * (the PnP manager matches devices to drivers through
 * CriticalDeviceDatabase, drivers and the HAL read their keys), so:
 *   \REGISTRY\MACHINE\SYSTEM     the SYSTEM hive FreeLoader loaded, read in
 *                                place with ReactOS's hive library (cmlib);
 *                                CurrentControlSet resolves through
 *                                Select\Current. Writes are dropped.
 *   \REGISTRY\MACHINE\HARDWARE   volatile and writable, as on NT; its
 *                                DESCRIPTION subtree is built from the
 *                                loader's ARC configuration tree (this is
 *                                where the HAL finds the PCI BIOS).
 *   everything else              volatile in-memory keys.
 * A key created below the SYSTEM hive cannot be stored there yet; it comes
 * back as a detached in-memory key, so drivers recording state keep
 * working until B5 makes the hive writable.
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
    BOOLEAN MountsSystemHive;          /* \REGISTRY\MACHINE\SYSTEM */
    LARGE_INTEGER LastWriteTime;
    USHORT NameLength;                 /* bytes */
    WCHAR Name[1];
} QZ_MEM_KEY, *PQZ_MEM_KEY;

/* A key object: a cell of the SYSTEM hive or an in-memory key */
typedef struct _QZ_KEY
{
    BOOLEAN IsCell;
    HCELL_INDEX Cell;
    PQZ_MEM_KEY Mem;
} QZ_KEY, *PQZ_KEY;

typedef struct _QZ_KEY_PARSE
{
    CSHORT Type;                       /* 'K' */
    BOOLEAN Create;
    ULONG Disposition;
} QZ_KEY_PARSE, *PQZ_KEY_PARSE;

POBJECT_TYPE CmKeyObjectType;
static HHIVE QzSystemHive;
static BOOLEAN QzHiveReady;
static HCELL_INDEX QzControlSetCell = HCELL_NIL;
static ULONG QzRegistryWritesDropped;
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

ULONG
QzCmDroppedWrites(VOID)
{
    return QzRegistryWritesDropped;
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
    return (PCM_KEY_NODE)HvGetCell(&QzSystemHive, Cell);
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
    HCELL_INDEX Cell = CmpFindValueByName(&QzSystemHive, Node, (PUNICODE_STRING)Name);
    return (Cell == HCELL_NIL) ? NULL : (PCM_KEY_VALUE)HvGetCell(&QzSystemHive, Cell);
}

static PCM_KEY_VALUE
QzCmValueByIndex(HCELL_INDEX KeyCell, ULONG Index)
{
    PCM_KEY_NODE Node = QzCmNode(KeyCell);
    PHCELL_INDEX List;

    if (Index >= Node->ValueList.Count)
        return NULL;
    List = (PHCELL_INDEX)HvGetCell(&QzSystemHive, Node->ValueList.List);
    return (PCM_KEY_VALUE)HvGetCell(&QzSystemHive, List[Index]);
}

static PVOID
QzCmValueData(PCM_KEY_VALUE Value, PULONG Length)
{
    return CmpValueToData(&QzSystemHive, Value, Length);
}

static HCELL_INDEX
QzCmSubKey(HCELL_INDEX Parent, PCUNICODE_STRING Name)
{
    return CmpFindSubKeyByName(&QzSystemHive, QzCmNode(Parent), (PUNICODE_STRING)Name);
}

/* ControlSetNNN named by Select\Current */
static HCELL_INDEX
QzCmFindControlSet(VOID)
{
    UNICODE_STRING Select = RTL_CONSTANT_STRING(L"Select"), Current = RTL_CONSTANT_STRING(L"Current");
    HCELL_INDEX SelectCell = QzCmSubKey(QzSystemHive.BaseBlock->RootCell, &Select);
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
    return QzCmSubKey(QzSystemHive.BaseBlock->RootCell, &Name);
}

/* ---------------------------------------------------------------------- */
/* Key objects and the parse procedure                                     */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzCmCreateKeyObject(BOOLEAN IsCell, HCELL_INDEX Cell, PQZ_MEM_KEY Mem, PQZ_KEY *Result)
{
    PQZ_KEY Key;
    NTSTATUS Status = ObCreateObject(KernelMode, CmKeyObjectType, NULL, KernelMode, NULL,
                                     sizeof(QZ_KEY), 0, 0, (PVOID *)&Key);
    if (!NT_SUCCESS(Status))
        return Status;
    Key->IsCell = IsCell;
    Key->Cell = Cell;
    Key->Mem = Mem;
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
    return QzCmSubKey(QzSystemHive.BaseBlock->RootCell, &Inner);
}

static NTSTATUS NTAPI
QzCmParseKey(PVOID ParseObject, PVOID ObjectType, PACCESS_STATE AccessState,
             KPROCESSOR_MODE AccessMode, ULONG Attributes, PUNICODE_STRING CompleteName,
             PUNICODE_STRING RemainingName, PVOID Context, PSECURITY_QUALITY_OF_SERVICE SecurityQos,
             PVOID *Object)
{
    PQZ_KEY_PARSE Parse = Context;
    PQZ_KEY Start = ParseObject;
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
    Parse->Disposition = REG_OPENED_EXISTING_KEY;

    ExAcquireFastMutex(&QzRegistryLock);
    for (;;)
    {
        /* Entering \REGISTRY\MACHINE\SYSTEM switches to the hive */
        if (!IsCell && Mem && Mem->MountsSystemHive && QzHiveReady)
        {
            IsCell = TRUE;
            Cell = QzSystemHive.BaseBlock->RootCell;
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
            if (Next == HCELL_NIL && Cell == QzSystemHive.BaseBlock->RootCell &&
                RtlEqualUnicodeString(&Component, &Ccs, TRUE))
                Next = QzControlSetCell;
            if (Next != HCELL_NIL && (QzCmNode(Next)->Flags & KEY_SYM_LINK))
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
            /* The hive cannot take new keys yet: detached from here on */
            IsCell = FALSE;
            Mem = QzMemCreateKey(NULL, Component.Buffer, Component.Length);
            Parse->Disposition = REG_CREATED_NEW_KEY;
            if (!Mem)
            {
                ExReleaseFastMutex(&QzRegistryLock);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
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

    Status = QzCmCreateKeyObject(IsCell, Cell, IsCell ? NULL : Mem, (PQZ_KEY *)Object);
    return Status;
}

/* ---------------------------------------------------------------------- */
/* Key services                                                            */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzCmOpen(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
         BOOLEAN Create, PULONG Disposition)
{
    QZ_KEY_PARSE Parse;
    OBJECT_ATTRIBUTES Attributes = *ObjectAttributes;
    NTSTATUS Status;

    Parse.Type = 'K';
    Parse.Create = Create;
    Parse.Disposition = REG_OPENED_EXISTING_KEY;
    Attributes.Attributes |= OBJ_CASE_INSENSITIVE;

    /* An empty name relative to a key reopens that key */
    if (Attributes.RootDirectory && (!Attributes.ObjectName || !Attributes.ObjectName->Length))
    {
        PQZ_KEY Key, Copy;
        Status = ObReferenceObjectByHandle(Attributes.RootDirectory, 0, CmKeyObjectType, KernelMode,
                                           (PVOID *)&Key, NULL);
        if (!NT_SUCCESS(Status))
            return Status;
        Status = QzCmCreateKeyObject(Key->IsCell, Key->Cell, Key->Mem, &Copy);
        if (NT_SUCCESS(Status))
        {
            Status = ObOpenObjectByPointer(Copy, OBJ_KERNEL_HANDLE, NULL, DesiredAccess,
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
    return QzCmOpen(KeyHandle, DesiredAccess, ObjectAttributes, FALSE, NULL);
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
    UNREFERENCED_PARAMETER(CreateOptions);
    return QzCmOpen(KeyHandle, DesiredAccess, ObjectAttributes, TRUE, Disposition);
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
    return ObReferenceObjectByHandle(KeyHandle, 0, CmKeyObjectType, KernelMode, (PVOID *)Key, NULL);
}

/* Everything the KEY_*_INFORMATION classes report */
typedef struct _QZ_KEY_FACTS
{
    WCHAR Name[256];
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
    QzCmGetFacts(Key->IsCell, Key->Cell, Key->Mem, &Facts);
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
    if (Key->IsCell)
    {
        HCELL_INDEX Child = CmpFindSubKeyByNumber(&QzSystemHive, QzCmNode(Key->Cell), Index);
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
    if (Key->IsCell)
    {
        /* The SYSTEM hive becomes writable with stage B5 */
        QzRegistryWritesDropped++;
        Status = STATUS_SUCCESS;
    }
    else
    {
        ExAcquireFastMutex(&QzRegistryLock);
        Status = QzMemSetValue(Key->Mem, ValueName ? ValueName : &Empty, Type, Data, DataSize);
        ExReleaseFastMutex(&QzRegistryLock);
    }
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
        QzRegistryWritesDropped++;
    }
    else
    {
        PQZ_MEM_VALUE *Link, Value;
        Status = STATUS_OBJECT_NAME_NOT_FOUND;
        ExAcquireFastMutex(&QzRegistryLock);
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
        QzRegistryWritesDropped++;
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
        for (Link = &Key->Mem->Parent->Child; *Link; Link = &(*Link)->Sibling)
        {
            if (*Link == Key->Mem)
            {
                *Link = Key->Mem->Sibling;
                Key->Mem->Parent->SubKeys--;
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
ZwFlushKey(IN HANDLE KeyHandle)
{
    UNREFERENCED_PARAMETER(KeyHandle);
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
    QzMemCreatePath(Machine, L"SYSTEM")->MountsSystemHive = TRUE;

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
    UNREFERENCED_PARAMETER(RelativeTo);
    UNREFERENCED_PARAMETER(Path);
    UNREFERENCED_PARAMETER(ValueName);
    UNREFERENCED_PARAMETER(ValueType);
    UNREFERENCED_PARAMETER(ValueData);
    UNREFERENCED_PARAMETER(ValueLength);
    QzRegistryWritesDropped++;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlDeleteRegistryValue(IN ULONG RelativeTo, IN PCWSTR Path, IN PCWSTR ValueName)
{
    UNREFERENCED_PARAMETER(RelativeTo);
    UNREFERENCED_PARAMETER(Path);
    UNREFERENCED_PARAMETER(ValueName);
    QzRegistryWritesDropped++;
    return STATUS_SUCCESS;
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
    Status = ObCreateObjectType(&TypeName, &Init, NULL, &CmKeyObjectType);
    if (!NT_SUCCESS(Status))
        KeBugCheckEx(CONFIG_INITIALIZATION_FAILED, 1, Status, 0, 0);

    /* The loader's SYSTEM hive, read in place */
    if (LoaderBlock->RegistryBase && LoaderBlock->RegistryLength)
    {
        Status = HvInitialize(&QzSystemHive, HINIT_FLAT, 0, HFILE_TYPE_PRIMARY,
                              LoaderBlock->RegistryBase, CmpAllocate, CmpFree,
                              NULL, NULL, NULL, NULL, 1, NULL);
        QzHiveReady = NT_SUCCESS(Status);
        if (QzHiveReady)
            QzControlSetCell = QzCmFindControlSet();
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
            Text.Buffer = (PWCHAR)Data;
            Text.Length = Text.MaximumLength = (USHORT)Info->DataLength;
            while (Text.Length && !Text.Buffer[Text.Length / sizeof(WCHAR) - 1])
                Text.Length -= sizeof(WCHAR);
            QzPrint("    %-24wZ REG_SZ    %wZ\n", &ValueName, &Text);
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
