/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     File system runtime: mapping control blocks (VBN -> LBN)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * An MCB maps a file's sectors (VBNs) to the volume's (LBNs). Its runs
 * cover VBN 0 up to the highest one mapped without gaps; the gaps are
 * holes (LBN -1) and count as runs. The BASE_MCB's Mapping is an array
 * of PairCount pairs, each the VBN where a run ends and the LBN where it
 * starts: run i covers [NextVbn(i - 1), NextVbn(i)). Changes rebuild the
 * array with the changed range in place and merge neighbours that
 * continue each other.
 */

#include "qz.h"

#define QZ_MCB_TAG 'bMzQ'

typedef struct _QZ_MCB_PAIR
{
    LONGLONG NextVbn;
    LONGLONG Lbn;                   /* -1: a hole */
} QZ_MCB_PAIR, *PQZ_MCB_PAIR;

#define QzPairs(Mcb) ((PQZ_MCB_PAIR)(Mcb)->Mapping)
#define QzRunStart(Mcb, i) ((i) ? QzPairs(Mcb)[(i) - 1].NextVbn : 0)

static VOID
QzMcbLock(PLARGE_MCB Mcb)
{
    ExAcquireFastMutex((PFAST_MUTEX)Mcb->GuardedMutex);
}

static VOID
QzMcbUnlock(PLARGE_MCB Mcb)
{
    ExReleaseFastMutex((PFAST_MUTEX)Mcb->GuardedMutex);
}

VOID
NTAPI
FsRtlInitializeBaseMcb(IN PBASE_MCB Mcb, IN POOL_TYPE PoolType)
{
    Mcb->PairCount = 0;
    Mcb->MaximumPairCount = 0;
    Mcb->PoolType = (USHORT)PoolType;
    Mcb->Flags = 0;
    Mcb->Mapping = NULL;
}

VOID
NTAPI
FsRtlUninitializeBaseMcb(IN PBASE_MCB Mcb)
{
    if (Mcb->Mapping)
        ExFreePoolWithTag(Mcb->Mapping, QZ_MCB_TAG);
    Mcb->Mapping = NULL;
    Mcb->PairCount = Mcb->MaximumPairCount = 0;
}

VOID
NTAPI
FsRtlInitializeLargeMcb(IN PLARGE_MCB Mcb, IN POOL_TYPE PoolType)
{
    PFAST_MUTEX Mutex = ExAllocatePoolWithTag(NonPagedPool, sizeof(FAST_MUTEX), QZ_MCB_TAG);
    if (!Mutex)
        ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    ExInitializeFastMutex(Mutex);
    Mcb->GuardedMutex = (PKGUARDED_MUTEX)Mutex;
    FsRtlInitializeBaseMcb(&Mcb->BaseMcb, PoolType);
}

VOID
NTAPI
FsRtlUninitializeLargeMcb(IN PLARGE_MCB Mcb)
{
    if (Mcb->GuardedMutex)
    {
        FsRtlUninitializeBaseMcb(&Mcb->BaseMcb);
        ExFreePoolWithTag(Mcb->GuardedMutex, QZ_MCB_TAG);
        Mcb->GuardedMutex = NULL;
    }
}

/* Room for Count pairs (the old ones kept) */
static BOOLEAN
QzMcbReserve(PBASE_MCB Mcb, ULONG Count)
{
    PQZ_MCB_PAIR Pairs;
    ULONG Capacity;

    if (Count <= Mcb->MaximumPairCount)
        return TRUE;
    Capacity = max(Count, Mcb->MaximumPairCount * 2);
    Capacity = max(Capacity, 16);
    Pairs = ExAllocatePoolWithTag(Mcb->PoolType == PagedPool ? PagedPool : NonPagedPool,
                                  Capacity * sizeof(QZ_MCB_PAIR), QZ_MCB_TAG);
    if (!Pairs)
        return FALSE;
    if (Mcb->Mapping)
    {
        RtlCopyMemory(Pairs, Mcb->Mapping, Mcb->PairCount * sizeof(QZ_MCB_PAIR));
        ExFreePoolWithTag(Mcb->Mapping, QZ_MCB_TAG);
    }
    Mcb->Mapping = Pairs;
    Mcb->MaximumPairCount = Capacity;
    return TRUE;
}

