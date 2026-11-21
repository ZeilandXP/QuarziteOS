/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel loader self-test (shell command "ldrtest")
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * qzldrtst.sys, a driver on the boot CD that imports the export driver
 * qzldrdll.sys, is loaded through a service key with ZwLoadDriver: both
 * images come from disk, relocated and bound; DllInitialize runs before
 * DriverEntry, which reports back in its service key. Then unloading
 * (DriverUnload, DllUnload, the pages back), loading again, and the
 * failures: a missing file, a disabled service, a file that is no image.
 */

#include "qzio.h"
#include "qzps.h"
#include <ndk/exfuncs.h>
#include <ntddmou.h>
#include <ntddkbd.h>

static ULONG QzPassed, QzFailed;

static VOID
QzCheck(PCSTR Name, BOOLEAN Condition)
{
    if (Condition)
        QzPassed++;
    else
        QzFailed++;
    QzPrint("  [%s] %s\n", Condition ? "PASS" : "FAIL", Name);
}

#define QZ_TEST_SERVICES L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\"

static VOID
QzSetDword(PCWSTR Service, PCWSTR Name, ULONG Value)
{
    RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, Service, Name, REG_DWORD, &Value, sizeof(Value));
}

static ULONG
QzGetDword(PCWSTR Name)
{
    ULONG Value = 0xFFFFFFFF, Default = 0xFFFFFFFF;
    RTL_QUERY_REGISTRY_TABLE Table[2];

    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_REQUIRED;
    Table[0].Name = (PWSTR)Name;
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = REG_DWORD;
    Table[0].DefaultData = &Default;
    Table[0].DefaultLength = sizeof(Default);
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_SERVICES, L"QzLdrTest", Table, NULL, NULL)))
        return 0xFFFFFFFF;
    return Value;
}

static VOID
QzMakeService(PCWSTR Service, PCWSTR ImagePath, ULONG Start)
{
    ULONG Type = SERVICE_KERNEL_DRIVER;
    RtlCreateRegistryKey(RTL_REGISTRY_SERVICES, (PWSTR)Service);
    RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, Service, L"Type", REG_DWORD, &Type, sizeof(Type));
    RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, Service, L"Start", REG_DWORD, &Start, sizeof(Start));
    RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, Service, L"ImagePath", REG_EXPAND_SZ, (PVOID)ImagePath,
                          (ULONG)(wcslen(ImagePath) + 1) * sizeof(WCHAR));
}

/* Is a module (by base name) on the loaded module list? */
static BOOLEAN
QzModuleLoaded(PCSTR Name)
{
    PRTL_PROCESS_MODULES Modules;
    ULONG Length = 0, i;
    BOOLEAN Found = FALSE;

    ZwQuerySystemInformation(SystemModuleInformation, &Length, sizeof(Length), &Length);
    Modules = ExAllocatePoolWithTag(NonPagedPool, Length + 1024, 'tLzQ');
    if (!Modules)
        return FALSE;
    if (NT_SUCCESS(ZwQuerySystemInformation(SystemModuleInformation, Modules, Length + 1024, &Length)))
    {
        for (i = 0; i < Modules->NumberOfModules; i++)
        {
            if (!_stricmp((PCSTR)Modules->Modules[i].FullPathName + Modules->Modules[i].OffsetToFileName, Name))
                Found = TRUE;
        }
    }
    ExFreePoolWithTag(Modules, 'tLzQ');
    return Found;
}

static VOID
QzResetResults(VOID)
{
    static const PCWSTR Names[] = { L"Sum", L"DllInit", L"Relocated", L"RegistryPath", L"InImage", L"Unloaded",
                                    L"DllUnloaded" };
    ULONG i;
    for (i = 0; i < RTL_NUMBER_OF(Names); i++)
        QzSetDword(L"QzLdrTest", Names[i], 0);
}

