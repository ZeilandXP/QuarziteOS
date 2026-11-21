/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Process address spaces: page directories, private virtual
 *              memory, page faults, user memory access
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Every process has its own page directory. The system half (PDEs 512 and
 * up) is the same in all of them: QzVmInit gives every system PDE a page
 * table before the first process exists, so a system mapping made in any
 * address space is seen in all. PDE 768 maps each directory onto itself
 * (the self-map at 0xC0000000), so the current process's page tables are
 * always at QZ_PTE_BASE.
 *
 * User memory is described by VADs (reserved ranges). Committed pages are
 * demand-zero: their PTE is "not present" with QZ_PTE_COMMITTED and the
 * Win32 protection; the first touch allocates a zeroed page. Guard pages
 * fault once with STATUS_GUARD_PAGE_VIOLATION and then become ordinary.
 * There is no paging file: committed memory is backed by RAM when used.
 */

#include "qzio.h"
#include "qzps.h"

#define QZ_VAD_TAG  'daVQ'

/* Exported data: drivers probe user addresses against these */
ULONG_PTR MmUserProbeAddress = QZ_USER_PROBE_ADDRESS;
PVOID MmHighestUserAddress = (PVOID)QZ_USER_HIGHEST;
PVOID MmSystemRangeStart = (PVOID)QZ_SYSTEM_RANGE_START;

static ULONG_PTR QzHyperVa;           /* a scratch page for touching other pages */
static ULONG QzSharedDataPfn;
static ULONG QzKernelPageTables;

static __inline VOID
QzWriteCr3(ULONG Value)
{
    __asm__ __volatile__("movl %0, %%cr3" : : "r"(Value) : "memory");
}

/* Map one physical page at the scratch address (interrupts must be off) */
PVOID
QzMmMapHyper(ULONG Pfn)
{
    *QzPteOf(QzHyperVa) = (Pfn << QZ_PAGE_SHIFT) | QZ_PTE_PRESENT | QZ_PTE_WRITE;
    QzInvlpg(QzHyperVa);
    return (PVOID)QzHyperVa;
}

VOID
QzMmUnmapHyper(VOID)
{
    *QzPteOf(QzHyperVa) = 0;
    QzInvlpg(QzHyperVa);
}

VOID
QzVmInit(VOID)
{
    ULONG Index;
    ULONG_PTR Va;

    /* A page table behind every system PDE, so all directories share them */
    for (Index = QZ_SYSTEM_RANGE_START >> 22; Index < 1024; Index++)
    {
        volatile ULONG *Pde = (volatile ULONG *)(QZ_PDE_BASE + Index * 4);
        if (Index == (QZ_PTE_BASE >> 22) || (*Pde & QZ_PTE_PRESENT))
            continue;
        *Pde = (QzAllocPhysicalPage() << QZ_PAGE_SHIFT) | QZ_PTE_PRESENT | QZ_PTE_WRITE;
        QzInvlpg(QZ_PTE_BASE + (Index << QZ_PAGE_SHIFT));
        RtlZeroMemory((PVOID)(QZ_PTE_BASE + (Index << QZ_PAGE_SHIFT)), QZ_PAGE_SIZE);
        QzKernelPageTables++;
    }
    /* Flush the stale translations of the new tables */
    QzWriteCr3(QzReadCr3());

    /* The scratch page: any system address with a PTE of its own */
    Va = (ULONG_PTR)MmAllocateMappingAddress(QZ_PAGE_SIZE, 'pyHQ');
    QzHyperVa = Va;
    QzMmUnmapHyper();

    QzSharedDataPfn = (ULONG)(MmGetPhysicalAddress((PVOID)KI_USER_SHARED_DATA).QuadPart >> QZ_PAGE_SHIFT);
    DbgPrint(QZ_KERNEL_NAME ": vm: %lu system page tables added, shared user data at PFN 0x%lx\n",
             QzKernelPageTables, QzSharedDataPfn);
}

/* ---------------------------------------------------------------------- */
/* Address spaces                                                          */
/* ---------------------------------------------------------------------- */

NTSTATUS
QzMmEnsurePageTable(ULONG_PTR Va)
{
    volatile ULONG *Pde = QzPdeOf(Va);
    ULONG_PTR TableVa = QZ_PTE_BASE + ((Va >> 22) << QZ_PAGE_SHIFT);

    if (*Pde & QZ_PTE_PRESENT)
        return STATUS_SUCCESS;
    *Pde = (QzAllocPhysicalPage() << QZ_PAGE_SHIFT) | QZ_PTE_PRESENT | QZ_PTE_WRITE | QZ_PTE_USER;
    QzInvlpg(TableVa);
    RtlZeroMemory((PVOID)TableVa, QZ_PAGE_SIZE);
    if (QzCurrentThread->ApcProcess)
        QzCurrentThread->ApcProcess->PageTablePages++;
    return STATUS_SUCCESS;
}

VOID
QzMmSwitchAddressSpace(PQZ_PROCESS Process)
{
    if (Process && Process->DirectoryTableBase && QzReadCr3() != Process->DirectoryTableBase)
        QzWriteCr3(Process->DirectoryTableBase);
}

