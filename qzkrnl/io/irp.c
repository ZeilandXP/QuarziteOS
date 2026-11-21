/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     I/O manager: IRP allocation, dispatch, completion,
 *              cancellation and the IRP-building helpers
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Completion follows the NT contract drivers depend on: stack locations
 * unwind from the bottom up, each completion routine sees the device of
 * the driver that registered it, STATUS_MORE_PROCESSING_REQUIRED stops
 * the unwind, and pending state propagates upward. The final stage (copy
 * back buffered data, free MDLs, store the I/O status, signal the event)
 * runs directly in IoCompleteRequest: NT defers it to an APC in the
 * requesting thread, which only matters for user-mode buffers (stage B6).
 */

#include "qzio.h"
#include "qzps.h"
#include <ndk/kefuncs.h>

#undef IoCallDriver
#undef IoCompleteRequest

static volatile LONG QzIrpAllocations, QzIrpOutstanding;
BOOLEAN QzIoTraceIrps;   /* shell "irptrace": log dispatch and completion */
static KSPIN_LOCK QzIoCancelLock;

ULONG QzIoIrpsAllocated(VOID)   { return (ULONG)QzIrpAllocations; }
ULONG QzIoIrpsOutstanding(VOID) { return (ULONG)QzIrpOutstanding; }

/* ---------------------------------------------------------------------- */
/* Allocation                                                              */
/* ---------------------------------------------------------------------- */

VOID
NTAPI
IoInitializeIrp(IN PIRP Irp,
                IN USHORT PacketSize,
                IN CCHAR StackSize)
{
    RtlZeroMemory(Irp, PacketSize);
    Irp->Type = IO_TYPE_IRP;
    Irp->Size = PacketSize;
    Irp->StackCount = StackSize;
    Irp->CurrentLocation = StackSize + 1;
    Irp->ApcEnvironment = OriginalApcEnvironment;
    Irp->Tail.Overlay.CurrentStackLocation = (PIO_STACK_LOCATION)(Irp + 1) + StackSize;
    InitializeListHead(&Irp->ThreadListEntry);
}

PIRP
NTAPI
IoAllocateIrp(IN CCHAR StackSize,
              IN BOOLEAN ChargeQuota)
{
    USHORT Size = IoSizeOfIrp(StackSize);
    PIRP Irp;

    UNREFERENCED_PARAMETER(ChargeQuota);
    if (StackSize <= 0)
        return NULL;
    Irp = ExAllocatePoolWithTag(NonPagedPool, Size, QZ_IRP_TAG);
    if (!Irp)
        return NULL;
    IoInitializeIrp(Irp, Size, StackSize);
    InterlockedIncrement(&QzIrpAllocations);
    InterlockedIncrement(&QzIrpOutstanding);
    return Irp;
}

VOID
NTAPI
IoFreeIrp(IN PIRP Irp)
{
    if (Irp->Type != IO_TYPE_IRP)
        KeBugCheckEx(MULTIPLE_IRP_COMPLETE_REQUESTS, (ULONG_PTR)Irp, 0, 0, 0);
    Irp->Type = 0;
    InterlockedDecrement(&QzIrpOutstanding);
    ExFreePoolWithTag(Irp, QZ_IRP_TAG);
}

VOID
NTAPI
IoReuseIrp(IN OUT PIRP Irp,
           IN NTSTATUS Status)
{
    UCHAR AllocationFlags = Irp->AllocationFlags;
    IoInitializeIrp(Irp, Irp->Size, Irp->StackCount);
    Irp->AllocationFlags = AllocationFlags;
    Irp->IoStatus.Status = Status;
}

PIRP
NTAPI
IoMakeAssociatedIrp(IN PIRP Irp,
                    IN CCHAR StackSize)
{
    PIRP Associated = IoAllocateIrp(StackSize, FALSE);
    if (!Associated)
        return NULL;
    Associated->Flags |= IRP_ASSOCIATED_IRP;
    Associated->AssociatedIrp.MasterIrp = Irp;
    Associated->Tail.Overlay.Thread = Irp->Tail.Overlay.Thread;
    return Associated;
}

