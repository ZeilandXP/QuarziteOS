/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Executive synchronization helpers drivers import:
 *              interlocked lists, S-lists, lookaside lists, rundown
 *              protection, guarded mutexes, interrupt spin locks
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * One processor: the interlocked list operations take the caller's spin
 * lock (raising to DISPATCH_LEVEL) and the S-list operations a private
 * one, which is all the atomicity needed. Interrupt-level callers are not
 * expected for these on x86 NT either.
 */

#include "qzio.h"

#undef ExInterlockedAddUlong
#undef ExInterlockedAddLargeStatistic
#undef ExInterlockedInsertHeadList
#undef ExInterlockedInsertTailList
#undef ExInterlockedRemoveHeadList
#undef ExInterlockedPopEntryList
#undef ExInterlockedPushEntryList
#undef ExInterlockedFlushSList
#undef ExInterlockedPopEntrySList
#undef ExInterlockedPushEntrySList
#undef InterlockedPopEntrySList
#undef InterlockedPushEntrySList

#ifndef EX_RUNDOWN_ACTIVE
#define EX_RUNDOWN_ACTIVE     0x1
#define EX_RUNDOWN_COUNT_INC  0x2
#endif


/* Interrupts off as well: S-lists are used from ISRs on NT */
static ULONG
QzSListLockAcquire(VOID)
{
    ULONG Flags = __readeflags();
    _disable();
    return Flags;
}

static VOID
QzSListLockRelease(ULONG Flags)
{
    __writeeflags(Flags);
}

/* ---------------------------------------------------------------------- */
/* Interlocked doubly/singly linked lists                                  */
/* ---------------------------------------------------------------------- */

PLIST_ENTRY
FASTCALL
ExfInterlockedInsertHeadList(IN OUT PLIST_ENTRY ListHead, IN OUT PLIST_ENTRY ListEntry,
                             IN OUT PKSPIN_LOCK Lock)
{
    KIRQL OldIrql = QzAcquireSpinLock(Lock);
    PLIST_ENTRY First = IsListEmpty(ListHead) ? NULL : ListHead->Flink;
    InsertHeadList(ListHead, ListEntry);
    QzReleaseSpinLock(Lock, OldIrql);
    return First;
}

PLIST_ENTRY
FASTCALL
ExfInterlockedInsertTailList(IN OUT PLIST_ENTRY ListHead, IN OUT PLIST_ENTRY ListEntry,
                             IN OUT PKSPIN_LOCK Lock)
{
    KIRQL OldIrql = QzAcquireSpinLock(Lock);
    PLIST_ENTRY Last = IsListEmpty(ListHead) ? NULL : ListHead->Blink;
    InsertTailList(ListHead, ListEntry);
    QzReleaseSpinLock(Lock, OldIrql);
    return Last;
}

PLIST_ENTRY
FASTCALL
ExfInterlockedRemoveHeadList(IN OUT PLIST_ENTRY ListHead, IN OUT PKSPIN_LOCK Lock)
{
    KIRQL OldIrql = QzAcquireSpinLock(Lock);
    PLIST_ENTRY Entry = IsListEmpty(ListHead) ? NULL : RemoveHeadList(ListHead);
    QzReleaseSpinLock(Lock, OldIrql);
    return Entry;
}

PSINGLE_LIST_ENTRY
FASTCALL
ExfInterlockedPushEntryList(IN OUT PSINGLE_LIST_ENTRY ListHead, IN OUT PSINGLE_LIST_ENTRY ListEntry,
                            IN OUT PKSPIN_LOCK Lock)
{
    KIRQL OldIrql = QzAcquireSpinLock(Lock);
    PSINGLE_LIST_ENTRY First = ListHead->Next;
    PushEntryList(ListHead, ListEntry);
    QzReleaseSpinLock(Lock, OldIrql);
    return First;
}

PSINGLE_LIST_ENTRY
FASTCALL
ExfInterlockedPopEntryList(IN OUT PSINGLE_LIST_ENTRY ListHead, IN OUT PKSPIN_LOCK Lock)
{
    KIRQL OldIrql = QzAcquireSpinLock(Lock);
    PSINGLE_LIST_ENTRY Entry = PopEntryList(ListHead);
    QzReleaseSpinLock(Lock, OldIrql);
    return Entry;
}

