/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Plug and Play manager: device tree, enumeration, driver
 *              matching, resource assignment, device interfaces and
 *              notifications, device properties, power IRP helpers
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * The tree starts at the root device of "\Driver\PnpManager". The HAL's
 * PnP driver (started through HalInitPnpDriver) reports itself with
 * IoReportDetectedDevice and enumerates the PCI root bus below that; from
 * there each node goes through the usual NT sequence:
 *   query IDs and capabilities -> find the service (Enum key, then
 *   CriticalDeviceDatabase) -> start the driver -> AddDevice (with class
 *   filters) -> resources -> IRP_MN_START_DEVICE -> bus relations.
 * Resources are taken from the device's requirements (preferring fixed
 * and preferred descriptors) or its boot configuration and translated
 * through the HAL; there is no arbitration between devices yet.
 */

#include "qzio.h"
#include "qzps.h"
#include <initguid.h>
#include <wdmguid.h>
#include <mountmgr.h>

VOID QzIsoInit(VOID);   /* io/isofs.c */
unsigned long __cdecl strtoul(const char *s, char **End, int Base);
char * __cdecl strstr(const char *Haystack, const char *Needle);
int __cdecl _strnicmp(const char *A, const char *B, size_t Count);

#define QZ_DN_REPORTED     0x0001
#define QZ_DN_IDS          0x0002
#define QZ_DN_ADDED        0x0004
#define QZ_DN_STARTED      0x0008
#define QZ_DN_FAILED       0x0010
#define QZ_DN_NO_DRIVER    0x0020
#define QZ_DN_RAW          0x0040
#define QZ_DN_REENUMERATE  0x0080
#define QZ_DN_RECORDED     0x0100
#define QZ_DN_DEFERRED     0x0200   /* its driver is on disk: after the boot volume */

/* Enum\...\Capabilities bits (cfgmgr32.h's CM_DEVCAP_*) */
#define CM_DEVCAP_LOCKSUPPORTED      0x0001
#define CM_DEVCAP_EJECTSUPPORTED     0x0002
#define CM_DEVCAP_REMOVABLE          0x0004
#define CM_DEVCAP_DOCKDEVICE         0x0008
#define CM_DEVCAP_UNIQUEID           0x0010
#define CM_DEVCAP_SILENTINSTALL      0x0020
#define CM_DEVCAP_RAWDEVICEOK        0x0040
#define CM_DEVCAP_SURPRISEREMOVALOK  0x0080

typedef struct _QZ_DEVICE_NODE
{
    struct _QZ_DEVICE_NODE *Parent, *Child, *Sibling;
    PDEVICE_OBJECT Pdo;
    ULONG Flags;
    NTSTATUS Problem;
    PCSTR ProblemText;
    ULONG Level;
    UNICODE_STRING InstancePath;
    UNICODE_STRING ServiceName;
    PWSTR DeviceId;
    PWSTR HardwareIds;                /* MULTI_SZ */
    PWSTR CompatibleIds;              /* MULTI_SZ */
    PWSTR Description;
    PWSTR Location;
    WCHAR ClassGuid[40];
    WCHAR DriverKey[48];              /* "{class guid}\NNNN" below Control\Class */
    PCM_RESOURCE_LIST BootResources;
    PIO_RESOURCE_REQUIREMENTS_LIST Requirements;  /* from the PDO, until the start */
    PCM_RESOURCE_LIST RawResources;
    PCM_RESOURCE_LIST TranslatedResources;
    DEVICE_CAPABILITIES Capabilities;
    PNP_BUS_INFORMATION BusInformation;
    BOOLEAN HasBusInformation;
    INTERFACE_TYPE ReportedBusType;
    ULONG ReportedBusNumber;
} QZ_DEVICE_NODE, *PQZ_DEVICE_NODE;

typedef struct _QZ_INTERFACE
{
    LIST_ENTRY Link;
    GUID Guid;
    PDEVICE_OBJECT Pdo;
    UNICODE_STRING Name;              /* "\??\...#{guid}[\ref]" */
    USHORT LinkLength;                /* bytes of Name that form the link object */
    BOOLEAN Enabled;
} QZ_INTERFACE, *PQZ_INTERFACE;

typedef struct _QZ_NOTIFY
{
    LIST_ENTRY Link;
    IO_NOTIFICATION_EVENT_CATEGORY Category;
    GUID Guid;
    PVOID Target;                     /* file object for target device change */
    PDRIVER_OBJECT DriverObject;
    PDRIVER_NOTIFICATION_CALLBACK_ROUTINE Callback;
    PVOID Context;
    BOOLEAN Unregistered;
} QZ_NOTIFY, *PQZ_NOTIFY;

typedef struct _QZ_NOTIFY_WORK
{
    WORK_QUEUE_ITEM Item;
    GUID Event;
    GUID Guid;
    UNICODE_STRING Name;
    WCHAR Buffer[1];
} QZ_NOTIFY_WORK, *PQZ_NOTIFY_WORK;

static PDRIVER_OBJECT QzPnpRootDriver;
static PDEVICE_OBJECT QzPnpRootDevice;
static PQZ_DEVICE_NODE QzPnpRoot;
static KMUTEX QzPnpLock;
static LIST_ENTRY QzPnpInterfaces = { &QzPnpInterfaces, &QzPnpInterfaces };
static LIST_ENTRY QzPnpNotifications = { &QzPnpNotifications, &QzPnpNotifications };
static FAST_MUTEX QzPnpListLock;
static ULONG QzPnpNodeCount, QzPnpStartedCount;
static BOOLEAN QzPnpStarted;
static BOOLEAN QzPnpBootPhase = TRUE;   /* only boot drivers until \SystemRoot is mounted */

static const WCHAR QzControlSet[] = L"\\Registry\\Machine\\System\\CurrentControlSet\\";

NTSTATUS QzCmReadValue(PCWSTR KeyPath, PCWSTR ValueName, PVOID Buffer, ULONG Length,
                       PULONG Type, PULONG DataLength);

#define QzNodeOf(Device) ((PQZ_DEVICE_NODE)QzDeviceExtension(Device)->DeviceNode)

static VOID QzPnpQueueInstallEvent(PQZ_DEVICE_NODE Node);

static VOID
QzPnpLockTree(VOID)
{
    KeWaitForSingleObject(&QzPnpLock, Executive, KernelMode, FALSE, NULL);
}

static VOID
QzPnpUnlockTree(VOID)
{
    KeReleaseMutex(&QzPnpLock, FALSE);
}

/* ---------------------------------------------------------------------- */
/* Strings                                                                 */
/* ---------------------------------------------------------------------- */

static PWSTR
QzPnpDuplicate(PCWSTR Source, ULONG Bytes)
{
    PWSTR Copy = ExAllocatePoolWithTag(NonPagedPool, Bytes + 2 * sizeof(WCHAR), QZ_PNP_TAG);
    if (Copy)
    {
        RtlCopyMemory(Copy, Source, Bytes);
        Copy[Bytes / sizeof(WCHAR)] = UNICODE_NULL;
        Copy[Bytes / sizeof(WCHAR) + 1] = UNICODE_NULL;
    }
    return Copy;
}

static ULONG
QzPnpMultiSzBytes(PCWSTR List)
{
    PCWSTR p = List;
    if (!List)
        return 0;
    while (*p)
        p += wcslen(p) + 1;
    return (ULONG)((p - List) + 1) * sizeof(WCHAR);
}

static VOID
QzPnpSetString(PUNICODE_STRING String, PCWSTR Text)
{
    ULONG Bytes = (ULONG)wcslen(Text) * sizeof(WCHAR);
    if (String->Buffer)
        ExFreePoolWithTag(String->Buffer, QZ_PNP_TAG);
    String->Buffer = QzPnpDuplicate(Text, Bytes);
    String->Length = String->Buffer ? (USHORT)Bytes : 0;
    String->MaximumLength = String->Buffer ? (USHORT)(Bytes + sizeof(WCHAR)) : 0;
}

/* ---------------------------------------------------------------------- */
/* Sending PnP IRPs to the top of a device's stack                          */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzPnpSend(PDEVICE_OBJECT Pdo, UCHAR Minor, PIO_STACK_LOCATION Parameters, PULONG_PTR Information)
{
    PDEVICE_OBJECT Top = IoGetAttachedDeviceReference(Pdo);
    IO_STATUS_BLOCK IoStatus;
    PIO_STACK_LOCATION Stack;
    NTSTATUS Status;
    KEVENT Event;
    PIRP Irp;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, Top, NULL, 0, NULL, &Event, &IoStatus);
    if (!Irp)
    {
        ObDereferenceObject(Top);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    /* The PnP contract: unhandled requests come back STATUS_NOT_SUPPORTED */
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = Information ? *Information : 0;
    Stack = IoGetNextIrpStackLocation(Irp);
    if (Parameters)
        Stack->Parameters = Parameters->Parameters;
    Stack->MajorFunction = IRP_MJ_PNP;
    Stack->MinorFunction = Minor;

    Status = IoCallDriver(Top, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }
    else
    {
        Status = IoStatus.Status;
    }
    if (Information)
        *Information = NT_SUCCESS(Status) ? IoStatus.Information : 0;
    ObDereferenceObject(Top);
    return Status;
}

static PWSTR
QzPnpQueryId(PDEVICE_OBJECT Pdo, BUS_QUERY_ID_TYPE Type)
{
    IO_STACK_LOCATION Stack;
    ULONG_PTR Information = 0;

    RtlZeroMemory(&Stack, sizeof(Stack));
    Stack.Parameters.QueryId.IdType = Type;
    if (!NT_SUCCESS(QzPnpSend(Pdo, IRP_MN_QUERY_ID, &Stack, &Information)))
        return NULL;
    return (PWSTR)Information;
}

static PWSTR
QzPnpQueryText(PDEVICE_OBJECT Pdo, DEVICE_TEXT_TYPE Type)
{
    IO_STACK_LOCATION Stack;
    ULONG_PTR Information = 0;

    RtlZeroMemory(&Stack, sizeof(Stack));
    Stack.Parameters.QueryDeviceText.DeviceTextType = Type;
    Stack.Parameters.QueryDeviceText.LocaleId = 0x409;
    if (!NT_SUCCESS(QzPnpSend(Pdo, IRP_MN_QUERY_DEVICE_TEXT, &Stack, &Information)))
        return NULL;
    return (PWSTR)Information;
}

/* ---------------------------------------------------------------------- */
/* Device nodes                                                            */
/* ---------------------------------------------------------------------- */

static PQZ_DEVICE_NODE
QzPnpCreateNode(PQZ_DEVICE_NODE Parent, PDEVICE_OBJECT Pdo)
{
    PQZ_DEVICE_NODE Node = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Node), QZ_PNP_TAG);
    PQZ_DEVICE_NODE *Link;

    if (!Node)
        return NULL;
    RtlZeroMemory(Node, sizeof(*Node));
    Node->Pdo = Pdo;
    Node->Parent = Parent;
    Node->Level = Parent ? Parent->Level + 1 : 0;
    Node->ReportedBusType = InterfaceTypeUndefined;
    QzDeviceExtension(Pdo)->DeviceNode = (struct _DEVICE_NODE *)Node;
    if (Parent)
    {
        for (Link = &Parent->Child; *Link; Link = &(*Link)->Sibling)
            ;
        *Link = Node;
    }
    QzPnpNodeCount++;
    return Node;
}

static VOID
QzPnpFail(PQZ_DEVICE_NODE Node, NTSTATUS Status, PCSTR Why)
{
    Node->Flags |= QZ_DN_FAILED;
    Node->Problem = Status;
    Node->ProblemText = Why;
    DbgPrint(QZ_KERNEL_NAME ": PnP: %wZ: %s (0x%08lx)\n", &Node->InstancePath, Why, Status);
}

/* IDs, text and capabilities; builds the instance path */
static VOID
QzPnpQueryIdentity(PQZ_DEVICE_NODE Node)
{
    IO_STACK_LOCATION Stack;
    ULONG_PTR Information;
    PWSTR InstanceId;
    WCHAR Path[256];
    NTSTATUS Status;

    Node->Flags |= QZ_DN_IDS;
    Node->DeviceId = QzPnpQueryId(Node->Pdo, BusQueryDeviceID);
    InstanceId = QzPnpQueryId(Node->Pdo, BusQueryInstanceID);
    Node->HardwareIds = QzPnpQueryId(Node->Pdo, BusQueryHardwareIDs);
    Node->CompatibleIds = QzPnpQueryId(Node->Pdo, BusQueryCompatibleIDs);
    Node->Description = QzPnpQueryText(Node->Pdo, DeviceTextDescription);
    Node->Location = QzPnpQueryText(Node->Pdo, DeviceTextLocationInformation);

    RtlZeroMemory(&Node->Capabilities, sizeof(Node->Capabilities));
    Node->Capabilities.Size = sizeof(DEVICE_CAPABILITIES);
    Node->Capabilities.Version = 1;
    Node->Capabilities.Address = (ULONG)-1;
    Node->Capabilities.UINumber = (ULONG)-1;
    RtlZeroMemory(&Stack, sizeof(Stack));
    Stack.Parameters.DeviceCapabilities.Capabilities = &Node->Capabilities;
    Information = 0;
    QzPnpSend(Node->Pdo, IRP_MN_QUERY_CAPABILITIES, &Stack, &Information);

    Information = 0;
    Status = QzPnpSend(Node->Pdo, IRP_MN_QUERY_BUS_INFORMATION, NULL, &Information);
    if (NT_SUCCESS(Status) && Information)
    {
        Node->BusInformation = *(PPNP_BUS_INFORMATION)Information;
        Node->HasBusInformation = TRUE;
        ExFreePool((PVOID)Information);
    }

    /* "<device id>\<instance id>", made unique below the parent unless the
       bus guarantees uniqueness (as NT does: "level&hash&0&id") */
    if (!Node->DeviceId)
    {
        Node->DeviceId = QzPnpDuplicate(L"UNKNOWN", 7 * sizeof(WCHAR));
    }
    if (Node->Capabilities.UniqueID || !Node->Parent || !Node->Parent->InstancePath.Length)
    {
        _snwprintf(Path, RTL_NUMBER_OF(Path), L"%s\\%s", Node->DeviceId,
                   InstanceId ? InstanceId : L"0000");
    }
    else
    {
        ULONG Hash = 0, i;
        for (i = 0; i < Node->Parent->InstancePath.Length / sizeof(WCHAR); i++)
            Hash = Hash * 37 + RtlUpcaseUnicodeChar(Node->Parent->InstancePath.Buffer[i]);
        _snwprintf(Path, RTL_NUMBER_OF(Path), L"%s\\%lu&%08lx&0&%s", Node->DeviceId, Node->Level,
                   Hash, InstanceId ? InstanceId : L"0000");
    }
    Path[RTL_NUMBER_OF(Path) - 1] = UNICODE_NULL;
    QzPnpSetString(&Node->InstancePath, Path);
    if (InstanceId)
        ExFreePool(InstanceId);

    /*
     * Resources, as NT asks for them: from the PDO alone, before any driver
     * is added above it (function drivers such as videoprt complete these
     * queries themselves instead of passing them down). The instance path
     * must exist by now: a KMDF PDO registers its device interfaces when
     * it gets IRP_MN_QUERY_RESOURCE_REQUIREMENTS.
     */
    if (!(Node->Flags & QZ_DN_REPORTED))
    {
        Information = 0;
        if (NT_SUCCESS(QzPnpSend(Node->Pdo, IRP_MN_QUERY_RESOURCES, NULL, &Information)) && Information)
            Node->BootResources = (PCM_RESOURCE_LIST)Information;
        Information = 0;
        if (NT_SUCCESS(QzPnpSend(Node->Pdo, IRP_MN_QUERY_RESOURCE_REQUIREMENTS, NULL, &Information)) && Information)
            Node->Requirements = (PIO_RESOURCE_REQUIREMENTS_LIST)Information;
    }
}

