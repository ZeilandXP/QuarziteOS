/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     I/O manager: StartIo packet queueing, interrupt connection,
 *              error log, configuration counters, process/thread queries
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qzio.h"

/* ---------------------------------------------------------------------- */
/* StartIo serialization                                                   */
/* ---------------------------------------------------------------------- */

VOID
NTAPI
IoStartPacket(IN PDEVICE_OBJECT DeviceObject,
              IN PIRP Irp,
              IN PULONG Key OPTIONAL,
              IN PDRIVER_CANCEL CancelFunction OPTIONAL)
{
    KIRQL OldIrql, CancelIrql = PASSIVE_LEVEL;
    BOOLEAN Inserted;

    OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    if (CancelFunction)
    {
        IoAcquireCancelSpinLock(&CancelIrql);
        Irp->CancelRoutine = CancelFunction;
    }

    Inserted = Key ? KeInsertByKeyDeviceQueue(&DeviceObject->DeviceQueue,
                                              &Irp->Tail.Overlay.DeviceQueueEntry, *Key)
                   : KeInsertDeviceQueue(&DeviceObject->DeviceQueue,
                                         &Irp->Tail.Overlay.DeviceQueueEntry);
    if (!Inserted)
    {
        /* The device was idle: start it now */
        DeviceObject->CurrentIrp = Irp;
        if (CancelFunction)
            IoReleaseCancelSpinLock(CancelIrql);
        DeviceObject->DriverObject->DriverStartIo(DeviceObject, Irp);
    }
    else if (CancelFunction)
    {
        if (Irp->Cancel)
        {
            /* Cancelled while we queued it */
            Irp->CancelRoutine = NULL;
            Irp->CancelIrql = CancelIrql;
            CancelFunction(DeviceObject, Irp);
        }
        else
        {
            IoReleaseCancelSpinLock(CancelIrql);
        }
    }
    QzLowerIrql(OldIrql);
}

static VOID
QzIoStartNext(PDEVICE_OBJECT DeviceObject, BOOLEAN Cancelable, BOOLEAN ByKey, ULONG Key)
{
    PKDEVICE_QUEUE_ENTRY Entry;
    KIRQL CancelIrql = PASSIVE_LEVEL;

    if (Cancelable)
        IoAcquireCancelSpinLock(&CancelIrql);
    DeviceObject->CurrentIrp = NULL;
    Entry = ByKey ? KeRemoveByKeyDeviceQueue(&DeviceObject->DeviceQueue, Key)
                  : KeRemoveDeviceQueue(&DeviceObject->DeviceQueue);
    if (Entry)
    {
        PIRP Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.DeviceQueueEntry);
        DeviceObject->CurrentIrp = Irp;
        if (Cancelable)
            IoReleaseCancelSpinLock(CancelIrql);
        DeviceObject->DriverObject->DriverStartIo(DeviceObject, Irp);
    }
    else if (Cancelable)
    {
        IoReleaseCancelSpinLock(CancelIrql);
    }
}

VOID
NTAPI
IoStartNextPacket(IN PDEVICE_OBJECT DeviceObject,
                  IN BOOLEAN Cancelable)
{
    QzIoStartNext(DeviceObject, Cancelable, FALSE, 0);
}

VOID
NTAPI
IoStartNextPacketByKey(IN PDEVICE_OBJECT DeviceObject,
                       IN BOOLEAN Cancelable,
                       IN ULONG Key)
{
    QzIoStartNext(DeviceObject, Cancelable, TRUE, Key);
}

/* ---------------------------------------------------------------------- */
/* Interrupts (one processor: one interrupt object)                        */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
IoConnectInterrupt(OUT PKINTERRUPT *InterruptObject,
                   IN PKSERVICE_ROUTINE ServiceRoutine,
                   IN PVOID ServiceContext OPTIONAL,
                   IN PKSPIN_LOCK SpinLock OPTIONAL,
                   IN ULONG Vector,
                   IN KIRQL Irql,
                   IN KIRQL SynchronizeIrql,
                   IN KINTERRUPT_MODE InterruptMode,
                   IN BOOLEAN ShareVector,
                   IN KAFFINITY ProcessorEnableMask,
                   IN BOOLEAN FloatingSave)
{
    PKINTERRUPT Interrupt;

    *InterruptObject = NULL;
    if (!(ProcessorEnableMask & 1) || SynchronizeIrql < Irql)
        return STATUS_INVALID_PARAMETER;

    Interrupt = ExAllocatePoolWithTag(NonPagedPool, sizeof(KINTERRUPT), QZ_IO_TAG);
    if (!Interrupt)
        return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeInterrupt(Interrupt, ServiceRoutine, ServiceContext, SpinLock, Vector, Irql,
                          SynchronizeIrql, InterruptMode, ShareVector, 0, FloatingSave);
    if (!KeConnectInterrupt(Interrupt))
    {
        ExFreePoolWithTag(Interrupt, QZ_IO_TAG);
        return STATUS_INVALID_PARAMETER;
    }
    *InterruptObject = Interrupt;
    return STATUS_SUCCESS;
}

