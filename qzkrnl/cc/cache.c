/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Cache manager
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * A cached stream (one per SECTION_OBJECT_POINTERS) has a shared cache
 * map; each file object that caches it has a private map pointing there.
 * The stream's data lives in views: 256 KB of the file mapped at 256 KB
 * of system space in the cache arena (QZ_CACHE_BASE), each page there
 * only once something needs it. A view's Valid bits say which pages hold
 * the file's data, Dirty which hold changes, Busy which are being read
 * in. Pages come in by paging reads to the file system (IoPageRead on the
 * stream's file object) and go back by paging writes; the hardware dirty
 * bit of a view's PTE is collected along with the Dirty bits.
 *
 * A pin or map (BCB) covers a range of the file: inside one view it is
 * that view's address; across views the pages are mapped once more,
 * contiguously, in the alias window. Mapped files (mm/section.c) map the
 * same physical pages into processes, so cached and mapped I/O see one
 * copy of the data.
 *
 * The lazy writer, a system thread, writes dirty data back about once a
 * second (through the file system's AcquireForLazyWrite callbacks),
 * tears down cache maps nobody uses any more, and gives clean pages back
 * when memory gets short; views no one holds are recycled oldest first.
 */

#include "qzio.h"
#include "qzps.h"

#define QZ_CC_TAG           'cCzQ'
#define QZ_VIEW_SHIFT       18
#define QZ_VIEW_SIZE        (1UL << QZ_VIEW_SHIFT)              /* VACB_MAPPING_GRANULARITY */
#define QZ_VIEW_PAGES       (QZ_VIEW_SIZE / PAGE_SIZE)          /* 64: one bit each */
#define QZ_CACHE_BASE       0xA4000000UL
#define QZ_CACHE_SIZE       (192UL * 1024 * 1024)
#define QZ_CACHE_VIEWS      (QZ_CACHE_SIZE / QZ_VIEW_SIZE)
#define QZ_ALIAS_BASE       0xBC000000UL
#define QZ_ALIAS_SIZE       (32UL * 1024 * 1024)
#define QZ_ALIAS_PAGES      (QZ_ALIAS_SIZE / PAGE_SIZE)
#define QZ_VIEW_HASH        256
#define QZ_BCB_MAX_VIEWS    8
#define QZ_PTE_DIRTY_BIT    0x040
#define QZ_CACHE_MAP_TAG    0x6D43587A                          /* signature */

#define QzPteAt(Va)         ((volatile ULONG *)(QZ_PTE_BASE + ((ULONG_PTR)(Va) >> 12) * 4))
#define QzPageBit(p)        (1ULL << (p))

typedef struct _QZ_CACHE_MAP QZ_CACHE_MAP, *PQZ_CACHE_MAP;

typedef struct _QZ_VIEW
{
    LIST_ENTRY HashLink;
    LIST_ENTRY MapLink;
    LIST_ENTRY LruLink;
    PQZ_CACHE_MAP Map;
    ULONGLONG Index;                    /* file offset >> QZ_VIEW_SHIFT */
    PUCHAR Va;
    ULONG Slot;
    ULONGLONG Valid, Dirty, Busy;
    LONG Pins;                          /* BCBs, copies and MDLs in progress */
    LONG MapRefs;                       /* pages mapped into processes */
} QZ_VIEW, *PQZ_VIEW;

struct _QZ_CACHE_MAP
{
    ULONG Signature;
    PSECTION_OBJECT_POINTERS Sop;
    PFILE_OBJECT FileObject;            /* referenced: the paging I/O goes here */
    LONG OpenCount;                     /* private cache maps */
    LONG MmRefs;                        /* mapped-file sections */
    LARGE_INTEGER FileSize, ValidDataLength, AllocationSize;
    PCACHE_MANAGER_CALLBACKS Callbacks;
    PVOID LazyWriteContext;
    BOOLEAN PinAccess;
    BOOLEAN DisableWriteBehind;
    BOOLEAN Teardown;                   /* no users: the lazy writer deletes it */
    UCHAR IdlePasses;                   /* lazy writer passes it has been unused for */
    LIST_ENTRY Views;
    LIST_ENTRY Link;                    /* QzCcMaps */
    PCACHE_UNINITIALIZE_EVENT UninitializeEvents;
    PVOID LogHandle;
    PFLUSH_TO_LSN FlushToLsn;
};

typedef struct _QZ_PRIVATE_MAP
{
    CSHORT NodeTypeCode;
    CSHORT NodeByteSize;
    PQZ_CACHE_MAP Shared;
    PFILE_OBJECT FileObject;
} QZ_PRIVATE_MAP, *PQZ_PRIVATE_MAP;

typedef struct _QZ_BCB
{
    PUBLIC_BCB Public;                  /* NodeTypeCode, NodeByteSize, MappedLength, MappedFileOffset */
    PQZ_CACHE_MAP Map;
    LONG RefCount;
    BOOLEAN Pinned;
    ULONG ViewCount;
    PQZ_VIEW Views[QZ_BCB_MAX_VIEWS];
    PUCHAR Buffer;
    ULONG AliasFirst, AliasPages;       /* AliasPages 0: inside one view */
    LARGE_INTEGER Lsn;
} QZ_BCB, *PQZ_BCB;

/* An MDL of cache pages, with the view it pins */
typedef struct _QZ_CC_MDL
{
    PQZ_VIEW View;
    ULONG Pad;
    MDL Mdl;                            /* its PFN array follows */
} QZ_CC_MDL, *PQZ_CC_MDL;

static KSPIN_LOCK QzCcLock;
static LIST_ENTRY QzCcMaps, QzCcLru, QzCcHash[QZ_VIEW_HASH];
static ULONG QzCcSlotBits[QZ_CACHE_VIEWS / 32], QzCcAliasBits[QZ_ALIAS_PAGES / 32];
static RTL_BITMAP QzCcSlots, QzCcAlias;
static KEVENT QzCcViewEvent;            /* Busy pages finished */
static KEVENT QzCcLazyWake, QzCcLazyDone;
static BOOLEAN QzCcReady, QzCcLazyStarted;
static ULONG QzCcViewCount, QzCcLazyPasses;
static FAST_MUTEX QzCcInitLock;

ULONG CcFastReadNotPossible, CcFastReadWait, CcFastReadNoWait, CcFastReadResourceMiss;
LARGE_INTEGER CcIdleDelayTick;

static VOID NTAPI QzCcLazyWriter(PVOID Context);

VOID
QzCcInit(VOID)
{
    ULONG i;
    KeInitializeSpinLock(&QzCcLock);
    InitializeListHead(&QzCcMaps);
    InitializeListHead(&QzCcLru);
    for (i = 0; i < QZ_VIEW_HASH; i++)
        InitializeListHead(&QzCcHash[i]);
    RtlInitializeBitMap(&QzCcSlots, QzCcSlotBits, QZ_CACHE_VIEWS);
    RtlClearAllBits(&QzCcSlots);
    RtlInitializeBitMap(&QzCcAlias, QzCcAliasBits, QZ_ALIAS_PAGES);
    RtlClearAllBits(&QzCcAlias);
    KeInitializeEvent(&QzCcViewEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&QzCcLazyWake, SynchronizationEvent, FALSE);
    KeInitializeEvent(&QzCcLazyDone, NotificationEvent, FALSE);
    ExInitializeFastMutex(&QzCcInitLock);
    QzCcReady = TRUE;
}

static VOID
QzCcStartLazyWriter(VOID)
{
    HANDLE Thread;
    if (QzCcLazyStarted)
        return;
    ExAcquireFastMutex(&QzCcInitLock);
    if (!QzCcLazyStarted &&
        NT_SUCCESS(PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, QzCcLazyWriter, NULL)))
    {
        QzCcLazyStarted = TRUE;
        ZwClose(Thread);
    }
    ExReleaseFastMutex(&QzCcInitLock);
}

static PQZ_CACHE_MAP
QzCcMapOf(PFILE_OBJECT FileObject)
{
    PSECTION_OBJECT_POINTERS Sop = FileObject ? FileObject->SectionObjectPointer : NULL;
    PQZ_CACHE_MAP Map = Sop ? Sop->SharedCacheMap : NULL;
    return (Map && Map->Signature == QZ_CACHE_MAP_TAG) ? Map : NULL;
}

/* ---------------------------------------------------------------------- */
/* Views                                                                   */
/* ---------------------------------------------------------------------- */

static ULONG
QzCcHashOf(PQZ_CACHE_MAP Map, ULONGLONG Index)
{
    return (ULONG)(((ULONG_PTR)Map >> 5) ^ (ULONG)Index ^ (ULONG)(Index >> 32)) % QZ_VIEW_HASH;
}

static PQZ_VIEW
QzCcFindView(PQZ_CACHE_MAP Map, ULONGLONG Index)
{
    PLIST_ENTRY Head = &QzCcHash[QzCcHashOf(Map, Index)], Entry;
    for (Entry = Head->Flink; Entry != Head; Entry = Entry->Flink)
    {
        PQZ_VIEW View = CONTAINING_RECORD(Entry, QZ_VIEW, HashLink);
        if (View->Map == Map && View->Index == Index)
            return View;
    }
    return NULL;
}

/* Fold the hardware dirty bits of the view's pages into Dirty (lock held) */
static VOID
QzCcHarvest(PQZ_VIEW View)
{
    ULONGLONG Valid = View->Valid & ~View->Busy;
    ULONG p;
    for (p = 0; Valid && p < QZ_VIEW_PAGES; p++)
    {
        volatile ULONG *Pte;
        if (!(Valid & QzPageBit(p)))
            continue;
        Pte = QzPteAt(View->Va + p * PAGE_SIZE);
        if (*Pte & QZ_PTE_DIRTY_BIT)
        {
            *Pte &= ~QZ_PTE_DIRTY_BIT;
            __invlpg(View->Va + p * PAGE_SIZE);
            View->Dirty |= QzPageBit(p);
        }
    }
}

static VOID
QzCcFreePages(PQZ_VIEW View, ULONGLONG Mask)
{
    ULONG p;
    for (p = 0; Mask && p < QZ_VIEW_PAGES; p++)
    {
        ULONG Pte;
        if (!(Mask & QzPageBit(p)))
            continue;
        Pte = *QzPteAt(View->Va + p * PAGE_SIZE);
        QzUnmapPage((ULONG_PTR)View->Va + p * PAGE_SIZE);
        if (Pte & QZ_PTE_PRESENT)
            QzFreePhysicalPage(Pte >> PAGE_SHIFT);
    }
}

/* Unlink and free an unused view (lock held) */
static VOID
QzCcDropView(PQZ_VIEW View)
{
    QzCcFreePages(View, View->Valid);
    RemoveEntryList(&View->HashLink);
    RemoveEntryList(&View->MapLink);
    RemoveEntryList(&View->LruLink);
    RtlClearBits(&QzCcSlots, View->Slot, 1);
    QzCcViewCount--;
    ExFreePoolWithTag(View, QZ_CC_TAG);
}

static BOOLEAN
QzCcViewIdle(PQZ_VIEW View)
{
    return !View->Pins && !View->MapRefs && !View->Busy;
}

static PQZ_VIEW
QzCcOldestIdle(BOOLEAN CleanOnly)
{
    PLIST_ENTRY Entry;
    for (Entry = QzCcLru.Flink; Entry != &QzCcLru; Entry = Entry->Flink)
    {
        PQZ_VIEW View = CONTAINING_RECORD(Entry, QZ_VIEW, LruLink);
        if (!QzCcViewIdle(View))
            continue;
        QzCcHarvest(View);
        if (!CleanOnly || !(View->Dirty & View->Valid))
            return View;
    }
    return NULL;
}

static NTSTATUS QzCcWriteView(PQZ_VIEW View, ULONGLONG Mask);

/* The view for Index, created if need be; returned with a pin */
static NTSTATUS
QzCcReferenceView(PQZ_CACHE_MAP Map, ULONGLONG Index, PQZ_VIEW *Result)
{
    PQZ_VIEW View, Victim, New = NULL;
    ULONG Slot, Attempt;
    KIRQL OldIrql;

    for (Attempt = 0; Attempt < 4; Attempt++)
    {
        if (!New)
        {
            New = ExAllocatePoolWithTag(NonPagedPool, sizeof(QZ_VIEW), QZ_CC_TAG);
            if (!New)
                return STATUS_INSUFFICIENT_RESOURCES;
        }
        KeAcquireSpinLock(&QzCcLock, &OldIrql);
        View = QzCcFindView(Map, Index);
        if (View)
        {
            View->Pins++;
            RemoveEntryList(&View->LruLink);
            InsertTailList(&QzCcLru, &View->LruLink);
            KeReleaseSpinLock(&QzCcLock, OldIrql);
            ExFreePoolWithTag(New, QZ_CC_TAG);
            *Result = View;
            return STATUS_SUCCESS;
        }
        Slot = RtlFindClearBitsAndSet(&QzCcSlots, 1, 0);
        if (Slot == 0xFFFFFFFF && (Victim = QzCcOldestIdle(TRUE)) != NULL)
        {
            QzCcDropView(Victim);
            Slot = RtlFindClearBitsAndSet(&QzCcSlots, 1, 0);
        }
        if (Slot != 0xFFFFFFFF)
        {
            RtlZeroMemory(New, sizeof(*New));
            New->Map = Map;
            New->Index = Index;
            New->Slot = Slot;
            New->Va = (PUCHAR)(QZ_CACHE_BASE + Slot * QZ_VIEW_SIZE);
            New->Pins = 1;
            InsertTailList(&QzCcHash[QzCcHashOf(Map, Index)], &New->HashLink);
            InsertTailList(&Map->Views, &New->MapLink);
            InsertTailList(&QzCcLru, &New->LruLink);
            QzCcViewCount++;
            KeReleaseSpinLock(&QzCcLock, OldIrql);
            *Result = New;
            return STATUS_SUCCESS;
        }
        /* Every slot holds changes: write the oldest back, then try again */
        Victim = QzCcOldestIdle(FALSE);
        if (Victim)
            Victim->Pins++;
        KeReleaseSpinLock(&QzCcLock, OldIrql);
        if (!Victim)
            break;
        QzCcWriteView(Victim, ~0ULL);
        InterlockedDecrement(&Victim->Pins);
    }
    if (New)
        ExFreePoolWithTag(New, QZ_CC_TAG);
    return STATUS_INSUFFICIENT_RESOURCES;
}

static VOID
QzCcReleaseView(PQZ_VIEW View)
{
    KIRQL OldIrql;
    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    View->Pins--;
    KeReleaseSpinLock(&QzCcLock, OldIrql);
}

static ULONGLONG
QzCcRangeMask(ULONG First, ULONG Last)
{
    ULONGLONG Mask = (Last >= 63) ? ~0ULL : ((1ULL << (Last + 1)) - 1);
    return Mask & ~((1ULL << First) - 1);
}

/* An MDL for Count pages of the view from page First (they are mapped) */
static PMDL
QzCcBuildMdl(PQZ_VIEW View, ULONG First, ULONG Count, BOOLEAN Hold)
{
    ULONG Size = FIELD_OFFSET(QZ_CC_MDL, Mdl) + (ULONG)MmSizeOfMdl(NULL, Count * PAGE_SIZE);
    PQZ_CC_MDL Wrapper = ExAllocatePoolWithTag(NonPagedPool, Size, QZ_CC_TAG);
    PPFN_NUMBER Pfns;
    ULONG i;

    if (!Wrapper)
        return NULL;
    Wrapper->View = Hold ? View : NULL;
    MmInitializeMdl(&Wrapper->Mdl, View->Va + First * PAGE_SIZE, Count * PAGE_SIZE);
    Pfns = MmGetMdlPfnArray(&Wrapper->Mdl);
    for (i = 0; i < Count; i++)
        Pfns[i] = *QzPteAt(View->Va + (First + i) * PAGE_SIZE) >> PAGE_SHIFT;
    Wrapper->Mdl.MappedSystemVa = View->Va + First * PAGE_SIZE;
    Wrapper->Mdl.MdlFlags |= MDL_SOURCE_IS_NONPAGED_POOL | MDL_PAGES_LOCKED;
    return &Wrapper->Mdl;
}

static VOID
QzCcFreeMdl(PMDL Mdl)
{
    ExFreePoolWithTag(CONTAINING_RECORD(Mdl, QZ_CC_MDL, Mdl), QZ_CC_TAG);
}

/* One paging transfer of whole pages of a view */
static NTSTATUS
QzCcPagingIo(PQZ_VIEW View, ULONG First, ULONG Count, BOOLEAN Write)
{
    PMDL Mdl = QzCcBuildMdl(View, First, Count, FALSE);
    LARGE_INTEGER Offset;
    IO_STATUS_BLOCK Io;
    KEVENT Event;
    NTSTATUS Status;

    if (!Mdl)
        return STATUS_INSUFFICIENT_RESOURCES;
    Offset.QuadPart = (LONGLONG)(View->Index << QZ_VIEW_SHIFT) + First * PAGE_SIZE;
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Status = Write ? IoSynchronousPageWrite(View->Map->FileObject, Mdl, &Offset, &Event, &Io)
                   : IoPageRead(View->Map->FileObject, Mdl, &Offset, &Event, &Io);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Io.Status;
    }
    QzCcFreeMdl(Mdl);
    /* A read across the end of the file is fine: the rest is zero */
    if (!Write && Status == STATUS_END_OF_FILE)
        Status = STATUS_SUCCESS;
    return Status;
}

