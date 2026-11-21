/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Access tokens: one system token, until security is real
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Every process runs as LocalSystem with every privilege. There is a
 * single "Token" object; opening a process's token opens it, adjusting
 * privileges succeeds, and queries describe SYSTEM in the Administrators
 * and Everyone groups. It is what smss and csrss ask about while they
 * start; real tokens (and access checks against them) come later.
 */

#include "qzio.h"
#include "qzps.h"

#define QZ_TOKEN_TAG 'koTQ'

POBJECT_TYPE SeTokenObjectType;
static PVOID QzSystemToken;
static LUID QzSystemLuid = { 0x3E7, 0 };      /* SYSTEM_LUID */

typedef struct _QZ_TOKEN
{
    TOKEN_SOURCE Source;
    LUID TokenId;
} QZ_TOKEN;

VOID
QzSeInitTokens(VOID)
{
    static GENERIC_MAPPING Mapping = { TOKEN_READ, TOKEN_WRITE, TOKEN_EXECUTE, TOKEN_ALL_ACCESS };
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"Token");
    OBJECT_TYPE_INITIALIZER Init;
    QZ_TOKEN *Token;

    RtlZeroMemory(&Init, sizeof(Init));
    Init.Length = sizeof(Init);
    Init.GenericMapping = Mapping;
    Init.ValidAccessMask = TOKEN_ALL_ACCESS;
    Init.PoolType = NonPagedPool;
    Init.UseDefaultObject = TRUE;
    if (!NT_SUCCESS(ObCreateObjectType(&Name, &Init, NULL, &SeTokenObjectType)) ||
        !NT_SUCCESS(ObCreateObject(KernelMode, SeTokenObjectType, NULL, KernelMode, NULL, sizeof(QZ_TOKEN), 0, 0,
                                   (PVOID *)&Token)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, 6, 0, 0, 0);
    RtlZeroMemory(Token, sizeof(*Token));
    RtlCopyMemory(Token->Source.SourceName, "*SYSTEM*", 8);
    Token->TokenId.LowPart = 0x3E8;
    QzSystemToken = Token;
}

static NTSTATUS
QzSeOpenToken(HANDLE ProcessHandle, ACCESS_MASK DesiredAccess, ULONG HandleAttributes, PHANDLE TokenHandle)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PVOID Process;
    HANDLE Handle;
    NTSTATUS Status;

    Status = ObReferenceObjectByHandle(ProcessHandle, PROCESS_QUERY_INFORMATION, PsProcessType, Mode, &Process,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    ObDereferenceObject(Process);
    if (Mode != KernelMode)
        HandleAttributes &= ~OBJ_KERNEL_HANDLE;
    Status = ObOpenObjectByPointer(QzSystemToken, HandleAttributes, NULL, DesiredAccess, SeTokenObjectType, Mode,
                                   &Handle);
    if (NT_SUCCESS(Status) && !QzWriteArg(Mode, TokenHandle, &Handle, sizeof(Handle)))
        Status = STATUS_ACCESS_VIOLATION;
    return Status;
}

NTSTATUS
NTAPI
NtOpenProcessToken(IN HANDLE ProcessHandle, IN ACCESS_MASK DesiredAccess, OUT PHANDLE TokenHandle)
{
    return QzSeOpenToken(ProcessHandle, DesiredAccess, 0, TokenHandle);
}

NTSTATUS
NTAPI
NtOpenProcessTokenEx(IN HANDLE ProcessHandle, IN ACCESS_MASK DesiredAccess, IN ULONG HandleAttributes,
                     OUT PHANDLE TokenHandle)
{
    return QzSeOpenToken(ProcessHandle, DesiredAccess, HandleAttributes, TokenHandle);
}

NTSTATUS
NTAPI
ZwOpenProcessTokenEx(IN HANDLE ProcessHandle, IN ACCESS_MASK DesiredAccess, IN ULONG HandleAttributes,
                     OUT PHANDLE TokenHandle)
{
    return QZ_AS_KERNEL(QzSeOpenToken(ProcessHandle, DesiredAccess, HandleAttributes, TokenHandle));
}

static NTSTATUS
QzSeReferenceToken(HANDLE TokenHandle, ACCESS_MASK Access)
{
    PVOID Token;
    NTSTATUS Status = ObReferenceObjectByHandle(TokenHandle, Access, SeTokenObjectType, ExGetPreviousMode(),
                                                &Token, NULL);
    if (NT_SUCCESS(Status))
        ObDereferenceObject(Token);
    return Status;
}

