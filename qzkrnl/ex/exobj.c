/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Executive objects (Event, Semaphore, Timer) and the
 *              handle-based wait services
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Their bodies are the kernel dispatcher objects from stage B1, so a
 * handle-based wait is a KeWaitFor*Object(s) on the body. Timer APC
 * routines need APC delivery (stage B6) and are refused for now.
 */

#include "qz.h"

#define QZ_WAIT_TAG 'tiWQ'

POBJECT_TYPE ExEventObjectType;
POBJECT_TYPE ExSemaphoreObjectType;
POBJECT_TYPE ExTimerObjectType;

static GENERIC_MAPPING QzEventMapping =
{
    STANDARD_RIGHTS_READ | EVENT_QUERY_STATE,
    STANDARD_RIGHTS_WRITE | EVENT_MODIFY_STATE,
    STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
    EVENT_ALL_ACCESS
};

static GENERIC_MAPPING QzSemaphoreMapping =
{
    STANDARD_RIGHTS_READ | SEMAPHORE_QUERY_STATE,
    STANDARD_RIGHTS_WRITE | SEMAPHORE_MODIFY_STATE,
    STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
    SEMAPHORE_ALL_ACCESS
};

static GENERIC_MAPPING QzTimerMapping =
{
    STANDARD_RIGHTS_READ | TIMER_QUERY_STATE,
    STANDARD_RIGHTS_WRITE | TIMER_MODIFY_STATE,
    STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
    TIMER_ALL_ACCESS
};

static VOID NTAPI
QzExDeleteTimer(PVOID Object)
{
    KeCancelTimer(Object);
}

static VOID
QzExCreateType(PCWSTR Name, ULONG ValidAccess, PGENERIC_MAPPING Mapping,
               OB_DELETE_METHOD DeleteProcedure, POBJECT_TYPE *Type)
{
    OBJECT_TYPE_INITIALIZER Init;
    UNICODE_STRING TypeName;
    NTSTATUS Status;

    RtlZeroMemory(&Init, sizeof(Init));
    Init.Length = sizeof(Init);
    Init.ValidAccessMask = ValidAccess;
    Init.GenericMapping = *Mapping;
    Init.PoolType = NonPagedPool;
    Init.CaseInsensitive = TRUE;
    Init.InvalidAttributes = OBJ_OPENLINK;
    Init.DeleteProcedure = DeleteProcedure;
    RtlInitUnicodeString(&TypeName, Name);
    Status = ObCreateObjectType(&TypeName, &Init, NULL, Type);
    if (!NT_SUCCESS(Status))
        QzBugCheckWithText(OBJECT_INITIALIZATION_FAILED, 3, Status, 0, 0,
                           "cannot create an executive object type");
}

VOID
QzExObjectsInit(VOID)
{
    QzExCreateType(L"Event", EVENT_ALL_ACCESS, &QzEventMapping, NULL, &ExEventObjectType);
    QzExCreateType(L"Semaphore", SEMAPHORE_ALL_ACCESS, &QzSemaphoreMapping, NULL,
                   &ExSemaphoreObjectType);
    QzExCreateType(L"Timer", TIMER_ALL_ACCESS, &QzTimerMapping, QzExDeleteTimer,
                   &ExTimerObjectType);
}

/* Look up a handle and modify its object under the given type and right */
static NTSTATUS
QzExReference(HANDLE Handle, POBJECT_TYPE Type, ACCESS_MASK Access, PVOID *Object)
{
    return ObReferenceObjectByHandle(Handle, Access, Type, KernelMode, Object, NULL);
}

