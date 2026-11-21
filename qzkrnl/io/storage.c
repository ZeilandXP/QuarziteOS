/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     I/O manager services for storage and bus drivers: IRP
 *              forwarding, controller objects, DMA adapters, register
 *              access, MBR partition tables, assorted small services
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qzio.h"
#include <ntdddisk.h>

#undef HalGetDmaAdapter
#undef READ_REGISTER_UCHAR
#undef READ_REGISTER_USHORT
#undef READ_REGISTER_ULONG
#undef WRITE_REGISTER_UCHAR
#undef WRITE_REGISTER_USHORT
#undef WRITE_REGISTER_ULONG

/* ---------------------------------------------------------------------- */
/* Forwarding                                                              */
/* ---------------------------------------------------------------------- */

static NTSTATUS NTAPI
QzIoSignalCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent(Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

BOOLEAN
NTAPI
IoForwardIrpSynchronously(IN PDEVICE_OBJECT DeviceObject,
                          IN PIRP Irp)
{
    KEVENT Event;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, QzIoSignalCompletion, &Event, TRUE, TRUE, TRUE);
    if (IoCallDriver(DeviceObject, Irp) == STATUS_PENDING)
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    return TRUE;
}

NTSTATUS
NTAPI
IoSetCompletionRoutineEx(IN PDEVICE_OBJECT DeviceObject,
                         IN PIRP Irp,
                         IN PIO_COMPLETION_ROUTINE CompletionRoutine,
                         IN PVOID Context,
                         IN BOOLEAN InvokeOnSuccess,
                         IN BOOLEAN InvokeOnError,
                         IN BOOLEAN InvokeOnCancel)
{
    /* Drivers cannot unload yet, so the device needs no extra reference */
    UNREFERENCED_PARAMETER(DeviceObject);
    IoSetCompletionRoutine(Irp, CompletionRoutine, Context, InvokeOnSuccess, InvokeOnError,
                           InvokeOnCancel);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Controller objects: one device at a time owns the controller            */
/* ---------------------------------------------------------------------- */

PCONTROLLER_OBJECT
NTAPI
IoCreateController(IN ULONG Size)
{
    PCONTROLLER_OBJECT Controller = ExAllocatePoolWithTag(NonPagedPool,
                                                          sizeof(CONTROLLER_OBJECT) + Size, QZ_IO_TAG);
    if (!Controller)
        return NULL;
    RtlZeroMemory(Controller, sizeof(CONTROLLER_OBJECT) + Size);
    Controller->Type = IO_TYPE_CONTROLLER;
    Controller->Size = (CSHORT)(sizeof(CONTROLLER_OBJECT) + Size);
    Controller->ControllerExtension = Controller + 1;
    KeInitializeDeviceQueue(&Controller->DeviceWaitQueue);
    return Controller;
}

VOID
NTAPI
IoDeleteController(IN PCONTROLLER_OBJECT ControllerObject)
{
    ExFreePoolWithTag(ControllerObject, QZ_IO_TAG);
}

VOID
NTAPI
IoFreeController(IN PCONTROLLER_OBJECT ControllerObject)
{
    PKDEVICE_QUEUE_ENTRY Entry;

    while ((Entry = KeRemoveDeviceQueue(&ControllerObject->DeviceWaitQueue)))
    {
        PWAIT_CONTEXT_BLOCK Wcb = CONTAINING_RECORD(Entry, WAIT_CONTEXT_BLOCK, WaitQueueEntry);
        if (Wcb->DeviceRoutine(Wcb->DeviceObject, Wcb->CurrentIrp, NULL, Wcb->DeviceContext) !=
            DeallocateObject)
            return;   /* the new owner keeps it */
    }
}

VOID
NTAPI
IoAllocateController(IN PCONTROLLER_OBJECT ControllerObject,
                     IN PDEVICE_OBJECT DeviceObject,
                     IN PDRIVER_CONTROL ExecutionRoutine,
                     IN PVOID Context)
{
    PWAIT_CONTEXT_BLOCK Wcb = &DeviceObject->Queue.Wcb;

    Wcb->DeviceRoutine = ExecutionRoutine;
    Wcb->DeviceContext = Context;
    Wcb->DeviceObject = DeviceObject;
    Wcb->CurrentIrp = DeviceObject->CurrentIrp;
    if (KeInsertDeviceQueue(&ControllerObject->DeviceWaitQueue, &Wcb->WaitQueueEntry))
        return;   /* busy: we will be called from IoFreeController */
    if (ExecutionRoutine(DeviceObject, DeviceObject->CurrentIrp, NULL, Context) == DeallocateObject)
        IoFreeController(ControllerObject);
}

/* ---------------------------------------------------------------------- */
/* DMA adapters (the HAL implements them)                                  */
/* ---------------------------------------------------------------------- */

PDMA_ADAPTER
NTAPI
IoGetDmaAdapter(IN PDEVICE_OBJECT PhysicalDeviceObject OPTIONAL,
                IN PDEVICE_DESCRIPTION DeviceDescription,
                IN OUT PULONG NumberOfMapRegisters)
{
    if (HalDispatchTable.HalGetDmaAdapter)
        return HalDispatchTable.HalGetDmaAdapter(PhysicalDeviceObject, DeviceDescription,
                                                 NumberOfMapRegisters);
    return (PDMA_ADAPTER)HalGetAdapter(DeviceDescription, NumberOfMapRegisters);
}

/* ---------------------------------------------------------------------- */
/* Memory-mapped register access                                           */
/* ---------------------------------------------------------------------- */

UCHAR NTAPI READ_REGISTER_UCHAR(IN PUCHAR Register) { return *(volatile UCHAR *)Register; }
USHORT NTAPI READ_REGISTER_USHORT(IN PUSHORT Register) { return *(volatile USHORT *)Register; }
ULONG NTAPI READ_REGISTER_ULONG(IN PULONG Register) { return *(volatile ULONG *)Register; }

VOID NTAPI WRITE_REGISTER_UCHAR(IN PUCHAR Register, IN UCHAR Value)
{
    *(volatile UCHAR *)Register = Value;
}

VOID NTAPI WRITE_REGISTER_USHORT(IN PUSHORT Register, IN USHORT Value)
{
    *(volatile USHORT *)Register = Value;
}

VOID NTAPI WRITE_REGISTER_ULONG(IN PULONG Register, IN ULONG Value)
{
    *(volatile ULONG *)Register = Value;
}

#define QZ_REGISTER_BUFFER(Type, Name)                                              \
VOID NTAPI READ_REGISTER_BUFFER_##Name(IN Type *Register, OUT Type *Buffer, IN ULONG Count) \
{                                                                                   \
    while (Count--) *Buffer++ = *(volatile Type *)Register++;                       \
}                                                                                   \
VOID NTAPI WRITE_REGISTER_BUFFER_##Name(IN Type *Register, IN Type *Buffer, IN ULONG Count) \
{                                                                                   \
    while (Count--) *(volatile Type *)Register++ = *Buffer++;                       \
}

#undef READ_REGISTER_BUFFER_UCHAR
#undef READ_REGISTER_BUFFER_USHORT
#undef READ_REGISTER_BUFFER_ULONG
#undef WRITE_REGISTER_BUFFER_UCHAR
#undef WRITE_REGISTER_BUFFER_USHORT
#undef WRITE_REGISTER_BUFFER_ULONG
QZ_REGISTER_BUFFER(UCHAR, UCHAR)
QZ_REGISTER_BUFFER(USHORT, USHORT)
QZ_REGISTER_BUFFER(ULONG, ULONG)

/* ---------------------------------------------------------------------- */
/* Disk sectors and MBR partition tables                                   */
/* ---------------------------------------------------------------------- */

#define QZ_MBR_SIGNATURE_OFFSET  0x1FE
#define QZ_MBR_TABLE_OFFSET      0x1BE
#define QZ_MBR_DISK_ID_OFFSET    0x1B8
#define QZ_PARTITION_GPT         0xEE

#include <pshpack1.h>
typedef struct _QZ_MBR_ENTRY
{
    UCHAR BootIndicator;
    UCHAR StartHead, StartSector, StartCylinder;
    UCHAR Type;
    UCHAR EndHead, EndSector, EndCylinder;
    ULONG StartLba;
    ULONG Sectors;
} QZ_MBR_ENTRY, *PQZ_MBR_ENTRY;
#include <poppack.h>

static NTSTATUS
QzIoReadSectors(PDEVICE_OBJECT Device, ULONGLONG Offset, PVOID Buffer, ULONG Length)
{
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER ByteOffset;
    KEVENT Event;
    PIRP Irp;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    ByteOffset.QuadPart = (LONGLONG)Offset;
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ, Device, Buffer, Length, &ByteOffset, &Event,
                                       &IoStatus);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    IoGetNextIrpStackLocation(Irp)->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    return QzIoSynchronousCall(Device, Irp, &Event, &IoStatus);
}