/* Service and class for an ID from CriticalDeviceDatabase ("\" -> "#") */
static BOOLEAN
QzPnpLookupCriticalDevice(PQZ_DEVICE_NODE Node, PCWSTR Id, PWSTR Service, ULONG ServiceBytes)
{
    WCHAR Key[256];
    ULONG i, Length;
    PWSTR p;

    Length = _snwprintf(Key, RTL_NUMBER_OF(Key), L"%sControl\\CriticalDeviceDatabase\\%s",
                        QzControlSet, Id);
    if (Length >= RTL_NUMBER_OF(Key))
        return FALSE;
    for (p = Key + wcslen(QzControlSet) + wcslen(L"Control\\CriticalDeviceDatabase\\"); *p; p++)
        if (*p == L'\\')
            *p = L'#';
    for (i = 0; i < ServiceBytes / sizeof(WCHAR); i++)
        Service[i] = 0;
    if (!NT_SUCCESS(QzCmReadValue(Key, L"Service", Service, ServiceBytes - sizeof(WCHAR), NULL, NULL)))
        return FALSE;
    if (!Node->ClassGuid[0])
        QzCmReadValue(Key, L"ClassGUID", Node->ClassGuid, sizeof(Node->ClassGuid) - sizeof(WCHAR),
                      NULL, NULL);
    return Service[0] != 0;
}