/*
 * Make pages First..Last of a (pinned) view valid. Pages in NoRead are
 * about to be overwritten whole: they are zeroed, not read. Without Wait,
 * STATUS_CANT_WAIT if that would take I/O.
 */
static NTSTATUS
QzCcFill(PQZ_VIEW View, ULONG First, ULONG Last, BOOLEAN Wait, ULONGLONG NoRead)
{
    PQZ_CACHE_MAP Map = View->Map;
    ULONGLONG Mask = QzCcRangeMask(First, Last), Need, Read;
    LONGLONG ViewStart = (LONGLONG)(View->Index << QZ_VIEW_SHIFT), Limit;
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;
    ULONG p;

    for (;;)
    {
        KeAcquireSpinLock(&QzCcLock, &OldIrql);
        Need = Mask & ~View->Valid;
        if (!Need)
        {
            KeReleaseSpinLock(&QzCcLock, OldIrql);
            return STATUS_SUCCESS;
        }
        if (Need & View->Busy)
        {
            KeClearEvent(&QzCcViewEvent);
            KeReleaseSpinLock(&QzCcLock, OldIrql);
            if (!Wait)
                return STATUS_CANT_WAIT;
            KeWaitForSingleObject(&QzCcViewEvent, Executive, KernelMode, FALSE, NULL);
            continue;
        }
        /* Pages past the valid data are zero: no I/O for them */
        Limit = min(Map->ValidDataLength.QuadPart, Map->FileSize.QuadPart);
        Read = 0;
        for (p = First; p <= Last; p++)
            if ((Need & QzPageBit(p)) && !(NoRead & QzPageBit(p)) && ViewStart + (LONGLONG)p * PAGE_SIZE < Limit)
                Read |= QzPageBit(p);
        if (Read && !Wait)
        {
            KeReleaseSpinLock(&QzCcLock, OldIrql);
            return STATUS_CANT_WAIT;
        }
        View->Busy |= Need;
        KeReleaseSpinLock(&QzCcLock, OldIrql);
        break;
    }

    for (p = First; p <= Last; p++)
    {
        if (!(Need & QzPageBit(p)))
            continue;
        QzMapPage((ULONG_PTR)View->Va + p * PAGE_SIZE, QzAllocPhysicalPage(), QZ_PTE_WRITE);
        if (!(Read & QzPageBit(p)))
            RtlZeroMemory(View->Va + p * PAGE_SIZE, PAGE_SIZE);
    }
    /* Read the runs of pages that need it */
    for (p = First; p <= Last && NT_SUCCESS(Status);)
    {
        ULONG Run = p;
        if (!(Read & QzPageBit(p)))
        {
            p++;
            continue;
        }
        while (Run <= Last && (Read & QzPageBit(Run)))
            Run++;
        Status = QzCcPagingIo(View, p, Run - p, FALSE);
        p = Run;
    }

    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    if (NT_SUCCESS(Status))
    {
        View->Valid |= Need;
        /* Reading set no dirty bits worth keeping */
        for (p = First; p <= Last; p++)
            if (Need & QzPageBit(p))
            {
                *QzPteAt(View->Va + p * PAGE_SIZE) &= ~QZ_PTE_DIRTY_BIT;
                __invlpg(View->Va + p * PAGE_SIZE);
            }
    }
    else
    {
        QzCcFreePages(View, Need);
    }
    View->Busy &= ~Need;
    KeReleaseSpinLock(&QzCcLock, OldIrql);
    KeSetEvent(&QzCcViewEvent, IO_NO_INCREMENT, FALSE);
    return Status;
}

