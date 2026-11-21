/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Section objects and mapped views
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * A section owns its pages; every view maps those same pages.
 *  - Image sections (SEC_IMAGE) lay a PE file out as it runs: headers and
 *    each section at its RVA, the rest zero. Views map the pages with the
 *    sections' protections; writable ones are copy-on-write, so each
 *    process gets its own .data, and an image ntdll must relocate gets
 *    private copies of the pages it patches.
 *  - Data sections are page-file backed (zeroed) or hold a copy of a
 *    file's contents. Views share writes, or copy on write with
 *    PAGE_WRITECOPY.
 * The pages are allocated when the section is made: there is no paging
 * yet, so nothing is gained by waiting.
 */

#include "qzio.h"
#include "qzps.h"

#define QZ_SECTION_TAG  'ceSQ'
#define QZ_SECTION_MAX_PAGES (256UL * 256)   /* 256 MB */

POBJECT_TYPE MmSectionObjectType;

static GENERIC_MAPPING QzSectionMapping =
{
    STANDARD_RIGHTS_READ | SECTION_MAP_READ | SECTION_QUERY,
    STANDARD_RIGHTS_WRITE | SECTION_MAP_WRITE,
    STANDARD_RIGHTS_EXECUTE | SECTION_MAP_EXECUTE,
    SECTION_ALL_ACCESS
};

/* ---------------------------------------------------------------------- */
/* Pages                                                                   */
/* ---------------------------------------------------------------------- */

static VOID
QzMmFillPage(ULONG Pfn, ULONG Offset, const VOID *Source, ULONG Length)
{
    ULONG Flags = QzSaveFlagsAndDisable();
    PUCHAR Page = QzMmMapHyper(Pfn);
    if (Source)
        RtlCopyMemory(Page + Offset, Source, Length);
    else
        RtlZeroMemory(Page + Offset, Length);
    QzMmUnmapHyper();
    QzRestoreFlags(Flags);
}

static NTSTATUS
QzMmAllocateSectionPages(PQZ_SECTION Section, ULONG Pages)
{
    ULONG i;

    if (Pages > QZ_SECTION_MAX_PAGES)
        return STATUS_SECTION_TOO_BIG;
    Section->Pfns = ExAllocatePoolWithTag(NonPagedPool, max(Pages, 1) * sizeof(ULONG), QZ_SECTION_TAG);
    if (!Section->Pfns)
        return STATUS_INSUFFICIENT_RESOURCES;
    for (i = 0; i < Pages; i++)
    {
        Section->Pfns[i] = QzAllocPhysicalPage();
        QzMmFillPage(Section->Pfns[i], 0, NULL, QZ_PAGE_SIZE);
    }
    Section->PageCount = Pages;
    return STATUS_SUCCESS;
}

/* Copy bytes into the section's pages at a byte offset */
static VOID
QzMmFillSection(PQZ_SECTION Section, ULONG Offset, const UCHAR *Source, ULONG Length)
{
    while (Length)
    {
        ULONG Page = Offset >> QZ_PAGE_SHIFT, Within = Offset & (QZ_PAGE_SIZE - 1);
        ULONG Chunk = min(Length, QZ_PAGE_SIZE - Within);
        if (Page >= Section->PageCount)
            return;
        QzMmFillPage(Section->Pfns[Page], Within, Source, Chunk);
        Offset += Chunk;
        Source += Chunk;
        Length -= Chunk;
    }
}

/* ---------------------------------------------------------------------- */
/* Reading the backing file                                                */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzMmReadFile(PFILE_OBJECT FileObject, ULONG Offset, PVOID Buffer, ULONG Length, PULONG Read)
{
    PDEVICE_OBJECT Device = IoGetRelatedDeviceObject(FileObject);
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER ByteOffset;
    PIO_STACK_LOCATION Stack;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    ByteOffset.QuadPart = Offset;
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ, Device, Buffer, Length, &ByteOffset, &Event, &IoStatus);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    /* Completion drops a reference on the file object of the IRP; the
       event is ours (IRP_SYNCHRONOUS_API), not an object reference */
    ObReferenceObject(FileObject);
    Irp->Tail.Overlay.OriginalFileObject = FileObject;
    Irp->Flags |= IRP_NOCACHE | IRP_SYNCHRONOUS_API;
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->FileObject = FileObject;
    IoStatus.Information = 0;
    Status = QzIoSynchronousCall(Device, Irp, &Event, &IoStatus);
    if (Status == STATUS_END_OF_FILE)
        Status = STATUS_SUCCESS;
    *Read = NT_SUCCESS(Status) ? (ULONG)IoStatus.Information : 0;
    return Status;
}

