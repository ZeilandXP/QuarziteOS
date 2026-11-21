/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     I/O manager: driver objects, starting boot drivers,
 *              export-driver initialization, reinitialization routines
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * FreeLoader has already loaded the boot drivers and the export drivers
 * they import (scsiport, classpnp, ...), relocated them and bound their
 * imports to qzkrnl. Every other driver is loaded from the file its
 * service key names (ImagePath, or System32\drivers\<service>.sys) by
 * mm/sysload.c. Starting a driver means: create its driver object, run
 * DllInitialize for every export driver it depends on, then call
 * DriverEntry. Drivers are started on demand (when the PnP manager finds
 * a device for them, ZwLoadDriver, or the shell), so one driver hitting a
 * missing export does not take the others down with it.
 */

#include "qzio.h"

LIST_ENTRY QzIoDriverList;

typedef struct _QZ_REINIT
{
    LIST_ENTRY Link;
    PDRIVER_OBJECT DriverObject;
    PDRIVER_REINITIALIZE Routine;
    PVOID Context;
    ULONG Count;
} QZ_REINIT, *PQZ_REINIT;

/* Export drivers whose DllInitialize has run */
#define QZ_MAX_DLLS 32
static PVOID QzIoInitializedDlls[QZ_MAX_DLLS];
static ULONG QzIoInitializedDllCount;

static const WCHAR QzServicesPath[] = L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\";

NTSTATUS
NTAPI
QzIoInvalidDeviceRequest(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_INVALID_DEVICE_REQUEST;
}

VOID NTAPI
QzIoDeleteDriver(PVOID Object)
{
    PDRIVER_OBJECT Driver = Object;
    PQZ_DRIVER_EXTENSION Extension = QzDriverExtension(Driver);
    PIO_CLIENT_EXTENSION Client, Next;

    if (Extension->Link.Flink)
        RemoveEntryList(&Extension->Link);
    for (Client = Extension->Public.ClientDriverExtension; Client; Client = Next)
    {
        Next = Client->NextExtension;
        ExFreePoolWithTag(Client, QZ_IO_TAG);
    }
    /* A driver loaded from disk takes its image (and imports) with it */
    if (QzMmIsDiskImage(Driver->DriverSection))
        QzMmUnloadSystemImage(Driver->DriverSection);
}