static VOID
QzCcSetDirty(PQZ_VIEW View, ULONG First, ULONG Last)
{
    KIRQL OldIrql;
    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    View->Dirty |= QzCcRangeMask(First, Last) & View->Valid;
    KeReleaseSpinLock(&QzCcLock, OldIrql);
}

/* Write a (pinned) view's dirty pages in Mask back to the file */
static NTSTATUS
QzCcWriteView(PQZ_VIEW View, ULONGLONG Mask)
{
    PQZ_CACHE_MAP Map = View->Map;
    LONGLONG ViewStart = (LONGLONG)(View->Index << QZ_VIEW_SHIFT);
    ULONGLONG Dirty;
    NTSTATUS Status = STATUS_SUCCESS, Result;
    KIRQL OldIrql;
    ULONG p, Pages;

    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    QzCcHarvest(View);
    Dirty = View->Dirty & View->Valid & ~View->Busy & Mask;
    View->Dirty &= ~Dirty;
    /* Nothing past the end of the file goes to disk */
    if (Map->FileSize.QuadPart <= ViewStart)
        Pages = 0;
    else
        Pages = (ULONG)min(QZ_VIEW_PAGES, (Map->FileSize.QuadPart - ViewStart + PAGE_SIZE - 1) / PAGE_SIZE);
    if (Pages < QZ_VIEW_PAGES)
        Dirty &= QzCcRangeMask(0, Pages ? Pages - 1 : 0) & (Pages ? ~0ULL : 0);
    KeReleaseSpinLock(&QzCcLock, OldIrql);

    for (p = 0; p < QZ_VIEW_PAGES && Dirty;)
    {
        ULONG Run = p;
        if (!(Dirty & QzPageBit(p)))
        {
            p++;
            continue;
        }
        while (Run < QZ_VIEW_PAGES && (Dirty & QzPageBit(Run)))
            Run++;
        Result = QzCcPagingIo(View, p, Run - p, TRUE);
        if (!NT_SUCCESS(Result))
        {
            KeAcquireSpinLock(&QzCcLock, &OldIrql);
            View->Dirty |= QzCcRangeMask(p, Run - 1);
            KeReleaseSpinLock(&QzCcLock, OldIrql);
            DbgPrint(QZ_KERNEL_NAME ": cache: writing %wZ at 0x%I64x failed: 0x%08lx\n",
                     &Map->FileObject->FileName, ViewStart + p * PAGE_SIZE, Result);
            Status = Result;
        }
        Dirty &= ~QzCcRangeMask(p, Run - 1);
        p = Run;
    }
    return Status;
}

/* Pinned views of Map overlapping [Start, End) (or all), for flushing */
static ULONG
QzCcCollectViews(PQZ_CACHE_MAP Map, LONGLONG Start, LONGLONG End, PQZ_VIEW *Views, ULONG Max)
{
    PLIST_ENTRY Entry;
    ULONG Count = 0;
    KIRQL OldIrql;

    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    for (Entry = Map->Views.Flink; Entry != &Map->Views && Count < Max; Entry = Entry->Flink)
    {
        PQZ_VIEW View = CONTAINING_RECORD(Entry, QZ_VIEW, MapLink);
        LONGLONG ViewStart = (LONGLONG)(View->Index << QZ_VIEW_SHIFT);
        if (ViewStart + QZ_VIEW_SIZE <= Start || ViewStart >= End)
            continue;
        QzCcHarvest(View);
        if (!(View->Dirty & View->Valid))
            continue;
        View->Pins++;
        Views[Count++] = View;
    }
    KeReleaseSpinLock(&QzCcLock, OldIrql);
    return Count;
}

VOID (*QzMmCollectFileDirty)(PSECTION_OBJECT_POINTERS Sop);   /* mm/section.c */

static NTSTATUS
QzCcFlushRange(PQZ_CACHE_MAP Map, LONGLONG Start, LONGLONG End)
{
    PQZ_VIEW Views[32];
    NTSTATUS Status = STATUS_SUCCESS, Result;
    ULONG Count, i;

    if (QzMmCollectFileDirty)
        QzMmCollectFileDirty(Map->Sop);
    do
    {
        Count = QzCcCollectViews(Map, Start, End, Views, RTL_NUMBER_OF(Views));
        for (i = 0; i < Count; i++)
        {
            LONGLONG ViewStart = (LONGLONG)(Views[i]->Index << QZ_VIEW_SHIFT);
            ULONG First = (ULONG)(max(Start, ViewStart) - ViewStart) / PAGE_SIZE;
            ULONG Last = (ULONG)((min(End, ViewStart + QZ_VIEW_SIZE) - ViewStart + PAGE_SIZE - 1) / PAGE_SIZE) - 1;
            Result = QzCcWriteView(Views[i], QzCcRangeMask(First, Last));
            if (!NT_SUCCESS(Result))
                Status = Result;
            QzCcReleaseView(Views[i]);
        }
    } while (Count == RTL_NUMBER_OF(Views) && NT_SUCCESS(Status));
    return Status;
}

static BOOLEAN
QzCcMapIsDirty(PQZ_CACHE_MAP Map)
{
    PLIST_ENTRY Entry;
    BOOLEAN Dirty = FALSE;
    KIRQL OldIrql;

    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    for (Entry = Map->Views.Flink; Entry != &Map->Views && !Dirty; Entry = Entry->Flink)
    {
        PQZ_VIEW View = CONTAINING_RECORD(Entry, QZ_VIEW, MapLink);
        QzCcHarvest(View);
        Dirty = (View->Dirty & View->Valid) != 0;
    }
    KeReleaseSpinLock(&QzCcLock, OldIrql);
    return Dirty;
}

/* Drop the pages of [Start, End) (no write); FALSE if some are in use */
static BOOLEAN
QzCcPurge(PQZ_CACHE_MAP Map, LONGLONG Start, LONGLONG End)
{
    PLIST_ENTRY Entry, Next;
    BOOLEAN Result = TRUE;
    KIRQL OldIrql;

    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    for (Entry = Map->Views.Flink; Entry != &Map->Views; Entry = Next)
    {
        PQZ_VIEW View = CONTAINING_RECORD(Entry, QZ_VIEW, MapLink);
        LONGLONG ViewStart = (LONGLONG)(View->Index << QZ_VIEW_SHIFT);
        ULONGLONG Mask;
        Next = Entry->Flink;
        if (ViewStart + QZ_VIEW_SIZE <= Start || ViewStart >= End)
            continue;
        if (!QzCcViewIdle(View))
        {
            Result = FALSE;
            continue;
        }
        if (Start <= ViewStart && End >= ViewStart + QZ_VIEW_SIZE)
        {
            QzCcDropView(View);
            continue;
        }
        /* Only the pages wholly inside the range go */
        {
            LONGLONG From = max(Start, ViewStart), To = min(End, ViewStart + QZ_VIEW_SIZE);
            ULONG First = (ULONG)((From - ViewStart + PAGE_SIZE - 1) / PAGE_SIZE);
            ULONG Last = (ULONG)((To - ViewStart) / PAGE_SIZE);
            if (Last <= First)
                continue;
            Mask = QzCcRangeMask(First, Last - 1) & View->Valid;
            QzCcFreePages(View, Mask);
            View->Valid &= ~Mask;
            View->Dirty &= ~Mask;
        }
    }
    KeReleaseSpinLock(&QzCcLock, OldIrql);
    return Result;
}

/* ---------------------------------------------------------------------- */
/* Cache maps                                                              */
/* ---------------------------------------------------------------------- */