NTSTATUS
QzMmCreateAddressSpace(PQZ_PROCESS Process)
{
    ULONG Pfn = QzAllocPhysicalPage(), Flags, Index;
    PULONG Directory;
    PQZ_PROCESS Saved;
    PQZ_VAD Vad;
    NTSTATUS Status;

    /* The system half from the current directory, an empty user half, and
       the self-map entry pointing at the new directory */
    Flags = QzSaveFlagsAndDisable();
    Directory = QzMmMapHyper(Pfn);
    for (Index = 0; Index < 1024; Index++)
        Directory[Index] = (Index >= (QZ_SYSTEM_RANGE_START >> 22))
                               ? *(volatile ULONG *)(QZ_PDE_BASE + Index * 4) : 0;
    Directory[QZ_PTE_BASE >> 22] = (Pfn << QZ_PAGE_SHIFT) | QZ_PTE_PRESENT | QZ_PTE_WRITE;
    QzMmUnmapHyper();
    QzRestoreFlags(Flags);

    Process->DirectoryTableBase = Pfn << QZ_PAGE_SHIFT;
    InitializeListHead(&Process->VadList);
    ExInitializeFastMutex(&Process->AddressLock);

    /* KUSER_SHARED_DATA, read-only at 0x7FFE0000 */
    QzPsAttach(Process, &Saved);
    QzMmEnsurePageTable(QZ_USER_SHARED_DATA);
    *QzPteOf(QZ_USER_SHARED_DATA) = (QzSharedDataPfn << QZ_PAGE_SHIFT) | QZ_PTE_PRESENT | QZ_PTE_USER;
    QzInvlpg(QZ_USER_SHARED_DATA);
    QzPsDetach(Saved);
    Status = QzMmInsertVad(Process, QZ_USER_SHARED_DATA, QZ_PAGE_SIZE, MEM_PRIVATE, PAGE_READONLY,
                           FALSE, 0, &Vad);
    return Status;
}

/* Free a page a PTE maps, if the process owns it */
static VOID
QzMmReleasePte(PQZ_PROCESS Process, ULONG_PTR Va)
{
    volatile ULONG *Pte;
    ULONG Value;

    if (!(*QzPdeOf(Va) & QZ_PTE_PRESENT))
        return;
    Pte = QzPteOf(Va);
    Value = *Pte;
    *Pte = 0;
    QzInvlpg(Va);
    if ((Value & QZ_PTE_PRESENT) && (Value & QZ_PTE_PRIVATE))
    {
        QzFreePhysicalPage(Value >> QZ_PAGE_SHIFT);
        Process->PrivatePages--;
    }
}

VOID QzMmDeleteSectionView(PQZ_PROCESS Process, PQZ_VAD Vad);   /* mm/section.c */

VOID
QzMmDeleteAddressSpace(PQZ_PROCESS Process)
{
    PQZ_PROCESS Saved;
    ULONG Index, DirectoryPfn = Process->DirectoryTableBase >> QZ_PAGE_SHIFT;

    if (!Process->DirectoryTableBase || Process->AddressSpaceDeleted)
        return;
    Process->AddressSpaceDeleted = TRUE;

    QzPsAttach(Process, &Saved);
    while (!IsListEmpty(&Process->VadList))
    {
        PQZ_VAD Vad = CONTAINING_RECORD(Process->VadList.Flink, QZ_VAD, Link);
        ULONG_PTR Va;
        for (Va = Vad->Start; Va < Vad->End; Va += QZ_PAGE_SIZE)
            QzMmReleasePte(Process, Va);
        QzMmRemoveVad(Process, Vad);
    }
    for (Index = 0; Index < (QZ_SYSTEM_RANGE_START >> 22); Index++)
    {
        volatile ULONG *Pde = (volatile ULONG *)(QZ_PDE_BASE + Index * 4);
        if (*Pde & QZ_PTE_PRESENT)
        {
            QzFreePhysicalPage(*Pde >> QZ_PAGE_SHIFT);
            *Pde = 0;
        }
    }
    QzPsDetach(Saved);
    QzFreePhysicalPage(DirectoryPfn);
    Process->DirectoryTableBase = 0;
}

/* ---------------------------------------------------------------------- */
/* VADs                                                                    */
/* ---------------------------------------------------------------------- */

PQZ_VAD
QzMmFindVad(PQZ_PROCESS Process, ULONG_PTR Address)
{
    PLIST_ENTRY Entry;
    for (Entry = Process->VadList.Flink; Entry != &Process->VadList; Entry = Entry->Flink)
    {
        PQZ_VAD Vad = CONTAINING_RECORD(Entry, QZ_VAD, Link);
        if (Address < Vad->Start)
            break;
        if (Address < Vad->End)
            return Vad;
    }
    return NULL;
}

static BOOLEAN
QzMmRangeFree(PQZ_PROCESS Process, ULONG_PTR Start, ULONG_PTR End)
{
    PLIST_ENTRY Entry;
    for (Entry = Process->VadList.Flink; Entry != &Process->VadList; Entry = Entry->Flink)
    {
        PQZ_VAD Vad = CONTAINING_RECORD(Entry, QZ_VAD, Link);
        if (Vad->Start >= End)
            break;
        if (Vad->End > Start)
            return FALSE;
    }
    return TRUE;
}

/* Lowest (or highest) free, 64 KB aligned range of Size bytes below Limit */
static ULONG_PTR
QzMmFindFree(PQZ_PROCESS Process, SIZE_T Size, BOOLEAN TopDown, ULONG_PTR Limit)
{
    PLIST_ENTRY Entry;
    ULONG_PTR Candidate;

    if (!Limit || Limit > QZ_USER_HIGHEST + 1)
        Limit = QZ_USER_HIGHEST + 1;
    if (!TopDown)
    {
        Candidate = QZ_USER_LOWEST;
        for (Entry = Process->VadList.Flink; Entry != &Process->VadList; Entry = Entry->Flink)
        {
            PQZ_VAD Vad = CONTAINING_RECORD(Entry, QZ_VAD, Link);
            if (Candidate + Size <= Vad->Start)
                break;
            if (Vad->End > Candidate)
                Candidate = (Vad->End + QZ_ALLOCATION_GRANULARITY - 1) & ~(QZ_ALLOCATION_GRANULARITY - 1);
        }
        return (Candidate + Size <= Limit && Candidate + Size > Candidate) ? Candidate : 0;
    }

    /* Top down: walk the gaps from the end */
    {
        ULONG_PTR Ceiling = Limit;
        for (Entry = Process->VadList.Blink; ; Entry = Entry->Blink)
        {
            ULONG_PTR Floor = (Entry == &Process->VadList) ? QZ_USER_LOWEST
                                                           : CONTAINING_RECORD(Entry, QZ_VAD, Link)->End;
            if (Entry != &Process->VadList && CONTAINING_RECORD(Entry, QZ_VAD, Link)->Start >= Ceiling)
                continue;
            if (Ceiling > Size)
            {
                Candidate = (Ceiling - Size) & ~(QZ_ALLOCATION_GRANULARITY - 1);
                if (Candidate >= Floor && Candidate >= QZ_USER_LOWEST)
                    return Candidate;
            }
            if (Entry == &Process->VadList)
                return 0;
            Ceiling = CONTAINING_RECORD(Entry, QZ_VAD, Link)->Start;
        }
    }
}

