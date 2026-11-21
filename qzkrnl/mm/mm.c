/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Physical page allocator and virtual mapping via the PDE self-map
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qz.h"

#undef MmGetPhysicalAddress

#define QZ_MAX_PAGES     (1024UL * 1024)          /* 4 GB of 4 KB pages */
#define QZ_LOW_PAGES     0x100                    /* keep the first MB untouched */

static ULONG QzFreeBitmap[QZ_MAX_PAGES / 32];      /* 1 = free */
static ULONG QzTotalPages, QzFreePages, QzHighestPfn, QzSearchHint;
static ULONG QzTypePages[LoaderMaximum];

static PCSTR
QzMemoryTypeName(TYPE_OF_MEMORY Type)
{
    switch (Type)
    {
        case LoaderExceptionBlock:      return "ExceptionBlock";
        case LoaderSystemBlock:         return "SystemBlock";
        case LoaderFree:                return "Free";
        case LoaderBad:                 return "Bad";
        case LoaderLoadedProgram:       return "LoadedProgram";
        case LoaderFirmwareTemporary:   return "FirmwareTemporary";
        case LoaderFirmwarePermanent:   return "FirmwarePermanent";
        case LoaderOsloaderHeap:        return "OsloaderHeap";
        case LoaderOsloaderStack:       return "OsloaderStack";
        case LoaderSystemCode:          return "SystemCode";
        case LoaderHalCode:             return "HalCode";
        case LoaderBootDriver:          return "BootDriver";
        case LoaderConsoleInDriver:     return "ConsoleInDriver";
        case LoaderConsoleOutDriver:    return "ConsoleOutDriver";
        case LoaderStartupDpcStack:     return "StartupDpcStack";
        case LoaderStartupKernelStack:  return "StartupKernelStack";
        case LoaderStartupPanicStack:   return "StartupPanicStack";
        case LoaderStartupPcrPage:      return "StartupPcrPage";
        case LoaderStartupPdrPage:      return "StartupPdrPage";
        case LoaderRegistryData:        return "RegistryData";
        case LoaderMemoryData:          return "MemoryData";
        case LoaderNlsData:             return "NlsData";
        case LoaderSpecialMemory:       return "SpecialMemory";
        case LoaderBBTMemory:           return "BBTMemory";
        case LoaderReserve:             return "Reserve";
        case LoaderXIPRom:              return "XIPRom";
        case LoaderHALCachedMemory:     return "HALCachedMemory";
        default:                        return "Other";
    }
}

static __inline VOID
QzSetFree(ULONG Pfn)
{
    QzFreeBitmap[Pfn >> 5] |= (1UL << (Pfn & 31));
}

static __inline VOID
QzClearFree(ULONG Pfn)
{
    QzFreeBitmap[Pfn >> 5] &= ~(1UL << (Pfn & 31));
}

VOID
QzMmInit(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PLIST_ENTRY Entry;
    PMEMORY_ALLOCATION_DESCRIPTOR Md;
    ULONG Pfn;

    for (Entry = LoaderBlock->MemoryDescriptorListHead.Flink;
         Entry != &LoaderBlock->MemoryDescriptorListHead;
         Entry = Entry->Flink)
    {
        Md = CONTAINING_RECORD(Entry, MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);

        if (Md->MemoryType < LoaderMaximum)
            QzTypePages[Md->MemoryType] += Md->PageCount;

        if (Md->MemoryType == LoaderBad ||
            Md->MemoryType == LoaderFirmwarePermanent ||
            Md->MemoryType == LoaderSpecialMemory ||
            Md->MemoryType == LoaderBBTMemory)
        {
            continue;
        }

        QzTotalPages += Md->PageCount;
        if (Md->BasePage + Md->PageCount > QzHighestPfn)
            QzHighestPfn = Md->BasePage + Md->PageCount;

        if (Md->MemoryType != LoaderFree)
            continue;

        for (Pfn = Md->BasePage; Pfn < Md->BasePage + Md->PageCount; Pfn++)
        {
            if (Pfn < QZ_LOW_PAGES || Pfn >= QZ_MAX_PAGES)
                continue;
            QzSetFree(Pfn);
            QzFreePages++;
        }
    }

    if (QzHighestPfn > QZ_MAX_PAGES)
        QzHighestPfn = QZ_MAX_PAGES;
    QzSearchHint = QZ_LOW_PAGES;

    if (QzFreePages < 1024)
        KeBugCheckEx(INSTALL_MORE_MEMORY, QzTotalPages, QzFreePages, 0, 0);
}

