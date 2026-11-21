/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     System worker threads (ExQueueWorkItem) and I/O work items
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * One queue served by a few threads; NT's critical/delayed/hypercritical
 * queues differ only in priority, which the round-robin scheduler does not
 * have yet.
 */

#include "qzio.h"

#define QZ_WORKER_THREADS 3

static LIST_ENTRY QzWorkQueue;
static KSPIN_LOCK QzWorkLock;
static KSEMAPHORE QzWorkSemaphore;
static volatile LONG QzWorkItemsDone;

typedef struct _QZ_IO_WORKITEM
{
    WORK_QUEUE_ITEM Item;
    PVOID IoObject;                /* device or driver object */
    PVOID Routine;
    PVOID Context;
    BOOLEAN Extended;              /* IO_WORKITEM_ROUTINE_EX */
    BOOLEAN Allocated;
} QZ_IO_WORKITEM, *PQZ_IO_WORKITEM;

static VOID NTAPI
QzWorkerThread(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    for (;;)
    {
        PWORK_QUEUE_ITEM Item;
        KIRQL OldIrql;

        KeWaitForSingleObject(&QzWorkSemaphore, Executive, KernelMode, FALSE, NULL);
        OldIrql = QzAcquireSpinLock(&QzWorkLock);
        Item = CONTAINING_RECORD(RemoveHeadList(&QzWorkQueue), WORK_QUEUE_ITEM, List);
        QzReleaseSpinLock(&QzWorkLock, OldIrql);

        Item->List.Flink = NULL;
        Item->WorkerRoutine(Item->Parameter);
        InterlockedIncrement(&QzWorkItemsDone);
        if (QzGetCurrentIrql() != PASSIVE_LEVEL)
            KeBugCheckEx(WORKER_THREAD_RETURNED_AT_BAD_IRQL, (ULONG_PTR)Item->WorkerRoutine,
                         QzGetCurrentIrql(), (ULONG_PTR)Item->Parameter, (ULONG_PTR)Item);
    }
}

VOID
QzExWorkerInit(VOID)
{
    ULONG i;

    InitializeListHead(&QzWorkQueue);
    KeInitializeSemaphore(&QzWorkSemaphore, 0, MAXLONG);
    for (i = 0; i < QZ_WORKER_THREADS; i++)
        QzCreateThread("worker", QzWorkerThread, NULL);
}

VOID
NTAPI
ExQueueWorkItem(IN PWORK_QUEUE_ITEM WorkItem,
                IN WORK_QUEUE_TYPE QueueType)
{
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(QueueType);
    if (WorkItem->List.Flink)
        KeBugCheckEx(WORKER_INVALID, 1, (ULONG_PTR)WorkItem, (ULONG_PTR)WorkItem->WorkerRoutine, 0);
    OldIrql = QzAcquireSpinLock(&QzWorkLock);
    InsertTailList(&QzWorkQueue, &WorkItem->List);
    QzReleaseSpinLock(&QzWorkLock, OldIrql);
    KeReleaseSemaphore(&QzWorkSemaphore, 0, 1, FALSE);
}

/* ---------------------------------------------------------------------- */
/* I/O work items: keep the device (and so the driver) alive while queued  */
/* ---------------------------------------------------------------------- */

static VOID NTAPI
QzIoWorkItemRoutine(PVOID Parameter)
{
    PQZ_IO_WORKITEM WorkItem = Parameter;
    PVOID IoObject = WorkItem->IoObject;

    if (WorkItem->Extended)
        ((PIO_WORKITEM_ROUTINE_EX)WorkItem->Routine)(IoObject, WorkItem->Context,
                                                     (PIO_WORKITEM)WorkItem);
    else
        ((PIO_WORKITEM_ROUTINE)WorkItem->Routine)(IoObject, WorkItem->Context);
    /* The routine may have freed the work item: use only the copy */
    ObDereferenceObject(IoObject);
}

ULONG
NTAPI
IoSizeofWorkItem(VOID)
{
    return sizeof(QZ_IO_WORKITEM);
}

VOID
NTAPI
IoInitializeWorkItem(IN PVOID IoObject,
                     IN PIO_WORKITEM IoWorkItem)
{
    PQZ_IO_WORKITEM WorkItem = (PQZ_IO_WORKITEM)IoWorkItem;
    RtlZeroMemory(WorkItem, sizeof(*WorkItem));
    WorkItem->IoObject = IoObject;
}

VOID
NTAPI
IoUninitializeWorkItem(IN PIO_WORKITEM IoWorkItem)
{
    UNREFERENCED_PARAMETER(IoWorkItem);
}

PIO_WORKITEM
NTAPI
IoAllocateWorkItem(IN PDEVICE_OBJECT DeviceObject)
{
    PQZ_IO_WORKITEM WorkItem = ExAllocatePoolWithTag(NonPagedPool, sizeof(*WorkItem), QZ_IO_TAG);
    if (!WorkItem)
        return NULL;
    IoInitializeWorkItem(DeviceObject, (PIO_WORKITEM)WorkItem);
    WorkItem->Allocated = TRUE;
    return (PIO_WORKITEM)WorkItem;
}

VOID
NTAPI
IoFreeWorkItem(IN PIO_WORKITEM IoWorkItem)
{
    ExFreePoolWithTag(IoWorkItem, QZ_IO_TAG);
}

static VOID
QzIoQueue(PIO_WORKITEM IoWorkItem, PVOID Routine, BOOLEAN Extended, WORK_QUEUE_TYPE QueueType,
          PVOID Context)
{
    PQZ_IO_WORKITEM WorkItem = (PQZ_IO_WORKITEM)IoWorkItem;

    ObReferenceObject(WorkItem->IoObject);
    WorkItem->Routine = Routine;
    WorkItem->Extended = Extended;
    WorkItem->Context = Context;
    ExInitializeWorkItem(&WorkItem->Item, QzIoWorkItemRoutine, WorkItem);
    ExQueueWorkItem(&WorkItem->Item, QueueType);
}

VOID
NTAPI
IoQueueWorkItem(IN PIO_WORKITEM IoWorkItem,
                IN PIO_WORKITEM_ROUTINE WorkerRoutine,
                IN WORK_QUEUE_TYPE QueueType,
                IN PVOID Context)
{
    QzIoQueue(IoWorkItem, (PVOID)WorkerRoutine, FALSE, QueueType, Context);
}

VOID
NTAPI
IoQueueWorkItemEx(IN PIO_WORKITEM IoWorkItem,
                  IN PIO_WORKITEM_ROUTINE_EX WorkerRoutine,
                  IN WORK_QUEUE_TYPE QueueType,
                  IN PVOID Context)
{
    QzIoQueue(IoWorkItem, (PVOID)WorkerRoutine, TRUE, QueueType, Context);
}