static PQZ_CACHE_MAP
QzCcCreateMap(PFILE_OBJECT FileObject, PCC_FILE_SIZES Sizes)
{
    PSECTION_OBJECT_POINTERS Sop = FileObject->SectionObjectPointer;
    PQZ_CACHE_MAP Map = ExAllocatePoolWithTag(NonPagedPool, sizeof(QZ_CACHE_MAP), QZ_CC_TAG), Existing;
    KIRQL OldIrql;

    if (!Map)
        return NULL;
    RtlZeroMemory(Map, sizeof(*Map));
    Map->Signature = QZ_CACHE_MAP_TAG;
    Map->Sop = Sop;
    Map->FileObject = FileObject;
    InitializeListHead(&Map->Views);
    if (Sizes)
    {
        Map->FileSize = Sizes->FileSize;
        Map->ValidDataLength = Sizes->ValidDataLength;
        Map->AllocationSize = Sizes->AllocationSize;
    }
    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    Existing = Sop->SharedCacheMap;
    if (Existing)
    {
        KeReleaseSpinLock(&QzCcLock, OldIrql);
        ExFreePoolWithTag(Map, QZ_CC_TAG);
        return Existing;
    }
    Sop->SharedCacheMap = Map;
    InsertTailList(&QzCcMaps, &Map->Link);
    KeReleaseSpinLock(&QzCcLock, OldIrql);
    ObReferenceObject(FileObject);
    return Map;
}

static VOID
QzCcMaybeTeardown(PQZ_CACHE_MAP Map)
{
    /* The lazy writer deletes it after a couple of idle passes: a file
       opened again soon (a section read piece by piece) keeps its pages */
    if (!Map->OpenCount && !Map->MmRefs)
    {
        Map->Teardown = TRUE;
        Map->IdlePasses = 0;
    }
}

/* The lazy writer's: delete a map nobody uses, if it is clean and idle */
static BOOLEAN
QzCcDeleteMap(PQZ_CACHE_MAP Map)
{
    PCACHE_UNINITIALIZE_EVENT Events;
    PLIST_ENTRY Entry;
    PFILE_OBJECT FileObject;
    KIRQL OldIrql;

    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    if (!Map->Teardown || Map->OpenCount || Map->MmRefs || ++Map->IdlePasses < 2)
    {
        KeReleaseSpinLock(&QzCcLock, OldIrql);
        return FALSE;
    }
    for (Entry = Map->Views.Flink; Entry != &Map->Views; Entry = Entry->Flink)
    {
        PQZ_VIEW View = CONTAINING_RECORD(Entry, QZ_VIEW, MapLink);
        QzCcHarvest(View);
        if (!QzCcViewIdle(View) || (View->Dirty & View->Valid))
        {
            KeReleaseSpinLock(&QzCcLock, OldIrql);
            return FALSE;
        }
    }
    while (!IsListEmpty(&Map->Views))
        QzCcDropView(CONTAINING_RECORD(Map->Views.Flink, QZ_VIEW, MapLink));
    RemoveEntryList(&Map->Link);
    if (Map->Sop->SharedCacheMap == Map)
        Map->Sop->SharedCacheMap = NULL;
    Events = Map->UninitializeEvents;
    FileObject = Map->FileObject;
    KeReleaseSpinLock(&QzCcLock, OldIrql);

    while (Events)
    {
        PCACHE_UNINITIALIZE_EVENT Next = Events->Next;
        KeSetEvent(&Events->Event, IO_NO_INCREMENT, FALSE);
        Events = Next;
    }
    Map->Signature = 0;
    ExFreePoolWithTag(Map, QZ_CC_TAG);
    ObDereferenceObject(FileObject);        /* may close the stream */
    return TRUE;
}

VOID
NTAPI
CcInitializeCacheMap(IN PFILE_OBJECT FileObject, IN PCC_FILE_SIZES FileSizes, IN BOOLEAN PinAccess,
                     IN PCACHE_MANAGER_CALLBACKS Callbacks, IN PVOID LazyWriteContext)
{
    PQZ_CACHE_MAP Map;
    PQZ_PRIVATE_MAP Private = NULL;
    KIRQL OldIrql;

    if (!QzCcReady || !FileObject->SectionObjectPointer)
        ExRaiseStatus(STATUS_INVALID_PARAMETER);
    QzCcStartLazyWriter();
    if (!FileObject->PrivateCacheMap)
    {
        Private = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Private), QZ_CC_TAG);
        if (!Private)
            ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    }
    Map = QzCcMapOf(FileObject);
    if (!Map)
        Map = QzCcCreateMap(FileObject, FileSizes);
    if (!Map)
    {
        if (Private)
            ExFreePoolWithTag(Private, QZ_CC_TAG);
        ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    }

    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    Map->Teardown = FALSE;
    if (!Map->Callbacks)
    {
        Map->Callbacks = Callbacks;
        Map->LazyWriteContext = LazyWriteContext;
        Map->PinAccess = PinAccess;
    }
    if (FileSizes)
    {
        Map->AllocationSize = FileSizes->AllocationSize;
        Map->FileSize = FileSizes->FileSize;
        Map->ValidDataLength = FileSizes->ValidDataLength;
    }
    if (Private && !FileObject->PrivateCacheMap)
    {
        Private->NodeTypeCode = 0x2FE;
        Private->NodeByteSize = sizeof(*Private);
        Private->Shared = Map;
        Private->FileObject = FileObject;
        FileObject->PrivateCacheMap = Private;
        Map->OpenCount++;
        Private = NULL;
    }
    KeReleaseSpinLock(&QzCcLock, OldIrql);
    if (Private)
        ExFreePoolWithTag(Private, QZ_CC_TAG);
}

BOOLEAN
NTAPI
CcUninitializeCacheMap(IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER TruncateSize OPTIONAL,
                       IN PCACHE_UNINITIALIZE_EVENT UninitializeCompleteEvent OPTIONAL)
{
    PQZ_CACHE_MAP Map = QzCcMapOf(FileObject);
    PQZ_PRIVATE_MAP Private = FileObject->PrivateCacheMap;
    KIRQL OldIrql;

    if (!Map)
    {
        if (Private)
        {
            FileObject->PrivateCacheMap = NULL;
            ExFreePoolWithTag(Private, QZ_CC_TAG);
        }
        if (UninitializeCompleteEvent)
            KeSetEvent(&UninitializeCompleteEvent->Event, IO_NO_INCREMENT, FALSE);
        return FALSE;
    }
    if (TruncateSize)
    {
        CC_FILE_SIZES Sizes;
        Sizes.AllocationSize = Map->AllocationSize;
        Sizes.FileSize.QuadPart = min(TruncateSize->QuadPart, Map->FileSize.QuadPart);
        Sizes.ValidDataLength.QuadPart = min(Sizes.FileSize.QuadPart, Map->ValidDataLength.QuadPart);
        CcSetFileSizes(FileObject, &Sizes);
    }

    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    if (Private)
    {
        FileObject->PrivateCacheMap = NULL;
        Map->OpenCount--;
    }
    QzCcMaybeTeardown(Map);
    if (UninitializeCompleteEvent)
    {
        if (Map->Teardown)
        {
            UninitializeCompleteEvent->Next = Map->UninitializeEvents;
            Map->UninitializeEvents = UninitializeCompleteEvent;
        }
        else
        {
            KeSetEvent(&UninitializeCompleteEvent->Event, IO_NO_INCREMENT, FALSE);
        }
    }
    KeReleaseSpinLock(&QzCcLock, OldIrql);
    if (Private)
        ExFreePoolWithTag(Private, QZ_CC_TAG);
    return TRUE;
}

VOID
NTAPI
CcSetFileSizes(IN PFILE_OBJECT FileObject, IN PCC_FILE_SIZES FileSizes)
{
    PQZ_CACHE_MAP Map = QzCcMapOf(FileObject);
    LONGLONG OldSize, NewSize;
    PQZ_VIEW Last = NULL;
    KIRQL OldIrql;

    if (!Map)
        return;
    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    OldSize = Map->FileSize.QuadPart;
    NewSize = FileSizes->FileSize.QuadPart;
    Map->AllocationSize = FileSizes->AllocationSize;
    Map->FileSize = FileSizes->FileSize;
    Map->ValidDataLength = FileSizes->ValidDataLength;
    if (NewSize < OldSize && (NewSize & (PAGE_SIZE - 1)))
    {
        Last = QzCcFindView(Map, (ULONGLONG)NewSize >> QZ_VIEW_SHIFT);
        if (Last)
            Last->Pins++;
    }
    KeReleaseSpinLock(&QzCcLock, OldIrql);

    if (NewSize < OldSize)
    {
        /* The tail of the last page reads as zeros if the file grows again */
        if (Last)
        {
            ULONG Page = (ULONG)((NewSize & (QZ_VIEW_SIZE - 1)) / PAGE_SIZE);
            if (Last->Valid & QzPageBit(Page))
                RtlZeroMemory(Last->Va + (NewSize & (QZ_VIEW_SIZE - 1)), PAGE_SIZE - (NewSize & (PAGE_SIZE - 1)));
            QzCcReleaseView(Last);
        }
        QzCcPurge(Map, (NewSize + PAGE_SIZE - 1) & ~(LONGLONG)(PAGE_SIZE - 1), MAXLONGLONG);
    }
}

