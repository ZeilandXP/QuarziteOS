/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Atom tables, and the global atom table behind Nt*Atom
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * An atom names a string, case-insensitively: 0xC000 and up are string
 * atoms (an index into the table), below that integer atoms ("#123" or
 * the number itself) that need no entry. Each entry counts its
 * references; a pinned one stays whatever the count.
 */

#include "qz.h"
#include "qzps.h"
int __cdecl _snwprintf(wchar_t *Buffer, size_t Count, const wchar_t *Format, ...);

#define QZ_ATOM_TAG         'tAzQ'
#define QZ_MAX_ATOM_LENGTH  255
#define QZ_FIRST_STRING_ATOM 0xC000
#define QZ_MAX_ATOMS        0x3FFF

typedef struct _QZ_ATOM_ENTRY
{
    USHORT ReferenceCount;
    BOOLEAN Pinned;
    UCHAR NameLength;               /* characters */
    WCHAR Name[QZ_MAX_ATOM_LENGTH + 1];
} QZ_ATOM_ENTRY, *PQZ_ATOM_ENTRY;

typedef struct _QZ_ATOM_TABLE
{
    ULONG Signature;
    FAST_MUTEX Lock;
    ULONG Capacity;
    PQZ_ATOM_ENTRY *Entries;
} QZ_ATOM_TABLE, *PQZ_ATOM_TABLE;

#define QZ_ATOM_SIGNATURE 0x6D6F7441

/* "#123" or a pointer-sized value below 0xC000: an integer atom */
static BOOLEAN
QzIntegerAtom(PCWSTR Name, PRTL_ATOM Atom)
{
    ULONG Value = 0, i;

    if ((ULONG_PTR)Name < 0x10000)
    {
        if (!(ULONG_PTR)Name || (ULONG_PTR)Name >= QZ_FIRST_STRING_ATOM)
            return FALSE;
        *Atom = (RTL_ATOM)(ULONG_PTR)Name;
        return TRUE;
    }
    if (Name[0] != L'#')
        return FALSE;
    for (i = 1; Name[i]; i++)
    {
        if (Name[i] < L'0' || Name[i] > L'9' || i > 6)
            return FALSE;
        Value = Value * 10 + (Name[i] - L'0');
    }
    if (i == 1 || !Value || Value >= QZ_FIRST_STRING_ATOM)
        return FALSE;
    *Atom = (RTL_ATOM)Value;
    return TRUE;
}

NTSTATUS
NTAPI
RtlCreateAtomTable(IN ULONG TableSize, IN OUT PRTL_ATOM_TABLE *AtomTable)
{
    PQZ_ATOM_TABLE Table;

    if (*AtomTable)
        return STATUS_SUCCESS;
    Table = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Table), QZ_ATOM_TAG);
    if (!Table)
        return STATUS_NO_MEMORY;
    Table->Signature = QZ_ATOM_SIGNATURE;
    ExInitializeFastMutex(&Table->Lock);
    Table->Capacity = max(TableSize, 37);
    Table->Entries = ExAllocatePoolWithTag(NonPagedPool, Table->Capacity * sizeof(PVOID), QZ_ATOM_TAG);
    if (!Table->Entries)
    {
        ExFreePoolWithTag(Table, QZ_ATOM_TAG);
        return STATUS_NO_MEMORY;
    }
    RtlZeroMemory(Table->Entries, Table->Capacity * sizeof(PVOID));
    *AtomTable = (PRTL_ATOM_TABLE)Table;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlDestroyAtomTable(IN PRTL_ATOM_TABLE AtomTable)
{
    PQZ_ATOM_TABLE Table = (PQZ_ATOM_TABLE)AtomTable;
    ULONG i;
    if (!Table || Table->Signature != QZ_ATOM_SIGNATURE)
        return STATUS_INVALID_PARAMETER;
    for (i = 0; i < Table->Capacity; i++)
        if (Table->Entries[i])
            ExFreePoolWithTag(Table->Entries[i], QZ_ATOM_TAG);
    ExFreePoolWithTag(Table->Entries, QZ_ATOM_TAG);
    Table->Signature = 0;
    ExFreePoolWithTag(Table, QZ_ATOM_TAG);
    return STATUS_SUCCESS;
}

