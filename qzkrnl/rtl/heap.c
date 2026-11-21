/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-mode RTL heaps (win32k's user and desktop heaps)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * A heap either lives inside memory the caller gives it (win32k's heaps
 * are sections mapped in system space and in its GUI processes, all of
 * it committed here) or, without a base, takes its blocks from pool.
 * Inside a region: a header, then blocks, each with a small header,
 * first fit over an address-ordered free list, neighbours merged on free.
 * The format is private: user mode only reads objects in such heaps, it
 * never walks them.
 */

#include "qz.h"

#define QZ_HEAP_SIGNATURE   0x70614871      /* 'qHap' */
#define QZ_HEAP_TAG         'pHzQ'
#define QZ_HEAP_ALIGN       8
#define QZ_BLOCK_BUSY       0x80000000UL

typedef struct _QZ_HEAP_BLOCK
{
    ULONG Size;                     /* whole block, header included; QZ_BLOCK_BUSY if allocated */
    ULONG Requested;                /* what the caller asked for (RtlSizeHeap) */
} QZ_HEAP_BLOCK, *PQZ_HEAP_BLOCK;

typedef struct _QZ_FREE_BLOCK
{
    QZ_HEAP_BLOCK Header;
    struct _QZ_FREE_BLOCK *Next;    /* address order */
} QZ_FREE_BLOCK, *PQZ_FREE_BLOCK;

typedef struct _QZ_HEAP
{
    ULONG Signature;
    ULONG Flags;
    BOOLEAN InPool;                 /* no region: blocks come from pool */
    BOOLEAN OwnsRegion;             /* the region was allocated by RtlCreateHeap */
    PUCHAR Start, End;              /* the blocks */
    PQZ_FREE_BLOCK FreeList;
    FAST_MUTEX Lock;
} QZ_HEAP, *PQZ_HEAP;

C_ASSERT(sizeof(QZ_HEAP_BLOCK) == QZ_HEAP_ALIGN);

static VOID
QzHeapLock(PQZ_HEAP Heap, ULONG Flags)
{
    if (!((Heap->Flags | Flags) & HEAP_NO_SERIALIZE))
        ExAcquireFastMutex(&Heap->Lock);
}

static VOID
QzHeapUnlock(PQZ_HEAP Heap, ULONG Flags)
{
    if (!((Heap->Flags | Flags) & HEAP_NO_SERIALIZE))
        ExReleaseFastMutex(&Heap->Lock);
}

PVOID
NTAPI
RtlCreateHeap(IN ULONG Flags, IN PVOID HeapBase OPTIONAL, IN SIZE_T ReserveSize OPTIONAL,
              IN SIZE_T CommitSize OPTIONAL, IN PVOID Lock OPTIONAL, IN PRTL_HEAP_PARAMETERS Parameters OPTIONAL)
{
    PQZ_HEAP Heap;
    PQZ_FREE_BLOCK First;
    ULONG_PTR Start;

    UNREFERENCED_PARAMETER(CommitSize);
    UNREFERENCED_PARAMETER(Lock);
    UNREFERENCED_PARAMETER(Parameters);
    if (!HeapBase)
    {
        /* A growable heap: a header, blocks from pool */
        Heap = ExAllocatePoolWithTag(NonPagedPool, sizeof(QZ_HEAP), QZ_HEAP_TAG);
        if (!Heap)
            return NULL;
        RtlZeroMemory(Heap, sizeof(*Heap));
        Heap->Signature = QZ_HEAP_SIGNATURE;
        Heap->Flags = Flags;
        Heap->InPool = TRUE;
        ExInitializeFastMutex(&Heap->Lock);
        return Heap;
    }
    if (ReserveSize < sizeof(QZ_HEAP) + 2 * sizeof(QZ_FREE_BLOCK))
        return NULL;
    Heap = HeapBase;
    RtlZeroMemory(Heap, sizeof(*Heap));
    Heap->Signature = QZ_HEAP_SIGNATURE;
    Heap->Flags = Flags;
    ExInitializeFastMutex(&Heap->Lock);
    Start = ((ULONG_PTR)(Heap + 1) + QZ_HEAP_ALIGN - 1) & ~(ULONG_PTR)(QZ_HEAP_ALIGN - 1);
    Heap->Start = (PUCHAR)Start;
    Heap->End = (PUCHAR)(((ULONG_PTR)HeapBase + ReserveSize) & ~(ULONG_PTR)(QZ_HEAP_ALIGN - 1));
    First = (PQZ_FREE_BLOCK)Heap->Start;
    First->Header.Size = (ULONG)(Heap->End - Heap->Start);
    First->Header.Requested = 0;
    First->Next = NULL;
    Heap->FreeList = First;
    return Heap;
}