/* Set [Vbn, End) to start at Lbn (-1: a hole), then merge and drop holes at the end */
static BOOLEAN
QzMcbSetRange(PBASE_MCB Mcb, LONGLONG Vbn, LONGLONG End, LONGLONG Lbn)
{
    PQZ_MCB_PAIR Old = QzPairs(Mcb), New;
    ULONG OldCount = Mcb->PairCount, Count = 0, i;
    LONGLONG Last = OldCount ? Old[OldCount - 1].NextVbn : 0;

    New = ExAllocatePoolWithTag(NonPagedPool, (OldCount + 3) * sizeof(QZ_MCB_PAIR), QZ_MCB_TAG);
    if (!New)
        return FALSE;

    /* Runs (or their parts) before Vbn */
    for (i = 0; i < OldCount; i++)
    {
        LONGLONG Start = QzRunStart(Mcb, i);
        if (Start >= Vbn)
            break;
        New[Count].NextVbn = min(Old[i].NextVbn, Vbn);
        New[Count].Lbn = Old[i].Lbn;
        Count++;
    }
    if (Last < Vbn)
    {
        New[Count].NextVbn = Vbn;           /* a hole up to the new range */
        New[Count].Lbn = -1;
        Count++;
    }
    New[Count].NextVbn = End;
    New[Count].Lbn = Lbn;
    Count++;

    /* Runs (or their parts) after End */
    for (i = 0; i < OldCount; i++)
    {
        LONGLONG Start = QzRunStart(Mcb, i);
        if (Old[i].NextVbn <= End)
            continue;
        New[Count].NextVbn = Old[i].NextVbn;
        New[Count].Lbn = Old[i].Lbn == -1 ? -1 : Old[i].Lbn + (max(Start, End) - Start);
        Count++;
    }

    /* Merge runs that continue each other */
    {
        ULONG Out = 0;
        for (i = 0; i < Count; i++)
        {
            LONGLONG Start = Out ? New[Out - 1].NextVbn : 0;
            if (New[i].NextVbn <= Start)
                continue;                   /* empty */
            if (Out)
            {
                LONGLONG PrevStart = Out > 1 ? New[Out - 2].NextVbn : 0;
                PQZ_MCB_PAIR Prev = &New[Out - 1];
                if ((Prev->Lbn == -1 && New[i].Lbn == -1) ||
                    (Prev->Lbn != -1 && New[i].Lbn != -1 && Prev->Lbn + (Prev->NextVbn - PrevStart) == New[i].Lbn))
                {
                    Prev->NextVbn = New[i].NextVbn;
                    continue;
                }
            }
            New[Out++] = New[i];
        }
        Count = Out;
        while (Count && New[Count - 1].Lbn == -1)
            Count--;                        /* no holes at the end */
    }

    if (!QzMcbReserve(Mcb, Count))
    {
        ExFreePoolWithTag(New, QZ_MCB_TAG);
        return FALSE;
    }
    RtlCopyMemory(Mcb->Mapping, New, Count * sizeof(QZ_MCB_PAIR));
    Mcb->PairCount = Count;
    ExFreePoolWithTag(New, QZ_MCB_TAG);
    return TRUE;
}