static NTSTATUS
QzIoCreateDriverObject(PCUNICODE_STRING Name, PCUNICODE_STRING Service, PVOID ImageBase,
                       ULONG ImageSize, PDRIVER_INITIALIZE Entry, PDRIVER_OBJECT *Result)
{
    OBJECT_ATTRIBUTES Attributes;
    PDRIVER_OBJECT Driver;
    PQZ_DRIVER_EXTENSION Extension;
    NTSTATUS Status;
    ULONG i;
    static UNICODE_STRING HardwareDatabase =
        RTL_CONSTANT_STRING(L"\\Registry\\Machine\\Hardware\\Description\\System");

    InitializeObjectAttributes(&Attributes, (PUNICODE_STRING)Name,
                               OBJ_PERMANENT | OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ObCreateObject(KernelMode, IoDriverObjectType, &Attributes, KernelMode, NULL,
                            sizeof(DRIVER_OBJECT) + sizeof(QZ_DRIVER_EXTENSION), 0, 0,
                            (PVOID *)&Driver);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(Driver, sizeof(DRIVER_OBJECT) + sizeof(QZ_DRIVER_EXTENSION));
    Extension = (PQZ_DRIVER_EXTENSION)(Driver + 1);
    Driver->Type = IO_TYPE_DRIVER;
    Driver->Size = sizeof(DRIVER_OBJECT);
    Driver->DriverExtension = (PDRIVER_EXTENSION)Extension;
    Driver->DriverStart = ImageBase;
    Driver->DriverSize = ImageSize;
    Driver->DriverInit = Entry;
    Driver->HardwareDatabase = &HardwareDatabase;
    Driver->Flags = ImageBase ? DRVO_LEGACY_DRIVER : DRVO_BUILTIN_DRIVER;
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        Driver->MajorFunction[i] = QzIoInvalidDeviceRequest;

    Extension->Public.DriverObject = Driver;
    Extension->ImageBase = ImageBase;
    InitializeListHead(&Extension->ReinitList);
    InitializeListHead(&Extension->BootReinitList);

    Driver->DriverName.Buffer = Extension->NameBuffer;
    Driver->DriverName.MaximumLength = sizeof(Extension->NameBuffer);
    RtlCopyUnicodeString(&Driver->DriverName, Name);
    Extension->Public.ServiceKeyName.Buffer = Extension->ServiceBuffer;
    Extension->Public.ServiceKeyName.MaximumLength = sizeof(Extension->ServiceBuffer);
    RtlCopyUnicodeString(&Extension->Public.ServiceKeyName, Service);

    Status = ObInsertObject(Driver, NULL, FILE_READ_DATA, 0, NULL, NULL);
    if (!NT_SUCCESS(Status))
        return Status;   /* ObInsertObject released the object */

    InsertTailList(&QzIoDriverList, &Extension->Link);
    *Result = Driver;
    return STATUS_SUCCESS;
}

/* Legacy drivers' devices are usable once DriverEntry returns */
static VOID
QzIoFinishDriverEntry(PDRIVER_OBJECT Driver, NTSTATUS Status)
{
    PQZ_DRIVER_EXTENSION Extension = QzDriverExtension(Driver);
    PDEVICE_OBJECT Device;

    Extension->EntryCalled = TRUE;
    Extension->EntryStatus = Status;
    if (NT_SUCCESS(Status))
    {
        Driver->Flags |= DRVO_INITIALIZED;
        for (Device = Driver->DeviceObject; Device; Device = Device->NextDevice)
            Device->Flags &= ~DO_DEVICE_INITIALIZING;
    }
}

NTSTATUS
NTAPI
IoCreateDriver(IN PUNICODE_STRING DriverName OPTIONAL,
               IN PDRIVER_INITIALIZE InitializationFunction)
{
    WCHAR Buffer[64];
    UNICODE_STRING Name, Service;
    PDRIVER_OBJECT Driver;
    NTSTATUS Status;
    static ULONG Counter;

    if (DriverName)
    {
        Name = *DriverName;
    }
    else
    {
        Name.Buffer = Buffer;
        Name.MaximumLength = sizeof(Buffer);
        Name.Length = (USHORT)(_snwprintf(Buffer, RTL_NUMBER_OF(Buffer), L"\\Driver\\%08lx",
                                          (ULONG)(ULONG_PTR)InitializationFunction + Counter++)
                               * sizeof(WCHAR));
    }

    /* The service name is the last path component */
    Service = Name;
    while (Service.Length && Service.Buffer[Service.Length / sizeof(WCHAR) - 1] != L'\\')
        Service.Length -= sizeof(WCHAR);
    Service.Buffer += Service.Length / sizeof(WCHAR);
    Service.Length = Name.Length - Service.Length;
    Service.MaximumLength = Service.Length;

    Status = QzIoCreateDriverObject(&Name, &Service, NULL, 0, InitializationFunction, &Driver);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = InitializationFunction(Driver, NULL);
    QzIoFinishDriverEntry(Driver, Status);
    if (!NT_SUCCESS(Status))
    {
        ObMakeTemporaryObject(Driver);
        ObDereferenceObject(Driver);
    }
    return Status;
}

VOID
NTAPI
IoDeleteDriver(IN PDRIVER_OBJECT DriverObject)
{
    ObMakeTemporaryObject(DriverObject);
    ObDereferenceObject(DriverObject);
}

PDRIVER_OBJECT
QzIoFindDriver(PCUNICODE_STRING ServiceName)
{
    PLIST_ENTRY Entry;

    for (Entry = QzIoDriverList.Flink; Entry != &QzIoDriverList; Entry = Entry->Flink)
    {
        PQZ_DRIVER_EXTENSION Extension = CONTAINING_RECORD(Entry, QZ_DRIVER_EXTENSION, Link);
        if (RtlEqualUnicodeString(&Extension->Public.ServiceKeyName, ServiceName, TRUE))
            return Extension->Public.DriverObject;
    }
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Loaded modules                                                          */
/* ---------------------------------------------------------------------- */

static PQZ_LDR_ENTRY
QzIoFindModuleA(PCSTR Name)
{
    PLIST_ENTRY Entry;

    for (Entry = QzLoaderBlock->LoadOrderListHead.Flink;
         Entry != &QzLoaderBlock->LoadOrderListHead;
         Entry = Entry->Flink)
    {
        PQZ_LDR_ENTRY Module = CONTAINING_RECORD(Entry, QZ_LDR_ENTRY, InLoadOrderLinks);
        ULONG i, Length = Module->BaseDllName.Length / sizeof(WCHAR);

        for (i = 0; i < Length && Name[i]; i++)
        {
            if (RtlUpcaseUnicodeChar(Module->BaseDllName.Buffer[i]) !=
                RtlUpcaseUnicodeChar((UCHAR)Name[i]))
                break;
        }
        if (i == Length && !Name[i])
            return Module;
    }
    return NULL;
}

static VOID
QzIoServiceFromModule(PQZ_LDR_ENTRY Module, PUNICODE_STRING Service, PWCHAR Buffer, ULONG Size)
{
    ULONG i;

    Service->Buffer = Buffer;
    Service->MaximumLength = (USHORT)Size;
    for (i = 0; i < Module->BaseDllName.Length / sizeof(WCHAR) && (i + 1) * sizeof(WCHAR) < Size; i++)
    {
        if (Module->BaseDllName.Buffer[i] == L'.')
            break;
        Buffer[i] = Module->BaseDllName.Buffer[i];
    }
    Service->Length = (USHORT)(i * sizeof(WCHAR));
}

static BOOLEAN
QzIoIsCoreModule(PCSTR Name)
{
    static const PCSTR Core[] = { "ntoskrnl.exe", "hal.dll", "kdcom.dll", "bootvid.dll" };
    ULONG i, k;

    for (i = 0; i < RTL_NUMBER_OF(Core); i++)
    {
        for (k = 0; Core[i][k] && Name[k]; k++)
        {
            CHAR a = Name[k];
            if (a >= 'A' && a <= 'Z')
                a += 'a' - 'A';
            if (a != Core[i][k])
                break;
        }
        if (!Core[i][k] && !Name[k])
            return TRUE;
    }
    return FALSE;
}

/* An export driver went away: a new load runs its DllInitialize again */
VOID
QzIoForgetDll(PVOID ImageBase)
{
    ULONG i;
    for (i = 0; i < QzIoInitializedDllCount; i++)
    {
        if (QzIoInitializedDlls[i] == ImageBase)
        {
            QzIoInitializedDlls[i] = QzIoInitializedDlls[--QzIoInitializedDllCount];
            return;
        }
    }
}

/* Run DllInitialize for the export drivers an image imports (depth first) */
NTSTATUS
QzIoInitializeImports(PVOID ImageBase, ULONG Depth)
{
    PUCHAR Base = ImageBase;
    PIMAGE_NT_HEADERS Nt = (PIMAGE_NT_HEADERS)(Base + ((PIMAGE_DOS_HEADER)Base)->e_lfanew);
    PIMAGE_DATA_DIRECTORY Directory = &Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    PIMAGE_IMPORT_DESCRIPTOR Import;
    NTSTATUS Status;

    if (!Directory->Size || Depth > 8)
        return STATUS_SUCCESS;

    for (Import = (PIMAGE_IMPORT_DESCRIPTOR)(Base + Directory->VirtualAddress); Import->Name; Import++)
    {
        PCSTR DllName = (PCSTR)(Base + Import->Name);
        PQZ_LDR_ENTRY Module = QzIoFindModuleA(DllName);
        PIMAGE_NT_HEADERS DllNt;
        QZ_DLL_INITIALIZE DllInitialize;
        ULONG i;
        BOOLEAN Done = FALSE;

        if (!Module || QzIoIsCoreModule(DllName))
        {
            continue;
        }
        for (i = 0; i < QzIoInitializedDllCount; i++)
            Done |= (QzIoInitializedDlls[i] == Module->DllBase);
        if (Done)
            continue;

        /* Dependencies first */
        Status = QzIoInitializeImports(Module->DllBase, Depth + 1);
        if (!NT_SUCCESS(Status))
            return Status;

        if (QzIoInitializedDllCount < QZ_MAX_DLLS)
            QzIoInitializedDlls[QzIoInitializedDllCount++] = Module->DllBase;

        DllNt = (PIMAGE_NT_HEADERS)((PUCHAR)Module->DllBase +
                                    ((PIMAGE_DOS_HEADER)Module->DllBase)->e_lfanew);
        if (!(DllNt->FileHeader.Characteristics & IMAGE_FILE_DLL))
            continue;
        DllInitialize = (QZ_DLL_INITIALIZE)QzFindImageExport(Module->DllBase, "DllInitialize");
        if (DllInitialize)
        {
            WCHAR PathBuffer[128], ServiceBuffer[32];
            UNICODE_STRING Path, Service;

            QzIoServiceFromModule(Module, &Service, ServiceBuffer, sizeof(ServiceBuffer));
            Path.Buffer = PathBuffer;
            Path.Length = 0;
            Path.MaximumLength = sizeof(PathBuffer);
            RtlAppendUnicodeToString(&Path, QzServicesPath);
            RtlAppendUnicodeStringToString(&Path, &Service);

            DbgPrint(QZ_KERNEL_NAME ": DllInitialize %wZ\n", &Module->BaseDllName);
            Status = DllInitialize(&Path);
            if (!NT_SUCCESS(Status))
                DbgPrint(QZ_KERNEL_NAME ": DllInitialize %wZ failed: 0x%08lx\n",
                         &Module->BaseDllName, Status);
        }
    }
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* The import gate: a driver bound to export stubs is not started          */
/* ---------------------------------------------------------------------- */

/*
 * Count the kernel imports of an image (and of the export drivers it
 * pulls in) that qzkrnl only has stubs for. Calling a stub stops the
 * system, so such a driver is refused up front with a clear message.
 */
static PUCHAR QzKernelBase;
static ULONG QzKernelSize;

/* The name of the kernel export at Address (an implemented alias if there is one) */
static PCSTR
QzKernelExportNameAt(PVOID Address)
{
    PIMAGE_NT_HEADERS Nt = (PIMAGE_NT_HEADERS)(QzKernelBase + ((PIMAGE_DOS_HEADER)QzKernelBase)->e_lfanew);
    PIMAGE_EXPORT_DIRECTORY Exports = (PIMAGE_EXPORT_DIRECTORY)(QzKernelBase +
        Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    PULONG Names = (PULONG)(QzKernelBase + Exports->AddressOfNames);
    PULONG Functions = (PULONG)(QzKernelBase + Exports->AddressOfFunctions);
    PUSHORT Ordinals = (PUSHORT)(QzKernelBase + Exports->AddressOfNameOrdinals);
    PCSTR Found = NULL;
    ULONG i;

    for (i = 0; i < Exports->NumberOfNames; i++)
    {
        if (QzKernelBase + Functions[Ordinals[i]] != (PUCHAR)Address)
            continue;
        Found = (PCSTR)(QzKernelBase + Names[i]);
        if (QzIsExportImplemented(Found))
            break;
    }
    return Found;
}

ULONG
QzIoCheckImports(PVOID ImageBase, PCSTR *FirstMissing, ULONG Depth)
{
    PUCHAR Base = ImageBase;
    PIMAGE_NT_HEADERS Nt = (PIMAGE_NT_HEADERS)(Base + ((PIMAGE_DOS_HEADER)Base)->e_lfanew);
    PIMAGE_DATA_DIRECTORY Directory = &Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    PIMAGE_IMPORT_DESCRIPTOR Import;
    ULONG Missing = 0;

    if (!Directory->Size || Depth > 8)
        return 0;
    if (!QzKernelBase)
    {
        PQZ_LDR_ENTRY Kernel = CONTAINING_RECORD(QzLoaderBlock->LoadOrderListHead.Flink, QZ_LDR_ENTRY,
                                                 InLoadOrderLinks);
        QzKernelBase = Kernel->DllBase;
        QzKernelSize = Kernel->SizeOfImage;
    }
    for (Import = (PIMAGE_IMPORT_DESCRIPTOR)(Base + Directory->VirtualAddress); Import->Name; Import++)
    {
        PCSTR DllName = (PCSTR)(Base + Import->Name);
        PIMAGE_THUNK_DATA Thunk;

        if (!_stricmp(DllName, "ntoskrnl.exe"))
        {
            if (!Import->OriginalFirstThunk)
                continue;
            for (Thunk = (PIMAGE_THUNK_DATA)(Base + Import->OriginalFirstThunk); Thunk->u1.AddressOfData; Thunk++)
            {
                PCSTR Name;
                if (Thunk->u1.AddressOfData >= (ULONG_PTR)Base &&
                    Thunk->u1.AddressOfData < (ULONG_PTR)Base + Nt->OptionalHeader.SizeOfImage)
                {
                    /* The loader turned this name table's RVAs into addresses
                       (it does for export drivers): a pointer to the name */
                    Name = (PCSTR)((PIMAGE_IMPORT_BY_NAME)Thunk->u1.AddressOfData)->Name;
                }
                else if (Thunk->u1.AddressOfData >= (ULONG_PTR)QzKernelBase &&
                         Thunk->u1.AddressOfData < (ULONG_PTR)QzKernelBase + QzKernelSize)
                {
                    /* ... or bound it in place: the kernel address itself */
                    Name = QzKernelExportNameAt((PVOID)Thunk->u1.AddressOfData);
                    if (!Name)
                        continue;
                }
                else if (IMAGE_SNAP_BY_ORDINAL(Thunk->u1.Ordinal))
                {
                    continue;
                }
                else
                {
                    Name = (PCSTR)((PIMAGE_IMPORT_BY_NAME)(Base + Thunk->u1.AddressOfData))->Name;
                }
                if (!QzIsExportImplemented(Name))
                {
                    if (!Missing && FirstMissing)
                        *FirstMissing = Name;
                    Missing++;
                }
            }
        }
        else if (!QzIoIsCoreModule(DllName))
        {
            PQZ_LDR_ENTRY Module = QzIoFindModuleA(DllName);
            if (Module)
            {
                PCSTR DllMissing = NULL;
                ULONG Count = QzIoCheckImports(Module->DllBase, &DllMissing, Depth + 1);
                if (Count && !Missing && FirstMissing)
                    *FirstMissing = DllMissing;
                Missing += Count;
            }
        }
    }
    return Missing;
}

/* ---------------------------------------------------------------------- */
/* Boot drivers                                                            */
/* ---------------------------------------------------------------------- */

static PBOOT_DRIVER_LIST_ENTRY
QzIoFindBootDriver(PCUNICODE_STRING ServiceName)
{
    PLIST_ENTRY Entry;

    for (Entry = QzLoaderBlock->BootDriverListHead.Flink;
         Entry != &QzLoaderBlock->BootDriverListHead;
         Entry = Entry->Flink)
    {
        PBOOT_DRIVER_LIST_ENTRY Driver = CONTAINING_RECORD(Entry, BOOT_DRIVER_LIST_ENTRY, Link);
        UNICODE_STRING Last = Driver->RegistryPath;
        USHORT i = Last.Length / sizeof(WCHAR);

        while (i && Last.Buffer[i - 1] != L'\\')
            i--;
        Last.Buffer += i;
        Last.Length -= i * sizeof(WCHAR);
        Last.MaximumLength = Last.Length;
        if (RtlEqualUnicodeString(&Last, ServiceName, TRUE))
            return Driver;
    }
    return NULL;
}

/* What a service key says about loading its driver */
typedef struct _QZ_SERVICE_INFO
{
    ULONG Type;                         /* SERVICE_KERNEL_DRIVER, SERVICE_FILE_SYSTEM_DRIVER, ... */
    ULONG Start;
    WCHAR ImagePath[160];               /* an NT path */
} QZ_SERVICE_INFO, *PQZ_SERVICE_INFO;

static NTSTATUS
QzIoQueryService(PCUNICODE_STRING ServiceName, PQZ_SERVICE_INFO Info)
{
    UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 160 * sizeof(WCHAR)];
    PKEY_VALUE_PARTIAL_INFORMATION Value = (PVOID)Buffer;
    UNICODE_STRING ValueName, Path, Raw;
    OBJECT_ATTRIBUTES Attributes;
    WCHAR KeyBuffer[160];
    UNICODE_STRING KeyName;
    HANDLE Key;
    ULONG Length;
    NTSTATUS Status;

    KeyName.Buffer = KeyBuffer;
    KeyName.Length = 0;
    KeyName.MaximumLength = sizeof(KeyBuffer);
    RtlAppendUnicodeToString(&KeyName, QzServicesPath);
    RtlAppendUnicodeStringToString(&KeyName, ServiceName);
    InitializeObjectAttributes(&Attributes, &KeyName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenKey(&Key, KEY_READ, &Attributes);
    if (!NT_SUCCESS(Status))
        return Status;

    Info->Type = SERVICE_KERNEL_DRIVER;
    Info->Start = SERVICE_DEMAND_START;
    RtlInitUnicodeString(&ValueName, L"Type");
    if (NT_SUCCESS(ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Value, sizeof(Buffer), &Length)) &&
        Value->Type == REG_DWORD)
        Info->Type = *(PULONG)Value->Data;
    RtlInitUnicodeString(&ValueName, L"Start");
    if (NT_SUCCESS(ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Value, sizeof(Buffer), &Length)) &&
        Value->Type == REG_DWORD)
        Info->Start = *(PULONG)Value->Data;

    Path.Buffer = Info->ImagePath;
    Path.Length = 0;
    Path.MaximumLength = sizeof(Info->ImagePath);
    RtlInitUnicodeString(&ValueName, L"ImagePath");
    Raw.Length = 0;
    if (NT_SUCCESS(ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Value,
                                   sizeof(Buffer) - sizeof(WCHAR), &Length)) &&
        (Value->Type == REG_SZ || Value->Type == REG_EXPAND_SZ) && Value->DataLength > sizeof(WCHAR))
    {
        Raw.Buffer = (PWCHAR)Value->Data;
        Raw.Length = (USHORT)Value->DataLength;
        while (Raw.Length && !Raw.Buffer[Raw.Length / sizeof(WCHAR) - 1])
            Raw.Length -= sizeof(WCHAR);
        Raw.MaximumLength = Raw.Length;
    }
    ZwClose(Key);

    if (!Raw.Length)
    {
        /* The default: System32\drivers\<service>.sys */
        RtlAppendUnicodeToString(&Path, L"\\SystemRoot\\System32\\drivers\\");
        RtlAppendUnicodeStringToString(&Path, ServiceName);
        RtlAppendUnicodeToString(&Path, L".sys");
    }
    else
    {
        static const WCHAR Expand[] = L"%SystemRoot%\\";
        UNICODE_STRING Prefix;
        RtlInitUnicodeString(&Prefix, Expand);
        if (RtlPrefixUnicodeString(&Prefix, &Raw, TRUE))
        {
            Raw.Buffer += Prefix.Length / sizeof(WCHAR);
            Raw.Length -= Prefix.Length;
            RtlAppendUnicodeToString(&Path, L"\\SystemRoot\\");
        }
        else if (Raw.Buffer[0] != L'\\')
        {
            /* "system32\drivers\x.sys" is relative to the system root */
            RtlAppendUnicodeToString(&Path, L"\\SystemRoot\\");
        }
        if (!NT_SUCCESS(RtlAppendUnicodeStringToString(&Path, &Raw)))
            return STATUS_NAME_TOO_LONG;
    }
    Info->ImagePath[Path.Length / sizeof(WCHAR)] = UNICODE_NULL;
    return STATUS_SUCCESS;
}

/* Create the driver object for a loaded image and run DriverEntry */
static NTSTATUS
QzIoStartDriver(PCUNICODE_STRING ServiceName, PQZ_LDR_ENTRY Module, PUNICODE_STRING RegistryPath,
                BOOLEAN FileSystem, PDRIVER_OBJECT *DriverObject)
{
    PIMAGE_NT_HEADERS Nt;
    PDRIVER_OBJECT Driver;
    WCHAR NameBuffer[64];
    UNICODE_STRING Name;
    NTSTATUS Status;

    Nt = (PIMAGE_NT_HEADERS)((PUCHAR)Module->DllBase + ((PIMAGE_DOS_HEADER)Module->DllBase)->e_lfanew);
    Name.Buffer = NameBuffer;
    Name.Length = 0;
    Name.MaximumLength = sizeof(NameBuffer);
    RtlAppendUnicodeToString(&Name, FileSystem ? L"\\FileSystem\\" : L"\\Driver\\");
    RtlAppendUnicodeStringToString(&Name, ServiceName);

    Status = QzIoCreateDriverObject(&Name, ServiceName, Module->DllBase, Module->SizeOfImage,
                                    (PDRIVER_INITIALIZE)((PUCHAR)Module->DllBase +
                                                         Nt->OptionalHeader.AddressOfEntryPoint),
                                    &Driver);
    if (!NT_SUCCESS(Status))
    {
        if (QzMmIsDiskImage(Module))
            QzMmUnloadSystemImage(Module);
        return Status;
    }
    Driver->DriverSection = Module;   /* the driver object owns a disk image from here on */

    {
        PCSTR FirstMissing = NULL;
        ULONG Missing = QzIoCheckImports(Module->DllBase, &FirstMissing, 0);
        if (Missing)
        {
            DbgPrint(QZ_KERNEL_NAME ": not starting %wZ: it imports %lu export(s) qzkrnl does not "
                     "implement yet (%s, ...)\n", ServiceName, Missing, FirstMissing);
            if (QzMmIsDiskImage(Module))
            {
                /* Nothing of it ran: give the image back now */
                Driver->DriverSection = NULL;
                Driver->DriverInit = NULL;
                Driver->DriverStart = NULL;
                QzMmUnloadSystemImage(Module);
            }
            QzIoFinishDriverEntry(Driver, STATUS_PROCEDURE_NOT_FOUND);
            *DriverObject = Driver;
            return STATUS_PROCEDURE_NOT_FOUND;
        }
    }

    Status = QzIoInitializeImports(Module->DllBase, 0);
    if (NT_SUCCESS(Status))
    {
        DbgPrint(QZ_KERNEL_NAME ": DriverEntry %wZ\n", &Name);
        Status = Driver->DriverInit(Driver, RegistryPath);
        DbgPrint(QZ_KERNEL_NAME ": DriverEntry %wZ -> 0x%08lx\n", &Name, Status);
    }
    QzIoFinishDriverEntry(Driver, Status);
    *DriverObject = Driver;
    return Status;
}

/*
 * The file system drivers FreeLoader loaded (cdfs for a CD boot): NT
 * starts every boot driver before it mounts the boot volume; these are
 * the ones that must be there by then, to register and get the first try
 * at it.
 */
VOID
QzIoStartBootFileSystems(VOID)
{
    PLIST_ENTRY Entry;

    for (Entry = QzLoaderBlock->BootDriverListHead.Flink; Entry != &QzLoaderBlock->BootDriverListHead;
         Entry = Entry->Flink)
    {
        PBOOT_DRIVER_LIST_ENTRY Boot = CONTAINING_RECORD(Entry, BOOT_DRIVER_LIST_ENTRY, Link);
        UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
        PKEY_VALUE_PARTIAL_INFORMATION Value = (PVOID)Buffer;
        UNICODE_STRING ValueName = RTL_CONSTANT_STRING(L"Type"), Service;
        OBJECT_ATTRIBUTES Attributes;
        PDRIVER_OBJECT Driver;
        HANDLE Key;
        ULONG Length;
        USHORT i;
        NTSTATUS Status;

        if (!Boot->LdrEntry)
            continue;
        InitializeObjectAttributes(&Attributes, &Boot->RegistryPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                   NULL, NULL);
        if (!NT_SUCCESS(ZwOpenKey(&Key, KEY_READ, &Attributes)))
            continue;
        Status = ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Value, sizeof(Buffer), &Length);
        ZwClose(Key);
        if (!NT_SUCCESS(Status) || Value->Type != REG_DWORD ||
            !(*(PULONG)Value->Data & (SERVICE_FILE_SYSTEM_DRIVER | SERVICE_RECOGNIZER_DRIVER)))
            continue;

        Service = Boot->RegistryPath;
        for (i = Service.Length / sizeof(WCHAR); i && Service.Buffer[i - 1] != L'\\'; i--)
            ;
        Service.Buffer += i;
        Service.Length -= i * sizeof(WCHAR);
        Service.MaximumLength = Service.Length;
        Status = QzIoLoadDriver(&Service, &Driver);
        DbgPrint(QZ_KERNEL_NAME ": boot file system %wZ: 0x%08lx\n", &Service, Status);
    }
}