/* The stdcall versions of the same */
PLIST_ENTRY NTAPI
ExInterlockedInsertHeadList(PLIST_ENTRY ListHead, PLIST_ENTRY ListEntry, PKSPIN_LOCK Lock)
{
    return ExfInterlockedInsertHeadList(ListHead, ListEntry, Lock);
}

PLIST_ENTRY NTAPI
ExInterlockedInsertTailList(PLIST_ENTRY ListHead, PLIST_ENTRY ListEntry, PKSPIN_LOCK Lock)
{
    return ExfInterlockedInsertTailList(ListHead, ListEntry, Lock);
}

PLIST_ENTRY NTAPI
ExInterlockedRemoveHeadList(PLIST_ENTRY ListHead, PKSPIN_LOCK Lock)
{
    return ExfInterlockedRemoveHeadList(ListHead, Lock);
}

PSINGLE_LIST_ENTRY NTAPI
ExInterlockedPushEntryList(PSINGLE_LIST_ENTRY ListHead, PSINGLE_LIST_ENTRY ListEntry, PKSPIN_LOCK Lock)
{
    return ExfInterlockedPushEntryList(ListHead, ListEntry, Lock);
}

PSINGLE_LIST_ENTRY NTAPI
ExInterlockedPopEntryList(PSINGLE_LIST_ENTRY ListHead, PKSPIN_LOCK Lock)
{
    return ExfInterlockedPopEntryList(ListHead, Lock);
}

ULONG
FASTCALL
ExfInterlockedAddUlong(IN OUT PULONG Addend, IN ULONG Increment, IN OUT PKSPIN_LOCK Lock)
{
    KIRQL OldIrql = QzAcquireSpinLock(Lock);
    ULONG Old = *Addend;
    *Addend += Increment;
    QzReleaseSpinLock(Lock, OldIrql);
    return Old;
}

ULONG NTAPI
ExInterlockedAddUlong(PULONG Addend, ULONG Increment, PKSPIN_LOCK Lock)
{
    return ExfInterlockedAddUlong(Addend, Increment, Lock);
}

LARGE_INTEGER NTAPI
ExInterlockedAddLargeInteger(PLARGE_INTEGER Addend, LARGE_INTEGER Increment, PKSPIN_LOCK Lock)
{
    KIRQL OldIrql = QzAcquireSpinLock(Lock);
    LARGE_INTEGER Old = *Addend;
    Addend->QuadPart += Increment.QuadPart;
    QzReleaseSpinLock(Lock, OldIrql);
    return Old;
}

VOID
FASTCALL
ExInterlockedAddLargeStatistic(IN PLARGE_INTEGER Addend, IN ULONG Increment)
{
    ULONG Flags = QzSListLockAcquire();
    Addend->QuadPart += Increment;
    QzSListLockRelease(Flags);
}

/* ---------------------------------------------------------------------- */
/* S-lists                                                                 */
/* ---------------------------------------------------------------------- */

PSLIST_ENTRY
FASTCALL
InterlockedPushEntrySList(IN OUT PSLIST_HEADER ListHead, IN OUT PSLIST_ENTRY ListEntry)
{
    ULONG Flags = QzSListLockAcquire();
    PSLIST_ENTRY First = ListHead->Next.Next;
    ListEntry->Next = First;
    ListHead->Next.Next = ListEntry;
    ListHead->Depth++;
    ListHead->Sequence++;
    QzSListLockRelease(Flags);
    return First;
}

PSLIST_ENTRY
FASTCALL
InterlockedPopEntrySList(IN OUT PSLIST_HEADER ListHead)
{
    ULONG Flags = QzSListLockAcquire();
    PSLIST_ENTRY First = ListHead->Next.Next;
    if (First)
    {
        ListHead->Next.Next = First->Next;
        ListHead->Depth--;
        ListHead->Sequence++;
    }
    QzSListLockRelease(Flags);
    return First;
}

PSLIST_ENTRY
FASTCALL
ExInterlockedFlushSList(IN OUT PSLIST_HEADER ListHead)
{
    ULONG Flags = QzSListLockAcquire();
    PSLIST_ENTRY First = ListHead->Next.Next;
    ListHead->Next.Next = NULL;
    ListHead->Depth = 0;
    ListHead->Sequence++;
    QzSListLockRelease(Flags);
    return First;
}