BOOLEAN
NTAPI
CcPurgeCacheSection(IN PSECTION_OBJECT_POINTERS SectionObjectPointer, IN PLARGE_INTEGER FileOffset OPTIONAL,
                    IN ULONG Length, IN BOOLEAN UninitializeCacheMaps)
{
    PQZ_CACHE_MAP Map = SectionObjectPointer->SharedCacheMap;
    LONGLONG Start = FileOffset ? FileOffset->QuadPart : 0;
    LONGLONG End = (FileOffset && Length) ? Start + Length : MAXLONGLONG;

    UNREFERENCED_PARAMETER(UninitializeCacheMaps);
    if (!Map || Map->Signature != QZ_CACHE_MAP_TAG)
        return TRUE;
    return QzCcPurge(Map, Start, End);
}

VOID
NTAPI
CcFlushCache(IN PSECTION_OBJECT_POINTERS SectionObjectPointer, IN PLARGE_INTEGER FileOffset OPTIONAL,
             IN ULONG Length, OUT PIO_STATUS_BLOCK IoStatus OPTIONAL)
{
    PQZ_CACHE_MAP Map = SectionObjectPointer->SharedCacheMap;
    LONGLONG Start = FileOffset ? FileOffset->QuadPart : 0;
    LONGLONG End = (FileOffset && Length) ? Start + Length : MAXLONGLONG;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Map && Map->Signature == QZ_CACHE_MAP_TAG)
        Status = QzCcFlushRange(Map, Start, End);
    if (IoStatus)
    {
        IoStatus->Status = Status;
        IoStatus->Information = 0;
    }
}

/* ---------------------------------------------------------------------- */
/* Copy interface                                                          */
/* ---------------------------------------------------------------------- */

/*
 * Walk [Offset, Offset + Length) view by view: make the pages valid (or,
 * for writes, ready to be overwritten) and copy. FALSE: it would have to
 * wait and may not.
 */
static NTSTATUS
QzCcCopy(PQZ_CACHE_MAP Map, LONGLONG Offset, ULONG Length, BOOLEAN Wait, PUCHAR Buffer, BOOLEAN Write,
         BOOLEAN Zero)
{
    NTSTATUS Status = STATUS_SUCCESS;

    while (Length)
    {
        ULONGLONG Index = (ULONGLONG)Offset >> QZ_VIEW_SHIFT;
        ULONG InView = (ULONG)(Offset & (QZ_VIEW_SIZE - 1));
        ULONG Chunk = min(Length, QZ_VIEW_SIZE - InView);
        ULONG First = InView / PAGE_SIZE, Last = (InView + Chunk - 1) / PAGE_SIZE;
        ULONGLONG NoRead = 0;
        PQZ_VIEW View;

        Status = QzCcReferenceView(Map, Index, &View);
        if (!NT_SUCCESS(Status))
            break;
        if (Write)
        {
            /* Pages written whole need not be read first */
            ULONG p;
            for (p = First; p <= Last; p++)
            {
                ULONG PageStart = p * PAGE_SIZE;
                if (PageStart >= InView && PageStart + PAGE_SIZE <= InView + Chunk)
                    NoRead |= QzPageBit(p);
            }
        }
        Status = QzCcFill(View, First, Last, Wait, NoRead);
        if (NT_SUCCESS(Status))
        {
            if (!Write)
                RtlCopyMemory(Buffer, View->Va + InView, Chunk);
            else if (Zero)
                RtlZeroMemory(View->Va + InView, Chunk);
            else
                RtlCopyMemory(View->Va + InView, Buffer, Chunk);
            if (Write)
                QzCcSetDirty(View, First, Last);
        }
        QzCcReleaseView(View);
        if (!NT_SUCCESS(Status))
            break;
        Offset += Chunk;
        if (Buffer)
            Buffer += Chunk;
        Length -= Chunk;
    }
    return Status;
}

static BOOLEAN
QzCcCopyResult(NTSTATUS Status)
{
    if (Status == STATUS_CANT_WAIT)
        return FALSE;
    if (!NT_SUCCESS(Status))
        ExRaiseStatus(Status);              /* the file system's handlers take it */
    return TRUE;
}

BOOLEAN
NTAPI
CcCopyRead(IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER FileOffset, IN ULONG Length, IN BOOLEAN Wait,
           OUT PVOID Buffer, OUT PIO_STATUS_BLOCK IoStatus)
{
    PQZ_CACHE_MAP Map = QzCcMapOf(FileObject);
    NTSTATUS Status;

    if (!Map)
        ExRaiseStatus(STATUS_INVALID_PARAMETER);
    Status = QzCcCopy(Map, FileOffset->QuadPart, Length, Wait, Buffer, FALSE, FALSE);
    if (!QzCcCopyResult(Status))
        return FALSE;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = Length;
    return TRUE;
}

VOID
NTAPI
CcFastCopyRead(IN PFILE_OBJECT FileObject, IN ULONG FileOffset, IN ULONG Length, IN ULONG PageCount,
               OUT PVOID Buffer, OUT PIO_STATUS_BLOCK IoStatus)
{
    LARGE_INTEGER Offset;
    UNREFERENCED_PARAMETER(PageCount);
    Offset.QuadPart = FileOffset;
    CcCopyRead(FileObject, &Offset, Length, TRUE, Buffer, IoStatus);
}

BOOLEAN
NTAPI
CcCopyWrite(IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER FileOffset, IN ULONG Length, IN BOOLEAN Wait,
            IN PVOID Buffer)
{
    PQZ_CACHE_MAP Map = QzCcMapOf(FileObject);
    NTSTATUS Status;

    if (!Map)
        ExRaiseStatus(STATUS_INVALID_PARAMETER);
    Status = QzCcCopy(Map, FileOffset->QuadPart, Length, Wait, Buffer, TRUE, FALSE);
    if (!QzCcCopyResult(Status))
        return FALSE;
    if ((FileObject->Flags & FO_WRITE_THROUGH) || Map->DisableWriteBehind)
    {
        Status = QzCcFlushRange(Map, FileOffset->QuadPart, FileOffset->QuadPart + Length);
        if (!NT_SUCCESS(Status))
            ExRaiseStatus(Status);
    }
    return TRUE;
}

VOID
NTAPI
CcFastCopyWrite(IN PFILE_OBJECT FileObject, IN ULONG FileOffset, IN ULONG Length, IN PVOID Buffer)
{
    LARGE_INTEGER Offset;
    Offset.QuadPart = FileOffset;
    CcCopyWrite(FileObject, &Offset, Length, TRUE, Buffer);
}

/* Zeros: through the cache when the file is cached, else straight to disk */
BOOLEAN
NTAPI
CcZeroData(IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER StartOffset, IN PLARGE_INTEGER EndOffset, IN BOOLEAN Wait)
{
    PQZ_CACHE_MAP Map = QzCcMapOf(FileObject);
    LONGLONG Offset = StartOffset->QuadPart, End = EndOffset->QuadPart;
    NTSTATUS Status = STATUS_SUCCESS;

    if (End <= Offset)
        return TRUE;
    if (Map)
    {
        while (Offset < End)
        {
            ULONG Chunk = (ULONG)min(End - Offset, 0x10000000);
            Status = QzCcCopy(Map, Offset, Chunk, Wait, NULL, TRUE, TRUE);
            if (!NT_SUCCESS(Status))
                break;
            Offset += Chunk;
        }
        return QzCcCopyResult(Status);
    }

    {
        ULONG Size = 0x10000;
        PVOID Zeros;
        if (!Wait)
            return FALSE;
        Zeros = ExAllocatePoolWithTag(NonPagedPool, Size, QZ_CC_TAG);
        if (!Zeros)
            ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
        RtlZeroMemory(Zeros, Size);
        while (Offset < End && NT_SUCCESS(Status))
        {
            ULONG Chunk = (ULONG)min(End - Offset, Size);
            LARGE_INTEGER At;
            IO_STATUS_BLOCK Io;
            KEVENT Event;
            PIRP Irp;
            At.QuadPart = Offset;
            KeInitializeEvent(&Event, NotificationEvent, FALSE);
            Irp = IoBuildSynchronousFsdRequest(IRP_MJ_WRITE, IoGetRelatedDeviceObject(FileObject), Zeros, Chunk,
                                               &At, &Event, &Io);
            if (!Irp)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            Irp->Flags |= IRP_NOCACHE;
            IoGetNextIrpStackLocation(Irp)->FileObject = FileObject;
            Status = IoCallDriver(IoGetRelatedDeviceObject(FileObject), Irp);
            if (Status == STATUS_PENDING)
            {
                KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
                Status = Io.Status;
            }
            Offset += Chunk;
        }
        ExFreePoolWithTag(Zeros, QZ_CC_TAG);
        if (!NT_SUCCESS(Status))
            ExRaiseStatus(Status);
        return TRUE;
    }
}

/* ---------------------------------------------------------------------- */
/* Pin interface                                                           */
/* ---------------------------------------------------------------------- */