/* ---------------------------------------------------------------------- */
/* Dispatch                                                                */
/* ---------------------------------------------------------------------- */

NTSTATUS
FASTCALL
IofCallDriver(IN PDEVICE_OBJECT DeviceObject,
              IN PIRP Irp)
{
    PIO_STACK_LOCATION Stack;

    if (--Irp->CurrentLocation <= 0)
        KeBugCheckEx(NO_MORE_IRP_STACK_LOCATIONS, (ULONG_PTR)Irp, 0, 0, 0);
    Stack = --Irp->Tail.Overlay.CurrentStackLocation;
    Stack->DeviceObject = DeviceObject;
    if (QzIoTraceIrps)
    {
        NTSTATUS Status;
        if (Stack->MajorFunction == IRP_MJ_DEVICE_CONTROL ||
            Stack->MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL)
            DbgPrint("irp %p: mj %02x ioctl %08lx -> %wZ (%p)\n", Irp, Stack->MajorFunction,
                     Stack->Parameters.DeviceIoControl.IoControlCode,
                     &DeviceObject->DriverObject->DriverName, DeviceObject);
        else
            DbgPrint("irp %p: mj %02x mn %02x -> %wZ (%p)\n", Irp, Stack->MajorFunction,
                     Stack->MinorFunction, &DeviceObject->DriverObject->DriverName, DeviceObject);
        Status = DeviceObject->DriverObject->MajorFunction[Stack->MajorFunction](DeviceObject, Irp);
        DbgPrint("irp %p: %wZ returned 0x%08lx\n", Irp, &DeviceObject->DriverObject->DriverName, Status);
        return Status;
    }
    return DeviceObject->DriverObject->MajorFunction[Stack->MajorFunction](DeviceObject, Irp);
}

NTSTATUS
NTAPI
IoCallDriver(IN PDEVICE_OBJECT DeviceObject,
             IN PIRP Irp)
{
    return IofCallDriver(DeviceObject, Irp);
}

/* ---------------------------------------------------------------------- */
/* Completion                                                              */
/* ---------------------------------------------------------------------- */

static VOID
QzIoFreeMdlChain(PIRP Irp)
{
    PMDL Mdl, Next;

    for (Mdl = Irp->MdlAddress; Mdl; Mdl = Next)
    {
        Next = Mdl->Next;
        if (Mdl->MdlFlags & MDL_PAGES_LOCKED)
            MmUnlockPages(Mdl);
        IoFreeMdl(Mdl);
    }
    Irp->MdlAddress = NULL;
}

#define QZ_IRP_IN_REQUESTER     0x20000000UL    /* the last stage was sent to the requester */

static VOID QzIoFinishIrp(PIRP Irp, CCHAR PriorityBoost);

/* The last stage, in the thread that made the request */
static VOID NTAPI
QzIoCompletionApc(PKAPC Apc, PKNORMAL_ROUTINE *NormalRoutine, PVOID *NormalContext, PVOID *SystemArgument1,
                  PVOID *SystemArgument2)
{
    PIRP Irp = CONTAINING_RECORD(Apc, IRP, Tail.Apc);
    UNREFERENCED_PARAMETER(NormalRoutine);
    UNREFERENCED_PARAMETER(NormalContext);
    /* The APC overlaid Tail.Overlay: its file object came as an argument */
    Irp->Tail.Overlay.OriginalFileObject = *SystemArgument1;
    QzIoFinishIrp(Irp, (CCHAR)(ULONG_PTR)*SystemArgument2);
}

/* The requester ended before it ran: finish without touching its memory */
static VOID NTAPI
QzIoCompletionRundown(PKAPC Apc)
{
    PIRP Irp = CONTAINING_RECORD(Apc, IRP, Tail.Apc);
    Irp->Tail.Overlay.OriginalFileObject = Apc->SystemArgument1;
    Irp->Flags &= ~QZ_IRP_USER_REQUEST;
    Irp->UserBuffer = NULL;
    if (Irp->UserIosb && (ULONG_PTR)Irp->UserIosb < QZ_SYSTEM_RANGE_START)
        Irp->UserIosb = NULL;
    QzIoFinishIrp(Irp, IO_NO_INCREMENT);
}