static LONG
QzFindAtom(PQZ_ATOM_TABLE Table, PCWSTR Name, ULONG Length)
{
    ULONG i, k;
    for (i = 0; i < Table->Capacity; i++)
    {
        PQZ_ATOM_ENTRY Entry = Table->Entries[i];
        if (!Entry || Entry->NameLength != Length)
            continue;
        for (k = 0; k < Length; k++)
            if (RtlUpcaseUnicodeChar(Entry->Name[k]) != RtlUpcaseUnicodeChar(Name[k]))
                break;
        if (k == Length)
            return (LONG)i;
    }
    return -1;
}

static PQZ_ATOM_ENTRY
QzAtomEntry(PQZ_ATOM_TABLE Table, RTL_ATOM Atom)
{
    ULONG Index = (ULONG)Atom - QZ_FIRST_STRING_ATOM;
    return (Atom >= QZ_FIRST_STRING_ATOM && Index < Table->Capacity) ? Table->Entries[Index] : NULL;
}

NTSTATUS
NTAPI
RtlAddAtomToAtomTable(IN PRTL_ATOM_TABLE AtomTable, IN PWSTR AtomName, OUT PRTL_ATOM Atom OPTIONAL)
{
    PQZ_ATOM_TABLE Table = (PQZ_ATOM_TABLE)AtomTable;
    RTL_ATOM Result;
    ULONG Length;
    LONG Index;

    if (!Table || Table->Signature != QZ_ATOM_SIGNATURE)
        return STATUS_INVALID_PARAMETER;
    if (QzIntegerAtom(AtomName, &Result))
    {
        if (Atom)
            *Atom = Result;
        return STATUS_SUCCESS;
    }
    if ((ULONG_PTR)AtomName < 0x10000)
        return STATUS_INVALID_PARAMETER;
    Length = (ULONG)wcslen(AtomName);
    if (!Length || Length > QZ_MAX_ATOM_LENGTH)
        return STATUS_INVALID_PARAMETER;

    ExAcquireFastMutex(&Table->Lock);
    Index = QzFindAtom(Table, AtomName, Length);
    if (Index >= 0)
    {
        Table->Entries[Index]->ReferenceCount++;
    }
    else
    {
        PQZ_ATOM_ENTRY Entry;
        ULONG i;
        for (i = 0; i < Table->Capacity && Table->Entries[i]; i++)
            ;
        if (i == Table->Capacity)
        {
            ULONG Capacity = min(Table->Capacity * 2, QZ_MAX_ATOMS);
            PQZ_ATOM_ENTRY *Entries = Capacity > Table->Capacity
                ? ExAllocatePoolWithTag(NonPagedPool, Capacity * sizeof(PVOID), QZ_ATOM_TAG) : NULL;
            if (!Entries)
            {
                ExReleaseFastMutex(&Table->Lock);
                return STATUS_NO_MEMORY;
            }
            RtlZeroMemory(Entries, Capacity * sizeof(PVOID));
            RtlCopyMemory(Entries, Table->Entries, Table->Capacity * sizeof(PVOID));
            ExFreePoolWithTag(Table->Entries, QZ_ATOM_TAG);
            Table->Entries = Entries;
            Table->Capacity = Capacity;
        }
        Entry = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Entry), QZ_ATOM_TAG);
        if (!Entry)
        {
            ExReleaseFastMutex(&Table->Lock);
            return STATUS_NO_MEMORY;
        }
        Entry->ReferenceCount = 1;
        Entry->Pinned = FALSE;
        Entry->NameLength = (UCHAR)Length;
        RtlCopyMemory(Entry->Name, AtomName, Length * sizeof(WCHAR));
        Entry->Name[Length] = 0;
        Table->Entries[i] = Entry;
        Index = (LONG)i;
    }
    ExReleaseFastMutex(&Table->Lock);
    if (Atom)
        *Atom = (RTL_ATOM)(QZ_FIRST_STRING_ATOM + Index);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlLookupAtomInAtomTable(IN PRTL_ATOM_TABLE AtomTable, IN PWSTR AtomName, OUT PRTL_ATOM Atom OPTIONAL)
{
    PQZ_ATOM_TABLE Table = (PQZ_ATOM_TABLE)AtomTable;
    RTL_ATOM Result;
    LONG Index;

    if (!Table || Table->Signature != QZ_ATOM_SIGNATURE)
        return STATUS_INVALID_PARAMETER;
    if (QzIntegerAtom(AtomName, &Result))
    {
        if (Atom)
            *Atom = Result;
        return STATUS_SUCCESS;
    }
    if ((ULONG_PTR)AtomName < 0x10000)
        return STATUS_INVALID_PARAMETER;
    ExAcquireFastMutex(&Table->Lock);
    Index = QzFindAtom(Table, AtomName, (ULONG)wcslen(AtomName));
    ExReleaseFastMutex(&Table->Lock);
    if (Index < 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    if (Atom)
        *Atom = (RTL_ATOM)(QZ_FIRST_STRING_ATOM + Index);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlDeleteAtomFromAtomTable(IN PRTL_ATOM_TABLE AtomTable, IN RTL_ATOM Atom)
{
    PQZ_ATOM_TABLE Table = (PQZ_ATOM_TABLE)AtomTable;
    PQZ_ATOM_ENTRY Entry;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!Table || Table->Signature != QZ_ATOM_SIGNATURE)
        return STATUS_INVALID_PARAMETER;
    if (Atom < QZ_FIRST_STRING_ATOM)
        return STATUS_SUCCESS;
    ExAcquireFastMutex(&Table->Lock);
    Entry = QzAtomEntry(Table, Atom);
    if (!Entry)
        Status = STATUS_INVALID_HANDLE;
    else if (!Entry->Pinned && --Entry->ReferenceCount == 0)
    {
        Table->Entries[Atom - QZ_FIRST_STRING_ATOM] = NULL;
        ExFreePoolWithTag(Entry, QZ_ATOM_TAG);
    }
    else if (Entry->Pinned)
        Status = STATUS_WAS_LOCKED;
    ExReleaseFastMutex(&Table->Lock);
    return Status;
}

NTSTATUS
NTAPI
RtlPinAtomInAtomTable(IN PRTL_ATOM_TABLE AtomTable, IN RTL_ATOM Atom)
{
    PQZ_ATOM_TABLE Table = (PQZ_ATOM_TABLE)AtomTable;
    PQZ_ATOM_ENTRY Entry;

    if (!Table || Table->Signature != QZ_ATOM_SIGNATURE)
        return STATUS_INVALID_PARAMETER;
    if (Atom < QZ_FIRST_STRING_ATOM)
        return STATUS_SUCCESS;
    ExAcquireFastMutex(&Table->Lock);
    Entry = QzAtomEntry(Table, Atom);
    if (Entry)
        Entry->Pinned = TRUE;
    ExReleaseFastMutex(&Table->Lock);
    return Entry ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

/* NameLength: bytes, in (room) and out (the name's) */
NTSTATUS
NTAPI
RtlQueryAtomInAtomTable(IN PRTL_ATOM_TABLE AtomTable, IN RTL_ATOM Atom, OUT PULONG RefCount OPTIONAL,
                        OUT PULONG PinCount OPTIONAL, OUT PWSTR AtomName OPTIONAL, IN OUT PULONG NameLength OPTIONAL)
{
    PQZ_ATOM_TABLE Table = (PQZ_ATOM_TABLE)AtomTable;
    WCHAR Number[8];
    PCWSTR Name;
    ULONG Length, Copy;
    PQZ_ATOM_ENTRY Entry = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!Table || Table->Signature != QZ_ATOM_SIGNATURE || !Atom)
        return STATUS_INVALID_PARAMETER;
    ExAcquireFastMutex(&Table->Lock);
    if (Atom < QZ_FIRST_STRING_ATOM)
    {
        Length = (ULONG)_snwprintf(Number, RTL_NUMBER_OF(Number), L"#%u", Atom);
        Name = Number;
        if (RefCount)
            *RefCount = 1;
        if (PinCount)
            *PinCount = 1;
    }
    else
    {
        Entry = QzAtomEntry(Table, Atom);
        if (!Entry)
        {
            ExReleaseFastMutex(&Table->Lock);
            return STATUS_INVALID_HANDLE;
        }
        Name = Entry->Name;
        Length = Entry->NameLength;
        if (RefCount)
            *RefCount = Entry->ReferenceCount;
        if (PinCount)
            *PinCount = Entry->Pinned;
    }
    if (NameLength)
    {
        if (AtomName && *NameLength >= sizeof(WCHAR))
        {
            Copy = min(Length, *NameLength / sizeof(WCHAR) - 1);
            RtlCopyMemory(AtomName, Name, Copy * sizeof(WCHAR));
            AtomName[Copy] = 0;
            if (Copy < Length)
                Status = STATUS_BUFFER_TOO_SMALL;
            *NameLength = Copy * sizeof(WCHAR);
        }
        else
        {
            *NameLength = Length * sizeof(WCHAR);
            if (AtomName)
                Status = STATUS_BUFFER_TOO_SMALL;
        }
    }
    ExReleaseFastMutex(&Table->Lock);
    return Status;
}

/* ---------------------------------------------------------------------- */
/* The global atom table (kernel32's GlobalAddAtom & co.)                  */
/* ---------------------------------------------------------------------- */

static PRTL_ATOM_TABLE QzGlobalAtoms;

static PRTL_ATOM_TABLE
QzGlobalAtomTable(VOID)
{
    if (!QzGlobalAtoms)
        RtlCreateAtomTable(37, &QzGlobalAtoms);
    return QzGlobalAtoms;
}

static NTSTATUS
QzCaptureAtomName(PWSTR UserName, ULONG Length, PWSTR Buffer)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    if (Length > QZ_MAX_ATOM_LENGTH * sizeof(WCHAR) || (Length & 1))
        return STATUS_INVALID_PARAMETER;
    if (!QzReadArg(Mode, Buffer, UserName, Length))
        return STATUS_ACCESS_VIOLATION;
    Buffer[Length / sizeof(WCHAR)] = 0;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NtAddAtom(IN PWSTR AtomName OPTIONAL, IN ULONG AtomNameLength, OUT PRTL_ATOM Atom OPTIONAL)
{
    WCHAR Name[QZ_MAX_ATOM_LENGTH + 1];
    RTL_ATOM Result = 0;
    NTSTATUS Status = QzCaptureAtomName(AtomName, AtomNameLength, Name);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = RtlAddAtomToAtomTable(QzGlobalAtomTable(), Name, &Result);
    if (NT_SUCCESS(Status) && Atom && !QzWriteArg(ExGetPreviousMode(), Atom, &Result, sizeof(Result)))
        Status = STATUS_ACCESS_VIOLATION;
    return Status;
}

NTSTATUS
NTAPI
NtFindAtom(IN PWSTR AtomName OPTIONAL, IN ULONG AtomNameLength, OUT PRTL_ATOM Atom OPTIONAL)
{
    WCHAR Name[QZ_MAX_ATOM_LENGTH + 1];
    RTL_ATOM Result = 0;
    NTSTATUS Status = QzCaptureAtomName(AtomName, AtomNameLength, Name);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = RtlLookupAtomInAtomTable(QzGlobalAtomTable(), Name, &Result);
    if (NT_SUCCESS(Status) && Atom && !QzWriteArg(ExGetPreviousMode(), Atom, &Result, sizeof(Result)))
        Status = STATUS_ACCESS_VIOLATION;
    return Status;
}

NTSTATUS
NTAPI
NtDeleteAtom(IN RTL_ATOM Atom)
{
    return RtlDeleteAtomFromAtomTable(QzGlobalAtomTable(), Atom);
}

NTSTATUS
NTAPI
NtQueryInformationAtom(IN RTL_ATOM Atom, IN ATOM_INFORMATION_CLASS AtomInformationClass, OUT PVOID AtomInformation,
                       IN ULONG AtomInformationLength, OUT PULONG ReturnLength OPTIONAL)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    UCHAR Buffer[sizeof(ATOM_BASIC_INFORMATION) + QZ_MAX_ATOM_LENGTH * sizeof(WCHAR) + sizeof(WCHAR)];
    PATOM_BASIC_INFORMATION Info = (PVOID)Buffer;
    ULONG RefCount, PinCount, NameLength, Needed;
    NTSTATUS Status;

    if (AtomInformationClass != AtomBasicInformation)
        return STATUS_INVALID_INFO_CLASS;
    NameLength = (QZ_MAX_ATOM_LENGTH + 1) * sizeof(WCHAR);
    Status = RtlQueryAtomInAtomTable(QzGlobalAtomTable(), Atom, &RefCount, &PinCount, Info->Name, &NameLength);
    if (!NT_SUCCESS(Status))
        return Status;
    Info->UsageCount = (USHORT)RefCount;
    Info->Flags = PinCount ? 1 : 0;           /* RTL_ATOM_PINNED */
    Info->NameLength = (USHORT)NameLength;
    Needed = FIELD_OFFSET(ATOM_BASIC_INFORMATION, Name) + NameLength + sizeof(WCHAR);
    if (ReturnLength && !QzWriteArg(Mode, ReturnLength, &Needed, sizeof(Needed)))
        return STATUS_ACCESS_VIOLATION;
    if (AtomInformationLength < Needed)
        return STATUS_BUFFER_TOO_SMALL;
    return QzWriteArg(Mode, AtomInformation, Info, Needed) ? STATUS_SUCCESS : STATUS_ACCESS_VIOLATION;
}
