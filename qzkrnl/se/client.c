/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Security descriptors kept by file systems, client security
 *              contexts (npfs), auditing hooks
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Access is not checked yet (every caller runs as SYSTEM), but file
 * systems still store and return security descriptors: npfs assigns one to
 * each pipe, and clients query it. Descriptors are kept self-relative;
 * setting one is accepted and leaves it as it was. There is a single token,
 * so impersonation changes nothing.
 */

#include "qz.h"

#define QZ_SD_TAG       'dSzQ'

NTSTATUS NTAPI RtlAbsoluteToSelfRelativeSD(PSECURITY_DESCRIPTOR Absolute, PSECURITY_DESCRIPTOR SelfRelative,
                                           PULONG BufferLength);
ULONG NTAPI RtlLengthSecurityDescriptor(PSECURITY_DESCRIPTOR SecurityDescriptor);
NTSTATUS NTAPI RtlCreateSecurityDescriptor(PSECURITY_DESCRIPTOR SecurityDescriptor, ULONG Revision);
NTSTATUS NTAPI RtlSetOwnerSecurityDescriptor(PSECURITY_DESCRIPTOR SecurityDescriptor, PSID Owner, BOOLEAN Defaulted);
NTSTATUS NTAPI RtlSetGroupSecurityDescriptor(PSECURITY_DESCRIPTOR SecurityDescriptor, PSID Group, BOOLEAN Defaulted);
NTSTATUS NTAPI RtlSetDaclSecurityDescriptor(PSECURITY_DESCRIPTOR SecurityDescriptor, BOOLEAN Present, PACL Dacl,
                                            BOOLEAN Defaulted);

/* A self-relative copy of Descriptor (or SYSTEM's with a NULL DACL) in PoolType */
static NTSTATUS
QzSeCopyDescriptor(PSECURITY_DESCRIPTOR Descriptor, POOL_TYPE PoolType, ULONG Extra, PVOID *Block,
                   PSECURITY_DESCRIPTOR *Copy)
{
    SECURITY_DESCRIPTOR Default;
    ULONG Length = 0;
    PUCHAR Buffer;
    NTSTATUS Status;

    if (!Descriptor)
    {
        RtlCreateSecurityDescriptor(&Default, SECURITY_DESCRIPTOR_REVISION);
        RtlSetOwnerSecurityDescriptor(&Default, SeExports->SeLocalSystemSid, FALSE);
        RtlSetGroupSecurityDescriptor(&Default, SeExports->SeLocalSystemSid, FALSE);
        RtlSetDaclSecurityDescriptor(&Default, TRUE, NULL, FALSE);
        Descriptor = &Default;
    }
    if (((PISECURITY_DESCRIPTOR)Descriptor)->Control & SE_SELF_RELATIVE)
    {
        Length = RtlLengthSecurityDescriptor(Descriptor);
        Buffer = ExAllocatePoolWithTag(PoolType, Extra + Length, QZ_SD_TAG);
        if (!Buffer)
            return STATUS_INSUFFICIENT_RESOURCES;
        RtlCopyMemory(Buffer + Extra, Descriptor, Length);
    }
    else
    {
        Status = RtlAbsoluteToSelfRelativeSD(Descriptor, NULL, &Length);
        if (Status != STATUS_BUFFER_TOO_SMALL)
            return NT_SUCCESS(Status) ? STATUS_INVALID_SECURITY_DESCR : Status;
        Buffer = ExAllocatePoolWithTag(PoolType, Extra + Length, QZ_SD_TAG);
        if (!Buffer)
            return STATUS_INSUFFICIENT_RESOURCES;
        Status = RtlAbsoluteToSelfRelativeSD(Descriptor, Buffer + Extra, &Length);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(Buffer, QZ_SD_TAG);
            return Status;
        }
    }
    *Block = Buffer;
    *Copy = Buffer + Extra;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
SeAssignSecurity(IN PSECURITY_DESCRIPTOR ParentDescriptor OPTIONAL, IN PSECURITY_DESCRIPTOR ExplicitDescriptor OPTIONAL,
                 OUT PSECURITY_DESCRIPTOR *NewDescriptor, IN BOOLEAN IsDirectoryObject,
                 IN PSECURITY_SUBJECT_CONTEXT SubjectContext, IN PGENERIC_MAPPING GenericMapping,
                 IN POOL_TYPE PoolType)
{
    PVOID Block;
    UNREFERENCED_PARAMETER(ParentDescriptor);
    UNREFERENCED_PARAMETER(IsDirectoryObject);
    UNREFERENCED_PARAMETER(SubjectContext);
    UNREFERENCED_PARAMETER(GenericMapping);
    return QzSeCopyDescriptor(ExplicitDescriptor, PoolType, 0, &Block, NewDescriptor);
}

