/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Executive pool (ExAllocatePoolWithTag & co.)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * A single nonpaged pool: an address-ordered free list with coalescing on
 * top of a virtual window that grows on demand. Paged pool requests are
 * served from it as well until qzkrnl has a pager.
 */

#include "qz.h"

#define QZ_POOL_MAGIC_USED   0x6C507A51   /* 'QzPl' */
#define QZ_POOL_MAGIC_FREE   0x72467A51   /* 'QzFr' */
#define QZ_POOL_ALIGN        16

typedef struct _QZ_POOL_HEADER
{
    ULONG Size;                       /* whole block, header included */
    ULONG Tag;
    ULONG Magic;
    struct _QZ_POOL_HEADER *NextFree;
} QZ_POOL_HEADER, *PQZ_POOL_HEADER;

C_ASSERT(sizeof(QZ_POOL_HEADER) == QZ_POOL_ALIGN);

static PQZ_POOL_HEADER QzFreeList;
static ULONG_PTR QzPoolBreak = QZ_POOL_BASE;      /* end of carved space */
static ULONG_PTR QzPoolMapped = QZ_POOL_BASE;     /* end of mapped space */
static ULONG QzPoolBytesUsed, QzPoolAllocations;

VOID
QzPoolInit(VOID)
{
    QzFreeList = NULL;
}

static PQZ_POOL_HEADER
QzPoolGrow(ULONG Size)
{
    PQZ_POOL_HEADER Block;

    if (QzPoolBreak + Size > QZ_POOL_BASE + QZ_POOL_SIZE)
        return NULL;

    while (QzPoolMapped < QzPoolBreak + Size)
    {
        QzMapPage(QzPoolMapped, QzAllocPhysicalPage(), QZ_PTE_WRITE);
        QzPoolMapped += QZ_PAGE_SIZE;
    }

    Block = (PQZ_POOL_HEADER)QzPoolBreak;
    QzPoolBreak += Size;
    Block->Size = Size;
    return Block;
}

/* Address-ordered insert with coalescing; flags already saved */
static VOID
QzPoolInsertFree(PQZ_POOL_HEADER Block)
{
    PQZ_POOL_HEADER *Link, Prev = NULL;

    Block->Magic = QZ_POOL_MAGIC_FREE;
    for (Link = &QzFreeList; *Link && *Link < Block; Link = &(*Link)->NextFree)
        Prev = *Link;
    Block->NextFree = *Link;
    *Link = Block;

    if (Block->NextFree &&
        (PUCHAR)Block + Block->Size == (PUCHAR)Block->NextFree)
    {
        Block->Size += Block->NextFree->Size;
        Block->NextFree = Block->NextFree->NextFree;
    }
    if (Prev && (PUCHAR)Prev + Prev->Size == (PUCHAR)Block)
    {
        Prev->Size += Block->Size;
        Prev->NextFree = Block->NextFree;
    }
}

/* Bytes to skip at Start so the data after a header lands on Align */
static ULONG
QzPoolFrontGap(ULONG_PTR Start, ULONG Align)
{
    ULONG_PTR Data, Aligned;
    ULONG Gap;

    if (!Align)
        return 0;
    Data = Start + sizeof(QZ_POOL_HEADER);
    Aligned = (Data + Align - 1) & ~(ULONG_PTR)(Align - 1);
    Gap = (ULONG)(Aligned - Data);
    /* A non-empty gap must hold a free block of its own */
    if (Gap && Gap < 2 * sizeof(QZ_POOL_HEADER))
        Gap += Align;
    return Gap;
}

/*
 * As on NT, requests of a page or more get page-aligned memory (drivers
 * such as cdrom.sys check), smaller ones 16-byte aligned memory.
 */
