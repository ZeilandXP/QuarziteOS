/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     I/O completion ports
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * An IoCompletion object is a kernel queue of completion packets (key,
 * APC context, status, information). Packets come from
 * NtSetIoCompletion/IoSetIoCompletion and from the I/O manager, when an
 * asynchronous request on a file bound to the port (FileCompletionInformation)
 * completes; NtRemoveIoCompletion takes them out, waiting if there are none.
 * The RTL thread pool and kernel32's completion port API sit on these.
 */

#include "qzio.h"
#include "qzps.h"

#define QZ_IOCOMP_TAG   'cIzQ'

typedef struct _QZ_IO_PACKET
{
    LIST_ENTRY Link;                    /* in the KQUEUE */
    PVOID KeyContext;
    PVOID ApcContext;
    IO_STATUS_BLOCK IoStatus;
} QZ_IO_PACKET, *PQZ_IO_PACKET;

POBJECT_TYPE IoCompletionObjectType;

static GENERIC_MAPPING QzIoCompletionMapping =
{
    STANDARD_RIGHTS_READ | IO_COMPLETION_QUERY_STATE,
    STANDARD_RIGHTS_WRITE | IO_COMPLETION_MODIFY_STATE,
    STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
    IO_COMPLETION_ALL_ACCESS
};

/* The port goes away: drop the packets nobody took */
static VOID NTAPI
QzIoDeleteCompletion(PVOID Object)
{
    PLIST_ENTRY First = KeRundownQueue(Object), Entry, Next;

    if (!First)
        return;
    Entry = First;
    do
    {
        Next = Entry->Flink;
        ExFreePoolWithTag(CONTAINING_RECORD(Entry, QZ_IO_PACKET, Link), QZ_IOCOMP_TAG);
        Entry = Next;
    } while (Entry != First);
}

static VOID
QzIoCompletionInit(VOID)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"IoCompletion");
    OBJECT_TYPE_INITIALIZER Init;

    if (IoCompletionObjectType)
        return;
    RtlZeroMemory(&Init, sizeof(Init));
    Init.Length = sizeof(Init);
    Init.GenericMapping = QzIoCompletionMapping;
    Init.ValidAccessMask = IO_COMPLETION_ALL_ACCESS;
    Init.PoolType = NonPagedPool;
    Init.InvalidAttributes = OBJ_PERMANENT | OBJ_OPENLINK;
    Init.DeleteProcedure = QzIoDeleteCompletion;
    ObCreateObjectType(&Name, &Init, NULL, &IoCompletionObjectType);
}

NTSTATUS
QzIoQueueCompletion(PVOID Port, PVOID KeyContext, PVOID ApcContext, NTSTATUS Status, ULONG_PTR Information)
{
    PQZ_IO_PACKET Packet = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Packet), QZ_IOCOMP_TAG);

    if (!Packet)
        return STATUS_INSUFFICIENT_RESOURCES;
    Packet->KeyContext = KeyContext;
    Packet->ApcContext = ApcContext;
    Packet->IoStatus.Status = Status;
    Packet->IoStatus.Information = Information;
    KeInsertQueue(Port, &Packet->Link);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
IoSetIoCompletion(IN PVOID IoCompletion, IN PVOID KeyContext, IN PVOID ApcContext, IN NTSTATUS IoStatus,
                  IN ULONG_PTR IoStatusInformation, IN BOOLEAN Quota)
{
    UNREFERENCED_PARAMETER(Quota);
    return QzIoQueueCompletion(IoCompletion, KeyContext, ApcContext, IoStatus, IoStatusInformation);
}

NTSTATUS
NTAPI
NtCreateIoCompletion(OUT PHANDLE IoCompletionHandle, IN ACCESS_MASK DesiredAccess,
                     IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL, IN ULONG NumberOfConcurrentThreads)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PKQUEUE Queue;
    HANDLE Handle;
    NTSTATUS Status;

    QzIoCompletionInit();
    Status = ObCreateObject(Mode, IoCompletionObjectType, ObjectAttributes, Mode, NULL, sizeof(KQUEUE), 0, 0,
                            (PVOID *)&Queue);
    if (!NT_SUCCESS(Status))
        return Status;
    KeInitializeQueue(Queue, NumberOfConcurrentThreads);
    Status = ObInsertObject(Queue, NULL, DesiredAccess, 0, NULL, &Handle);
    if (NT_SUCCESS(Status) && !QzWriteArg(Mode, IoCompletionHandle, &Handle, sizeof(Handle)))
    {
        ObCloseHandle(Handle, Mode);
        Status = STATUS_ACCESS_VIOLATION;
    }
    return Status;
}

NTSTATUS
NTAPI
NtOpenIoCompletion(OUT PHANDLE IoCompletionHandle, IN ACCESS_MASK DesiredAccess,
                   IN POBJECT_ATTRIBUTES ObjectAttributes)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    HANDLE Handle;
    NTSTATUS Status;

    QzIoCompletionInit();
    Status = ObOpenObjectByName(ObjectAttributes, IoCompletionObjectType, Mode, NULL, DesiredAccess, NULL,
                                &Handle);
    if (NT_SUCCESS(Status) && !QzWriteArg(Mode, IoCompletionHandle, &Handle, sizeof(Handle)))
    {
        ObCloseHandle(Handle, Mode);
        Status = STATUS_ACCESS_VIOLATION;
    }
    return Status;
}

