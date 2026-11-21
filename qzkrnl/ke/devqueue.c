/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel device queues (KDEVICE_QUEUE)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * A device queue is busy while its driver works on a request; inserting
 * into a busy queue parks the entry, removing from an empty one makes the
 * queue idle again. Callers run at DISPATCH_LEVEL.
 */

#include "qz.h"

VOID
NTAPI
KeInitializeDeviceQueue(IN PKDEVICE_QUEUE DeviceQueue)
{
    DeviceQueue->Type = DeviceQueueObject;
    DeviceQueue->Size = sizeof(KDEVICE_QUEUE);
    InitializeListHead(&DeviceQueue->DeviceListHead);
    KeInitializeSpinLock(&DeviceQueue->Lock);
    DeviceQueue->Busy = FALSE;
}

static BOOLEAN
QzInsertDeviceQueue(PKDEVICE_QUEUE DeviceQueue, PKDEVICE_QUEUE_ENTRY Entry, BOOLEAN ByKey, ULONG Key)
{
    BOOLEAN Inserted = FALSE;
    KIRQL OldIrql = QzAcquireSpinLock(&DeviceQueue->Lock);

    if (DeviceQueue->Busy)
    {
        if (ByKey)
        {
            PLIST_ENTRY Link;
            Entry->SortKey = Key;
            for (Link = DeviceQueue->DeviceListHead.Flink; Link != &DeviceQueue->DeviceListHead;
                 Link = Link->Flink)
            {
                if (CONTAINING_RECORD(Link, KDEVICE_QUEUE_ENTRY, DeviceListEntry)->SortKey > Key)
                    break;
            }
            InsertTailList(Link, &Entry->DeviceListEntry);   /* i.e. before Link */
        }
        else
        {
            InsertTailList(&DeviceQueue->DeviceListHead, &Entry->DeviceListEntry);
        }
        Inserted = TRUE;
    }
    else
    {
        DeviceQueue->Busy = TRUE;
    }
    Entry->Inserted = Inserted;
    QzReleaseSpinLock(&DeviceQueue->Lock, OldIrql);
    return Inserted;
}

BOOLEAN
NTAPI
KeInsertDeviceQueue(IN PKDEVICE_QUEUE DeviceQueue,
                    IN PKDEVICE_QUEUE_ENTRY DeviceQueueEntry)
{
    return QzInsertDeviceQueue(DeviceQueue, DeviceQueueEntry, FALSE, 0);
}

BOOLEAN
NTAPI
KeInsertByKeyDeviceQueue(IN PKDEVICE_QUEUE DeviceQueue,
                         IN PKDEVICE_QUEUE_ENTRY DeviceQueueEntry,
                         IN ULONG SortKey)
{
    return QzInsertDeviceQueue(DeviceQueue, DeviceQueueEntry, TRUE, SortKey);
}

PKDEVICE_QUEUE_ENTRY
NTAPI
KeRemoveDeviceQueue(IN PKDEVICE_QUEUE DeviceQueue)
{
    PKDEVICE_QUEUE_ENTRY Entry = NULL;
    KIRQL OldIrql = QzAcquireSpinLock(&DeviceQueue->Lock);

    if (IsListEmpty(&DeviceQueue->DeviceListHead))
    {
        DeviceQueue->Busy = FALSE;
    }
    else
    {
        Entry = CONTAINING_RECORD(RemoveHeadList(&DeviceQueue->DeviceListHead),
                                  KDEVICE_QUEUE_ENTRY, DeviceListEntry);
        Entry->Inserted = FALSE;
    }
    QzReleaseSpinLock(&DeviceQueue->Lock, OldIrql);
    return Entry;
}

/* The first entry with SortKey >= Key, wrapping around to the head */
static PKDEVICE_QUEUE_ENTRY
QzRemoveByKey(PKDEVICE_QUEUE DeviceQueue, ULONG Key)
{
    PLIST_ENTRY Link;
    PKDEVICE_QUEUE_ENTRY Entry = NULL;

    for (Link = DeviceQueue->DeviceListHead.Flink; Link != &DeviceQueue->DeviceListHead;
         Link = Link->Flink)
    {
        PKDEVICE_QUEUE_ENTRY Candidate = CONTAINING_RECORD(Link, KDEVICE_QUEUE_ENTRY, DeviceListEntry);
        if (Candidate->SortKey >= Key)
        {
            Entry = Candidate;
            break;
        }
    }
    if (!Entry)
        Entry = CONTAINING_RECORD(DeviceQueue->DeviceListHead.Flink, KDEVICE_QUEUE_ENTRY,
                                  DeviceListEntry);
    RemoveEntryList(&Entry->DeviceListEntry);
    Entry->Inserted = FALSE;
    return Entry;
}

PKDEVICE_QUEUE_ENTRY
NTAPI
KeRemoveByKeyDeviceQueue(IN PKDEVICE_QUEUE DeviceQueue,
                         IN ULONG SortKey)
{
    PKDEVICE_QUEUE_ENTRY Entry = NULL;
    KIRQL OldIrql = QzAcquireSpinLock(&DeviceQueue->Lock);

    if (IsListEmpty(&DeviceQueue->DeviceListHead))
        DeviceQueue->Busy = FALSE;
    else
        Entry = QzRemoveByKey(DeviceQueue, SortKey);
    QzReleaseSpinLock(&DeviceQueue->Lock, OldIrql);
    return Entry;
}

PKDEVICE_QUEUE_ENTRY
NTAPI
KeRemoveByKeyDeviceQueueIfBusy(IN PKDEVICE_QUEUE DeviceQueue,
                               IN ULONG SortKey)
{
    PKDEVICE_QUEUE_ENTRY Entry = NULL;
    KIRQL OldIrql = QzAcquireSpinLock(&DeviceQueue->Lock);

    if (DeviceQueue->Busy)
    {
        if (IsListEmpty(&DeviceQueue->DeviceListHead))
            DeviceQueue->Busy = FALSE;
        else
            Entry = QzRemoveByKey(DeviceQueue, SortKey);
    }
    QzReleaseSpinLock(&DeviceQueue->Lock, OldIrql);
    return Entry;
}

BOOLEAN
NTAPI
KeRemoveEntryDeviceQueue(IN PKDEVICE_QUEUE DeviceQueue,
                         IN PKDEVICE_QUEUE_ENTRY DeviceQueueEntry)
{
    BOOLEAN WasInserted;
    KIRQL OldIrql = QzAcquireSpinLock(&DeviceQueue->Lock);

    WasInserted = DeviceQueueEntry->Inserted;
    if (WasInserted)
    {
        DeviceQueueEntry->Inserted = FALSE;
        RemoveEntryList(&DeviceQueueEntry->DeviceListEntry);
    }
    QzReleaseSpinLock(&DeviceQueue->Lock, OldIrql);
    return WasInserted;
}