NTSTATUS
NTAPI
NtAdjustPrivilegesToken(IN HANDLE TokenHandle,
                        IN BOOLEAN DisableAllPrivileges,
                        IN PTOKEN_PRIVILEGES NewState,
                        IN ULONG BufferLength,
                        OUT PTOKEN_PRIVILEGES PreviousState OPTIONAL,
                        OUT PULONG ReturnLength OPTIONAL)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    ULONG Count = 0, Needed;
    NTSTATUS Status = QzSeReferenceToken(TokenHandle, TOKEN_ADJUST_PRIVILEGES);

    UNREFERENCED_PARAMETER(DisableAllPrivileges);
    if (!NT_SUCCESS(Status))
        return Status;
    if (NewState && !QzReadArg(Mode, &Count, &NewState->PrivilegeCount, sizeof(Count)))
        return STATUS_ACCESS_VIOLATION;

    /* The privileges were all enabled: report them as such */
    Needed = FIELD_OFFSET(TOKEN_PRIVILEGES, Privileges) + Count * sizeof(LUID_AND_ATTRIBUTES);
    if (ReturnLength)
        QzWriteArg(Mode, ReturnLength, &Needed, sizeof(Needed));
    if (PreviousState)
    {
        ULONG i;
        if (BufferLength < Needed)
            return STATUS_BUFFER_TOO_SMALL;
        QzWriteArg(Mode, &PreviousState->PrivilegeCount, &Count, sizeof(Count));
        for (i = 0; i < Count; i++)
        {
            LUID_AND_ATTRIBUTES Entry;
            if (!QzReadArg(Mode, &Entry, &NewState->Privileges[i], sizeof(Entry)))
                return STATUS_ACCESS_VIOLATION;
            Entry.Attributes = SE_PRIVILEGE_ENABLED;
            QzWriteArg(Mode, &PreviousState->Privileges[i], &Entry, sizeof(Entry));
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NtAdjustGroupsToken(IN HANDLE TokenHandle, IN BOOLEAN ResetToDefault, IN PTOKEN_GROUPS NewState,
                    IN ULONG BufferLength, OUT PTOKEN_GROUPS PreviousState, OUT PULONG ReturnLength)
{
    UNREFERENCED_PARAMETER(ResetToDefault);
    UNREFERENCED_PARAMETER(NewState);
    UNREFERENCED_PARAMETER(BufferLength);
    UNREFERENCED_PARAMETER(PreviousState);
    UNREFERENCED_PARAMETER(ReturnLength);
    return QzSeReferenceToken(TokenHandle, TOKEN_ADJUST_GROUPS);
}

/* Lay a SID at Out (a kernel buffer that becomes the caller's at UserBase) */
static PSID
QzSePlaceSid(PUCHAR *Cursor, PUCHAR Base, PUCHAR UserBase, PSID Sid)
{
    ULONG Length = RtlLengthSid(Sid);
    PUCHAR At = *Cursor;
    RtlCopyMemory(At, Sid, Length);
    *Cursor += (Length + 3) & ~3UL;
    return (PSID)(UserBase + (At - Base));
}

NTSTATUS
NTAPI
NtQueryInformationToken(IN HANDLE TokenHandle,
                        IN TOKEN_INFORMATION_CLASS TokenInformationClass,
                        OUT PVOID TokenInformation,
                        IN ULONG TokenInformationLength,
                        OUT PULONG ReturnLength)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PUCHAR Buffer, Cursor, User = TokenInformation;
    ULONG Needed = 0;
    NTSTATUS Status = QzSeReferenceToken(TokenHandle, TOKEN_QUERY);

    if (!NT_SUCCESS(Status))
        return Status;
    Buffer = ExAllocatePoolWithTag(NonPagedPool, 2048, QZ_TOKEN_TAG);
    if (!Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Buffer, 2048);

    switch (TokenInformationClass)
    {
        case TokenUser:
        {
            PTOKEN_USER Info = (PVOID)Buffer;
            Cursor = Buffer + sizeof(*Info);
            Info->User.Sid = QzSePlaceSid(&Cursor, Buffer, User, SeExports->SeLocalSystemSid);
            Needed = (ULONG)(Cursor - Buffer);
            break;
        }
        case TokenOwner:
        case TokenPrimaryGroup:
        {
            PTOKEN_OWNER Info = (PVOID)Buffer;
            Cursor = Buffer + sizeof(*Info);
            Info->Owner = QzSePlaceSid(&Cursor, Buffer, User, TokenInformationClass == TokenOwner
                                                                  ? SeExports->SeAliasAdminsSid
                                                                  : SeExports->SeLocalSystemSid);
            Needed = (ULONG)(Cursor - Buffer);
            break;
        }
        case TokenGroups:
        {
            PTOKEN_GROUPS Info = (PVOID)Buffer;
            Info->GroupCount = 3;
            Cursor = Buffer + FIELD_OFFSET(TOKEN_GROUPS, Groups) + 3 * sizeof(SID_AND_ATTRIBUTES);
            Info->Groups[0].Sid = QzSePlaceSid(&Cursor, Buffer, User, SeExports->SeAliasAdminsSid);
            Info->Groups[0].Attributes = SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_OWNER |
                                         SE_GROUP_MANDATORY;
            Info->Groups[1].Sid = QzSePlaceSid(&Cursor, Buffer, User, SeExports->SeWorldSid);
            Info->Groups[1].Attributes = SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_MANDATORY;
            Info->Groups[2].Sid = QzSePlaceSid(&Cursor, Buffer, User, SeExports->SeAuthenticatedUsersSid);
            Info->Groups[2].Attributes = SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_MANDATORY;
            Needed = (ULONG)(Cursor - Buffer);
            break;
        }
        case TokenPrivileges:
        {
            PTOKEN_PRIVILEGES Info = (PVOID)Buffer;
            ULONG i, Count = SE_MAX_WELL_KNOWN_PRIVILEGE - SE_MIN_WELL_KNOWN_PRIVILEGE + 1;
            Info->PrivilegeCount = Count;
            for (i = 0; i < Count; i++)
            {
                Info->Privileges[i].Luid.LowPart = SE_MIN_WELL_KNOWN_PRIVILEGE + i;
                Info->Privileges[i].Attributes = SE_PRIVILEGE_ENABLED | SE_PRIVILEGE_ENABLED_BY_DEFAULT;
            }
            Needed = FIELD_OFFSET(TOKEN_PRIVILEGES, Privileges) + Count * sizeof(LUID_AND_ATTRIBUTES);
            break;
        }
        case TokenDefaultDacl:
            Needed = sizeof(TOKEN_DEFAULT_DACL);   /* a NULL DACL: everyone has access */
            break;
        case TokenSource:
            RtlCopyMemory(Buffer, &((QZ_TOKEN *)QzSystemToken)->Source, sizeof(TOKEN_SOURCE));
            Needed = sizeof(TOKEN_SOURCE);
            break;
        case TokenType:
            *(PTOKEN_TYPE)Buffer = TokenPrimary;
            Needed = sizeof(TOKEN_TYPE);
            break;
        case TokenImpersonationLevel:
            *(PSECURITY_IMPERSONATION_LEVEL)Buffer = SecurityImpersonation;
            Needed = sizeof(SECURITY_IMPERSONATION_LEVEL);
            break;
        case TokenSessionId:
            Needed = sizeof(ULONG);
            break;
        case TokenStatistics:
        {
            PTOKEN_STATISTICS Info = (PVOID)Buffer;
            Info->TokenId = ((QZ_TOKEN *)QzSystemToken)->TokenId;
            Info->AuthenticationId = QzSystemLuid;
            Info->ExpirationTime.QuadPart = MAXLONGLONG;
            Info->TokenType = TokenPrimary;
            Info->GroupCount = 3;
            Info->PrivilegeCount = SE_MAX_WELL_KNOWN_PRIVILEGE - SE_MIN_WELL_KNOWN_PRIVILEGE + 1;
            Needed = sizeof(*Info);
            break;
        }
        default:
            ExFreePoolWithTag(Buffer, QZ_TOKEN_TAG);
            return STATUS_INVALID_INFO_CLASS;
    }

    if (ReturnLength && !QzWriteArg(Mode, ReturnLength, &Needed, sizeof(Needed)))
        Status = STATUS_ACCESS_VIOLATION;
    else if (TokenInformationLength < Needed)
        Status = STATUS_BUFFER_TOO_SMALL;
    else if (!QzWriteArg(Mode, TokenInformation, Buffer, Needed))
        Status = STATUS_ACCESS_VIOLATION;
    ExFreePoolWithTag(Buffer, QZ_TOKEN_TAG);
    return Status;
}

NTSTATUS
NTAPI
NtSetInformationToken(IN HANDLE TokenHandle, IN TOKEN_INFORMATION_CLASS TokenInformationClass,
                      IN PVOID TokenInformation, IN ULONG TokenInformationLength)
{
    UNREFERENCED_PARAMETER(TokenInformationClass);
    UNREFERENCED_PARAMETER(TokenInformation);
    UNREFERENCED_PARAMETER(TokenInformationLength);
    return QzSeReferenceToken(TokenHandle, TOKEN_ADJUST_DEFAULT);
}

/* A "copy": another handle to the same token */
NTSTATUS
NTAPI
NtDuplicateToken(IN HANDLE ExistingTokenHandle, IN ACCESS_MASK DesiredAccess,
                 IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL, IN BOOLEAN EffectiveOnly,
                 IN TOKEN_TYPE TokenType, OUT PHANDLE NewTokenHandle)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    HANDLE Handle;
    NTSTATUS Status = QzSeReferenceToken(ExistingTokenHandle, TOKEN_DUPLICATE);

    UNREFERENCED_PARAMETER(ObjectAttributes);
    UNREFERENCED_PARAMETER(EffectiveOnly);
    UNREFERENCED_PARAMETER(TokenType);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = ObOpenObjectByPointer(QzSystemToken, 0, NULL, DesiredAccess ? DesiredAccess : TOKEN_ALL_ACCESS,
                                   SeTokenObjectType, Mode, &Handle);
    if (NT_SUCCESS(Status) && !QzWriteArg(Mode, NewTokenHandle, &Handle, sizeof(Handle)))
        Status = STATUS_ACCESS_VIOLATION;
    return Status;
}