static NTSTATUS
QzMmFileSize(PFILE_OBJECT FileObject, PULONGLONG Size)
{
    PDEVICE_OBJECT Device = IoGetRelatedDeviceObject(FileObject);
    FILE_STANDARD_INFORMATION Info;
    IO_STATUS_BLOCK IoStatus;
    PIO_STACK_LOCATION Stack;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;

    Irp = IoAllocateIrp(Device->StackSize, FALSE);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp->UserEvent = &Event;
    Irp->UserIosb = &IoStatus;
    Irp->AssociatedIrp.SystemBuffer = &Info;
    ObReferenceObject(FileObject);
    Irp->Tail.Overlay.OriginalFileObject = FileObject;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->Flags = IRP_SYNCHRONOUS_API;
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = IRP_MJ_QUERY_INFORMATION;
    Stack->FileObject = FileObject;
    Stack->Parameters.QueryFile.Length = sizeof(Info);
    Stack->Parameters.QueryFile.FileInformationClass = FileStandardInformation;
    RtlZeroMemory(&Info, sizeof(Info));
    Status = QzIoSynchronousCall(Device, Irp, &Event, &IoStatus);
    *Size = NT_SUCCESS(Status) ? (ULONGLONG)Info.EndOfFile.QuadPart : 0;
    return Status;
}

/* Copy [FileOffset, +Length) of the file to section offset SectionOffset */
static NTSTATUS
QzMmLoadFromFile(PQZ_SECTION Section, PFILE_OBJECT FileObject, ULONG FileOffset, ULONG SectionOffset,
                 ULONG Length, PUCHAR Bounce, ULONG BounceSize)
{
    while (Length)
    {
        ULONG Chunk = min(Length, BounceSize), Read;
        NTSTATUS Status = QzMmReadFile(FileObject, FileOffset, Bounce, Chunk, &Read);
        if (!NT_SUCCESS(Status))
            return Status;
        if (!Read)
            break;   /* past the end of the file: the rest stays zero */
        QzMmFillSection(Section, SectionOffset, Bounce, Read);
        FileOffset += Read;
        SectionOffset += Read;
        Length -= Read;
    }
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Image sections                                                          */
/* ---------------------------------------------------------------------- */

static ULONG
QzMmSectionProtect(ULONG Characteristics)
{
    BOOLEAN Execute = (Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE)) != 0;
    BOOLEAN Write = (Characteristics & IMAGE_SCN_MEM_WRITE) != 0;

    if (Write)
        return Execute ? PAGE_EXECUTE_WRITECOPY : PAGE_WRITECOPY;
    return Execute ? PAGE_EXECUTE_READ : PAGE_READONLY;
}

