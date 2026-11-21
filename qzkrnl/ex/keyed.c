/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Keyed events, and hard errors (no one to show them to yet)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * A keyed event is a rendezvous: a thread releasing a key waits until a
 * thread waiting for that key arrives (or the other way round), then both
 * go on. ntdll falls back to the global one,
 * \KernelObjects\CritSecOutOfMemoryEvent, when a critical section cannot
 * get an event of its own.
 */

#include "qzio.h"
#include "qzps.h"

#define QZ_KEYED_TAG 'yeKQ'

typedef struct _QZ_KEYED_EVENT
{
    LIST_ENTRY Waiters;                 /* QZ_KEYED_WAIT, both kinds */
} QZ_KEYED_EVENT, *PQZ_KEYED_EVENT;

typedef struct _QZ_KEYED_WAIT
{
    LIST_ENTRY Link;
    PVOID Key;
    BOOLEAN Releasing;                  /* a releaser waiting for a waiter */
    BOOLEAN Matched;
    KEVENT Event;
} QZ_KEYED_WAIT, *PQZ_KEYED_WAIT;

static POBJECT_TYPE QzKeyedEventType;
static PQZ_KEYED_EVENT QzCritSecEvent;
static FAST_MUTEX QzKeyedLock;

VOID
QzExInitKeyedEvents(VOID)
{
    static GENERIC_MAPPING Mapping =
    {
        STANDARD_RIGHTS_READ | EVENT_QUERY_STATE, STANDARD_RIGHTS_WRITE | EVENT_MODIFY_STATE,
        STANDARD_RIGHTS_EXECUTE, EVENT_ALL_ACCESS
    };
    UNICODE_STRING TypeName = RTL_CONSTANT_STRING(L"KeyedEvent");
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\KernelObjects\\CritSecOutOfMemoryEvent");
    OBJECT_TYPE_INITIALIZER Init;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Handle;

    ExInitializeFastMutex(&QzKeyedLock);
    RtlZeroMemory(&Init, sizeof(Init));
    Init.Length = sizeof(Init);
    Init.GenericMapping = Mapping;
    Init.ValidAccessMask = EVENT_ALL_ACCESS;
    Init.PoolType = NonPagedPool;
    Init.UseDefaultObject = TRUE;
    if (!NT_SUCCESS(ObCreateObjectType(&TypeName, &Init, NULL, &QzKeyedEventType)))
        return;
    InitializeObjectAttributes(&Attributes, &Name, OBJ_PERMANENT | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ObCreateObject(KernelMode, QzKeyedEventType, &Attributes, KernelMode, NULL,
                                  sizeof(QZ_KEYED_EVENT), 0, 0, (PVOID *)&QzCritSecEvent)))
    {
        InitializeListHead(&QzCritSecEvent->Waiters);
        ObReferenceObject(QzCritSecEvent);
        if (NT_SUCCESS(ObInsertObject(QzCritSecEvent, NULL, EVENT_ALL_ACCESS, 0, NULL, &Handle)))
            ZwClose(Handle);
    }
}

NTSTATUS
NTAPI
NtCreateKeyedEvent(OUT PHANDLE OutHandle, IN ACCESS_MASK AccessMask, IN POBJECT_ATTRIBUTES ObjectAttributes,
                   IN ULONG Flags)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PQZ_KEYED_EVENT Event;
    HANDLE Handle;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Flags);
    Status = ObCreateObject(Mode, QzKeyedEventType, ObjectAttributes, Mode, NULL, sizeof(QZ_KEYED_EVENT), 0, 0,
                            (PVOID *)&Event);
    if (!NT_SUCCESS(Status))
        return Status;
    InitializeListHead(&Event->Waiters);
    Status = ObInsertObject(Event, NULL, AccessMask, 0, NULL, &Handle);
    if (NT_SUCCESS(Status) && !QzWriteArg(Mode, OutHandle, &Handle, sizeof(Handle)))
        Status = STATUS_ACCESS_VIOLATION;
    return Status;
}

NTSTATUS
NTAPI
NtOpenKeyedEvent(OUT PHANDLE OutHandle, IN ACCESS_MASK AccessMask, IN POBJECT_ATTRIBUTES ObjectAttributes)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    HANDLE Handle;
    NTSTATUS Status = ObOpenObjectByName(ObjectAttributes, QzKeyedEventType, Mode, NULL, AccessMask, NULL, &Handle);
    if (NT_SUCCESS(Status) && !QzWriteArg(Mode, OutHandle, &Handle, sizeof(Handle)))
        Status = STATUS_ACCESS_VIOLATION;
    return Status;
}

