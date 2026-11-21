/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Interrupt objects (KINTERRUPT) and device interrupt dispatch
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Device interrupts follow the NT HAL protocol:
 *   HalBeginSystemInterrupt(Irql, Vector, &OldIrql)
 *     - raises to the line's IRQL, masks lower lines and EOIs the PIC; with
 *       the HAL's lazy IRQL it may instead defer the interrupt (it is then
 *       replayed later through the same vector) and return FALSE;
 *   service routines run with interrupts enabled;
 *   HalEndSystemInterrupt(OldIrql, TrapFrame)
 *     - restores the IRQL and may tail-jump into a pending software
 *       interrupt (DPC/dispatch) with this trap frame;
 *   KiEoiHelper(TrapFrame) leaves the interrupt.
 */

#include "qz.h"

#define QZ_INTERRUPT_OBJECT 0x16   /* KOBJECTS InterruptObject */

static LIST_ENTRY QzInterruptLists[16];
static ULONG QzConnectedCount;
static BOOLEAN QzListsInitialized;

static VOID
QzInitLists(VOID)
{
    ULONG i;
    if (QzListsInitialized)
        return;
    for (i = 0; i < 16; i++)
        InitializeListHead(&QzInterruptLists[i]);
    QzListsInitialized = TRUE;
}

ULONG
QzConnectedInterruptCount(VOID)
{
    return QzConnectedCount;
}

/* Called from the trap stub, interrupts disabled. Does not return. */
VOID
FASTCALL
QzDispatchDeviceInterrupt(PKTRAP_FRAME TrapFrame, ULONG Vector)
{
    ULONG Irq = Vector - PRIMARY_VECTOR_BASE;
    PLIST_ENTRY Head, Entry;
    PKINTERRUPT Interrupt;
    KIRQL Irql, OldIrql;

    QzInitLists();
    QzPrcb->InterruptCount++;

    /* Interrupts never nest SEH registrations: terminate the chain */
    QzPcr.NtTib.ExceptionList = EXCEPTION_CHAIN_END;

    Head = &QzInterruptLists[Irq];
    if (IsListEmpty(Head))
        Irql = (KIRQL)(PROFILE_LEVEL - Irq);     /* unclaimed line: just dismiss it */
    else
        Irql = CONTAINING_RECORD(Head->Flink, KINTERRUPT, InterruptListEntry)->Irql;

    if (!HalBeginSystemInterrupt(Irql, Vector, &OldIrql))
    {
        /* Spurious, or deferred by the lazy IRQL scheme */
        KiEoiHelper(TrapFrame);
    }

    _enable();
    for (Entry = Head->Flink; Entry != Head; Entry = Entry->Flink)
    {
        Interrupt = CONTAINING_RECORD(Entry, KINTERRUPT, InterruptListEntry);
        QzKeAcquireSpinLockAtDpcLevel(Interrupt->ActualLock);
        if (Interrupt->ServiceRoutine(Interrupt, Interrupt->ServiceContext))
            Interrupt->ServiceCount++;
        QzKeReleaseSpinLockFromDpcLevel(Interrupt->ActualLock);
        Interrupt->DispatchCount++;
    }
    _disable();

    HalEndSystemInterrupt(OldIrql, TrapFrame);
    KiEoiHelper(TrapFrame);
}

/* ---------------------------------------------------------------------- */
/* Exports                                                                 */
/* ---------------------------------------------------------------------- */

