/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The loader block and its configuration tree for drivers,
 *              random numbers, NMI callbacks, WMI data blocks
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qz.h"

/* What NT exports as KeLoaderBlock (ramdisk and acpi read it at DriverEntry) */
PLOADER_PARAMETER_BLOCK KeLoaderBlock;

VOID
QzKeSetLoaderBlock(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    KeLoaderBlock = LoaderBlock;
}

/*
 * The next node of the ARC configuration tree, depth first from Child,
 * after *NextLink if that is set (it is the last one returned).
 */
PCONFIGURATION_COMPONENT_DATA
NTAPI
KeFindConfigurationNextEntry(IN PCONFIGURATION_COMPONENT_DATA Child, IN CONFIGURATION_CLASS Class,
                             IN CONFIGURATION_TYPE Type, IN PULONG ComponentKey OPTIONAL,
                             IN PCONFIGURATION_COMPONENT_DATA *NextLink)
{
    PCONFIGURATION_COMPONENT_DATA Entry = Child, Start = *NextLink;
    BOOLEAN Searching = Start == NULL;
    ULONG Mask = ComponentKey ? *ComponentKey : 0;

    while (Entry)
    {
        if (Searching)
        {
            if (Entry->ComponentEntry.Class == Class && Entry->ComponentEntry.Type == Type &&
                (!ComponentKey || (Entry->ComponentEntry.Key & Mask) == Mask))
            {
                *NextLink = Entry;
                return Entry;
            }
        }
        else if (Entry == Start)
        {
            Searching = TRUE;
        }
        /* Depth first: children, then siblings, then back up */
        if (Entry->Child)
        {
            Entry = Entry->Child;
            continue;
        }
        while (Entry && !Entry->Sibling)
        {
            Entry = Entry->Parent;
            if (Entry == Child || (Child && Entry == Child->Parent))
                return NULL;
        }
        if (Entry)
            Entry = Entry->Sibling;
    }
    return NULL;
}

PCONFIGURATION_COMPONENT_DATA
NTAPI
KeFindConfigurationEntry(IN PCONFIGURATION_COMPONENT_DATA Child, IN CONFIGURATION_CLASS Class,
                         IN CONFIGURATION_TYPE Type, IN PULONG ComponentKey OPTIONAL)
{
    PCONFIGURATION_COMPONENT_DATA Next = NULL;
    return KeFindConfigurationNextEntry(Child, Class, Type, ComponentKey, &Next);
}

/* ---- random numbers (ksecdd) ---- */

/* A multiplicative congruential generator with a shuffle table, as NT's RtlRandomEx describes */
static ULONG QzRandomTable[128];
static ULONG QzRandomLast;
static BOOLEAN QzRandomReady;

static ULONG
QzRandomStep(PULONG Seed)
{
    *Seed = (*Seed * 0x7FFFFFED + 0x7FFFFFC3) % 0x7FFFFFFF;
    return *Seed;
}

ULONG
NTAPI
RtlRandomEx(IN OUT PULONG Seed)
{
    ULONG Index, Result;

    if (!QzRandomReady)
    {
        ULONG Init = 0x12345678;
        for (Index = 0; Index < RTL_NUMBER_OF(QzRandomTable); Index++)
            QzRandomTable[Index] = QzRandomStep(&Init);
        QzRandomLast = QzRandomStep(&Init);
        QzRandomReady = TRUE;
    }
    Index = QzRandomLast & (RTL_NUMBER_OF(QzRandomTable) - 1);
    Result = QzRandomTable[Index];
    QzRandomLast = Result;
    QzRandomTable[Index] = QzRandomStep(Seed);
    return Result & 0x7FFFFFFF;
}

/* ---- NMI callbacks: kept; there is no NMI handling to call them yet ---- */

typedef struct _QZ_NMI_CALLBACK
{
    struct _QZ_NMI_CALLBACK *Next;
    PNMI_CALLBACK Callback;
    PVOID Context;
} QZ_NMI_CALLBACK, *PQZ_NMI_CALLBACK;

static PQZ_NMI_CALLBACK QzNmiCallbacks;

PVOID
NTAPI
KeRegisterNmiCallback(IN PNMI_CALLBACK CallbackRoutine, IN PVOID Context OPTIONAL)
{
    PQZ_NMI_CALLBACK Entry = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Entry), 'imNQ');
    if (!Entry)
        return NULL;
    Entry->Callback = CallbackRoutine;
    Entry->Context = Context;
    Entry->Next = InterlockedExchangePointer((PVOID *)&QzNmiCallbacks, NULL);
    InterlockedExchangePointer((PVOID *)&QzNmiCallbacks, Entry);
    return Entry;
}

NTSTATUS
NTAPI
KeDeregisterNmiCallback(IN PVOID Handle)
{
    PQZ_NMI_CALLBACK *Link;
    KIRQL OldIrql = QzRaiseIrql(HIGH_LEVEL);

    for (Link = &QzNmiCallbacks; *Link; Link = &(*Link)->Next)
    {
        if (*Link == Handle)
        {
            *Link = ((PQZ_NMI_CALLBACK)Handle)->Next;
            QzLowerIrql(OldIrql);
            ExFreePoolWithTag(Handle, 'imNQ');
            return STATUS_SUCCESS;
        }
    }
    QzLowerIrql(OldIrql);
    return STATUS_INVALID_HANDLE;
}

/* ---- WMI data blocks (i8042prt asks for the port's configuration): none registered ---- */

NTSTATUS
NTAPI
IoWMIOpenBlock(IN LPCGUID DataBlockGuid, IN ULONG DesiredAccess, OUT PVOID *DataBlockObject)
{
    UNREFERENCED_PARAMETER(DataBlockGuid);
    UNREFERENCED_PARAMETER(DesiredAccess);
    *DataBlockObject = NULL;
    return STATUS_WMI_GUID_NOT_FOUND;
}

NTSTATUS
NTAPI
IoWMIQueryAllData(IN PVOID DataBlockObject, IN OUT PULONG InOutBufferSize, OUT PVOID OutBuffer OPTIONAL)
{
    UNREFERENCED_PARAMETER(DataBlockObject);
    UNREFERENCED_PARAMETER(OutBuffer);
    *InOutBufferSize = 0;
    return STATUS_WMI_GUID_NOT_FOUND;
}