static NTSTATUS
QzMmCreateImageSection(PQZ_SECTION Section, PFILE_OBJECT FileObject)
{
    ULONG BounceSize = 0x10000, Read, i, Page;
    PUCHAR Bounce = ExAllocatePoolWithTag(NonPagedPool, BounceSize, QZ_SECTION_TAG);
    PIMAGE_DOS_HEADER Dos;
    PIMAGE_NT_HEADERS32 Nt;
    PIMAGE_SECTION_HEADER Headers;
    ULONG SizeOfImage, SizeOfHeaders, Sections;
    ULONGLONG FileSize = 0;
    NTSTATUS Status;

    if (!Bounce)
        return STATUS_INSUFFICIENT_RESOURCES;
    QzMmFileSize(FileObject, &FileSize);
    Status = QzMmReadFile(FileObject, 0, Bounce, PAGE_SIZE, &Read);
    if (!NT_SUCCESS(Status))
        goto Done;

    /* Validate: an i386 PE image whose headers fit the first page */
    Status = STATUS_INVALID_IMAGE_FORMAT;
    Dos = (PIMAGE_DOS_HEADER)Bounce;
    if (Read < sizeof(*Dos) || Dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        Status = STATUS_INVALID_IMAGE_NOT_MZ;
        goto Done;
    }
    if ((ULONG)Dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) > Read)
        goto Done;
    Nt = (PIMAGE_NT_HEADERS32)(Bounce + Dos->e_lfanew);
    if (Nt->Signature != IMAGE_NT_SIGNATURE)
        goto Done;
    if (Nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        Nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    {
        Status = STATUS_INVALID_IMAGE_WIN_64;
        goto Done;
    }
    Sections = Nt->FileHeader.NumberOfSections;
    Headers = (PIMAGE_SECTION_HEADER)((PUCHAR)&Nt->OptionalHeader + Nt->FileHeader.SizeOfOptionalHeader);
    if ((PUCHAR)(Headers + Sections) > Bounce + Read)
        goto Done;
    SizeOfImage = (Nt->OptionalHeader.SizeOfImage + QZ_PAGE_SIZE - 1) & ~(QZ_PAGE_SIZE - 1);
    SizeOfHeaders = min(Nt->OptionalHeader.SizeOfHeaders, QZ_PAGE_SIZE);
    if (!SizeOfImage || SizeOfImage > QZ_SECTION_MAX_PAGES * QZ_PAGE_SIZE)
        goto Done;

    /* What NtQuerySection(SectionImageInformation) reports */
    {
        PSECTION_IMAGE_INFORMATION Info = &Section->ImageInformation;
        Info->TransferAddress = (PVOID)(Nt->OptionalHeader.ImageBase + Nt->OptionalHeader.AddressOfEntryPoint);
        Info->ZeroBits = 0;
        Info->MaximumStackSize = Nt->OptionalHeader.SizeOfStackReserve;
        Info->CommittedStackSize = Nt->OptionalHeader.SizeOfStackCommit;
        Info->SubSystemType = Nt->OptionalHeader.Subsystem;
        Info->SubSystemMinorVersion = Nt->OptionalHeader.MinorSubsystemVersion;
        Info->SubSystemMajorVersion = Nt->OptionalHeader.MajorSubsystemVersion;
        Info->GpValue = 0;
        Info->ImageCharacteristics = Nt->FileHeader.Characteristics;
        Info->DllCharacteristics = Nt->OptionalHeader.DllCharacteristics;
        Info->Machine = Nt->FileHeader.Machine;
        Info->ImageContainsCode = Nt->OptionalHeader.SizeOfCode != 0;
        Info->LoaderFlags = Nt->OptionalHeader.LoaderFlags;
        Info->ImageFileSize = (ULONG)FileSize;
    }
    Section->Image = TRUE;
    Section->ImageBase = Nt->OptionalHeader.ImageBase;
    Section->MaximumSize = SizeOfImage;

    Status = QzMmAllocateSectionPages(Section, SizeOfImage >> QZ_PAGE_SHIFT);
    if (!NT_SUCCESS(Status))
        goto Done;
    Section->PageProtect = ExAllocatePoolWithTag(NonPagedPool, Section->PageCount * sizeof(USHORT),
                                                 QZ_SECTION_TAG);
    if (!Section->PageProtect)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }
    for (Page = 0; Page < Section->PageCount; Page++)
        Section->PageProtect[Page] = PAGE_READONLY;

    /* The headers, then each section's raw data at its RVA */
    QzMmFillSection(Section, 0, Bounce, min(SizeOfHeaders, Read));
    {
        /* The section table goes away with the bounce buffer: keep a copy */
        ULONG TableBytes = Sections * sizeof(IMAGE_SECTION_HEADER);
        PIMAGE_SECTION_HEADER Table = ExAllocatePoolWithTag(NonPagedPool, max(TableBytes, 1), QZ_SECTION_TAG);
        if (!Table)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Done;
        }
        RtlCopyMemory(Table, Headers, TableBytes);
        for (i = 0; i < Sections && NT_SUCCESS(Status); i++)
        {
            ULONG Va = Table[i].VirtualAddress;
            ULONG VirtualSize = Table[i].Misc.VirtualSize ? Table[i].Misc.VirtualSize : Table[i].SizeOfRawData;
            ULONG Raw = min(Table[i].SizeOfRawData, VirtualSize);
            ULONG Protect = QzMmSectionProtect(Table[i].Characteristics);
            if (Va >= SizeOfImage)
                continue;
            if (Va + VirtualSize > SizeOfImage)
                VirtualSize = SizeOfImage - Va;
            if (Raw > VirtualSize)
                Raw = VirtualSize;
            if (Raw && Table[i].PointerToRawData)
                Status = QzMmLoadFromFile(Section, FileObject, Table[i].PointerToRawData, Va, Raw, Bounce, BounceSize);
            for (Page = Va >> QZ_PAGE_SHIFT;
                 Page < ((Va + VirtualSize + QZ_PAGE_SIZE - 1) >> QZ_PAGE_SHIFT) && Page < Section->PageCount; Page++)
                Section->PageProtect[Page] = (USHORT)Protect;
        }
        ExFreePoolWithTag(Table, QZ_SECTION_TAG);
    }

