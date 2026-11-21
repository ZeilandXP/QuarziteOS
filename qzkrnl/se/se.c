/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Security reference monitor stubs
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Everything runs as the system until tokens exist (stage B6), so access
 * checks grant what was asked for (after generic mapping) and privilege
 * checks succeed. SeExports is real: drivers read privilege LUIDs and
 * well-known SIDs from it.
 */

#include "qz.h"

/* ---------------------------------------------------------------------- */
/* Well-known SIDs                                                         */
/* ---------------------------------------------------------------------- */

typedef struct _QZ_SID2
{
    UCHAR Revision;
    UCHAR SubAuthorityCount;
    SID_IDENTIFIER_AUTHORITY IdentifierAuthority;
    ULONG SubAuthority[2];
} QZ_SID2;

#define QZ_SID1(Auth, A)    { SID_REVISION, 1, { { 0, 0, 0, 0, 0, Auth } }, { A, 0 } }
#define QZ_SID2(Auth, A, B) { SID_REVISION, 2, { { 0, 0, 0, 0, 0, Auth } }, { A, B } }
#define QZ_SID0(Auth)       { SID_REVISION, 0, { { 0, 0, 0, 0, 0, Auth } }, { 0, 0 } }

static QZ_SID2 QzNullSid            = QZ_SID1(0, 0);
static QZ_SID2 QzWorldSid           = QZ_SID1(1, 0);
static QZ_SID2 QzLocalSid           = QZ_SID1(2, 0);
static QZ_SID2 QzCreatorOwnerSid    = QZ_SID1(3, 0);
static QZ_SID2 QzCreatorGroupSid    = QZ_SID1(3, 1);
static QZ_SID2 QzNtAuthoritySid     = QZ_SID0(5);
static QZ_SID2 QzDialupSid          = QZ_SID1(5, SECURITY_DIALUP_RID);
static QZ_SID2 QzNetworkSid         = QZ_SID1(5, SECURITY_NETWORK_RID);
static QZ_SID2 QzBatchSid           = QZ_SID1(5, SECURITY_BATCH_RID);
static QZ_SID2 QzInteractiveSid     = QZ_SID1(5, SECURITY_INTERACTIVE_RID);
static QZ_SID2 QzAnonymousSid       = QZ_SID1(5, SECURITY_ANONYMOUS_LOGON_RID);
static QZ_SID2 QzAuthUsersSid       = QZ_SID1(5, SECURITY_AUTHENTICATED_USER_RID);
static QZ_SID2 QzRestrictedSid      = QZ_SID1(5, SECURITY_RESTRICTED_CODE_RID);
static QZ_SID2 QzIUserSid           = QZ_SID1(5, 17);
static QZ_SID2 QzLocalSystemSid     = QZ_SID1(5, SECURITY_LOCAL_SYSTEM_RID);
static QZ_SID2 QzLocalServiceSid    = QZ_SID1(5, SECURITY_LOCAL_SERVICE_RID);
static QZ_SID2 QzNetworkServiceSid  = QZ_SID1(5, SECURITY_NETWORK_SERVICE_RID);
static QZ_SID2 QzAdminsSid          = QZ_SID2(5, 32, DOMAIN_ALIAS_RID_ADMINS);
static QZ_SID2 QzUsersSid           = QZ_SID2(5, 32, DOMAIN_ALIAS_RID_USERS);
static QZ_SID2 QzGuestsSid          = QZ_SID2(5, 32, DOMAIN_ALIAS_RID_GUESTS);
static QZ_SID2 QzPowerUsersSid      = QZ_SID2(5, 32, DOMAIN_ALIAS_RID_POWER_USERS);
static QZ_SID2 QzAccountOpsSid      = QZ_SID2(5, 32, DOMAIN_ALIAS_RID_ACCOUNT_OPS);
static QZ_SID2 QzSystemOpsSid       = QZ_SID2(5, 32, DOMAIN_ALIAS_RID_SYSTEM_OPS);
static QZ_SID2 QzPrintOpsSid        = QZ_SID2(5, 32, DOMAIN_ALIAS_RID_PRINT_OPS);
static QZ_SID2 QzBackupOpsSid       = QZ_SID2(5, 32, DOMAIN_ALIAS_RID_BACKUP_OPS);
static QZ_SID2 QzUntrustedSid       = QZ_SID1(16, 0x0000);
static QZ_SID2 QzLowSid             = QZ_SID1(16, 0x1000);
static QZ_SID2 QzMediumSid          = QZ_SID1(16, 0x2000);
static QZ_SID2 QzHighSid            = QZ_SID1(16, 0x3000);
static QZ_SID2 QzSystemMandatorySid = QZ_SID1(16, 0x4000);
static QZ_SID2 QzOwnerRightsSid     = QZ_SID1(3, 4);
static QZ_SID2 QzAllAppPackagesSid  = QZ_SID2(15, 2, 1);
static QZ_SID2 QzUserModeDriversSid = QZ_SID2(5, 84, 0);