PVOID
NTAPI
ExAllocatePoolWithTag(IN POOL_TYPE PoolType,
                      IN SIZE_T NumberOfBytes,
                      IN ULONG Tag)
{
    PQZ_POOL_HEADER *Link, Block = NULL, Rest;
    ULONG Size, Flags, Align = (NumberOfBytes >= QZ_PAGE_SIZE) ? QZ_PAGE_SIZE : 0, Gap;

    if (NumberOfBytes == 0 || NumberOfBytes > QZ_POOL_SIZE)
        return NULL;

    Size = (ULONG)((NumberOfBytes + sizeof(QZ_POOL_HEADER) + QZ_POOL_ALIGN - 1) &
                   ~(QZ_POOL_ALIGN - 1));

    Flags = QzSaveFlagsAndDisable();

    /* First fit (with room for the alignment gap) */
    for (Link = &QzFreeList; *Link; Link = &(*Link)->NextFree)
    {
        Gap = QzPoolFrontGap((ULONG_PTR)*Link, Align);
        if ((*Link)->Size >= Gap + Size)
        {
            Block = *Link;
            if (Gap)
            {
                /* The front part stays free; carve the aligned block after it */
                PQZ_POOL_HEADER Aligned = (PQZ_POOL_HEADER)((PUCHAR)Block + Gap);
                Aligned->Size = Block->Size - Gap;
                Aligned->NextFree = Block->NextFree;
                Block->Size = Gap;
                Block->NextFree = Aligned;
                Link = &Block->NextFree;
                Block = Aligned;
            }
            if (Block->Size - Size >= 2 * sizeof(QZ_POOL_HEADER))
            {
                Rest = (PQZ_POOL_HEADER)((PUCHAR)Block + Size);
                Rest->Size = Block->Size - Size;
                Rest->Magic = QZ_POOL_MAGIC_FREE;
                Rest->NextFree = Block->NextFree;
                Block->Size = Size;
                *Link = Rest;
            }
            else
            {
                *Link = Block->NextFree;
            }
            break;
        }
    }

    if (!Block)
    {
        /* Grow; an alignment gap at the old break becomes a free block */
        Gap = QzPoolFrontGap(QzPoolBreak, Align);
        if (Gap)
        {
            PQZ_POOL_HEADER Pad = QzPoolGrow(Gap);
            if (Pad)
                QzPoolInsertFree(Pad);
        }
        Block = QzPoolGrow(Size);
    }

    if (Block)
    {
        Block->Tag = Tag;
        Block->Magic = QZ_POOL_MAGIC_USED;
        Block->NextFree = NULL;
        QzPoolBytesUsed += Block->Size;
        QzPoolAllocations++;
    }

    QzRestoreFlags(Flags);

    if (!Block)
    {
        if (PoolType & 2) /* MustSucceed pool types */
            KeBugCheckEx(MUST_SUCCEED_POOL_EMPTY, NumberOfBytes, 0, 0, 0);
        return NULL;
    }
    return Block + 1;
}

PVOID
NTAPI
ExAllocatePool(IN POOL_TYPE PoolType,
               IN SIZE_T NumberOfBytes)
{
    return ExAllocatePoolWithTag(PoolType, NumberOfBytes, 'enoN');
}

VOID
NTAPI
ExFreePoolWithTag(IN PVOID P,
                  IN ULONG TagToFree)
{
    PQZ_POOL_HEADER Block = (PQZ_POOL_HEADER)P - 1;
    ULONG Flags;

    if ((ULONG_PTR)P < QZ_POOL_BASE + sizeof(QZ_POOL_HEADER) ||
        (ULONG_PTR)P >= QzPoolBreak ||
        Block->Magic != QZ_POOL_MAGIC_USED)
    {
        KeBugCheckEx(BAD_POOL_CALLER, 7, 0, (ULONG_PTR)P, 0);
    }
    if (TagToFree && (TagToFree & ~PROTECTED_POOL) != (Block->Tag & ~PROTECTED_POOL))
        KeBugCheckEx(BAD_POOL_CALLER, 0xA, (ULONG_PTR)P, Block->Tag, TagToFree);

    Flags = QzSaveFlagsAndDisable();

    QzPoolBytesUsed -= Block->Size;
    QzPoolAllocations--;
    QzPoolInsertFree(Block);

    QzRestoreFlags(Flags);
}

VOID
NTAPI
ExFreePool(IN PVOID P)
{
    ExFreePoolWithTag(P, 0);
}

VOID
QzPoolDumpInfo(VOID)
{
    PQZ_POOL_HEADER Block;
    ULONG FreeBlocks = 0, FreeBytes = 0;
    ULONG Flags = QzSaveFlagsAndDisable();

    for (Block = QzFreeList; Block; Block = Block->NextFree)
    {
        FreeBlocks++;
        FreeBytes += Block->Size;
    }
    QzRestoreFlags(Flags);

    QzPrint("  Nonpaged pool at 0x%08lx: %lu KB mapped, %lu allocations (%lu bytes),\n"
            "  %lu free blocks (%lu bytes) on the free list\n",
            QZ_POOL_BASE, (ULONG)(QzPoolMapped - QZ_POOL_BASE) / 1024,
            QzPoolAllocations, QzPoolBytesUsed, FreeBlocks, FreeBytes);
}
