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

/*
 * Mapped files: a data section of a file maps the cache manager's pages
 * of it (cc/cache.c), so a mapped view, cached reads and writes, and the
 * file on disk all see one copy. Each view holds a cache reference on
 * every page of its range from mapping to unmapping; writes through it
 * show in the user PTEs' dirty bits, which go to the cache when the view
 * goes, and whenever the cache flushes the file (QzMmCollectFileDirty).
 */
typedef struct _QZ_SECTION_VIEW
{
    LIST_ENTRY Link;                /* the section's ViewList */
    PQZ_PROCESS Process;
    PQZ_VAD Vad;
    ULONGLONG Offset;               /* into the file */
    ULONG Pages;
} QZ_SECTION_VIEW, *PQZ_SECTION_VIEW;

static LIST_ENTRY QzMmFileSections;     /* sections with a CacheMap */
static FAST_MUTEX QzMmFileLock;
VOID QzMmCollectDirty(PSECTION_OBJECT_POINTERS Sop);

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

static NTSTATUS QzMmFileSize(PFILE_OBJECT FileObject, PULONGLONG Size);

static NTSTATUS
QzMmReadFileNoCache(PFILE_OBJECT FileObject, ULONG Offset, PVOID Buffer, ULONG Length, PULONG Read)
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

/*
 * Through the cache when the file system gives the file section object
 * pointers: the reads become paging reads, which a file system takes at
 * any offset and also after the file's handle is closed. Otherwise a
 * non-cached read.
 */
static NTSTATUS
QzMmReadFile(PFILE_OBJECT FileObject, ULONG Offset, PVOID Buffer, ULONG Length, PULONG Read)
{
    ULONGLONG Size;
    PVOID Map;
    NTSTATUS Status;

    if (!FileObject->SectionObjectPointer)
        return QzMmReadFileNoCache(FileObject, Offset, Buffer, Length, Read);
    Status = QzMmFileSize(FileObject, &Size);
    if (!NT_SUCCESS(Status))
        return Status;
    if (Offset >= Size)
    {
        *Read = 0;
        return STATUS_SUCCESS;
    }
    Length = (ULONG)min(Length, Size - Offset);
    Map = QzCcMmAcquire(FileObject, (LONGLONG)Size);
    if (!Map)
        return QzMmReadFileNoCache(FileObject, Offset, Buffer, Length, Read);
    Status = QzCcMmRead(Map, Offset, Buffer, Length);
    QzCcMmRelease(Map);
    *Read = NT_SUCCESS(Status) ? Length : 0;
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
        for (i = 0; i < Section->PageCount && !Section->Physical; i++)
            if (Section->Pfns[i])
                QzFreePhysicalPage(Section->Pfns[i]);
        ExFreePoolWithTag(Section->Pfns, QZ_SECTION_TAG);
    }
    if (Section->PageProtect)
        ExFreePoolWithTag(Section->PageProtect, QZ_SECTION_TAG);
    if (Section->CacheMap)
    {
        ExAcquireFastMutex(&QzMmFileLock);
        RemoveEntryList(&Section->FileLink);
        ExReleaseFastMutex(&QzMmFileLock);
        QzCcMmRelease(Section->CacheMap);
    }
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
    InitializeListHead(&QzMmFileSections);
    ExInitializeFastMutex(&QzMmFileLock);
    QzMmCollectFileDirty = QzMmCollectDirty;
}

/*
 * \Device\PhysicalMemory, for the first megabyte: videoprt maps the video
 * memory and BIOS area (A0000-FFFFF) into csrss for its int 10h support.
 */
#define QZ_PHYSICAL_SECTION_PAGES   256