/* The user APC an I/O request asked for: its kernel part only frees it */
static VOID NTAPI
QzIoUserApcKernelRoutine(PKAPC Apc, PKNORMAL_ROUTINE *NormalRoutine, PVOID *NormalContext,
                         PVOID *SystemArgument1, PVOID *SystemArgument2)
{
    UNREFERENCED_PARAMETER(NormalRoutine);
    UNREFERENCED_PARAMETER(NormalContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    ExFreePool(Apc);
}

/* The I/O manager's own last stage, once every driver is done with it */
static VOID
QzIoFinishIrp(PIRP Irp, CCHAR PriorityBoost)
{
    PFILE_OBJECT FileObject = Irp->Tail.Overlay.OriginalFileObject;
    NTSTATUS Status = Irp->IoStatus.Status;

    /* Paging I/O: the MDL is the caller's (the cache's or Mm's pages) */
    if (Irp->Flags & IRP_PAGING_IO)
    {
        if (Irp->UserIosb)
            *Irp->UserIosb = Irp->IoStatus;
        if (Irp->UserEvent)
            KeSetEvent(Irp->UserEvent, PriorityBoost, FALSE);
        Irp->MdlAddress = NULL;
        IoFreeIrp(Irp);
        return;
    }

    if (Irp->Flags & IRP_ASSOCIATED_IRP)
    {
        PIRP Master = Irp->AssociatedIrp.MasterIrp;
        QzIoFreeMdlChain(Irp);
        IoFreeIrp(Irp);
        if (InterlockedDecrement(&Master->AssociatedIrp.IrpCount) == 0)
            IofCompleteRequest(Master, PriorityBoost);
        return;
    }

    /*
     * A request from user mode finishes in the thread that made it: the
     * buffers, the IOSB and the event handle belong to its process, and
     * this may be a DPC or another process's thread. A special kernel APC
     * takes it there (NT's IopCompleteRequest).
     */
    if ((Irp->Flags & QZ_IRP_USER_REQUEST) && !(Irp->Flags & QZ_IRP_IN_REQUESTER))
    {
        PQZ_THREAD Requester = QzKeThreadOf(Irp->Tail.Overlay.Thread);
        if (Requester && (Requester != QzCurrentThread || KeGetCurrentIrql() != PASSIVE_LEVEL ||
                          Requester->ApcProcess != Requester->Process))
        {
            KeInitializeApc(&Irp->Tail.Apc, (PRKTHREAD)Requester, OriginalApcEnvironment, QzIoCompletionApc,
                            QzIoCompletionRundown, NULL, KernelMode, NULL);
            Irp->Flags |= QZ_IRP_IN_REQUESTER;
            if (KeInsertQueueApc(&Irp->Tail.Apc, FileObject, (PVOID)(ULONG_PTR)PriorityBoost, PriorityBoost))
                return;
            Irp->Tail.Overlay.OriginalFileObject = FileObject;
            /* The thread is gone: nobody to tell, and its address space may be too */
            Irp->Flags &= ~(QZ_IRP_USER_REQUEST | QZ_IRP_IN_REQUESTER);
            Irp->UserBuffer = NULL;
            if (Irp->UserIosb && (ULONG_PTR)Irp->UserIosb < QZ_SYSTEM_RANGE_START)
                Irp->UserIosb = NULL;
        }
    }

    /* Copy buffered output back to the caller */
    if (Irp->Flags & IRP_BUFFERED_IO)
    {
        if ((Irp->Flags & IRP_INPUT_OPERATION) && !NT_ERROR(Status) &&
            Status != STATUS_VERIFY_REQUIRED && Irp->UserBuffer &&
            Irp->AssociatedIrp.SystemBuffer && Irp->IoStatus.Information)
        {
            RtlCopyMemory(Irp->UserBuffer, Irp->AssociatedIrp.SystemBuffer,
                          Irp->IoStatus.Information);
        }
        if (Irp->Flags & IRP_DEALLOCATE_BUFFER)
            ExFreePool(Irp->AssociatedIrp.SystemBuffer);
        Irp->AssociatedIrp.SystemBuffer = NULL;
        Irp->Flags &= ~(IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER);
    }

    QzIoFreeMdlChain(Irp);

    if (Irp->UserIosb)
        *Irp->UserIosb = Irp->IoStatus;

    if (FileObject && !(Irp->Flags & IRP_CREATE_OPERATION))
    {
        FileObject->FinalStatus = Status;
        if (!Irp->UserEvent || !(Irp->Flags & IRP_SYNCHRONOUS_API))
            KeSetEvent(&FileObject->Event, 0, FALSE);
    }

    if (Irp->UserEvent)
    {
        KeSetEvent(Irp->UserEvent, PriorityBoost, FALSE);
        /* An event the caller passed by handle was referenced for us */
        if (FileObject && !(Irp->Flags & IRP_SYNCHRONOUS_API))
            ObDereferenceObject(Irp->UserEvent);
    }

    /*
     * Asynchronous requests: the caller's APC routine, or its completion
     * port. From user mode it is a user APC (this is the requesting thread);
     * from kernel mode a normal kernel APC to the thread that asked
     * (win32k's raw input thread reads the keyboard and mouse that way).
     */
    if (!(Irp->Flags & (IRP_SYNCHRONOUS_API | IRP_CREATE_OPERATION | IRP_CLOSE_OPERATION)) && FileObject)
    {
        PIO_APC_ROUTINE ApcRoutine = Irp->Overlay.AsynchronousParameters.UserApcRoutine;
        PVOID ApcContext = Irp->Overlay.AsynchronousParameters.UserApcContext;
        BOOLEAN FromUser = (Irp->Flags & QZ_IRP_USER_REQUEST) != 0;
        PQZ_THREAD Target = FromUser ? QzCurrentThread : QzKeThreadOf(Irp->Tail.Overlay.Thread);

        if (ApcRoutine && Target)
        {
            PKAPC Apc = ExAllocatePoolWithTag(NonPagedPool, sizeof(KAPC), 'pAzQ');
            if (Apc)
            {
                KeInitializeApc(Apc, (PRKTHREAD)Target, OriginalApcEnvironment, QzIoUserApcKernelRoutine,
                                NULL, (PKNORMAL_ROUTINE)ApcRoutine, FromUser ? UserMode : KernelMode, ApcContext);
                if (!KeInsertQueueApc(Apc, Irp->UserIosb, NULL, PriorityBoost))
                    ExFreePool(Apc);
            }
        }
        else if (!ApcRoutine && FileObject->CompletionContext && !((ULONG_PTR)ApcContext & 1))
        {
            QzIoQueueCompletion(FileObject->CompletionContext->Port, FileObject->CompletionContext->Key,
                                ApcContext, Irp->IoStatus.Status, Irp->IoStatus.Information);
        }
    }

    /* The IRP held a reference on its file object (close and create IRPs
       are owned by the file object's own code) */
    if (FileObject && !(Irp->Flags & (IRP_CREATE_OPERATION | IRP_CLOSE_OPERATION)))
        ObDereferenceObject(FileObject);

    IoFreeIrp(Irp);
}

VOID
FASTCALL
IofCompleteRequest(IN PIRP Irp,
                   IN CCHAR PriorityBoost)
{
    PIO_STACK_LOCATION Stack;
    PDEVICE_OBJECT Device;
    NTSTATUS Status;

    if (Irp->Type != IO_TYPE_IRP || Irp->CurrentLocation > Irp->StackCount + 1)
        KeBugCheckEx(MULTIPLE_IRP_COMPLETE_REQUESTS, (ULONG_PTR)Irp, 0, 0, 0);

    while (Irp->CurrentLocation <= Irp->StackCount)
    {
        PIO_COMPLETION_ROUTINE Routine;
        PVOID Context;
        UCHAR Control;

        Stack = IoGetCurrentIrpStackLocation(Irp);
        Control = Stack->Control;
        Routine = Stack->CompletionRoutine;
        Context = Stack->Context;
        Irp->PendingReturned = (Control & SL_PENDING_RETURNED) != 0;
        Status = Irp->IoStatus.Status;

        /* This location is finished */
        Stack->MinorFunction = 0;
        Stack->Flags = 0;
        Stack->Control = 0;
        Stack->CompletionRoutine = NULL;
        Stack->Context = NULL;
        Stack->FileObject = NULL;

        Irp->CurrentLocation++;
        Irp->Tail.Overlay.CurrentStackLocation++;
        Device = (Irp->CurrentLocation <= Irp->StackCount)
                     ? IoGetCurrentIrpStackLocation(Irp)->DeviceObject : NULL;

        if (Routine &&
            ((NT_SUCCESS(Status) && (Control & SL_INVOKE_ON_SUCCESS)) ||
             (!NT_SUCCESS(Status) && (Control & SL_INVOKE_ON_ERROR)) ||
             (Irp->Cancel && (Control & SL_INVOKE_ON_CANCEL))))
        {
            if (Routine(Device, Irp, Context) == STATUS_MORE_PROCESSING_REQUIRED)
                return;
        }
        else if (Irp->PendingReturned && Irp->CurrentLocation <= Irp->StackCount)
        {
            IoMarkIrpPending(Irp);
        }
    }

    QzIoFinishIrp(Irp, PriorityBoost);
}

VOID
NTAPI
IoCompleteRequest(IN PIRP Irp,
                  IN CCHAR PriorityBoost)
{
    IofCompleteRequest(Irp, PriorityBoost);
}

/* Call a driver and wait for an IRP whose UserEvent/UserIosb are ours */
NTSTATUS
QzIoSynchronousCall(PDEVICE_OBJECT DeviceObject, PIRP Irp, PKEVENT Event,
                    PIO_STATUS_BLOCK IoStatus)
{
    NTSTATUS Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus->Status;
    }
    return Status;
}

