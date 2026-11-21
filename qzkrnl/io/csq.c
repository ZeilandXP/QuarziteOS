/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Cancel-safe IRP queues (IoCsq*)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * The driver owns the queue and its lock; these routines keep the IRPs'
 * cancel routines and the queue consistent. An IRP's DriverContext[3]
 * points at its IO_CSQ_IRP_CONTEXT if the driver gave one, else at the
 * queue itself (both start with a Type field).
 */

#include "qzio.h"

static PIO_CSQ
QzCsqOf(PIRP Irp, PIO_CSQ_IRP_CONTEXT *Context)
{
    PVOID Owner = Irp->Tail.Overlay.DriverContext[3];

    *Context = NULL;
    if (Owner && *(PULONG)Owner == IO_TYPE_CSQ_IRP_CONTEXT)
    {
        *Context = Owner;
        return (*Context)->Csq;
    }
    return Owner;
}

static VOID NTAPI
QzCsqCancel(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_CSQ_IRP_CONTEXT Context;
    PIO_CSQ Csq;
    KIRQL Irql;

    UNREFERENCED_PARAMETER(DeviceObject);
    IoReleaseCancelSpinLock(Irp->CancelIrql);
    Csq = QzCsqOf(Irp, &Context);
    Csq->CsqAcquireLock(Csq, &Irql);
    Csq->CsqRemoveIrp(Csq, Irp);
    if (Context)
        Context->Irp = NULL;
    Irp->Tail.Overlay.DriverContext[3] = NULL;
    Csq->CsqReleaseLock(Csq, Irql);
    Csq->CsqCompleteCanceledIrp(Csq, Irp);
}

NTSTATUS
NTAPI
IoCsqInitialize(IN PIO_CSQ Csq,
                IN PIO_CSQ_INSERT_IRP CsqInsertIrp,
                IN PIO_CSQ_REMOVE_IRP CsqRemoveIrp,
                IN PIO_CSQ_PEEK_NEXT_IRP CsqPeekNextIrp,
                IN PIO_CSQ_ACQUIRE_LOCK CsqAcquireLock,
                IN PIO_CSQ_RELEASE_LOCK CsqReleaseLock,
                IN PIO_CSQ_COMPLETE_CANCELED_IRP CsqCompleteCanceledIrp)
{
    Csq->Type = IO_TYPE_CSQ;
    Csq->CsqInsertIrp = CsqInsertIrp;
    Csq->CsqRemoveIrp = CsqRemoveIrp;
    Csq->CsqPeekNextIrp = CsqPeekNextIrp;
    Csq->CsqAcquireLock = CsqAcquireLock;
    Csq->CsqReleaseLock = CsqReleaseLock;
    Csq->CsqCompleteCanceledIrp = CsqCompleteCanceledIrp;
    Csq->ReservePointer = NULL;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
IoCsqInitializeEx(IN PIO_CSQ Csq,
                  IN PIO_CSQ_INSERT_IRP_EX CsqInsertIrpEx,
                  IN PIO_CSQ_REMOVE_IRP CsqRemoveIrp,
                  IN PIO_CSQ_PEEK_NEXT_IRP CsqPeekNextIrp,
                  IN PIO_CSQ_ACQUIRE_LOCK CsqAcquireLock,
                  IN PIO_CSQ_RELEASE_LOCK CsqReleaseLock,
                  IN PIO_CSQ_COMPLETE_CANCELED_IRP CsqCompleteCanceledIrp)
{
    IoCsqInitialize(Csq, (PIO_CSQ_INSERT_IRP)CsqInsertIrpEx, CsqRemoveIrp, CsqPeekNextIrp, CsqAcquireLock,
                    CsqReleaseLock, CsqCompleteCanceledIrp);
    Csq->Type = IO_TYPE_CSQ_EX;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
IoCsqInsertIrpEx(IN PIO_CSQ Csq,
                 IN PIRP Irp,
                 IN PIO_CSQ_IRP_CONTEXT Context OPTIONAL,
                 IN PVOID InsertContext OPTIONAL)
{
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL Irql;

    Csq->CsqAcquireLock(Csq, &Irql);
    if (Csq->Type == IO_TYPE_CSQ_EX)
    {
        Status = ((PIO_CSQ_INSERT_IRP_EX)Csq->CsqInsertIrp)(Csq, Irp, InsertContext);
        if (!NT_SUCCESS(Status))
        {
            Csq->CsqReleaseLock(Csq, Irql);
            return Status;
        }
    }
    else
    {
        Csq->CsqInsertIrp(Csq, Irp);
    }

    if (Context)
    {
        Context->Type = IO_TYPE_CSQ_IRP_CONTEXT;
        Context->Irp = Irp;
        Context->Csq = Csq;
        Irp->Tail.Overlay.DriverContext[3] = Context;
    }
    else
    {
        Irp->Tail.Overlay.DriverContext[3] = Csq;
    }

    IoSetCancelRoutine(Irp, QzCsqCancel);
    if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL))
    {
        /* Cancelled before we armed the routine, and it did not run: undo */
        Csq->CsqRemoveIrp(Csq, Irp);
        if (Context)
            Context->Irp = NULL;
        Irp->Tail.Overlay.DriverContext[3] = NULL;
        Csq->CsqReleaseLock(Csq, Irql);
        Csq->CsqCompleteCanceledIrp(Csq, Irp);
        return Status;
    }
    IoMarkIrpPending(Irp);
    Csq->CsqReleaseLock(Csq, Irql);
    return Status;
}

VOID
NTAPI
IoCsqInsertIrp(IN PIO_CSQ Csq,
               IN PIRP Irp,
               IN PIO_CSQ_IRP_CONTEXT Context OPTIONAL)
{
    IoCsqInsertIrpEx(Csq, Irp, Context, NULL);
}

PIRP
NTAPI
IoCsqRemoveIrp(IN PIO_CSQ Csq,
               IN PIO_CSQ_IRP_CONTEXT Context)
{
    PIRP Irp;
    KIRQL Irql;

    Csq->CsqAcquireLock(Csq, &Irql);
    Irp = Context->Irp;
    if (Irp)
    {
        if (!IoSetCancelRoutine(Irp, NULL))
        {
            Irp = NULL;   /* being cancelled: the cancel routine has it */
        }
        else
        {
            Csq->CsqRemoveIrp(Csq, Irp);
            Context->Irp = NULL;
            Irp->Tail.Overlay.DriverContext[3] = NULL;
        }
    }
    Csq->CsqReleaseLock(Csq, Irql);
    return Irp;
}

PIRP
NTAPI
IoCsqRemoveNextIrp(IN PIO_CSQ Csq,
                   IN PVOID PeekContext OPTIONAL)
{
    PIO_CSQ_IRP_CONTEXT Context;
    PIRP Irp;
    KIRQL Irql;

    Csq->CsqAcquireLock(Csq, &Irql);
    Irp = Csq->CsqPeekNextIrp(Csq, NULL, PeekContext);
    while (Irp)
    {
        if (IoSetCancelRoutine(Irp, NULL))
        {
            Csq->CsqRemoveIrp(Csq, Irp);
            QzCsqOf(Irp, &Context);
            if (Context)
                Context->Irp = NULL;
            Irp->Tail.Overlay.DriverContext[3] = NULL;
            break;
        }
        /* Its cancel routine is running: skip it */
        Irp = Csq->CsqPeekNextIrp(Csq, Irp, PeekContext);
    }
    Csq->CsqReleaseLock(Csq, Irql);
    return Irp;
}