/* ---------------------------------------------------------------------- */
/* Events                                                                  */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
NtCreateEvent(OUT PHANDLE EventHandle,
              IN ACCESS_MASK DesiredAccess,
              IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL,
              IN EVENT_TYPE EventType,
              IN BOOLEAN InitialState)
{
    PKEVENT Event;
    NTSTATUS Status;

    *EventHandle = NULL;
    if (EventType != NotificationEvent && EventType != SynchronizationEvent)
        return STATUS_INVALID_PARAMETER;

    Status = ObCreateObject(KernelMode, ExEventObjectType, ObjectAttributes, KernelMode, NULL,
                            sizeof(KEVENT), 0, 0, (PVOID *)&Event);
    if (!NT_SUCCESS(Status))
        return Status;
    KeInitializeEvent(Event, EventType, InitialState);
    return ObInsertObject(Event, NULL, DesiredAccess, 0, NULL, EventHandle);
}

NTSTATUS
NTAPI
ZwCreateEvent(OUT PHANDLE EventHandle,
              IN ACCESS_MASK DesiredAccess,
              IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL,
              IN EVENT_TYPE EventType,
              IN BOOLEAN InitialState)
{
    return NtCreateEvent(EventHandle, DesiredAccess, ObjectAttributes, EventType, InitialState);
}

NTSTATUS
NTAPI
ZwOpenEvent(OUT PHANDLE EventHandle,
            IN ACCESS_MASK DesiredAccess,
            IN POBJECT_ATTRIBUTES ObjectAttributes)
{
    return ObOpenObjectByName(ObjectAttributes, ExEventObjectType, KernelMode, NULL,
                              DesiredAccess, NULL, EventHandle);
}

typedef enum _QZ_EVENT_OP { QzEventSet, QzEventReset, QzEventClear, QzEventPulse } QZ_EVENT_OP;