VOID
QzMmCreatePhysicalMemorySection(VOID)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Device\\PhysicalMemory");
    OBJECT_ATTRIBUTES Attributes;
    PQZ_SECTION Section;
    HANDLE Handle;
    ULONG i;
    NTSTATUS Status;

    InitializeObjectAttributes(&Attributes, &Name, OBJ_PERMANENT | OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL, NULL);
    Status = ObCreateObject(KernelMode, MmSectionObjectType, &Attributes, KernelMode, NULL, sizeof(QZ_SECTION), 0, 0,
                            (PVOID *)&Section);
    if (!NT_SUCCESS(Status))
        return;
    RtlZeroMemory(Section, sizeof(*Section));
    InitializeListHead(&Section->ViewList);
    Section->AllocationAttributes = SEC_COMMIT;
    Section->Protect = PAGE_READWRITE;
    Section->Physical = TRUE;
    Section->PageCount = QZ_PHYSICAL_SECTION_PAGES;
    Section->MaximumSize = (ULONGLONG)QZ_PHYSICAL_SECTION_PAGES * QZ_PAGE_SIZE;
    Section->Pfns = ExAllocatePoolWithTag(NonPagedPool, QZ_PHYSICAL_SECTION_PAGES * sizeof(ULONG), QZ_SECTION_TAG);
    if (!Section->Pfns)
    {
        ObDereferenceObject(Section);
        return;
    }
    for (i = 0; i < QZ_PHYSICAL_SECTION_PAGES; i++)
        Section->Pfns[i] = i;
    if (NT_SUCCESS(ObInsertObject(Section, NULL, SECTION_ALL_ACCESS, 0, NULL, &Handle)))
        ZwClose(Handle);
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
    InitializeListHead(&Section->ViewList);
    if (FileObject && FileObject->SectionObjectPointer)
    {
        ULONGLONG FileSize = 0;
        QzMmFileSize(FileObject, &FileSize);
        Section->CacheMap = QzCcMmAcquire(FileObject, (LONGLONG)FileSize);
        if (Section->CacheMap)
        {
            ExAcquireFastMutex(&QzMmFileLock);
            InsertTailList(&QzMmFileSections, &Section->FileLink);
            ExReleaseFastMutex(&QzMmFileLock);
            Section->PageCount = (ULONG)((Section->MaximumSize + QZ_PAGE_SIZE - 1) >> QZ_PAGE_SHIFT);
            return STATUS_SUCCESS;
        }
    }
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
    {
        /* The System process has no user space: its views go in system space (kernel LPC views) */
        if (Offset)
            return STATUS_NOT_SUPPORTED;
        return MmMapViewInSystemSpace(Section, BaseAddress, ViewSize);
    }
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

    /* A mapped file: the cache's pages of the range, held while mapped */
    if (Section->CacheMap)
    {
        PQZ_SECTION_VIEW View = ExAllocatePoolWithTag(NonPagedPool, sizeof(*View), QZ_SECTION_TAG);
        ULONG Count = (ULONG)((Vad->End - Vad->Start) >> QZ_PAGE_SHIFT), i;
        PULONG Pfns = ExAllocatePoolWithTag(NonPagedPool, max(Count, 1) * sizeof(ULONG), QZ_SECTION_TAG);

        Status = (View && Pfns) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
        for (i = 0; NT_SUCCESS(Status) && i < Count; i++)
            Status = QzCcMmGetPage(Section->CacheMap, (LONGLONG)Offset + (LONGLONG)i * QZ_PAGE_SIZE, &Pfns[i]);
        if (!NT_SUCCESS(Status))
        {
            while (Pfns && i--)
                QzCcMmPutPage(Section->CacheMap, (LONGLONG)Offset + (LONGLONG)i * QZ_PAGE_SIZE, FALSE);
            if (View)
                ExFreePoolWithTag(View, QZ_SECTION_TAG);
            if (Pfns)
                ExFreePoolWithTag(Pfns, QZ_SECTION_TAG);
            QzMmRemoveVad(Process, Vad);
            ExReleaseFastMutex(&Process->AddressLock);
            return Status;
        }
        View->Process = Process;
        View->Vad = Vad;
        View->Offset = Offset;
        View->Pages = Count;
        ExAcquireFastMutex(&QzMmFileLock);
        InsertTailList(&Section->ViewList, &View->Link);
        ExReleaseFastMutex(&QzMmFileLock);

        QzPsAttach(Process, &Saved);
        for (i = 0, Va = Vad->Start; i < Count; i++, Va += QZ_PAGE_SIZE)
        {
            Bits = QzMmProtectToPte(Protect);
            QzMmEnsurePageTable(Va);
            if (!Bits)
            {
                *QzPteOf(Va) = QZ_SWPTE(PAGE_NOACCESS);
                continue;
            }
            if ((Bits & QZ_PTE_WRITE) && (Protect & (PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)))
                Bits = (Bits & ~QZ_PTE_WRITE) | QZ_PTE_COPY_ON_WRITE;
            *QzPteOf(Va) = (Pfns[i] << QZ_PAGE_SHIFT) | Bits;
            QzInvlpg(Va);
        }
        QzPsDetach(Saved);
        ExFreePoolWithTag(Pfns, QZ_SECTION_TAG);
        ExReleaseFastMutex(&Process->AddressLock);
        *BaseAddress = (PVOID)Vad->Start;
        *ViewSize = Vad->End - Vad->Start;
        if (SectionOffset)
            SectionOffset->QuadPart = Offset;
        return STATUS_SUCCESS;
    }
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

    if (Process == QzSystemProcess)
        return MmUnmapViewInSystemSpace(BaseAddress);

    ExAcquireFastMutex(&Process->AddressLock);
    Vad = QzMmFindVad(Process, (ULONG_PTR)BaseAddress);
    if (!Vad || !Vad->Section)
    {
        ExReleaseFastMutex(&Process->AddressLock);
        return STATUS_NOT_MAPPED_VIEW;
    }
    QzPsAttach(Process, &Saved);
    QzMmReleaseViewPages(Process, Vad);
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