static ULONG
QzIoSectorSize(PDEVICE_OBJECT Device)
{
    DISK_GEOMETRY Geometry;
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    PIRP Irp;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(IOCTL_DISK_GET_DRIVE_GEOMETRY, Device, NULL, 0, &Geometry,
                                        sizeof(Geometry), FALSE, &Event, &IoStatus);
    if (Irp && NT_SUCCESS(QzIoSynchronousCall(Device, Irp, &Event, &IoStatus)) &&
        Geometry.BytesPerSector >= 512 && Geometry.BytesPerSector <= 4096)
        return Geometry.BytesPerSector;
    return 512;
}

VOID
FASTCALL
HalExamineMBR(IN PDEVICE_OBJECT DeviceObject,
              IN ULONG SectorSize,
              IN ULONG MbrTypeIdentifier,
              OUT PVOID *MbrBuffer)
{
    PUCHAR Sector;

    *MbrBuffer = NULL;
    SectorSize = max(SectorSize, 512);
    Sector = ExAllocatePoolWithTag(NonPagedPool, SectorSize, QZ_IO_TAG);
    if (!Sector)
        return;
    if (NT_SUCCESS(QzIoReadSectors(DeviceObject, 0, Sector, SectorSize)) &&
        *(PUSHORT)(Sector + QZ_MBR_SIGNATURE_OFFSET) == 0xAA55 &&
        ((PQZ_MBR_ENTRY)(Sector + QZ_MBR_TABLE_OFFSET))->Type == (UCHAR)MbrTypeIdentifier)
    {
        *MbrBuffer = Sector;   /* the caller frees it */
        return;
    }
    ExFreePoolWithTag(Sector, QZ_IO_TAG);
}

