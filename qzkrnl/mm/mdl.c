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

    if (Irp)
    {
        if (!SecondaryBuffer || !Irp->MdlAddress)
        {
            Irp->MdlAddress = Mdl;
        }
        else
        {
            PMDL Last = Irp->MdlAddress;
            while (Last->Next)
                Last = Last->Next;
            Last->Next = Mdl;
        }
    }
    return Mdl;
}

VOID
NTAPI
MmUnmapLockedPages(IN PVOID BaseAddress,
                   IN PMDL Mdl)
{
    ULONG Pages = ADDRESS_AND_SIZE_TO_SPAN_PAGES((PUCHAR)Mdl->StartVa + Mdl->ByteOffset,
                                                 Mdl->ByteCount);

    QzIoSpaceFree(BaseAddress, Pages);
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

/*
 * Everything is resident and every caller is in kernel mode until stage
 * B6/B7, so probing just records the physical pages. The buffer already
 * has a system address, which MmGetSystemAddressForMdlSafe returns.
 */
VOID
NTAPI
MmProbeAndLockPages(IN OUT PMDL Mdl,
                    IN KPROCESSOR_MODE AccessMode,
                    IN LOCK_OPERATION Operation)
{
    PPFN_NUMBER Pfns = MmGetMdlPfnArray(Mdl);
    PUCHAR Base = (PUCHAR)Mdl->StartVa;
    ULONG Pages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(Base + Mdl->ByteOffset, Mdl->ByteCount);
    ULONG i;

    UNREFERENCED_PARAMETER(AccessMode);
    UNREFERENCED_PARAMETER(Operation);

    for (i = 0; i < Pages; i++)
    {
        if (!MmIsAddressValid(Base + i * PAGE_SIZE))
            ExRaiseStatus(STATUS_ACCESS_VIOLATION);
        Pfns[i] = (PFN_NUMBER)(MmGetPhysicalAddress(Base + i * PAGE_SIZE).QuadPart >> PAGE_SHIFT);
    }
    Mdl->MappedSystemVa = Base + Mdl->ByteOffset;
    Mdl->MdlFlags |= MDL_PAGES_LOCKED | MDL_SOURCE_IS_NONPAGED_POOL;
}

VOID
NTAPI
MmUnlockPages(IN OUT PMDL Mdl)
{
    if ((Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA) && !(Mdl->MdlFlags & MDL_SOURCE_IS_NONPAGED_POOL))
        MmUnmapLockedPages(Mdl->MappedSystemVa, Mdl);
    Mdl->MdlFlags &= ~MDL_PAGES_LOCKED;
}

VOID
NTAPI
IoBuildPartialMdl(IN PMDL SourceMdl,
                  IN PMDL TargetMdl,
                  IN PVOID VirtualAddress,
                  IN ULONG Length)
{
    ULONG_PTR Offset = (PUCHAR)VirtualAddress - (PUCHAR)MmGetMdlVirtualAddress(SourceMdl);
    PPFN_NUMBER Source, Target = MmGetMdlPfnArray(TargetMdl);
    ULONG Pages, i;

    if (!Length)
        Length = MmGetMdlByteCount(SourceMdl) - (ULONG)Offset;
    Source = MmGetMdlPfnArray(SourceMdl) +
             (((ULONG_PTR)MmGetMdlVirtualAddress(SourceMdl) & (PAGE_SIZE - 1)) + Offset) / PAGE_SIZE;

    TargetMdl->StartVa = PAGE_ALIGN(VirtualAddress);
    TargetMdl->ByteOffset = BYTE_OFFSET(VirtualAddress);
    TargetMdl->ByteCount = Length;
    TargetMdl->MdlFlags &= (MDL_ALLOCATED_FIXED_SIZE | MDL_ALLOCATED_MUST_SUCCEED);
    TargetMdl->MdlFlags |= MDL_PARTIAL | (SourceMdl->MdlFlags & (MDL_SOURCE_IS_NONPAGED_POOL |
                                                                 MDL_PAGES_LOCKED));
    TargetMdl->Process = SourceMdl->Process;
    if (SourceMdl->MdlFlags & (MDL_SOURCE_IS_NONPAGED_POOL | MDL_MAPPED_TO_SYSTEM_VA))
    {
        TargetMdl->MappedSystemVa = (PUCHAR)SourceMdl->MappedSystemVa + Offset;
        if (SourceMdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA)
            TargetMdl->MdlFlags |= MDL_PARTIAL_HAS_BEEN_MAPPED;
    }
    Pages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(VirtualAddress, Length);
    for (i = 0; i < Pages; i++)
        Target[i] = Source[i];
}