ULONG
QzAllocPhysicalPage(VOID)
{
    ULONG Flags = QzSaveFlagsAndDisable();
    ULONG Index, Bit, Pfn, Pass;

    for (Pass = 0; Pass < 2; Pass++)
    {
        for (Index = QzSearchHint >> 5; Index < (QzHighestPfn + 31) >> 5; Index++)
        {
            if (!QzFreeBitmap[Index])
                continue;
            Bit = __builtin_ctz(QzFreeBitmap[Index]);
            Pfn = (Index << 5) | Bit;
            QzClearFree(Pfn);
            QzFreePages--;
            QzSearchHint = Pfn;
            QzRestoreFlags(Flags);
            return Pfn;
        }
        QzSearchHint = QZ_LOW_PAGES;
    }

    QzRestoreFlags(Flags);
    KeBugCheckEx(INSTALL_MORE_MEMORY, QzTotalPages, 0, 0, 1);
}

VOID
QzFreePhysicalPage(ULONG Pfn)
{
    ULONG Flags = QzSaveFlagsAndDisable();
    if (Pfn < QZ_MAX_PAGES && !(QzFreeBitmap[Pfn >> 5] & (1UL << (Pfn & 31))))
    {
        QzSetFree(Pfn);
        QzFreePages++;
        if (Pfn < QzSearchHint)
            QzSearchHint = Pfn;
    }
    QzRestoreFlags(Flags);
}

VOID
QzMapPage(ULONG_PTR Va, ULONG Pfn, ULONG Flags)
{
    volatile ULONG *Pde = (volatile ULONG *)(QZ_PDE_BASE + (Va >> 22) * 4);
    volatile ULONG *Pte = (volatile ULONG *)(QZ_PTE_BASE + (Va >> 12) * 4);
    ULONG_PTR PageTableVa = QZ_PTE_BASE + ((Va >> 22) << QZ_PAGE_SHIFT);
    ULONG IrqFlags = QzSaveFlagsAndDisable();

    if (!(*Pde & QZ_PTE_PRESENT))
    {
        /* Allocate a page table; it appears in the self-map immediately */
        *Pde = (QzAllocPhysicalPage() << QZ_PAGE_SHIFT) | QZ_PTE_PRESENT | QZ_PTE_WRITE;
        QzInvlpg(PageTableVa);
        RtlZeroMemory((PVOID)PageTableVa, QZ_PAGE_SIZE);
    }
    else if (*Pde & QZ_PTE_LARGE)
    {
        QzRestoreFlags(IrqFlags);
        KeBugCheckEx(NO_MORE_SYSTEM_PTES, Va, *Pde, 0, 0);
    }

    *Pte = (Pfn << QZ_PAGE_SHIFT) | (Flags & 0xFFF) | QZ_PTE_PRESENT;
    QzInvlpg(Va);
    QzRestoreFlags(IrqFlags);
}

VOID
QzUnmapPage(ULONG_PTR Va)
{
    volatile ULONG *Pde = (volatile ULONG *)(QZ_PDE_BASE + (Va >> 22) * 4);
    volatile ULONG *Pte = (volatile ULONG *)(QZ_PTE_BASE + (Va >> 12) * 4);

    if ((*Pde & QZ_PTE_PRESENT) && !(*Pde & QZ_PTE_LARGE))
    {
        *Pte = 0;
        QzInvlpg(Va);
    }
}