/*
 * System-start file systems (fs_rec, npfs, msfs): NT starts every
 * system-start driver once the boot volume is up. For now only file
 * systems and recognizers are started: the rest (i8042prt, beep, ...)
 * would compete with qzkrnl's own console drivers.
 */
VOID
QzIoStartSystemFileSystems(VOID)
{
    UNICODE_STRING Root = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\System\\CurrentControlSet\\Services");
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Services;
    ULONG Index;

    InitializeObjectAttributes(&Attributes, &Root, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&Services, KEY_READ, &Attributes)))
        return;
    for (Index = 0;; Index++)
    {
        UCHAR Buffer[sizeof(KEY_BASIC_INFORMATION) + 64 * sizeof(WCHAR)];
        PKEY_BASIC_INFORMATION Info = (PVOID)Buffer;
        UNICODE_STRING Name;
        QZ_SERVICE_INFO Service;
        PDRIVER_OBJECT Driver;
        ULONG Length;
        NTSTATUS Status = ZwEnumerateKey(Services, Index, KeyBasicInformation, Info, sizeof(Buffer), &Length);

        if (Status == STATUS_NO_MORE_ENTRIES)
            break;
        if (!NT_SUCCESS(Status))
            continue;
        Name.Buffer = Info->Name;
        Name.Length = Name.MaximumLength = (USHORT)Info->NameLength;
        if (!NT_SUCCESS(QzIoQueryService(&Name, &Service)) || Service.Start != SERVICE_SYSTEM_START ||
            !(Service.Type & (SERVICE_FILE_SYSTEM_DRIVER | SERVICE_RECOGNIZER_DRIVER)) || QzIoFindDriver(&Name))
            continue;
        Status = QzIoLoadDriver(&Name, &Driver);
        DbgPrint(QZ_KERNEL_NAME ": system file system %wZ: 0x%08lx\n", &Name, Status);
    }
    ZwClose(Services);
}