static BOOLEAN
QzPnpFindService(PQZ_DEVICE_NODE Node)
{
    WCHAR Service[64], Key[300];
    PCWSTR Lists[2] = { Node->HardwareIds, Node->CompatibleIds }, Id;
    ULONG i;

    if (Node->ServiceName.Length)
        return TRUE;

    /* An installed device names its service in its Enum key */
    RtlZeroMemory(Service, sizeof(Service));
    _snwprintf(Key, RTL_NUMBER_OF(Key), L"%sEnum\\%wZ", QzControlSet, &Node->InstancePath);
    if (NT_SUCCESS(QzCmReadValue(Key, L"Service", Service, sizeof(Service) - sizeof(WCHAR), NULL, NULL)) &&
        Service[0])
    {
        QzCmReadValue(Key, L"ClassGUID", Node->ClassGuid, sizeof(Node->ClassGuid) - sizeof(WCHAR),
                      NULL, NULL);
        QzPnpSetString(&Node->ServiceName, Service);
        return TRUE;
    }

    for (i = 0; i < 2; i++)
    {
        for (Id = Lists[i]; Id && *Id; Id += wcslen(Id) + 1)
        {
            if (QzPnpLookupCriticalDevice(Node, Id, Service, sizeof(Service)))
            {
                QzPnpSetString(&Node->ServiceName, Service);
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* ---------------------------------------------------------------------- */
/* Recording devices in the registry (Enum and Control\Class)              */
/* ---------------------------------------------------------------------- */

static NTSTATUS QzPnpOpenKey(PCWSTR Path, ACCESS_MASK DesiredAccess, PHANDLE Handle);

static VOID
QzPnpSetValue(HANDLE Key, PCWSTR Name, ULONG Type, const VOID *Data, ULONG Length)
{
    UNICODE_STRING ValueName;
    RtlInitUnicodeString(&ValueName, Name);
    ZwSetValueKey(Key, &ValueName, 0, Type, (PVOID)Data, Length);
}

static VOID
QzPnpSetText(HANDLE Key, PCWSTR Name, PCWSTR Text)
{
    if (Text && *Text)
        QzPnpSetValue(Key, Name, REG_SZ, Text, (ULONG)(wcslen(Text) + 1) * sizeof(WCHAR));
}

/* The device's software key: the Driver value already recorded for this
   instance, or the first free Control\Class\{guid}\NNNN */
static VOID
QzPnpAssignDriverKey(PQZ_DEVICE_NODE Node, PCWSTR EnumPath)
{
    WCHAR Path[160];
    ULONG Index;
    HANDLE Key;

    if (!Node->ClassGuid[0])
        return;
    if (NT_SUCCESS(QzCmReadValue(EnumPath, L"Driver", Node->DriverKey,
                                 sizeof(Node->DriverKey) - sizeof(WCHAR), NULL, NULL)) &&
        Node->DriverKey[0])
        return;
    for (Index = 0; Index < 10000; Index++)
    {
        UNICODE_STRING Name;
        OBJECT_ATTRIBUTES Attributes;
        ULONG Disposition = 0;

        _snwprintf(Path, RTL_NUMBER_OF(Path), L"%sControl\\Class\\%s\\%04lu", QzControlSet,
                   Node->ClassGuid, Index);
        Path[RTL_NUMBER_OF(Path) - 1] = UNICODE_NULL;
        RtlInitUnicodeString(&Name, Path);
        InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                   NULL, NULL);
        if (!NT_SUCCESS(ZwCreateKey(&Key, KEY_ALL_ACCESS, &Attributes, 0, NULL, 0, &Disposition)))
            return;
        if (Disposition == REG_CREATED_NEW_KEY)
        {
            QzPnpSetText(Key, L"DriverDesc", Node->Description);
            ZwClose(Key);
            _snwprintf(Node->DriverKey, RTL_NUMBER_OF(Node->DriverKey), L"%s\\%04lu",
                       Node->ClassGuid, Index);
            Node->DriverKey[RTL_NUMBER_OF(Node->DriverKey) - 1] = UNICODE_NULL;
            return;
        }
        ZwClose(Key);
    }
}

/* CurrentControlSet\Enum\<instance path>, as the NT PnP manager keeps it */
static VOID
QzPnpRecordNode(PQZ_DEVICE_NODE Node)
{
    WCHAR Path[320];
    ULONG Capabilities = 0, ConfigFlags = 0;
    HANDLE Key;

    if ((Node->Flags & QZ_DN_RECORDED) || !Node->InstancePath.Length)
        return;
    Node->Flags |= QZ_DN_RECORDED;
    _snwprintf(Path, RTL_NUMBER_OF(Path), L"%sEnum\\%wZ", QzControlSet, &Node->InstancePath);
    Path[RTL_NUMBER_OF(Path) - 1] = UNICODE_NULL;
    QzPnpAssignDriverKey(Node, Path);
    if (!NT_SUCCESS(QzPnpOpenKey(Path, KEY_ALL_ACCESS, &Key)))
        return;

    if (Node->HardwareIds)
        QzPnpSetValue(Key, L"HardwareID", REG_MULTI_SZ, Node->HardwareIds,
                      QzPnpMultiSzBytes(Node->HardwareIds));
    if (Node->CompatibleIds)
        QzPnpSetValue(Key, L"CompatibleIDs", REG_MULTI_SZ, Node->CompatibleIds,
                      QzPnpMultiSzBytes(Node->CompatibleIds));
    QzPnpSetText(Key, L"DeviceDesc", Node->Description);
    QzPnpSetText(Key, L"LocationInformation", Node->Location);
    if (Node->ServiceName.Length)
        QzPnpSetText(Key, L"Service", Node->ServiceName.Buffer);
    QzPnpSetText(Key, L"ClassGUID", Node->ClassGuid);
    QzPnpSetText(Key, L"Driver", Node->DriverKey);

    if (Node->Capabilities.LockSupported)      Capabilities |= CM_DEVCAP_LOCKSUPPORTED;
    if (Node->Capabilities.EjectSupported)     Capabilities |= CM_DEVCAP_EJECTSUPPORTED;
    if (Node->Capabilities.Removable)          Capabilities |= CM_DEVCAP_REMOVABLE;
    if (Node->Capabilities.DockDevice)         Capabilities |= CM_DEVCAP_DOCKDEVICE;
    if (Node->Capabilities.UniqueID)           Capabilities |= CM_DEVCAP_UNIQUEID;
    if (Node->Capabilities.SilentInstall)      Capabilities |= CM_DEVCAP_SILENTINSTALL;
    if (Node->Capabilities.RawDeviceOK)        Capabilities |= CM_DEVCAP_RAWDEVICEOK;
    if (Node->Capabilities.SurpriseRemovalOK)  Capabilities |= CM_DEVCAP_SURPRISEREMOVALOK;
    QzPnpSetValue(Key, L"Capabilities", REG_DWORD, &Capabilities, sizeof(Capabilities));
    if (Node->Capabilities.UINumber != (ULONG)-1)
        QzPnpSetValue(Key, L"UINumber", REG_DWORD, &Node->Capabilities.UINumber, sizeof(ULONG));
    {
        UNICODE_STRING ConfigFlagsName = RTL_CONSTANT_STRING(L"ConfigFlags");
        UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
        ULONG Result;
        if (!NT_SUCCESS(ZwQueryValueKey(Key, &ConfigFlagsName, KeyValuePartialInformation, Buffer,
                                        sizeof(Buffer), &Result)))
            QzPnpSetValue(Key, L"ConfigFlags", REG_DWORD, &ConfigFlags, sizeof(ConfigFlags));
    }
    ZwClose(Key);
}

/* Start one driver and call its AddDevice for this PDO */
static NTSTATUS
QzPnpAddDriver(PQZ_DEVICE_NODE Node, PCWSTR Service, BOOLEAN Function)
{
    UNICODE_STRING Name;
    PDRIVER_OBJECT Driver;
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, Service);
    Status = QzIoLoadDriver(&Name, &Driver);
    if (!NT_SUCCESS(Status))
    {
        if (Function && (Status == STATUS_OBJECT_NAME_NOT_FOUND || Status == STATUS_OBJECT_PATH_NOT_FOUND ||
                         Status == STATUS_DLL_NOT_FOUND || Status == STATUS_DRIVER_UNABLE_TO_LOAD ||
                         Status == STATUS_PROCEDURE_NOT_FOUND || Status == STATUS_INVALID_IMAGE_FORMAT))
        {
            /* The driver never ran: the device just has none */
            Node->Flags |= QZ_DN_NO_DRIVER;
            Node->Problem = Status;
            Node->ProblemText = Status == STATUS_PROCEDURE_NOT_FOUND ? "driver needs exports qzkrnl lacks" :
                                Status == STATUS_DRIVER_UNABLE_TO_LOAD ? "service disabled" :
                                Status == STATUS_INVALID_IMAGE_FORMAT ? "driver image is not valid" :
                                Status == STATUS_DLL_NOT_FOUND ? "a DLL the driver imports is missing" :
                                "driver file or service not found";
        }
        else if (Function)
        {
            QzPnpFail(Node, Status, "DriverEntry failed");
        }
        return Status;
    }
    if (!Driver->DriverExtension->AddDevice)
        return STATUS_SUCCESS;   /* a legacy driver: nothing to add */

    DbgPrint(QZ_KERNEL_NAME ": PnP: AddDevice %wZ for %wZ\n", &Driver->DriverName, &Node->InstancePath);
    Status = Driver->DriverExtension->AddDevice(Driver, Node->Pdo);
    if (!NT_SUCCESS(Status) && Function)
        QzPnpFail(Node, Status, "AddDevice failed");
    return Status;
}

static VOID
QzPnpAddFilters(PQZ_DEVICE_NODE Node, PCWSTR ValueName)
{
    WCHAR Key[128], List[128];
    ULONG Type = 0, Length = 0;
    PCWSTR Filter;

    if (!Node->ClassGuid[0])
        return;
    RtlZeroMemory(List, sizeof(List));
    _snwprintf(Key, RTL_NUMBER_OF(Key), L"%sControl\\Class\\%s", QzControlSet, Node->ClassGuid);
    if (!NT_SUCCESS(QzCmReadValue(Key, ValueName, List, sizeof(List) - 2 * sizeof(WCHAR), &Type, &Length)))
        return;
    for (Filter = List; *Filter; Filter += wcslen(Filter) + 1)
        QzPnpAddDriver(Node, Filter, FALSE);
}

static VOID
QzPnpAddDrivers(PQZ_DEVICE_NODE Node)
{
    BOOLEAN Found = QzPnpFindService(Node);

    QzPnpRecordNode(Node);
    if (!Found)
    {
        if (Node->Capabilities.RawDeviceOK)
        {
            Node->Flags |= QZ_DN_ADDED | QZ_DN_RAW;
            return;
        }
        Node->Flags |= QZ_DN_NO_DRIVER;
        QzPnpQueueInstallEvent(Node);   /* umpnpmgr may have a driver for it */
        return;
    }
    if (QzPnpBootPhase && !QzIoIsBootService(&Node->ServiceName))
    {
        /* As on NT: drivers that are not boot drivers load once the boot
           volume is there (see QzPnpStart) */
        Node->Flags |= QZ_DN_NO_DRIVER | QZ_DN_DEFERRED;
        Node->ProblemText = "waits for the boot volume";
        return;
    }

    QzPnpAddFilters(Node, L"LowerFilters");
    if (!NT_SUCCESS(QzPnpAddDriver(Node, Node->ServiceName.Buffer, TRUE)))
        return;
    QzPnpAddFilters(Node, L"UpperFilters");
    Node->Flags |= QZ_DN_ADDED;
}

/* ---------------------------------------------------------------------- */
/* Resources                                                               */
/* ---------------------------------------------------------------------- */

/* Choose one descriptor from each group of the first alternative list */
static PCM_RESOURCE_LIST
QzPnpChooseResources(PIO_RESOURCE_REQUIREMENTS_LIST Requirements, PCM_RESOURCE_LIST Boot)
{
    PIO_RESOURCE_LIST Alternative = &Requirements->List[0];
    PCM_RESOURCE_LIST List;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Out;
    ULONG i, Count = 0, Size;

    if (!Requirements->AlternativeLists || !Alternative->Count)
        return NULL;
    Size = FIELD_OFFSET(CM_RESOURCE_LIST, List[0].PartialResourceList.PartialDescriptors) +
           Alternative->Count * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);
    List = ExAllocatePoolWithTag(NonPagedPool, Size, QZ_PNP_TAG);
    if (!List)
        return NULL;
    RtlZeroMemory(List, Size);
    List->Count = 1;
    List->List[0].InterfaceType = Requirements->InterfaceType;
    List->List[0].BusNumber = Requirements->BusNumber;
    List->List[0].PartialResourceList.Version = 1;
    List->List[0].PartialResourceList.Revision = 1;
    Out = List->List[0].PartialResourceList.PartialDescriptors;

    for (i = 0; i < Alternative->Count; i++)
    {
        PIO_RESOURCE_DESCRIPTOR In = &Alternative->Descriptors[i];
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc;

        /* An alternative to a descriptor we already took */
        if ((In->Option & IO_RESOURCE_ALTERNATIVE) && Count)
            continue;
        if (In->Type == CmResourceTypeConfigData)
            continue;

        Desc = &Out[Count++];
        Desc->Type = In->Type;
        Desc->ShareDisposition = In->ShareDisposition;
        Desc->Flags = In->Flags;
        switch (In->Type)
        {
            case CmResourceTypePort:
            case CmResourceTypeMemory:
                Desc->u.Port.Start = In->u.Port.MinimumAddress;
                Desc->u.Port.Length = In->u.Port.Length;
                break;
            case CmResourceTypeInterrupt:
            {
                /* A range (PCI asks for 0-255): the line the device was
                   given at boot, as an arbiter would choose */
                ULONG Line = In->u.Interrupt.MinimumVector;
                if (In->u.Interrupt.MaximumVector != In->u.Interrupt.MinimumVector && Boot && Boot->Count)
                {
                    PCM_PARTIAL_RESOURCE_LIST Partial = &Boot->List[0].PartialResourceList;
                    ULONG k;
                    for (k = 0; k < Partial->Count; k++)
                    {
                        PCM_PARTIAL_RESOURCE_DESCRIPTOR B = &Partial->PartialDescriptors[k];
                        if (B->Type == CmResourceTypeInterrupt &&
                            B->u.Interrupt.Vector >= In->u.Interrupt.MinimumVector &&
                            B->u.Interrupt.Vector <= In->u.Interrupt.MaximumVector)
                        {
                            Line = B->u.Interrupt.Vector;
                            break;
                        }
                    }
                }
                Desc->u.Interrupt.Level = Line;
                Desc->u.Interrupt.Vector = Line;
                Desc->u.Interrupt.Affinity = (KAFFINITY)-1;
                break;
            }
            case CmResourceTypeDma:
                Desc->u.Dma.Channel = In->u.Dma.MinimumChannel;
                break;
            case CmResourceTypeBusNumber:
                Desc->u.BusNumber.Start = In->u.BusNumber.MinBusNumber;
                Desc->u.BusNumber.Length = In->u.BusNumber.Length;
                break;
            case CmResourceTypeDevicePrivate:
                Desc->u.DevicePrivate.Data[0] = In->u.DevicePrivate.Data[0];
                Desc->u.DevicePrivate.Data[1] = In->u.DevicePrivate.Data[1];
                Desc->u.DevicePrivate.Data[2] = In->u.DevicePrivate.Data[2];
                break;
            default:
                Count--;
                break;
        }
    }
    List->List[0].PartialResourceList.Count = Count;
    return List;
}

static ULONG
QzPnpResourceListSize(PCM_RESOURCE_LIST List)
{
    PCM_FULL_RESOURCE_DESCRIPTOR Full = List->List;
    ULONG i;

    for (i = 0; i < List->Count; i++)
        Full = (PCM_FULL_RESOURCE_DESCRIPTOR)(Full->PartialResourceList.PartialDescriptors +
                                               Full->PartialResourceList.Count);
    return (ULONG)((PUCHAR)Full - (PUCHAR)List);
}

/* Raw -> translated through the HAL (ports may become memory, IRQs vectors) */
static PCM_RESOURCE_LIST
QzPnpTranslate(PCM_RESOURCE_LIST Raw)
{
    ULONG Size = QzPnpResourceListSize(Raw), i, j;
    PCM_RESOURCE_LIST Translated = ExAllocatePoolWithTag(NonPagedPool, Size, QZ_PNP_TAG);
    PCM_FULL_RESOURCE_DESCRIPTOR Full;

    if (!Translated)
        return NULL;
    RtlCopyMemory(Translated, Raw, Size);
    Full = Translated->List;
    for (i = 0; i < Translated->Count; i++)
    {
        for (j = 0; j < Full->PartialResourceList.Count; j++)
        {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = &Full->PartialResourceList.PartialDescriptors[j];
            switch (Desc->Type)
            {
                case CmResourceTypePort:
                case CmResourceTypeMemory:
                {
                    ULONG AddressSpace = (Desc->Type == CmResourceTypePort) ? 1 : 0;
                    PHYSICAL_ADDRESS Out;
                    if (HalTranslateBusAddress(Full->InterfaceType, Full->BusNumber,
                                               Desc->u.Port.Start, &AddressSpace, &Out))
                    {
                        Desc->u.Port.Start = Out;
                        if (Desc->Type == CmResourceTypePort && AddressSpace == 0)
                        {
                            Desc->Type = CmResourceTypeMemory;
                            Desc->Flags = CM_RESOURCE_MEMORY_READ_WRITE;
                        }
                    }
                    break;
                }
                case CmResourceTypeInterrupt:
                {
                    KIRQL Irql;
                    KAFFINITY Affinity;
                    ULONG Vector = HalGetInterruptVector(Full->InterfaceType, Full->BusNumber,
                                                         Desc->u.Interrupt.Level,
                                                         Desc->u.Interrupt.Vector, &Irql, &Affinity);
                    Desc->u.Interrupt.Vector = Vector;
                    Desc->u.Interrupt.Level = Irql;
                    Desc->u.Interrupt.Affinity = Affinity;
                    break;
                }
                default:
                    break;
            }
        }
        Full = (PCM_FULL_RESOURCE_DESCRIPTOR)(Full->PartialResourceList.PartialDescriptors +
                                               Full->PartialResourceList.Count);
    }
    return Translated;
}

static VOID
QzPnpStartNode(PQZ_DEVICE_NODE Node)
{
    PIO_RESOURCE_REQUIREMENTS_LIST Requirements = NULL;
    IO_STACK_LOCATION Stack;
    ULONG_PTR Information;
    NTSTATUS Status;

    Requirements = Node->Requirements;
    Node->Requirements = NULL;
    if (!(Node->Flags & QZ_DN_REPORTED))
    {
        /* Let the drivers above the PDO adjust them (videoprt adds the
           miniport's legacy ranges). One that replaces the list frees the
           old one itself */
        RtlZeroMemory(&Stack, sizeof(Stack));
        Stack.Parameters.FilterResourceRequirements.IoResourceRequirementList = Requirements;
        Information = (ULONG_PTR)Requirements;
        if (NT_SUCCESS(QzPnpSend(Node->Pdo, IRP_MN_FILTER_RESOURCE_REQUIREMENTS, &Stack, &Information)) &&
            Information && Information != (ULONG_PTR)Requirements)
            Requirements = (PIO_RESOURCE_REQUIREMENTS_LIST)Information;
    }
    if (!Node->RawResources)
    {
        if (Requirements)
            Node->RawResources = QzPnpChooseResources(Requirements, Node->BootResources);
        else if (Node->BootResources)
        {
            ULONG Size = QzPnpResourceListSize(Node->BootResources);
            Node->RawResources = ExAllocatePoolWithTag(NonPagedPool, Size, QZ_PNP_TAG);
            if (Node->RawResources)
                RtlCopyMemory(Node->RawResources, Node->BootResources, Size);
        }
    }
    if (Requirements)
        ExFreePool(Requirements);
    /* An empty list still goes to the driver (the PCI root bus's boot
       configuration is just "PCI bus 0", with no descriptors) */
    if (Node->RawResources && !Node->TranslatedResources)
        Node->TranslatedResources = QzPnpTranslate(Node->RawResources);

    RtlZeroMemory(&Stack, sizeof(Stack));
    if (Node->RawResources && Node->TranslatedResources)
    {
        Stack.Parameters.StartDevice.AllocatedResources = Node->RawResources;
        Stack.Parameters.StartDevice.AllocatedResourcesTranslated = Node->TranslatedResources;
    }
    DbgPrint(QZ_KERNEL_NAME ": PnP: start %wZ (%lu resources)\n", &Node->InstancePath,
             (Node->RawResources && Node->TranslatedResources && Node->RawResources->Count)
                 ? Node->RawResources->List[0].PartialResourceList.Count : 0);
    Status = QzPnpSend(Node->Pdo, IRP_MN_START_DEVICE, &Stack, NULL);
    if (!NT_SUCCESS(Status))
    {
        QzPnpFail(Node, Status, "IRP_MN_START_DEVICE failed");
        return;
    }
    Node->Flags |= QZ_DN_STARTED;
    QzPnpStartedCount++;
}

/* ---------------------------------------------------------------------- */
/* Enumeration                                                             */
/* ---------------------------------------------------------------------- */

static VOID
QzPnpEnumerateChildren(PQZ_DEVICE_NODE Node)
{
    PDEVICE_RELATIONS Relations;
    IO_STACK_LOCATION Stack;
    ULONG_PTR Information = 0;
    NTSTATUS Status;
    ULONG i;

    if (Node == QzPnpRoot)
        return;   /* the root's children are the reported devices */

    RtlZeroMemory(&Stack, sizeof(Stack));
    Stack.Parameters.QueryDeviceRelations.Type = BusRelations;
    Status = QzPnpSend(Node->Pdo, IRP_MN_QUERY_DEVICE_RELATIONS, &Stack, &Information);
    if (!NT_SUCCESS(Status) || !Information)
    {
        if (Status != STATUS_NOT_SUPPORTED)
            DbgPrint(QZ_KERNEL_NAME ": PnP: %wZ: bus relations 0x%08lx\n", &Node->InstancePath, Status);
        return;
    }

    Relations = (PDEVICE_RELATIONS)Information;
    DbgPrint(QZ_KERNEL_NAME ": PnP: %wZ: %lu child device(s)\n", &Node->InstancePath, Relations->Count);
    for (i = 0; i < Relations->Count; i++)
    {
        PDEVICE_OBJECT Pdo = Relations->Objects[i];
        PQZ_DEVICE_NODE Child = QzNodeOf(Pdo);

        if (Child)
        {
            ObDereferenceObject(Pdo);   /* known: the node holds its reference */
            continue;
        }
        Pdo->Flags &= ~DO_DEVICE_INITIALIZING;
        if (!QzPnpCreateNode(Node, Pdo))
            ObDereferenceObject(Pdo);
    }
    ExFreePool(Relations);
}

static VOID QzPnpProcessNode(PQZ_DEVICE_NODE Node);

/* The second pass: devices whose drivers waited for the boot volume */
static VOID
QzPnpRetryDeferred(PQZ_DEVICE_NODE Node)
{
    PQZ_DEVICE_NODE Child;

    if (Node->Flags & QZ_DN_DEFERRED)
    {
        Node->Flags &= ~(QZ_DN_DEFERRED | QZ_DN_NO_DRIVER);
        Node->ProblemText = NULL;
        QzPnpProcessNode(Node);
        return;
    }
    for (Child = Node->Child; Child; Child = Child->Sibling)
        QzPnpRetryDeferred(Child);
}

static VOID
QzPnpProcessNode(PQZ_DEVICE_NODE Node)
{
    PQZ_DEVICE_NODE Child;

    Node->Flags &= ~QZ_DN_REENUMERATE;
    if (!(Node->Flags & QZ_DN_IDS))
        QzPnpQueryIdentity(Node);
    if (!(Node->Flags & (QZ_DN_ADDED | QZ_DN_FAILED | QZ_DN_NO_DRIVER)))
        QzPnpAddDrivers(Node);
    if ((Node->Flags & QZ_DN_ADDED) && !(Node->Flags & (QZ_DN_STARTED | QZ_DN_FAILED)))
        QzPnpStartNode(Node);
    if (Node->Flags & QZ_DN_STARTED)
        QzPnpEnumerateChildren(Node);
    for (Child = Node->Child; Child; Child = Child->Sibling)
        QzPnpProcessNode(Child);
}

typedef struct _QZ_REENUMERATE
{
    WORK_QUEUE_ITEM Item;
    PQZ_DEVICE_NODE Node;
} QZ_REENUMERATE, *PQZ_REENUMERATE;

static VOID NTAPI
QzPnpReenumerateWorker(PVOID Context)
{
    PQZ_REENUMERATE Work = Context;
    PQZ_DEVICE_NODE Node = Work->Node;

    ExFreePoolWithTag(Work, QZ_PNP_TAG);
    QzPnpLockTree();
    if (Node->Flags & QZ_DN_REENUMERATE)
        QzPnpProcessNode(Node);
    QzPnpUnlockTree();
}

VOID
NTAPI
IoInvalidateDeviceRelations(IN PDEVICE_OBJECT DeviceObject,
                            IN DEVICE_RELATION_TYPE Type)
{
    PQZ_DEVICE_NODE Node = QzNodeOf(DeviceObject);
    PQZ_REENUMERATE Work;

    if (!Node || Type != BusRelations || (Node->Flags & QZ_DN_REENUMERATE))
        return;
    Work = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Work), QZ_PNP_TAG);
    if (!Work)
        return;
    Node->Flags |= QZ_DN_REENUMERATE;
    Work->Node = Node;
    ExInitializeWorkItem(&Work->Item, QzPnpReenumerateWorker, Work);
    ExQueueWorkItem(&Work->Item, DelayedWorkQueue);
}

NTSTATUS
NTAPI
IoSynchronousInvalidateDeviceRelations(IN PDEVICE_OBJECT DeviceObject,
                                       IN DEVICE_RELATION_TYPE Type)
{
    PQZ_DEVICE_NODE Node = QzNodeOf(DeviceObject);
    if (!Node)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (Type == BusRelations)
    {
        QzPnpLockTree();
        QzPnpProcessNode(Node);
        QzPnpUnlockTree();
    }
    return STATUS_SUCCESS;
}

VOID
NTAPI
IoInvalidateDeviceState(IN PDEVICE_OBJECT PhysicalDeviceObject)
{
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);
}

VOID
NTAPI
IoRequestDeviceEject(IN PDEVICE_OBJECT PhysicalDeviceObject)
{
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);
}