Done:
    ExFreePoolWithTag(Bounce, QZ_SECTION_TAG);
    return Status;
}

/* ---------------------------------------------------------------------- */
/* Section objects                                                         */
/* ---------------------------------------------------------------------- */

static VOID NTAPI
QzMmDeleteSection(PVOID Object)
{
    PQZ_SECTION Section = Object;
    ULONG i;

    if (Section->Pfns)
    {
        for (i = 0; i < Section->PageCount; i++)
            if (Section->Pfns[i])
                QzFreePhysicalPage(Section->Pfns[i]);
        ExFreePoolWithTag(Section->Pfns, QZ_SECTION_TAG);
    }
    if (Section->PageProtect)
        ExFreePoolWithTag(Section->PageProtect, QZ_SECTION_TAG);
    if (Section->FileObject)
        ObDereferenceObject(Section->FileObject);
}

VOID
QzMmSectionInit(VOID)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"Section");
    OBJECT_TYPE_INITIALIZER Init;

    RtlZeroMemory(&Init, sizeof(Init));
    Init.Length = sizeof(Init);
    Init.GenericMapping = QzSectionMapping;
    Init.ValidAccessMask = SECTION_ALL_ACCESS;
    Init.PoolType = NonPagedPool;
    Init.UseDefaultObject = TRUE;
    Init.DeleteProcedure = QzMmDeleteSection;
    if (!NT_SUCCESS(ObCreateObjectType(&Name, &Init, NULL, &MmSectionObjectType)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, 3, 0, 0, 0);
}

/* Make the body of a new section object (already created by the caller) */
static NTSTATUS
QzMmInitializeSection(PQZ_SECTION Section, PLARGE_INTEGER MaximumSize, ULONG Protect, ULONG Attributes,
                      PFILE_OBJECT FileObject)
{
    NTSTATUS Status;

    RtlZeroMemory(Section, sizeof(*Section));
    Section->AllocationAttributes = Attributes;
    Section->Protect = Protect;
    if (FileObject)
    {
        ObReferenceObject(FileObject);
        Section->FileObject = FileObject;
    }

    if (Attributes & SEC_IMAGE)
    {
        if (!FileObject)
            return STATUS_INVALID_FILE_FOR_SECTION;
        return QzMmCreateImageSection(Section, FileObject);
    }

    /* Data: the size asked for, or the file's */
    if (MaximumSize && MaximumSize->QuadPart)
        Section->MaximumSize = MaximumSize->QuadPart;
    else if (FileObject)
        QzMmFileSize(FileObject, &Section->MaximumSize);
    if (!Section->MaximumSize)
        return FileObject ? STATUS_MAPPED_FILE_SIZE_ZERO : STATUS_INVALID_PARAMETER_4;
    if (Section->MaximumSize > (ULONGLONG)QZ_SECTION_MAX_PAGES * QZ_PAGE_SIZE)
        return STATUS_SECTION_TOO_BIG;
    Status = QzMmAllocateSectionPages(Section, (ULONG)((Section->MaximumSize + QZ_PAGE_SIZE - 1) >> QZ_PAGE_SHIFT));
    if (NT_SUCCESS(Status) && FileObject)
    {
        PUCHAR Bounce = ExAllocatePoolWithTag(NonPagedPool, 0x10000, QZ_SECTION_TAG);
        if (!Bounce)
            return STATUS_INSUFFICIENT_RESOURCES;
        Status = QzMmLoadFromFile(Section, FileObject, 0, 0, (ULONG)Section->MaximumSize, Bounce, 0x10000);
        ExFreePoolWithTag(Bounce, QZ_SECTION_TAG);
    }
    return Status;
}

/* A section with no name, for the kernel's own use (the system DLL) */
NTSTATUS
QzMmCreateSection(PQZ_SECTION *Result, PLARGE_INTEGER MaximumSize, ULONG Protect, ULONG Attributes,
                  PFILE_OBJECT FileObject)
{
    PQZ_SECTION Section;
    NTSTATUS Status;

    *Result = NULL;
    Status = ObCreateObject(KernelMode, MmSectionObjectType, NULL, KernelMode, NULL, sizeof(QZ_SECTION), 0, 0,
                            (PVOID *)&Section);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = QzMmInitializeSection(Section, MaximumSize, Protect, Attributes, FileObject);
    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(Section);
        return Status;
    }
    *Result = Section;
    return STATUS_SUCCESS;
}