NTSTATUS
QzMmInsertVad(PQZ_PROCESS Process, ULONG_PTR Start, SIZE_T Size, ULONG Type, ULONG Protect,
              BOOLEAN TopDown, ULONG_PTR Limit, PQZ_VAD *Result)
{
    PLIST_ENTRY Entry;
    PQZ_VAD Vad;

    Size = (Size + QZ_PAGE_SIZE - 1) & ~(QZ_PAGE_SIZE - 1);
    if (!Size)
        return STATUS_INVALID_PARAMETER;
    if (!Start)
    {
        Start = QzMmFindFree(Process, Size, TopDown, Limit);
        if (!Start)
            return STATUS_NO_MEMORY;
    }
    else if (Start < QZ_USER_LOWEST || Start + Size > QZ_USER_HIGHEST + 1 || Start + Size < Start)
    {
        return STATUS_INVALID_PARAMETER;
    }
    else if (!QzMmRangeFree(Process, Start, Start + Size))
    {
        return STATUS_CONFLICTING_ADDRESSES;
    }

    Vad = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Vad), QZ_VAD_TAG);
    if (!Vad)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Vad, sizeof(*Vad));
    Vad->Start = Start;
    Vad->End = Start + Size;
    Vad->Type = Type;
    Vad->AllocationProtect = Protect;
    for (Entry = Process->VadList.Flink; Entry != &Process->VadList; Entry = Entry->Flink)
        if (CONTAINING_RECORD(Entry, QZ_VAD, Link)->Start > Start)
            break;
    InsertTailList(Entry, &Vad->Link);
    *Result = Vad;
    return STATUS_SUCCESS;
}

VOID
QzMmRemoveVad(PQZ_PROCESS Process, PQZ_VAD Vad)
{
    UNREFERENCED_PARAMETER(Process);
    RemoveEntryList(&Vad->Link);
    if (Vad->Section)
        QzMmDeleteSectionView(Process, Vad);
    ExFreePoolWithTag(Vad, QZ_VAD_TAG);
}

/* ---------------------------------------------------------------------- */
/* Protection                                                              */
/* ---------------------------------------------------------------------- */

static BOOLEAN
QzMmValidProtect(ULONG Protect)
{
    switch (Protect & ~(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE))
    {
        case PAGE_NOACCESS: case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
        case PAGE_EXECUTE: case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
            return !((Protect & PAGE_GUARD) && (Protect & 0xFF) == PAGE_NOACCESS);
        default:
            return FALSE;
    }
}

/* Hardware bits for a Win32 protection; 0 = no access */
ULONG
QzMmProtectToPte(ULONG Protect)
{
    ULONG Bits;
    switch (Protect & 0xFF)
    {
        case PAGE_READONLY: case PAGE_EXECUTE: case PAGE_EXECUTE_READ:
            Bits = QZ_PTE_PRESENT | QZ_PTE_USER;
            break;
        case PAGE_READWRITE: case PAGE_WRITECOPY: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
            Bits = QZ_PTE_PRESENT | QZ_PTE_USER | QZ_PTE_WRITE;
            break;
        default:
            return 0;
    }
    if (Protect & PAGE_NOCACHE)
        Bits |= QZ_PTE_PCD;
    return Bits;
}

/* The Win32 protection a PTE stands for (0: not committed) */
static ULONG
QzMmPteToProtect(PQZ_VAD Vad, ULONG Pte)
{
    BOOLEAN Execute = (Vad->AllocationProtect & 0xF0) != 0;
    if (Pte & QZ_PTE_PRESENT)
    {
        if (!(Pte & QZ_PTE_USER))
            return PAGE_NOACCESS;
        if (Pte & QZ_PTE_WRITE)
            return Execute ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
        if (Pte & QZ_PTE_COPY_ON_WRITE)
            return Execute ? PAGE_EXECUTE_WRITECOPY : PAGE_WRITECOPY;
        return Execute ? PAGE_EXECUTE_READ : PAGE_READONLY;
    }
    if (Pte & QZ_PTE_COMMITTED)
        return QZ_SWPTE_PROTECT(Pte);
    return 0;
}

static ULONG
QzMmReadPte(ULONG_PTR Va)
{
    if (!(*QzPdeOf(Va) & QZ_PTE_PRESENT))
        return 0;
    return *QzPteOf(Va);
}

/* Set the protection of a committed (or committing) page. A section's
   page that becomes writable is copy-on-write unless the view shares writes */
static VOID
QzMmSetPageProtect(PQZ_VAD Vad, ULONG_PTR Va, ULONG Protect)
{
    volatile ULONG *Pte;
    ULONG Value, Bits;

    QzMmEnsurePageTable(Va);
    Pte = QzPteOf(Va);
    Value = *Pte;
    Bits = QzMmProtectToPte(Protect);
    if ((Value & QZ_PTE_PRESENT) && !(Value & QZ_PTE_PRIVATE) && (Bits & QZ_PTE_WRITE) && Vad &&
        (Vad->Type == MEM_IMAGE || (Protect & (PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))))
        Bits = (Bits & ~QZ_PTE_WRITE) | QZ_PTE_COPY_ON_WRITE;
    if ((Value & QZ_PTE_PRESENT) && !(Protect & PAGE_GUARD) && Bits)
        *Pte = (Value & ~0xFFFUL) | Bits | (Value & QZ_PTE_PRIVATE);
    else if (Value & QZ_PTE_PRESENT)
        *Pte = (Value & ~0xFFFUL) | QZ_PTE_PRESENT | (Value & QZ_PTE_PRIVATE);  /* the frame stays, no user access */
    else
        *Pte = QZ_SWPTE(Protect);
    QzInvlpg(Va);
}