NTSTATUS
NTAPI
IoReadDiskSignature(IN PDEVICE_OBJECT DeviceObject,
                    IN ULONG BytesPerSector,
                    OUT PDISK_SIGNATURE Signature)
{
    PUCHAR Sector;
    NTSTATUS Status;
    ULONG i, Sum = 0;

    BytesPerSector = max(BytesPerSector, 512);
    Sector = ExAllocatePoolWithTag(NonPagedPool, BytesPerSector, QZ_IO_TAG);
    if (!Sector)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = QzIoReadSectors(DeviceObject, 0, Sector, BytesPerSector);
    if (NT_SUCCESS(Status) && *(PUSHORT)(Sector + QZ_MBR_SIGNATURE_OFFSET) != 0xAA55)
        Status = STATUS_DISK_CORRUPT_ERROR;
    if (NT_SUCCESS(Status))
    {
        for (i = 0; i < 512 / sizeof(ULONG); i++)
            Sum += ((PULONG)Sector)[i];
        Signature->PartitionStyle = PARTITION_STYLE_MBR;
        Signature->Mbr.Signature = *(PULONG)(Sector + QZ_MBR_DISK_ID_OFFSET);
        Signature->Mbr.CheckSum = (ULONG)-(LONG)Sum;
    }
    ExFreePoolWithTag(Sector, QZ_IO_TAG);
    return Status;
}

