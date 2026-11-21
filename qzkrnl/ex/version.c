/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     OS version reporting -- QuarziteOS targets Windows 7 SP1
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qz.h"

/* NT encodes the build flavour in the top nibble: 0xF = free, 0xC = checked */
#if DBG
ULONG NtBuildNumber = QZ_NT_BUILD | 0xC0000000;
#else
ULONG NtBuildNumber = QZ_NT_BUILD | 0xF0000000;
#endif

static WCHAR QzCsdString[] = QZ_NT_CSD_STRING;

BOOLEAN
NTAPI
PsGetVersion(OUT PULONG MajorVersion OPTIONAL,
             OUT PULONG MinorVersion OPTIONAL,
             OUT PULONG BuildNumber OPTIONAL,
             OUT PUNICODE_STRING CSDVersion OPTIONAL)
{
    if (MajorVersion) *MajorVersion = QZ_NT_MAJOR;
    if (MinorVersion) *MinorVersion = QZ_NT_MINOR;
    if (BuildNumber) *BuildNumber = QZ_NT_BUILD;
    if (CSDVersion)
    {
        CSDVersion->Buffer = QzCsdString;
        CSDVersion->Length = sizeof(QzCsdString) - sizeof(WCHAR);
        CSDVersion->MaximumLength = sizeof(QzCsdString);
    }
    /* TRUE means "checked build" */
    return (NtBuildNumber & 0xF0000000) == 0xC0000000;
}

NTSTATUS
NTAPI
RtlGetVersion(IN OUT PRTL_OSVERSIONINFOW lpVersionInformation)
{
    /* Like NT's kernel-mode RtlGetVersion: the basic fields are filled
       whatever dwOSVersionInfoSize says (drivers such as the KMDF loader
       call it with the size uninitialized); the extended ones only for
       an RTL_OSVERSIONINFOEXW */
    ULONG Size = lpVersionInformation->dwOSVersionInfoSize;

    lpVersionInformation->dwMajorVersion = QZ_NT_MAJOR;
    lpVersionInformation->dwMinorVersion = QZ_NT_MINOR;
    lpVersionInformation->dwBuildNumber = QZ_NT_BUILD;
    lpVersionInformation->dwPlatformId = VER_PLATFORM_WIN32_NT;
    RtlZeroMemory(lpVersionInformation->szCSDVersion,
                  sizeof(lpVersionInformation->szCSDVersion));
    RtlCopyMemory(lpVersionInformation->szCSDVersion, QzCsdString, sizeof(QzCsdString));

    if (Size == sizeof(RTL_OSVERSIONINFOEXW))
    {
        PRTL_OSVERSIONINFOEXW Ex = (PRTL_OSVERSIONINFOEXW)lpVersionInformation;
        Ex->wServicePackMajor = (USHORT)(QZ_NT_CSD_VERSION >> 8);
        Ex->wServicePackMinor = (USHORT)(QZ_NT_CSD_VERSION & 0xFF);
        Ex->wSuiteMask = VER_SUITE_SINGLEUSERTS;
        Ex->wProductType = VER_NT_WORKSTATION;
        Ex->wReserved = 0;
    }
    return STATUS_SUCCESS;
}