/* ---------------------------------------------------------------------- */
/* The PnP manager's own driver: the root device and reported PDOs          */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzPnpCompletePdo(PIRP Irp, NTSTATUS Status, ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS NTAPI
QzPnpRootDispatchPnp(PDEVICE_OBJECT Device, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PQZ_DEVICE_NODE Node = QzNodeOf(Device);

    switch (Stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_CANCEL_STOP_DEVICE:
        case IRP_MN_STOP_DEVICE:
        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
        case IRP_MN_REMOVE_DEVICE:
            return QzPnpCompletePdo(Irp, STATUS_SUCCESS, 0);

        case IRP_MN_QUERY_CAPABILITIES:
        {
            PDEVICE_CAPABILITIES Caps = Stack->Parameters.DeviceCapabilities.Capabilities;
            Caps->UniqueID = FALSE;
            Caps->SilentInstall = TRUE;
            Caps->RawDeviceOK = FALSE;
            return QzPnpCompletePdo(Irp, STATUS_SUCCESS, 0);
        }

        case IRP_MN_QUERY_PNP_DEVICE_STATE:
            return QzPnpCompletePdo(Irp, STATUS_SUCCESS, PNP_DEVICE_NOT_DISABLEABLE);

        case IRP_MN_QUERY_DEVICE_RELATIONS:
            if (Stack->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation)
            {
                PDEVICE_RELATIONS Relations = ExAllocatePoolWithTag(PagedPool, sizeof(*Relations),
                                                                    QZ_PNP_TAG);
                if (!Relations)
                    return QzPnpCompletePdo(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
                Relations->Count = 1;
                Relations->Objects[0] = Device;
                ObReferenceObject(Device);
                return QzPnpCompletePdo(Irp, STATUS_SUCCESS, (ULONG_PTR)Relations);
            }
            break;

        case IRP_MN_QUERY_RESOURCES:
            if (Node && Node->BootResources)
            {
                ULONG Size = QzPnpResourceListSize(Node->BootResources);
                PVOID Copy = ExAllocatePoolWithTag(PagedPool, Size, QZ_PNP_TAG);
                if (Copy)
                {
                    RtlCopyMemory(Copy, Node->BootResources, Size);
                    return QzPnpCompletePdo(Irp, STATUS_SUCCESS, (ULONG_PTR)Copy);
                }
            }
            break;

        default:
            break;
    }

    /* Not ours to answer: keep whatever the stack above put there */
    {
        NTSTATUS Status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }
}

static NTSTATUS NTAPI
QzPnpRootDispatchPower(PDEVICE_OBJECT Device, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Device);
    PoStartNextPowerIrp(Irp);
    return QzPnpCompletePdo(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS NTAPI
QzPnpRootDispatchOther(PDEVICE_OBJECT Device, PIRP Irp)
{
    NTSTATUS Status = Irp->IoStatus.Status;
    UNREFERENCED_PARAMETER(Device);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS NTAPI
QzPnpRootEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);
    Driver->MajorFunction[IRP_MJ_PNP] = QzPnpRootDispatchPnp;
    Driver->MajorFunction[IRP_MJ_POWER] = QzPnpRootDispatchPower;
    Driver->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = QzPnpRootDispatchOther;
    QzPnpRootDriver = Driver;
    return IoCreateDevice(Driver, 0, NULL, FILE_DEVICE_CONTROLLER, FILE_AUTOGENERATED_DEVICE_NAME,
                          FALSE, &QzPnpRootDevice);
}

NTSTATUS
NTAPI
IoReportDetectedDevice(IN PDRIVER_OBJECT DriverObject,
                       IN INTERFACE_TYPE LegacyBusType,
                       IN ULONG BusNumber,
                       IN ULONG SlotNumber,
                       IN PCM_RESOURCE_LIST ResourceList OPTIONAL,
                       IN PIO_RESOURCE_REQUIREMENTS_LIST ResourceRequirements OPTIONAL,
                       IN BOOLEAN ResourceAssigned,
                       IN OUT PDEVICE_OBJECT *DeviceObject OPTIONAL)
{
    PQZ_DEVICE_NODE Node;
    PDEVICE_OBJECT Pdo;
    WCHAR Path[96];
    NTSTATUS Status;
    ULONG i;

    UNREFERENCED_PARAMETER(SlotNumber);
    UNREFERENCED_PARAMETER(ResourceRequirements);

    if (DeviceObject && *DeviceObject)
        return STATUS_SUCCESS;   /* already reported */

    Status = IoCreateDevice(QzPnpRootDriver, 0, NULL, FILE_DEVICE_CONTROLLER,
                            FILE_AUTOGENERATED_DEVICE_NAME, FALSE, &Pdo);
    if (!NT_SUCCESS(Status))
        return Status;
    Pdo->Flags &= ~DO_DEVICE_INITIALIZING;

    QzPnpLockTree();
    Node = QzPnpCreateNode(QzPnpRoot, Pdo);
    if (!Node)
    {
        QzPnpUnlockTree();
        IoDeleteDevice(Pdo);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Node->Flags |= QZ_DN_REPORTED | QZ_DN_IDS | QZ_DN_ADDED;
    Node->ReportedBusType = LegacyBusType;
    Node->ReportedBusNumber = BusNumber;
    QzPnpSetString(&Node->ServiceName, QzDriverExtension(DriverObject)->Public.ServiceKeyName.Buffer);
    _snwprintf(Path, RTL_NUMBER_OF(Path), L"Root\\LEGACY_%wZ\\0000",
               &QzDriverExtension(DriverObject)->Public.ServiceKeyName);
    for (i = 12; Path[i] && Path[i] != L'\\'; i++)
        Path[i] = RtlUpcaseUnicodeChar(Path[i]);
    QzPnpSetString(&Node->InstancePath, Path);
    Node->DeviceId = QzPnpDuplicate(Path, (ULONG)(wcsrchr(Path, L'\\') - Path) * sizeof(WCHAR));
    if (ResourceList && ResourceAssigned)
    {
        ULONG Size = QzPnpResourceListSize(ResourceList);
        Node->BootResources = ExAllocatePoolWithTag(NonPagedPool, Size, QZ_PNP_TAG);
        if (Node->BootResources)
            RtlCopyMemory(Node->BootResources, ResourceList, Size);
    }
    QzPnpUnlockTree();

    if (DeviceObject)
        *DeviceObject = Pdo;
    return STATUS_SUCCESS;
}

/* No resource map or arbitration yet: reports are accepted, never conflict */
NTSTATUS
NTAPI
IoReportHalResourceUsage(IN PUNICODE_STRING HalName,
                         IN PCM_RESOURCE_LIST RawResourceList,
                         IN PCM_RESOURCE_LIST TranslatedResourceList,
                         IN ULONG ResourceListSize)
{
    UNREFERENCED_PARAMETER(HalName);
    UNREFERENCED_PARAMETER(RawResourceList);
    UNREFERENCED_PARAMETER(TranslatedResourceList);
    UNREFERENCED_PARAMETER(ResourceListSize);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
IoReportResourceUsage(IN PUNICODE_STRING DriverClassName OPTIONAL,
                      IN PDRIVER_OBJECT DriverObject,
                      IN PCM_RESOURCE_LIST DriverList OPTIONAL,
                      IN ULONG DriverListSize OPTIONAL,
                      IN PDEVICE_OBJECT DeviceObject,
                      IN PCM_RESOURCE_LIST DeviceList OPTIONAL,
                      IN ULONG DeviceListSize OPTIONAL,
                      IN BOOLEAN OverrideConflict,
                      OUT PBOOLEAN ConflictDetected)
{
    UNREFERENCED_PARAMETER(DriverClassName);
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(DriverList);
    UNREFERENCED_PARAMETER(DriverListSize);
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(DeviceList);
    UNREFERENCED_PARAMETER(DeviceListSize);
    UNREFERENCED_PARAMETER(OverrideConflict);
    *ConflictDetected = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
IoReportResourceForDetection(IN PDRIVER_OBJECT DriverObject,
                             IN PCM_RESOURCE_LIST DriverList OPTIONAL,
                             IN ULONG DriverListSize OPTIONAL,
                             IN PDEVICE_OBJECT DeviceObject OPTIONAL,
                             IN PCM_RESOURCE_LIST DeviceList OPTIONAL,
                             IN ULONG DeviceListSize OPTIONAL,
                             OUT PBOOLEAN ConflictDetected)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(DriverList);
    UNREFERENCED_PARAMETER(DriverListSize);
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(DeviceList);
    UNREFERENCED_PARAMETER(DeviceListSize);
    *ConflictDetected = FALSE;
    return STATUS_SUCCESS;
}

/*
 * Legacy resource assignment (videoprt, for adapters it finds itself):
 * nothing arbitrates resources yet, so each requirement gets the start of
 * its range, from the first alternative list.
 */
NTSTATUS
NTAPI
IoAssignResources(IN PUNICODE_STRING RegistryPath, IN PUNICODE_STRING DriverClassName OPTIONAL,
                  IN PDRIVER_OBJECT DriverObject, IN PDEVICE_OBJECT DeviceObject OPTIONAL,
                  IN PIO_RESOURCE_REQUIREMENTS_LIST RequestedResources OPTIONAL,
                  IN OUT PCM_RESOURCE_LIST *AllocatedResources)
{
    PIO_RESOURCE_LIST Alternative;
    PCM_RESOURCE_LIST List;
    PCM_PARTIAL_RESOURCE_LIST Partial;
    ULONG i, Count = 0, Size;

    UNREFERENCED_PARAMETER(RegistryPath);
    UNREFERENCED_PARAMETER(DriverClassName);
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(DeviceObject);
    if (!RequestedResources || !RequestedResources->AlternativeLists)
        return STATUS_INVALID_PARAMETER;
    Alternative = &RequestedResources->List[0];
    for (i = 0; i < Alternative->Count; i++)
        if (!(Alternative->Descriptors[i].Option & IO_RESOURCE_ALTERNATIVE))
            Count++;
    Size = FIELD_OFFSET(CM_RESOURCE_LIST, List[0].PartialResourceList.PartialDescriptors[Count]);
    List = ExAllocatePoolWithTag(PagedPool, Size, 'rAzQ');
    if (!List)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(List, Size);
    List->Count = 1;
    List->List[0].InterfaceType = RequestedResources->InterfaceType;
    List->List[0].BusNumber = RequestedResources->BusNumber;
    Partial = &List->List[0].PartialResourceList;
    Partial->Version = Alternative->Version;
    Partial->Revision = Alternative->Revision;
    for (i = 0; i < Alternative->Count; i++)
    {
        PIO_RESOURCE_DESCRIPTOR In = &Alternative->Descriptors[i];
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Out;
        if (In->Option & IO_RESOURCE_ALTERNATIVE)
            continue;
        Out = &Partial->PartialDescriptors[Partial->Count++];
        Out->Type = In->Type;
        Out->ShareDisposition = In->ShareDisposition;
        Out->Flags = In->Flags;
        switch (In->Type)
        {
            case CmResourceTypePort:
            case CmResourceTypeMemory:
                Out->u.Generic.Start = In->u.Generic.MinimumAddress;
                Out->u.Generic.Length = In->u.Generic.Length;
                break;
            case CmResourceTypeInterrupt:
                Out->u.Interrupt.Level = In->u.Interrupt.MinimumVector;
                Out->u.Interrupt.Vector = In->u.Interrupt.MinimumVector;
                Out->u.Interrupt.Affinity = 1;
                break;
            case CmResourceTypeDma:
                Out->u.Dma.Channel = In->u.Dma.MinimumChannel;
                break;
            case CmResourceTypeBusNumber:
                Out->u.BusNumber.Start = In->u.BusNumber.MinBusNumber;
                Out->u.BusNumber.Length = In->u.BusNumber.Length;
                break;
            default:
                break;
        }
    }
    *AllocatedResources = List;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Device properties and registry keys                                     */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzPnpReturn(PVOID Buffer, ULONG BufferLength, PULONG ResultLength, const VOID *Data, ULONG Size)
{
    *ResultLength = Size;
    if (BufferLength < Size)
        return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(Buffer, Data, Size);
    return STATUS_SUCCESS;
}

static NTSTATUS
QzPnpReturnString(PVOID Buffer, ULONG BufferLength, PULONG ResultLength, PCWSTR String)
{
    if (!String)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    return QzPnpReturn(Buffer, BufferLength, ResultLength, String,
                       (ULONG)(wcslen(String) + 1) * sizeof(WCHAR));
}

NTSTATUS
NTAPI
IoGetDeviceProperty(IN PDEVICE_OBJECT DeviceObject,
                    IN DEVICE_REGISTRY_PROPERTY DeviceProperty,
                    IN ULONG BufferLength,
                    OUT PVOID PropertyBuffer,
                    OUT PULONG ResultLength)
{
    PQZ_DEVICE_NODE Node = QzNodeOf(DeviceObject);
    ULONG Value;

    *ResultLength = 0;
    if (!Node || Node->Pdo != DeviceObject)
        return STATUS_INVALID_DEVICE_REQUEST;

    switch (DeviceProperty)
    {
        case DevicePropertyDeviceDescription:
        case DevicePropertyFriendlyName:
            return QzPnpReturnString(PropertyBuffer, BufferLength, ResultLength, Node->Description);
        case DevicePropertyHardwareID:
            return Node->HardwareIds
                       ? QzPnpReturn(PropertyBuffer, BufferLength, ResultLength, Node->HardwareIds,
                                     QzPnpMultiSzBytes(Node->HardwareIds))
                       : STATUS_OBJECT_NAME_NOT_FOUND;
        case DevicePropertyCompatibleIDs:
            return Node->CompatibleIds
                       ? QzPnpReturn(PropertyBuffer, BufferLength, ResultLength, Node->CompatibleIds,
                                     QzPnpMultiSzBytes(Node->CompatibleIds))
                       : STATUS_OBJECT_NAME_NOT_FOUND;
        case DevicePropertyBootConfiguration:
        case DevicePropertyBootConfigurationTranslated:
        {
            PCM_RESOURCE_LIST List = (DeviceProperty == DevicePropertyBootConfiguration)
                                         ? Node->BootResources : Node->TranslatedResources;
            if (!List)
                return STATUS_OBJECT_NAME_NOT_FOUND;
            return QzPnpReturn(PropertyBuffer, BufferLength, ResultLength, List,
                               QzPnpResourceListSize(List));
        }
        case DevicePropertyClassGuid:
            return Node->ClassGuid[0] ? QzPnpReturnString(PropertyBuffer, BufferLength, ResultLength,
                                                          Node->ClassGuid)
                                      : STATUS_OBJECT_NAME_NOT_FOUND;
        case DevicePropertyDriverKeyName:
        {
            WCHAR Name[64];
            if (!Node->ClassGuid[0])
                return STATUS_OBJECT_NAME_NOT_FOUND;
            _snwprintf(Name, RTL_NUMBER_OF(Name), L"%s\\0000", Node->ClassGuid);
            return QzPnpReturnString(PropertyBuffer, BufferLength, ResultLength, Name);
        }
        case DevicePropertyLocationInformation:
            return QzPnpReturnString(PropertyBuffer, BufferLength, ResultLength, Node->Location);
        case DevicePropertyPhysicalDeviceObjectName:
        {
            union { OBJECT_NAME_INFORMATION Info; UCHAR Raw[256]; } Name;
            ULONG Length;
            if (!NT_SUCCESS(ObQueryNameString(DeviceObject, &Name.Info, sizeof(Name), &Length)))
                return STATUS_OBJECT_NAME_NOT_FOUND;
            return QzPnpReturn(PropertyBuffer, BufferLength, ResultLength, Name.Info.Name.Buffer,
                               Name.Info.Name.Length + sizeof(WCHAR));
        }
        case DevicePropertyBusTypeGuid:
            if (!Node->HasBusInformation)
                return STATUS_OBJECT_NAME_NOT_FOUND;
            return QzPnpReturn(PropertyBuffer, BufferLength, ResultLength,
                               &Node->BusInformation.BusTypeGuid, sizeof(GUID));
        case DevicePropertyLegacyBusType:
            Value = Node->HasBusInformation ? (ULONG)Node->BusInformation.LegacyBusType
                                            : (ULONG)Node->ReportedBusType;
            if (Value == (ULONG)InterfaceTypeUndefined)
                return STATUS_OBJECT_NAME_NOT_FOUND;
            return QzPnpReturn(PropertyBuffer, BufferLength, ResultLength, &Value, sizeof(Value));
        case DevicePropertyBusNumber:
            if (!Node->HasBusInformation && Node->ReportedBusType == InterfaceTypeUndefined)
                return STATUS_OBJECT_NAME_NOT_FOUND;
            Value = Node->HasBusInformation ? Node->BusInformation.BusNumber : Node->ReportedBusNumber;
            return QzPnpReturn(PropertyBuffer, BufferLength, ResultLength, &Value, sizeof(Value));
        case DevicePropertyEnumeratorName:
        {
            WCHAR Name[32];
            ULONG i;
            for (i = 0; i < RTL_NUMBER_OF(Name) - 1 && Node->InstancePath.Buffer[i] &&
                        Node->InstancePath.Buffer[i] != L'\\'; i++)
                Name[i] = Node->InstancePath.Buffer[i];
            Name[i] = UNICODE_NULL;
            return QzPnpReturnString(PropertyBuffer, BufferLength, ResultLength, Name);
        }
        case DevicePropertyAddress:
            if (Node->Capabilities.Address == (ULONG)-1)
                return STATUS_OBJECT_NAME_NOT_FOUND;
            return QzPnpReturn(PropertyBuffer, BufferLength, ResultLength,
                               &Node->Capabilities.Address, sizeof(ULONG));
        case DevicePropertyUINumber:
            if (Node->Capabilities.UINumber == (ULONG)-1)
                return STATUS_OBJECT_NAME_NOT_FOUND;
            return QzPnpReturn(PropertyBuffer, BufferLength, ResultLength,
                               &Node->Capabilities.UINumber, sizeof(ULONG));
        case DevicePropertyInstallState:
            Value = InstallStateInstalled;
            return QzPnpReturn(PropertyBuffer, BufferLength, ResultLength, &Value, sizeof(Value));
        case DevicePropertyRemovalPolicy:
            Value = Node->Capabilities.Removable ? RemovalPolicyExpectSurpriseRemoval
                                                 : RemovalPolicyExpectNoRemoval;
            return QzPnpReturn(PropertyBuffer, BufferLength, ResultLength, &Value, sizeof(Value));
        default:
            return STATUS_OBJECT_NAME_NOT_FOUND;
    }
}

static NTSTATUS
QzPnpOpenKey(PCWSTR Path, ACCESS_MASK DesiredAccess, PHANDLE Handle)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;

    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    return ZwCreateKey(Handle, DesiredAccess, &Attributes, 0, NULL, 0, NULL);
}

NTSTATUS
NTAPI
IoOpenDeviceRegistryKey(IN PDEVICE_OBJECT DeviceObject,
                        IN ULONG DevInstKeyType,
                        IN ACCESS_MASK DesiredAccess,
                        OUT PHANDLE DevInstRegKey)
{
    PQZ_DEVICE_NODE Node = QzNodeOf(DeviceObject);
    WCHAR Path[320];

    if (!Node || !Node->InstancePath.Length)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (DevInstKeyType & PLUGPLAY_REGKEY_DRIVER)
    {
        if (!Node->DriverKey[0])
            return STATUS_OBJECT_NAME_NOT_FOUND;   /* no class, no software key */
        _snwprintf(Path, RTL_NUMBER_OF(Path), L"%sControl\\Class\\%s", QzControlSet, Node->DriverKey);
    }
    else
        _snwprintf(Path, RTL_NUMBER_OF(Path), L"%sEnum\\%wZ\\Device Parameters", QzControlSet,
                   &Node->InstancePath);
    Path[RTL_NUMBER_OF(Path) - 1] = UNICODE_NULL;
    return QzPnpOpenKey(Path, DesiredAccess, DevInstRegKey);
}

NTSTATUS
NTAPI
IoOpenDeviceInterfaceRegistryKey(IN PUNICODE_STRING SymbolicLinkName,
                                 IN ACCESS_MASK DesiredAccess,
                                 OUT PHANDLE DeviceInterfaceKey)
{
    UNREFERENCED_PARAMETER(SymbolicLinkName);
    return QzPnpOpenKey(L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\DeviceClasses",
                        DesiredAccess, DeviceInterfaceKey);
}

/* ---------------------------------------------------------------------- */
/* Device interfaces and notifications                                     */
/* ---------------------------------------------------------------------- */

static VOID
QzPnpGuidToString(const GUID *Guid, PWSTR Buffer)
{
    _snwprintf(Buffer, 39, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               Guid->Data1, Guid->Data2, Guid->Data3, Guid->Data4[0], Guid->Data4[1],
               Guid->Data4[2], Guid->Data4[3], Guid->Data4[4], Guid->Data4[5], Guid->Data4[6],
               Guid->Data4[7]);
    Buffer[38] = UNICODE_NULL;
}

static VOID NTAPI
QzPnpNotifyWorker(PVOID Parameter)
{
    PQZ_NOTIFY_WORK Work = Parameter;
    DEVICE_INTERFACE_CHANGE_NOTIFICATION Notification;
    PLIST_ENTRY Entry;

    Notification.Version = 1;
    Notification.Size = sizeof(Notification);
    Notification.Event = Work->Event;
    Notification.InterfaceClassGuid = Work->Guid;
    Notification.SymbolicLinkName = &Work->Name;

    ExAcquireFastMutex(&QzPnpListLock);
    for (Entry = QzPnpNotifications.Flink; Entry != &QzPnpNotifications; Entry = Entry->Flink)
    {
        PQZ_NOTIFY Notify = CONTAINING_RECORD(Entry, QZ_NOTIFY, Link);
        if (Notify->Unregistered || Notify->Category != EventCategoryDeviceInterfaceChange ||
            !IsEqualGUID(&Notify->Guid, &Work->Guid))
            continue;
        ExReleaseFastMutex(&QzPnpListLock);
        Notify->Callback(&Notification, Notify->Context);
        ExAcquireFastMutex(&QzPnpListLock);
    }
    ExReleaseFastMutex(&QzPnpListLock);
    ExFreePoolWithTag(Work, QZ_PNP_TAG);
}

static VOID
QzPnpQueueNotification(const GUID *Event, PQZ_INTERFACE Interface)
{
    PQZ_NOTIFY_WORK Work = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Work) + Interface->Name.Length,
                                                 QZ_PNP_TAG);
    if (!Work)
        return;
    Work->Event = *Event;
    Work->Guid = Interface->Guid;
    Work->Name.Buffer = Work->Buffer;
    Work->Name.Length = Interface->Name.Length;
    Work->Name.MaximumLength = Interface->Name.Length + sizeof(WCHAR);
    RtlCopyMemory(Work->Buffer, Interface->Name.Buffer, Interface->Name.Length);
    Work->Buffer[Interface->Name.Length / sizeof(WCHAR)] = UNICODE_NULL;
    ExInitializeWorkItem(&Work->Item, QzPnpNotifyWorker, Work);
    ExQueueWorkItem(&Work->Item, DelayedWorkQueue);
}

static PQZ_INTERFACE
QzPnpFindInterface(PCUNICODE_STRING Name)
{
    PLIST_ENTRY Entry;
    for (Entry = QzPnpInterfaces.Flink; Entry != &QzPnpInterfaces; Entry = Entry->Flink)
    {
        PQZ_INTERFACE Interface = CONTAINING_RECORD(Entry, QZ_INTERFACE, Link);
        if (RtlEqualUnicodeString(&Interface->Name, Name, TRUE))
            return Interface;
    }
    return NULL;
}

NTSTATUS
NTAPI
IoRegisterDeviceInterface(IN PDEVICE_OBJECT PhysicalDeviceObject,
                          IN CONST GUID *InterfaceClassGuid,
                          IN PUNICODE_STRING ReferenceString OPTIONAL,
                          OUT PUNICODE_STRING SymbolicLinkName)
{
    PQZ_DEVICE_NODE Node = QzNodeOf(PhysicalDeviceObject);
    WCHAR Buffer[384], GuidString[40];
    UNICODE_STRING Name;
    PQZ_INTERFACE Interface;
    ULONG i, Prefix, LinkChars;

    if (!Node || !Node->InstancePath.Length)
        return STATUS_INVALID_DEVICE_REQUEST;

    /* \??\<instance path with \ as #>#{guid}[\reference] */
    QzPnpGuidToString(InterfaceClassGuid, GuidString);
    Prefix = (ULONG)_snwprintf(Buffer, RTL_NUMBER_OF(Buffer), L"\\??\\%wZ#%s", &Node->InstancePath,
                               GuidString);
    for (i = 4; i < Prefix; i++)
        if (Buffer[i] == L'\\')
            Buffer[i] = L'#';
    LinkChars = Prefix;
    if (ReferenceString && ReferenceString->Length)
        Prefix += (ULONG)_snwprintf(Buffer + Prefix, RTL_NUMBER_OF(Buffer) - Prefix, L"\\%wZ",
                                    ReferenceString);
    Buffer[RTL_NUMBER_OF(Buffer) - 1] = UNICODE_NULL;
    RtlInitUnicodeString(&Name, Buffer);

    ExAcquireFastMutex(&QzPnpListLock);
    Interface = QzPnpFindInterface(&Name);
    if (!Interface)
    {
        Interface = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Interface) + Name.Length + sizeof(WCHAR),
                                          QZ_PNP_TAG);
        if (!Interface)
        {
            ExReleaseFastMutex(&QzPnpListLock);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Interface->Guid = *InterfaceClassGuid;
        Interface->Pdo = PhysicalDeviceObject;
        Interface->Name.Buffer = (PWSTR)(Interface + 1);
        Interface->Name.Length = Name.Length;
        Interface->Name.MaximumLength = Name.Length + sizeof(WCHAR);
        RtlCopyMemory(Interface->Name.Buffer, Name.Buffer, Name.Length + sizeof(WCHAR));
        Interface->LinkLength = (USHORT)(LinkChars * sizeof(WCHAR));
        Interface->Enabled = FALSE;
        InsertTailList(&QzPnpInterfaces, &Interface->Link);

        /* The link exists from registration on, as ReactOS has it: its
           class drivers (mouclass, kbdclass) open their own interface
           while starting, before they enable it */
        {
            union { OBJECT_NAME_INFORMATION Info; UCHAR Raw[256]; } Target;
            UNICODE_STRING Link;
            ULONG Length;
            Link.Buffer = Interface->Name.Buffer;
            Link.Length = Link.MaximumLength = Interface->LinkLength;
            if (NT_SUCCESS(ObQueryNameString(PhysicalDeviceObject, &Target.Info, sizeof(Target), &Length)) &&
                Target.Info.Name.Length)
                IoCreateSymbolicLink(&Link, &Target.Info.Name);
        }
    }
    ExReleaseFastMutex(&QzPnpListLock);

    /* The caller frees it with RtlFreeUnicodeString */
    return RtlCreateUnicodeString(SymbolicLinkName, Buffer) ? STATUS_SUCCESS
                                                            : STATUS_INSUFFICIENT_RESOURCES;
}

NTSTATUS
NTAPI
IoSetDeviceInterfaceState(IN PUNICODE_STRING SymbolicLinkName,
                          IN BOOLEAN Enable)
{
    union { OBJECT_NAME_INFORMATION Info; UCHAR Raw[256]; } Target;
    PQZ_INTERFACE Interface;
    UNICODE_STRING Link;
    ULONG Length;
    NTSTATUS Status = STATUS_SUCCESS;

    ExAcquireFastMutex(&QzPnpListLock);
    Interface = QzPnpFindInterface(SymbolicLinkName);
    if (!Interface)
    {
        ExReleaseFastMutex(&QzPnpListLock);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (Interface->Enabled == Enable)
    {
        ExReleaseFastMutex(&QzPnpListLock);
        return Enable ? STATUS_OBJECT_NAME_EXISTS : STATUS_SUCCESS;
    }
    Link.Buffer = Interface->Name.Buffer;
    Link.Length = Link.MaximumLength = Interface->LinkLength;
    if (Enable)
    {
        Status = ObQueryNameString(Interface->Pdo, &Target.Info, sizeof(Target), &Length);
        if (NT_SUCCESS(Status) && !Target.Info.Name.Length)
            Status = STATUS_INVALID_DEVICE_REQUEST;   /* PDOs must have names */
        if (NT_SUCCESS(Status))
        {
            Status = IoCreateSymbolicLink(&Link, &Target.Info.Name);
            if (Status == STATUS_OBJECT_NAME_COLLISION)
                Status = STATUS_SUCCESS;   /* another reference string's link */
        }
    }
    else
    {
        IoDeleteSymbolicLink(&Link);
    }
    if (NT_SUCCESS(Status))
    {
        Interface->Enabled = Enable;
        QzPnpQueueNotification(Enable ? &GUID_DEVICE_INTERFACE_ARRIVAL : &GUID_DEVICE_INTERFACE_REMOVAL,
                               Interface);
    }
    ExReleaseFastMutex(&QzPnpListLock);
    return Status;
}

NTSTATUS
NTAPI
IoGetDeviceInterfaces(IN CONST GUID *InterfaceClassGuid,
                      IN PDEVICE_OBJECT PhysicalDeviceObject OPTIONAL,
                      IN ULONG Flags,
                      OUT PWSTR *SymbolicLinkList)
{
    PLIST_ENTRY Entry;
    ULONG Bytes = sizeof(WCHAR);
    PWSTR List, Cursor;

    ExAcquireFastMutex(&QzPnpListLock);
    for (Entry = QzPnpInterfaces.Flink; Entry != &QzPnpInterfaces; Entry = Entry->Flink)
    {
        PQZ_INTERFACE Interface = CONTAINING_RECORD(Entry, QZ_INTERFACE, Link);
        if (IsEqualGUID(&Interface->Guid, InterfaceClassGuid) &&
            (!PhysicalDeviceObject || Interface->Pdo == PhysicalDeviceObject) &&
            (Interface->Enabled || (Flags & DEVICE_INTERFACE_INCLUDE_NONACTIVE)))
            Bytes += Interface->Name.Length + sizeof(WCHAR);
    }
    List = ExAllocatePoolWithTag(PagedPool, Bytes, QZ_PNP_TAG);
    if (!List)
    {
        ExReleaseFastMutex(&QzPnpListLock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Cursor = List;
    for (Entry = QzPnpInterfaces.Flink; Entry != &QzPnpInterfaces; Entry = Entry->Flink)
    {
        PQZ_INTERFACE Interface = CONTAINING_RECORD(Entry, QZ_INTERFACE, Link);
        if (IsEqualGUID(&Interface->Guid, InterfaceClassGuid) &&
            (!PhysicalDeviceObject || Interface->Pdo == PhysicalDeviceObject) &&
            (Interface->Enabled || (Flags & DEVICE_INTERFACE_INCLUDE_NONACTIVE)))
        {
            RtlCopyMemory(Cursor, Interface->Name.Buffer, Interface->Name.Length);
            Cursor += Interface->Name.Length / sizeof(WCHAR);
            *Cursor++ = UNICODE_NULL;
        }
    }
    *Cursor = UNICODE_NULL;
    ExReleaseFastMutex(&QzPnpListLock);
    *SymbolicLinkList = List;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
IoRegisterPlugPlayNotification(IN IO_NOTIFICATION_EVENT_CATEGORY EventCategory,
                               IN ULONG EventCategoryFlags,
                               IN PVOID EventCategoryData OPTIONAL,
                               IN PDRIVER_OBJECT DriverObject,
                               IN PDRIVER_NOTIFICATION_CALLBACK_ROUTINE CallbackRoutine,
                               IN PVOID Context,
                               OUT PVOID *NotificationEntry)
{
    PQZ_NOTIFY Notify = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Notify), QZ_PNP_TAG);
    PLIST_ENTRY Entry;

    if (!Notify)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Notify, sizeof(*Notify));
    Notify->Category = EventCategory;
    if (EventCategory == EventCategoryDeviceInterfaceChange && EventCategoryData)
        Notify->Guid = *(GUID *)EventCategoryData;
    else
        Notify->Target = EventCategoryData;
    Notify->DriverObject = DriverObject;
    Notify->Callback = CallbackRoutine;
    Notify->Context = Context;
    ObReferenceObject(DriverObject);

    ExAcquireFastMutex(&QzPnpListLock);
    InsertTailList(&QzPnpNotifications, &Notify->Link);
    ExReleaseFastMutex(&QzPnpListLock);

    /* Interfaces that are already there, delivered right away */
    if (EventCategory == EventCategoryDeviceInterfaceChange &&
        (EventCategoryFlags & PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES))
    {
        ExAcquireFastMutex(&QzPnpListLock);
        for (Entry = QzPnpInterfaces.Flink; Entry != &QzPnpInterfaces; Entry = Entry->Flink)
        {
            PQZ_INTERFACE Interface = CONTAINING_RECORD(Entry, QZ_INTERFACE, Link);
            DEVICE_INTERFACE_CHANGE_NOTIFICATION Notification;
            if (!Interface->Enabled || !IsEqualGUID(&Interface->Guid, &Notify->Guid))
                continue;
            Notification.Version = 1;
            Notification.Size = sizeof(Notification);
            Notification.Event = GUID_DEVICE_INTERFACE_ARRIVAL;
            Notification.InterfaceClassGuid = Interface->Guid;
            Notification.SymbolicLinkName = &Interface->Name;
            ExReleaseFastMutex(&QzPnpListLock);
            CallbackRoutine(&Notification, Context);
            ExAcquireFastMutex(&QzPnpListLock);
        }
        ExReleaseFastMutex(&QzPnpListLock);
    }

    *NotificationEntry = Notify;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
IoUnregisterPlugPlayNotification(IN PVOID NotificationEntry)
{
    PQZ_NOTIFY Notify = NotificationEntry;
    ExAcquireFastMutex(&QzPnpListLock);
    Notify->Unregistered = TRUE;   /* stays listed: a delivery may be running */
    ExReleaseFastMutex(&QzPnpListLock);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Power IRP helpers (no power policy yet: requests go straight down)      */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
PoCallDriver(IN PDEVICE_OBJECT DeviceObject,
             IN OUT PIRP Irp)
{
    return IofCallDriver(DeviceObject, Irp);
}

VOID
NTAPI
PoStartNextPowerIrp(IN PIRP Irp)
{
    UNREFERENCED_PARAMETER(Irp);
}

POWER_STATE
NTAPI
PoSetPowerState(IN PDEVICE_OBJECT DeviceObject,
                IN POWER_STATE_TYPE Type,
                IN POWER_STATE State)
{
    POWER_STATE Old;
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Type);
    Old.DeviceState = PowerDeviceD0;
    UNREFERENCED_PARAMETER(State);
    return Old;
}

typedef struct _QZ_POWER_REQUEST
{
    PDEVICE_OBJECT DeviceObject;
    UCHAR MinorFunction;
    POWER_STATE PowerState;
    PREQUEST_POWER_COMPLETE Completion;
    PVOID Context;
} QZ_POWER_REQUEST, *PQZ_POWER_REQUEST;

static NTSTATUS NTAPI
QzPoRequestComplete(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    PQZ_POWER_REQUEST Request = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    if (Request->Completion)
        Request->Completion(Request->DeviceObject, Request->MinorFunction, Request->PowerState,
                            Request->Context, &Irp->IoStatus);
    ObDereferenceObject(Request->DeviceObject);
    ExFreePoolWithTag(Request, QZ_PNP_TAG);
    IoFreeIrp(Irp);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
NTAPI
PoRequestPowerIrp(IN PDEVICE_OBJECT DeviceObject,
                  IN UCHAR MinorFunction,
                  IN POWER_STATE PowerState,
                  IN PREQUEST_POWER_COMPLETE CompletionFunction OPTIONAL,
                  IN PVOID Context OPTIONAL,
                  OUT PIRP *pIrp OPTIONAL)
{
    PDEVICE_OBJECT Top;
    PQZ_POWER_REQUEST Request;
    PIO_STACK_LOCATION Stack;
    PIRP Irp;

    if (MinorFunction != IRP_MN_SET_POWER && MinorFunction != IRP_MN_QUERY_POWER &&
        MinorFunction != IRP_MN_WAIT_WAKE)
        return STATUS_INVALID_PARAMETER_2;

    Request = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Request), QZ_PNP_TAG);
    if (!Request)
        return STATUS_INSUFFICIENT_RESOURCES;
    Top = IoGetAttachedDeviceReference(DeviceObject);
    Irp = IoAllocateIrp(Top->StackSize, FALSE);
    if (!Irp)
    {
        ObDereferenceObject(Top);
        ExFreePoolWithTag(Request, QZ_PNP_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ObReferenceObject(DeviceObject);
    Request->DeviceObject = DeviceObject;
    Request->MinorFunction = MinorFunction;
    Request->PowerState = PowerState;
    Request->Completion = CompletionFunction;
    Request->Context = Context;

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = IRP_MJ_POWER;
    Stack->MinorFunction = MinorFunction;
    if (MinorFunction == IRP_MN_WAIT_WAKE)
    {
        Stack->Parameters.WaitWake.PowerState = PowerState.SystemState;
    }
    else
    {
        Stack->Parameters.Power.Type = DevicePowerState;
        Stack->Parameters.Power.State = PowerState;
    }
    /* Our completion routine in the bottom location runs last */
    /* In the top driver's location: it runs last, after every driver */
    IoSetCompletionRoutine(Irp, QzPoRequestComplete, Request, TRUE, TRUE, TRUE);
    if (pIrp)
        *pIrp = Irp;
    PoCallDriver(Top, Irp);
    ObDereferenceObject(Top);
    return STATUS_PENDING;
}

PULONG
NTAPI
PoRegisterDeviceForIdleDetection(IN PDEVICE_OBJECT DeviceObject,
                                 IN ULONG ConservationIdleTime,
                                 IN ULONG PerformanceIdleTime,
                                 IN DEVICE_POWER_STATE State)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(ConservationIdleTime);
    UNREFERENCED_PARAMETER(PerformanceIdleTime);
    UNREFERENCED_PARAMETER(State);
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Start-up and the shell's view                                           */
/* ---------------------------------------------------------------------- */

VOID
QzPnpInit(VOID)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Driver\\PnpManager");

    KeInitializeMutex(&QzPnpLock, 0);
    ExInitializeFastMutex(&QzPnpListLock);
    if (!NT_SUCCESS(IoCreateDriver(&Name, QzPnpRootEntry)) || !QzPnpRootDevice)
        KeBugCheckEx(PNP_DETECTED_FATAL_ERROR, 1, 0, 0, 0);
    QzPnpRootDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    QzPnpRoot = QzPnpCreateNode(NULL, QzPnpRootDevice);
    QzPnpRoot->Flags = QZ_DN_IDS | QZ_DN_ADDED | QZ_DN_STARTED;
    QzPnpSetString(&QzPnpRoot->InstancePath, L"HTREE\\ROOT\\0");
}

/* HalInitPnpDriver is a macro over HALDISPATCH; use the field */
#undef HalInitPnpDriver

/* Bring up the device tree: the HAL's PnP driver, then everything below */
/* What the session manager does once the volumes are up: ask the mount
   manager to give every volume without one a drive letter */
static VOID
QzPnpAssignDriveLetters(VOID)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(MOUNTMGR_DEVICE_NAME);
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT Device;
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;

    Status = IoGetDeviceObjectPointer(&Name, FILE_READ_ATTRIBUTES, &FileObject, &Device);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(QZ_KERNEL_NAME ": PnP: no mount manager (0x%08lx)\n", Status);
        return;
    }
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(IOCTL_MOUNTMGR_AUTO_DL_ASSIGNMENTS, Device, NULL, 0, NULL, 0,
                                        FALSE, &Event, &IoStatus);
    if (Irp)
    {
        Status = IoCallDriver(Device, Irp);
        if (Status == STATUS_PENDING)
        {
            KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
            Status = IoStatus.Status;
        }
    }
    else
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }
    DbgPrint(QZ_KERNEL_NAME ": PnP: drive letter assignment 0x%08lx\n", Status);
    ObDereferenceObject(FileObject);
}