static BOOLEAN
QzIoIsExtended(UCHAR Type)
{
    return Type == PARTITION_EXTENDED || Type == PARTITION_XINT13_EXTENDED || Type == 0x85;
}

/*
 * NT's MBR layout: four entries per partition table sector (the MBR, then
 * each EBR in chain order), unused and container entries included.
 * Recognized data partitions get numbers from 1 upwards.
 */
NTSTATUS
NTAPI
IoReadPartitionTableEx(IN PDEVICE_OBJECT DeviceObject,
                       OUT PDRIVE_LAYOUT_INFORMATION_EX *DriveLayout)
{
    ULONG SectorSize = QzIoSectorSize(DeviceObject), Tables = 0, Number = 1, i;
    ULONGLONG TableLba = 0, ExtendedStart = 0;
    PDRIVE_LAYOUT_INFORMATION_EX Layout;
    PUCHAR Sector;
    NTSTATUS Status;
    const ULONG MaxTables = 32;

    *DriveLayout = NULL;
    Sector = ExAllocatePoolWithTag(NonPagedPool, SectorSize, QZ_IO_TAG);
    Layout = ExAllocatePoolWithTag(NonPagedPool,
                                   FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) +
                                   MaxTables * 4 * sizeof(PARTITION_INFORMATION_EX), QZ_IO_TAG);
    if (!Sector || !Layout)
    {
        if (Sector) ExFreePoolWithTag(Sector, QZ_IO_TAG);
        if (Layout) ExFreePoolWithTag(Layout, QZ_IO_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Layout, FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry));

    Status = QzIoReadSectors(DeviceObject, 0, Sector, SectorSize);
    if (!NT_SUCCESS(Status))
        goto Done;

    if (*(PUSHORT)(Sector + QZ_MBR_SIGNATURE_OFFSET) != 0xAA55)
    {
        Layout->PartitionStyle = PARTITION_STYLE_RAW;
        goto Done;
    }
    Layout->PartitionStyle = PARTITION_STYLE_MBR;
    Layout->Mbr.Signature = *(PULONG)(Sector + QZ_MBR_DISK_ID_OFFSET);

    for (;;)
    {
        PQZ_MBR_ENTRY Entries = (PQZ_MBR_ENTRY)(Sector + QZ_MBR_TABLE_OFFSET);
        ULONGLONG NextTable = 0;

        for (i = 0; i < 4; i++)
        {
            PPARTITION_INFORMATION_EX Out = &Layout->PartitionEntry[Tables * 4 + i];
            PQZ_MBR_ENTRY In = &Entries[i];
            ULONGLONG Base = QzIoIsExtended(In->Type) ? ExtendedStart : TableLba;

            RtlZeroMemory(Out, sizeof(*Out));
            Out->PartitionStyle = PARTITION_STYLE_MBR;
            Out->Mbr.PartitionType = In->Type;
            Out->Mbr.BootIndicator = In->BootIndicator == 0x80;
            Out->Mbr.RecognizedPartition = IsRecognizedPartition(In->Type);
            Out->Mbr.HiddenSectors = (ULONG)(Base + In->StartLba);
            if (In->Type == PARTITION_ENTRY_UNUSED)
                continue;
            if (QzIoIsExtended(In->Type))
            {
                /* EBR links are relative to the start of the extended partition */
                Out->StartingOffset.QuadPart = (LONGLONG)((TableLba ? ExtendedStart : 0) + In->StartLba) *
                                               SectorSize;
                if (!NextTable)
                    NextTable = (TableLba ? ExtendedStart : 0) + In->StartLba;
            }
            else
            {
                Out->StartingOffset.QuadPart = (LONGLONG)(TableLba + In->StartLba) * SectorSize;
                if (In->Type != QZ_PARTITION_GPT)
                    Out->PartitionNumber = Number++;
            }
            Out->PartitionLength.QuadPart = (LONGLONG)In->Sectors * SectorSize;
        }
        Tables++;

        if (!NextTable || Tables >= MaxTables)
            break;
        if (!TableLba)
            ExtendedStart = NextTable;
        TableLba = NextTable;
        Status = QzIoReadSectors(DeviceObject, TableLba * SectorSize, Sector, SectorSize);
        if (!NT_SUCCESS(Status) || *(PUSHORT)(Sector + QZ_MBR_SIGNATURE_OFFSET) != 0xAA55)
        {
            Status = STATUS_SUCCESS;   /* a broken chain ends the layout */
            break;
        }
    }
    Layout->PartitionCount = Tables * 4;