/* A data section holding a copy of kernel memory (the NLS tables) */
NTSTATUS
QzMmCreateSectionFromMemory(PVOID Data, ULONG Size, PQZ_SECTION *Result)
{
    LARGE_INTEGER Maximum;
    NTSTATUS Status;

    Maximum.QuadPart = Size;
    Status = QzMmCreateSection(Result, &Maximum, PAGE_READONLY, SEC_COMMIT, NULL);
    if (NT_SUCCESS(Status))
        QzMmFillSection(*Result, 0, Data, Size);
    return Status;
}

/* ---------------------------------------------------------------------- */
/* Views                                                                   */
/* ---------------------------------------------------------------------- */

NTSTATUS
QzMmMapView(PQZ_SECTION Section, PQZ_PROCESS Process, PVOID *BaseAddress, ULONG_PTR ZeroBits,
            PLARGE_INTEGER SectionOffset, PSIZE_T ViewSize, ULONG AllocationType, ULONG Protect)
{
    ULONG_PTR Base = (ULONG_PTR)*BaseAddress, Limit = 0, Va;
    ULONGLONG Offset = SectionOffset ? SectionOffset->QuadPart : 0;
    SIZE_T Size = *ViewSize;
    ULONG FirstPage, Page, Bits, PageProtect;
    PQZ_PROCESS Saved;
    PQZ_VAD Vad;
    NTSTATUS Status, Result = STATUS_SUCCESS;

    if (Process == QzSystemProcess)
        return STATUS_NOT_SUPPORTED;   /* views in system space: MmMapViewInSystemSpace, later */
    if (ZeroBits > 21)
        return STATUS_INVALID_PARAMETER_4;
    if (ZeroBits)
        Limit = (ULONG_PTR)(0xFFFFFFFFUL >> ZeroBits) + 1;

    if (Section->Image)
    {
        Offset = 0;
        Size = Section->PageCount << QZ_PAGE_SHIFT;
        if (!Base)
            Base = Section->ImageBase;
    }
    else
    {
        if (Offset & (QZ_ALLOCATION_GRANULARITY - 1))
            return STATUS_MAPPED_ALIGNMENT;
        if (Offset >= Section->MaximumSize)
            return STATUS_INVALID_VIEW_SIZE;
        if (!Size)
            Size = (SIZE_T)(Section->MaximumSize - Offset);
        if (Offset + Size > Section->MaximumSize)
            return STATUS_INVALID_VIEW_SIZE;
        if (Protect == 0)
            Protect = Section->Protect;
    }
    Size = (Size + QZ_PAGE_SIZE - 1) & ~(QZ_PAGE_SIZE - 1);
    FirstPage = (ULONG)(Offset >> QZ_PAGE_SHIFT);

    ExAcquireFastMutex(&Process->AddressLock);
    Base &= ~(QZ_ALLOCATION_GRANULARITY - 1);
    Status = QzMmInsertVad(Process, Base, Size, Section->Image ? MEM_IMAGE : MEM_MAPPED,
                           Section->Image ? PAGE_EXECUTE_WRITECOPY : Protect,
                           (AllocationType & MEM_TOP_DOWN) != 0, Limit, &Vad);
    if (!NT_SUCCESS(Status) && Section->Image && *BaseAddress == NULL)
    {
        /* The preferred base is taken: anywhere else, and ntdll relocates */
        Status = QzMmInsertVad(Process, 0, Size, MEM_IMAGE, PAGE_EXECUTE_WRITECOPY,
                               (AllocationType & MEM_TOP_DOWN) != 0, Limit, &Vad);
    }
    if (!NT_SUCCESS(Status))
    {
        ExReleaseFastMutex(&Process->AddressLock);
        return Status;
    }
    Vad->Section = Section;
    Vad->SectionOffset = (ULONG_PTR)Offset;
    ObReferenceObject(Section);
    InterlockedIncrement(&Section->Views);
    if (Section->Image && Vad->Start != Section->ImageBase)
        Result = STATUS_IMAGE_NOT_AT_BASE;

    QzPsAttach(Process, &Saved);
    for (Page = 0, Va = Vad->Start; Va < Vad->End; Page++, Va += QZ_PAGE_SIZE)
    {
        PageProtect = Section->Image ? Section->PageProtect[FirstPage + Page] : Protect;
        Bits = QzMmProtectToPte(PageProtect);
        QzMmEnsurePageTable(Va);
        if (!Bits)
        {
            *QzPteOf(Va) = QZ_SWPTE(PAGE_NOACCESS);
            continue;
        }
        /* Writes to an image, or to a write-copy view, get private pages */
        if ((Bits & QZ_PTE_WRITE) &&
            (Section->Image || (PageProtect & (PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))))
            Bits = (Bits & ~QZ_PTE_WRITE) | QZ_PTE_COPY_ON_WRITE;
        *QzPteOf(Va) = (Section->Pfns[FirstPage + Page] << QZ_PAGE_SHIFT) | Bits;
        QzInvlpg(Va);
    }
    QzPsDetach(Saved);
    ExReleaseFastMutex(&Process->AddressLock);

    *BaseAddress = (PVOID)Vad->Start;
    *ViewSize = Vad->End - Vad->Start;
    if (SectionOffset)
        SectionOffset->QuadPart = Offset;
    return Result;
}