/* A service whose driver needs no file system: FreeLoader loaded it, or it runs */
BOOLEAN
QzIoIsBootService(PCUNICODE_STRING ServiceName)
{
    PBOOT_DRIVER_LIST_ENTRY Boot = QzIoFindBootDriver(ServiceName);
    return (Boot && Boot->LdrEntry) || QzIoFindDriver(ServiceName) != NULL;
}

/*
 * Start the driver of a service: a boot driver FreeLoader loaded, or the
 * image its service key names, loaded from disk. A driver is started once;
 * later calls return its driver object and DriverEntry's status.
 */
NTSTATUS
QzIoLoadDriver(PCUNICODE_STRING ServiceName, PDRIVER_OBJECT *DriverObject)
{
    PBOOT_DRIVER_LIST_ENTRY Boot;
    PDRIVER_OBJECT Driver;
    QZ_SERVICE_INFO Info;
    UNICODE_STRING Path, RegistryPath;
    WCHAR RegistryBuffer[160];
    PVOID Module;
    NTSTATUS Status;

    *DriverObject = NULL;
    Driver = QzIoFindDriver(ServiceName);
    if (Driver)
    {
        *DriverObject = Driver;
        return QzDriverExtension(Driver)->EntryStatus;
    }

    Boot = QzIoFindBootDriver(ServiceName);
    if (Boot && Boot->LdrEntry)
    {
        BOOLEAN FileSystem = NT_SUCCESS(QzIoQueryService(ServiceName, &Info)) &&
                             (Info.Type & (SERVICE_FILE_SYSTEM_DRIVER | SERVICE_RECOGNIZER_DRIVER));
        return QzIoStartDriver(ServiceName, (PQZ_LDR_ENTRY)Boot->LdrEntry, &Boot->RegistryPath, FileSystem,
                               DriverObject);
    }

    Status = QzIoQueryService(ServiceName, &Info);
    if (!NT_SUCCESS(Status))
        return Status;   /* no such service */
    if (Info.Start == SERVICE_DISABLED)
        return STATUS_DRIVER_UNABLE_TO_LOAD;
    if (!(Info.Type & (SERVICE_KERNEL_DRIVER | SERVICE_FILE_SYSTEM_DRIVER | SERVICE_RECOGNIZER_DRIVER)))
        return STATUS_INVALID_IMAGE_FORMAT;   /* a Win32 service */

    RtlInitUnicodeString(&Path, Info.ImagePath);
    Status = QzMmLoadSystemImage(&Path, &Module);
    if (Status == STATUS_IMAGE_ALREADY_LOADED)
    {
        /* Loaded as some other image's import; the driver now holds it too */
        Status = STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(QZ_KERNEL_NAME ": cannot load %wZ (%ls): 0x%08lx\n", ServiceName, Info.ImagePath, Status);
        return Status;
    }

    RegistryPath.Buffer = RegistryBuffer;
    RegistryPath.Length = 0;
    RegistryPath.MaximumLength = sizeof(RegistryBuffer);
    RtlAppendUnicodeToString(&RegistryPath, QzServicesPath);
    RtlAppendUnicodeStringToString(&RegistryPath, ServiceName);
    return QzIoStartDriver(ServiceName, Module, &RegistryPath,
                           (Info.Type & (SERVICE_FILE_SYSTEM_DRIVER | SERVICE_RECOGNIZER_DRIVER)) != 0,
                           DriverObject);
}