Done:
    ExFreePoolWithTag(Sector, QZ_IO_TAG);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Layout, QZ_IO_TAG);
        return Status;
    }
    *DriveLayout = Layout;
    return STATUS_SUCCESS;
}

/* Writing partition tables needs a writable story for the disk (later) */
NTSTATUS
NTAPI
IoWritePartitionTableEx(IN PDEVICE_OBJECT DeviceObject,
                        IN PDRIVE_LAYOUT_INFORMATION_EX DriveLayout)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(DriveLayout);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
FASTCALL
IoSetPartitionInformation(IN PDEVICE_OBJECT DeviceObject, IN ULONG SectorSize,
                          IN ULONG PartitionNumber, IN ULONG PartitionType)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(SectorSize);
    UNREFERENCED_PARAMETER(PartitionNumber);
    UNREFERENCED_PARAMETER(PartitionType);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
IoSetPartitionInformationEx(IN PDEVICE_OBJECT DeviceObject, IN ULONG PartitionNumber,
                            IN PSET_PARTITION_INFORMATION_EX PartitionInfo)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(PartitionNumber);
    UNREFERENCED_PARAMETER(PartitionInfo);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
IoCreateDisk(IN PDEVICE_OBJECT DeviceObject, IN PCREATE_DISK Disk)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Disk);
    return STATUS_NOT_SUPPORTED;
}

/* ---------------------------------------------------------------------- */
/* Small services                                                          */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
IoGetDevicePropertyData(IN PDEVICE_OBJECT Pdo, IN CONST DEVPROPKEY *PropertyKey, IN LCID Lcid,
                        IN ULONG Flags, IN ULONG Size, OUT PVOID Data, OUT PULONG RequiredSize,
                        OUT PDEVPROPTYPE Type)
{
    UNREFERENCED_PARAMETER(Pdo);
    UNREFERENCED_PARAMETER(PropertyKey);
    UNREFERENCED_PARAMETER(Lcid);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(Data);
    *RequiredSize = 0;
    *Type = DEVPROP_TYPE_EMPTY;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

IO_PAGING_PRIORITY
FASTCALL
IoGetPagingIoPriority(IN PIRP Irp)
{
    return (Irp->Flags & IRP_PAGING_IO) ? IoPagingPriorityNormal : IoPagingPriorityInvalid;
}

NTSTATUS
NTAPI
IoReportTargetDeviceChangeAsynchronous(IN PDEVICE_OBJECT PhysicalDeviceObject,
                                       IN PVOID NotificationStructure,
                                       IN PDEVICE_CHANGE_COMPLETE_CALLBACK Callback OPTIONAL,
                                       IN PVOID Context OPTIONAL)
{
    /* No target-device-change listeners are delivered to yet */
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);
    UNREFERENCED_PARAMETER(NotificationStructure);
    if (Callback)
        Callback(Context);
    return STATUS_SUCCESS;
}