static VOID
QzCcReleaseBcb(PQZ_BCB Bcb)
{
    ULONG i;

    if (InterlockedDecrement(&Bcb->RefCount))
        return;
    if (Bcb->AliasPages)
    {
        /* Changes made through the alias count as the views' */
        LONGLONG Offset = Bcb->Public.MappedFileOffset.QuadPart & ~(LONGLONG)(PAGE_SIZE - 1);
        ULONG p;
        for (p = 0; p < Bcb->AliasPages; p++)
        {
            PUCHAR Va = (PUCHAR)QZ_ALIAS_BASE + (Bcb->AliasFirst + p) * PAGE_SIZE;
            if (*QzPteAt(Va) & QZ_PTE_DIRTY_BIT)
            {
                LONGLONG At = Offset + (LONGLONG)p * PAGE_SIZE;
                for (i = 0; i < Bcb->ViewCount; i++)
                {
                    PQZ_VIEW View = Bcb->Views[i];
                    LONGLONG ViewStart = (LONGLONG)(View->Index << QZ_VIEW_SHIFT);
                    if (At >= ViewStart && At < ViewStart + QZ_VIEW_SIZE)
                        QzCcSetDirty(View, (ULONG)((At - ViewStart) / PAGE_SIZE), (ULONG)((At - ViewStart) / PAGE_SIZE));
                }
            }
            QzUnmapPage((ULONG_PTR)Va);
        }
        {
            KIRQL OldIrql;
            KeAcquireSpinLock(&QzCcLock, &OldIrql);
            RtlClearBits(&QzCcAlias, Bcb->AliasFirst, Bcb->AliasPages);
            KeReleaseSpinLock(&QzCcLock, OldIrql);
        }
    }
    for (i = 0; i < Bcb->ViewCount; i++)
        QzCcReleaseView(Bcb->Views[i]);
    ExFreePoolWithTag(Bcb, QZ_CC_TAG);
}

/* Map [Offset, Offset + Length) contiguously; FALSE: it would have to wait */
static BOOLEAN
QzCcMapRange(PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset, ULONG Length, BOOLEAN Wait, BOOLEAN NoRead,
             PQZ_BCB *Result, PVOID *Buffer)
{
    PQZ_CACHE_MAP Map = QzCcMapOf(FileObject);
    LONGLONG Offset = FileOffset->QuadPart, End = Offset + max(Length, 1);
    ULONGLONG FirstIndex = (ULONGLONG)Offset >> QZ_VIEW_SHIFT, LastIndex = (ULONGLONG)(End - 1) >> QZ_VIEW_SHIFT;
    NTSTATUS Status = STATUS_SUCCESS;
    PQZ_BCB Bcb;
    ULONG i;

    if (!Map || LastIndex - FirstIndex >= QZ_BCB_MAX_VIEWS)
        ExRaiseStatus(STATUS_INVALID_PARAMETER);
    Bcb = ExAllocatePoolWithTag(NonPagedPool, sizeof(QZ_BCB), QZ_CC_TAG);
    if (!Bcb)
        ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    RtlZeroMemory(Bcb, sizeof(*Bcb));
    Bcb->Public.NodeTypeCode = 0x2FD;
    Bcb->Public.NodeByteSize = sizeof(PUBLIC_BCB);
    Bcb->Public.MappedLength = Length;
    Bcb->Public.MappedFileOffset = *FileOffset;
    Bcb->Map = Map;
    Bcb->RefCount = 1;

    for (i = 0; i <= LastIndex - FirstIndex; i++)
    {
        PQZ_VIEW View;
        LONGLONG ViewStart = (LONGLONG)((FirstIndex + i) << QZ_VIEW_SHIFT);
        ULONG From = (ULONG)(max(Offset, ViewStart) - ViewStart), To = (ULONG)(min(End, ViewStart + QZ_VIEW_SIZE) - ViewStart);
        ULONG First = From / PAGE_SIZE, Last = (To - 1) / PAGE_SIZE, p;
        ULONGLONG Skip = 0;

        Status = QzCcReferenceView(Map, FirstIndex + i, &View);
        if (!NT_SUCCESS(Status))
            break;
        Bcb->Views[Bcb->ViewCount++] = View;
        if (NoRead)
            for (p = First; p <= Last; p++)
                if (p * PAGE_SIZE >= From && (p + 1) * PAGE_SIZE <= To)
                    Skip |= QzPageBit(p);
        Status = QzCcFill(View, First, Last, Wait, Skip);
        if (!NT_SUCCESS(Status))
            break;
    }
    if (NT_SUCCESS(Status) && Bcb->ViewCount == 1)
    {
        Bcb->Buffer = Bcb->Views[0]->Va + (Offset & (QZ_VIEW_SIZE - 1));
    }
    else if (NT_SUCCESS(Status))
    {
        /* Across views: the same pages again, side by side */
        LONGLONG PageStart = Offset & ~(LONGLONG)(PAGE_SIZE - 1);
        ULONG Pages = (ULONG)((End - PageStart + PAGE_SIZE - 1) / PAGE_SIZE), First, p;
        KIRQL OldIrql;

        KeAcquireSpinLock(&QzCcLock, &OldIrql);
        First = RtlFindClearBitsAndSet(&QzCcAlias, Pages, 0);
        KeReleaseSpinLock(&QzCcLock, OldIrql);
        if (First == 0xFFFFFFFF)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            Bcb->AliasFirst = First;
            Bcb->AliasPages = Pages;
            for (p = 0; p < Pages; p++)
            {
                LONGLONG At = PageStart + (LONGLONG)p * PAGE_SIZE;
                PQZ_VIEW View = Bcb->Views[(ULONG)(((ULONGLONG)At >> QZ_VIEW_SHIFT) - FirstIndex)];
                ULONG Pfn = *QzPteAt(View->Va + (At & (QZ_VIEW_SIZE - 1))) >> PAGE_SHIFT;
                QzMapPage(QZ_ALIAS_BASE + (First + p) * PAGE_SIZE, Pfn, QZ_PTE_WRITE);
            }
            Bcb->Buffer = (PUCHAR)QZ_ALIAS_BASE + First * PAGE_SIZE + (Offset & (PAGE_SIZE - 1));
        }
    }

    if (!NT_SUCCESS(Status))
    {
        QzCcReleaseBcb(Bcb);
        return QzCcCopyResult(Status);
    }
    *Result = Bcb;
    *Buffer = Bcb->Buffer;
    return TRUE;
}

BOOLEAN
NTAPI
CcMapData(IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER FileOffset, IN ULONG Length, IN ULONG Flags,
          OUT PVOID *Bcb, OUT PVOID *Buffer)
{
    return QzCcMapRange(FileObject, FileOffset, Length, (Flags & MAP_WAIT) != 0, FALSE, (PQZ_BCB *)Bcb, Buffer);
}

BOOLEAN
NTAPI
CcPinRead(IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER FileOffset, IN ULONG Length, IN ULONG Flags,
          OUT PVOID *Bcb, OUT PVOID *Buffer)
{
    if (Flags & PIN_IF_BCB)
        return FALSE;                       /* BCBs do not outlive their pins here */
    if (!QzCcMapRange(FileObject, FileOffset, Length, (Flags & PIN_WAIT) != 0, (Flags & PIN_NO_READ) != 0,
                      (PQZ_BCB *)Bcb, Buffer))
        return FALSE;
    ((PQZ_BCB)*Bcb)->Pinned = TRUE;
    return TRUE;
}

BOOLEAN
NTAPI
CcPinMappedData(IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER FileOffset, IN ULONG Length, IN ULONG Flags,
                IN OUT PVOID *Bcb)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Flags);
    ((PQZ_BCB)*Bcb)->Pinned = TRUE;         /* a mapping stays put already */
    return TRUE;
}

BOOLEAN
NTAPI
CcPreparePinWrite(IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER FileOffset, IN ULONG Length, IN BOOLEAN Zero,
                  IN ULONG Flags, OUT PVOID *Bcb, OUT PVOID *Buffer)
{
    PQZ_BCB Result;
    ULONG i;

    if (!QzCcMapRange(FileObject, FileOffset, Length, (Flags & PIN_WAIT) != 0, TRUE, &Result, Buffer))
        return FALSE;
    Result->Pinned = TRUE;
    if (Zero)
        RtlZeroMemory(*Buffer, Length);
    CcSetDirtyPinnedData(Result, NULL);
    UNREFERENCED_PARAMETER(i);
    *Bcb = Result;
    return TRUE;
}

VOID
NTAPI
CcSetDirtyPinnedData(IN PVOID BcbVoid, IN PLARGE_INTEGER Lsn OPTIONAL)
{
    PQZ_BCB Bcb = BcbVoid;
    LONGLONG Offset = Bcb->Public.MappedFileOffset.QuadPart, End = Offset + max(Bcb->Public.MappedLength, 1);
    ULONG i;

    if (Lsn)
        Bcb->Lsn = *Lsn;
    for (i = 0; i < Bcb->ViewCount; i++)
    {
        PQZ_VIEW View = Bcb->Views[i];
        LONGLONG ViewStart = (LONGLONG)(View->Index << QZ_VIEW_SHIFT);
        ULONG From = (ULONG)(max(Offset, ViewStart) - ViewStart), To = (ULONG)(min(End, ViewStart + QZ_VIEW_SIZE) - ViewStart);
        QzCcSetDirty(View, From / PAGE_SIZE, (To - 1) / PAGE_SIZE);
    }
}

VOID
NTAPI
CcUnpinData(IN PVOID Bcb)
{
    QzCcReleaseBcb(Bcb);
}

VOID
NTAPI
CcUnpinDataForThread(IN PVOID Bcb, IN ERESOURCE_THREAD ResourceThreadId)
{
    UNREFERENCED_PARAMETER(ResourceThreadId);
    QzCcReleaseBcb(Bcb);
}

VOID
NTAPI
CcRepinBcb(IN PVOID Bcb)
{
    InterlockedIncrement(&((PQZ_BCB)Bcb)->RefCount);
}

VOID
NTAPI
CcUnpinRepinnedBcb(IN PVOID BcbVoid, IN BOOLEAN WriteThrough, OUT PIO_STATUS_BLOCK IoStatus)
{
    PQZ_BCB Bcb = BcbVoid;
    NTSTATUS Status = STATUS_SUCCESS;

    if (WriteThrough)
        Status = QzCcFlushRange(Bcb->Map, Bcb->Public.MappedFileOffset.QuadPart,
                                Bcb->Public.MappedFileOffset.QuadPart + max(Bcb->Public.MappedLength, 1));
    IoStatus->Status = Status;
    IoStatus->Information = 0;
    QzCcReleaseBcb(Bcb);
}