/* DriverUnload, then the driver object goes, and its image with it */
NTSTATUS
QzIoUnloadDriver(PCUNICODE_STRING ServiceName)
{
    PDRIVER_OBJECT Driver = QzIoFindDriver(ServiceName);
    PQZ_DRIVER_EXTENSION Extension;

    if (!Driver)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    Extension = QzDriverExtension(Driver);
    if (NT_SUCCESS(Extension->EntryStatus) || !Extension->EntryCalled)
    {
        /* A running driver unloads only if it can, and has no PnP devices */
        if (!Driver->DriverUnload || (Driver->DriverExtension->AddDevice && Driver->DeviceObject))
            return STATUS_INVALID_DEVICE_REQUEST;
        if (!QzMmIsDiskImage(Driver->DriverSection))
            return STATUS_INVALID_DEVICE_REQUEST;   /* boot images stay */
        DbgPrint(QZ_KERNEL_NAME ": DriverUnload %wZ\n", &Driver->DriverName);
        Driver->DriverUnload(Driver);
    }
    RemoveEntryList(&Extension->Link);
    Extension->Link.Flink = NULL;
    ObMakeTemporaryObject(Driver);
    ObDereferenceObject(Driver);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Driver object extensions, reinitialization                              */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
IoAllocateDriverObjectExtension(IN PDRIVER_OBJECT DriverObject,
                                IN PVOID ClientIdentificationAddress,
                                IN ULONG DriverObjectExtensionSize,
                                OUT PVOID *DriverObjectExtension)
{
    PQZ_DRIVER_EXTENSION Extension = QzDriverExtension(DriverObject);
    PIO_CLIENT_EXTENSION Client;

    *DriverObjectExtension = NULL;
    for (Client = Extension->Public.ClientDriverExtension; Client; Client = Client->NextExtension)
    {
        if (Client->ClientIdentificationAddress == ClientIdentificationAddress)
            return STATUS_OBJECT_NAME_COLLISION;
    }

    Client = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Client) + DriverObjectExtensionSize,
                                   QZ_IO_TAG);
    if (!Client)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Client, sizeof(*Client) + DriverObjectExtensionSize);
    Client->ClientIdentificationAddress = ClientIdentificationAddress;
    Client->NextExtension = Extension->Public.ClientDriverExtension;
    Extension->Public.ClientDriverExtension = Client;
    *DriverObjectExtension = Client + 1;
    return STATUS_SUCCESS;
}