PSLIST_ENTRY
FASTCALL
ExInterlockedPushEntrySList(IN OUT PSLIST_HEADER ListHead, IN OUT PSLIST_ENTRY ListEntry,
                            IN OUT PKSPIN_LOCK Lock)
{
    UNREFERENCED_PARAMETER(Lock);
    return InterlockedPushEntrySList(ListHead, ListEntry);
}

PSLIST_ENTRY
FASTCALL
ExInterlockedPopEntrySList(IN OUT PSLIST_HEADER ListHead, IN OUT PKSPIN_LOCK Lock)
{
    UNREFERENCED_PARAMETER(Lock);
    return InterlockedPopEntrySList(ListHead);
}

/* ---------------------------------------------------------------------- */
/* Lookaside lists                                                         */
/* ---------------------------------------------------------------------- */

static PVOID NTAPI
QzLookasideAllocate(POOL_TYPE PoolType, SIZE_T Size, ULONG Tag)
{
    return ExAllocatePoolWithTag(PoolType, Size, Tag);
}

static VOID NTAPI
QzLookasideFree(PVOID Buffer)
{
    ExFreePool(Buffer);
}

static VOID
QzInitializeLookaside(PGENERAL_LOOKASIDE List, PALLOCATE_FUNCTION Allocate, PFREE_FUNCTION Free,
                      POOL_TYPE PoolType, ULONG Flags, SIZE_T Size, ULONG Tag, USHORT Depth)
{
    UNREFERENCED_PARAMETER(Flags);
    RtlZeroMemory(List, sizeof(*List));
    List->ListHead.Next.Next = NULL;
    List->Depth = 4;
    List->MaximumDepth = Depth ? Depth : 256;
    List->Type = PoolType;
    List->Tag = Tag;
    List->Size = (ULONG)max(Size, sizeof(SLIST_ENTRY));
    List->Allocate = Allocate ? Allocate : QzLookasideAllocate;
    List->Free = Free ? Free : QzLookasideFree;
    InitializeListHead(&List->ListEntry);
}

VOID
NTAPI
ExInitializeNPagedLookasideList(IN PNPAGED_LOOKASIDE_LIST Lookaside, IN PALLOCATE_FUNCTION Allocate,
                                IN PFREE_FUNCTION Free, IN ULONG Flags, IN SIZE_T Size,
                                IN ULONG Tag, IN USHORT Depth)
{
    QzInitializeLookaside(&Lookaside->L, Allocate, Free, NonPagedPool, Flags, Size, Tag, Depth);
}

VOID
NTAPI
ExInitializePagedLookasideList(IN PPAGED_LOOKASIDE_LIST Lookaside, IN PALLOCATE_FUNCTION Allocate,
                               IN PFREE_FUNCTION Free, IN ULONG Flags, IN SIZE_T Size,
                               IN ULONG Tag, IN USHORT Depth)
{
    QzInitializeLookaside(&Lookaside->L, Allocate, Free, PagedPool, Flags, Size, Tag, Depth);
}

static VOID
QzDeleteLookaside(PGENERAL_LOOKASIDE List)
{
    PSLIST_ENTRY Entry;
    while ((Entry = InterlockedPopEntrySList(&List->ListHead)))
        List->Free(Entry);
}

VOID
NTAPI
ExDeleteNPagedLookasideList(IN PNPAGED_LOOKASIDE_LIST Lookaside)
{
    QzDeleteLookaside(&Lookaside->L);
}

VOID
NTAPI
ExDeletePagedLookasideList(IN PPAGED_LOOKASIDE_LIST Lookaside)
{
    QzDeleteLookaside(&Lookaside->L);
}

PVOID
NTAPI
ExiAllocateFromPagedLookasideList(IN PPAGED_LOOKASIDE_LIST Lookaside)
{
    PVOID Entry;
    Lookaside->L.TotalAllocates++;
    Entry = InterlockedPopEntrySList(&Lookaside->L.ListHead);
    if (!Entry)
    {
        Lookaside->L.AllocateMisses++;
        Entry = Lookaside->L.Allocate(Lookaside->L.Type, Lookaside->L.Size, Lookaside->L.Tag);
    }
    return Entry;
}