/* ---------------------------------------------------------------------- */
/* Cancellation                                                            */
/* ---------------------------------------------------------------------- */

VOID
NTAPI
IoAcquireCancelSpinLock(OUT PKIRQL Irql)
{
    *Irql = QzAcquireSpinLock(&QzIoCancelLock);
}

VOID
NTAPI
IoReleaseCancelSpinLock(IN KIRQL Irql)
{
    QzReleaseSpinLock(&QzIoCancelLock, Irql);
}

BOOLEAN
NTAPI
IoCancelIrp(IN PIRP Irp)
{
    PDRIVER_CANCEL Routine;
    KIRQL OldIrql;

    IoAcquireCancelSpinLock(&OldIrql);
    Irp->Cancel = TRUE;
    Routine = (PDRIVER_CANCEL)InterlockedExchangePointer((PVOID *)&Irp->CancelRoutine, NULL);
    if (!Routine)
    {
        IoReleaseCancelSpinLock(OldIrql);
        return FALSE;
    }
    if (Irp->CurrentLocation > Irp->StackCount + 1)
        KeBugCheckEx(CANCEL_STATE_IN_COMPLETED_IRP, (ULONG_PTR)Irp, (ULONG_PTR)Routine, 0, 0);

    /* The cancel routine releases the cancel spin lock */
    Irp->CancelIrql = OldIrql;
    Routine(IoGetCurrentIrpStackLocation(Irp)->DeviceObject, Irp);
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* IRP builders                                                            */
/* ---------------------------------------------------------------------- */

BOOLEAN
QzIoAttachBuffer(PIRP Irp, PDEVICE_OBJECT Device, PVOID Buffer, ULONG Length, BOOLEAN Read)
{
    if (!Length)
        return TRUE;

    Irp->UserBuffer = Buffer;
    if (Device->Flags & DO_BUFFERED_IO)
    {
        Irp->AssociatedIrp.SystemBuffer = ExAllocatePoolWithTag(NonPagedPool, Length, QZ_IRP_TAG);
        if (!Irp->AssociatedIrp.SystemBuffer)
            return FALSE;
        if (!Read)
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Buffer, Length);
        Irp->Flags |= IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER | (Read ? IRP_INPUT_OPERATION : 0);
    }
    else if (Device->Flags & DO_DIRECT_IO)
    {
        PMDL Mdl = IoAllocateMdl(Buffer, Length, FALSE, FALSE, Irp);
        if (!Mdl)
            return FALSE;
        MmProbeAndLockPages(Mdl, KernelMode, Read ? IoWriteAccess : IoReadAccess);
    }
    return TRUE;
}

