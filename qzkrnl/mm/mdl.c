/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Memory descriptor lists (MDLs) and their system mappings
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Enough of the MDL interface for the HAL (its x86 BIOS emulator maps the
 * first megabyte through an MDL in HalInitSystem phase 1) and for future
 * drivers: allocation, nonpaged pool MDLs, kernel-mode mapping. All memory
 * is resident (there is no pager yet), so "locked" is implicit.
 */

#include "qz.h"

PMDL
NTAPI
IoAllocateMdl(IN PVOID VirtualAddress,
              IN ULONG Length,
              IN BOOLEAN SecondaryBuffer,
              IN BOOLEAN ChargeQuota,
              IN OUT PIRP Irp OPTIONAL)
{
    ULONG Pages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(VirtualAddress, Length);
    PMDL Mdl;

    UNREFERENCED_PARAMETER(SecondaryBuffer);
    UNREFERENCED_PARAMETER(ChargeQuota);

    Mdl = ExAllocatePoolWithTag(NonPagedPool,
                                sizeof(MDL) + Pages * sizeof(PFN_NUMBER), 'ldMQ');
    if (!Mdl)
        return NULL;

    MmInitializeMdl(Mdl, VirtualAddress, Length);
    RtlZeroMemory(MmGetMdlPfnArray(Mdl), Pages * sizeof(PFN_NUMBER));

    /* IRP chaining arrives with the I/O manager (stage B4) */
    if (Irp && !Irp->MdlAddress)
        Irp->MdlAddress = Mdl;
    return Mdl;
}

VOID
NTAPI
MmUnmapLockedPages(IN PVOID BaseAddress,
                   IN PMDL Mdl)
{
    ULONG_PTR Va = (ULONG_PTR)PAGE_ALIGN(BaseAddress);
    ULONG Pages = ADDRESS_AND_SIZE_TO_SPAN_PAGES((PUCHAR)Mdl->StartVa + Mdl->ByteOffset,
                                                 Mdl->ByteCount);
    ULONG i;

    for (i = 0; i < Pages; i++)
        QzUnmapPage(Va + i * PAGE_SIZE);
    Mdl->MdlFlags &= ~MDL_MAPPED_TO_SYSTEM_VA;
    Mdl->MappedSystemVa = NULL;
}

VOID
NTAPI
IoFreeMdl(IN PMDL Mdl)
{
    if ((Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA) &&
        !(Mdl->MdlFlags & MDL_SOURCE_IS_NONPAGED_POOL))
    {
        MmUnmapLockedPages(Mdl->MappedSystemVa, Mdl);
    }
    ExFreePoolWithTag(Mdl, 'ldMQ');
}

VOID
NTAPI
MmBuildMdlForNonPagedPool(IN PMDL Mdl)
{
    PPFN_NUMBER Pfns = MmGetMdlPfnArray(Mdl);
    PUCHAR Base = (PUCHAR)Mdl->StartVa;
    ULONG Pages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(Base + Mdl->ByteOffset, Mdl->ByteCount);
    ULONG i;

    for (i = 0; i < Pages; i++)
        Pfns[i] = (PFN_NUMBER)(MmGetPhysicalAddress(Base + i * PAGE_SIZE).QuadPart >> PAGE_SHIFT);

    Mdl->MappedSystemVa = Base + Mdl->ByteOffset;
    Mdl->MdlFlags |= MDL_SOURCE_IS_NONPAGED_POOL;
}

PVOID
NTAPI
MmMapLockedPagesSpecifyCache(IN PMDL Mdl,
                             IN KPROCESSOR_MODE AccessMode,
                             IN MEMORY_CACHING_TYPE CacheType,
                             IN PVOID BaseAddress OPTIONAL,
                             IN ULONG BugCheckOnFailure,
                             IN ULONG Priority)
{
    ULONG Pages;
    PUCHAR Va;

    UNREFERENCED_PARAMETER(BaseAddress);
    UNREFERENCED_PARAMETER(Priority);

    if (Mdl->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA | MDL_SOURCE_IS_NONPAGED_POOL))
        return Mdl->MappedSystemVa;

    if (AccessMode != KernelMode)
        return NULL;            /* no user address spaces yet (stage B6) */

    Pages = ADDRESS_AND_SIZE_TO_SPAN_PAGES((PUCHAR)Mdl->StartVa + Mdl->ByteOffset, Mdl->ByteCount);
    Va = QzMapPages(MmGetMdlPfnArray(Mdl), Pages, CacheType != MmCached);
    if (!Va)
    {
        if (BugCheckOnFailure)
            KeBugCheckEx(NO_MORE_SYSTEM_PTES, 0, Pages, 0, 0);
        return NULL;
    }

    Mdl->MappedSystemVa = Va + Mdl->ByteOffset;
    Mdl->MdlFlags |= MDL_MAPPED_TO_SYSTEM_VA;
    return Mdl->MappedSystemVa;
}

PVOID
NTAPI
MmMapLockedPages(IN PMDL Mdl,
                 IN KPROCESSOR_MODE AccessMode)
{
    return MmMapLockedPagesSpecifyCache(Mdl, AccessMode, MmCached, NULL, TRUE,
                                        NormalPagePriority);
}