PVOID
NTAPI
RtlDestroyHeap(IN PVOID HeapHandle)
{
    PQZ_HEAP Heap = HeapHandle;
    if (!Heap || Heap->Signature != QZ_HEAP_SIGNATURE)
        return HeapHandle;
    Heap->Signature = 0;
    if (Heap->InPool)
        ExFreePoolWithTag(Heap, QZ_HEAP_TAG);   /* blocks still allocated leak, as they would on NT */
    return NULL;
}

PVOID
NTAPI
RtlAllocateHeap(IN PVOID HeapHandle, IN ULONG Flags, IN SIZE_T Size)
{
    PQZ_HEAP Heap = HeapHandle;
    PQZ_FREE_BLOCK *Link, Block;
    ULONG Need;
    PVOID Result = NULL;

    if (!Heap || Heap->Signature != QZ_HEAP_SIGNATURE || Size > 0x7FFF0000)
        return NULL;
    Need = (ULONG)((Size + sizeof(QZ_HEAP_BLOCK) + QZ_HEAP_ALIGN - 1) & ~(QZ_HEAP_ALIGN - 1));
    if (Need < sizeof(QZ_FREE_BLOCK))
        Need = sizeof(QZ_FREE_BLOCK);

    if (Heap->InPool)
    {
        PQZ_HEAP_BLOCK Header = ExAllocatePoolWithTag(NonPagedPool, Need, QZ_HEAP_TAG);
        if (Header)
        {
            Header->Size = Need | QZ_BLOCK_BUSY;
            Header->Requested = (ULONG)Size;
            Result = Header + 1;
        }
    }
    else
    {
        QzHeapLock(Heap, Flags);
        for (Link = &Heap->FreeList; (Block = *Link) != NULL; Link = &Block->Next)
        {
            if (Block->Header.Size < Need)
                continue;
            if (Block->Header.Size - Need >= sizeof(QZ_FREE_BLOCK))
            {
                /* Split: the front is the allocation, the rest stays free */
                PQZ_FREE_BLOCK Rest = (PQZ_FREE_BLOCK)((PUCHAR)Block + Need);
                Rest->Header.Size = Block->Header.Size - Need;
                Rest->Header.Requested = 0;
                Rest->Next = Block->Next;
                *Link = Rest;
                Block->Header.Size = Need;
            }
            else
            {
                *Link = Block->Next;
            }
            Block->Header.Size |= QZ_BLOCK_BUSY;
            Block->Header.Requested = (ULONG)Size;
            Result = &Block->Header + 1;
            break;
        }
        QzHeapUnlock(Heap, Flags);
    }
    if (Result && ((Flags | Heap->Flags) & HEAP_ZERO_MEMORY))
        RtlZeroMemory(Result, Size);
    if (!Result && ((Flags | Heap->Flags) & HEAP_GENERATE_EXCEPTIONS))
        ExRaiseStatus(STATUS_NO_MEMORY);
    return Result;
}