VOID
NTAPI
ExiFreeToPagedLookasideList(IN PPAGED_LOOKASIDE_LIST Lookaside, IN PVOID Entry)
{
    Lookaside->L.TotalFrees++;
    if (ExQueryDepthSList(&Lookaside->L.ListHead) >= Lookaside->L.Depth)
    {
        Lookaside->L.FreeMisses++;
        Lookaside->L.Free(Entry);
    }
    else
    {
        InterlockedPushEntrySList(&Lookaside->L.ListHead, Entry);
    }
}

/* ---------------------------------------------------------------------- */
/* Rundown protection                                                      */
/* ---------------------------------------------------------------------- */

VOID
FASTCALL
ExfInitializeRundownProtection(OUT PEX_RUNDOWN_REF RunRef)
{
    RunRef->Count = 0;
}

VOID
FASTCALL
ExfReInitializeRundownProtection(OUT PEX_RUNDOWN_REF RunRef)
{
    RunRef->Count = 0;
}

BOOLEAN
FASTCALL
ExfAcquireRundownProtectionEx(IN OUT PEX_RUNDOWN_REF RunRef, IN ULONG Count)
{
    for (;;)
    {
        ULONG_PTR Value = RunRef->Count;
        if (Value & EX_RUNDOWN_ACTIVE)
            return FALSE;
        if (InterlockedCompareExchangePointer(&RunRef->Ptr, (PVOID)(Value + Count * EX_RUNDOWN_COUNT_INC),
                                              (PVOID)Value) == (PVOID)Value)
            return TRUE;
    }
}

BOOLEAN
FASTCALL
ExfAcquireRundownProtection(IN OUT PEX_RUNDOWN_REF RunRef)
{
    return ExfAcquireRundownProtectionEx(RunRef, 1);
}

VOID
FASTCALL
ExfReleaseRundownProtectionEx(IN OUT PEX_RUNDOWN_REF RunRef, IN ULONG Count)
{
    InterlockedExchangeAdd((PLONG)&RunRef->Count, -(LONG)(Count * EX_RUNDOWN_COUNT_INC));
}

VOID
FASTCALL
ExfReleaseRundownProtection(IN OUT PEX_RUNDOWN_REF RunRef)
{
    ExfReleaseRundownProtectionEx(RunRef, 1);
}