#define P(Sid) ((PSID)&Sid)
#define L(Value) { Value, 0 }

static SE_EXPORTS QzSeExports =
{
    .SeCreateTokenPrivilege          = L(SE_CREATE_TOKEN_PRIVILEGE),
    .SeAssignPrimaryTokenPrivilege   = L(SE_ASSIGNPRIMARYTOKEN_PRIVILEGE),
    .SeLockMemoryPrivilege           = L(SE_LOCK_MEMORY_PRIVILEGE),
    .SeIncreaseQuotaPrivilege        = L(SE_INCREASE_QUOTA_PRIVILEGE),
    .SeUnsolicitedInputPrivilege     = L(SE_UNSOLICITED_INPUT_PRIVILEGE),
    .SeTcbPrivilege                  = L(SE_TCB_PRIVILEGE),
    .SeSecurityPrivilege             = L(SE_SECURITY_PRIVILEGE),
    .SeTakeOwnershipPrivilege        = L(SE_TAKE_OWNERSHIP_PRIVILEGE),
    .SeLoadDriverPrivilege           = L(SE_LOAD_DRIVER_PRIVILEGE),
    .SeCreatePagefilePrivilege       = L(SE_CREATE_PAGEFILE_PRIVILEGE),
    .SeIncreaseBasePriorityPrivilege = L(SE_INC_BASE_PRIORITY_PRIVILEGE),
    .SeSystemProfilePrivilege        = L(SE_SYSTEM_PROFILE_PRIVILEGE),
    .SeSystemtimePrivilege           = L(SE_SYSTEMTIME_PRIVILEGE),
    .SeProfileSingleProcessPrivilege = L(SE_PROF_SINGLE_PROCESS_PRIVILEGE),
    .SeCreatePermanentPrivilege      = L(SE_CREATE_PERMANENT_PRIVILEGE),
    .SeBackupPrivilege               = L(SE_BACKUP_PRIVILEGE),
    .SeRestorePrivilege              = L(SE_RESTORE_PRIVILEGE),
    .SeShutdownPrivilege             = L(SE_SHUTDOWN_PRIVILEGE),
    .SeDebugPrivilege                = L(SE_DEBUG_PRIVILEGE),
    .SeAuditPrivilege                = L(SE_AUDIT_PRIVILEGE),
    .SeSystemEnvironmentPrivilege    = L(SE_SYSTEM_ENVIRONMENT_PRIVILEGE),
    .SeChangeNotifyPrivilege         = L(SE_CHANGE_NOTIFY_PRIVILEGE),
    .SeRemoteShutdownPrivilege       = L(SE_REMOTE_SHUTDOWN_PRIVILEGE),
    .SeUndockPrivilege               = L(SE_UNDOCK_PRIVILEGE),
    .SeSyncAgentPrivilege            = L(SE_SYNC_AGENT_PRIVILEGE),
    .SeEnableDelegationPrivilege     = L(SE_ENABLE_DELEGATION_PRIVILEGE),
    .SeManageVolumePrivilege         = L(SE_MANAGE_VOLUME_PRIVILEGE),
    .SeImpersonatePrivilege          = L(SE_IMPERSONATE_PRIVILEGE),
    .SeCreateGlobalPrivilege         = L(SE_CREATE_GLOBAL_PRIVILEGE),
    .SeTrustedCredManAccessPrivilege = L(SE_TRUSTED_CREDMAN_ACCESS_PRIVILEGE),
    .SeRelabelPrivilege              = L(SE_RELABEL_PRIVILEGE),
    .SeIncreaseWorkingSetPrivilege   = L(SE_INC_WORKING_SET_PRIVILEGE),
    .SeTimeZonePrivilege             = L(SE_TIME_ZONE_PRIVILEGE),
    .SeCreateSymbolicLinkPrivilege   = L(SE_CREATE_SYMBOLIC_LINK_PRIVILEGE),

    .SeNullSid               = P(QzNullSid),
    .SeWorldSid              = P(QzWorldSid),
    .SeLocalSid              = P(QzLocalSid),
    .SeCreatorOwnerSid       = P(QzCreatorOwnerSid),
    .SeCreatorGroupSid       = P(QzCreatorGroupSid),
    .SeNtAuthoritySid        = P(QzNtAuthoritySid),
    .SeDialupSid             = P(QzDialupSid),
    .SeNetworkSid            = P(QzNetworkSid),
    .SeBatchSid              = P(QzBatchSid),
    .SeInteractiveSid        = P(QzInteractiveSid),
    .SeLocalSystemSid        = P(QzLocalSystemSid),
    .SeAliasAdminsSid        = P(QzAdminsSid),
    .SeAliasUsersSid         = P(QzUsersSid),
    .SeAliasGuestsSid        = P(QzGuestsSid),
    .SeAliasPowerUsersSid    = P(QzPowerUsersSid),
    .SeAliasAccountOpsSid    = P(QzAccountOpsSid),
    .SeAliasSystemOpsSid     = P(QzSystemOpsSid),
    .SeAliasPrintOpsSid      = P(QzPrintOpsSid),
    .SeAliasBackupOpsSid     = P(QzBackupOpsSid),
    .SeAuthenticatedUsersSid = P(QzAuthUsersSid),
    .SeRestrictedSid         = P(QzRestrictedSid),
    .SeAnonymousLogonSid     = P(QzAnonymousSid),
    .SeLocalServiceSid       = P(QzLocalServiceSid),
    .SeNetworkServiceSid     = P(QzNetworkServiceSid),
    .SeIUserSid              = P(QzIUserSid),
    .SeUntrustedMandatorySid = P(QzUntrustedSid),
    .SeLowMandatorySid       = P(QzLowSid),
    .SeMediumMandatorySid    = P(QzMediumSid),
    .SeHighMandatorySid      = P(QzHighSid),
    .SeSystemMandatorySid    = P(QzSystemMandatorySid),
    .SeOwnerRightsSid        = P(QzOwnerRightsSid),
    .SeAllAppPackagesSid     = P(QzAllAppPackagesSid),
    .SeUserModeDriversSid    = P(QzUserModeDriversSid),
};