BOOLEAN
NTAPI
RtlFreeHeap(IN PVOID HeapHandle, IN ULONG Flags, IN PVOID BaseAddress)
{
    PQZ_HEAP Heap = HeapHandle;
    PQZ_HEAP_BLOCK Header;
    PQZ_FREE_BLOCK Block, *Link, Prev = NULL;

    if (!BaseAddress)
        return TRUE;
    if (!Heap || Heap->Signature != QZ_HEAP_SIGNATURE)
        return FALSE;
    Header = (PQZ_HEAP_BLOCK)BaseAddress - 1;
    if (!(Header->Size & QZ_BLOCK_BUSY))
    {
        DbgPrint(QZ_KERNEL_NAME ": RtlFreeHeap: %p is not allocated in heap %p\n", BaseAddress, Heap);
        return FALSE;
    }
    if (Heap->InPool)
    {
        Header->Size = 0;
        ExFreePoolWithTag(Header, QZ_HEAP_TAG);
        return TRUE;
    }
    if ((PUCHAR)Header < Heap->Start || (PUCHAR)Header >= Heap->End)
        return FALSE;

    QzHeapLock(Heap, Flags);
    Block = (PQZ_FREE_BLOCK)Header;
    Block->Header.Size &= ~QZ_BLOCK_BUSY;
    for (Link = &Heap->FreeList; *Link && *Link < Block; Link = &(*Link)->Next)
        Prev = *Link;
    Block->Next = *Link;
    *Link = Block;
    /* Merge with the next block, then with the previous one */
    if (Block->Next && (PUCHAR)Block + Block->Header.Size == (PUCHAR)Block->Next)
    {
        Block->Header.Size += Block->Next->Header.Size;
        Block->Next = Block->Next->Next;
    }
    if (Prev && (PUCHAR)Prev + Prev->Header.Size == (PUCHAR)Block)
    {
        Prev->Header.Size += Block->Header.Size;
        Prev->Next = Block->Next;
    }
    QzHeapUnlock(Heap, Flags);
    return TRUE;
}

SIZE_T
NTAPI
RtlSizeHeap(IN PVOID HeapHandle, IN ULONG Flags, IN PVOID MemoryPointer)
{
    PQZ_HEAP_BLOCK Header = (PQZ_HEAP_BLOCK)MemoryPointer - 1;
    UNREFERENCED_PARAMETER(HeapHandle);
    UNREFERENCED_PARAMETER(Flags);
    if (!MemoryPointer || !(Header->Size & QZ_BLOCK_BUSY))
        return (SIZE_T)-1;
    return Header->Requested;
}

PVOID
NTAPI
RtlReAllocateHeap(IN PVOID HeapHandle, IN ULONG Flags, IN PVOID Ptr, IN SIZE_T Size)
{
    SIZE_T Old;
    PVOID New;

    if (!Ptr)
        return RtlAllocateHeap(HeapHandle, Flags, Size);
    Old = RtlSizeHeap(HeapHandle, Flags, Ptr);
    if (Old == (SIZE_T)-1)
        return NULL;
    if (Flags & HEAP_REALLOC_IN_PLACE_ONLY)
        return Size <= Old ? Ptr : NULL;
    New = RtlAllocateHeap(HeapHandle, Flags & ~HEAP_ZERO_MEMORY, Size);
    if (!New)
        return NULL;
    RtlCopyMemory(New, Ptr, min(Old, Size));
    if (Size > Old && (Flags & HEAP_ZERO_MEMORY))
        RtlZeroMemory((PUCHAR)New + Old, Size - Old);
    RtlFreeHeap(HeapHandle, Flags, Ptr);
    return New;
}

BOOLEAN
NTAPI
RtlLockHeap(IN PVOID HeapHandle)
{
    QzHeapLock(HeapHandle, 0);
    return TRUE;
}

BOOLEAN
NTAPI
RtlUnlockHeap(IN PVOID HeapHandle)
{
    QzHeapUnlock(HeapHandle, 0);
    return TRUE;
}