VOID
NTAPI
IoDisconnectInterrupt(IN PKINTERRUPT InterruptObject)
{
    KeDisconnectInterrupt(InterruptObject);
    ExFreePoolWithTag(InterruptObject, QZ_IO_TAG);
}

/* ---------------------------------------------------------------------- */
/* Error log: entries go to the debug output until there is an event log  */
/* ---------------------------------------------------------------------- */

PVOID
NTAPI
IoAllocateErrorLogEntry(IN PVOID IoObject,
                        IN UCHAR EntrySize)
{
    PIO_ERROR_LOG_PACKET Packet;

    UNREFERENCED_PARAMETER(IoObject);
    if (EntrySize < sizeof(IO_ERROR_LOG_PACKET) || EntrySize > ERROR_LOG_MAXIMUM_SIZE)
        return NULL;
    Packet = ExAllocatePoolWithTag(NonPagedPool, EntrySize, QZ_IO_TAG);
    if (Packet)
        RtlZeroMemory(Packet, EntrySize);
    return Packet;
}

VOID
NTAPI
IoFreeErrorLogEntry(IN PVOID ElEntry)
{
    ExFreePoolWithTag(ElEntry, QZ_IO_TAG);
}

VOID
NTAPI
IoWriteErrorLogEntry(IN PVOID ElEntry)
{
    PIO_ERROR_LOG_PACKET Packet = ElEntry;
    DbgPrint(QZ_KERNEL_NAME ": error log: code 0x%08lx, status 0x%08lx, unique 0x%lx\n",
             Packet->ErrorCode, Packet->FinalStatus, Packet->UniqueErrorValue);
    ExFreePoolWithTag(ElEntry, QZ_IO_TAG);
}

/* ---------------------------------------------------------------------- */
/* Counters, versions, WMI                                                 */
/* ---------------------------------------------------------------------- */

static CONFIGURATION_INFORMATION QzIoConfiguration =
{
    .Version = sizeof(CONFIGURATION_INFORMATION),
};

PCONFIGURATION_INFORMATION
NTAPI
IoGetConfigurationInformation(VOID)
{
    return &QzIoConfiguration;
}

/* Windows 7 implements WDM 6.1 */
BOOLEAN
NTAPI
IoIsWdmVersionAvailable(IN UCHAR MajorVersion,
                        IN UCHAR MinorVersion)
{
    return MajorVersion < 6 || (MajorVersion == 6 && MinorVersion <= 0x01);
}

NTSTATUS
NTAPI
IoWMIRegistrationControl(IN PDEVICE_OBJECT DeviceObject,
                         IN ULONG Action)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Action);
    return STATUS_SUCCESS;   /* no WMI consumers yet */
}

/* ---------------------------------------------------------------------- */
/* Requestor and stack queries (processes are in ps/process.c)            */
/* ---------------------------------------------------------------------- */

PEPROCESS
NTAPI
IoGetRequestorProcess(IN PIRP Irp)
{
    if (Irp->Tail.Overlay.Thread)
        return PsGetThreadProcess(Irp->Tail.Overlay.Thread);
    return IoGetCurrentProcess();
}

ULONG
NTAPI
IoGetRequestorProcessId(IN PIRP Irp)
{
    return (ULONG)(ULONG_PTR)PsGetProcessId(IoGetRequestorProcess(Irp));
}

VOID
NTAPI
IoGetStackLimits(OUT PULONG_PTR LowLimit,
                 OUT PULONG_PTR HighLimit)
{
    *LowLimit = (ULONG_PTR)QzCurrentThread->StackBase;
    *HighLimit = (ULONG_PTR)QzCurrentThread->StackBase + QzCurrentThread->StackSize;
}