/*
 * The MmMapIoSpace/MmMapLockedPages window: a page bitmap, so that what
 * is unmapped can be mapped again (paging I/O maps and unmaps MDLs all
 * the time).
 */
#define QZ_IOSPACE_PAGES (QZ_IOSPACE_SIZE / QZ_PAGE_SIZE)
static ULONG QzIoSpaceBits[QZ_IOSPACE_PAGES / 32];
static RTL_BITMAP QzIoSpaceMap;
static ULONG QzIoSpaceHint;

static ULONG_PTR
QzIoSpaceAlloc(ULONG Pages)
{
    ULONG IrqFlags = QzSaveFlagsAndDisable(), First;

    if (!QzIoSpaceMap.Buffer)
        RtlInitializeBitMap(&QzIoSpaceMap, QzIoSpaceBits, QZ_IOSPACE_PAGES);
    First = RtlFindClearBitsAndSet(&QzIoSpaceMap, Pages, QzIoSpaceHint);
    if (First != 0xFFFFFFFF)
        QzIoSpaceHint = First + Pages;
    QzRestoreFlags(IrqFlags);
    return First == 0xFFFFFFFF ? 0 : QZ_IOSPACE_BASE + First * QZ_PAGE_SIZE;
}

VOID
QzIoSpaceFree(PVOID Address, ULONG Pages)
{
    ULONG_PTR Va = (ULONG_PTR)Address & ~(QZ_PAGE_SIZE - 1);
    ULONG IrqFlags, i;

    if (Va < QZ_IOSPACE_BASE || Va >= QZ_IOSPACE_BASE + QZ_IOSPACE_SIZE || !QzIoSpaceMap.Buffer)
        return;
    for (i = 0; i < Pages; i++)
        QzUnmapPage(Va + i * QZ_PAGE_SIZE);
    IrqFlags = QzSaveFlagsAndDisable();
    RtlClearBits(&QzIoSpaceMap, (ULONG)((Va - QZ_IOSPACE_BASE) / QZ_PAGE_SIZE), Pages);
    QzRestoreFlags(IrqFlags);
}

/* Map an arbitrary list of physical pages contiguously in system space */
PVOID
QzMapPages(PPFN_NUMBER Pages, ULONG Count, BOOLEAN Uncached)
{
    ULONG Flags = QZ_PTE_WRITE | (Uncached ? (QZ_PTE_PCD | QZ_PTE_PWT) : 0);
    ULONG_PTR Va = QzIoSpaceAlloc(Count);
    ULONG i;

    if (!Va)
        return NULL;
    for (i = 0; i < Count; i++)
        QzMapPage(Va + i * QZ_PAGE_SIZE, Pages[i], Flags);
    return (PVOID)Va;
}

PVOID
QzMapPhysical(ULONG PhysicalAddress, ULONG Size, BOOLEAN Uncached)
{
    ULONG Offset = PhysicalAddress & (QZ_PAGE_SIZE - 1);
    ULONG Pages = (Offset + Size + QZ_PAGE_SIZE - 1) >> QZ_PAGE_SHIFT;
    ULONG Flags = QZ_PTE_WRITE | (Uncached ? (QZ_PTE_PCD | QZ_PTE_PWT) : 0);
    ULONG_PTR Va = QzIoSpaceAlloc(Pages);
    ULONG i;

    if (!Va)
        return NULL;
    for (i = 0; i < Pages; i++)
        QzMapPage(Va + i * QZ_PAGE_SIZE, (PhysicalAddress >> QZ_PAGE_SHIFT) + i, Flags);
    return (PVOID)(Va + Offset);
}

VOID
QzMmGetStats(PULONG TotalPages, PULONG FreePages)
{
    *TotalPages = QzTotalPages;
    *FreePages = QzFreePages;
}

