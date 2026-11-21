/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Security descriptors and ACLs (building them; nothing checks them yet)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qz.h"

NTSTATUS
NTAPI
RtlCreateSecurityDescriptor(OUT PSECURITY_DESCRIPTOR SecurityDescriptor, IN ULONG Revision)
{
    PISECURITY_DESCRIPTOR Sd = SecurityDescriptor;
    if (Revision != SECURITY_DESCRIPTOR_REVISION)
        return STATUS_UNKNOWN_REVISION;
    RtlZeroMemory(Sd, sizeof(SECURITY_DESCRIPTOR));
    Sd->Revision = (UCHAR)Revision;
    return STATUS_SUCCESS;
}

/* The parts of a descriptor, absolute or self-relative */
static PVOID
QzSdPart(PISECURITY_DESCRIPTOR Sd, ULONG Which)
{
    if (Sd->Control & SE_SELF_RELATIVE)
    {
        PISECURITY_DESCRIPTOR_RELATIVE Rel = (PVOID)Sd;
        ULONG Offset = Which == 0 ? Rel->Owner : Which == 1 ? Rel->Group : Which == 2 ? Rel->Sacl : Rel->Dacl;
        return Offset ? (PUCHAR)Sd + Offset : NULL;
    }
    return Which == 0 ? (PVOID)Sd->Owner : Which == 1 ? (PVOID)Sd->Group : Which == 2 ? (PVOID)Sd->Sacl
                                                                                      : (PVOID)Sd->Dacl;
}

ULONG
NTAPI
RtlLengthSecurityDescriptor(IN PSECURITY_DESCRIPTOR SecurityDescriptor)
{
    PISECURITY_DESCRIPTOR Sd = SecurityDescriptor;
    ULONG Length = sizeof(SECURITY_DESCRIPTOR_RELATIVE);
    PVOID Part;

    if ((Part = QzSdPart(Sd, 0)) != NULL)
        Length += (RtlLengthSid(Part) + 3) & ~3;
    if ((Part = QzSdPart(Sd, 1)) != NULL)
        Length += (RtlLengthSid(Part) + 3) & ~3;
    if ((Sd->Control & SE_SACL_PRESENT) && (Part = QzSdPart(Sd, 2)) != NULL)
        Length += (((PACL)Part)->AclSize + 3) & ~3;
    if ((Sd->Control & SE_DACL_PRESENT) && (Part = QzSdPart(Sd, 3)) != NULL)
        Length += (((PACL)Part)->AclSize + 3) & ~3;
    return Length;
}