/* Access checks grant what is asked: there is one user */
NTSTATUS
NTAPI
NtAccessCheck(IN PSECURITY_DESCRIPTOR SecurityDescriptor, IN HANDLE TokenHandle, IN ACCESS_MASK DesiredAccess,
              IN PGENERIC_MAPPING GenericMapping, OUT PPRIVILEGE_SET PrivilegeSet, IN OUT PULONG PrivilegeSetLength,
              OUT PACCESS_MASK GrantedAccess, OUT PNTSTATUS AccessStatus)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    GENERIC_MAPPING Mapping;
    ACCESS_MASK Granted = DesiredAccess;
    NTSTATUS Ok = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(SecurityDescriptor);
    UNREFERENCED_PARAMETER(TokenHandle);
    UNREFERENCED_PARAMETER(PrivilegeSet);
    UNREFERENCED_PARAMETER(PrivilegeSetLength);
    if (GenericMapping && QzReadArg(Mode, &Mapping, GenericMapping, sizeof(Mapping)))
        RtlMapGenericMask(&Granted, &Mapping);
    if (!QzWriteArg(Mode, GrantedAccess, &Granted, sizeof(Granted)) ||
        !QzWriteArg(Mode, AccessStatus, &Ok, sizeof(Ok)))
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NtPrivilegeCheck(IN HANDLE ClientToken, IN PPRIVILEGE_SET RequiredPrivileges, OUT PBOOLEAN Result)
{
    BOOLEAN Yes = TRUE;
    UNREFERENCED_PARAMETER(ClientToken);
    UNREFERENCED_PARAMETER(RequiredPrivileges);
    return QzWriteArg(ExGetPreviousMode(), Result, &Yes, sizeof(Yes)) ? STATUS_SUCCESS : STATUS_ACCESS_VIOLATION;
}