NTSTATUS
NTAPI
NtSetIoCompletion(IN HANDLE IoCompletionHandle, IN PVOID KeyContext, IN PVOID ApcContext,
                  IN NTSTATUS CompletionStatus, IN ULONG_PTR CompletionInformation)
{
    PVOID Port;
    NTSTATUS Status = ObReferenceObjectByHandle(IoCompletionHandle, IO_COMPLETION_MODIFY_STATE,
                                                IoCompletionObjectType, ExGetPreviousMode(), &Port, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = QzIoQueueCompletion(Port, KeyContext, ApcContext, CompletionStatus, CompletionInformation);
    ObDereferenceObject(Port);
    return Status;
}

NTSTATUS
NTAPI
NtRemoveIoCompletion(IN HANDLE IoCompletionHandle, OUT PVOID *KeyContext, OUT PVOID *ApcContext,
                     OUT PIO_STATUS_BLOCK IoStatusBlock, IN PLARGE_INTEGER Timeout OPTIONAL)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    LARGE_INTEGER LocalTimeout;
    PQZ_IO_PACKET Packet;
    PLIST_ENTRY Entry;
    PVOID Port;
    NTSTATUS Status;

    if (Timeout && !QzReadArg(Mode, &LocalTimeout, Timeout, sizeof(LocalTimeout)))
        return STATUS_ACCESS_VIOLATION;
    Status = ObReferenceObjectByHandle(IoCompletionHandle, IO_COMPLETION_MODIFY_STATE, IoCompletionObjectType,
                                       Mode, &Port, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    Entry = KeRemoveQueue(Port, Mode, Timeout ? &LocalTimeout : NULL);
    ObDereferenceObject(Port);
    if ((ULONG_PTR)Entry == (ULONG_PTR)STATUS_TIMEOUT || (ULONG_PTR)Entry == (ULONG_PTR)STATUS_USER_APC ||
        (ULONG_PTR)Entry == (ULONG_PTR)STATUS_ALERTED)
        return (NTSTATUS)(ULONG_PTR)Entry;

    Packet = CONTAINING_RECORD(Entry, QZ_IO_PACKET, Link);
    if (!QzWriteArg(Mode, KeyContext, &Packet->KeyContext, sizeof(PVOID)) ||
        !QzWriteArg(Mode, ApcContext, &Packet->ApcContext, sizeof(PVOID)) ||
        !QzWriteArg(Mode, IoStatusBlock, &Packet->IoStatus, sizeof(IO_STATUS_BLOCK)))
        Status = STATUS_ACCESS_VIOLATION;
    else
        Status = STATUS_SUCCESS;
    ExFreePoolWithTag(Packet, QZ_IOCOMP_TAG);
    return Status;
}

NTSTATUS
NTAPI
NtQueryIoCompletion(IN HANDLE IoCompletionHandle, IN IO_COMPLETION_INFORMATION_CLASS InformationClass,
                    OUT PVOID IoCompletionInformation, IN ULONG BufferLength, OUT PULONG ResultLength OPTIONAL)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    IO_COMPLETION_BASIC_INFORMATION Info;
    ULONG Length = sizeof(Info);
    PVOID Port;
    NTSTATUS Status;

    if (InformationClass != IoCompletionBasicInformation)
        return STATUS_INVALID_INFO_CLASS;
    if (BufferLength < sizeof(Info))
        return STATUS_INFO_LENGTH_MISMATCH;
    Status = ObReferenceObjectByHandle(IoCompletionHandle, IO_COMPLETION_QUERY_STATE, IoCompletionObjectType,
                                       Mode, &Port, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    Info.Depth = KeReadStateQueue(Port);
    ObDereferenceObject(Port);
    if (!QzWriteArg(Mode, IoCompletionInformation, &Info, sizeof(Info)) ||
        (ResultLength && !QzWriteArg(Mode, ResultLength, &Length, sizeof(Length))))
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/*
 * NtSetInformationFile(FileCompletionInformation): bind the file to a port.
 * The I/O manager handles it (no IRP); the context goes with the file.
 */
NTSTATUS
QzIoSetFileCompletion(PFILE_OBJECT FileObject, PFILE_COMPLETION_INFORMATION Information)
{
    PIO_COMPLETION_CONTEXT Context;
    PVOID Port;
    NTSTATUS Status;

    if (FileObject->CompletionContext)
        return STATUS_INVALID_PARAMETER;
    if (FileObject->Flags & FO_SYNCHRONOUS_IO)
        return STATUS_INVALID_PARAMETER;
    QzIoCompletionInit();
    Status = ObReferenceObjectByHandle(Information->Port, IO_COMPLETION_MODIFY_STATE, IoCompletionObjectType,
                                       ExGetPreviousMode(), &Port, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    Context = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Context), QZ_IOCOMP_TAG);
    if (!Context)
    {
        ObDereferenceObject(Port);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Context->Port = Port;               /* the reference goes with the file */
    Context->Key = Information->Key;
    if (InterlockedCompareExchangePointer((PVOID *)&FileObject->CompletionContext, Context, NULL))
    {
        ExFreePoolWithTag(Context, QZ_IOCOMP_TAG);
        ObDereferenceObject(Port);
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

VOID
QzIoFreeFileCompletion(PFILE_OBJECT FileObject)
{
    PIO_COMPLETION_CONTEXT Context = FileObject->CompletionContext;
    if (!Context)
        return;
    FileObject->CompletionContext = NULL;
    ObDereferenceObject(Context->Port);
    ExFreePoolWithTag(Context, QZ_IOCOMP_TAG);
}