/* ---------------------------------------------------------------------- */
/* NtAllocate/Free/Protect/QueryVirtualMemory, on a given process          */
/* ---------------------------------------------------------------------- */

NTSTATUS
QzMmAllocate(PQZ_PROCESS Process, PVOID *BaseAddress, ULONG_PTR ZeroBits, PSIZE_T RegionSize,
             ULONG AllocationType, ULONG Protect)
{
    ULONG_PTR Base = (ULONG_PTR)*BaseAddress, Start, End, Va, Limit = 0;
    SIZE_T Size = *RegionSize;
    PQZ_PROCESS Saved;
    PQZ_VAD Vad;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!(AllocationType & (MEM_COMMIT | MEM_RESERVE | MEM_RESET)) ||
        (AllocationType & ~(MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN | MEM_WRITE_WATCH)))
        return STATUS_INVALID_PARAMETER_5;
    if (!QzMmValidProtect(Protect))
        return STATUS_INVALID_PAGE_PROTECTION;
    if (!Size)
        return STATUS_INVALID_PARAMETER_4;
    if (ZeroBits > 21)
        return STATUS_INVALID_PARAMETER_3;
    if (ZeroBits)
        Limit = (ULONG_PTR)(0xFFFFFFFFUL >> ZeroBits) + 1;
    if (AllocationType & MEM_RESET)
        return STATUS_SUCCESS;   /* nothing to discard: there is no paging */

    ExAcquireFastMutex(&Process->AddressLock);
    QzPsAttach(Process, &Saved);

    if ((AllocationType & MEM_RESERVE) || !Base)
    {
        if (Base)
        {
            Start = Base & ~(QZ_ALLOCATION_GRANULARITY - 1);
            End = (Base + Size + QZ_PAGE_SIZE - 1) & ~(QZ_PAGE_SIZE - 1);
        }
        else
        {
            Start = 0;
            End = (Size + QZ_PAGE_SIZE - 1) & ~(QZ_PAGE_SIZE - 1);
        }
        Status = QzMmInsertVad(Process, Start, End - Start, MEM_PRIVATE, Protect,
                               (AllocationType & MEM_TOP_DOWN) != 0, Limit, &Vad);
        if (!NT_SUCCESS(Status))
            goto Done;
        Start = Vad->Start;
        End = Vad->End;
    }
    else
    {
        Start = Base & ~(QZ_PAGE_SIZE - 1);
        End = (Base + Size + QZ_PAGE_SIZE - 1) & ~(QZ_PAGE_SIZE - 1);
        Vad = QzMmFindVad(Process, Start);
        if (!Vad || End > Vad->End || End <= Start)
        {
            Status = STATUS_CONFLICTING_ADDRESSES;
            goto Done;
        }
        if (Vad->Type != MEM_PRIVATE)
        {
            Status = STATUS_CONFLICTING_ADDRESSES;   /* committing in views: SEC_RESERVE, later */
            goto Done;
        }
    }

    if (AllocationType & MEM_COMMIT)
        for (Va = Start; Va < End; Va += QZ_PAGE_SIZE)
            QzMmSetPageProtect(Vad, Va, Protect);

    *BaseAddress = (PVOID)Start;
    *RegionSize = End - Start;
Done:
    QzPsDetach(Saved);
    ExReleaseFastMutex(&Process->AddressLock);
    return Status;
}

NTSTATUS
QzMmFree(PQZ_PROCESS Process, PVOID *BaseAddress, PSIZE_T RegionSize, ULONG FreeType)
{
    ULONG_PTR Base = (ULONG_PTR)*BaseAddress, Start, End, Va;
    SIZE_T Size = *RegionSize;
    PQZ_PROCESS Saved;
    PQZ_VAD Vad;
    NTSTATUS Status = STATUS_SUCCESS;

    if (FreeType != MEM_RELEASE && FreeType != MEM_DECOMMIT)
        return STATUS_INVALID_PARAMETER_4;

    ExAcquireFastMutex(&Process->AddressLock);
    QzPsAttach(Process, &Saved);

    Start = Base & ~(QZ_PAGE_SIZE - 1);
    Vad = QzMmFindVad(Process, Start);
    if (!Vad)
    {
        Status = STATUS_MEMORY_NOT_ALLOCATED;
        goto Done;
    }
    if (Vad->Type != MEM_PRIVATE)
    {
        Status = STATUS_UNABLE_TO_DELETE_SECTION;
        goto Done;
    }
    if (!Size)
    {
        if (FreeType == MEM_RELEASE && Start != Vad->Start)
        {
            Status = STATUS_FREE_VM_NOT_AT_BASE;
            goto Done;
        }
        End = Vad->End;
    }
    else
    {
        End = (Base + Size + QZ_PAGE_SIZE - 1) & ~(QZ_PAGE_SIZE - 1);
        if (End > Vad->End)
        {
            Status = STATUS_UNABLE_TO_FREE_VM;
            goto Done;
        }
    }

    for (Va = Start; Va < End; Va += QZ_PAGE_SIZE)
        QzMmReleasePte(Process, Va);

    if (FreeType == MEM_RELEASE)
    {
        if (Start == Vad->Start && End == Vad->End)
        {
            QzMmRemoveVad(Process, Vad);
        }
        else if (Start == Vad->Start)
        {
            Vad->Start = End;
        }
        else if (End == Vad->End)
        {
            Vad->End = Start;
        }
        else
        {
            /* Split: the tail becomes a VAD of its own */
            PQZ_VAD Tail = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Tail), QZ_VAD_TAG);
            if (!Tail)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto Done;
            }
            *Tail = *Vad;
            Tail->Start = End;
            Vad->End = Start;
            InsertHeadList(&Vad->Link, &Tail->Link);
        }
    }
    *BaseAddress = (PVOID)Start;
    *RegionSize = End - Start;