/* The run Vbn is in, or MAXULONG past the end */
static ULONG
QzMcbFindRun(PBASE_MCB Mcb, LONGLONG Vbn)
{
    LONG Low = 0, High = (LONG)Mcb->PairCount - 1;
    while (Low <= High)
    {
        LONG Middle = (Low + High) / 2;
        if (QzPairs(Mcb)[Middle].NextVbn <= Vbn)
            Low = Middle + 1;
        else if (QzRunStart(Mcb, Middle) > Vbn)
            High = Middle - 1;
        else
            return (ULONG)Middle;
    }
    return MAXULONG;
}

BOOLEAN
NTAPI
FsRtlAddBaseMcbEntry(IN PBASE_MCB Mcb, IN LONGLONG Vbn, IN LONGLONG Lbn, IN LONGLONG SectorCount)
{
    LONGLONG End = Vbn + SectorCount, At;

    if (Vbn < 0 || Lbn < 0 || SectorCount <= 0)
        return FALSE;
    /* What is mapped already must agree */
    for (At = Vbn; At < End;)
    {
        ULONG Run = QzMcbFindRun(Mcb, At);
        LONGLONG Start, RunEnd;
        if (Run == MAXULONG)
            break;
        Start = QzRunStart(Mcb, Run);
        RunEnd = QzPairs(Mcb)[Run].NextVbn;
        if (QzPairs(Mcb)[Run].Lbn != -1 && QzPairs(Mcb)[Run].Lbn + (At - Start) != Lbn + (At - Vbn))
            return FALSE;
        At = RunEnd;
    }
    return QzMcbSetRange(Mcb, Vbn, End, Lbn);
}

BOOLEAN
NTAPI
FsRtlAddLargeMcbEntry(IN PLARGE_MCB Mcb, IN LONGLONG Vbn, IN LONGLONG Lbn, IN LONGLONG SectorCount)
{
    BOOLEAN Result;
    QzMcbLock(Mcb);
    Result = FsRtlAddBaseMcbEntry(&Mcb->BaseMcb, Vbn, Lbn, SectorCount);
    QzMcbUnlock(Mcb);
    return Result;
}

BOOLEAN
NTAPI
FsRtlRemoveBaseMcbEntry(IN PBASE_MCB Mcb, IN LONGLONG Vbn, IN LONGLONG SectorCount)
{
    LONGLONG Last = Mcb->PairCount ? QzPairs(Mcb)[Mcb->PairCount - 1].NextVbn : 0;
    if (SectorCount <= 0 || Vbn >= Last)
        return TRUE;
    return QzMcbSetRange(Mcb, Vbn, min(Vbn + SectorCount, Last), -1);
}

VOID
NTAPI
FsRtlRemoveLargeMcbEntry(IN PLARGE_MCB Mcb, IN LONGLONG Vbn, IN LONGLONG SectorCount)
{
    QzMcbLock(Mcb);
    FsRtlRemoveBaseMcbEntry(&Mcb->BaseMcb, Vbn, SectorCount);
    QzMcbUnlock(Mcb);
}

VOID
NTAPI
FsRtlTruncateBaseMcb(IN PBASE_MCB Mcb, IN LONGLONG Vbn)
{
    ULONG i;
    for (i = 0; i < Mcb->PairCount; i++)
    {
        if (QzPairs(Mcb)[i].NextVbn >= Vbn)
        {
            if (QzRunStart(Mcb, i) >= Vbn)
                break;                      /* this run goes */
            QzPairs(Mcb)[i].NextVbn = Vbn;  /* it ends at Vbn now */
            i++;
            break;
        }
    }
    Mcb->PairCount = i;
    while (Mcb->PairCount && QzPairs(Mcb)[Mcb->PairCount - 1].Lbn == -1)
        Mcb->PairCount--;
}

VOID
NTAPI
FsRtlTruncateLargeMcb(IN PLARGE_MCB Mcb, IN LONGLONG Vbn)
{
    QzMcbLock(Mcb);
    FsRtlTruncateBaseMcb(&Mcb->BaseMcb, Vbn);
    QzMcbUnlock(Mcb);
}