NTSTATUS
QzMmUnmapView(PQZ_PROCESS Process, PVOID BaseAddress)
{
    PQZ_PROCESS Saved;
    PQZ_VAD Vad;
    ULONG_PTR Va;

    ExAcquireFastMutex(&Process->AddressLock);
    Vad = QzMmFindVad(Process, (ULONG_PTR)BaseAddress);
    if (!Vad || !Vad->Section)
    {
        ExReleaseFastMutex(&Process->AddressLock);
        return STATUS_NOT_MAPPED_VIEW;
    }
    QzPsAttach(Process, &Saved);
    for (Va = Vad->Start; Va < Vad->End; Va += QZ_PAGE_SIZE)
    {
        volatile ULONG *Pte;
        ULONG Value;
        if (!(*QzPdeOf(Va) & QZ_PTE_PRESENT))
            continue;
        Pte = QzPteOf(Va);
        Value = *Pte;
        *Pte = 0;
        QzInvlpg(Va);
        if ((Value & QZ_PTE_PRESENT) && (Value & QZ_PTE_PRIVATE))
        {
            QzFreePhysicalPage(Value >> QZ_PAGE_SHIFT);
            Process->PrivatePages--;
        }
    }
    QzPsDetach(Saved);
    QzMmRemoveVad(Process, Vad);
    ExReleaseFastMutex(&Process->AddressLock);
    return STATUS_SUCCESS;
}

/* A view's VAD is going away (its pages are already unmapped) */
VOID
QzMmDeleteSectionView(PQZ_PROCESS Process, PQZ_VAD Vad)
{
    PQZ_SECTION Section = Vad->Section;

    UNREFERENCED_PARAMETER(Process);
    if (Section)
    {
        Vad->Section = NULL;
        InterlockedDecrement(&Section->Views);
        ObDereferenceObject(Section);
    }
}

/* ---------------------------------------------------------------------- */
/* System services                                                         */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
NtCreateSection(OUT PHANDLE SectionHandle,
                IN ACCESS_MASK DesiredAccess,
                IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL,
                IN PLARGE_INTEGER MaximumSize OPTIONAL,
                IN ULONG SectionPageProtection,
                IN ULONG AllocationAttributes,
                IN HANDLE FileHandle OPTIONAL)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    LARGE_INTEGER Size;
    PFILE_OBJECT FileObject = NULL;
    PQZ_SECTION Section;
    HANDLE Handle;
    NTSTATUS Status;

    Size.QuadPart = 0;
    if (MaximumSize && !QzReadArg(Mode, &Size, MaximumSize, sizeof(Size)))
        return STATUS_ACCESS_VIOLATION;
    if (!(AllocationAttributes & (SEC_IMAGE | SEC_COMMIT | SEC_RESERVE)))
        AllocationAttributes |= SEC_COMMIT;
    if (FileHandle)
    {
        Status = ObReferenceObjectByHandle(FileHandle, FILE_READ_DATA, IoFileObjectType, Mode,
                                           (PVOID *)&FileObject, NULL);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    Status = ObCreateObject(Mode, MmSectionObjectType, ObjectAttributes, Mode, NULL, sizeof(QZ_SECTION), 0, 0,
                            (PVOID *)&Section);
    if (NT_SUCCESS(Status))
    {
        Status = QzMmInitializeSection(Section, &Size, SectionPageProtection, AllocationAttributes, FileObject);
        if (NT_SUCCESS(Status))
            Status = ObInsertObject(Section, NULL, DesiredAccess, 0, NULL, &Handle);
        else
            ObDereferenceObject(Section);
    }
    if (FileObject)
        ObDereferenceObject(FileObject);
    if (NT_SUCCESS(Status) && !QzWriteArg(Mode, SectionHandle, &Handle, sizeof(Handle)))
    {
        ObCloseHandle(Handle, Mode);
        Status = STATUS_ACCESS_VIOLATION;
    }
    return Status;
}