VOID
NTAPI
IoSetStartIoAttributes(IN PDEVICE_OBJECT DeviceObject, IN BOOLEAN DeferredStartIo,
                       IN BOOLEAN NonCancelable)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(DeferredStartIo);
    UNREFERENCED_PARAMETER(NonCancelable);
}

NTSTATUS
NTAPI
IoWMIWriteEvent(IN PVOID WnodeEventItem)
{
    /* On success the event belongs to WMI, which frees it */
    ExFreePool(WnodeEventItem);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
IoSetSystemPartition(IN PUNICODE_STRING VolumeNameString)
{
    UNREFERENCED_PARAMETER(VolumeNameString);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlCreateSystemVolumeInformationFolder(IN PCUNICODE_STRING VolumeRootPath)
{
    UNREFERENCED_PARAMETER(VolumeRootPath);
    return STATUS_NOT_SUPPORTED;   /* needs a mounted file system */
}

/* Directory listings are a file system request */
NTSTATUS
NTAPI
ZwQueryDirectoryFile(IN HANDLE FileHandle, IN HANDLE Event OPTIONAL,
                     IN PIO_APC_ROUTINE ApcRoutine OPTIONAL, IN PVOID ApcContext OPTIONAL,
                     OUT PIO_STATUS_BLOCK IoStatusBlock, OUT PVOID FileInformation,
                     IN ULONG Length, IN FILE_INFORMATION_CLASS FileInformationClass,
                     IN BOOLEAN ReturnSingleEntry, IN PUNICODE_STRING FileName OPTIONAL,
                     IN BOOLEAN RestartScan)
{
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT Device;
    PIO_STACK_LOCATION Stack;
    KEVENT LocalEvent;
    NTSTATUS Status;
    PIRP Irp;

    UNREFERENCED_PARAMETER(ApcContext);
    if (ApcRoutine || Event)
        return STATUS_NOT_IMPLEMENTED;
    Status = ObReferenceObjectByHandle(FileHandle, FILE_LIST_DIRECTORY, IoFileObjectType, KernelMode,
                                       (PVOID *)&FileObject, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    Device = IoGetRelatedDeviceObject(FileObject);
    Irp = IoAllocateIrp(Device->StackSize, FALSE);
    if (!Irp)
    {
        ObDereferenceObject(FileObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KeInitializeEvent(&LocalEvent, NotificationEvent, FALSE);
    Irp->UserEvent = &LocalEvent;
    Irp->UserIosb = IoStatusBlock;
    Irp->Flags = IRP_SYNCHRONOUS_API;
    Irp->Tail.Overlay.OriginalFileObject = FileObject;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    /* The volume device's method, as for reads: file systems mostly take
       the caller's buffer as it is (UserBuffer) */
    if (!QzIoAttachBuffer(Irp, Device, FileInformation, Length, TRUE))
    {
        IoFreeIrp(Irp);
        ObDereferenceObject(FileObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Irp->UserBuffer = FileInformation;
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = IRP_MJ_DIRECTORY_CONTROL;
    Stack->MinorFunction = IRP_MN_QUERY_DIRECTORY;
    Stack->FileObject = FileObject;
    Stack->Flags = (RestartScan ? SL_RESTART_SCAN : 0) | (ReturnSingleEntry ? SL_RETURN_SINGLE_ENTRY : 0);
    Stack->Parameters.QueryDirectory.Length = Length;
    Stack->Parameters.QueryDirectory.FileName = FileName;
    Stack->Parameters.QueryDirectory.FileInformationClass = FileInformationClass;

    Status = IoCallDriver(Device, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&LocalEvent, Executive, KernelMode, FALSE, NULL);
        Status = IoStatusBlock->Status;
    }
    return Status;
}