static VOID
QzIoFreeBuiltIrp(PIRP Irp)
{
    if (Irp->Flags & IRP_DEALLOCATE_BUFFER)
        ExFreePool(Irp->AssociatedIrp.SystemBuffer);
    QzIoFreeMdlChain(Irp);
    IoFreeIrp(Irp);
}

PIRP
NTAPI
IoBuildAsynchronousFsdRequest(IN ULONG MajorFunction,
                              IN PDEVICE_OBJECT DeviceObject,
                              IN PVOID Buffer OPTIONAL,
                              IN ULONG Length OPTIONAL,
                              IN PLARGE_INTEGER StartingOffset OPTIONAL,
                              IN PIO_STATUS_BLOCK IoStatusBlock OPTIONAL)
{
    PIO_STACK_LOCATION Stack;
    PIRP Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);

    if (!Irp)
        return NULL;
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = (UCHAR)MajorFunction;

    if (MajorFunction == IRP_MJ_READ || MajorFunction == IRP_MJ_WRITE)
    {
        if (!QzIoAttachBuffer(Irp, DeviceObject, Buffer, Length, MajorFunction == IRP_MJ_READ))
        {
            QzIoFreeBuiltIrp(Irp);
            return NULL;
        }
        Stack->Parameters.Read.Length = Length;
        if (StartingOffset)
            Stack->Parameters.Read.ByteOffset = *StartingOffset;
    }

    Irp->UserIosb = IoStatusBlock;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    return Irp;
}