Done:
    QzPsDetach(Saved);
    ExReleaseFastMutex(&Process->AddressLock);
    return Status;
}

NTSTATUS
QzMmProtect(PQZ_PROCESS Process, PVOID *BaseAddress, PSIZE_T RegionSize, ULONG NewProtect,
            PULONG OldProtect)
{
    ULONG_PTR Base = (ULONG_PTR)*BaseAddress, Start, End, Va;
    PQZ_PROCESS Saved;
    PQZ_VAD Vad;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!QzMmValidProtect(NewProtect))
        return STATUS_INVALID_PAGE_PROTECTION;
    Start = Base & ~(QZ_PAGE_SIZE - 1);
    End = (Base + *RegionSize + QZ_PAGE_SIZE - 1) & ~(QZ_PAGE_SIZE - 1);
    if (End <= Start)
        End = Start + QZ_PAGE_SIZE;

    ExAcquireFastMutex(&Process->AddressLock);
    QzPsAttach(Process, &Saved);
    Vad = QzMmFindVad(Process, Start);
    if (!Vad || End > Vad->End)
    {
        Status = STATUS_CONFLICTING_ADDRESSES;
        goto Done;
    }
    for (Va = Start; Va < End; Va += QZ_PAGE_SIZE)
    {
        if (!QzMmPteToProtect(Vad, QzMmReadPte(Va)))
        {
            Status = STATUS_NOT_COMMITTED;
            goto Done;
        }
    }
    *OldProtect = QzMmPteToProtect(Vad, QzMmReadPte(Start));
    for (Va = Start; Va < End; Va += QZ_PAGE_SIZE)
        QzMmSetPageProtect(Vad, Va, NewProtect);
    *BaseAddress = (PVOID)Start;
    *RegionSize = End - Start;
Done:
    QzPsDetach(Saved);
    ExReleaseFastMutex(&Process->AddressLock);
    return Status;
}