/* Read sector 0 of disk N straight from its device (no file system mounts) */
static NTSTATUS
QzPnpReadMbr(ULONG Disk, PULONG Mbr)
{
    WCHAR Path[64];
    UNICODE_STRING Name;
    PDEVICE_OBJECT Device, Top;
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Offset;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;

    _snwprintf(Path, RTL_NUMBER_OF(Path), L"\\Device\\Harddisk%lu\\DR%lu", Disk, Disk);
    RtlInitUnicodeString(&Name, Path);
    Status = ObReferenceObjectByName(&Name, OBJ_CASE_INSENSITIVE, NULL, 0, IoDeviceObjectType, KernelMode, NULL,
                                     (PVOID *)&Device);
    if (!NT_SUCCESS(Status))
        return Status;
    Top = IoGetAttachedDeviceReference(Device);
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Offset.QuadPart = 0;
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ, Top, Mbr, 512, &Offset, &Event, &IoStatus);
    if (Irp)
    {
        IoGetNextIrpStackLocation(Irp)->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
        Status = QzIoSynchronousCall(Top, Irp, &Event, &IoStatus);
    }
    else
        Status = STATUS_INSUFFICIENT_RESOURCES;
    ObDereferenceObject(Top);
    ObDereferenceObject(Device);
    return Status;
}