NTSTATUS
NTAPI
ZwCreateSection(OUT PHANDLE SectionHandle, IN ACCESS_MASK DesiredAccess,
                IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL, IN PLARGE_INTEGER MaximumSize OPTIONAL,
                IN ULONG SectionPageProtection, IN ULONG AllocationAttributes, IN HANDLE FileHandle OPTIONAL)
{
    return QZ_AS_KERNEL(NtCreateSection(SectionHandle, DesiredAccess, ObjectAttributes, MaximumSize,
                                        SectionPageProtection, AllocationAttributes, FileHandle));
}

NTSTATUS
NTAPI
NtOpenSection(OUT PHANDLE SectionHandle,
              IN ACCESS_MASK DesiredAccess,
              IN POBJECT_ATTRIBUTES ObjectAttributes)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    HANDLE Handle;
    NTSTATUS Status = ObOpenObjectByName(ObjectAttributes, MmSectionObjectType, Mode, NULL, DesiredAccess,
                                         NULL, &Handle);
    if (NT_SUCCESS(Status) && !QzWriteArg(Mode, SectionHandle, &Handle, sizeof(Handle)))
        Status = STATUS_ACCESS_VIOLATION;
    return Status;
}

NTSTATUS
NTAPI
ZwOpenSection(OUT PHANDLE SectionHandle, IN ACCESS_MASK DesiredAccess, IN POBJECT_ATTRIBUTES ObjectAttributes)
{
    return QZ_AS_KERNEL(NtOpenSection(SectionHandle, DesiredAccess, ObjectAttributes));
}

NTSTATUS
NTAPI
NtMapViewOfSection(IN HANDLE SectionHandle,
                   IN HANDLE ProcessHandle,
                   IN OUT PVOID *UBaseAddress,
                   IN ULONG_PTR ZeroBits,
                   IN SIZE_T CommitSize,
                   IN OUT PLARGE_INTEGER USectionOffset OPTIONAL,
                   IN OUT PSIZE_T UViewSize,
                   IN SECTION_INHERIT InheritDisposition,
                   IN ULONG AllocationType,
                   IN ULONG Win32Protect)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PQZ_SECTION Section;
    PQZ_PROCESS Process;
    PVOID Base;
    SIZE_T Size;
    LARGE_INTEGER Offset;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(CommitSize);
    UNREFERENCED_PARAMETER(InheritDisposition);
    Offset.QuadPart = 0;
    if (!QzReadArg(Mode, &Base, UBaseAddress, sizeof(Base)) || !QzReadArg(Mode, &Size, UViewSize, sizeof(Size)) ||
        (USectionOffset && !QzReadArg(Mode, &Offset, USectionOffset, sizeof(Offset))))
        return STATUS_ACCESS_VIOLATION;
    Status = ObReferenceObjectByHandle(SectionHandle, SECTION_MAP_READ, MmSectionObjectType, Mode,
                                       (PVOID *)&Section, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = ObReferenceObjectByHandle(ProcessHandle, PROCESS_VM_OPERATION, PsProcessType, Mode,
                                       (PVOID *)&Process, NULL);
    if (NT_SUCCESS(Status))
    {
        Status = QzMmMapView(Section, Process, &Base, ZeroBits, &Offset, &Size, AllocationType, Win32Protect);
        ObDereferenceObject(Process);
    }
    ObDereferenceObject(Section);
    if (NT_SUCCESS(Status) &&
        (!QzWriteArg(Mode, UBaseAddress, &Base, sizeof(Base)) || !QzWriteArg(Mode, UViewSize, &Size, sizeof(Size)) ||
         (USectionOffset && !QzWriteArg(Mode, USectionOffset, &Offset, sizeof(Offset)))))
        Status = STATUS_ACCESS_VIOLATION;
    return Status;
}