NTSTATUS
QzMmQuery(PQZ_PROCESS Process, PVOID Address, PMEMORY_BASIC_INFORMATION Info)
{
    ULONG_PTR Va = (ULONG_PTR)Address & ~(QZ_PAGE_SIZE - 1), Run;
    PQZ_PROCESS Saved;
    PLIST_ENTRY Entry;
    PQZ_VAD Vad;

    if ((ULONG_PTR)Address > QZ_USER_HIGHEST)
        return STATUS_INVALID_PARAMETER;

    ExAcquireFastMutex(&Process->AddressLock);
    QzPsAttach(Process, &Saved);
    RtlZeroMemory(Info, sizeof(*Info));
    Info->BaseAddress = (PVOID)Va;
    Vad = QzMmFindVad(Process, Va);
    if (!Vad)
    {
        /* A free region up to the next VAD */
        ULONG_PTR Next = QZ_USER_HIGHEST + 1;
        for (Entry = Process->VadList.Flink; Entry != &Process->VadList; Entry = Entry->Flink)
        {
            PQZ_VAD Later = CONTAINING_RECORD(Entry, QZ_VAD, Link);
            if (Later->Start > Va)
            {
                Next = Later->Start;
                break;
            }
        }
        Info->State = MEM_FREE;
        Info->Protect = PAGE_NOACCESS;
        Info->RegionSize = Next - Va;
    }
    else
    {
        ULONG Protect = QzMmPteToProtect(Vad, QzMmReadPte(Va));
        Info->AllocationBase = (PVOID)Vad->Start;
        Info->AllocationProtect = Vad->AllocationProtect;
        Info->Type = Vad->Type;
        Info->State = Protect ? MEM_COMMIT : MEM_RESERVE;
        Info->Protect = Protect;
        for (Run = Va + QZ_PAGE_SIZE; Run < Vad->End; Run += QZ_PAGE_SIZE)
            if (QzMmPteToProtect(Vad, QzMmReadPte(Run)) != Protect)
                break;
        Info->RegionSize = Run - Va;
    }
    QzPsDetach(Saved);
    ExReleaseFastMutex(&Process->AddressLock);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Page faults on user addresses                                           */
/* ---------------------------------------------------------------------- */

/*
 * Called from the #PF trap with interrupts off. Resolves demand-zero pages
 * and guard pages of the current address space; anything else is an
 * access violation for the caller to deal with.
 */
NTSTATUS
QzMmAccessFault(ULONG_PTR Address, ULONG ErrorCode, KPROCESSOR_MODE Mode)
{
    volatile ULONG *Pte;
    ULONG Value, Protect, Bits, Pfn;
    PQZ_PROCESS Process = QzCurrentThread->ApcProcess;

    UNREFERENCED_PARAMETER(Mode);
    if (Address >= QZ_SYSTEM_RANGE_START || !(*QzPdeOf(Address) & QZ_PTE_PRESENT))
        return STATUS_ACCESS_VIOLATION;
    Pte = QzPteOf(Address);
    Value = *Pte;

    if (Value & QZ_PTE_PRESENT)
    {
        if (!(Value & QZ_PTE_USER))
            return STATUS_ACCESS_VIOLATION;
        if ((ErrorCode & 2) && !(Value & QZ_PTE_WRITE) && (Value & QZ_PTE_COPY_ON_WRITE))
        {
            /* Copy on write: a private page with the shared page's contents */
            ULONG_PTR Page = Address & ~(QZ_PAGE_SIZE - 1);
            PVOID Copy;
            Pfn = QzAllocPhysicalPage();
            Copy = QzMmMapHyper(Pfn);
            RtlCopyMemory(Copy, (PVOID)Page, QZ_PAGE_SIZE);
            QzMmUnmapHyper();
            *Pte = (Pfn << QZ_PAGE_SHIFT) | (Value & (QZ_PTE_PCD | QZ_PTE_PWT)) | QZ_PTE_PRESENT | QZ_PTE_USER |
                   QZ_PTE_WRITE | QZ_PTE_PRIVATE;
            QzInvlpg(Address);
            if (Process)
                Process->PrivatePages++;
            return STATUS_SUCCESS;
        }
        if ((ErrorCode & 2) && !(Value & QZ_PTE_WRITE))
            return STATUS_ACCESS_VIOLATION;
        QzInvlpg(Address);   /* a stale translation */
        return STATUS_SUCCESS;
    }
    if (!(Value & QZ_PTE_COMMITTED))
        return STATUS_ACCESS_VIOLATION;

    Protect = QZ_SWPTE_PROTECT(Value);
    if (Protect & PAGE_GUARD)
    {
        *Pte = QZ_SWPTE(Protect & ~PAGE_GUARD);
        return STATUS_GUARD_PAGE_VIOLATION;
    }
    Bits = QzMmProtectToPte(Protect);
    if (!Bits)
        return STATUS_ACCESS_VIOLATION;

    /* Demand zero */
    Pfn = QzAllocPhysicalPage();
    *Pte = (Pfn << QZ_PAGE_SHIFT) | QZ_PTE_PRESENT | QZ_PTE_WRITE | QZ_PTE_PRIVATE;
    QzInvlpg(Address);
    RtlZeroMemory((PVOID)(Address & ~(QZ_PAGE_SIZE - 1)), QZ_PAGE_SIZE);
    *Pte = (Pfn << QZ_PAGE_SHIFT) | Bits | QZ_PTE_PRIVATE;
    QzInvlpg(Address);
    if (Process)
        Process->PrivatePages++;
    if ((ErrorCode & 2) && !(Bits & QZ_PTE_WRITE))
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* User memory access from the kernel                                      */
/* ---------------------------------------------------------------------- */

/* A copy that resumes at "1:" if it touches an address that cannot be made
   valid; the #PF handler sends it there through FaultRecovery */
static BOOLEAN
QzCopyUserBytes(PVOID Destination, const VOID *Source, SIZE_T Length)
{
    ULONG Result;
    __asm__ __volatile__(
        "movl %%fs:0x124, %%eax\n\t"
        "movl $1f, %c[off](%%eax)\n\t"
        "rep movsb\n\t"
        "movl $1, %%edx\n\t"
        "jmp 2f\n"
        "1:\n\t"
        "xorl %%edx, %%edx\n"
        "2:\n\t"
        "movl %%fs:0x124, %%eax\n\t"
        "movl $0, %c[off](%%eax)\n\t"
        : "=d"(Result), "+D"(Destination), "+S"(Source), "+c"(Length)
        : [off] "i"(FIELD_OFFSET(QZ_THREAD, FaultRecovery))
        : "eax", "memory", "cc");
    return Result != 0;
}

BOOLEAN
QzProbe(const VOID *Address, SIZE_T Length, BOOLEAN Write)
{
    ULONG_PTR Start = (ULONG_PTR)Address;
    UNREFERENCED_PARAMETER(Write);
    if (!Length)
        return TRUE;
    return Start + Length >= Start && Start + Length <= QZ_USER_PROBE_ADDRESS;
}

BOOLEAN
QzCopyFromUser(PVOID Destination, const VOID *UserSource, SIZE_T Length)
{
    if (!QzProbe(UserSource, Length, FALSE))
        return FALSE;
    return QzCopyUserBytes(Destination, UserSource, Length);
}

BOOLEAN
QzCopyToUser(PVOID UserDestination, const VOID *Source, SIZE_T Length)
{
    if (!QzProbe(UserDestination, Length, TRUE))
        return FALSE;
    return QzCopyUserBytes(UserDestination, Source, Length);
}

BOOLEAN
QzReadArg(KPROCESSOR_MODE Mode, PVOID Destination, const VOID *Source, SIZE_T Length)
{
    if (Mode == KernelMode)
    {
        RtlCopyMemory(Destination, Source, Length);
        return TRUE;
    }
    return QzCopyFromUser(Destination, Source, Length);
}

BOOLEAN
QzWriteArg(KPROCESSOR_MODE Mode, PVOID Destination, const VOID *Source, SIZE_T Length)
{
    if (Mode == KernelMode)
    {
        RtlCopyMemory(Destination, Source, Length);
        return TRUE;
    }
    return QzCopyToUser(Destination, Source, Length);
}

/* ---------------------------------------------------------------------- */
/* System services                                                         */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzMmReferenceProcess(HANDLE ProcessHandle, ACCESS_MASK Access, PQZ_PROCESS *Process)
{
    return ObReferenceObjectByHandle(ProcessHandle, Access, PsProcessType, ExGetPreviousMode(),
                                     (PVOID *)Process, NULL);
}

NTSTATUS
NTAPI
NtAllocateVirtualMemory(IN HANDLE ProcessHandle,
                        IN OUT PVOID *UBaseAddress,
                        IN ULONG_PTR ZeroBits,
                        IN OUT PSIZE_T URegionSize,
                        IN ULONG AllocationType,
                        IN ULONG Protect)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PVOID Base;
    SIZE_T Size;
    PQZ_PROCESS Process;
    NTSTATUS Status;

    if (!QzReadArg(Mode, &Base, UBaseAddress, sizeof(Base)) ||
        !QzReadArg(Mode, &Size, URegionSize, sizeof(Size)))
        return STATUS_ACCESS_VIOLATION;
    Status = QzMmReferenceProcess(ProcessHandle, PROCESS_VM_OPERATION, &Process);
    if (!NT_SUCCESS(Status))
        return Status;
    if (Process == QzSystemProcess)
        Status = STATUS_INVALID_PARAMETER;   /* no user space in System */
    else
        Status = QzMmAllocate(Process, &Base, ZeroBits, &Size, AllocationType, Protect);
    ObDereferenceObject(Process);
    if (NT_SUCCESS(Status) &&
        (!QzWriteArg(Mode, UBaseAddress, &Base, sizeof(Base)) ||
         !QzWriteArg(Mode, URegionSize, &Size, sizeof(Size))))
        Status = STATUS_ACCESS_VIOLATION;
    return Status;
}

NTSTATUS
NTAPI
ZwAllocateVirtualMemory(IN HANDLE ProcessHandle, IN OUT PVOID *BaseAddress, IN ULONG_PTR ZeroBits,
                        IN OUT PSIZE_T RegionSize, IN ULONG AllocationType, IN ULONG Protect)
{
    return QZ_AS_KERNEL(NtAllocateVirtualMemory(ProcessHandle, BaseAddress, ZeroBits, RegionSize,
                                                AllocationType, Protect));
}

NTSTATUS
NTAPI
NtFreeVirtualMemory(IN HANDLE ProcessHandle,
                    IN OUT PVOID *UBaseAddress,
                    IN OUT PSIZE_T URegionSize,
                    IN ULONG FreeType)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PVOID Base;
    SIZE_T Size;
    PQZ_PROCESS Process;
    NTSTATUS Status;

    if (!QzReadArg(Mode, &Base, UBaseAddress, sizeof(Base)) ||
        !QzReadArg(Mode, &Size, URegionSize, sizeof(Size)))
        return STATUS_ACCESS_VIOLATION;
    Status = QzMmReferenceProcess(ProcessHandle, PROCESS_VM_OPERATION, &Process);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = QzMmFree(Process, &Base, &Size, FreeType);
    ObDereferenceObject(Process);
    if (NT_SUCCESS(Status) &&
        (!QzWriteArg(Mode, UBaseAddress, &Base, sizeof(Base)) ||
         !QzWriteArg(Mode, URegionSize, &Size, sizeof(Size))))
        Status = STATUS_ACCESS_VIOLATION;
    return Status;
}