VOID
QzMmDumpInfo(VOID)
{
    ULONG Type;

    QzPrint("  Physical memory: %lu KB usable, %lu KB free, highest PFN 0x%lx\n",
            QzTotalPages * 4, QzFreePages * 4, QzHighestPfn);
    QzPrint("  Loader memory descriptors (pages by type):\n");
    for (Type = 0; Type < LoaderMaximum; Type++)
    {
        if (QzTypePages[Type])
            QzPrint("    %-20s %8lu pages (%lu KB)\n",
                    QzMemoryTypeName((TYPE_OF_MEMORY)Type),
                    QzTypePages[Type], QzTypePages[Type] * 4);
    }
    QzPrint("  Page directory at CR3=0x%08lx, self-map at 0x%08lx\n",
            QzReadCr3(), QZ_PTE_BASE);
}

/* ---------------------------------------------------------------------- */
/* Exports                                                                 */
/* ---------------------------------------------------------------------- */

PVOID
NTAPI
MmMapIoSpace(IN PHYSICAL_ADDRESS PhysicalAddress,
             IN SIZE_T NumberOfBytes,
             IN MEMORY_CACHING_TYPE CacheType)
{
    if (PhysicalAddress.HighPart != 0 || NumberOfBytes == 0)
        return NULL;
    return QzMapPhysical(PhysicalAddress.LowPart, (ULONG)NumberOfBytes,
                         CacheType != MmCached);
}

VOID
NTAPI
MmUnmapIoSpace(IN PVOID BaseAddress,
               IN SIZE_T NumberOfBytes)
{
    QzIoSpaceFree(BaseAddress, ADDRESS_AND_SIZE_TO_SPAN_PAGES(BaseAddress, NumberOfBytes));
}

PHYSICAL_ADDRESS
NTAPI
MmGetPhysicalAddress(IN PVOID Address)
{
    PHYSICAL_ADDRESS Result;
    ULONG_PTR Va = (ULONG_PTR)Address;
    ULONG Pde = *(volatile ULONG *)(QZ_PDE_BASE + (Va >> 22) * 4);
    ULONG Pte;

    Result.QuadPart = 0;
    if (!(Pde & QZ_PTE_PRESENT))
        return Result;

    if (Pde & QZ_PTE_LARGE)
    {
        Result.LowPart = (Pde & 0xFFC00000) | (Va & 0x003FFFFF);
        return Result;
    }

    Pte = *(volatile ULONG *)(QZ_PTE_BASE + (Va >> 12) * 4);
    if (Pte & QZ_PTE_PRESENT)
        Result.LowPart = (Pte & 0xFFFFF000) | (Va & 0xFFF);
    return Result;
}

/* ---------------------------------------------------------------------- */
/* Physically contiguous memory (the HAL's ISA DMA map buffers use it)     */
/* ---------------------------------------------------------------------- */