VOID
NTAPI
KeInitializeInterrupt(IN PKINTERRUPT Interrupt,
                      IN PKSERVICE_ROUTINE ServiceRoutine,
                      IN PVOID ServiceContext,
                      IN PKSPIN_LOCK SpinLock OPTIONAL,
                      IN ULONG Vector,
                      IN KIRQL Irql,
                      IN KIRQL SynchronizeIrql,
                      IN KINTERRUPT_MODE InterruptMode,
                      IN BOOLEAN ShareVector,
                      IN CHAR ProcessorNumber,
                      IN BOOLEAN FloatingSave)
{
    RtlZeroMemory(Interrupt, sizeof(*Interrupt));
    Interrupt->Type = QZ_INTERRUPT_OBJECT;
    Interrupt->Size = sizeof(KINTERRUPT);
    InitializeListHead(&Interrupt->InterruptListEntry);
    Interrupt->ServiceRoutine = ServiceRoutine;
    Interrupt->ServiceContext = ServiceContext;
    Interrupt->SpinLock = 0;
    Interrupt->ActualLock = SpinLock ? SpinLock : &Interrupt->SpinLock;
    Interrupt->Vector = Vector;
    Interrupt->Irql = Irql;
    Interrupt->SynchronizeIrql = SynchronizeIrql;
    Interrupt->Mode = InterruptMode;
    Interrupt->ShareVector = ShareVector;
    Interrupt->Number = ProcessorNumber;
    Interrupt->FloatingSave = FloatingSave;
    Interrupt->Connected = FALSE;
}

BOOLEAN
NTAPI
KeConnectInterrupt(IN PKINTERRUPT Interrupt)
{
    ULONG Irq = Interrupt->Vector - PRIMARY_VECTOR_BASE;
    PLIST_ENTRY Head;
    PKINTERRUPT First;
    ULONG Flags;

    if (Interrupt->Connected || Interrupt->Vector < PRIMARY_VECTOR_BASE ||
        Irq >= 16 || Interrupt->Irql > HIGH_LEVEL ||
        Interrupt->SynchronizeIrql < Interrupt->Irql)
    {
        return FALSE;
    }

    QzInitLists();
    Head = &QzInterruptLists[Irq];

    Flags = QzSaveFlagsAndDisable();
    if (!IsListEmpty(Head))
    {
        First = CONTAINING_RECORD(Head->Flink, KINTERRUPT, InterruptListEntry);
        if (!First->ShareVector || !Interrupt->ShareVector || First->Mode != Interrupt->Mode)
        {
            QzRestoreFlags(Flags);
            return FALSE;
        }
        InsertTailList(Head, &Interrupt->InterruptListEntry);
    }
    else
    {
        InsertTailList(Head, &Interrupt->InterruptListEntry);
        HalEnableSystemInterrupt(Interrupt->Vector, Interrupt->Irql, Interrupt->Mode);
    }
    Interrupt->Connected = TRUE;
    QzConnectedCount++;
    QzRestoreFlags(Flags);
    return TRUE;
}

BOOLEAN
NTAPI
KeDisconnectInterrupt(IN PKINTERRUPT Interrupt)
{
    ULONG Irq = Interrupt->Vector - PRIMARY_VECTOR_BASE;
    ULONG Flags;

    if (!Interrupt->Connected)
        return FALSE;

    Flags = QzSaveFlagsAndDisable();
    RemoveEntryList(&Interrupt->InterruptListEntry);
    if (IsListEmpty(&QzInterruptLists[Irq]))
        HalDisableSystemInterrupt(Interrupt->Vector, Interrupt->Irql);
    Interrupt->Connected = FALSE;
    QzConnectedCount--;
    QzRestoreFlags(Flags);
    return TRUE;
}

BOOLEAN
NTAPI
KeSynchronizeExecution(IN OUT PKINTERRUPT Interrupt,
                       IN PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
                       IN PVOID SynchronizeContext OPTIONAL)
{
    KIRQL OldIrql;
    BOOLEAN Result;

    OldIrql = QzRaiseIrql(Interrupt->SynchronizeIrql);
    QzKeAcquireSpinLockAtDpcLevel(Interrupt->ActualLock);
    Result = SynchronizeRoutine(SynchronizeContext);
    QzKeReleaseSpinLockFromDpcLevel(Interrupt->ActualLock);
    QzLowerIrql(OldIrql);
    return Result;
}