NTSTATUS
NTAPI
ZwFreeVirtualMemory(IN HANDLE ProcessHandle, IN OUT PVOID *BaseAddress, IN OUT PSIZE_T RegionSize,
                    IN ULONG FreeType)
{
    return QZ_AS_KERNEL(NtFreeVirtualMemory(ProcessHandle, BaseAddress, RegionSize, FreeType));
}

NTSTATUS
NTAPI
NtProtectVirtualMemory(IN HANDLE ProcessHandle,
                       IN OUT PVOID *UBaseAddress,
                       IN OUT PSIZE_T UNumberOfBytesToProtect,
                       IN ULONG NewAccessProtection,
                       OUT PULONG UOldAccessProtection)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PVOID Base;
    SIZE_T Size;
    ULONG Old = 0;
    PQZ_PROCESS Process;
    NTSTATUS Status;

    if (!QzReadArg(Mode, &Base, UBaseAddress, sizeof(Base)) ||
        !QzReadArg(Mode, &Size, UNumberOfBytesToProtect, sizeof(Size)))
        return STATUS_ACCESS_VIOLATION;
    Status = QzMmReferenceProcess(ProcessHandle, PROCESS_VM_OPERATION, &Process);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = QzMmProtect(Process, &Base, &Size, NewAccessProtection, &Old);
    ObDereferenceObject(Process);
    if (NT_SUCCESS(Status) &&
        (!QzWriteArg(Mode, UBaseAddress, &Base, sizeof(Base)) ||
         !QzWriteArg(Mode, UNumberOfBytesToProtect, &Size, sizeof(Size)) ||
         !QzWriteArg(Mode, UOldAccessProtection, &Old, sizeof(Old))))
        Status = STATUS_ACCESS_VIOLATION;
    return Status;
}

NTSTATUS
NTAPI
NtQueryVirtualMemory(IN HANDLE ProcessHandle,
                     IN PVOID BaseAddress,
                     IN MEMORY_INFORMATION_CLASS MemoryInformationClass,
                     OUT PVOID MemoryInformation,
                     IN SIZE_T MemoryInformationLength,
                     OUT PSIZE_T UReturnLength OPTIONAL)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    MEMORY_BASIC_INFORMATION Info;
    PQZ_PROCESS Process;
    SIZE_T Length = sizeof(Info);
    NTSTATUS Status;

    if (MemoryInformationClass != MemoryBasicInformation)
        return STATUS_INVALID_INFO_CLASS;   /* MemorySectionName: later */
    if (MemoryInformationLength < sizeof(Info))
        return STATUS_INFO_LENGTH_MISMATCH;
    Status = QzMmReferenceProcess(ProcessHandle, PROCESS_QUERY_INFORMATION, &Process);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = (Process == QzSystemProcess) ? STATUS_INVALID_PARAMETER : QzMmQuery(Process, BaseAddress, &Info);
    ObDereferenceObject(Process);
    if (NT_SUCCESS(Status) &&
        (!QzWriteArg(Mode, MemoryInformation, &Info, sizeof(Info)) ||
         (UReturnLength && !QzWriteArg(Mode, UReturnLength, &Length, sizeof(Length)))))
        Status = STATUS_ACCESS_VIOLATION;
    return Status;
}

NTSTATUS
NTAPI
ZwQueryVirtualMemory(IN HANDLE ProcessHandle, IN PVOID BaseAddress,
                     IN MEMORY_INFORMATION_CLASS MemoryInformationClass, OUT PVOID MemoryInformation,
                     IN SIZE_T MemoryInformationLength, OUT PSIZE_T ReturnLength OPTIONAL)
{
    return QZ_AS_KERNEL(NtQueryVirtualMemory(ProcessHandle, BaseAddress, MemoryInformationClass,
                                             MemoryInformation, MemoryInformationLength, ReturnLength));
}