static NTSTATUS
QzExEventOp(HANDLE Handle, QZ_EVENT_OP Op, PLONG PreviousState)
{
    PKEVENT Event;
    LONG Previous = 0;
    NTSTATUS Status = QzExReference(Handle, ExEventObjectType, EVENT_MODIFY_STATE, (PVOID *)&Event);

    if (!NT_SUCCESS(Status))
        return Status;
    switch (Op)
    {
        case QzEventSet:   Previous = KeSetEvent(Event, EVENT_INCREMENT, FALSE); break;
        case QzEventReset: Previous = KeResetEvent(Event); break;
        case QzEventClear: KeClearEvent(Event); break;
        case QzEventPulse: Previous = KePulseEvent(Event, EVENT_INCREMENT, FALSE); break;
    }
    ObfDereferenceObject(Event);
    if (PreviousState)
        *PreviousState = Previous;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtSetEvent(IN HANDLE EventHandle, OUT PLONG PreviousState OPTIONAL)
{
    return QzExEventOp(EventHandle, QzEventSet, PreviousState);
}

NTSTATUS NTAPI ZwSetEvent(IN HANDLE EventHandle, OUT PLONG PreviousState OPTIONAL)
{
    return QzExEventOp(EventHandle, QzEventSet, PreviousState);
}

NTSTATUS NTAPI ZwResetEvent(IN HANDLE EventHandle, OUT PLONG PreviousState OPTIONAL)
{
    return QzExEventOp(EventHandle, QzEventReset, PreviousState);
}

NTSTATUS NTAPI ZwClearEvent(IN HANDLE EventHandle)
{
    return QzExEventOp(EventHandle, QzEventClear, NULL);
}

NTSTATUS NTAPI ZwPulseEvent(IN HANDLE EventHandle, OUT PLONG PreviousState OPTIONAL)
{
    return QzExEventOp(EventHandle, QzEventPulse, PreviousState);
}

/* ---------------------------------------------------------------------- */
/* Semaphores (not exported by ntoskrnl as Zw*; used by the tests)         */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
NtCreateSemaphore(OUT PHANDLE SemaphoreHandle,
                  IN ACCESS_MASK DesiredAccess,
                  IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL,
                  IN LONG InitialCount,
                  IN LONG MaximumCount)
{
    PKSEMAPHORE Semaphore;
    NTSTATUS Status;

    *SemaphoreHandle = NULL;
    if (MaximumCount <= 0 || InitialCount < 0 || InitialCount > MaximumCount)
        return STATUS_INVALID_PARAMETER;

    Status = ObCreateObject(KernelMode, ExSemaphoreObjectType, ObjectAttributes, KernelMode, NULL,
                            sizeof(KSEMAPHORE), 0, 0, (PVOID *)&Semaphore);
    if (!NT_SUCCESS(Status))
        return Status;
    KeInitializeSemaphore(Semaphore, InitialCount, MaximumCount);
    return ObInsertObject(Semaphore, NULL, DesiredAccess, 0, NULL, SemaphoreHandle);
}

NTSTATUS
NTAPI
NtReleaseSemaphore(IN HANDLE SemaphoreHandle,
                   IN LONG ReleaseCount,
                   OUT PLONG PreviousCount OPTIONAL)
{
    PKSEMAPHORE Semaphore;
    LONG Previous;
    NTSTATUS Status;

    if (ReleaseCount <= 0)
        return STATUS_INVALID_PARAMETER;
    Status = QzExReference(SemaphoreHandle, ExSemaphoreObjectType, SEMAPHORE_MODIFY_STATE,
                           (PVOID *)&Semaphore);
    if (!NT_SUCCESS(Status))
        return Status;

    /* KeReleaseSemaphore raises on overflow; check first */
    if (ReleaseCount > Semaphore->Limit - Semaphore->Header.SignalState)
    {
        ObfDereferenceObject(Semaphore);
        return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    }
    Previous = KeReleaseSemaphore(Semaphore, IO_NO_INCREMENT, ReleaseCount, FALSE);
    ObfDereferenceObject(Semaphore);
    if (PreviousCount)
        *PreviousCount = Previous;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Timers                                                                  */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
ZwCreateTimer(OUT PHANDLE TimerHandle,
              IN ACCESS_MASK DesiredAccess,
              IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL,
              IN TIMER_TYPE TimerType)
{
    PKTIMER Timer;
    NTSTATUS Status;

    *TimerHandle = NULL;
    if (TimerType != NotificationTimer && TimerType != SynchronizationTimer)
        return STATUS_INVALID_PARAMETER;

    Status = ObCreateObject(KernelMode, ExTimerObjectType, ObjectAttributes, KernelMode, NULL,
                            sizeof(KTIMER), 0, 0, (PVOID *)&Timer);
    if (!NT_SUCCESS(Status))
        return Status;
    KeInitializeTimerEx(Timer, TimerType);
    return ObInsertObject(Timer, NULL, DesiredAccess, 0, NULL, TimerHandle);
}

NTSTATUS
NTAPI
ZwOpenTimer(OUT PHANDLE TimerHandle,
            IN ACCESS_MASK DesiredAccess,
            IN POBJECT_ATTRIBUTES ObjectAttributes)
{
    return ObOpenObjectByName(ObjectAttributes, ExTimerObjectType, KernelMode, NULL,
                              DesiredAccess, NULL, TimerHandle);
}

NTSTATUS
NTAPI
ZwSetTimer(IN HANDLE TimerHandle,
           IN PLARGE_INTEGER DueTime,
           IN PTIMER_APC_ROUTINE TimerApcRoutine OPTIONAL,
           IN PVOID TimerContext OPTIONAL,
           IN BOOLEAN WakeTimer,
           IN LONG Period OPTIONAL,
           OUT PBOOLEAN PreviousState OPTIONAL)
{
    PKTIMER Timer;
    BOOLEAN Previous;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(TimerContext);
    UNREFERENCED_PARAMETER(WakeTimer);

    if (TimerApcRoutine)
        return STATUS_NOT_IMPLEMENTED;   /* needs APCs (B6) */
    if (Period < 0)
        return STATUS_INVALID_PARAMETER_6;

    Status = QzExReference(TimerHandle, ExTimerObjectType, TIMER_MODIFY_STATE, (PVOID *)&Timer);
    if (!NT_SUCCESS(Status))
        return Status;
    Previous = Timer->Header.SignalState ? TRUE : FALSE;
    KeSetTimerEx(Timer, *DueTime, Period, NULL);
    ObfDereferenceObject(Timer);
    if (PreviousState)
        *PreviousState = Previous;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
ZwCancelTimer(IN HANDLE TimerHandle,
              OUT PBOOLEAN CurrentState OPTIONAL)
{
    PKTIMER Timer;
    NTSTATUS Status;

    Status = QzExReference(TimerHandle, ExTimerObjectType, TIMER_MODIFY_STATE, (PVOID *)&Timer);
    if (!NT_SUCCESS(Status))
        return Status;
    KeCancelTimer(Timer);
    if (CurrentState)
        *CurrentState = Timer->Header.SignalState ? TRUE : FALSE;
    ObfDereferenceObject(Timer);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Waits                                                                   */
/* ---------------------------------------------------------------------- */

/* The dispatcher object a wait on Object really waits for */
static PVOID
QzExWaitableObject(PVOID Object)
{
    POBJECT_TYPE Type = OBJECT_TO_OBJECT_HEADER(Object)->Type;
    return Type->DefaultObject ? Type->DefaultObject : Object;
}

NTSTATUS
NTAPI
NtWaitForSingleObject(IN HANDLE Handle,
                      IN BOOLEAN Alertable,
                      IN PLARGE_INTEGER Timeout OPTIONAL)
{
    PVOID Object;
    NTSTATUS Status;

    Status = ObReferenceObjectByHandle(Handle, SYNCHRONIZE, NULL, KernelMode, &Object, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = KeWaitForSingleObject(QzExWaitableObject(Object), UserRequest, KernelMode,
                                   Alertable, Timeout);
    ObfDereferenceObject(Object);
    return Status;
}

NTSTATUS
NTAPI
ZwWaitForSingleObject(IN HANDLE Handle,
                      IN BOOLEAN Alertable,
                      IN PLARGE_INTEGER Timeout OPTIONAL)
{
    return NtWaitForSingleObject(Handle, Alertable, Timeout);
}

NTSTATUS
NTAPI
ZwWaitForMultipleObjects(IN ULONG Count,
                         IN HANDLE Handles[],
                         IN WAIT_TYPE WaitType,
                         IN BOOLEAN Alertable,
                         IN PLARGE_INTEGER Timeout OPTIONAL)
{
    PVOID Objects[MAXIMUM_WAIT_OBJECTS], Waitables[MAXIMUM_WAIT_OBJECTS];
    PKWAIT_BLOCK WaitBlocks = NULL;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG i, Referenced = 0;

    if (Count == 0 || Count > MAXIMUM_WAIT_OBJECTS)
        return STATUS_INVALID_PARAMETER_1;
    if (WaitType != WaitAll && WaitType != WaitAny)
        return STATUS_INVALID_PARAMETER_3;

    for (i = 0; i < Count; i++)
    {
        Status = ObReferenceObjectByHandle(Handles[i], SYNCHRONIZE, NULL, KernelMode,
                                           &Objects[i], NULL);
        if (!NT_SUCCESS(Status))
            goto Done;
        Referenced++;
        Waitables[i] = QzExWaitableObject(Objects[i]);
    }

    if (Count > THREAD_WAIT_OBJECTS)
    {
        WaitBlocks = ExAllocatePoolWithTag(NonPagedPool, Count * sizeof(KWAIT_BLOCK), QZ_WAIT_TAG);
        if (!WaitBlocks)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Done;
        }
    }

    Status = KeWaitForMultipleObjects(Count, Waitables, WaitType, UserRequest, KernelMode,
                                      Alertable, Timeout, WaitBlocks);

Done:
    if (WaitBlocks)
        ExFreePoolWithTag(WaitBlocks, QZ_WAIT_TAG);
    for (i = 0; i < Referenced; i++)
        ObfDereferenceObject(Objects[i]);
    return Status;
}
