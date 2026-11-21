/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Export driver for the kernel loader self-test (ldrtest)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * qzldrtst.sys imports this one, so loading the driver loads it from disk
 * as well, runs DllInitialize, and the last unload runs DllUnload.
 */

#include <ntddk.h>

static LONG InitializeCount;

static VOID
Note(PCWSTR Name, ULONG Value)
{
    RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, L"QzLdrTest", Name, REG_DWORD, &Value, sizeof(Value));
}

ULONG
NTAPI
QzLdrDllAdd(ULONG A, ULONG B)
{
    return A + B;
}

ULONG
NTAPI
QzLdrDllInitializeCount(VOID)
{
    return (ULONG)InitializeCount;
}

NTSTATUS
NTAPI
DllInitialize(IN PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);
    InterlockedIncrement(&InitializeCount);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DllUnload(VOID)
{
    Note(L"DllUnloaded", 1);
    return STATUS_SUCCESS;
}

/* An export driver is never started as a driver */
NTSTATUS
NTAPI
DriverEntry(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    return STATUS_SUCCESS;
}