PVOID
NTAPI
IoGetDriverObjectExtension(IN PDRIVER_OBJECT DriverObject,
                           IN PVOID ClientIdentificationAddress)
{
    PIO_CLIENT_EXTENSION Client;

    for (Client = QzDriverExtension(DriverObject)->Public.ClientDriverExtension; Client;
         Client = Client->NextExtension)
    {
        if (Client->ClientIdentificationAddress == ClientIdentificationAddress)
            return Client + 1;
    }
    return NULL;
}

static VOID
QzIoQueueReinit(PDRIVER_OBJECT Driver, PDRIVER_REINITIALIZE Routine, PVOID Context, BOOLEAN Boot)
{
    PQZ_REINIT Reinit = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Reinit), QZ_IO_TAG);
    if (!Reinit)
        return;
    Reinit->DriverObject = Driver;
    Reinit->Routine = Routine;
    Reinit->Context = Context;
    Reinit->Count = 0;
    InsertTailList(Boot ? &QzDriverExtension(Driver)->BootReinitList
                        : &QzDriverExtension(Driver)->ReinitList, &Reinit->Link);
    Driver->Flags |= Boot ? DRVO_BOOTREINIT_REGISTERED : DRVO_REINIT_REGISTERED;
}

VOID
NTAPI
IoRegisterDriverReinitialization(IN PDRIVER_OBJECT DriverObject,
                                 IN PDRIVER_REINITIALIZE DriverReinitializationRoutine,
                                 IN PVOID Context)
{
    QzIoQueueReinit(DriverObject, DriverReinitializationRoutine, Context, FALSE);
}

