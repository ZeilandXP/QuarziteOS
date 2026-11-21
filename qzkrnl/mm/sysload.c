/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Loading kernel-mode images (drivers, export drivers, win32k)
 *              from disk
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * FreeLoader loads the boot drivers; everything else comes from here. An
 * image is read whole from its file, laid out at its RVAs in a window of
 * its own (QZ_SYSIMAGE_BASE), relocated, and bound to the modules it
 * imports, which are loaded from \SystemRoot\System32\drivers (or
 * System32) first if they are not there yet. It then goes on the loaded
 * module list (PsLoadedModuleList's list, the loader block's) like the
 * boot images. Images are reference counted: a driver holds the export
 * drivers it imports, and the last reference unloads (DllUnload for an
 * export driver, then the pages go back).
 *
 * Everything is resident and writable: there are no paged or discardable
 * sections yet.
 */

#include "qzio.h"
#include "qzps.h"

#define QZ_SYSLOAD_TAG      'dLzQ'
#define QZ_SYSIMAGE_BASE    0xB8000000UL
#define QZ_SYSIMAGE_SIZE    (64UL * 1024 * 1024)
#define QZ_SYSIMAGE_PAGES   (QZ_SYSIMAGE_SIZE / QZ_PAGE_SIZE)
#define QZ_MAX_IMAGE_FILE   (32UL * 1024 * 1024)
#define QZ_MAX_LOAD_DEPTH   8

/* Base relocation types (the DDK headers leave them to winnt.h) */
#ifndef IMAGE_REL_BASED_ABSOLUTE
#define IMAGE_REL_BASED_ABSOLUTE    0
#define IMAGE_REL_BASED_HIGH        1
#define IMAGE_REL_BASED_LOW         2
#define IMAGE_REL_BASED_HIGHLOW     3
#define IMAGE_REL_BASED_HIGHADJ     4
#endif
#define QZ_HIWORD(x)    ((USHORT)(((ULONG_PTR)(x) >> 16) & 0xFFFF))
#define QZ_LOWORD(x)    ((USHORT)((ULONG_PTR)(x) & 0xFFFF))

typedef struct _QZ_SYSIMAGE
{
    LDR_DATA_TABLE_ENTRY Ldr;
    ULONG FirstPage;                    /* in the window */
    ULONG Pages;
    ULONG ImportCount;
    struct _QZ_SYSIMAGE **Imports;      /* loaded images this one holds */
    WCHAR Name[ANYSIZE_ARRAY];          /* FullDllName; BaseDllName points into it */
} QZ_SYSIMAGE, *PQZ_SYSIMAGE;

static ULONG QzSysImageBits[QZ_SYSIMAGE_PAGES / 32];
static RTL_BITMAP QzSysImageMap;
static KMUTEX QzLoaderMutex;
static BOOLEAN QzLoaderReady;

VOID QzIoForgetDll(PVOID ImageBase);    /* io/driver.c */
unsigned long __cdecl strtoul(const char *s, char **End, int Base);

VOID
QzMmInitSystemImages(VOID)
{
    RtlInitializeBitMap(&QzSysImageMap, QzSysImageBits, QZ_SYSIMAGE_PAGES);
    RtlClearAllBits(&QzSysImageMap);
    KeInitializeMutex(&QzLoaderMutex, 0);
    QzLoaderReady = TRUE;
}

/* Images in the window are ours; the loader's boot images are not */
static PQZ_SYSIMAGE
QzMmpSysImage(PLDR_DATA_TABLE_ENTRY Entry)
{
    ULONG_PTR Base = (ULONG_PTR)Entry->DllBase;
    if (Base >= QZ_SYSIMAGE_BASE && Base < QZ_SYSIMAGE_BASE + QZ_SYSIMAGE_SIZE && (Entry->Flags & LDRP_MM_LOADED))
        return CONTAINING_RECORD(Entry, QZ_SYSIMAGE, Ldr);
    return NULL;
}

BOOLEAN
QzMmIsDiskImage(PVOID Entry)
{
    return Entry && QzMmpSysImage(Entry) != NULL;
}

ULONG
QzMmSystemImagePages(VOID)
{
    return QzLoaderReady ? QZ_SYSIMAGE_PAGES - RtlNumberOfClearBits(&QzSysImageMap) : 0;
}

/* ---------------------------------------------------------------------- */
/* The loaded module list                                                  */
/* ---------------------------------------------------------------------- */