/* ---------------------------------------------------------------------- */
/* Shared images                                                           */
/* ---------------------------------------------------------------------- */

static ULONG QzMmImageCacheHits;

static BOOLEAN
QzObjectAttributesHaveName(KPROCESSOR_MODE Mode, POBJECT_ATTRIBUTES ObjectAttributes)
{
    OBJECT_ATTRIBUTES Local;
    return QzReadArg(Mode, &Local, ObjectAttributes, sizeof(Local)) && Local.ObjectName;
}

/* The file's image: kept (referenced) in its SectionObjectPointer until the file changes */
static VOID
QzMmCacheImage(PQZ_SECTION Section, PSECTION_OBJECT_POINTERS Sop)
{
    ExAcquireFastMutex(&QzMmFileLock);
    if (!Sop->ImageSectionObject)
    {
        ObReferenceObject(Section);
        Sop->ImageSectionObject = Section;
    }
    ExReleaseFastMutex(&QzMmFileLock);
}

/* MmFlushImageSection: the file is about to be written or deleted. FALSE if a process maps it */
BOOLEAN
QzMmPurgeImage(PSECTION_OBJECT_POINTERS Sop)
{
    PQZ_SECTION Section;

    ExAcquireFastMutex(&QzMmFileLock);
    Section = Sop->ImageSectionObject;
    if (Section && Section->Views)
    {
        ExReleaseFastMutex(&QzMmFileLock);
        return FALSE;
    }
    Sop->ImageSectionObject = NULL;
    ExReleaseFastMutex(&QzMmFileLock);
    if (Section)
        ObDereferenceObject(Section);
    return TRUE;
}

