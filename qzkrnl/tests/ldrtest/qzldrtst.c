/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver for the kernel loader self-test (ldrtest)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Loaded from disk by its service key (QzLdrTest). DriverEntry reports
 * what it sees in the service key's values: the export driver it imports
 * answers and was initialized once, and its own absolute pointers were
 * relocated. Unload notes that it ran.
 */

#include <ntddk.h>

ULONG NTAPI QzLdrDllAdd(ULONG A, ULONG B);
ULONG NTAPI QzLdrDllInitializeCount(VOID);

static ULONG Target = 0x1234;
static PULONG Pointer = &Target;            /* needs a base relocation */

static VOID
Note(PCWSTR Name, ULONG Value)
{
    RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, L"QzLdrTest", Name, REG_DWORD, &Value, sizeof(Value));
}

static VOID
NTAPI
Unload(IN PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    Note(L"Unloaded", 1);
}

NTSTATUS
NTAPI
DriverEntry(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath)
{
    Note(L"Sum", QzLdrDllAdd(2, 3));
    Note(L"DllInit", QzLdrDllInitializeCount());
    Note(L"Relocated", Pointer == &Target && *Pointer == 0x1234);
    Note(L"RegistryPath", RegistryPath && RegistryPath->Length > 0);
    Note(L"InImage", (PUCHAR)DriverEntry >= (PUCHAR)DriverObject->DriverStart &&
                     (PUCHAR)DriverEntry < (PUCHAR)DriverObject->DriverStart + DriverObject->DriverSize);
    DriverObject->DriverUnload = Unload;
    return STATUS_SUCCESS;
}