VOID
NTAPI
IoRegisterBootDriverReinitialization(IN PDRIVER_OBJECT DriverObject,
                                     IN PDRIVER_REINITIALIZE DriverReinitializationRoutine,
                                     IN PVOID Context)
{
    QzIoQueueReinit(DriverObject, DriverReinitializationRoutine, Context, TRUE);
}

/* Each routine runs once per call; it may register itself again */
VOID
QzIoCallReinitRoutines(BOOLEAN Boot)
{
    PLIST_ENTRY Entry;

    for (Entry = QzIoDriverList.Flink; Entry != &QzIoDriverList; Entry = Entry->Flink)
    {
        PQZ_DRIVER_EXTENSION Extension = CONTAINING_RECORD(Entry, QZ_DRIVER_EXTENSION, Link);
        PLIST_ENTRY Head = Boot ? &Extension->BootReinitList : &Extension->ReinitList;
        LIST_ENTRY Pending;

        if (IsListEmpty(Head))
            continue;
        /* Detach the current list so re-registrations wait for the next pass */
        Pending = *Head;
        Pending.Flink->Blink = &Pending;
        Pending.Blink->Flink = &Pending;
        InitializeListHead(Head);
        while (!IsListEmpty(&Pending))
        {
            PQZ_REINIT Reinit = CONTAINING_RECORD(RemoveHeadList(&Pending), QZ_REINIT, Link);
            Reinit->Routine(Reinit->DriverObject, Reinit->Context, ++Reinit->Count);
            ExFreePoolWithTag(Reinit, QZ_IO_TAG);
        }
    }
}