PVOID
NTAPI
MmAllocateContiguousMemorySpecifyCache(IN SIZE_T NumberOfBytes,
                                       IN PHYSICAL_ADDRESS LowestAcceptableAddress,
                                       IN PHYSICAL_ADDRESS HighestAcceptableAddress,
                                       IN PHYSICAL_ADDRESS BoundaryAddressMultiple,
                                       IN MEMORY_CACHING_TYPE CacheType)
{
    ULONG Pages = (ULONG)((NumberOfBytes + QZ_PAGE_SIZE - 1) >> QZ_PAGE_SHIFT);
    ULONG Low = (ULONG)(LowestAcceptableAddress.QuadPart >> QZ_PAGE_SHIFT);
    ULONGLONG HighAddress = HighestAcceptableAddress.QuadPart;
    ULONG High, Boundary, Start, Pfn, Flags;
    PVOID Va;

    if (Pages == 0)
        return NULL;
    if (HighAddress >= ((ULONGLONG)QzHighestPfn << QZ_PAGE_SHIFT))
        HighAddress = ((ULONGLONG)QzHighestPfn << QZ_PAGE_SHIFT) - 1;
    High = (ULONG)(HighAddress >> QZ_PAGE_SHIFT);       /* last acceptable PFN */
    Boundary = (ULONG)(BoundaryAddressMultiple.QuadPart >> QZ_PAGE_SHIFT);
    if (Low < QZ_LOW_PAGES)
        Low = QZ_LOW_PAGES;

    Flags = QzSaveFlagsAndDisable();
    for (Start = Low; Start + Pages - 1 <= High; Start++)
    {
        /* The run must not cross a boundary multiple */
        if (Boundary && (Start / Boundary) != ((Start + Pages - 1) / Boundary))
            continue;
        for (Pfn = Start; Pfn < Start + Pages; Pfn++)
        {
            if (!(QzFreeBitmap[Pfn >> 5] & (1UL << (Pfn & 31))))
                break;
        }
        if (Pfn == Start + Pages)
        {
            for (Pfn = Start; Pfn < Start + Pages; Pfn++)
                QzClearFree(Pfn);
            QzFreePages -= Pages;
            QzRestoreFlags(Flags);

            Va = QzMapPhysical(Start << QZ_PAGE_SHIFT, Pages << QZ_PAGE_SHIFT,
                               CacheType != MmCached);
            if (Va)
                RtlZeroMemory(Va, Pages << QZ_PAGE_SHIFT);
            return Va;
        }
        Start = Pfn;    /* skip past the page that was in use */
    }
    QzRestoreFlags(Flags);
    return NULL;
}

VOID
NTAPI
MmFreeContiguousMemorySpecifyCache(IN PVOID BaseAddress,
                                   IN SIZE_T NumberOfBytes,
                                   IN MEMORY_CACHING_TYPE CacheType)
{
    ULONG_PTR Va = (ULONG_PTR)BaseAddress & ~(QZ_PAGE_SIZE - 1);
    ULONG Pages = (ULONG)((NumberOfBytes + QZ_PAGE_SIZE - 1) >> QZ_PAGE_SHIFT);
    ULONG i;

    UNREFERENCED_PARAMETER(CacheType);

    for (i = 0; i < Pages; i++)
    {
        PHYSICAL_ADDRESS Pa = MmGetPhysicalAddress((PVOID)(Va + i * QZ_PAGE_SIZE));
        QzUnmapPage(Va + i * QZ_PAGE_SIZE);
        if (Pa.QuadPart)
            QzFreePhysicalPage((ULONG)(Pa.QuadPart >> QZ_PAGE_SHIFT));
    }
}

BOOLEAN
NTAPI
MmIsAddressValid(IN PVOID VirtualAddress)
{
    ULONG_PTR Va = (ULONG_PTR)VirtualAddress;
    ULONG Pde = *(volatile ULONG *)(QZ_PDE_BASE + (Va >> 22) * 4);

    if (!(Pde & QZ_PTE_PRESENT))
        return FALSE;
    if (Pde & QZ_PTE_LARGE)
        return TRUE;
    return (*(volatile ULONG *)(QZ_PTE_BASE + (Va >> 12) * 4) & QZ_PTE_PRESENT) != 0;
}

/* ---------------------------------------------------------------------- */
/* Reserved system mappings (a driver maps different MDLs into one range)  */
/* ---------------------------------------------------------------------- */

typedef struct _QZ_RESERVED_MAPPING
{
    ULONG Tag;
    ULONG Pages;
} QZ_RESERVED_MAPPING;