#undef P
#undef L

PSE_EXPORTS SeExports = &QzSeExports;

/* ---------------------------------------------------------------------- */
/* Subject contexts and checks                                             */
/* ---------------------------------------------------------------------- */

VOID
NTAPI
SeCaptureSubjectContext(OUT PSECURITY_SUBJECT_CONTEXT SubjectContext)
{
    /* No tokens yet: the system context is all-NULL */
    RtlZeroMemory(SubjectContext, sizeof(*SubjectContext));
    SubjectContext->ImpersonationLevel = SecurityAnonymous;
}

VOID
NTAPI
SeReleaseSubjectContext(IN PSECURITY_SUBJECT_CONTEXT SubjectContext)
{
    UNREFERENCED_PARAMETER(SubjectContext);
}

VOID
NTAPI
SeLockSubjectContext(IN PSECURITY_SUBJECT_CONTEXT SubjectContext)
{
    UNREFERENCED_PARAMETER(SubjectContext);
}

VOID
NTAPI
SeUnlockSubjectContext(IN PSECURITY_SUBJECT_CONTEXT SubjectContext)
{
    UNREFERENCED_PARAMETER(SubjectContext);
}

BOOLEAN
NTAPI
SeSinglePrivilegeCheck(IN LUID PrivilegeValue,
                       IN KPROCESSOR_MODE PreviousMode)
{
    UNREFERENCED_PARAMETER(PrivilegeValue);
    UNREFERENCED_PARAMETER(PreviousMode);
    return TRUE;   /* the system holds every privilege */
}