VOID
QzIoDumpDrivers(VOID)
{
    PLIST_ENTRY Entry;
    ULONG Count = 0;

    for (Entry = QzIoDriverList.Flink; Entry != &QzIoDriverList; Entry = Entry->Flink)
    {
        PQZ_DRIVER_EXTENSION Extension = CONTAINING_RECORD(Entry, QZ_DRIVER_EXTENSION, Link);
        PDRIVER_OBJECT Driver = Extension->Public.DriverObject;
        PDEVICE_OBJECT Device;
        ULONG Devices = 0;

        for (Device = Driver->DeviceObject; Device; Device = Device->NextDevice)
            Devices++;
        QzPrint("  %-28wZ %-9s %2lu device(s)%s%s\n", &Driver->DriverName,
                !Extension->EntryCalled ? "loading" :
                NT_SUCCESS(Extension->EntryStatus) ? "started" : "failed",
                Devices, Extension->Public.AddDevice ? ", PnP" : "",
                Extension->ImageBase ? "" : ", built-in");
        Count++;
    }
    QzPrint("  %lu driver object(s)\n", Count);
}

VOID
QzIoInit(VOID)
{
    InitializeListHead(&QzIoDriverList);
    QzMmInitSystemImages();
    QzCcInit();

    /* Give the I/O object types (created by the object manager) their
       procedures */
    IoDeviceObjectType->TypeInfo.ParseProcedure = QzIoParseDevice;
    IoDeviceObjectType->TypeInfo.DeleteProcedure = QzIoDeleteDevice;
    IoFileObjectType->TypeInfo.CloseProcedure = QzIoCloseFile;
    IoFileObjectType->TypeInfo.DeleteProcedure = QzIoDeleteFile;
    IoFileObjectType->TypeInfo.ParseProcedure = QzIoParseFile;
    IoFileObjectType->TypeInfo.UseDefaultObject = FALSE;
    IoFileObjectType->DefaultObject = NULL;   /* waits use FILE_OBJECT.Event */
    IoDriverObjectType->TypeInfo.DeleteProcedure = QzIoDeleteDriver;
}

/* The shell's "drivers": every boot driver, started or not, and why not */
VOID
QzIoDumpBootDrivers(VOID)
{
    PLIST_ENTRY Entry;
    ULONG Count = 0, Started = 0, Blocked = 0;

    for (Entry = QzLoaderBlock->BootDriverListHead.Flink;
         Entry != &QzLoaderBlock->BootDriverListHead;
         Entry = Entry->Flink)
    {
        PBOOT_DRIVER_LIST_ENTRY Boot = CONTAINING_RECORD(Entry, BOOT_DRIVER_LIST_ENTRY, Link);
        PQZ_LDR_ENTRY Module = (PQZ_LDR_ENTRY)Boot->LdrEntry;
        UNICODE_STRING Service = Boot->RegistryPath;
        PDRIVER_OBJECT Driver;
        PCSTR FirstMissing = NULL;
        ULONG Missing;
        USHORT i = Service.Length / sizeof(WCHAR);

        while (i && Service.Buffer[i - 1] != L'\\')
            i--;
        Service.Buffer += i;
        Service.Length -= i * sizeof(WCHAR);
        Count++;

        Driver = QzIoFindDriver(&Service);
        Missing = Module ? QzIoCheckImports(Module->DllBase, &FirstMissing, 0) : 0;
        if (Driver && QzDriverExtension(Driver)->EntryCalled && NT_SUCCESS(QzDriverExtension(Driver)->EntryStatus))
        {
            Started++;
            QzPrint("  %-12wZ started%s\n", &Service, Driver->DriverExtension->AddDevice ? " (PnP)" : "");
        }
        else if (Missing)
        {
            Blocked++;
            QzPrint("  %-12wZ needs %lu more kernel export(s), e.g. %s\n", &Service, Missing, FirstMissing);
        }
        else if (Driver)
        {
            QzPrint("  %-12wZ DriverEntry failed (0x%08lx)\n", &Service, QzDriverExtension(Driver)->EntryStatus);
        }
        else
        {
            QzPrint("  %-12wZ ready (starts when a device needs it)\n", &Service);
        }
    }
    QzPrint("  %lu boot drivers: %lu started, %lu waiting for kernel exports\n", Count, Started, Blocked);
}