PVOID
NTAPI
MmAllocateMappingAddress(IN SIZE_T NumberOfBytes, IN ULONG PoolTag)
{
    ULONG Pages = (ULONG)((NumberOfBytes + QZ_PAGE_SIZE - 1) >> QZ_PAGE_SHIFT) + 1;
    ULONG_PTR Va = QzIoSpaceAlloc(Pages);

    if (!Va)
        return NULL;

    /* The first page records the reservation; callers get the rest */
    QzMapPage(Va, QzAllocPhysicalPage(), QZ_PTE_WRITE);
    ((QZ_RESERVED_MAPPING *)Va)->Tag = PoolTag;
    ((QZ_RESERVED_MAPPING *)Va)->Pages = Pages - 1;
    return (PVOID)(Va + QZ_PAGE_SIZE);
}

VOID
NTAPI
MmFreeMappingAddress(IN PVOID BaseAddress, IN ULONG PoolTag)
{
    QZ_RESERVED_MAPPING *Reservation = (QZ_RESERVED_MAPPING *)((ULONG_PTR)BaseAddress - QZ_PAGE_SIZE);
    ULONG Pages = Reservation->Pages + 1, Pte;

    UNREFERENCED_PARAMETER(PoolTag);
    Pte = *(volatile ULONG *)(QZ_PTE_BASE + (((ULONG_PTR)Reservation) >> 12) * 4);
    QzIoSpaceFree(Reservation, Pages);
    if (Pte & QZ_PTE_PRESENT)
        QzFreePhysicalPage(Pte >> QZ_PAGE_SHIFT);
}

PVOID
NTAPI
MmMapLockedPagesWithReservedMapping(IN PVOID MappingAddress, IN ULONG PoolTag, IN PMDL Mdl,
                                    IN MEMORY_CACHING_TYPE CacheType)
{
    QZ_RESERVED_MAPPING *Reservation = (QZ_RESERVED_MAPPING *)((ULONG_PTR)MappingAddress - QZ_PAGE_SIZE);
    ULONG Pages = ADDRESS_AND_SIZE_TO_SPAN_PAGES((PUCHAR)Mdl->StartVa + Mdl->ByteOffset, Mdl->ByteCount);
    PPFN_NUMBER Pfns = MmGetMdlPfnArray(Mdl);
    ULONG Flags = QZ_PTE_WRITE | (CacheType != MmCached ? (QZ_PTE_PCD | QZ_PTE_PWT) : 0), i;

    UNREFERENCED_PARAMETER(PoolTag);
    if (Pages > Reservation->Pages)
        return NULL;
    for (i = 0; i < Pages; i++)
        QzMapPage((ULONG_PTR)MappingAddress + i * QZ_PAGE_SIZE, (ULONG)Pfns[i], Flags);
    return (PUCHAR)MappingAddress + Mdl->ByteOffset;
}

VOID
NTAPI
MmUnmapReservedMapping(IN PVOID BaseAddress, IN ULONG PoolTag, IN PMDL Mdl)
{
    ULONG Pages = ADDRESS_AND_SIZE_TO_SPAN_PAGES((PUCHAR)Mdl->StartVa + Mdl->ByteOffset, Mdl->ByteCount);
    ULONG i;

    UNREFERENCED_PARAMETER(PoolTag);
    for (i = 0; i < Pages; i++)
        QzUnmapPage((ULONG_PTR)PAGE_ALIGN(BaseAddress) + i * QZ_PAGE_SIZE);
}

/* ---------------------------------------------------------------------- */
/* MmGetSystemRoutineAddress: only what is really implemented             */
/* ---------------------------------------------------------------------- */

#include "exportnames.h"

static int
QzCompareExportName(PCSTR Name, ULONG Index)
{
    return strcmp(Name, QzImplementedExports[Index]);
}

BOOLEAN
QzIsExportImplemented(PCSTR Name)
{
    ULONG Low = 0, High = RTL_NUMBER_OF(QzImplementedExports);
    while (Low < High)
    {
        ULONG Mid = (Low + High) / 2;
        int Order = QzCompareExportName(Name, Mid);
        if (!Order)
            return TRUE;
        if (Order < 0)
            High = Mid;
        else
            Low = Mid + 1;
    }
    return FALSE;
}