BOOLEAN
NTAPI
SeAccessCheck(IN PSECURITY_DESCRIPTOR SecurityDescriptor,
              IN PSECURITY_SUBJECT_CONTEXT SubjectSecurityContext,
              IN BOOLEAN SubjectContextLocked,
              IN ACCESS_MASK DesiredAccess,
              IN ACCESS_MASK PreviouslyGrantedAccess,
              OUT PPRIVILEGE_SET *Privileges OPTIONAL,
              IN PGENERIC_MAPPING GenericMapping,
              IN KPROCESSOR_MODE AccessMode,
              OUT PACCESS_MASK GrantedAccess,
              OUT PNTSTATUS AccessStatus)
{
    ACCESS_MASK Access = DesiredAccess;

    UNREFERENCED_PARAMETER(SecurityDescriptor);
    UNREFERENCED_PARAMETER(SubjectSecurityContext);
    UNREFERENCED_PARAMETER(SubjectContextLocked);
    UNREFERENCED_PARAMETER(AccessMode);

    if (Privileges)
        *Privileges = NULL;
    if (Access & MAXIMUM_ALLOWED)
        Access = (Access & ~MAXIMUM_ALLOWED) | GENERIC_ALL;
    RtlMapGenericMask(&Access, GenericMapping);
    *GrantedAccess = Access | PreviouslyGrantedAccess;
    *AccessStatus = STATUS_SUCCESS;
    return TRUE;
}

VOID
NTAPI
RtlMapGenericMask(IN OUT PACCESS_MASK AccessMask,
                  IN PGENERIC_MAPPING GenericMapping)
{
    ACCESS_MASK Mask = *AccessMask;

    if (Mask & GENERIC_READ)    Mask |= GenericMapping->GenericRead;
    if (Mask & GENERIC_WRITE)   Mask |= GenericMapping->GenericWrite;
    if (Mask & GENERIC_EXECUTE) Mask |= GenericMapping->GenericExecute;
    if (Mask & GENERIC_ALL)     Mask |= GenericMapping->GenericAll;
    *AccessMask = Mask & ~(GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL);
}

/* No security descriptors on objects yet */
NTSTATUS
NTAPI
ObGetObjectSecurity(IN PVOID Object,
                    OUT PSECURITY_DESCRIPTOR *SecurityDescriptor,
                    OUT PBOOLEAN MemoryAllocated)
{
    UNREFERENCED_PARAMETER(Object);
    *SecurityDescriptor = NULL;
    *MemoryAllocated = FALSE;
    return STATUS_SUCCESS;
}

VOID
NTAPI
ObReleaseObjectSecurity(IN PSECURITY_DESCRIPTOR SecurityDescriptor,
                        IN BOOLEAN MemoryAllocated)
{
    if (SecurityDescriptor && MemoryAllocated)
        ExFreePool(SecurityDescriptor);
}
