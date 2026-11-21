/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Security identifiers
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qzio.h"

ULONG
NTAPI
RtlLengthRequiredSid(IN ULONG SubAuthorityCount)
{
    return FIELD_OFFSET(SID, SubAuthority) + SubAuthorityCount * sizeof(ULONG);
}

BOOLEAN
NTAPI
RtlValidSid(IN PSID Sid)
{
    PISID S = Sid;
    return S && S->Revision == SID_REVISION && S->SubAuthorityCount <= SID_MAX_SUB_AUTHORITIES;
}

ULONG
NTAPI
RtlLengthSid(IN PSID Sid)
{
    return RtlLengthRequiredSid(((PISID)Sid)->SubAuthorityCount);
}

NTSTATUS
NTAPI
RtlCopySid(IN ULONG BufferLength, IN PSID Destination, IN PSID Source)
{
    ULONG Length = RtlLengthSid(Source);
    if (BufferLength < Length)
        return STATUS_BUFFER_TOO_SMALL;
    RtlMoveMemory(Destination, Source, Length);
    return STATUS_SUCCESS;
}

BOOLEAN
NTAPI
RtlEqualSid(IN PSID Sid1, IN PSID Sid2)
{
    ULONG Length = RtlLengthSid(Sid1);
    return ((PISID)Sid1)->Revision == ((PISID)Sid2)->Revision && Length == RtlLengthSid(Sid2) &&
           RtlCompareMemory(Sid1, Sid2, Length) == Length;
}

NTSTATUS
NTAPI
RtlInitializeSid(IN PSID Sid, IN PSID_IDENTIFIER_AUTHORITY Authority, IN UCHAR SubAuthorityCount)
{
    PISID S = Sid;
    if (SubAuthorityCount > SID_MAX_SUB_AUTHORITIES)
        return STATUS_INVALID_PARAMETER;
    S->Revision = SID_REVISION;
    S->SubAuthorityCount = SubAuthorityCount;
    S->IdentifierAuthority = *Authority;
    return STATUS_SUCCESS;
}

PULONG
NTAPI
RtlSubAuthoritySid(IN PSID Sid, IN ULONG SubAuthority)
{
    return &((PISID)Sid)->SubAuthority[SubAuthority];
}

PUCHAR
NTAPI
RtlSubAuthorityCountSid(IN PSID Sid)
{
    return &((PISID)Sid)->SubAuthorityCount;
}

/* "S-1-5-18" */
NTSTATUS
NTAPI
RtlConvertSidToUnicodeString(OUT PUNICODE_STRING String, IN PSID Sid, IN BOOLEAN AllocateDestinationString)
{
    PISID S = Sid;
    WCHAR Text[256], *At = Text;
    const WCHAR *End = Text + RTL_NUMBER_OF(Text);
    ULONGLONG Authority = 0;
    ULONG i, Length;

    if (!RtlValidSid(Sid))
        return STATUS_INVALID_SID;
    for (i = 0; i < 6; i++)
        Authority = (Authority << 8) | S->IdentifierAuthority.Value[i];
    At += _snwprintf(At, End - At, (Authority >> 32) ? L"S-%u-0x%I64X" : L"S-%u-%I64u", S->Revision, Authority);
    for (i = 0; i < S->SubAuthorityCount; i++)
        At += _snwprintf(At, End - At, L"-%lu", S->SubAuthority[i]);
    Length = (ULONG)(At - Text) * sizeof(WCHAR);

    if (AllocateDestinationString)
    {
        String->Buffer = ExAllocatePoolWithTag(PagedPool, Length + sizeof(WCHAR), 'diSQ');
        if (!String->Buffer)
            return STATUS_NO_MEMORY;
        String->MaximumLength = (USHORT)(Length + sizeof(WCHAR));
    }
    else if (String->MaximumLength < Length + sizeof(WCHAR))
        return STATUS_BUFFER_OVERFLOW;
    RtlCopyMemory(String->Buffer, Text, Length);
    String->Buffer[Length / sizeof(WCHAR)] = 0;
    String->Length = (USHORT)Length;
    return STATUS_SUCCESS;
}