VOID
QzRunLoaderTests(VOID)
{
    UNICODE_STRING Service = RTL_CONSTANT_STRING(QZ_TEST_SERVICES L"QzLdrTest");
    UNICODE_STRING Missing = RTL_CONSTANT_STRING(QZ_TEST_SERVICES L"QzLdrMissing");
    UNICODE_STRING Disabled = RTL_CONSTANT_STRING(QZ_TEST_SERVICES L"QzLdrDisabled");
    UNICODE_STRING NotImage = RTL_CONSTANT_STRING(L"\\SystemRoot\\System32\\config\\system");
    ULONG Baseline;
    PVOID Entry;
    NTSTATUS Status;

    QzPassed = QzFailed = 0;
    QzPrint("Loading drivers from disk:\n");

    /* A relative ImagePath, as hivesys.inf writes them */
    QzMakeService(L"QzLdrTest", L"System32\\drivers\\qzldrtst.sys", SERVICE_DEMAND_START);
    QzMakeService(L"QzLdrMissing", L"\\SystemRoot\\System32\\drivers\\qznosuch.sys", SERVICE_DEMAND_START);
    QzMakeService(L"QzLdrDisabled", L"System32\\drivers\\qzldrtst.sys", SERVICE_DISABLED);
    QzResetResults();
    Baseline = QzMmSystemImagePages();

    Status = ZwLoadDriver(&Service);
    QzCheck("ZwLoadDriver loads qzldrtst.sys by its service key", NT_SUCCESS(Status));
    if (!NT_SUCCESS(Status))
    {
        QzPrint("  (status 0x%08lx)\n", Status);
        goto Failures;
    }
    QzCheck("the export driver it imports answers (qzldrdll.sys)", QzGetDword(L"Sum") == 5);
    QzCheck("its DllInitialize ran once, before DriverEntry", QzGetDword(L"DllInit") == 1);
    QzCheck("absolute addresses were relocated", QzGetDword(L"Relocated") == 1);
    QzCheck("DriverEntry got the registry path", QzGetDword(L"RegistryPath") == 1);
    QzCheck("the driver object describes the image", QzGetDword(L"InImage") == 1);
    QzCheck("both images are on the module list", QzModuleLoaded("qzldrtst.sys") && QzModuleLoaded("qzldrdll.sys"));
    QzCheck("the images have pages of their own", QzMmSystemImagePages() > Baseline);
    QzCheck("loading it again: already loaded", ZwLoadDriver(&Service) == STATUS_IMAGE_ALREADY_LOADED);

    Status = ZwUnloadDriver(&Service);
    QzCheck("ZwUnloadDriver", NT_SUCCESS(Status));
    QzCheck("DriverUnload ran", QzGetDword(L"Unloaded") == 1);
    QzCheck("the export driver's DllUnload ran", QzGetDword(L"DllUnloaded") == 1);
    QzCheck("both images left the module list", !QzModuleLoaded("qzldrtst.sys") && !QzModuleLoaded("qzldrdll.sys"));
    QzCheck("and gave their pages back", QzMmSystemImagePages() == Baseline);

    QzResetResults();
    Status = ZwLoadDriver(&Service);
    QzCheck("load it again: a fresh image, DllInitialize again",
            NT_SUCCESS(Status) && QzGetDword(L"Sum") == 5 && QzGetDword(L"DllInit") == 1);
    QzCheck("and unload it", NT_SUCCESS(ZwUnloadDriver(&Service)) && QzMmSystemImagePages() == Baseline);

Failures:
    QzCheck("a service whose file is missing fails", !NT_SUCCESS(ZwLoadDriver(&Missing)));
    QzCheck("a disabled service does not load", ZwLoadDriver(&Disabled) == STATUS_DRIVER_UNABLE_TO_LOAD);
    Status = QzMmLoadSystemImage(&NotImage, &Entry);
    QzCheck("a file that is no image is refused", Status == STATUS_INVALID_IMAGE_FORMAT);
    QzCheck("unloading a driver that is not loaded fails", !NT_SUCCESS(ZwUnloadDriver(&Missing)));
    QzCheck("no image pages leaked", QzMmSystemImagePages() == Baseline);
    QzPrint("  ldrtest: %lu passed, %lu failed\n", QzPassed, QzFailed);
}

/*
 * The shell's "input mouse|kbd [ms]": one event from the class driver
 * (\Device\PointerClass0 or \Device\KeyboardClass0), which on QEMU's USB
 * tablet and keyboard means hidusb, hidclass, mouhid/kbdhid and
 * mouclass/kbdclass, all loaded from disk, working together.
 */
VOID
QzIoShellInput(PCSTR Which, ULONG Milliseconds)
{
    BOOLEAN Mouse = !Which || Which[0] != 'k';
    UNICODE_STRING Name;
    PFILE_OBJECT File;
    PDEVICE_OBJECT Device;
    union { MOUSE_INPUT_DATA Mouse; KEYBOARD_INPUT_DATA Keyboard; } Data;
    IO_STATUS_BLOCK Io;
    LARGE_INTEGER Offset, Timeout;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, Mouse ? L"\\Device\\PointerClass0" : L"\\Device\\KeyboardClass0");
    Status = IoGetDeviceObjectPointer(&Name, FILE_READ_DATA, &File, &Device);
    if (!NT_SUCCESS(Status))
    {
        QzPrint("  input: cannot open %wZ (0x%08lx)\n", &Name, Status);
        return;
    }
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Offset.QuadPart = 0;
    RtlZeroMemory(&Data, sizeof(Data));
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ, Device, &Data,
                                       Mouse ? sizeof(MOUSE_INPUT_DATA) : sizeof(KEYBOARD_INPUT_DATA),
                                       &Offset, &Event, &Io);
    if (!Irp)
    {
        ObDereferenceObject(File);
        return;
    }
    IoGetNextIrpStackLocation(Irp)->FileObject = File;
    QzPrint("  input: waiting %lu ms for an event on %wZ\n", Milliseconds, &Name);
    Status = IoCallDriver(Device, Irp);
    if (Status == STATUS_PENDING)
    {
        Timeout.QuadPart = -10000LL * Milliseconds;
        if (KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, &Timeout) == STATUS_TIMEOUT)
        {
            IoCancelIrp(Irp);
            KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        }
        Status = Io.Status;
    }
    if (!NT_SUCCESS(Status))
        QzPrint("  input: no event (0x%08lx)\n", Status);
    else if (Mouse)
        QzPrint("  input: mouse event: flags 0x%x, buttons 0x%x, x %ld, y %ld\n", Data.Mouse.Flags,
                Data.Mouse.ButtonFlags, Data.Mouse.LastX, Data.Mouse.LastY);
    else
        QzPrint("  input: keyboard event: make code 0x%02x, flags 0x%x\n", Data.Keyboard.MakeCode,
                Data.Keyboard.Flags);
    ObDereferenceObject(File);
}
