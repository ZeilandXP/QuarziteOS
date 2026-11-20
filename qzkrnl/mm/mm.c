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
static ULONG_PTR QzIoSpaceNext = QZ_IOSPACE_BASE;
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

PVOID
QzMapPhysical(ULONG PhysicalAddress, ULONG Size, BOOLEAN Uncached)
{
    ULONG Offset = PhysicalAddress & (QZ_PAGE_SIZE - 1);
    ULONG Pages = (Offset + Size + QZ_PAGE_SIZE - 1) >> QZ_PAGE_SHIFT;
    ULONG Flags = QZ_PTE_WRITE | (Uncached ? (QZ_PTE_PCD | QZ_PTE_PWT) : 0);
    ULONG_PTR Va;
    ULONG i, IrqFlags;

    IrqFlags = QzSaveFlagsAndDisable();
    if (QzIoSpaceNext + Pages * QZ_PAGE_SIZE > QZ_IOSPACE_BASE + QZ_IOSPACE_SIZE)
    {
        QzRestoreFlags(IrqFlags);
        return NULL;
    }
    Va = QzIoSpaceNext;
    QzIoSpaceNext += Pages * QZ_PAGE_SIZE;
    QzRestoreFlags(IrqFlags);

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
    ULONG_PTR Va = (ULONG_PTR)BaseAddress & ~(QZ_PAGE_SIZE - 1);
    ULONG_PTR End = (ULONG_PTR)BaseAddress + NumberOfBytes;

    /* Virtual space is not recycled yet; just drop the translations */
    for (; Va < End; Va += QZ_PAGE_SIZE)
        QzUnmapPage(Va);
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