static BOOLEAN
QzMmpNameEquals(PCUNICODE_STRING Base, PCSTR Name, BOOLEAN StemOnly)
{
    ULONG i, Length = Base->Length / sizeof(WCHAR);

    for (i = 0; i < Length && Name[i]; i++)
    {
        if (RtlUpcaseUnicodeChar(Base->Buffer[i]) != RtlUpcaseUnicodeChar((UCHAR)Name[i]))
            return FALSE;
    }
    if (StemOnly)               /* "NTOSKRNL" against "ntoskrnl.exe" */
        return !Name[i] && (i == Length || Base->Buffer[i] == L'.');
    return i == Length && !Name[i];
}

static PLDR_DATA_TABLE_ENTRY
QzMmpKernelEntry(VOID)
{
    return CONTAINING_RECORD(QzLoaderBlock->LoadOrderListHead.Flink, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
}

/* A loaded module by file name ("hal.dll") or, for forwarders, by stem ("HAL") */
static PLDR_DATA_TABLE_ENTRY
QzMmpFindModule(PCSTR Name, BOOLEAN StemOnly)
{
    static const PCSTR KernelNames[] = { "ntoskrnl.exe", "ntkrnlpa.exe", "ntkrnlmp.exe", "ntkrpamp.exe",
                                         "NTOSKRNL", "ntkrnlpa", "ntkrnlmp", "ntkrpamp" };
    PLIST_ENTRY Entry;
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(KernelNames); i++)
    {
        if (!_stricmp(Name, KernelNames[i]))
            return QzMmpKernelEntry();
    }
    for (Entry = QzLoaderBlock->LoadOrderListHead.Flink; Entry != &QzLoaderBlock->LoadOrderListHead;
         Entry = Entry->Flink)
    {
        PLDR_DATA_TABLE_ENTRY Module = CONTAINING_RECORD(Entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
        if (!(Module->Flags & LDRP_UNLOAD_IN_PROGRESS) && QzMmpNameEquals(&Module->BaseDllName, Name, StemOnly))
            return Module;
    }
    return NULL;
}

static PLDR_DATA_TABLE_ENTRY
QzMmpFindModuleW(PCUNICODE_STRING BaseName)
{
    PLIST_ENTRY Entry;

    for (Entry = QzLoaderBlock->LoadOrderListHead.Flink; Entry != &QzLoaderBlock->LoadOrderListHead;
         Entry = Entry->Flink)
    {
        PLDR_DATA_TABLE_ENTRY Module = CONTAINING_RECORD(Entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
        if (!(Module->Flags & LDRP_UNLOAD_IN_PROGRESS) && RtlEqualUnicodeString(&Module->BaseDllName, BaseName, TRUE))
            return Module;
    }
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Exports                                                                 */
/* ---------------------------------------------------------------------- */

static PIMAGE_NT_HEADERS
QzMmpNtHeaders(PVOID Base)
{
    return (PIMAGE_NT_HEADERS)((PUCHAR)Base + ((PIMAGE_DOS_HEADER)Base)->e_lfanew);
}

static PVOID QzMmpResolveForwarder(PCSTR Forwarder, ULONG Depth);

/* Address of export Index (an index into AddressOfFunctions), forwarders followed */
static PVOID
QzMmpExportAt(PUCHAR Base, PIMAGE_EXPORT_DIRECTORY Exports, ULONG DirRva, ULONG DirSize, ULONG Index,
              ULONG Depth)
{
    PULONG Functions = (PULONG)(Base + Exports->AddressOfFunctions);
    ULONG Rva;

    if (Index >= Exports->NumberOfFunctions)
        return NULL;
    Rva = Functions[Index];
    if (!Rva)
        return NULL;
    if (Rva >= DirRva && Rva < DirRva + DirSize)
        return QzMmpResolveForwarder((PCSTR)(Base + Rva), Depth + 1);
    return Base + Rva;
}

static PVOID
QzMmpFindExport(PVOID ImageBase, PCSTR Name, ULONG Ordinal, USHORT Hint, ULONG Depth)
{
    PUCHAR Base = ImageBase;
    PIMAGE_NT_HEADERS Nt = QzMmpNtHeaders(Base);
    PIMAGE_DATA_DIRECTORY Dir = &Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    PIMAGE_EXPORT_DIRECTORY Exports;
    PULONG Names;
    PUSHORT Ordinals;
    LONG Low, High;

    if (!Dir->VirtualAddress || !Dir->Size || Depth > QZ_MAX_LOAD_DEPTH)
        return NULL;
    Exports = (PIMAGE_EXPORT_DIRECTORY)(Base + Dir->VirtualAddress);
    if (!Name)
        return QzMmpExportAt(Base, Exports, Dir->VirtualAddress, Dir->Size, Ordinal - Exports->Base, Depth);

    Names = (PULONG)(Base + Exports->AddressOfNames);
    Ordinals = (PUSHORT)(Base + Exports->AddressOfNameOrdinals);
    if (Hint < Exports->NumberOfNames && !strcmp((PCSTR)(Base + Names[Hint]), Name))
        return QzMmpExportAt(Base, Exports, Dir->VirtualAddress, Dir->Size, Ordinals[Hint], Depth);

    /* The name table is sorted */
    Low = 0;
    High = (LONG)Exports->NumberOfNames - 1;
    while (Low <= High)
    {
        LONG Middle = (Low + High) / 2;
        int Order = strcmp(Name, (PCSTR)(Base + Names[Middle]));
        if (!Order)
            return QzMmpExportAt(Base, Exports, Dir->VirtualAddress, Dir->Size, Ordinals[Middle], Depth);
        if (Order < 0)
            High = Middle - 1;
        else
            Low = Middle + 1;
    }
    return NULL;
}

/* "NTOSKRNL.KeTickCount" or "HAL.#12": only modules already loaded */
static PVOID
QzMmpResolveForwarder(PCSTR Forwarder, ULONG Depth)
{
    CHAR Module[64];
    PCSTR Dot = strchr(Forwarder, '.');
    PLDR_DATA_TABLE_ENTRY Target;
    ULONG Length;

    if (!Dot || Depth > QZ_MAX_LOAD_DEPTH)
        return NULL;
    Length = (ULONG)(Dot - Forwarder);
    if (Length >= sizeof(Module))
        return NULL;
    RtlCopyMemory(Module, Forwarder, Length);
    Module[Length] = 0;
    Target = QzMmpFindModule(Module, TRUE);
    if (!Target)
        return NULL;
    if (Dot[1] == '#')
        return QzMmpFindExport(Target->DllBase, NULL, strtoul(Dot + 2, NULL, 10), 0, Depth);
    return QzMmpFindExport(Target->DllBase, Dot + 1, 0, 0xFFFF, Depth);
}

PVOID
QzMmFindModuleExport(PVOID Entry, PCSTR Name)
{
    return QzMmpFindExport(((PLDR_DATA_TABLE_ENTRY)Entry)->DllBase, Name, 0, 0xFFFF, 0);
}

/* ---------------------------------------------------------------------- */
/* Reading and laying out an image                                         */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzMmpReadFile(PCUNICODE_STRING Path, PUCHAR *Buffer, PULONG Size)
{
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Io;
    FILE_STANDARD_INFORMATION Info;
    LARGE_INTEGER Offset;
    HANDLE File;
    NTSTATUS Status;
    PUCHAR Data;

    InitializeObjectAttributes(&Attributes, (PUNICODE_STRING)Path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    Status = ZwOpenFile(&File, FILE_READ_DATA | SYNCHRONIZE, &Attributes, &Io, FILE_SHARE_READ | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = ZwQueryInformationFile(File, &Io, &Info, sizeof(Info), FileStandardInformation);
    if (NT_SUCCESS(Status) && (Info.EndOfFile.QuadPart < (LONGLONG)sizeof(IMAGE_DOS_HEADER) ||
                               Info.EndOfFile.QuadPart > QZ_MAX_IMAGE_FILE))
        Status = STATUS_INVALID_IMAGE_FORMAT;
    if (NT_SUCCESS(Status))
    {
        Data = ExAllocatePoolWithTag(NonPagedPool, Info.EndOfFile.LowPart, QZ_SYSLOAD_TAG);
        if (!Data)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            Offset.QuadPart = 0;
            Status = ZwReadFile(File, NULL, NULL, NULL, &Io, Data, Info.EndOfFile.LowPart, &Offset, NULL);
            if (NT_SUCCESS(Status) && Io.Information != Info.EndOfFile.LowPart)
                Status = STATUS_END_OF_FILE;
            if (NT_SUCCESS(Status))
            {
                *Buffer = Data;
                *Size = Info.EndOfFile.LowPart;
            }
            else
            {
                ExFreePoolWithTag(Data, QZ_SYSLOAD_TAG);
            }
        }
    }
    ZwClose(File);
    return Status;
}

/* A 32-bit x86 PE whose sections fit in the image and in the file */
static PIMAGE_NT_HEADERS
QzMmpCheckImage(PUCHAR File, ULONG Size)
{
    PIMAGE_DOS_HEADER Dos = (PIMAGE_DOS_HEADER)File;
    PIMAGE_NT_HEADERS Nt;
    PIMAGE_SECTION_HEADER Section;
    ULONG i;

    if (Dos->e_magic != IMAGE_DOS_SIGNATURE || Dos->e_lfanew < sizeof(*Dos) ||
        (ULONG)Dos->e_lfanew > Size - sizeof(IMAGE_NT_HEADERS))
        return NULL;
    Nt = (PIMAGE_NT_HEADERS)(File + Dos->e_lfanew);
    if (Nt->Signature != IMAGE_NT_SIGNATURE || Nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        Nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        Nt->OptionalHeader.SizeOfImage == 0 || Nt->OptionalHeader.SizeOfImage > QZ_MAX_IMAGE_FILE ||
        Nt->OptionalHeader.SizeOfHeaders > Size || Nt->OptionalHeader.SizeOfHeaders > Nt->OptionalHeader.SizeOfImage ||
        Nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IAT)
        return NULL;
    Section = IMAGE_FIRST_SECTION(Nt);
    if ((PUCHAR)(Section + Nt->FileHeader.NumberOfSections) > File + Nt->OptionalHeader.SizeOfHeaders)
        return NULL;
    for (i = 0; i < Nt->FileHeader.NumberOfSections; i++, Section++)
    {
        ULONG Span = max(Section->Misc.VirtualSize, Section->SizeOfRawData);
        if (Section->VirtualAddress + Span < Section->VirtualAddress ||
            Section->VirtualAddress + Span > Nt->OptionalHeader.SizeOfImage + Nt->OptionalHeader.SectionAlignment)
            return NULL;
        if (Section->SizeOfRawData &&
            (Section->PointerToRawData > Size || Section->SizeOfRawData > Size - Section->PointerToRawData))
            return NULL;
    }
    return Nt;
}

static NTSTATUS
QzMmpRelocate(PUCHAR Base, PIMAGE_NT_HEADERS Nt)
{
    ULONG_PTR Delta = (ULONG_PTR)Base - Nt->OptionalHeader.ImageBase;
    PIMAGE_DATA_DIRECTORY Dir = &Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    ULONG Size = Nt->OptionalHeader.SizeOfImage, Offset = 0;

    if (!Delta)
        return STATUS_SUCCESS;
    if (!Dir->VirtualAddress || !Dir->Size || (Nt->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED))
        return STATUS_CONFLICTING_ADDRESSES;
    if (Dir->VirtualAddress > Size || Dir->Size > Size - Dir->VirtualAddress)
        return STATUS_INVALID_IMAGE_FORMAT;

    while (Offset + sizeof(IMAGE_BASE_RELOCATION) <= Dir->Size)
    {
        PIMAGE_BASE_RELOCATION Block = (PIMAGE_BASE_RELOCATION)(Base + Dir->VirtualAddress + Offset);
        PUSHORT Entry = (PUSHORT)(Block + 1);
        ULONG Count, i;

        if (Block->SizeOfBlock < sizeof(*Block) || Block->SizeOfBlock > Dir->Size - Offset)
            return STATUS_INVALID_IMAGE_FORMAT;
        Count = (Block->SizeOfBlock - sizeof(*Block)) / sizeof(USHORT);
        for (i = 0; i < Count; i++)
        {
            ULONG Rva = Block->VirtualAddress + (Entry[i] & 0xFFF);
            PUCHAR At = Base + Rva;

            switch (Entry[i] >> 12)
            {
                case IMAGE_REL_BASED_ABSOLUTE:
                    break;
                case IMAGE_REL_BASED_HIGHLOW:
                    if (Rva > Size - sizeof(ULONG))
                        return STATUS_INVALID_IMAGE_FORMAT;
                    *(PULONG)At += Delta;
                    break;
                case IMAGE_REL_BASED_HIGH:
                    if (Rva > Size - sizeof(USHORT))
                        return STATUS_INVALID_IMAGE_FORMAT;
                    *(PUSHORT)At += QZ_HIWORD(Delta);
                    break;
                case IMAGE_REL_BASED_LOW:
                    if (Rva > Size - sizeof(USHORT))
                        return STATUS_INVALID_IMAGE_FORMAT;
                    *(PUSHORT)At += QZ_LOWORD(Delta);
                    break;
                case IMAGE_REL_BASED_HIGHADJ:
                {
                    LONG Value;
                    if (Rva > Size - sizeof(USHORT) || i + 1 >= Count)
                        return STATUS_INVALID_IMAGE_FORMAT;
                    Value = (*(PUSHORT)At << 16) + (SHORT)Entry[++i];
                    Value += Delta + 0x8000;
                    *(PUSHORT)At = (USHORT)(Value >> 16);
                    break;
                }
                default:
                    return STATUS_INVALID_IMAGE_FORMAT;
            }
        }
        Offset += Block->SizeOfBlock;
    }
    Nt->OptionalHeader.ImageBase = (ULONG_PTR)Base;
    return STATUS_SUCCESS;
}

/* /GS images carry the default cookie until a loader sets a real one */
static VOID
QzMmpInitSecurityCookie(PUCHAR Base, PIMAGE_NT_HEADERS Nt)
{
    PIMAGE_DATA_DIRECTORY Dir = &Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    PIMAGE_LOAD_CONFIG_DIRECTORY32 Config;
    PULONG Cookie;
    ULONG Size = Nt->OptionalHeader.SizeOfImage;

    if (!Dir->VirtualAddress || Dir->VirtualAddress > Size - sizeof(*Config))
        return;
    Config = (PIMAGE_LOAD_CONFIG_DIRECTORY32)(Base + Dir->VirtualAddress);
    if (Config->Size < FIELD_OFFSET(IMAGE_LOAD_CONFIG_DIRECTORY32, SecurityCookie) + sizeof(ULONG) ||
        !Config->SecurityCookie)
        return;
    Cookie = (PULONG)Config->SecurityCookie;
    if ((PUCHAR)Cookie < Base || (PUCHAR)Cookie > Base + Size - sizeof(ULONG))
        return;
    if (*Cookie == 0xBB40E64E || *Cookie == 0)
    {
        *Cookie = (ULONG)__rdtsc() ^ (ULONG)(ULONG_PTR)Cookie ^ KeTickCount.LowPart;
        if (*Cookie == 0xBB40E64E || *Cookie == 0)
            *Cookie ^= 0x2F1E5A07;
    }
}

static PUCHAR
QzMmpMapWindow(ULONG Pages, PULONG FirstPage)
{
    ULONG First = RtlFindClearBitsAndSet(&QzSysImageMap, Pages, 0), i;
    PUCHAR Base;

    if (First == 0xFFFFFFFF)
        return NULL;
    Base = (PUCHAR)(QZ_SYSIMAGE_BASE + First * QZ_PAGE_SIZE);
    for (i = 0; i < Pages; i++)
        QzMapPage((ULONG_PTR)Base + i * QZ_PAGE_SIZE, QzAllocPhysicalPage(), QZ_PTE_WRITE);
    RtlZeroMemory(Base, Pages * QZ_PAGE_SIZE);
    *FirstPage = First;
    return Base;
}

static VOID
QzMmpUnmapWindow(ULONG FirstPage, ULONG Pages)
{
    ULONG i;
    for (i = 0; i < Pages; i++)
    {
        ULONG_PTR Va = QZ_SYSIMAGE_BASE + (FirstPage + i) * QZ_PAGE_SIZE;
        ULONG Pte = *(volatile ULONG *)(QZ_PTE_BASE + (Va >> 12) * 4);
        QzUnmapPage(Va);
        if (Pte & QZ_PTE_PRESENT)
            QzFreePhysicalPage(Pte >> QZ_PAGE_SHIFT);
    }
    RtlClearBits(&QzSysImageMap, FirstPage, Pages);
}

/* ---------------------------------------------------------------------- */
/* Loading and unloading                                                   */
/* ---------------------------------------------------------------------- */

static NTSTATUS QzMmpLoad(PCUNICODE_STRING Path, ULONG Depth, BOOLEAN Dependency, PLDR_DATA_TABLE_ENTRY *Result);
static VOID QzMmpRelease(PQZ_SYSIMAGE Image, BOOLEAN CallDllUnload);

/* A module an import names: loaded already, or from System32\drivers / System32 */
static NTSTATUS
QzMmpGetImport(PCSTR DllName, ULONG Depth, PLDR_DATA_TABLE_ENTRY *Result, BOOLEAN *Referenced)
{
    static const PCWSTR Directories[] = { L"\\SystemRoot\\System32\\drivers\\", L"\\SystemRoot\\System32\\" };
    PLDR_DATA_TABLE_ENTRY Module = QzMmpFindModule(DllName, FALSE);
    WCHAR Buffer[160];
    UNICODE_STRING Path;
    ULONG i, k;
    NTSTATUS Status = STATUS_DLL_NOT_FOUND;

    *Referenced = FALSE;
    if (Module)
    {
        PQZ_SYSIMAGE Image = QzMmpSysImage(Module);
        /* An image still loading imports back (win32k.sys and ftfd.dll):
           that edge holds no reference, or neither would ever unload */
        if (Image && !(Image->Ldr.Flags & LDRP_LOAD_IN_PROGRESS))
        {
            Image->Ldr.LoadCount++;
            *Referenced = TRUE;
        }
        *Result = Module;
        return STATUS_SUCCESS;
    }
    for (i = 0; i < RTL_NUMBER_OF(Directories); i++)
    {
        Path.Buffer = Buffer;
        Path.Length = 0;
        Path.MaximumLength = sizeof(Buffer);
        RtlAppendUnicodeToString(&Path, Directories[i]);
        for (k = 0; DllName[k] && Path.Length + sizeof(WCHAR) < Path.MaximumLength; k++)
        {
            Path.Buffer[Path.Length / sizeof(WCHAR)] = (UCHAR)DllName[k];
            Path.Length += sizeof(WCHAR);
        }
        Status = QzMmpLoad(&Path, Depth + 1, TRUE, &Module);
        if (Status == STATUS_IMAGE_ALREADY_LOADED)
        {
            *Result = Module;
            *Referenced = QzMmpSysImage(Module) != NULL;
            return STATUS_SUCCESS;
        }
        if (Status != STATUS_OBJECT_NAME_NOT_FOUND && Status != STATUS_OBJECT_PATH_NOT_FOUND &&
            Status != STATUS_NO_SUCH_FILE)
            break;
    }
    if (NT_SUCCESS(Status))
    {
        *Result = Module;
        *Referenced = TRUE;
    }
    else if (Status == STATUS_OBJECT_NAME_NOT_FOUND || Status == STATUS_OBJECT_PATH_NOT_FOUND)
    {
        Status = STATUS_DLL_NOT_FOUND;
    }
    return Status;
}

static NTSTATUS
QzMmpBindImports(PQZ_SYSIMAGE Image, PIMAGE_NT_HEADERS Nt, ULONG Depth)
{
    PUCHAR Base = Image->Ldr.DllBase;
    ULONG Size = Image->Ldr.SizeOfImage;
    PIMAGE_DATA_DIRECTORY Dir = &Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    PIMAGE_IMPORT_DESCRIPTOR Import;
    ULONG Count = 0, Capacity;

    if (!Dir->VirtualAddress || !Dir->Size)
        return STATUS_SUCCESS;
    if (Dir->VirtualAddress > Size - sizeof(*Import))
        return STATUS_INVALID_IMAGE_FORMAT;

    for (Import = (PIMAGE_IMPORT_DESCRIPTOR)(Base + Dir->VirtualAddress);
         (PUCHAR)(Import + 1) <= Base + Size && Import->Name; Import++)
        Count++;
    Capacity = Count;
    if (Capacity)
    {
        Image->Imports = ExAllocatePoolWithTag(NonPagedPool, Capacity * sizeof(PQZ_SYSIMAGE), QZ_SYSLOAD_TAG);
        if (!Image->Imports)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (Import = (PIMAGE_IMPORT_DESCRIPTOR)(Base + Dir->VirtualAddress); Count--; Import++)
    {
        PCSTR DllName;
        PLDR_DATA_TABLE_ENTRY Module;
        PIMAGE_THUNK_DATA Lookup, Iat;
        BOOLEAN Referenced;
        NTSTATUS Status;

        if (Import->Name >= Size || !Import->FirstThunk || Import->FirstThunk >= Size)
            return STATUS_INVALID_IMAGE_FORMAT;
        DllName = (PCSTR)(Base + Import->Name);
        Status = QzMmpGetImport(DllName, Depth, &Module, &Referenced);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(QZ_KERNEL_NAME ": %wZ imports %s: 0x%08lx\n", &Image->Ldr.BaseDllName, DllName, Status);
            return Status;
        }
        if (Referenced)
            Image->Imports[Image->ImportCount++] = QzMmpSysImage(Module);

        Lookup = (PIMAGE_THUNK_DATA)(Base + (Import->OriginalFirstThunk ? Import->OriginalFirstThunk
                                                                        : Import->FirstThunk));
        Iat = (PIMAGE_THUNK_DATA)(Base + Import->FirstThunk);
        for (; (PUCHAR)(Lookup + 1) <= Base + Size && Lookup->u1.AddressOfData; Lookup++, Iat++)
        {
            PVOID Address;
            PCSTR Name = NULL;

            if (IMAGE_SNAP_BY_ORDINAL(Lookup->u1.Ordinal))
            {
                Address = QzMmpFindExport(Module->DllBase, NULL, IMAGE_ORDINAL(Lookup->u1.Ordinal), 0, 0);
            }
            else
            {
                PIMAGE_IMPORT_BY_NAME ByName;
                if (Lookup->u1.AddressOfData >= Size - sizeof(*ByName))
                    return STATUS_INVALID_IMAGE_FORMAT;
                ByName = (PIMAGE_IMPORT_BY_NAME)(Base + Lookup->u1.AddressOfData);
                Name = (PCSTR)ByName->Name;
                Address = QzMmpFindExport(Module->DllBase, Name, 0, ByName->Hint, 0);
            }
            if (!Address)
            {
                if (Name)
                    DbgPrint(QZ_KERNEL_NAME ": %wZ imports %s!%s, which is not there\n",
                             &Image->Ldr.BaseDllName, DllName, Name);
                else
                    DbgPrint(QZ_KERNEL_NAME ": %wZ imports %s!#%lu, which is not there\n",
                             &Image->Ldr.BaseDllName, DllName, IMAGE_ORDINAL(Lookup->u1.Ordinal));
                return STATUS_PROCEDURE_NOT_FOUND;
            }
            Iat->u1.Function = (ULONG_PTR)Address;
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
QzMmpLoad(PCUNICODE_STRING Path, ULONG Depth, BOOLEAN Dependency, PLDR_DATA_TABLE_ENTRY *Result)
{
    UNICODE_STRING BaseName = *Path;
    PLDR_DATA_TABLE_ENTRY Existing;
    PQZ_SYSIMAGE Image;
    PIMAGE_NT_HEADERS FileNt, Nt;
    PIMAGE_SECTION_HEADER Section;
    PUCHAR File = NULL, Base;
    ULONG FileSize, Pages, FirstPage, i;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (Depth > QZ_MAX_LOAD_DEPTH)
        return STATUS_TOO_MANY_LINKS;

    /* Base name: the last component */
    for (i = BaseName.Length / sizeof(WCHAR); i && BaseName.Buffer[i - 1] != L'\\'; i--)
        ;
    BaseName.Buffer += i;
    BaseName.Length -= (USHORT)(i * sizeof(WCHAR));
    BaseName.MaximumLength = BaseName.Length;

    Existing = QzMmpFindModuleW(&BaseName);
    if (Existing)
    {
        PQZ_SYSIMAGE Loaded = QzMmpSysImage(Existing);
        if (Loaded)
            Loaded->Ldr.LoadCount++;
        *Result = Existing;
        return STATUS_IMAGE_ALREADY_LOADED;
    }

    Status = QzMmpReadFile(Path, &File, &FileSize);
    if (!NT_SUCCESS(Status))
        return Status;
    FileNt = QzMmpCheckImage(File, FileSize);
    if (!FileNt)
    {
        ExFreePoolWithTag(File, QZ_SYSLOAD_TAG);
        DbgPrint(QZ_KERNEL_NAME ": %wZ is not an x86 image\n", Path);
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    Pages = (FileNt->OptionalHeader.SizeOfImage + QZ_PAGE_SIZE - 1) / QZ_PAGE_SIZE;
    Base = QzMmpMapWindow(Pages, &FirstPage);
    Image = Base ? ExAllocatePoolWithTag(NonPagedPool, sizeof(QZ_SYSIMAGE) + Path->Length, QZ_SYSLOAD_TAG) : NULL;
    if (!Image)
    {
        if (Base)
            QzMmpUnmapWindow(FirstPage, Pages);
        ExFreePoolWithTag(File, QZ_SYSLOAD_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Headers, then each section at its RVA (the rest stays zero) */
    RtlCopyMemory(Base, File, FileNt->OptionalHeader.SizeOfHeaders);
    Section = IMAGE_FIRST_SECTION(FileNt);
    for (i = 0; i < FileNt->FileHeader.NumberOfSections; i++, Section++)
    {
        ULONG Bytes = Section->SizeOfRawData;
        if (Section->Misc.VirtualSize && Section->Misc.VirtualSize < Bytes)
            Bytes = Section->Misc.VirtualSize;
        if (Section->VirtualAddress + Bytes > Pages * QZ_PAGE_SIZE)
            Bytes = Pages * QZ_PAGE_SIZE - Section->VirtualAddress;
        if (Bytes)
            RtlCopyMemory(Base + Section->VirtualAddress, File + Section->PointerToRawData, Bytes);
    }
    ExFreePoolWithTag(File, QZ_SYSLOAD_TAG);
    Nt = QzMmpNtHeaders(Base);

    RtlZeroMemory(Image, sizeof(*Image));
    RtlCopyMemory(Image->Name, Path->Buffer, Path->Length);
    Image->Ldr.FullDllName.Buffer = Image->Name;
    Image->Ldr.FullDllName.Length = Image->Ldr.FullDllName.MaximumLength = Path->Length;
    Image->Ldr.BaseDllName.Buffer = Image->Name + (BaseName.Buffer - Path->Buffer);
    Image->Ldr.BaseDllName.Length = Image->Ldr.BaseDllName.MaximumLength = BaseName.Length;
    Image->Ldr.DllBase = Base;
    Image->Ldr.SizeOfImage = Nt->OptionalHeader.SizeOfImage;
    Image->Ldr.EntryPoint = Nt->OptionalHeader.AddressOfEntryPoint ? Base + Nt->OptionalHeader.AddressOfEntryPoint
                                                                   : NULL;
    Image->Ldr.CheckSum = Nt->OptionalHeader.CheckSum;
    Image->Ldr.LoadCount = 1;
    Image->Ldr.Flags = LDRP_MM_LOADED | LDRP_SYSTEM_MAPPED | LDRP_LOAD_IN_PROGRESS |
                       ((Nt->FileHeader.Characteristics & IMAGE_FILE_DLL) ? LDRP_IMAGE_DLL : 0) |
                       (Dependency ? LDRP_DRIVER_DEPENDENT_DLL : 0) |
                       ((ULONG_PTR)Base != FileNt->OptionalHeader.ImageBase ? LDRP_IMAGE_NOT_AT_BASE : 0);
    Image->FirstPage = FirstPage;
    Image->Pages = Pages;

    Status = QzMmpRelocate(Base, Nt);
    if (NT_SUCCESS(Status))
    {
        QzMmpInitSecurityCookie(Base, Nt);

        /* On the list before its imports, so that a cycle finds it */
        OldIrql = KfRaiseIrql(HIGH_LEVEL);
        InsertTailList(&QzLoaderBlock->LoadOrderListHead, &Image->Ldr.InLoadOrderLinks);
        KfLowerIrql(OldIrql);
        Status = QzMmpBindImports(Image, Nt, Depth);
        if (!NT_SUCCESS(Status))
        {
            OldIrql = KfRaiseIrql(HIGH_LEVEL);
            RemoveEntryList(&Image->Ldr.InLoadOrderLinks);
            KfLowerIrql(OldIrql);
        }
    }
    if (!NT_SUCCESS(Status))
    {
        for (i = 0; i < Image->ImportCount; i++)
            QzMmpRelease(Image->Imports[i], TRUE);
        if (Image->Imports)
            ExFreePoolWithTag(Image->Imports, QZ_SYSLOAD_TAG);
        QzMmpUnmapWindow(FirstPage, Pages);
        ExFreePoolWithTag(Image, QZ_SYSLOAD_TAG);
        return Status;
    }

    Image->Ldr.Flags = (Image->Ldr.Flags & ~LDRP_LOAD_IN_PROGRESS) | LDRP_ENTRY_PROCESSED;
    DbgPrint(QZ_KERNEL_NAME ": loaded %wZ at %p (%lu KB)%s\n", Path, Base, Pages * 4,
             Dependency ? " for an import" : "");
    *Result = &Image->Ldr;
    return STATUS_SUCCESS;
}

/* Drop a reference; the last one unloads (and calls DllUnload for an export driver) */
static VOID
QzMmpRelease(PQZ_SYSIMAGE Image, BOOLEAN CallDllUnload)
{
    KIRQL OldIrql;
    ULONG i;

    if (!Image || --Image->Ldr.LoadCount)
        return;
    Image->Ldr.Flags |= LDRP_UNLOAD_IN_PROGRESS;
    if (CallDllUnload && (Image->Ldr.Flags & LDRP_DRIVER_DEPENDENT_DLL))
    {
        NTSTATUS (NTAPI *DllUnload)(VOID) = QzMmpFindExport(Image->Ldr.DllBase, "DllUnload", 0, 0xFFFF, 0);
        if (DllUnload)
            DllUnload();
    }
    QzIoForgetDll(Image->Ldr.DllBase);
    OldIrql = KfRaiseIrql(HIGH_LEVEL);
    RemoveEntryList(&Image->Ldr.InLoadOrderLinks);
    KfLowerIrql(OldIrql);
    DbgPrint(QZ_KERNEL_NAME ": unloaded %wZ\n", &Image->Ldr.BaseDllName);

    for (i = 0; i < Image->ImportCount; i++)
        QzMmpRelease(Image->Imports[i], TRUE);
    if (Image->Imports)
        ExFreePoolWithTag(Image->Imports, QZ_SYSLOAD_TAG);
    QzMmpUnmapWindow(Image->FirstPage, Image->Pages);
    ExFreePoolWithTag(Image, QZ_SYSLOAD_TAG);
}

/*
 * Load Path (an NT path) and the images it imports. STATUS_IMAGE_ALREADY_LOADED
 * returns the loaded entry with a new reference, like a fresh load.
 */
NTSTATUS
QzMmLoadSystemImage(PCUNICODE_STRING Path, PVOID *Entry)
{
    PLDR_DATA_TABLE_ENTRY Result = NULL;
    NTSTATUS Status;

    if (!QzLoaderReady)
        return STATUS_NOT_SUPPORTED;
    KeWaitForSingleObject(&QzLoaderMutex, Executive, KernelMode, FALSE, NULL);
    Status = QzMmpLoad(Path, 0, FALSE, &Result);
    KeReleaseMutex(&QzLoaderMutex, FALSE);
    *Entry = Result;
    return Status;
}

VOID
QzMmUnloadSystemImage(PVOID Entry)
{
    PQZ_SYSIMAGE Image = QzMmpSysImage(Entry);
    if (!Image)
        return;
    KeWaitForSingleObject(&QzLoaderMutex, Executive, KernelMode, FALSE, NULL);
    QzMmpRelease(Image, TRUE);
    KeReleaseMutex(&QzLoaderMutex, FALSE);
}