/* Meet a thread of the other kind on Key, or wait for one */
static NTSTATUS
QzExKeyedRendezvous(HANDLE Handle, PVOID Key, BOOLEAN Releasing, PLARGE_INTEGER UserTimeout)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PQZ_KEYED_EVENT Event;
    PLIST_ENTRY Entry;
    QZ_KEYED_WAIT Wait;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    if (((ULONG_PTR)Key & 1))
        return STATUS_INVALID_PARAMETER_1;
    if (UserTimeout && !QzReadArg(Mode, &Timeout, UserTimeout, sizeof(Timeout)))
        return STATUS_ACCESS_VIOLATION;
    if (Handle)
    {
        Status = ObReferenceObjectByHandle(Handle, Releasing ? EVENT_MODIFY_STATE : SYNCHRONIZE, QzKeyedEventType,
                                           Mode, (PVOID *)&Event, NULL);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    else
    {
        Event = QzCritSecEvent;
        ObReferenceObject(Event);
    }

    ExAcquireFastMutex(&QzKeyedLock);
    for (Entry = Event->Waiters.Flink; Entry != &Event->Waiters; Entry = Entry->Flink)
    {
        PQZ_KEYED_WAIT Other = CONTAINING_RECORD(Entry, QZ_KEYED_WAIT, Link);
        if (Other->Key == Key && Other->Releasing != Releasing && !Other->Matched)
        {
            Other->Matched = TRUE;
            RemoveEntryList(&Other->Link);
            KeSetEvent(&Other->Event, IO_NO_INCREMENT, FALSE);
            ExReleaseFastMutex(&QzKeyedLock);
            ObDereferenceObject(Event);
            return STATUS_SUCCESS;
        }
    }
    Wait.Key = Key;
    Wait.Releasing = Releasing;
    Wait.Matched = FALSE;
    KeInitializeEvent(&Wait.Event, NotificationEvent, FALSE);
    InsertTailList(&Event->Waiters, &Wait.Link);
    ExReleaseFastMutex(&QzKeyedLock);

    Status = KeWaitForSingleObject(&Wait.Event, WrKeyedEvent, KernelMode, FALSE, UserTimeout ? &Timeout : NULL);
    ExAcquireFastMutex(&QzKeyedLock);
    if (!Wait.Matched)
        RemoveEntryList(&Wait.Link);
    else
        Status = STATUS_SUCCESS;
    ExReleaseFastMutex(&QzKeyedLock);
    ObDereferenceObject(Event);
    return Status;
}

NTSTATUS
NTAPI
NtReleaseKeyedEvent(IN HANDLE EventHandle, IN PVOID Key, IN BOOLEAN Alertable, IN PLARGE_INTEGER Timeout OPTIONAL)
{
    UNREFERENCED_PARAMETER(Alertable);
    return QzExKeyedRendezvous(EventHandle, Key, TRUE, Timeout);
}

NTSTATUS
NTAPI
NtWaitForKeyedEvent(IN HANDLE EventHandle, IN PVOID Key, IN BOOLEAN Alertable, IN PLARGE_INTEGER Timeout OPTIONAL)
{
    UNREFERENCED_PARAMETER(Alertable);
    return QzExKeyedRendezvous(EventHandle, Key, FALSE, Timeout);
}

/* ---------------------------------------------------------------------- */
/* Hard errors: no hard error port (csrss) yet, so they are logged         */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
NtRaiseHardError(IN NTSTATUS ErrorStatus,
                 IN ULONG NumberOfParameters,
                 IN ULONG UnicodeStringParameterMask,
                 IN PULONG_PTR Parameters,
                 IN ULONG ValidResponseOptions,
                 OUT PULONG Response)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    ULONG_PTR Values[MAXIMUM_HARDERROR_PARAMETERS];
    ULONG Answer = ResponseNotHandled, i;
    PQZ_PROCESS Process = QzCurrentThread->Process;

    UNREFERENCED_PARAMETER(ValidResponseOptions);
    if (NumberOfParameters > MAXIMUM_HARDERROR_PARAMETERS)
        return STATUS_INVALID_PARAMETER_2;
    if (NumberOfParameters && !QzReadArg(Mode, Values, Parameters, NumberOfParameters * sizeof(ULONG_PTR)))
        return STATUS_ACCESS_VIOLATION;

    DbgPrint(QZ_KERNEL_NAME ": hard error 0x%08lx from %s (pid %lu)", ErrorStatus,
             Process ? Process->ImageFileName : "?", Process ? Process->UniqueId : 0);
    for (i = 0; i < NumberOfParameters; i++)
    {
        if (UnicodeStringParameterMask & (1 << i))
        {
            UNICODE_STRING Text;
            WCHAR Buffer[128];
            if (QzReadArg(Mode, &Text, (PVOID)Values[i], sizeof(Text)))
            {
                USHORT Length = min(Text.Length, sizeof(Buffer) - sizeof(WCHAR));
                if (QzReadArg(Mode, Buffer, Text.Buffer, Length))
                {
                    Buffer[Length / sizeof(WCHAR)] = 0;
                    DbgPrint(" \"%ls\"", Buffer);
                    continue;
                }
            }
        }
        DbgPrint(" %p", (PVOID)Values[i]);
    }
    DbgPrint("\n");
    if (Response && !QzWriteArg(Mode, Response, &Answer, sizeof(Answer)))
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}