PVOID
NTAPI
MmGetSystemRoutineAddress(IN PUNICODE_STRING SystemRoutineName)
{
    CHAR Name[96];
    ULONG i, Low = 0, High = RTL_NUMBER_OF(QzImplementedExports);
    PLIST_ENTRY Entry;

    if (SystemRoutineName->Length / sizeof(WCHAR) >= sizeof(Name))
        return NULL;
    for (i = 0; i < SystemRoutineName->Length / sizeof(WCHAR); i++)
        Name[i] = (CHAR)SystemRoutineName->Buffer[i];
    Name[i] = 0;

    /* The kernel: a stub is as good as absent, callers fall back */
    while (Low < High)
    {
        ULONG Mid = (Low + High) / 2;
        int Order = QzCompareExportName(Name, Mid);
        if (!Order)
            return QzFindKernelExport(Name);
        if (Order < 0)
            High = Mid;
        else
            Low = Mid + 1;
    }

    /* Then the HAL (the second module the loader lists) */
    Entry = QzLoaderBlock->LoadOrderListHead.Flink->Flink;
    if (Entry != &QzLoaderBlock->LoadOrderListHead)
        return QzFindImageExport(CONTAINING_RECORD(Entry, QZ_LDR_ENTRY, InLoadOrderLinks)->DllBase, Name);
    return NULL;
}

/* The old contiguous and non-cached allocators, on top of the new one.
   MmFreeContiguousMemory gets no size: remember it */
static struct { PVOID Base; SIZE_T Bytes; } QzContiguous[64];

PVOID
NTAPI
MmAllocateContiguousMemory(IN SIZE_T NumberOfBytes,
                           IN PHYSICAL_ADDRESS HighestAcceptableAddress)
{
    PHYSICAL_ADDRESS Low, Boundary;
    PVOID Base;
    ULONG i, Flags;

    Low.QuadPart = 0;
    Boundary.QuadPart = 0;
    Base = MmAllocateContiguousMemorySpecifyCache(NumberOfBytes, Low, HighestAcceptableAddress, Boundary,
                                                  MmCached);
    if (!Base)
        return NULL;
    Flags = QzSaveFlagsAndDisable();
    for (i = 0; i < RTL_NUMBER_OF(QzContiguous); i++)
    {
        if (!QzContiguous[i].Base)
        {
            QzContiguous[i].Base = Base;
            QzContiguous[i].Bytes = NumberOfBytes;
            break;
        }
    }
    QzRestoreFlags(Flags);
    return Base;
}

VOID
NTAPI
MmFreeContiguousMemory(IN PVOID BaseAddress)
{
    SIZE_T Bytes = 0;
    ULONG i, Flags = QzSaveFlagsAndDisable();

    for (i = 0; i < RTL_NUMBER_OF(QzContiguous); i++)
    {
        if (QzContiguous[i].Base == BaseAddress)
        {
            Bytes = QzContiguous[i].Bytes;
            QzContiguous[i].Base = NULL;
            break;
        }
    }
    QzRestoreFlags(Flags);
    if (Bytes)
        MmFreeContiguousMemorySpecifyCache(BaseAddress, Bytes, MmCached);
}

PVOID
NTAPI
MmAllocateNonCachedMemory(IN SIZE_T NumberOfBytes)
{
    PHYSICAL_ADDRESS Low, High, Boundary;
    Low.QuadPart = 0;
    High.QuadPart = 0xFFFFFFFF;
    Boundary.QuadPart = 0;
    return MmAllocateContiguousMemorySpecifyCache(NumberOfBytes, Low, High, Boundary, MmNonCached);
}

VOID
NTAPI
MmFreeNonCachedMemory(IN PVOID BaseAddress,
                      IN SIZE_T NumberOfBytes)
{
    MmFreeContiguousMemorySpecifyCache(BaseAddress, NumberOfBytes, MmNonCached);
}