/* Kernel-mode callers */

NTSTATUS
NTAPI
ZwOpenProcessToken(IN HANDLE ProcessHandle, IN ACCESS_MASK DesiredAccess, OUT PHANDLE TokenHandle)
{
    return QZ_AS_KERNEL(QzSeOpenToken(ProcessHandle, DesiredAccess, 0, TokenHandle));
}

NTSTATUS
NTAPI
ZwAdjustPrivilegesToken(IN HANDLE TokenHandle, IN BOOLEAN DisableAllPrivileges, IN PTOKEN_PRIVILEGES NewState,
                        IN ULONG BufferLength, OUT PTOKEN_PRIVILEGES PreviousState OPTIONAL,
                        OUT PULONG ReturnLength OPTIONAL)
{
    return QZ_AS_KERNEL(NtAdjustPrivilegesToken(TokenHandle, DisableAllPrivileges, NewState, BufferLength,
                                                PreviousState, ReturnLength));
}

NTSTATUS
NTAPI
ZwQueryInformationToken(IN HANDLE TokenHandle, IN TOKEN_INFORMATION_CLASS TokenInformationClass,
                        OUT PVOID TokenInformation, IN ULONG TokenInformationLength, OUT PULONG ReturnLength)
{
    return QZ_AS_KERNEL(NtQueryInformationToken(TokenHandle, TokenInformationClass, TokenInformation,
                                                TokenInformationLength, ReturnLength));
}

NTSTATUS
NTAPI
ZwDuplicateToken(IN HANDLE ExistingTokenHandle, IN ACCESS_MASK DesiredAccess,
                 IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL, IN BOOLEAN EffectiveOnly, IN TOKEN_TYPE TokenType,
                 OUT PHANDLE NewTokenHandle)
{
    return QZ_AS_KERNEL(NtDuplicateToken(ExistingTokenHandle, DesiredAccess, ObjectAttributes, EffectiveOnly,
                                         TokenType, NewTokenHandle));
}