VOID
NTAPI
FsRtlResetLargeMcb(IN PLARGE_MCB Mcb, IN BOOLEAN SelfSynchronized)
{
    if (!SelfSynchronized)
        QzMcbLock(Mcb);
    Mcb->BaseMcb.PairCount = 0;
    if (!SelfSynchronized)
        QzMcbUnlock(Mcb);
}

BOOLEAN
NTAPI
FsRtlLookupBaseMcbEntry(IN PBASE_MCB Mcb, IN LONGLONG Vbn, OUT PLONGLONG Lbn OPTIONAL,
                        OUT PLONGLONG SectorCountFromLbn OPTIONAL, OUT PLONGLONG StartingLbn OPTIONAL,
                        OUT PLONGLONG SectorCountFromStartingLbn OPTIONAL, OUT PULONG Index OPTIONAL)
{
    ULONG Run = QzMcbFindRun(Mcb, Vbn);
    LONGLONG Start, RunLbn;

    if (Run == MAXULONG)
        return FALSE;
    Start = QzRunStart(Mcb, Run);
    RunLbn = QzPairs(Mcb)[Run].Lbn;
    if (Lbn)
        *Lbn = RunLbn == -1 ? -1 : RunLbn + (Vbn - Start);
    if (SectorCountFromLbn)
        *SectorCountFromLbn = QzPairs(Mcb)[Run].NextVbn - Vbn;
    if (StartingLbn)
        *StartingLbn = RunLbn;
    if (SectorCountFromStartingLbn)
        *SectorCountFromStartingLbn = QzPairs(Mcb)[Run].NextVbn - Start;
    if (Index)
        *Index = Run;
    return TRUE;
}

BOOLEAN
NTAPI
FsRtlLookupLargeMcbEntry(IN PLARGE_MCB Mcb, IN LONGLONG Vbn, OUT PLONGLONG Lbn OPTIONAL,
                         OUT PLONGLONG SectorCountFromLbn OPTIONAL, OUT PLONGLONG StartingLbn OPTIONAL,
                         OUT PLONGLONG SectorCountFromStartingLbn OPTIONAL, OUT PULONG Index OPTIONAL)
{
    BOOLEAN Result;
    QzMcbLock(Mcb);
    Result = FsRtlLookupBaseMcbEntry(&Mcb->BaseMcb, Vbn, Lbn, SectorCountFromLbn, StartingLbn,
                                     SectorCountFromStartingLbn, Index);
    QzMcbUnlock(Mcb);
    return Result;
}

BOOLEAN
NTAPI
FsRtlLookupLastBaseMcbEntryAndIndex(IN PBASE_MCB Mcb, OUT PLONGLONG LargeVbn, OUT PLONGLONG LargeLbn,
                                    OUT PULONG Index)
{
    ULONG Last;
    if (!Mcb->PairCount)
        return FALSE;
    Last = Mcb->PairCount - 1;
    *LargeVbn = QzPairs(Mcb)[Last].NextVbn - 1;
    *LargeLbn = QzPairs(Mcb)[Last].Lbn + (QzPairs(Mcb)[Last].NextVbn - QzRunStart(Mcb, Last) - 1);
    if (Index)
        *Index = Last;
    return TRUE;
}

BOOLEAN
NTAPI
FsRtlLookupLastLargeMcbEntryAndIndex(IN PLARGE_MCB Mcb, OUT PLONGLONG LargeVbn, OUT PLONGLONG LargeLbn,
                                     OUT PULONG Index)
{
    BOOLEAN Result;
    QzMcbLock(Mcb);
    Result = FsRtlLookupLastBaseMcbEntryAndIndex(&Mcb->BaseMcb, LargeVbn, LargeLbn, Index);
    QzMcbUnlock(Mcb);
    return Result;
}