ULONG
QzMmImageCacheStats(VOID)
{
    return QzMmImageCacheHits;
}

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
        /* An image needs the file opened for execution; data, for what the view may do */
        ACCESS_MASK FileAccess;
        if (AllocationAttributes & SEC_IMAGE)
            FileAccess = FILE_EXECUTE;
        else if (SectionPageProtection & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
            FileAccess = FILE_READ_DATA | FILE_WRITE_DATA;
        else if (SectionPageProtection & PAGE_EXECUTE)
            FileAccess = FILE_EXECUTE;
        else
            FileAccess = FILE_READ_DATA;
        if (SectionPageProtection & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            FileAccess |= FILE_EXECUTE;
        Status = ObReferenceObjectByHandle(FileHandle, FileAccess, IoFileObjectType, Mode,
                                           (PVOID *)&FileObject, NULL);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    /*
     * An image already built for this file is shared, as NT shares its
     * image control area: every process maps the same pages, copy on
     * write. A named section (\KnownDlls) still gets its own object.
     */
    if ((AllocationAttributes & SEC_IMAGE) && FileObject && FileObject->SectionObjectPointer &&
        !(ObjectAttributes && QzObjectAttributesHaveName(Mode, ObjectAttributes)))
    {
        PQZ_SECTION Cached;
        ExAcquireFastMutex(&QzMmFileLock);
        Cached = FileObject->SectionObjectPointer->ImageSectionObject;
        if (Cached)
            ObReferenceObject(Cached);
        ExReleaseFastMutex(&QzMmFileLock);
        if (Cached)
        {
            Status = ObOpenObjectByPointer(Cached, 0, NULL, DesiredAccess ? DesiredAccess : SECTION_ALL_ACCESS,
                                           MmSectionObjectType, Mode, &Handle);
            ObDereferenceObject(Cached);
            QzMmImageCacheHits++;
            goto Done;
        }
    }
    Status = ObCreateObject(Mode, MmSectionObjectType, ObjectAttributes, Mode, NULL, sizeof(QZ_SECTION), 0, 0,
                            (PVOID *)&Section);
    if (NT_SUCCESS(Status))
    {
        Status = QzMmInitializeSection(Section, &Size, SectionPageProtection, AllocationAttributes, FileObject);
        if (NT_SUCCESS(Status) && Section->Image && FileObject && FileObject->SectionObjectPointer)
            QzMmCacheImage(Section, FileObject->SectionObjectPointer);
        if (NT_SUCCESS(Status))
            Status = ObInsertObject(Section, NULL, DesiredAccess, 0, NULL, &Handle);
        else
            ObDereferenceObject(Section);
    }
Done:
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

/* The section access a view with this protection needs */
static ACCESS_MASK
QzMmViewAccess(ULONG Win32Protect)
{
    switch (Win32Protect & 0xFF)
    {
        case PAGE_EXECUTE:              return SECTION_MAP_EXECUTE;
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_WRITECOPY:    return SECTION_MAP_EXECUTE | SECTION_MAP_READ;
        case PAGE_EXECUTE_READWRITE:    return SECTION_MAP_EXECUTE | SECTION_MAP_READ | SECTION_MAP_WRITE;
        case PAGE_READWRITE:            return SECTION_MAP_READ | SECTION_MAP_WRITE;
        case PAGE_NOACCESS:             return 0;
        default:                        return SECTION_MAP_READ;
    }
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
    Status = ObReferenceObjectByHandle(SectionHandle, QzMmViewAccess(Win32Protect), MmSectionObjectType, Mode,
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

/* ---------------------------------------------------------------------- */
/* What file systems ask before deleting or truncating a file              */
/* ---------------------------------------------------------------------- */

/* Image sections are laid out in memory of their own: the file is free */
BOOLEAN
QzMmIsImageInUse(PSECTION_OBJECT_POINTERS Sop)
{
    PQZ_SECTION Section = Sop->ImageSectionObject;
    return Section && Section->Views > 0;
}

/* Is some view of a mapped file's section beyond Size? */
BOOLEAN
QzMmIsDataMappedBeyond(PSECTION_OBJECT_POINTERS Sop, LONGLONG Size)
{
    PLIST_ENTRY Entry, ViewEntry;
    BOOLEAN Beyond = FALSE;

    if (!QzMmFileSections.Flink)
        return FALSE;
    ExAcquireFastMutex(&QzMmFileLock);
    for (Entry = QzMmFileSections.Flink; Entry != &QzMmFileSections && !Beyond; Entry = Entry->Flink)
    {
        PQZ_SECTION Section = CONTAINING_RECORD(Entry, QZ_SECTION, FileLink);
        if (QzCcMmSop(Section->CacheMap) != Sop)
            continue;
        for (ViewEntry = Section->ViewList.Flink; ViewEntry != &Section->ViewList; ViewEntry = ViewEntry->Flink)
        {
            PQZ_SECTION_VIEW View = CONTAINING_RECORD(ViewEntry, QZ_SECTION_VIEW, Link);
            if ((LONGLONG)(View->Offset + ((ULONGLONG)View->Pages << QZ_PAGE_SHIFT)) > Size)
                Beyond = TRUE;
        }
    }
    ExReleaseFastMutex(&QzMmFileLock);
    return Beyond;
}

/* Find (and unlink) the record of a mapped file's view (file lock held) */
static PQZ_SECTION_VIEW
QzMmTakeView(PQZ_SECTION Section, PQZ_VAD Vad)
{
    PLIST_ENTRY Entry;
    for (Entry = Section->ViewList.Flink; Entry != &Section->ViewList; Entry = Entry->Flink)
    {
        PQZ_SECTION_VIEW View = CONTAINING_RECORD(Entry, QZ_SECTION_VIEW, Link);
        if (View->Vad == Vad)
        {
            RemoveEntryList(Entry);
            return View;
        }
    }
    return NULL;
}

/*
 * A view's pages leave the address space (Process attached): private
 * copies are freed; a mapped file's pages go back to the cache, dirty if
 * they were written.
 */
VOID
QzMmReleaseViewPages(PQZ_PROCESS Process, PQZ_VAD Vad)
{
    PQZ_SECTION Section = Vad->Section;
    PQZ_SECTION_VIEW View = NULL;
    ULONG_PTR Va;

    if (Section && Section->CacheMap)
    {
        ExAcquireFastMutex(&QzMmFileLock);
        View = QzMmTakeView(Section, Vad);
        ExReleaseFastMutex(&QzMmFileLock);
    }
    for (Va = Vad->Start; Va < Vad->End; Va += QZ_PAGE_SIZE)
    {
        volatile ULONG *Pte;
        ULONG Value = 0;
        if (*QzPdeOf(Va) & QZ_PTE_PRESENT)
        {
            Pte = QzPteOf(Va);
            Value = *Pte;
            *Pte = 0;
            QzInvlpg(Va);
        }
        if ((Value & QZ_PTE_PRESENT) && (Value & QZ_PTE_PRIVATE))
        {
            QzFreePhysicalPage(Value >> QZ_PAGE_SHIFT);
            Process->PrivatePages--;
            Value = 0;
        }
        if (View)
            QzCcMmPutPage(Section->CacheMap, (LONGLONG)View->Offset + (LONGLONG)(Va - Vad->Start),
                          (Value & QZ_PTE_PRESENT) && (Value & QZ_PTE_DIRTY));
    }
    if (View)
        ExFreePoolWithTag(View, QZ_SECTION_TAG);
}

/* The cache flushes a file: what views wrote becomes the cache's dirty data */
VOID
QzMmCollectDirty(PSECTION_OBJECT_POINTERS Sop)
{
    PLIST_ENTRY Entry, ViewEntry;

    if (IsListEmpty(&QzMmFileSections))
        return;
    ExAcquireFastMutex(&QzMmFileLock);
    for (Entry = QzMmFileSections.Flink; Entry != &QzMmFileSections; Entry = Entry->Flink)
    {
        PQZ_SECTION Section = CONTAINING_RECORD(Entry, QZ_SECTION, FileLink);
        if (Sop && QzCcMmSop(Section->CacheMap) != Sop)
            continue;
        for (ViewEntry = Section->ViewList.Flink; ViewEntry != &Section->ViewList; ViewEntry = ViewEntry->Flink)
        {
            PQZ_SECTION_VIEW View = CONTAINING_RECORD(ViewEntry, QZ_SECTION_VIEW, Link);
            PQZ_PROCESS Saved;
            ULONG i;
            QzPsAttach(View->Process, &Saved);
            for (i = 0; i < View->Pages; i++)
            {
                ULONG_PTR Va = View->Vad->Start + ((ULONG_PTR)i << QZ_PAGE_SHIFT);
                volatile ULONG *Pte;
                if (!(*QzPdeOf(Va) & QZ_PTE_PRESENT))
                    continue;
                Pte = QzPteOf(Va);
                if ((*Pte & (QZ_PTE_PRESENT | QZ_PTE_PRIVATE | QZ_PTE_DIRTY)) == (QZ_PTE_PRESENT | QZ_PTE_DIRTY))
                {
                    *Pte &= ~QZ_PTE_DIRTY;
                    QzInvlpg(Va);
                    QzCcMmSetDirty(Section->CacheMap, (LONGLONG)View->Offset + ((LONGLONG)i << QZ_PAGE_SHIFT));
                }
            }
            QzPsDetach(Saved);
        }
    }
    ExReleaseFastMutex(&QzMmFileLock);
}

/* Write a mapped file's view back (the part of it asked for) */
NTSTATUS
NTAPI
NtFlushVirtualMemory(IN HANDLE ProcessHandle, IN OUT PVOID *BaseAddress, IN OUT PSIZE_T NumberOfBytesToFlush,
                     OUT PIO_STATUS_BLOCK IoStatusBlock)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PQZ_PROCESS Process;
    PVOID Base;
    SIZE_T Size;
    PQZ_VAD Vad;
    IO_STATUS_BLOCK Io;
    PSECTION_OBJECT_POINTERS Sop = NULL;
    LARGE_INTEGER Offset;
    ULONG Length = 0;
    NTSTATUS Status;

    if (!QzReadArg(Mode, &Base, BaseAddress, sizeof(Base)) || !QzReadArg(Mode, &Size, NumberOfBytesToFlush, sizeof(Size)))
        return STATUS_ACCESS_VIOLATION;
    Status = ObReferenceObjectByHandle(ProcessHandle, PROCESS_VM_OPERATION, PsProcessType, Mode, (PVOID *)&Process,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    ExAcquireFastMutex(&Process->AddressLock);
    Vad = QzMmFindVad(Process, (ULONG_PTR)Base);
    if (!Vad || !Vad->Section)
        Status = STATUS_NOT_MAPPED_VIEW;
    else if (Vad->Section->CacheMap)
    {
        ULONG_PTR Start = (ULONG_PTR)Base & ~(QZ_PAGE_SIZE - 1);
        ULONG_PTR End = Size ? min(Vad->End, ((ULONG_PTR)Base + Size + QZ_PAGE_SIZE - 1) & ~(QZ_PAGE_SIZE - 1)) : Vad->End;
        Sop = QzCcMmSop(Vad->Section->CacheMap);
        Offset.QuadPart = (LONGLONG)Vad->SectionOffset + (Start - Vad->Start);
        Length = (ULONG)(End - Start);
        Base = (PVOID)Start;
        Size = Length;
    }
    ExReleaseFastMutex(&Process->AddressLock);
    ObDereferenceObject(Process);
    if (!NT_SUCCESS(Status))
        return Status;

    Io.Status = STATUS_SUCCESS;
    Io.Information = 0;
    if (Sop)
    {
        QzMmCollectDirty(Sop);
        CcFlushCache(Sop, &Offset, Length, &Io);
    }
    if (!QzWriteArg(Mode, BaseAddress, &Base, sizeof(Base)) ||
        !QzWriteArg(Mode, NumberOfBytesToFlush, &Size, sizeof(Size)) ||
        !QzWriteArg(Mode, IoStatusBlock, &Io, sizeof(Io)))
        return STATUS_ACCESS_VIOLATION;
    return Io.Status;
}

NTSTATUS
NTAPI
ZwFlushVirtualMemory(IN HANDLE ProcessHandle, IN OUT PVOID *BaseAddress, IN OUT PSIZE_T NumberOfBytesToFlush,
                     OUT PIO_STATUS_BLOCK IoStatusBlock)
{
    return QZ_AS_KERNEL(NtFlushVirtualMemory(ProcessHandle, BaseAddress, NumberOfBytesToFlush, IoStatusBlock));
}