/* Copy between the caller and another process, page by page through the kernel */
static NTSTATUS
QzMmCopyVirtual(HANDLE ProcessHandle, PVOID Remote, PVOID Local, SIZE_T Length, BOOLEAN Write,
                PSIZE_T UDone)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PQZ_PROCESS Process, Saved, Caller = QzCurrentThread->ApcProcess;
    UCHAR Buffer[512];
    SIZE_T Done = 0;
    NTSTATUS Status;

    Status = QzMmReferenceProcess(ProcessHandle, Write ? PROCESS_VM_WRITE : PROCESS_VM_READ, &Process);
    if (!NT_SUCCESS(Status))
        return Status;
    while (Done < Length && NT_SUCCESS(Status))
    {
        SIZE_T Chunk = min(Length - Done, sizeof(Buffer));
        PUCHAR RemoteAt = (PUCHAR)Remote + Done, LocalAt = (PUCHAR)Local + Done;
        BOOLEAN Ok;
        if (Write)
        {
            Ok = QzReadArg(Mode, Buffer, LocalAt, Chunk);
            if (Ok)
            {
                QzPsAttach(Process, &Saved);
                Ok = QzCopyToUser(RemoteAt, Buffer, Chunk);
                QzPsDetach(Saved);
            }
        }
        else
        {
            QzPsAttach(Process, &Saved);
            Ok = QzCopyFromUser(Buffer, RemoteAt, Chunk);
            QzPsDetach(Saved);
            if (Ok)
            {
                QzPsAttach(Caller, &Saved);
                Ok = QzWriteArg(Mode, LocalAt, Buffer, Chunk);
                QzPsDetach(Saved);
            }
        }
        if (!Ok)
            Status = Done ? STATUS_PARTIAL_COPY : STATUS_ACCESS_VIOLATION;
        else
            Done += Chunk;
    }
    ObDereferenceObject(Process);
    if (UDone)
        QzWriteArg(Mode, UDone, &Done, sizeof(Done));
    return Status;
}

NTSTATUS
NTAPI
NtReadVirtualMemory(IN HANDLE ProcessHandle, IN PVOID BaseAddress, OUT PVOID Buffer,
                    IN SIZE_T NumberOfBytesToRead, OUT PSIZE_T NumberOfBytesRead OPTIONAL)
{
    return QzMmCopyVirtual(ProcessHandle, BaseAddress, Buffer, NumberOfBytesToRead, FALSE,
                           NumberOfBytesRead);
}

NTSTATUS
NTAPI
NtWriteVirtualMemory(IN HANDLE ProcessHandle, IN PVOID BaseAddress, IN PVOID Buffer,
                     IN SIZE_T NumberOfBytesToWrite, OUT PSIZE_T NumberOfBytesWritten OPTIONAL)
{
    return QzMmCopyVirtual(ProcessHandle, BaseAddress, Buffer, NumberOfBytesToWrite, TRUE,
                           NumberOfBytesWritten);
}

NTSTATUS
NTAPI
NtFlushInstructionCache(IN HANDLE ProcessHandle, IN PVOID BaseAddress, IN SIZE_T NumberOfBytesToFlush)
{
    UNREFERENCED_PARAMETER(ProcessHandle);
    UNREFERENCED_PARAMETER(BaseAddress);
    UNREFERENCED_PARAMETER(NumberOfBytesToFlush);
    return STATUS_SUCCESS;   /* x86 caches are coherent */
}

NTSTATUS
NTAPI
NtLockVirtualMemory(IN HANDLE ProcessHandle, IN OUT PVOID *BaseAddress, IN OUT PSIZE_T NumberOfBytesToLock,
                    IN ULONG MapType)
{
    UNREFERENCED_PARAMETER(ProcessHandle);
    UNREFERENCED_PARAMETER(BaseAddress);
    UNREFERENCED_PARAMETER(NumberOfBytesToLock);
    UNREFERENCED_PARAMETER(MapType);
    return STATUS_SUCCESS;   /* nothing is ever paged out */
}

NTSTATUS
NTAPI
NtUnlockVirtualMemory(IN HANDLE ProcessHandle, IN OUT PVOID *BaseAddress,
                      IN OUT PSIZE_T NumberOfBytesToUnlock, IN ULONG MapType)
{
    UNREFERENCED_PARAMETER(ProcessHandle);
    UNREFERENCED_PARAMETER(BaseAddress);
    UNREFERENCED_PARAMETER(NumberOfBytesToUnlock);
    UNREFERENCED_PARAMETER(MapType);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* The shell's "vm" command                                                */
/* ---------------------------------------------------------------------- */

VOID
QzMmDumpAddressSpace(PQZ_PROCESS Process)
{
    PLIST_ENTRY Entry;

    QzPrint("  Address space of %s (pid %lu): page directory 0x%08lx, %lu private page(s), "
            "%lu page table(s)\n", Process->ImageFileName, Process->UniqueId, Process->DirectoryTableBase,
            Process->PrivatePages, Process->PageTablePages);
    if (Process == QzSystemProcess)
        return;
    ExAcquireFastMutex(&Process->AddressLock);
    for (Entry = Process->VadList.Flink; Entry != &Process->VadList; Entry = Entry->Flink)
    {
        PQZ_VAD Vad = CONTAINING_RECORD(Entry, QZ_VAD, Link);
        QzPrint("    %08lx-%08lx %-7s protect 0x%03lx%s\n", Vad->Start, Vad->End - 1,
                Vad->Type == MEM_IMAGE ? "image" : Vad->Type == MEM_MAPPED ? "mapped" : "private",
                Vad->AllocationProtect, Vad->Section ? " (section view)" : "");
    }
    ExReleaseFastMutex(&Process->AddressLock);
}