PFILE_OBJECT
NTAPI
CcGetFileObjectFromBcb(IN PVOID Bcb)
{
    return ((PQZ_BCB)Bcb)->Map->FileObject;
}

PFILE_OBJECT
NTAPI
CcGetFileObjectFromSectionPtrs(IN PSECTION_OBJECT_POINTERS SectionObjectPointer)
{
    PQZ_CACHE_MAP Map = SectionObjectPointer->SharedCacheMap;
    return (Map && Map->Signature == QZ_CACHE_MAP_TAG) ? Map->FileObject : NULL;
}

/* ---------------------------------------------------------------------- */
/* MDL interface                                                           */
/* ---------------------------------------------------------------------- */

/* A chain of MDLs over the cache pages of the range, each pinning its view */
static NTSTATUS
QzCcBuildMdlChain(PFILE_OBJECT FileObject, LONGLONG Offset, ULONG Length, BOOLEAN Write, PMDL *Chain)
{
    PQZ_CACHE_MAP Map = QzCcMapOf(FileObject);
    PMDL *Link = Chain;
    NTSTATUS Status = STATUS_SUCCESS;

    *Chain = NULL;
    if (!Map)
        return STATUS_INVALID_PARAMETER;
    while (Length)
    {
        ULONGLONG Index = (ULONGLONG)Offset >> QZ_VIEW_SHIFT;
        ULONG InView = (ULONG)(Offset & (QZ_VIEW_SIZE - 1));
        ULONG Chunk = min(Length, QZ_VIEW_SIZE - InView), First = InView / PAGE_SIZE, Last = (InView + Chunk - 1) / PAGE_SIZE;
        ULONGLONG NoRead = 0;
        PQZ_VIEW View;
        PMDL Mdl;
        ULONG p;

        Status = QzCcReferenceView(Map, Index, &View);
        if (!NT_SUCCESS(Status))
            break;
        if (Write)
            for (p = First; p <= Last; p++)
                if (p * PAGE_SIZE >= InView && (p + 1) * PAGE_SIZE <= InView + Chunk)
                    NoRead |= QzPageBit(p);
        Status = QzCcFill(View, First, Last, TRUE, NoRead);
        Mdl = NT_SUCCESS(Status) ? QzCcBuildMdl(View, First, Last - First + 1, TRUE) : NULL;
        if (!Mdl)
        {
            QzCcReleaseView(View);
            if (NT_SUCCESS(Status))
                Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        /* The MDL describes exactly the bytes asked for */
        Mdl->ByteOffset = InView & (PAGE_SIZE - 1);
        Mdl->ByteCount = Chunk;
        Mdl->MappedSystemVa = View->Va + InView;
        *Link = Mdl;
        Link = &Mdl->Next;
        Offset += Chunk;
        Length -= Chunk;
    }
    return Status;
}

static VOID
QzCcFreeMdlChain(PMDL Chain, BOOLEAN Dirty)
{
    while (Chain)
    {
        PMDL Next = Chain->Next;
        PQZ_CC_MDL Wrapper = CONTAINING_RECORD(Chain, QZ_CC_MDL, Mdl);
        if (Wrapper->View)
        {
            if (Dirty)
            {
                ULONG InView = (ULONG)((PUCHAR)Chain->MappedSystemVa - Wrapper->View->Va);
                QzCcSetDirty(Wrapper->View, InView / PAGE_SIZE, (InView + Chain->ByteCount - 1) / PAGE_SIZE);
            }
            QzCcReleaseView(Wrapper->View);
        }
        QzCcFreeMdl(Chain);
        Chain = Next;
    }
}

VOID
NTAPI
CcMdlRead(IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER FileOffset, IN ULONG Length, OUT PMDL *MdlChain,
          OUT PIO_STATUS_BLOCK IoStatus)
{
    NTSTATUS Status = QzCcBuildMdlChain(FileObject, FileOffset->QuadPart, Length, FALSE, MdlChain);
    if (!NT_SUCCESS(Status))
    {
        QzCcFreeMdlChain(*MdlChain, FALSE);
        *MdlChain = NULL;
        ExRaiseStatus(Status);
    }
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = Length;
}

VOID
NTAPI
CcMdlReadComplete(IN PFILE_OBJECT FileObject, IN PMDL MdlChain)
{
    UNREFERENCED_PARAMETER(FileObject);
    QzCcFreeMdlChain(MdlChain, FALSE);
}

VOID
NTAPI
CcPrepareMdlWrite(IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER FileOffset, IN ULONG Length, OUT PMDL *MdlChain,
                  OUT PIO_STATUS_BLOCK IoStatus)
{
    NTSTATUS Status = QzCcBuildMdlChain(FileObject, FileOffset->QuadPart, Length, TRUE, MdlChain);
    if (!NT_SUCCESS(Status))
    {
        QzCcFreeMdlChain(*MdlChain, FALSE);
        *MdlChain = NULL;
        ExRaiseStatus(Status);
    }
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = Length;
}

VOID
NTAPI
CcMdlWriteComplete(IN PFILE_OBJECT FileObject, IN PLARGE_INTEGER FileOffset, IN PMDL MdlChain)
{
    UNREFERENCED_PARAMETER(FileOffset);
    QzCcFreeMdlChain(MdlChain, TRUE);
    if (FileObject->Flags & FO_WRITE_THROUGH)
    {
        PQZ_CACHE_MAP Map = QzCcMapOf(FileObject);
        if (Map)
            QzCcFlushRange(Map, 0, MAXLONGLONG);
    }
}

VOID
NTAPI
CcMdlWriteAbort(IN PFILE_OBJECT FileObject, IN PMDL MdlChain)
{
    UNREFERENCED_PARAMETER(FileObject);
    QzCcFreeMdlChain(MdlChain, FALSE);
}

/* ---------------------------------------------------------------------- */
/* Throttling, attributes, logs: what the file systems ask about           */
/* ---------------------------------------------------------------------- */

BOOLEAN
NTAPI
CcCanIWrite(IN PFILE_OBJECT FileObject, IN ULONG BytesToWrite, IN BOOLEAN Wait, IN BOOLEAN Retrying)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(BytesToWrite);
    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(Retrying);
    return TRUE;
}

VOID
NTAPI
CcDeferWrite(IN PFILE_OBJECT FileObject, IN PCC_POST_DEFERRED_WRITE PostRoutine, IN PVOID Context1,
             IN PVOID Context2, IN ULONG BytesToWrite, IN BOOLEAN Retrying)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(BytesToWrite);
    UNREFERENCED_PARAMETER(Retrying);
    PostRoutine(Context1, Context2);
}

VOID
NTAPI
CcSetAdditionalCacheAttributes(IN PFILE_OBJECT FileObject, IN BOOLEAN DisableReadAhead,
                               IN BOOLEAN DisableWriteBehind)
{
    PQZ_CACHE_MAP Map = QzCcMapOf(FileObject);
    UNREFERENCED_PARAMETER(DisableReadAhead);
    if (Map)
        Map->DisableWriteBehind = DisableWriteBehind;
}

VOID
NTAPI
CcSetReadAheadGranularity(IN PFILE_OBJECT FileObject, IN ULONG Granularity)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(Granularity);
}

VOID
NTAPI
CcSetLogHandleForFile(IN PFILE_OBJECT FileObject, IN PVOID LogHandle, IN PFLUSH_TO_LSN FlushToLsnRoutine)
{
    PQZ_CACHE_MAP Map = QzCcMapOf(FileObject);
    if (Map)
    {
        Map->LogHandle = LogHandle;
        Map->FlushToLsn = FlushToLsnRoutine;
    }
}

LARGE_INTEGER
NTAPI
CcGetDirtyPages(IN PVOID LogHandle, IN PDIRTY_PAGE_ROUTINE DirtyPageRoutine, IN PVOID Context1,
                IN PVOID Context2)
{
    LARGE_INTEGER Oldest;
    UNREFERENCED_PARAMETER(LogHandle);
    UNREFERENCED_PARAMETER(DirtyPageRoutine);
    UNREFERENCED_PARAMETER(Context1);
    UNREFERENCED_PARAMETER(Context2);
    Oldest.QuadPart = 0;
    return Oldest;
}

BOOLEAN
NTAPI
CcIsThereDirtyData(IN PVPB Vpb)
{
    PLIST_ENTRY Entry;
    BOOLEAN Dirty = FALSE;
    KIRQL OldIrql;

    if (!QzCcReady)
        return FALSE;
    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    for (Entry = QzCcMaps.Flink; Entry != &QzCcMaps && !Dirty; Entry = Entry->Flink)
    {
        PQZ_CACHE_MAP Map = CONTAINING_RECORD(Entry, QZ_CACHE_MAP, Link);
        PLIST_ENTRY ViewEntry;
        if (Vpb && Map->FileObject->Vpb != Vpb)
            continue;
        for (ViewEntry = Map->Views.Flink; ViewEntry != &Map->Views && !Dirty; ViewEntry = ViewEntry->Flink)
        {
            PQZ_VIEW View = CONTAINING_RECORD(ViewEntry, QZ_VIEW, MapLink);
            QzCcHarvest(View);
            Dirty = (View->Dirty & View->Valid) != 0;
        }
    }
    KeReleaseSpinLock(&QzCcLock, OldIrql);
    return Dirty;
}