NTSTATUS
NTAPI
ZwMapViewOfSection(IN HANDLE SectionHandle, IN HANDLE ProcessHandle, IN OUT PVOID *BaseAddress,
                   IN ULONG_PTR ZeroBits, IN SIZE_T CommitSize, IN OUT PLARGE_INTEGER SectionOffset OPTIONAL,
                   IN OUT PSIZE_T ViewSize, IN SECTION_INHERIT InheritDisposition, IN ULONG AllocationType,
                   IN ULONG Win32Protect)
{
    return QZ_AS_KERNEL(NtMapViewOfSection(SectionHandle, ProcessHandle, BaseAddress, ZeroBits, CommitSize,
                                           SectionOffset, ViewSize, InheritDisposition, AllocationType,
                                           Win32Protect));
}

NTSTATUS
NTAPI
NtUnmapViewOfSection(IN HANDLE ProcessHandle,
                     IN PVOID BaseAddress)
{
    PQZ_PROCESS Process;
    NTSTATUS Status = ObReferenceObjectByHandle(ProcessHandle, PROCESS_VM_OPERATION, PsProcessType,
                                                ExGetPreviousMode(), (PVOID *)&Process, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = QzMmUnmapView(Process, BaseAddress);
    ObDereferenceObject(Process);
    return Status;
}

NTSTATUS
NTAPI
ZwUnmapViewOfSection(IN HANDLE ProcessHandle, IN PVOID BaseAddress)
{
    return QZ_AS_KERNEL(NtUnmapViewOfSection(ProcessHandle, BaseAddress));
}

NTSTATUS
NTAPI
NtQuerySection(IN HANDLE SectionHandle,
               IN SECTION_INFORMATION_CLASS SectionInformationClass,
               OUT PVOID SectionInformation,
               IN SIZE_T SectionInformationLength,
               OUT PSIZE_T ResultLength OPTIONAL)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PQZ_SECTION Section;
    SIZE_T Needed;
    NTSTATUS Status;

    Status = ObReferenceObjectByHandle(SectionHandle, SECTION_QUERY, MmSectionObjectType, Mode,
                                       (PVOID *)&Section, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    switch (SectionInformationClass)
    {
        case SectionBasicInformation:
        {
            SECTION_BASIC_INFORMATION Info;
            Needed = sizeof(Info);
            if (SectionInformationLength < Needed)
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            Info.BaseAddress = Section->Image ? (PVOID)Section->ImageBase : NULL;
            Info.Attributes = Section->AllocationAttributes;
            Info.Size.QuadPart = (LONGLONG)Section->MaximumSize;
            if (!QzWriteArg(Mode, SectionInformation, &Info, sizeof(Info)))
                Status = STATUS_ACCESS_VIOLATION;
            break;
        }
        case SectionImageInformation:
            Needed = sizeof(SECTION_IMAGE_INFORMATION);
            if (!Section->Image)
                Status = STATUS_SECTION_NOT_IMAGE;
            else if (SectionInformationLength < Needed)
                Status = STATUS_INFO_LENGTH_MISMATCH;
            else if (!QzWriteArg(Mode, SectionInformation, &Section->ImageInformation, Needed))
                Status = STATUS_ACCESS_VIOLATION;
            break;
        default:
            Needed = 0;
            Status = STATUS_INVALID_INFO_CLASS;
            break;
    }
    ObDereferenceObject(Section);
    if (ResultLength && Status != STATUS_INVALID_INFO_CLASS)
        QzWriteArg(Mode, ResultLength, &Needed, sizeof(Needed));
    return Status;
}

NTSTATUS
NTAPI
NtAreMappedFilesTheSame(IN PVOID File1MappedAsAnImage,
                        IN PVOID File2MappedAsFile)
{
    PQZ_PROCESS Process = QzCurrentThread->ApcProcess;
    PQZ_VAD A, B;
    NTSTATUS Status = STATUS_NOT_SAME_DEVICE;

    ExAcquireFastMutex(&Process->AddressLock);
    A = QzMmFindVad(Process, (ULONG_PTR)File1MappedAsAnImage);
    B = QzMmFindVad(Process, (ULONG_PTR)File2MappedAsFile);
    if (!A || !B || !A->Section || !B->Section)
        Status = STATUS_INVALID_ADDRESS;
    else if (A->Section->FileObject && B->Section->FileObject &&
             A->Section->FileObject->FsContext == B->Section->FileObject->FsContext)
        Status = STATUS_SUCCESS;
    ExReleaseFastMutex(&Process->AddressLock);
    return Status;
}