PIRP
NTAPI
IoBuildSynchronousFsdRequest(IN ULONG MajorFunction,
                             IN PDEVICE_OBJECT DeviceObject,
                             IN PVOID Buffer OPTIONAL,
                             IN ULONG Length OPTIONAL,
                             IN PLARGE_INTEGER StartingOffset OPTIONAL,
                             IN PKEVENT Event,
                             IN PIO_STATUS_BLOCK IoStatusBlock)
{
    PIRP Irp = IoBuildAsynchronousFsdRequest(MajorFunction, DeviceObject, Buffer, Length,
                                             StartingOffset, IoStatusBlock);
    if (Irp)
        Irp->UserEvent = Event;
    return Irp;
}

PIRP
NTAPI
IoBuildDeviceIoControlRequest(IN ULONG IoControlCode,
                              IN PDEVICE_OBJECT DeviceObject,
                              IN PVOID InputBuffer OPTIONAL,
                              IN ULONG InputBufferLength,
                              IN PVOID OutputBuffer OPTIONAL,
                              IN ULONG OutputBufferLength,
                              IN BOOLEAN InternalDeviceIoControl,
                              IN PKEVENT Event,
                              OUT PIO_STATUS_BLOCK IoStatusBlock)
{
    PIO_STACK_LOCATION Stack;
    PIRP Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    ULONG Method = METHOD_FROM_CTL_CODE(IoControlCode);

    if (!Irp)
        return NULL;
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = InternalDeviceIoControl ? IRP_MJ_INTERNAL_DEVICE_CONTROL
                                                   : IRP_MJ_DEVICE_CONTROL;
    Stack->Parameters.DeviceIoControl.IoControlCode = IoControlCode;
    Stack->Parameters.DeviceIoControl.InputBufferLength = InputBufferLength;
    Stack->Parameters.DeviceIoControl.OutputBufferLength = OutputBufferLength;

    switch (Method)
    {
        case METHOD_BUFFERED:
        {
            ULONG Size = max(InputBufferLength, OutputBufferLength);
            if (Size)
            {
                Irp->AssociatedIrp.SystemBuffer = ExAllocatePoolWithTag(NonPagedPool, Size,
                                                                        QZ_IRP_TAG);
                if (!Irp->AssociatedIrp.SystemBuffer)
                {
                    IoFreeIrp(Irp);
                    return NULL;
                }
                if (InputBuffer && InputBufferLength)
                    RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, InputBuffer, InputBufferLength);
                Irp->Flags = IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER |
                             (OutputBufferLength ? IRP_INPUT_OPERATION : 0);
            }
            Irp->UserBuffer = OutputBuffer;
            break;
        }

        case METHOD_IN_DIRECT:
        case METHOD_OUT_DIRECT:
            if (InputBuffer && InputBufferLength)
            {
                Irp->AssociatedIrp.SystemBuffer = ExAllocatePoolWithTag(NonPagedPool,
                                                                        InputBufferLength,
                                                                        QZ_IRP_TAG);
                if (!Irp->AssociatedIrp.SystemBuffer)
                {
                    IoFreeIrp(Irp);
                    return NULL;
                }
                RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, InputBuffer, InputBufferLength);
                Irp->Flags = IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER;
            }
            if (OutputBuffer && OutputBufferLength)
            {
                PMDL Mdl = IoAllocateMdl(OutputBuffer, OutputBufferLength, FALSE, FALSE, Irp);
                if (!Mdl)
                {
                    QzIoFreeBuiltIrp(Irp);
                    return NULL;
                }
                MmProbeAndLockPages(Mdl, KernelMode,
                                    Method == METHOD_IN_DIRECT ? IoReadAccess : IoWriteAccess);
            }
            Irp->UserBuffer = OutputBuffer;
            break;

        default: /* METHOD_NEITHER */
            Irp->UserBuffer = OutputBuffer;
            Stack->Parameters.DeviceIoControl.Type3InputBuffer = InputBuffer;
            break;
    }

    Irp->UserIosb = IoStatusBlock;
    Irp->UserEvent = Event;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    return Irp;
}