/*
 * The NT disk that is the loader's boot disk: the loader recorded each BIOS
 * disk's MBR signature and checksum, so compare them with what each
 * \Device\HarddiskN holds (IopCreateArcNames does the same).
 */
static LONG
QzPnpFindArcDisk(PCSTR BootName)
{
    PARC_DISK_INFORMATION Info = QzLoaderBlock->ArcDiskInformation;
    PARC_DISK_SIGNATURE Arc = NULL;
    PLIST_ENTRY Entry;
    PULONG Mbr;
    LONG Found = -1;
    ULONG Disk, i;

    if (!Info)
        return -1;
    for (Entry = Info->DiskSignatureListHead.Flink; Entry != &Info->DiskSignatureListHead; Entry = Entry->Flink)
    {
        PARC_DISK_SIGNATURE Candidate = CONTAINING_RECORD(Entry, ARC_DISK_SIGNATURE, ListEntry);
        if (!_strnicmp(Candidate->ArcName, BootName, strlen(Candidate->ArcName)))
        {
            Arc = Candidate;
            break;
        }
    }
    if (!Arc)
        return -1;
    Mbr = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, QZ_PNP_TAG);
    if (!Mbr)
        return -1;
    for (Disk = 0; Disk < 16 && Found < 0; Disk++)
    {
        ULONG CheckSum = 0;
        if (!NT_SUCCESS(QzPnpReadMbr(Disk, Mbr)))
            continue;
        for (i = 0; i < 512 / sizeof(ULONG); i++)
            CheckSum += Mbr[i];
        if (Mbr[0x1B8 / sizeof(ULONG)] == Arc->Signature && CheckSum + Arc->CheckSum == 0)
            Found = (LONG)Disk;
    }
    ExFreePoolWithTag(Mbr, QZ_PNP_TAG);
    return Found;
}

/*
 * \ArcName\<boot device> -> the NT device, so that \SystemRoot (which the
 * object manager made point at the ARC name) reaches the boot volume. A CD
 * is the first CD-ROM; a disk (a USB stick, too) is found by its MBR, and
 * may take a few seconds to appear while its bus enumerates.
 */
static VOID
QzPnpCreateArcNames(VOID)
{
    PCSTR Arc = QzLoaderBlock ? QzLoaderBlock->ArcBootDeviceName : NULL;
    WCHAR LinkBuffer[128], TargetBuffer[64];
    UNICODE_STRING Link, Target;
    PCSTR p;
    ULONG i, Partition = 0;
    LONG Disk = -1;
    NTSTATUS Status;

    if (!Arc)
        return;
    for (i = 0; Arc[i] && i < 100; i++)
        LinkBuffer[9 + i] = (UCHAR)Arc[i];
    RtlCopyMemory(LinkBuffer, L"\\ArcName\\", 9 * sizeof(WCHAR));
    LinkBuffer[9 + i] = UNICODE_NULL;
    RtlInitUnicodeString(&Link, LinkBuffer);

    if (strstr(Arc, "cdrom("))
    {
        RtlCopyMemory(TargetBuffer, L"\\Device\\CdRom0", sizeof(L"\\Device\\CdRom0"));
    }
    else
    {
        LARGE_INTEGER Delay;
        Delay.QuadPart = -5000000LL;   /* 0.5 s */
        for (i = 0; i < 20 && Disk < 0; i++)
        {
            Disk = QzPnpFindArcDisk(Arc);
            if (Disk < 0)
                KeDelayExecutionThread(KernelMode, FALSE, &Delay);
        }
        if (Disk < 0 && (p = strstr(Arc, "rdisk(")))
        {
            Disk = (LONG)strtoul(p + 6, NULL, 10);   /* no match: BIOS order */
            DbgPrint(QZ_KERNEL_NAME ": PnP: the boot disk was not found by its MBR; guessing disk %ld\n", Disk);
        }
        if ((p = strstr(Arc, "partition(")))
            Partition = strtoul(p + 10, NULL, 10);
        _snwprintf(TargetBuffer, RTL_NUMBER_OF(TargetBuffer), L"\\Device\\Harddisk%ld\\Partition%lu",
                   Disk < 0 ? 0 : Disk, Partition);
    }
    RtlInitUnicodeString(&Target, TargetBuffer);
    Status = IoCreateSymbolicLink(&Link, &Target);
    DbgPrint(QZ_KERNEL_NAME ": PnP: %wZ -> %wZ (0x%08lx)\n", &Link, &Target, Status);
}

BOOLEAN
QzPnpIsStarted(VOID)
{
    return QzPnpStarted;
}