/* One pass of the lazy writer, done */
NTSTATUS
NTAPI
CcWaitForCurrentLazyWriterActivity(VOID)
{
    ULONG Passes = QzCcLazyPasses;
    if (!QzCcLazyStarted)
        return STATUS_SUCCESS;
    while (QzCcLazyPasses - Passes < 2)
    {
        LARGE_INTEGER Timeout;
        Timeout.QuadPart = -10000000LL;
        KeClearEvent(&QzCcLazyDone);
        KeSetEvent(&QzCcLazyWake, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(&QzCcLazyDone, Executive, KernelMode, FALSE, &Timeout);
    }
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* The lazy writer                                                         */
/* ---------------------------------------------------------------------- */

/* Clean pages no one holds go back when free memory runs low */
static VOID
QzCcTrim(VOID)
{
    ULONG Total, Free;
    KIRQL OldIrql;

    QzMmGetStats(&Total, &Free);
    if (Free > Total / 8)
        return;
    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    while (Free < Total / 6)
    {
        PQZ_VIEW Victim = QzCcOldestIdle(TRUE);
        if (!Victim)
            break;
        QzCcDropView(Victim);
        QzMmGetStats(&Total, &Free);
    }
    KeReleaseSpinLock(&QzCcLock, OldIrql);
}

static VOID
NTAPI
QzCcLazyWriter(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    for (;;)
    {
        PQZ_CACHE_MAP Maps[64];
        LARGE_INTEGER Timeout;
        PLIST_ENTRY Entry;
        ULONG Count = 0, i;
        KIRQL OldIrql;

        Timeout.QuadPart = -10000000LL;     /* a second */
        KeWaitForSingleObject(&QzCcLazyWake, Executive, KernelMode, FALSE, &Timeout);

        /* Only this thread deletes maps, so the snapshot stays good */
        KeAcquireSpinLock(&QzCcLock, &OldIrql);
        for (Entry = QzCcMaps.Flink; Entry != &QzCcMaps && Count < RTL_NUMBER_OF(Maps); Entry = Entry->Flink)
            Maps[Count++] = CONTAINING_RECORD(Entry, QZ_CACHE_MAP, Link);
        KeReleaseSpinLock(&QzCcLock, OldIrql);

        for (i = 0; i < Count; i++)
        {
            PQZ_CACHE_MAP Map = Maps[i];
            if (QzMmCollectFileDirty)
                QzMmCollectFileDirty(Map->Sop);
            if (QzCcMapIsDirty(Map))
            {
                BOOLEAN Acquired = TRUE;
                if (Map->Callbacks && Map->Callbacks->AcquireForLazyWrite)
                    Acquired = Map->Callbacks->AcquireForLazyWrite(Map->LazyWriteContext, TRUE);
                if (Acquired)
                {
                    QzCcFlushRange(Map, 0, MAXLONGLONG);
                    if (Map->Callbacks && Map->Callbacks->ReleaseFromLazyWrite)
                        Map->Callbacks->ReleaseFromLazyWrite(Map->LazyWriteContext);
                }
            }
            if (Map->Teardown)
                QzCcDeleteMap(Map);
        }
        QzCcTrim();
        QzCcLazyPasses++;
        KeSetEvent(&QzCcLazyDone, IO_NO_INCREMENT, FALSE);
    }
}

/* ---------------------------------------------------------------------- */
/* For Mm: sections of files share the cache's pages                       */
/* ---------------------------------------------------------------------- */

/* A cache map for a file a section maps (created if the file is not cached yet) */
PVOID
QzCcMmAcquire(PFILE_OBJECT FileObject, LONGLONG FileSize)
{
    PQZ_CACHE_MAP Map;
    CC_FILE_SIZES Sizes;
    KIRQL OldIrql;

    if (!QzCcReady || !FileObject->SectionObjectPointer)
        return NULL;
    QzCcStartLazyWriter();
    Sizes.AllocationSize.QuadPart = Sizes.FileSize.QuadPart = Sizes.ValidDataLength.QuadPart = FileSize;
    Map = QzCcMapOf(FileObject);
    if (!Map)
        Map = QzCcCreateMap(FileObject, &Sizes);
    if (!Map)
        return NULL;
    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    Map->MmRefs++;
    Map->Teardown = FALSE;
    KeReleaseSpinLock(&QzCcLock, OldIrql);
    return Map;
}

VOID
QzCcMmRelease(PVOID MapVoid)
{
    PQZ_CACHE_MAP Map = MapVoid;
    KIRQL OldIrql;
    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    Map->MmRefs--;
    QzCcMaybeTeardown(Map);
    KeReleaseSpinLock(&QzCcLock, OldIrql);
}

LONGLONG
QzCcMmFileSize(PVOID MapVoid)
{
    return ((PQZ_CACHE_MAP)MapVoid)->FileSize.QuadPart;
}

/* Read through the cache (image sections, the loader) */
NTSTATUS
QzCcMmRead(PVOID MapVoid, LONGLONG Offset, PVOID Buffer, ULONG Length)
{
    return QzCcCopy(MapVoid, Offset, Length, TRUE, Buffer, FALSE, FALSE);
}

/* The physical page holding the file's page at Offset, held for a mapping */
NTSTATUS
QzCcMmGetPage(PVOID MapVoid, LONGLONG Offset, PULONG Pfn)
{
    PQZ_VIEW View;
    ULONG Page = (ULONG)((Offset & (QZ_VIEW_SIZE - 1)) / PAGE_SIZE);
    KIRQL OldIrql;
    NTSTATUS Status = QzCcReferenceView(MapVoid, (ULONGLONG)Offset >> QZ_VIEW_SHIFT, &View);

    if (!NT_SUCCESS(Status))
        return Status;
    Status = QzCcFill(View, Page, Page, TRUE, 0);
    if (NT_SUCCESS(Status))
    {
        *Pfn = *QzPteAt(View->Va + Page * PAGE_SIZE) >> PAGE_SHIFT;
        KeAcquireSpinLock(&QzCcLock, &OldIrql);
        View->MapRefs++;
        KeReleaseSpinLock(&QzCcLock, OldIrql);
    }
    QzCcReleaseView(View);
    return Status;
}

/* A mapping of the page at Offset goes away; Dirty: it was written through it */
VOID
QzCcMmPutPage(PVOID MapVoid, LONGLONG Offset, BOOLEAN Dirty)
{
    PQZ_VIEW View;
    ULONG Page = (ULONG)((Offset & (QZ_VIEW_SIZE - 1)) / PAGE_SIZE);
    KIRQL OldIrql;

    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    View = QzCcFindView(MapVoid, (ULONGLONG)Offset >> QZ_VIEW_SHIFT);
    if (View)
    {
        if (Dirty)
            View->Dirty |= QzPageBit(Page) & View->Valid;
        View->MapRefs--;
    }
    KeReleaseSpinLock(&QzCcLock, OldIrql);
}

/* Written through a mapping that stays */
VOID
QzCcMmSetDirty(PVOID MapVoid, LONGLONG Offset)
{
    PQZ_VIEW View;
    KIRQL OldIrql;

    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    View = QzCcFindView(MapVoid, (ULONGLONG)Offset >> QZ_VIEW_SHIFT);
    if (View)
        View->Dirty |= QzPageBit((ULONG)((Offset & (QZ_VIEW_SIZE - 1)) / PAGE_SIZE)) & View->Valid;
    KeReleaseSpinLock(&QzCcLock, OldIrql);
}

PSECTION_OBJECT_POINTERS
QzCcMmSop(PVOID MapVoid)
{
    return ((PQZ_CACHE_MAP)MapVoid)->Sop;
}

/* The shell's "cache": what is cached */
VOID
QzCcDump(VOID)
{
    PLIST_ENTRY Entry;
    ULONG Maps = 0, Views = 0, Pages = 0, Dirty = 0;
    KIRQL OldIrql;

    if (!QzCcReady)
        return;
    KeAcquireSpinLock(&QzCcLock, &OldIrql);
    for (Entry = QzCcMaps.Flink; Entry != &QzCcMaps; Entry = Entry->Flink)
    {
        PQZ_CACHE_MAP Map = CONTAINING_RECORD(Entry, QZ_CACHE_MAP, Link);
        PLIST_ENTRY ViewEntry;
        ULONG MapPages = 0;
        Maps++;
        for (ViewEntry = Map->Views.Flink; ViewEntry != &Map->Views; ViewEntry = ViewEntry->Flink)
        {
            PQZ_VIEW View = CONTAINING_RECORD(ViewEntry, QZ_VIEW, MapLink);
            ULONG p;
            QzCcHarvest(View);
            Views++;
            for (p = 0; p < QZ_VIEW_PAGES; p++)
            {
                if (View->Valid & QzPageBit(p))
                    MapPages++;
                if (View->Dirty & View->Valid & QzPageBit(p))
                    Dirty++;
            }
        }
        Pages += MapPages;
        if (Map->FileObject->FileName.Length)
            QzPrint("  %-40wZ", &Map->FileObject->FileName);
        else
            QzPrint("  %-40s", "(stream)");
        QzPrint(" %6lu KB cached, size %I64u%s\n", MapPages * 4, Map->FileSize.QuadPart,
                Map->Teardown ? " (closing)" : "");
    }
    KeReleaseSpinLock(&QzCcLock, OldIrql);
    QzPrint("  %lu cache map(s), %lu view(s), %lu KB cached, %lu KB dirty, %lu lazy writer pass(es)\n", Maps, Views,
            Pages * 4, Dirty * 4, QzCcLazyPasses);
}