VOID
FASTCALL
ExfWaitForRundownProtectionRelease(IN OUT PEX_RUNDOWN_REF RunRef)
{
    LARGE_INTEGER Interval;

    InterlockedOr((PLONG)&RunRef->Count, EX_RUNDOWN_ACTIVE);
    Interval.QuadPart = -10000;   /* 1 ms */
    while (RunRef->Count != EX_RUNDOWN_ACTIVE)
        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

/* The cache-aware variant is the plain one on a single processor: the
   NDK structure with one EX_RUNDOWN_REF right behind it */
SIZE_T
NTAPI
ExSizeOfRundownProtectionCacheAware(VOID)
{
    return sizeof(EX_RUNDOWN_REF_CACHE_AWARE) + sizeof(EX_RUNDOWN_REF);
}

VOID
NTAPI
ExInitializeRundownProtectionCacheAware(IN PEX_RUNDOWN_REF_CACHE_AWARE RunRefCacheAware,
                                        IN SIZE_T Size)
{
    UNREFERENCED_PARAMETER(Size);
    RunRefCacheAware->RunRefs = (PEX_RUNDOWN_REF)(RunRefCacheAware + 1);
    RunRefCacheAware->PoolToFree = NULL;
    RunRefCacheAware->RunRefSize = sizeof(EX_RUNDOWN_REF);
    RunRefCacheAware->Number = 1;
    RunRefCacheAware->RunRefs->Count = 0;
}

PEX_RUNDOWN_REF_CACHE_AWARE
NTAPI
ExAllocateCacheAwareRundownProtection(IN POOL_TYPE PoolType, IN ULONG Tag)
{
    PEX_RUNDOWN_REF_CACHE_AWARE Ref = ExAllocatePoolWithTag(PoolType,
                                                            ExSizeOfRundownProtectionCacheAware(), Tag);
    if (Ref)
    {
        ExInitializeRundownProtectionCacheAware(Ref, 0);
        Ref->PoolToFree = Ref;
    }
    return Ref;
}

VOID
NTAPI
ExFreeCacheAwareRundownProtection(IN PEX_RUNDOWN_REF_CACHE_AWARE RunRefCacheAware)
{
    ExFreePool(RunRefCacheAware->PoolToFree);
}

BOOLEAN FASTCALL
ExfAcquireRundownProtectionCacheAware(IN PEX_RUNDOWN_REF_CACHE_AWARE RunRef)
{
    return ExfAcquireRundownProtectionEx(RunRef->RunRefs, 1);
}

BOOLEAN FASTCALL
ExfAcquireRundownProtectionCacheAwareEx(IN PEX_RUNDOWN_REF_CACHE_AWARE RunRef, IN ULONG Count)
{
    return ExfAcquireRundownProtectionEx(RunRef->RunRefs, Count);
}

VOID FASTCALL
ExfReleaseRundownProtectionCacheAware(IN PEX_RUNDOWN_REF_CACHE_AWARE RunRef)
{
    ExfReleaseRundownProtectionEx(RunRef->RunRefs, 1);
}

VOID FASTCALL
ExfReleaseRundownProtectionCacheAwareEx(IN PEX_RUNDOWN_REF_CACHE_AWARE RunRef, IN ULONG Count)
{
    ExfReleaseRundownProtectionEx(RunRef->RunRefs, Count);
}

VOID FASTCALL
ExfWaitForRundownProtectionReleaseCacheAware(IN PEX_RUNDOWN_REF_CACHE_AWARE RunRef)
{
    ExfWaitForRundownProtectionRelease(RunRef->RunRefs);
}

VOID FASTCALL
ExfReInitializeRundownProtectionCacheAware(IN PEX_RUNDOWN_REF_CACHE_AWARE RunRef)
{
    RunRef->RunRefs->Count = 0;
}

/* ---------------------------------------------------------------------- */
/* Guarded mutexes and regions                                             */
/* ---------------------------------------------------------------------- */

VOID NTAPI
_KeEnterGuardedRegion(VOID)
{
    QzCurrentThread->KernelApcDisable--;
}

VOID NTAPI
_KeLeaveGuardedRegion(VOID)
{
    QzCurrentThread->KernelApcDisable++;
}

BOOLEAN
NTAPI
KeAreAllApcsDisabled(VOID)
{
    return QzCurrentThread->KernelApcDisable != 0 || QzGetCurrentIrql() >= APC_LEVEL;
}

/* The gate is used as a synchronization event: signaled = free */
VOID
FASTCALL
KeInitializeGuardedMutex(OUT PKGUARDED_MUTEX GuardedMutex)
{
    GuardedMutex->Count = 1;
    GuardedMutex->Owner = NULL;
    GuardedMutex->Contention = 0;
    KeInitializeEvent((PKEVENT)&GuardedMutex->Gate, SynchronizationEvent, TRUE);
}

VOID
FASTCALL
KeAcquireGuardedMutexUnsafe(IN OUT PKGUARDED_MUTEX GuardedMutex)
{
    if (GuardedMutex->Owner == KeGetCurrentThread())
        KeBugCheckEx(0x000000BF /* MUTEX_ALREADY_OWNED */, (ULONG_PTR)GuardedMutex, 0, 0, 0);
    KeWaitForSingleObject(&GuardedMutex->Gate, Executive, KernelMode, FALSE, NULL);
    GuardedMutex->Count = 0;
    GuardedMutex->Owner = KeGetCurrentThread();
}

VOID
FASTCALL
KeReleaseGuardedMutexUnsafe(IN OUT PKGUARDED_MUTEX GuardedMutex)
{
    GuardedMutex->Owner = NULL;
    GuardedMutex->Count = 1;
    KeSetEvent((PKEVENT)&GuardedMutex->Gate, 0, FALSE);
}

VOID
FASTCALL
KeAcquireGuardedMutex(IN OUT PKGUARDED_MUTEX GuardedMutex)
{
    _KeEnterGuardedRegion();
    KeAcquireGuardedMutexUnsafe(GuardedMutex);
}

VOID
FASTCALL
KeReleaseGuardedMutex(IN OUT PKGUARDED_MUTEX GuardedMutex)
{
    KeReleaseGuardedMutexUnsafe(GuardedMutex);
    _KeLeaveGuardedRegion();
}

BOOLEAN
FASTCALL
KeTryToAcquireGuardedMutex(IN OUT PKGUARDED_MUTEX GuardedMutex)
{
    LARGE_INTEGER Zero;
    _KeEnterGuardedRegion();
    Zero.QuadPart = 0;
    if (KeWaitForSingleObject(&GuardedMutex->Gate, Executive, KernelMode, FALSE, &Zero) != STATUS_WAIT_0)
    {
        _KeLeaveGuardedRegion();
        return FALSE;
    }
    GuardedMutex->Count = 0;
    GuardedMutex->Owner = KeGetCurrentThread();
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Interrupt spin locks                                                    */
/* ---------------------------------------------------------------------- */

KIRQL
NTAPI
KeAcquireInterruptSpinLock(IN PKINTERRUPT Interrupt)
{
    KIRQL OldIrql = QzRaiseIrql(Interrupt->SynchronizeIrql);
    QzKeAcquireSpinLockAtDpcLevel(Interrupt->ActualLock);
    return OldIrql;
}

VOID
NTAPI
KeReleaseInterruptSpinLock(IN PKINTERRUPT Interrupt, IN KIRQL OldIrql)
{
    QzKeReleaseSpinLockFromDpcLevel(Interrupt->ActualLock);
    QzLowerIrql(OldIrql);
}

/* ---------------------------------------------------------------------- */
/* Misc executive data and services                                        */
/* ---------------------------------------------------------------------- */

ULONG InitSafeBootMode;

/* Version 1 UUIDs from the system time and a counter */
NTSTATUS
NTAPI
ExUuidCreate(OUT UUID *Uuid)
{
    static ULONG Sequence;
    LARGE_INTEGER Time;

    KeQuerySystemTime(&Time);
    Uuid->Data1 = Time.LowPart;
    Uuid->Data2 = (USHORT)Time.HighPart;
    Uuid->Data3 = (USHORT)(((Time.HighPart >> 16) & 0x0FFF) | 0x1000);
    Sequence++;
    Uuid->Data4[0] = (UCHAR)(0x80 | ((Sequence >> 8) & 0x3F));
    Uuid->Data4[1] = (UCHAR)Sequence;
    Uuid->Data4[2] = 'Q';
    Uuid->Data4[3] = 'z';
    Uuid->Data4[4] = 'O';
    Uuid->Data4[5] = 'S';
    Uuid->Data4[6] = (UCHAR)(Time.LowPart >> 8);
    Uuid->Data4[7] = (UCHAR)(Time.LowPart >> 16);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Fast mutexes (the kernel's own; drivers on x86 use the HAL's, which     */
/* follow the same protocol: Count 1 = free, 0 = owned, < 0 = waiters)     */
/* ---------------------------------------------------------------------- */

VOID
FASTCALL
ExAcquireFastMutexUnsafe(IN OUT PFAST_MUTEX FastMutex)
{
    if (InterlockedDecrement(&FastMutex->Count) != 0)
    {
        FastMutex->Contention++;
        KeWaitForSingleObject(&FastMutex->Event, WrExecutive, KernelMode, FALSE, NULL);
    }
    FastMutex->Owner = KeGetCurrentThread();
}

VOID
FASTCALL
ExReleaseFastMutexUnsafe(IN OUT PFAST_MUTEX FastMutex)
{
    FastMutex->Owner = NULL;
    if (InterlockedIncrement(&FastMutex->Count) <= 0)
        KeSetEvent(&FastMutex->Event, IO_NO_INCREMENT, FALSE);
}

VOID
FASTCALL
ExiAcquireFastMutex(IN OUT PFAST_MUTEX FastMutex)
{
    KIRQL OldIrql = QzRaiseIrql(APC_LEVEL);
    ExAcquireFastMutexUnsafe(FastMutex);
    FastMutex->OldIrql = OldIrql;
}

VOID
FASTCALL
ExiReleaseFastMutex(IN OUT PFAST_MUTEX FastMutex)
{
    KIRQL OldIrql = (KIRQL)FastMutex->OldIrql;
    ExReleaseFastMutexUnsafe(FastMutex);
    QzLowerIrql(OldIrql);
}

BOOLEAN
FASTCALL
ExiTryToAcquireFastMutex(IN OUT PFAST_MUTEX FastMutex)
{
    KIRQL OldIrql = QzRaiseIrql(APC_LEVEL);
    if (InterlockedCompareExchange(&FastMutex->Count, 0, 1) == 1)
    {
        FastMutex->Owner = KeGetCurrentThread();
        FastMutex->OldIrql = OldIrql;
        return TRUE;
    }
    QzLowerIrql(OldIrql);
    return FALSE;
}