NTSTATUS
QzPnpStart(VOID)
{
    NTSTATUS Status;

    if (QzPnpStarted)
        return STATUS_SUCCESS;
    QzPnpStarted = TRUE;

    /* The HAL builds its bus handlers (PCI configuration access among them)
       when the I/O manager asks it to report its resources */
    HalReportResourceUsage();

    DbgPrint(QZ_KERNEL_NAME ": PnP: starting the HAL's PnP driver\n");
    Status = HalDispatchTable.HalInitPnpDriver ? HalDispatchTable.HalInitPnpDriver()
                                                      : STATUS_NOT_IMPLEMENTED;
    if (!NT_SUCCESS(Status))
        return Status;

    QzPnpLockTree();
    QzPnpProcessNode(QzPnpRoot);
    QzPnpUnlockTree();

    /* The boot drivers no device asked for (ndis, ...) */
    QzIoStartBootDrivers();

    /* The mount manager (a legacy boot driver NT always starts) picks up
       the volumes that are there now and gives them their names */
    {
        UNICODE_STRING MountMgr = RTL_CONSTANT_STRING(L"mountmgr");
        PDRIVER_OBJECT Driver;
        if (NT_SUCCESS(QzIoLoadDriver(&MountMgr, &Driver)))
            QzPnpAssignDriveLetters();
    }

    /* The boot volume: an ARC name for \SystemRoot, and a file system for the media */
    QzIsoInit();
    QzIoStartBootFileSystems();       /* registered after QzIso: tried before it */
    QzPnpCreateArcNames();

    /* Now drivers can come from \SystemRoot\System32\drivers */
    QzPnpLockTree();
    QzPnpBootPhase = FALSE;
    QzPnpRetryDeferred(QzPnpRoot);
    QzPnpUnlockTree();
    QzIoStartSystemFileSystems();

    QzIoCallReinitRoutines(TRUE);
    QzIoCallReinitRoutines(FALSE);
    return STATUS_SUCCESS;
}

ULONG
QzPnpDeviceCount(PULONG Started)
{
    *Started = QzPnpStartedCount;
    return QzPnpNodeCount;
}

static VOID
QzPnpDumpNode(PQZ_DEVICE_NODE Node, BOOLEAN Verbose)
{
    PQZ_DEVICE_NODE Child;
    PCSTR State = (Node->Flags & QZ_DN_STARTED) ? "started" :
                  (Node->Flags & QZ_DN_FAILED) ? "FAILED" :
                  (Node->Flags & QZ_DN_NO_DRIVER) ? (Node->ServiceName.Length ? "not loaded" : "no driver") :
                  (Node->Flags & QZ_DN_ADDED) ? "added" : "found";
    ULONG i;

    for (i = 0; i < Node->Level; i++)
        QzPrint("  ");
    if (Node->ServiceName.Length)
        QzPrint("  %wZ [%s, %wZ]\n", &Node->InstancePath, State, &Node->ServiceName);
    else
        QzPrint("  %wZ [%s]\n", &Node->InstancePath, State);
    if (Verbose && Node->ProblemText && (Node->Flags & QZ_DN_NO_DRIVER))
    {
        for (i = 0; i < Node->Level; i++)
            QzPrint("  ");
        QzPrint("      (%s, 0x%08lx)\n", Node->ProblemText, Node->Problem);
    }
    if (Verbose && Node->Description)
    {
        for (i = 0; i < Node->Level; i++)
            QzPrint("  ");
        QzPrint("      \"%S\"\n", Node->Description);
    }
    if (Verbose && (Node->Flags & QZ_DN_FAILED))
    {
        for (i = 0; i < Node->Level; i++)
            QzPrint("  ");
        QzPrint("      problem: %s (0x%08lx)\n", Node->ProblemText, Node->Problem);
    }
    if (Verbose && Node->TranslatedResources && Node->TranslatedResources->Count)
    {
        PCM_PARTIAL_RESOURCE_LIST Partial = &Node->TranslatedResources->List[0].PartialResourceList;
        for (i = 0; i < Partial->Count; i++)
        {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = &Partial->PartialDescriptors[i];
            ULONG k;
            for (k = 0; k < Node->Level; k++)
                QzPrint("  ");
            switch (Desc->Type)
            {
                case CmResourceTypePort:
                    QzPrint("      I/O    %04lx-%04lx\n", Desc->u.Port.Start.LowPart,
                            Desc->u.Port.Start.LowPart + Desc->u.Port.Length - 1);
                    break;
                case CmResourceTypeMemory:
                    QzPrint("      memory %08lx-%08lx\n", Desc->u.Memory.Start.LowPart,
                            Desc->u.Memory.Start.LowPart + Desc->u.Memory.Length - 1);
                    break;
                case CmResourceTypeInterrupt:
                    QzPrint("      IRQ    vector %02lx, IRQL %lu\n", Desc->u.Interrupt.Vector,
                            Desc->u.Interrupt.Level);
                    break;
                case CmResourceTypeBusNumber:
                    QzPrint("      bus    %lu-%lu\n", Desc->u.BusNumber.Start,
                            Desc->u.BusNumber.Start + Desc->u.BusNumber.Length - 1);
                    break;
                default:
                    QzPrint("      type %u\n", Desc->Type);
                    break;
            }
        }
    }
    for (Child = Node->Child; Child; Child = Child->Sibling)
        QzPnpDumpNode(Child, Verbose);
}

VOID
QzPnpDumpTree(BOOLEAN Verbose)
{
    QzPnpLockTree();
    QzPnpDumpNode(QzPnpRoot, Verbose);
    QzPnpUnlockTree();
    QzPrint("  %lu device node(s), %lu started\n", QzPnpNodeCount, QzPnpStartedCount);
}

/* ---------------------------------------------------------------------- */
/* IoConnectInterruptEx: the Vista+ interface (KMDF and storport use it)   */
/* ---------------------------------------------------------------------- */

/* The first interrupt the PnP manager gave the device */
static PCM_PARTIAL_RESOURCE_DESCRIPTOR
QzPnpFindInterrupt(PDEVICE_OBJECT Pdo)
{
    PQZ_DEVICE_NODE Node = Pdo ? QzNodeOf(Pdo) : NULL;
    PCM_PARTIAL_RESOURCE_LIST List;
    ULONG i;

    if (!Node || !Node->TranslatedResources || !Node->TranslatedResources->Count)
        return NULL;
    List = &Node->TranslatedResources->List[0].PartialResourceList;
    for (i = 0; i < List->Count; i++)
        if (List->PartialDescriptors[i].Type == CmResourceTypeInterrupt)
            return &List->PartialDescriptors[i];
    return NULL;
}