BOOLEAN
NTAPI
FsRtlLookupLastLargeMcbEntry(IN PLARGE_MCB Mcb, OUT PLONGLONG Vbn, OUT PLONGLONG Lbn)
{
    ULONG Index;
    return FsRtlLookupLastLargeMcbEntryAndIndex(Mcb, Vbn, Lbn, &Index);
}

ULONG
NTAPI
FsRtlNumberOfRunsInLargeMcb(IN PLARGE_MCB Mcb)
{
    ULONG Count;
    QzMcbLock(Mcb);
    Count = Mcb->BaseMcb.PairCount;
    QzMcbUnlock(Mcb);
    return Count;
}

ULONG
NTAPI
FsRtlNumberOfRunsInBaseMcb(IN PBASE_MCB Mcb)
{
    return Mcb->PairCount;
}

BOOLEAN
NTAPI
FsRtlGetNextBaseMcbEntry(IN PBASE_MCB Mcb, IN ULONG RunIndex, OUT PLONGLONG Vbn, OUT PLONGLONG Lbn,
                         OUT PLONGLONG SectorCount)
{
    if (RunIndex >= Mcb->PairCount)
        return FALSE;
    *Vbn = QzRunStart(Mcb, RunIndex);
    *Lbn = QzPairs(Mcb)[RunIndex].Lbn;
    *SectorCount = QzPairs(Mcb)[RunIndex].NextVbn - *Vbn;
    return TRUE;
}

BOOLEAN
NTAPI
FsRtlGetNextLargeMcbEntry(IN PLARGE_MCB Mcb, IN ULONG RunIndex, OUT PLONGLONG Vbn, OUT PLONGLONG Lbn,
                          OUT PLONGLONG SectorCount)
{
    BOOLEAN Result;
    QzMcbLock(Mcb);
    Result = FsRtlGetNextBaseMcbEntry(&Mcb->BaseMcb, RunIndex, Vbn, Lbn, SectorCount);
    QzMcbUnlock(Mcb);
    return Result;
}

/* Open a hole of Amount sectors at Vbn: what was there moves up */
BOOLEAN
NTAPI
FsRtlSplitLargeMcb(IN PLARGE_MCB Mcb, IN LONGLONG Vbn, IN LONGLONG Amount)
{
    PBASE_MCB Base = &Mcb->BaseMcb;
    ULONG Run, i;
    BOOLEAN Result = TRUE;

    QzMcbLock(Mcb);
    Run = QzMcbFindRun(Base, Vbn);
    if (Run != MAXULONG)
    {
        LONGLONG Start = QzRunStart(Base, Run);
        if (Start < Vbn)
        {
            /* Cut the run at Vbn first */
            LONGLONG Lbn = QzPairs(Base)[Run].Lbn;
            Result = QzMcbReserve(Base, Base->PairCount + 1);
            if (Result)
            {
                RtlMoveMemory(&QzPairs(Base)[Run + 1], &QzPairs(Base)[Run],
                              (Base->PairCount - Run) * sizeof(QZ_MCB_PAIR));
                Base->PairCount++;
                QzPairs(Base)[Run].NextVbn = Vbn;
                QzPairs(Base)[Run + 1].Lbn = Lbn == -1 ? -1 : Lbn + (Vbn - Start);
                Run++;
            }
        }
        if (Result)
        {
            for (i = Run; i < Base->PairCount; i++)
                QzPairs(Base)[i].NextVbn += Amount;
            /* The hole before the moved runs */
            Result = QzMcbReserve(Base, Base->PairCount + 1);
            if (Result)
            {
                RtlMoveMemory(&QzPairs(Base)[Run + 1], &QzPairs(Base)[Run],
                              (Base->PairCount - Run) * sizeof(QZ_MCB_PAIR));
                Base->PairCount++;
                QzPairs(Base)[Run].NextVbn = Vbn + Amount;
                QzPairs(Base)[Run].Lbn = -1;
            }
        }
    }
    QzMcbUnlock(Mcb);
    return Result;
}