/* ---------------------------------------------------------------------- */
/* Remove locks (the free-build variant: a count and an event)             */
/* ---------------------------------------------------------------------- */

VOID
NTAPI
IoInitializeRemoveLockEx(IN PIO_REMOVE_LOCK Lock,
                         IN ULONG AllocateTag,
                         IN ULONG MaxLockedMinutes,
                         IN ULONG HighWatermark,
                         IN ULONG RemlockSize)
{
    UNREFERENCED_PARAMETER(AllocateTag);
    UNREFERENCED_PARAMETER(MaxLockedMinutes);
    UNREFERENCED_PARAMETER(HighWatermark);

    RtlZeroMemory(Lock, RemlockSize);
    Lock->Common.Removed = FALSE;
    Lock->Common.IoCount = 1;
    KeInitializeEvent(&Lock->Common.RemoveEvent, SynchronizationEvent, FALSE);
}

NTSTATUS
NTAPI
IoAcquireRemoveLockEx(IN PIO_REMOVE_LOCK RemoveLock,
                      IN OPTIONAL PVOID Tag,
                      IN LPCSTR File,
                      IN ULONG Line,
                      IN ULONG RemlockSize)
{
    UNREFERENCED_PARAMETER(Tag);
    UNREFERENCED_PARAMETER(File);
    UNREFERENCED_PARAMETER(Line);
    UNREFERENCED_PARAMETER(RemlockSize);

    InterlockedIncrement(&RemoveLock->Common.IoCount);
    if (RemoveLock->Common.Removed)
    {
        if (InterlockedDecrement(&RemoveLock->Common.IoCount) == 0)
            KeSetEvent(&RemoveLock->Common.RemoveEvent, 0, FALSE);
        return STATUS_DELETE_PENDING;
    }
    return STATUS_SUCCESS;
}