NTSTATUS
NTAPI
IoConnectInterruptEx(IN OUT PIO_CONNECT_INTERRUPT_PARAMETERS Parameters)
{
    switch (Parameters->Version)
    {
        case CONNECT_FULLY_SPECIFIED:
        case CONNECT_FULLY_SPECIFIED_GROUP:
        {
            PIO_CONNECT_INTERRUPT_FULLY_SPECIFIED_PARAMETERS P = &Parameters->FullySpecified;
            if (P->Group != 0)
                return STATUS_INVALID_PARAMETER;   /* one processor group */
            return IoConnectInterrupt(P->InterruptObject, P->ServiceRoutine, P->ServiceContext, P->SpinLock,
                                      P->Vector, P->Irql, P->SynchronizeIrql, P->InterruptMode, P->ShareVector,
                                      P->ProcessorEnableMask, P->FloatingSave);
        }

        case CONNECT_MESSAGE_BASED:
        {
            /* No MSI yet: connect the line instead, as NT does when the
               device has no messages and a fallback routine is given */
            IO_CONNECT_INTERRUPT_MESSAGE_BASED_PARAMETERS Message = Parameters->MessageBased;
            if (!Message.FallBackServiceRoutine)
                return STATUS_NOT_SUPPORTED;
            Parameters->Version = CONNECT_LINE_BASED;
            Parameters->LineBased.PhysicalDeviceObject = Message.PhysicalDeviceObject;
            Parameters->LineBased.InterruptObject = Message.ConnectionContext.InterruptObject;
            Parameters->LineBased.ServiceRoutine = Message.FallBackServiceRoutine;
            Parameters->LineBased.ServiceContext = Message.ServiceContext;
            Parameters->LineBased.SpinLock = Message.SpinLock;
            Parameters->LineBased.SynchronizeIrql = Message.SynchronizeIrql;
            Parameters->LineBased.FloatingSave = Message.FloatingSave;
        }
        /* fall through */

        case CONNECT_LINE_BASED:
        {
            PIO_CONNECT_INTERRUPT_LINE_BASED_PARAMETERS P = &Parameters->LineBased;
            PCM_PARTIAL_RESOURCE_DESCRIPTOR Interrupt = QzPnpFindInterrupt(P->PhysicalDeviceObject);
            KIRQL Irql;
            if (!Interrupt)
                return STATUS_NOT_FOUND;
            Irql = (KIRQL)Interrupt->u.Interrupt.Level;
            return IoConnectInterrupt(P->InterruptObject, P->ServiceRoutine, P->ServiceContext, P->SpinLock,
                                      Interrupt->u.Interrupt.Vector, Irql,
                                      P->SynchronizeIrql > Irql ? P->SynchronizeIrql : Irql,
                                      (Interrupt->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? Latched : LevelSensitive,
                                      Interrupt->ShareDisposition == CmResourceShareShared,
                                      Interrupt->u.Interrupt.Affinity, P->FloatingSave);
        }

        default:
            return STATUS_INVALID_PARAMETER;
    }
}

VOID
NTAPI
IoDisconnectInterruptEx(IN PIO_DISCONNECT_INTERRUPT_PARAMETERS Parameters)
{
    if (Parameters->ConnectionContext.InterruptObject)
        IoDisconnectInterrupt(Parameters->ConnectionContext.InterruptObject);
}

/* ---------------------------------------------------------------------- */
/* User-mode PnP: NtPlugPlayControl and NtGetPlugPlayEvent (umpnpmgr,     */
/* setupapi, cfgmgr32)                                                     */
/* ---------------------------------------------------------------------- */

#include <ndk/cmtypes.h>

#define QZ_PNP_MAX_INSTANCE 512         /* bytes of an instance path from user mode */

typedef struct _QZ_PNP_EVENT
{
    LIST_ENTRY Link;
    ULONG Size;
    PLUGPLAY_EVENT_BLOCK Block;         /* variable length */
} QZ_PNP_EVENT, *PQZ_PNP_EVENT;

static LIST_ENTRY QzPnpEvents = { &QzPnpEvents, &QzPnpEvents };
static KEVENT QzPnpEventPending;
static BOOLEAN QzPnpEventsWanted;       /* umpnpmgr listens: new devices get install events */

static const GUID QzGuidDeviceEnumerated =
    { 0xCB3A400A, 0x46F0, 0x11D0, { 0xB0, 0x8F, 0x00, 0x60, 0x97, 0x13, 0x05, 0x3F } };

/* A device that needs a driver installed, for umpnpmgr */
static VOID
QzPnpQueueInstallEvent(PQZ_DEVICE_NODE Node)
{
    ULONG Size = FIELD_OFFSET(PLUGPLAY_EVENT_BLOCK, InstallDevice.DeviceId) + Node->InstancePath.Length +
                 sizeof(WCHAR);
    PQZ_PNP_EVENT Event;

    if (!QzPnpEventsWanted)
        return;
    Event = ExAllocatePoolWithTag(NonPagedPool, FIELD_OFFSET(QZ_PNP_EVENT, Block) + Size, QZ_PNP_TAG);
    if (!Event)
        return;
    RtlZeroMemory(&Event->Block, Size);
    Event->Size = Size;
    Event->Block.EventGuid = QzGuidDeviceEnumerated;
    Event->Block.EventCategory = DeviceInstallEvent;
    Event->Block.TotalSize = Size;
    RtlCopyMemory(Event->Block.InstallDevice.DeviceId, Node->InstancePath.Buffer, Node->InstancePath.Length);
    ExAcquireFastMutex(&QzPnpListLock);
    InsertTailList(&QzPnpEvents, &Event->Link);
    KeSetEvent(&QzPnpEventPending, IO_NO_INCREMENT, FALSE);
    ExReleaseFastMutex(&QzPnpListLock);
}

/* The node with this instance path (tree locked) */
static PQZ_DEVICE_NODE
QzPnpFindInstance(PQZ_DEVICE_NODE Node, PCUNICODE_STRING Instance)
{
    PQZ_DEVICE_NODE Child, Found;

    if (RtlEqualUnicodeString(&Node->InstancePath, Instance, TRUE))
        return Node;
    for (Child = Node->Child; Child; Child = Child->Sibling)
        if ((Found = QzPnpFindInstance(Child, Instance)) != NULL)
            return Found;
    return NULL;
}

/* An instance path from the caller's UNICODE_STRING, into Buffer */
static NTSTATUS
QzPnpCaptureInstance(KPROCESSOR_MODE Mode, PCUNICODE_STRING From, PUNICODE_STRING To, PWCHAR Buffer)
{
    if (!From->Length || From->Length > QZ_PNP_MAX_INSTANCE || (From->Length & 1))
        return STATUS_INVALID_PARAMETER;
    if (!QzReadArg(Mode, Buffer, From->Buffer, From->Length))
        return STATUS_ACCESS_VIOLATION;
    To->Buffer = Buffer;
    To->Length = To->MaximumLength = From->Length;
    return STATUS_SUCCESS;
}

/* NT's DN_* status bits and CM_PROB_* problem of a node */
static VOID
QzPnpNodeStatus(PQZ_DEVICE_NODE Node, PULONG Status, PULONG Problem)
{
    *Status = 0x00800000 /* DN_NT_ENUMERATOR */ | 0x01000000 /* DN_NT_DRIVER */;
    *Problem = 0;
    if (Node->Flags & QZ_DN_ADDED)
        *Status |= 0x00000002;          /* DN_DRIVER_LOADED */
    if (Node->Flags & QZ_DN_STARTED)
        *Status |= 0x00000008;          /* DN_STARTED */
    if (Node->Flags & QZ_DN_NO_DRIVER)
    {
        *Status |= 0x00000400;          /* DN_HAS_PROBLEM */
        *Problem = 28;                  /* CM_PROB_FAILED_INSTALL: no driver installed */
    }
    else if (Node->Flags & QZ_DN_FAILED)
    {
        *Status |= 0x00000400;
        *Problem = 10;                  /* CM_PROB_FAILED_START */
    }
    if (!Node->Parent || Node->Parent == QzPnpRoot)
        *Status |= 0x00000001;          /* DN_ROOT_ENUMERATED */
}

/* MULTI_SZ out to the caller: Bytes in and out, STATUS_BUFFER_TOO_SMALL if it does not fit */
static NTSTATUS
QzPnpReturnMultiSz(KPROCESSOR_MODE Mode, PCWSTR List, ULONG Needed, PVOID Buffer, PULONG Bytes)
{
    ULONG Have = *Bytes;
    *Bytes = Needed;
    if (!Buffer || Have < Needed)
        return STATUS_BUFFER_TOO_SMALL;
    return QzWriteArg(Mode, Buffer, List, Needed) ? STATUS_SUCCESS : STATUS_ACCESS_VIOLATION;
}

NTSTATUS
NTAPI
NtPlugPlayControl(IN PLUGPLAY_CONTROL_CLASS PlugPlayControlClass, IN OUT PVOID Buffer, IN ULONG BufferLength)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    union
    {
        PLUGPLAY_CONTROL_DEVICE_CONTROL_DATA Control;
        PLUGPLAY_CONTROL_ENUMERATE_DEVICE_DATA Enumerate;
        PLUGPLAY_CONTROL_STATUS_DATA Status;
        PLUGPLAY_CONTROL_DEPTH_DATA Depth;
        PLUGPLAY_CONTROL_RELATED_DEVICE_DATA Related;
        PLUGPLAY_CONTROL_PROPERTY_DATA Property;
        PLUGPLAY_CONTROL_DEVICE_RELATIONS_DATA Relations;
        PLUGPLAY_CONTROL_INTERFACE_DEVICE_LIST_DATA List;
        PLUGPLAY_CONTROL_CLASS_ASSOCIATION_DATA Association;
        PLUGPLAY_CONTROL_QUERY_REMOVE_DATA Remove;
        UCHAR Bytes[64];
    } Data;
    WCHAR InstanceBuffer[QZ_PNP_MAX_INSTANCE / sizeof(WCHAR)];
    UNICODE_STRING Instance;
    PQZ_DEVICE_NODE Node = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    if (BufferLength > sizeof(Data) || (BufferLength && !QzReadArg(Mode, &Data, Buffer, BufferLength)))
        return BufferLength > sizeof(Data) ? STATUS_INVALID_PARAMETER : STATUS_ACCESS_VIOLATION;

    /* Most classes start with the device instance */
    switch (PlugPlayControlClass)
    {
        case PlugPlayControlEnumerateDevice:
        case PlugPlayControlInitializeDevice:
        case PlugPlayControlStartDevice:
        case PlugPlayControlResetDevice:
        case PlugPlayControlDeviceStatus:
        case PlugPlayControlGetDeviceDepth:
        case PlugPlayControlProperty:
        case PlugPlayControlQueryDeviceRelations:
        case PlugPlayControlDeviceClassAssociation:
        case PlugPlayControlQueryAndRemoveDevice:
            if (BufferLength < sizeof(UNICODE_STRING))
                return STATUS_INVALID_PARAMETER;
            Status = QzPnpCaptureInstance(Mode, &Data.Control.DeviceInstance, &Instance, InstanceBuffer);
            break;
        case PlugPlayControlGetRelatedDevice:
            if (BufferLength < sizeof(Data.Related))
                return STATUS_INVALID_PARAMETER;
            Status = QzPnpCaptureInstance(Mode, &Data.Related.TargetDeviceInstance, &Instance, InstanceBuffer);
            break;
        case PlugPlayControlGetInterfaceDeviceList:
            if (BufferLength < sizeof(Data.List))
                return STATUS_INVALID_PARAMETER;
            if (Data.List.DeviceInstance.Length)
                Status = QzPnpCaptureInstance(Mode, &Data.List.DeviceInstance, &Instance, InstanceBuffer);
            else
                Instance.Length = 0;
            break;
        default:
            Instance.Length = 0;
            break;
    }
    if (!NT_SUCCESS(Status))
        return Status;

    QzPnpLockTree();
    if (Instance.Length)
    {
        Node = QzPnpFindInstance(QzPnpRoot, &Instance);
        if (!Node)
        {
            QzPnpUnlockTree();
            return STATUS_NO_SUCH_DEVICE;
        }
    }

    switch (PlugPlayControlClass)
    {
        case PlugPlayControlEnumerateDevice:
            if (Node->Flags & QZ_DN_STARTED)
                IoInvalidateDeviceRelations(Node->Pdo, BusRelations);
            break;

        case PlugPlayControlInitializeDevice:
            break;                      /* the node exists: nothing more to set up */

        case PlugPlayControlStartDevice:
        case PlugPlayControlResetDevice:
            /* A driver was installed for it (umpnpmgr, newdev): try again */
            if (!(Node->Flags & QZ_DN_STARTED))
            {
                Node->Flags &= ~(QZ_DN_NO_DRIVER | QZ_DN_FAILED | QZ_DN_DEFERRED);
                Node->Problem = STATUS_SUCCESS;
                Node->ProblemText = NULL;
                QzPnpProcessNode(Node);
                if (!(Node->Flags & QZ_DN_STARTED))
                    Status = (Node->Flags & QZ_DN_NO_DRIVER) ? STATUS_NO_SUCH_DEVICE : STATUS_UNSUCCESSFUL;
            }
            break;

        case PlugPlayControlDeviceStatus:
            if (Data.Status.Operation == PNP_GET_DEVICE_STATUS)
            {
                ULONG DeviceStatus, Problem;
                QzPnpNodeStatus(Node, &DeviceStatus, &Problem);
                Data.Status.DeviceStatus = DeviceStatus;
                Data.Status.DeviceProblem = Problem;
                if (!QzWriteArg(Mode, Buffer, &Data.Status, sizeof(Data.Status)))
                    Status = STATUS_ACCESS_VIOLATION;
            }
            break;

        case PlugPlayControlGetDeviceDepth:
            Data.Depth.Depth = Node->Level;
            if (!QzWriteArg(Mode, Buffer, &Data.Depth, sizeof(Data.Depth)))
                Status = STATUS_ACCESS_VIOLATION;
            break;

        case PlugPlayControlGetRelatedDevice:
        {
            PQZ_DEVICE_NODE Related = Data.Related.Relation == PNP_GET_PARENT_DEVICE ? Node->Parent
                                    : Data.Related.Relation == PNP_GET_CHILD_DEVICE ? Node->Child
                                    : Data.Related.Relation == PNP_GET_SIBLING_DEVICE ? Node->Sibling : NULL;
            if (!Related)
            {
                Status = STATUS_NO_SUCH_DEVICE;
                break;
            }
            if (Data.Related.RelatedDeviceInstanceLength < Related->InstancePath.Length)
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            Data.Related.RelatedDeviceInstanceLength = Related->InstancePath.Length;
            if (!QzWriteArg(Mode, Data.Related.RelatedDeviceInstance, Related->InstancePath.Buffer,
                            Related->InstancePath.Length) ||
                !QzWriteArg(Mode, Buffer, &Data.Related, sizeof(Data.Related)))
                Status = STATUS_ACCESS_VIOLATION;
            break;
        }

        case PlugPlayControlProperty:
        {
            static const DEVICE_REGISTRY_PROPERTY Map[] =
            {
                DevicePropertyUINumber, DevicePropertyPhysicalDeviceObjectName, DevicePropertyBusTypeGuid,
                DevicePropertyLegacyBusType, DevicePropertyBusNumber, (DEVICE_REGISTRY_PROPERTY)-1,
                DevicePropertyRemovalPolicy, (DEVICE_REGISTRY_PROPERTY)-1, DevicePropertyAddress,
                DevicePropertyEnumeratorName, DevicePropertyRemovalPolicy, DevicePropertyInstallState,
            };
            ULONG Size = Data.Property.BufferSize, Result = 0;
            PVOID Local;
            if (Data.Property.Property >= RTL_NUMBER_OF(Map) || Map[Data.Property.Property] == (DEVICE_REGISTRY_PROPERTY)-1)
            {
                Status = STATUS_NOT_SUPPORTED;
                break;
            }
            Local = ExAllocatePoolWithTag(NonPagedPool, max(Size, 1), QZ_PNP_TAG);
            if (!Local)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            Status = IoGetDeviceProperty(Node->Pdo, Map[Data.Property.Property], Size, Local, &Result);
            if (NT_SUCCESS(Status) && !QzWriteArg(Mode, Data.Property.Buffer, Local, Result))
                Status = STATUS_ACCESS_VIOLATION;
            ExFreePoolWithTag(Local, QZ_PNP_TAG);
            Data.Property.BufferSize = Result;
            QzWriteArg(Mode, Buffer, &Data.Property, sizeof(Data.Property));
            break;
        }

        case PlugPlayControlQueryDeviceRelations:
        {
            PQZ_DEVICE_NODE Child;
            ULONG Needed = sizeof(WCHAR), Used = 0;
            PWCHAR List;
            if (Data.Relations.Relations == PNP_BUS_RELATIONS)
                for (Child = Node->Child; Child; Child = Child->Sibling)
                    Needed += Child->InstancePath.Length + sizeof(WCHAR);
            List = ExAllocatePoolWithTag(NonPagedPool, Needed, QZ_PNP_TAG);
            if (!List)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            if (Data.Relations.Relations == PNP_BUS_RELATIONS)
            {
                for (Child = Node->Child; Child; Child = Child->Sibling)
                {
                    RtlCopyMemory((PUCHAR)List + Used, Child->InstancePath.Buffer, Child->InstancePath.Length);
                    Used += Child->InstancePath.Length;
                    *(PWCHAR)((PUCHAR)List + Used) = UNICODE_NULL;
                    Used += sizeof(WCHAR);
                }
            }
            *(PWCHAR)((PUCHAR)List + Used) = UNICODE_NULL;
            if (Needed == sizeof(WCHAR))
                Needed = 0;             /* no relations: an empty answer, as NT gives */
            Status = QzPnpReturnMultiSz(Mode, List, Needed, Data.Relations.Buffer, &Data.Relations.BufferSize);
            if (!Needed)
                Status = STATUS_SUCCESS;
            ExFreePoolWithTag(List, QZ_PNP_TAG);
            QzWriteArg(Mode, Buffer, &Data.Relations, sizeof(Data.Relations));
            break;
        }

        case PlugPlayControlGetInterfaceDeviceList:
        {
            GUID Filter;
            PWSTR Links, End;
            if (!QzReadArg(Mode, &Filter, Data.List.FilterGuid, sizeof(Filter)))
            {
                Status = STATUS_ACCESS_VIOLATION;
                break;
            }
            QzPnpUnlockTree();
            Status = IoGetDeviceInterfaces(&Filter, Node ? Node->Pdo : NULL, Data.List.Flags, &Links);
            QzPnpLockTree();
            if (!NT_SUCCESS(Status))
                break;
            for (End = Links; *End; End += wcslen(End) + 1)
                ;
            Status = QzPnpReturnMultiSz(Mode, Links, (ULONG)(End - Links + 1) * sizeof(WCHAR), Data.List.Buffer,
                                        &Data.List.BufferSize);
            ExFreePool(Links);
            QzWriteArg(Mode, Buffer, &Data.List, sizeof(Data.List));
            break;
        }

        case PlugPlayControlDeviceClassAssociation:
        {
            GUID InterfaceGuid;
            UNICODE_STRING Link, Reference;
            WCHAR ReferenceBuffer[64];
            if (!Data.Association.Register)
                break;                  /* interfaces go when their device goes */
            if (!QzReadArg(Mode, &InterfaceGuid, Data.Association.InterfaceGuid, sizeof(InterfaceGuid)))
            {
                Status = STATUS_ACCESS_VIOLATION;
                break;
            }
            RtlInitEmptyUnicodeString(&Reference, ReferenceBuffer, sizeof(ReferenceBuffer));
            if (Data.Association.Reference.Length)
            {
                if (Data.Association.Reference.Length > sizeof(ReferenceBuffer) ||
                    !QzReadArg(Mode, ReferenceBuffer, Data.Association.Reference.Buffer,
                               Data.Association.Reference.Length))
                {
                    Status = STATUS_INVALID_PARAMETER;
                    break;
                }
                Reference.Length = Data.Association.Reference.Length;
            }
            QzPnpUnlockTree();
            Status = IoRegisterDeviceInterface(Node->Pdo, &InterfaceGuid, Reference.Length ? &Reference : NULL,
                                               &Link);
            QzPnpLockTree();
            if (!NT_SUCCESS(Status))
                break;
            if (Data.Association.SymbolicLinkNameLength < Link.Length + sizeof(WCHAR))
                Status = STATUS_BUFFER_TOO_SMALL;
            else if (!QzWriteArg(Mode, Data.Association.SymbolicLinkName, Link.Buffer, Link.Length) ||
                     !QzWriteArg(Mode, (PUCHAR)Data.Association.SymbolicLinkName + Link.Length, L"", sizeof(WCHAR)))
                Status = STATUS_ACCESS_VIOLATION;
            Data.Association.SymbolicLinkNameLength = Link.Length + sizeof(WCHAR);
            QzWriteArg(Mode, Buffer, &Data.Association, sizeof(Data.Association));
            RtlFreeUnicodeString(&Link);
            break;
        }

        case PlugPlayControlUserResponse:
        {
            /* umpnpmgr is done with the event at the head */
            PQZ_PNP_EVENT Event = NULL;
            ExAcquireFastMutex(&QzPnpListLock);
            if (!IsListEmpty(&QzPnpEvents))
                Event = CONTAINING_RECORD(RemoveHeadList(&QzPnpEvents), QZ_PNP_EVENT, Link);
            if (IsListEmpty(&QzPnpEvents))
                KeClearEvent(&QzPnpEventPending);
            ExReleaseFastMutex(&QzPnpListLock);
            if (Event)
                ExFreePoolWithTag(Event, QZ_PNP_TAG);
            break;
        }

        case PlugPlayControlQueryAndRemoveDevice:
            Status = STATUS_NOT_SUPPORTED;      /* devices are not removed yet */
            break;

        case PlugPlayControlRetrieveDock:
            Status = STATUS_NO_SUCH_DEVICE;     /* no docking stations */
            break;

        default:
            DbgPrint(QZ_KERNEL_NAME ": NtPlugPlayControl: class %lu not supported yet\n", PlugPlayControlClass);
            Status = STATUS_NOT_IMPLEMENTED;
            break;
    }
    QzPnpUnlockTree();
    return Status;
}

NTSTATUS
NTAPI
NtGetPlugPlayEvent(IN ULONG Reserved1, IN ULONG Reserved2, OUT PPLUGPLAY_EVENT_BLOCK Buffer, IN ULONG BufferSize)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PQZ_PNP_EVENT Event;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Reserved1);
    UNREFERENCED_PARAMETER(Reserved2);
    if (!QzPnpEventsWanted)
    {
        KeInitializeEvent(&QzPnpEventPending, NotificationEvent, FALSE);
        QzPnpEventsWanted = TRUE;
    }
    Status = KeWaitForSingleObject(&QzPnpEventPending, UserRequest, Mode, TRUE, NULL);
    if (Status != STATUS_WAIT_0)
        return Status;                  /* STATUS_USER_APC, STATUS_ALERTED */

    /* The head event stays queued until PlugPlayControlUserResponse */
    ExAcquireFastMutex(&QzPnpListLock);
    if (IsListEmpty(&QzPnpEvents))
    {
        ExReleaseFastMutex(&QzPnpListLock);
        return STATUS_NO_MORE_ENTRIES;
    }
    Event = CONTAINING_RECORD(QzPnpEvents.Flink, QZ_PNP_EVENT, Link);
    if (BufferSize < Event->Size)
        Status = STATUS_BUFFER_TOO_SMALL;
    else if (!QzWriteArg(Mode, Buffer, &Event->Block, Event->Size))
        Status = STATUS_ACCESS_VIOLATION;
    else
        Status = STATUS_SUCCESS;
    ExReleaseFastMutex(&QzPnpListLock);
    return Status;
}