NTSTATUS
NTAPI
SeDeassignSecurity(IN OUT PSECURITY_DESCRIPTOR *SecurityDescriptor)
{
    if (*SecurityDescriptor)
        ExFreePoolWithTag(*SecurityDescriptor, QZ_SD_TAG);
    *SecurityDescriptor = NULL;
    return STATUS_SUCCESS;
}

/* The whole stored descriptor, whatever parts were asked for */
NTSTATUS
NTAPI
SeQuerySecurityDescriptorInfo(IN PSECURITY_INFORMATION SecurityInformation, OUT PSECURITY_DESCRIPTOR SecurityDescriptor,
                              IN OUT PULONG Length, IN PSECURITY_DESCRIPTOR *ObjectsSecurityDescriptor)
{
    ULONG Needed;
    UNREFERENCED_PARAMETER(SecurityInformation);
    if (!*ObjectsSecurityDescriptor)
    {
        *Length = 0;
        return STATUS_SUCCESS;
    }
    Needed = RtlLengthSecurityDescriptor(*ObjectsSecurityDescriptor);
    if (*Length < Needed)
    {
        *Length = Needed;
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(SecurityDescriptor, *ObjectsSecurityDescriptor, Needed);
    *Length = Needed;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
SeSetSecurityDescriptorInfo(IN PVOID Object OPTIONAL, IN PSECURITY_INFORMATION SecurityInformation,
                            IN PSECURITY_DESCRIPTOR ModificationDescriptor,
                            IN OUT PSECURITY_DESCRIPTOR *ObjectsSecurityDescriptor, IN POOL_TYPE PoolType,
                            IN PGENERIC_MAPPING GenericMapping)
{
    UNREFERENCED_PARAMETER(Object);
    UNREFERENCED_PARAMETER(SecurityInformation);
    UNREFERENCED_PARAMETER(ModificationDescriptor);
    UNREFERENCED_PARAMETER(ObjectsSecurityDescriptor);
    UNREFERENCED_PARAMETER(PoolType);
    UNREFERENCED_PARAMETER(GenericMapping);
    return STATUS_SUCCESS;
}

/*
 * The object manager's descriptor cache: a counted copy. The count sits in
 * front of the descriptor (8 bytes, keeping its alignment).
 */
NTSTATUS
NTAPI
ObLogSecurityDescriptor(IN PSECURITY_DESCRIPTOR InputSecurityDescriptor,
                        OUT PSECURITY_DESCRIPTOR *OutputSecurityDescriptor, IN ULONG RefBias)
{
    PVOID Block;
    NTSTATUS Status = QzSeCopyDescriptor(InputSecurityDescriptor, PagedPool, 8, &Block, OutputSecurityDescriptor);
    if (NT_SUCCESS(Status))
        *(PLONG)Block = RefBias ? (LONG)RefBias : 1;
    return Status;
}

VOID
NTAPI
ObDereferenceSecurityDescriptor(IN PSECURITY_DESCRIPTOR SecurityDescriptor, IN ULONG Count)
{
    PLONG Block;
    if (!SecurityDescriptor)
        return;
    Block = (PLONG)((PUCHAR)SecurityDescriptor - 8);
    if (InterlockedExchangeAdd(Block, -(LONG)Count) <= (LONG)Count)
        ExFreePoolWithTag(Block, QZ_SD_TAG);
}

VOID
NTAPI
ObReferenceSecurityDescriptor(IN PSECURITY_DESCRIPTOR SecurityDescriptor, IN ULONG Count)
{
    if (SecurityDescriptor)
        InterlockedExchangeAdd((PLONG)((PUCHAR)SecurityDescriptor - 8), (LONG)Count);
}

/* ---- privileges and auditing ---- */

NTSTATUS
NTAPI
SeAppendPrivileges(IN OUT PACCESS_STATE AccessState, IN PPRIVILEGE_SET Privileges)
{
    UNREFERENCED_PARAMETER(AccessState);
    UNREFERENCED_PARAMETER(Privileges);
    return STATUS_SUCCESS;
}

VOID
NTAPI
SeFreePrivileges(IN PPRIVILEGE_SET Privileges)
{
    if (Privileges)
        ExFreePool(Privileges);
}

VOID
NTAPI
SeOpenObjectAuditAlarm(IN PUNICODE_STRING ObjectTypeName, IN PVOID Object OPTIONAL,
                       IN PUNICODE_STRING AbsoluteObjectName OPTIONAL, IN PSECURITY_DESCRIPTOR SecurityDescriptor,
                       IN PACCESS_STATE AccessState, IN BOOLEAN ObjectCreated, IN BOOLEAN AccessGranted,
                       IN KPROCESSOR_MODE AccessMode, OUT PBOOLEAN GenerateOnClose)
{
    UNREFERENCED_PARAMETER(ObjectTypeName);
    UNREFERENCED_PARAMETER(Object);
    UNREFERENCED_PARAMETER(AbsoluteObjectName);
    UNREFERENCED_PARAMETER(SecurityDescriptor);
    UNREFERENCED_PARAMETER(AccessState);
    UNREFERENCED_PARAMETER(ObjectCreated);
    UNREFERENCED_PARAMETER(AccessGranted);
    UNREFERENCED_PARAMETER(AccessMode);
    *GenerateOnClose = FALSE;
}

/* ---- clients (the server end of a pipe or port) ---- */

PACCESS_TOKEN NTAPI PsReferencePrimaryToken(IN PEPROCESS Process);

TOKEN_TYPE
NTAPI
SeTokenType(IN PACCESS_TOKEN Token)
{
    UNREFERENCED_PARAMETER(Token);
    return TokenPrimary;
}

VOID
NTAPI
PsDereferencePrimaryToken(IN PACCESS_TOKEN PrimaryToken)
{
    if (PrimaryToken)
        ObDereferenceObject(PrimaryToken);
}

NTSTATUS
NTAPI
SeCreateClientSecurity(IN PETHREAD ClientThread, IN PSECURITY_QUALITY_OF_SERVICE ClientSecurityQos,
                       IN BOOLEAN RemoteClient, OUT PSECURITY_CLIENT_CONTEXT ClientContext)
{
    RtlZeroMemory(ClientContext, sizeof(*ClientContext));
    ClientContext->SecurityQos = *ClientSecurityQos;
    ClientContext->ServerIsRemote = RemoteClient;
    ClientContext->DirectlyAccessClientToken = FALSE;
    ClientContext->DirectAccessEffectiveOnly = FALSE;
    ClientContext->ClientToken = PsReferencePrimaryToken(PsGetThreadProcess(ClientThread));
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
SeImpersonateClientEx(IN PSECURITY_CLIENT_CONTEXT ClientContext, IN PETHREAD ServerThread OPTIONAL)
{
    UNREFERENCED_PARAMETER(ClientContext);
    UNREFERENCED_PARAMETER(ServerThread);
    return STATUS_SUCCESS;
}

VOID
NTAPI
SeImpersonateClient(IN PSECURITY_CLIENT_CONTEXT ClientContext, IN PETHREAD ServerThread OPTIONAL)
{
    SeImpersonateClientEx(ClientContext, ServerThread);
}

VOID
NTAPI
PsRevertToSelf(VOID)
{
}

PEPROCESS
NTAPI
IoThreadToProcess(IN PETHREAD Thread)
{
    return PsGetThreadProcess(Thread);
}

/* One token: impersonating another thread changes nothing */
NTSTATUS
NTAPI
NtImpersonateThread(IN HANDLE ThreadHandle, IN HANDLE ThreadToImpersonateHandle,
                    IN PSECURITY_QUALITY_OF_SERVICE SecurityQualityOfService)
{
    PVOID Server, Client;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(SecurityQualityOfService);
    Status = ObReferenceObjectByHandle(ThreadHandle, THREAD_IMPERSONATE, PsThreadType, ExGetPreviousMode(), &Server,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = ObReferenceObjectByHandle(ThreadToImpersonateHandle, THREAD_DIRECT_IMPERSONATION, PsThreadType,
                                       ExGetPreviousMode(), &Client, NULL);
    if (NT_SUCCESS(Status))
        ObDereferenceObject(Client);
    ObDereferenceObject(Server);
    return Status;
}