VOID
NTAPI
IoReleaseRemoveLockEx(IN PIO_REMOVE_LOCK RemoveLock,
                      IN PVOID Tag,
                      IN ULONG RemlockSize)
{
    UNREFERENCED_PARAMETER(Tag);
    UNREFERENCED_PARAMETER(RemlockSize);

    if (InterlockedDecrement(&RemoveLock->Common.IoCount) == 0)
        KeSetEvent(&RemoveLock->Common.RemoveEvent, 0, FALSE);
}

VOID
NTAPI
IoReleaseRemoveLockAndWaitEx(IN PIO_REMOVE_LOCK RemoveLock,
                             IN PVOID Tag,
                             IN ULONG RemlockSize)
{
    UNREFERENCED_PARAMETER(Tag);
    UNREFERENCED_PARAMETER(RemlockSize);

    RemoveLock->Common.Removed = TRUE;
    InterlockedDecrement(&RemoveLock->Common.IoCount);   /* the initial count */
    if (InterlockedDecrement(&RemoveLock->Common.IoCount) > 0)
        KeWaitForSingleObject(&RemoveLock->Common.RemoveEvent, Executive, KernelMode, FALSE, NULL);
}

/* ---------------------------------------------------------------------- */
/* Paging I/O: the cache manager's and Mm's reads and writes of file pages */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzIoPagingRequest(UCHAR Major, PFILE_OBJECT FileObject, PMDL Mdl, PLARGE_INTEGER Offset, PKEVENT Event,
                  PIO_STATUS_BLOCK StatusBlock, ULONG ExtraFlags)
{
    PDEVICE_OBJECT Device = IoGetRelatedDeviceObject(FileObject);
    PIO_STACK_LOCATION Stack;
    PIRP Irp = IoAllocateIrp(Device->StackSize, FALSE);

    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    Irp->MdlAddress = Mdl;
    Irp->UserBuffer = (PUCHAR)Mdl->StartVa + Mdl->ByteOffset;
    Irp->UserEvent = Event;
    Irp->UserIosb = StatusBlock;
    Irp->Flags = IRP_PAGING_IO | IRP_NOCACHE | IRP_SYNCHRONOUS_PAGING_IO | ExtraFlags;
    Irp->RequestorMode = KernelMode;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->Tail.Overlay.OriginalFileObject = FileObject;
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = Major;
    Stack->FileObject = FileObject;
    if (Major == IRP_MJ_READ)
    {
        Stack->Parameters.Read.Length = Mdl->ByteCount;
        Stack->Parameters.Read.ByteOffset = *Offset;
    }
    else
    {
        Stack->Parameters.Write.Length = Mdl->ByteCount;
        Stack->Parameters.Write.ByteOffset = *Offset;
    }
    return IoCallDriver(Device, Irp);
}

NTSTATUS
NTAPI
IoPageRead(IN PFILE_OBJECT FileObject, IN PMDL Mdl, IN PLARGE_INTEGER Offset, IN PKEVENT Event,
           IN PIO_STATUS_BLOCK StatusBlock)
{
    return QzIoPagingRequest(IRP_MJ_READ, FileObject, Mdl, Offset, Event, StatusBlock, IRP_INPUT_OPERATION);
}

NTSTATUS
NTAPI
IoSynchronousPageWrite(IN PFILE_OBJECT FileObject, IN PMDL Mdl, IN PLARGE_INTEGER Offset, IN PKEVENT Event,
                       IN PIO_STATUS_BLOCK StatusBlock)
{
    return QzIoPagingRequest(IRP_MJ_WRITE, FileObject, Mdl, Offset, Event, StatusBlock, 0);
}