NTSTATUS
NTAPI
RtlSetDaclSecurityDescriptor(IN OUT PSECURITY_DESCRIPTOR SecurityDescriptor, IN BOOLEAN DaclPresent,
                             IN PACL Dacl OPTIONAL, IN BOOLEAN DaclDefaulted OPTIONAL)
{
    PISECURITY_DESCRIPTOR Sd = SecurityDescriptor;
    if (Sd->Control & SE_SELF_RELATIVE)
        return STATUS_INVALID_SECURITY_DESCR;
    Sd->Control &= ~(SE_DACL_PRESENT | SE_DACL_DEFAULTED);
    Sd->Dacl = NULL;
    if (DaclPresent)
    {
        Sd->Control |= SE_DACL_PRESENT | (DaclDefaulted ? SE_DACL_DEFAULTED : 0);
        Sd->Dacl = Dacl;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlSetSaclSecurityDescriptor(IN OUT PSECURITY_DESCRIPTOR SecurityDescriptor, IN BOOLEAN SaclPresent,
                             IN PACL Sacl OPTIONAL, IN BOOLEAN SaclDefaulted OPTIONAL)
{
    PISECURITY_DESCRIPTOR Sd = SecurityDescriptor;
    if (Sd->Control & SE_SELF_RELATIVE)
        return STATUS_INVALID_SECURITY_DESCR;
    Sd->Control &= ~(SE_SACL_PRESENT | SE_SACL_DEFAULTED);
    Sd->Sacl = NULL;
    if (SaclPresent)
    {
        Sd->Control |= SE_SACL_PRESENT | (SaclDefaulted ? SE_SACL_DEFAULTED : 0);
        Sd->Sacl = Sacl;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlSetOwnerSecurityDescriptor(IN OUT PSECURITY_DESCRIPTOR SecurityDescriptor, IN PSID Owner OPTIONAL,
                              IN BOOLEAN OwnerDefaulted OPTIONAL)
{
    PISECURITY_DESCRIPTOR Sd = SecurityDescriptor;
    if (Sd->Control & SE_SELF_RELATIVE)
        return STATUS_INVALID_SECURITY_DESCR;
    Sd->Owner = Owner;
    Sd->Control = (Sd->Control & ~SE_OWNER_DEFAULTED) | (OwnerDefaulted ? SE_OWNER_DEFAULTED : 0);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlSetGroupSecurityDescriptor(IN OUT PSECURITY_DESCRIPTOR SecurityDescriptor, IN PSID Group OPTIONAL,
                              IN BOOLEAN GroupDefaulted OPTIONAL)
{
    PISECURITY_DESCRIPTOR Sd = SecurityDescriptor;
    if (Sd->Control & SE_SELF_RELATIVE)
        return STATUS_INVALID_SECURITY_DESCR;
    Sd->Group = Group;
    Sd->Control = (Sd->Control & ~SE_GROUP_DEFAULTED) | (GroupDefaulted ? SE_GROUP_DEFAULTED : 0);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlGetDaclSecurityDescriptor(IN PSECURITY_DESCRIPTOR SecurityDescriptor, OUT PBOOLEAN DaclPresent,
                             OUT PACL *Dacl, OUT PBOOLEAN DaclDefaulted)
{
    PISECURITY_DESCRIPTOR Sd = SecurityDescriptor;
    *DaclPresent = (Sd->Control & SE_DACL_PRESENT) != 0;
    *Dacl = *DaclPresent ? QzSdPart(Sd, 3) : NULL;
    *DaclDefaulted = (Sd->Control & SE_DACL_DEFAULTED) != 0;
    return STATUS_SUCCESS;
}

/* One block: the header, then owner, group, SACL and DACL at offsets */
NTSTATUS
NTAPI
RtlAbsoluteToSelfRelativeSD(IN PSECURITY_DESCRIPTOR AbsoluteSecurityDescriptor,
                            OUT PSECURITY_DESCRIPTOR SelfRelativeSecurityDescriptor, IN OUT PULONG BufferLength)
{
    PISECURITY_DESCRIPTOR Sd = AbsoluteSecurityDescriptor;
    PISECURITY_DESCRIPTOR_RELATIVE Rel = SelfRelativeSecurityDescriptor;
    ULONG Length = RtlLengthSecurityDescriptor(Sd), Offset = sizeof(*Rel), i;

    if (Sd->Control & SE_SELF_RELATIVE)
        return STATUS_BAD_DESCRIPTOR_FORMAT;
    if (*BufferLength < Length)
    {
        *BufferLength = Length;
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlZeroMemory(Rel, Length);
    Rel->Revision = Sd->Revision;
    Rel->Sbz1 = Sd->Sbz1;
    Rel->Control = Sd->Control | SE_SELF_RELATIVE;
    for (i = 0; i < 4; i++)
    {
        PVOID Part = QzSdPart(Sd, i);
        ULONG Size;
        if (!Part || (i == 2 && !(Sd->Control & SE_SACL_PRESENT)) || (i == 3 && !(Sd->Control & SE_DACL_PRESENT)))
            continue;
        Size = i < 2 ? RtlLengthSid(Part) : ((PACL)Part)->AclSize;
        RtlCopyMemory((PUCHAR)Rel + Offset, Part, Size);
        if (i == 0)
            Rel->Owner = Offset;
        else if (i == 1)
            Rel->Group = Offset;
        else if (i == 2)
            Rel->Sacl = Offset;
        else
            Rel->Dacl = Offset;
        Offset += (Size + 3) & ~3;
    }
    *BufferLength = Length;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlCreateAcl(OUT PACL Acl, IN ULONG AclLength, IN ULONG AclRevision)
{
    if (AclLength < sizeof(ACL) || AclLength > MAXUSHORT)
        return STATUS_BUFFER_TOO_SMALL;
    if (AclRevision < MIN_ACL_REVISION || AclRevision > MAX_ACL_REVISION)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Acl, sizeof(ACL));
    Acl->AclRevision = (UCHAR)AclRevision;
    Acl->AclSize = (USHORT)(AclLength & ~3);
    return STATUS_SUCCESS;
}

static NTSTATUS
QzAddAce(PACL Acl, ULONG Revision, UCHAR Type, ULONG Flags, ACCESS_MASK Mask, PSID Sid)
{
    PUCHAR End = (PUCHAR)(Acl + 1);
    PACCESS_ALLOWED_ACE Ace;
    ULONG i, Size;

    if (!RtlValidSid(Sid))
        return STATUS_INVALID_SID;
    for (i = 0; i < Acl->AceCount; i++)
        End += ((PACE_HEADER)End)->AceSize;
    Size = FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart) + RtlLengthSid(Sid);
    if (End + Size > (PUCHAR)Acl + Acl->AclSize)
        return STATUS_ALLOTTED_SPACE_EXCEEDED;
    Ace = (PACCESS_ALLOWED_ACE)End;
    Ace->Header.AceType = Type;
    Ace->Header.AceFlags = (UCHAR)Flags;
    Ace->Header.AceSize = (USHORT)Size;
    Ace->Mask = Mask;
    RtlCopySid(RtlLengthSid(Sid), &Ace->SidStart, Sid);
    Acl->AceCount++;
    if (Revision > Acl->AclRevision)
        Acl->AclRevision = (UCHAR)Revision;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlAddAccessAllowedAceEx(IN OUT PACL Acl, IN ULONG AceRevision, IN ULONG AceFlags, IN ACCESS_MASK AccessMask,
                         IN PSID Sid)
{
    return QzAddAce(Acl, AceRevision, ACCESS_ALLOWED_ACE_TYPE, AceFlags, AccessMask, Sid);
}

NTSTATUS
NTAPI
RtlAddAccessAllowedAce(IN OUT PACL Acl, IN ULONG AceRevision, IN ACCESS_MASK AccessMask, IN PSID Sid)
{
    return QzAddAce(Acl, AceRevision, ACCESS_ALLOWED_ACE_TYPE, 0, AccessMask, Sid);
}

NTSTATUS
NTAPI
RtlAddAccessDeniedAce(IN OUT PACL Acl, IN ULONG AceRevision, IN ACCESS_MASK AccessMask, IN PSID Sid)
{
    return QzAddAce(Acl, AceRevision, ACCESS_DENIED_ACE_TYPE, 0, AccessMask, Sid);
}

BOOLEAN
NTAPI
RtlAreAllAccessesGranted(IN ACCESS_MASK GrantedAccess, IN ACCESS_MASK DesiredAccess)
{
    return (GrantedAccess & DesiredAccess) == DesiredAccess;
}

BOOLEAN
NTAPI
RtlAreAnyAccessesGranted(IN ACCESS_MASK GrantedAccess, IN ACCESS_MASK DesiredAccess)
{
    return (GrantedAccess & DesiredAccess) != 0;
}

BOOLEAN
NTAPI
RtlValidSecurityDescriptor(IN PSECURITY_DESCRIPTOR SecurityDescriptor)
{
    return SecurityDescriptor && ((PISECURITY_DESCRIPTOR)SecurityDescriptor)->Revision == SECURITY_DESCRIPTOR_REVISION;
}
