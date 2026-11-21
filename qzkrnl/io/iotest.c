/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     I/O manager self-test (shell command "iotest")
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Three small drivers built into the kernel exercise the I/O manager the
 * way real ones do:
 *  - QzTestDisk: a 128 KB RAM disk (direct I/O, IOCTLs, optional pending
 *    completion from a DPC, a cancelable IOCTL, StartIo serialization);
 *  - QzTestFilter: a filter attached on top of it, with completion
 *    routines;
 *  - QzTestFs: a toy file system ("QZFS") that mounts the disk through
 *    its VPB and serves files from a one-sector directory.
 * Everything goes through the exported Io and Zw entry points.
 */

#include "qzio.h"
#include <ntdddisk.h>

#define QZT_DISK_SIZE      (128 * 1024)
#define QZT_TAG            'tsTQ'
#define IOCTL_QZT_HOLD     CTL_CODE(FILE_DEVICE_DISK, 0x800, METHOD_NEITHER, FILE_ANY_ACCESS)
#define IOCTL_QZT_STARTIO  CTL_CODE(FILE_DEVICE_DISK, 0x801, METHOD_NEITHER, FILE_ANY_ACCESS)
#define IOCTL_QZT_PING     CTL_CODE(FILE_DEVICE_DISK, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

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

static VOID
QzSleepMs(LONG Milliseconds)
{
    LARGE_INTEGER Interval;
    Interval.QuadPart = -10000LL * Milliseconds;
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

/* ====================================================================== */
/* QzTestDisk                                                              */
/* ====================================================================== */

typedef struct _QZT_DISK
{
    PUCHAR Data;
    volatile LONG Creates, Cleanups, Closes, Reads, Writes;
    BOOLEAN PendReads;
    PIRP PendingIrp;               /* a read completed later by the DPC */
    KTIMER Timer;
    KDPC Dpc;
    PIRP StartIoIrp;               /* the IRP StartIo is working on */
    KTIMER StartIoTimer;
    KDPC StartIoDpc;
    ULONG StartIoOrder[4];
    ULONG StartIoCount;
    volatile LONG TimerTicks;
} QZT_DISK, *PQZT_DISK;

static PDEVICE_OBJECT QztDiskDevice;

static NTSTATUS
QztComplete(PIRP Irp, NTSTATUS Status, ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS
QztDiskTransfer(PQZT_DISK Disk, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG Length = Stack->Parameters.Read.Length;
    LONGLONG Offset = Stack->Parameters.Read.ByteOffset.QuadPart;
    PUCHAR Buffer;

    if (Offset < 0 || Offset + Length > QZT_DISK_SIZE || (Offset & 511) || (Length & 511))
        return STATUS_INVALID_PARAMETER;
    if (!Length)
        return STATUS_SUCCESS;
    Buffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
    if (!Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (Stack->MajorFunction == IRP_MJ_READ)
        RtlCopyMemory(Buffer, Disk->Data + Offset, Length);
    else
        RtlCopyMemory(Disk->Data + Offset, Buffer, Length);
    Irp->IoStatus.Information = Length;
    return STATUS_SUCCESS;
}

static VOID NTAPI
QztDiskDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    PQZT_DISK Disk = Context;
    PIRP Irp = Disk->PendingIrp;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    Disk->PendingIrp = NULL;
    Irp->IoStatus.Status = QztDiskTransfer(Disk, Irp);
    IoCompleteRequest(Irp, IO_DISK_INCREMENT);   /* at DISPATCH_LEVEL */
}

static NTSTATUS NTAPI
QztDiskCreateClose(PDEVICE_OBJECT Device, PIRP Irp)
{
    PQZT_DISK Disk = Device->DeviceExtension;
    switch (IoGetCurrentIrpStackLocation(Irp)->MajorFunction)
    {
        case IRP_MJ_CREATE:  InterlockedIncrement(&Disk->Creates); break;
        case IRP_MJ_CLEANUP: InterlockedIncrement(&Disk->Cleanups); break;
        default:             InterlockedIncrement(&Disk->Closes); break;
    }
    return QztComplete(Irp, STATUS_SUCCESS, FILE_OPENED);
}

static NTSTATUS NTAPI
QztDiskReadWrite(PDEVICE_OBJECT Device, PIRP Irp)
{
    PQZT_DISK Disk = Device->DeviceExtension;
    NTSTATUS Status;

    if (IoGetCurrentIrpStackLocation(Irp)->MajorFunction == IRP_MJ_READ)
        InterlockedIncrement(&Disk->Reads);
    else
        InterlockedIncrement(&Disk->Writes);

    if (Disk->PendReads && IoGetCurrentIrpStackLocation(Irp)->MajorFunction == IRP_MJ_READ &&
        !Disk->PendingIrp)
    {
        LARGE_INTEGER Due;
        IoMarkIrpPending(Irp);
        Disk->PendingIrp = Irp;
        Due.QuadPart = -200000;   /* 20 ms */
        KeSetTimer(&Disk->Timer, Due, &Disk->Dpc);
        return STATUS_PENDING;
    }

    Irp->IoStatus.Information = 0;
    Status = QztDiskTransfer(Disk, Irp);
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_DISK_INCREMENT);
    return Status;
}

static VOID NTAPI
QztDiskCancelHold(PDEVICE_OBJECT Device, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Device);
    IoReleaseCancelSpinLock(Irp->CancelIrql);
    QztComplete(Irp, STATUS_CANCELLED, 0);
}

/* StartIo finishes each packet from a DPC a little later, so several
   packets queue up behind each other */
static VOID NTAPI
QztDiskStartIo(PDEVICE_OBJECT Device, PIRP Irp)
{
    PQZT_DISK Disk = Device->DeviceExtension;
    LARGE_INTEGER Due;

    Disk->StartIoIrp = Irp;
    Due.QuadPart = -100000;   /* 10 ms */
    KeSetTimer(&Disk->StartIoTimer, Due, &Disk->StartIoDpc);
}

static VOID NTAPI
QztDiskStartIoDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    PDEVICE_OBJECT Device = Context;
    PQZT_DISK Disk = Device->DeviceExtension;
    PIRP Irp = Disk->StartIoIrp;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    if (Disk->StartIoCount < RTL_NUMBER_OF(Disk->StartIoOrder))
        Disk->StartIoOrder[Disk->StartIoCount++] =
            (ULONG)(ULONG_PTR)IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl.Type3InputBuffer;
    Disk->StartIoIrp = NULL;
    IoStartNextPacket(Device, FALSE);
    QztComplete(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS NTAPI
QztDiskControl(PDEVICE_OBJECT Device, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);

    switch (Stack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_DISK_GET_DRIVE_GEOMETRY:
        {
            PDISK_GEOMETRY Geometry = Irp->AssociatedIrp.SystemBuffer;
            if (Stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(DISK_GEOMETRY))
                return QztComplete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
            Geometry->Cylinders.QuadPart = 1;
            Geometry->MediaType = FixedMedia;
            Geometry->TracksPerCylinder = 1;
            Geometry->SectorsPerTrack = QZT_DISK_SIZE / 512;
            Geometry->BytesPerSector = 512;
            return QztComplete(Irp, STATUS_SUCCESS, sizeof(DISK_GEOMETRY));
        }

        case IOCTL_QZT_PING:
        {
            PULONG Value = Irp->AssociatedIrp.SystemBuffer;
            *Value = *Value * 2 + 1;
            return QztComplete(Irp, STATUS_SUCCESS, sizeof(ULONG));
        }

        case IOCTL_QZT_HOLD:
        {
            KIRQL OldIrql;
            IoAcquireCancelSpinLock(&OldIrql);
            IoMarkIrpPending(Irp);
            IoSetCancelRoutine(Irp, QztDiskCancelHold);
            IoReleaseCancelSpinLock(OldIrql);
            return STATUS_PENDING;
        }

        case IOCTL_QZT_STARTIO:
            IoMarkIrpPending(Irp);
            IoStartPacket(Device, Irp, NULL, NULL);
            return STATUS_PENDING;

        default:
            return QztComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static VOID NTAPI
QztDiskTimer(PDEVICE_OBJECT Device, PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    InterlockedIncrement(&((PQZT_DISK)Device->DeviceExtension)->TimerTicks);
}

static NTSTATUS NTAPI
QztDiskEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Device\\QzTestDisk0");
    PDEVICE_OBJECT Device;
    PQZT_DISK Disk;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(RegistryPath);

    Driver->MajorFunction[IRP_MJ_CREATE] = QztDiskCreateClose;
    Driver->MajorFunction[IRP_MJ_CLEANUP] = QztDiskCreateClose;
    Driver->MajorFunction[IRP_MJ_CLOSE] = QztDiskCreateClose;
    Driver->MajorFunction[IRP_MJ_READ] = QztDiskReadWrite;
    Driver->MajorFunction[IRP_MJ_WRITE] = QztDiskReadWrite;
    Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = QztDiskControl;
    Driver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = QztDiskControl;
    Driver->DriverStartIo = QztDiskStartIo;

    Status = IoCreateDevice(Driver, sizeof(QZT_DISK), &Name, FILE_DEVICE_DISK, 0, FALSE, &Device);
    if (!NT_SUCCESS(Status))
        return Status;
    Disk = Device->DeviceExtension;
    Disk->Data = ExAllocatePoolWithTag(NonPagedPool, QZT_DISK_SIZE, QZT_TAG);
    if (!Disk->Data)
    {
        IoDeleteDevice(Device);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Disk->Data, QZT_DISK_SIZE);
    KeInitializeTimer(&Disk->Timer);
    KeInitializeDpc(&Disk->Dpc, QztDiskDpc, Disk);
    KeInitializeTimer(&Disk->StartIoTimer);
    KeInitializeDpc(&Disk->StartIoDpc, QztDiskStartIoDpc, Device);
    IoInitializeTimer(Device, QztDiskTimer, NULL);
    Device->Flags |= DO_DIRECT_IO;
    QztDiskDevice = Device;
    return STATUS_SUCCESS;
}

/* ====================================================================== */
/* QzTestFilter                                                            */
/* ====================================================================== */

typedef struct _QZT_FILTER
{
    PDEVICE_OBJECT Lower;
    volatile LONG Irps;
    volatile LONG Completions;
    volatile LONG RightDevice;
    volatile LONG SawPending;
} QZT_FILTER, *PQZT_FILTER;

static PDEVICE_OBJECT QztFilterDevice;

static NTSTATUS NTAPI
QztFilterCompletion(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context)
{
    PQZT_FILTER Filter = Context;

    InterlockedIncrement(&Filter->Completions);
    if (Device == QztFilterDevice)
        InterlockedIncrement(&Filter->RightDevice);
    if (Irp->PendingReturned)
    {
        InterlockedIncrement(&Filter->SawPending);
        IoMarkIrpPending(Irp);
    }
    return STATUS_CONTINUE_COMPLETION;
}

static NTSTATUS NTAPI
QztFilterPass(PDEVICE_OBJECT Device, PIRP Irp)
{
    PQZT_FILTER Filter = Device->DeviceExtension;

    InterlockedIncrement(&Filter->Irps);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, QztFilterCompletion, Filter, TRUE, TRUE, TRUE);
    return IoCallDriver(Filter->Lower, Irp);
}

static NTSTATUS NTAPI
QztFilterEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING Target = RTL_CONSTANT_STRING(L"\\Device\\QzTestDisk0");
    PDEVICE_OBJECT Device;
    PQZT_FILTER Filter;
    NTSTATUS Status;
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        Driver->MajorFunction[i] = QztFilterPass;

    Status = IoCreateDevice(Driver, sizeof(QZT_FILTER), NULL, FILE_DEVICE_DISK, 0, FALSE, &Device);
    if (!NT_SUCCESS(Status))
        return Status;
    Filter = Device->DeviceExtension;
    /* Known before attaching: IoAttachDevice's own file object is closed
       through the new stack, i.e. through this filter */
    QztFilterDevice = Device;
    Status = IoAttachDevice(Device, &Target, &Filter->Lower);
    if (!NT_SUCCESS(Status))
    {
        QztFilterDevice = NULL;
        IoDeleteDevice(Device);
        return Status;
    }
    Device->Flags |= Filter->Lower->Flags & (DO_DIRECT_IO | DO_BUFFERED_IO);
    Device->Flags &= ~DO_DEVICE_INITIALIZING;
    QztFilterDevice = Device;
    return STATUS_SUCCESS;
}

/* ====================================================================== */
/* QzTestFs: sector 0 = superblock, sector 1 = directory, then file data   */
/* ====================================================================== */

typedef struct _QZT_FS_SUPER
{
    ULONG Magic;                   /* 'SFZQ' */
    ULONG Files;
} QZT_FS_SUPER;

typedef struct _QZT_FS_ENTRY
{
    CHAR Name[24];
    ULONG Sector;
    ULONG Size;
} QZT_FS_ENTRY, *PQZT_FS_ENTRY;

#define QZT_FS_MAGIC  'SFZQ'

typedef struct _QZT_VOLUME
{
    PDEVICE_OBJECT Target;         /* top of the disk stack */
    QZT_FS_ENTRY Directory[512 / sizeof(QZT_FS_ENTRY)];
    ULONG Files;
    volatile LONG Opens, Closes;
} QZT_VOLUME, *PQZT_VOLUME;

static PDEVICE_OBJECT QztFsControlDevice, QztVolumeDevice;
static volatile LONG QztMountRequests;

static NTSTATUS
QztFsReadDisk(PDEVICE_OBJECT Target, ULONG Sector, PVOID Buffer, ULONG Length)
{
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Offset;
    KEVENT Event;
    PIRP Irp;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Offset.QuadPart = (LONGLONG)Sector * 512;
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ, Target, Buffer, Length, &Offset, &Event,
                                       &IoStatus);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    return QzIoSynchronousCall(Target, Irp, &Event, &IoStatus);
}

static NTSTATUS
QztFsMount(PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_OBJECT Target = Stack->Parameters.MountVolume.DeviceObject;
    PVPB Vpb = Stack->Parameters.MountVolume.Vpb;
    PUCHAR Sector;
    PDEVICE_OBJECT Volume;
    PQZT_VOLUME Extension;
    NTSTATUS Status;

    InterlockedIncrement(&QztMountRequests);
    Sector = ExAllocatePoolWithTag(NonPagedPool, 1024, QZT_TAG);
    if (!Sector)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = QztFsReadDisk(Target, 0, Sector, 1024);
    if (NT_SUCCESS(Status) && ((QZT_FS_SUPER *)Sector)->Magic != QZT_FS_MAGIC)
        Status = STATUS_UNRECOGNIZED_VOLUME;
    if (NT_SUCCESS(Status))
        Status = IoCreateDevice(QztFsControlDevice->DriverObject, sizeof(QZT_VOLUME), NULL,
                                FILE_DEVICE_DISK_FILE_SYSTEM, 0, FALSE, &Volume);
    if (NT_SUCCESS(Status))
    {
        static const WCHAR Label[] = L"QZTEST";
        Extension = Volume->DeviceExtension;
        Extension->Target = Target;
        Extension->Files = min(((QZT_FS_SUPER *)Sector)->Files, RTL_NUMBER_OF(Extension->Directory));
        RtlCopyMemory(Extension->Directory, Sector + 512, sizeof(Extension->Directory));
        Volume->StackSize = Target->StackSize + 1;
        Volume->Flags |= DO_BUFFERED_IO;
        Volume->Flags &= ~DO_DEVICE_INITIALIZING;
        Vpb->DeviceObject = Volume;
        Vpb->SerialNumber = 0x51A27E00;
        Vpb->VolumeLabelLength = sizeof(Label) - sizeof(WCHAR);
        RtlCopyMemory(Vpb->VolumeLabel, Label, sizeof(Label) - sizeof(WCHAR));
        QztVolumeDevice = Volume;
    }
    ExFreePoolWithTag(Sector, QZT_TAG);
    return Status;
}

static PQZT_FS_ENTRY
QztFsLookup(PQZT_VOLUME Volume, PCUNICODE_STRING Name)
{
    ULONG i, k, Length = Name->Length / sizeof(WCHAR);
    PCWSTR Chars = Name->Buffer;

    if (Length && Chars[0] == L'\\')
    {
        Chars++;
        Length--;
    }
    for (i = 0; i < Volume->Files; i++)
    {
        PQZT_FS_ENTRY Entry = &Volume->Directory[i];
        for (k = 0; k < Length && Entry->Name[k]; k++)
            if (RtlUpcaseUnicodeChar(Chars[k]) != RtlUpcaseUnicodeChar((UCHAR)Entry->Name[k]))
                break;
        if (k == Length && (k == sizeof(Entry->Name) || !Entry->Name[k]))
            return Entry;
    }
    return NULL;
}

static NTSTATUS NTAPI
QztFsDispatch(PDEVICE_OBJECT Device, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PQZT_VOLUME Volume = Device->DeviceExtension;
    PFILE_OBJECT FileObject = Stack->FileObject;

    /* The control device only mounts */
    if (Device == QztFsControlDevice)
    {
        if (Stack->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL &&
            Stack->MinorFunction == IRP_MN_MOUNT_VOLUME)
            return QztComplete(Irp, QztFsMount(Irp), 0);
        if (Stack->MajorFunction == IRP_MJ_CREATE || Stack->MajorFunction == IRP_MJ_CLOSE ||
            Stack->MajorFunction == IRP_MJ_CLEANUP)
            return QztComplete(Irp, STATUS_SUCCESS, FILE_OPENED);
        return QztComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }

    switch (Stack->MajorFunction)
    {
        case IRP_MJ_CREATE:
        {
            PQZT_FS_ENTRY Entry = NULL;
            BOOLEAN Root = FileObject->FileName.Length == 0 ||
                           (FileObject->FileName.Length == sizeof(WCHAR) &&
                            FileObject->FileName.Buffer[0] == L'\\');
            if (!Root)
            {
                /* Relative opens are only valid below the root */
                if (FileObject->RelatedFileObject && FileObject->RelatedFileObject->FsContext)
                    return QztComplete(Irp, STATUS_OBJECT_PATH_NOT_FOUND, 0);
                Entry = QztFsLookup(Volume, &FileObject->FileName);
                if (!Entry)
                    return QztComplete(Irp, STATUS_OBJECT_NAME_NOT_FOUND, 0);
            }
            FileObject->FsContext = Entry;   /* NULL: the root / the volume */
            InterlockedIncrement(&Volume->Opens);
            return QztComplete(Irp, STATUS_SUCCESS, FILE_OPENED);
        }

        case IRP_MJ_READ:
        {
            PQZT_FS_ENTRY Entry = FileObject->FsContext;
            ULONG Offset = Stack->Parameters.Read.ByteOffset.LowPart;
            ULONG Length = Stack->Parameters.Read.Length, Sectors;
            PUCHAR Bounce;
            NTSTATUS Status;

            if (!Entry)
                return QztComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
            if (Offset >= Entry->Size)
                return QztComplete(Irp, STATUS_END_OF_FILE, 0);
            Length = min(Length, Entry->Size - Offset);
            Sectors = (Offset % 512 + Length + 511) / 512;
            Bounce = ExAllocatePoolWithTag(NonPagedPool, Sectors * 512, QZT_TAG);
            if (!Bounce)
                return QztComplete(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
            /* A nested IRP from the file system to the disk below it */
            Status = QztFsReadDisk(Volume->Target, Entry->Sector + Offset / 512, Bounce, Sectors * 512);
            if (NT_SUCCESS(Status))
                RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Bounce + Offset % 512, Length);
            ExFreePoolWithTag(Bounce, QZT_TAG);
            return QztComplete(Irp, Status, NT_SUCCESS(Status) ? Length : 0);
        }

        case IRP_MJ_QUERY_INFORMATION:
        {
            PQZT_FS_ENTRY Entry = FileObject->FsContext;
            PFILE_STANDARD_INFORMATION Info = Irp->AssociatedIrp.SystemBuffer;
            if (Stack->Parameters.QueryFile.FileInformationClass != FileStandardInformation ||
                Stack->Parameters.QueryFile.Length < sizeof(*Info))
                return QztComplete(Irp, STATUS_INVALID_PARAMETER, 0);
            RtlZeroMemory(Info, sizeof(*Info));
            Info->EndOfFile.QuadPart = Entry ? Entry->Size : 0;
            Info->AllocationSize.QuadPart = Entry ? ALIGN_UP_BY(Entry->Size, 512) : 0;
            Info->NumberOfLinks = 1;
            Info->Directory = Entry == NULL;
            return QztComplete(Irp, STATUS_SUCCESS, sizeof(*Info));
        }

        case IRP_MJ_CLEANUP:
            return QztComplete(Irp, STATUS_SUCCESS, 0);

        case IRP_MJ_CLOSE:
            InterlockedIncrement(&Volume->Closes);
            return QztComplete(Irp, STATUS_SUCCESS, 0);

        default:
            return QztComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static NTSTATUS NTAPI
QztFsEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\FileSystem\\QzTestFs");
    NTSTATUS Status;
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        Driver->MajorFunction[i] = QztFsDispatch;
    Status = IoCreateDevice(Driver, 0, &Name, FILE_DEVICE_DISK_FILE_SYSTEM, 0, FALSE,
                            &QztFsControlDevice);
    if (NT_SUCCESS(Status))
        IoRegisterFileSystem(QztFsControlDevice);
    return Status;
}

/* ====================================================================== */
/* Helpers                                                                 */
/* ====================================================================== */

static NTSTATUS
QztOpen(PHANDLE Handle, PCWSTR Path, ULONG Options, HANDLE Root)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK IoStatus;

    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               Root, NULL);
    return ZwOpenFile(Handle, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE, &Attributes,
                      &IoStatus, FILE_SHARE_READ | FILE_SHARE_WRITE, Options);
}

static PFILE_OBJECT
QztFileOf(HANDLE Handle)
{
    PFILE_OBJECT FileObject = NULL;
    if (NT_SUCCESS(ObReferenceObjectByHandle(Handle, 0, IoFileObjectType, KernelMode,
                                             (PVOID *)&FileObject, NULL)))
        ObDereferenceObject(FileObject);
    return FileObject;
}

static PIRP
QztBuildIoctl(ULONG Code, PVOID In, ULONG InLength, PVOID Out, ULONG OutLength, PKEVENT Event,
              PIO_STATUS_BLOCK IoStatus)
{
    KeInitializeEvent(Event, NotificationEvent, FALSE);
    return IoBuildDeviceIoControlRequest(Code, QzIoGetAttachedDevice(QztDiskDevice), In, InLength,
                                         Out, OutLength, FALSE, Event, IoStatus);
}

static VOID NTAPI
QztWorkRoutine(PDEVICE_OBJECT Device, PVOID Context)
{
    PVOID *Result = Context;
    Result[0] = Device;
    Result[1] = (PVOID)(ULONG_PTR)QzGetCurrentIrql();
    KeSetEvent((PKEVENT)Result[2], 0, FALSE);
}

/* ====================================================================== */
/* The tests                                                               */
/* ====================================================================== */

static VOID
QztTestDriverAndDevice(VOID)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Driver\\QzTestDisk");
    UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(L"\\Device\\QzTestDisk0");
    UNICODE_STRING Link = RTL_CONSTANT_STRING(L"\\??\\QZDISK");
    PVOID Object = NULL;
    NTSTATUS Status;
    BOOLEAN Ok;

    Status = IoCreateDriver(&Name, QztDiskEntry);
    Ok = NT_SUCCESS(Status) && QztDiskDevice &&
         NT_SUCCESS(ObReferenceObjectByName(&Name, OBJ_CASE_INSENSITIVE, NULL, 0, IoDriverObjectType,
                                            KernelMode, NULL, &Object)) &&
         ((PDRIVER_OBJECT)Object)->DeviceObject == QztDiskDevice;
    if (Object)
        ObDereferenceObject(Object);
    QzCheck("IoCreateDriver: \\Driver\\QzTestDisk runs its DriverEntry, owns its device", Ok);

    Object = NULL;
    Ok = NT_SUCCESS(ObReferenceObjectByName(&DeviceName, OBJ_CASE_INSENSITIVE, NULL, 0,
                                            IoDeviceObjectType, KernelMode, NULL, &Object)) &&
         Object == QztDiskDevice && !(QztDiskDevice->Flags & DO_DEVICE_INITIALIZING) &&
         QztDiskDevice->Vpb && QztDiskDevice->SectorSize == 512 && QztDiskDevice->StackSize == 1;
    if (Object)
        ObDereferenceObject(Object);
    QzCheck("IoCreateDevice: named disk device with a VPB, initialized after DriverEntry", Ok);

    Status = IoCreateSymbolicLink(&Link, &DeviceName);
    QzCheck("IoCreateSymbolicLink \\??\\QZDISK -> \\Device\\QzTestDisk0", NT_SUCCESS(Status));
}

static VOID
QztTestSynchronousIo(VOID)
{
    PQZT_DISK Disk = QztDiskDevice->DeviceExtension;
    UCHAR Write[1024], Read[1024];
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Offset;
    DISK_GEOMETRY Geometry;
    PFILE_OBJECT FileObject;
    HANDLE File;
    NTSTATUS Status;
    ULONG i;
    BOOLEAN Ok;

    Status = QztOpen(&File, L"\\??\\QZDISK", FILE_SYNCHRONOUS_IO_NONALERT, NULL);
    FileObject = NT_SUCCESS(Status) ? QztFileOf(File) : NULL;
    Ok = NT_SUCCESS(Status) && FileObject && FileObject->DeviceObject == QztDiskDevice &&
         (FileObject->Flags & FO_DIRECT_DEVICE_OPEN) && Disk->Creates == 1 &&
         QztDiskDevice->ReferenceCount == 1;
    QzCheck("ZwOpenFile through a link: IRP_MJ_CREATE, unmounted volume opened directly", Ok);
    if (!NT_SUCCESS(Status))
        return;

    for (i = 0; i < sizeof(Write); i++)
        Write[i] = (UCHAR)(i * 7 + 3);
    Offset.QuadPart = 4096;
    Status = ZwWriteFile(File, NULL, NULL, NULL, &IoStatus, Write, sizeof(Write), &Offset, NULL);
    Ok = NT_SUCCESS(Status) && IoStatus.Information == sizeof(Write) &&
         FileObject->CurrentByteOffset.QuadPart == 4096 + sizeof(Write);
    RtlZeroMemory(Read, sizeof(Read));
    Offset.QuadPart = 4096;
    Status = ZwReadFile(File, NULL, NULL, NULL, &IoStatus, Read, sizeof(Read), &Offset, NULL);
    Ok = Ok && NT_SUCCESS(Status) && IoStatus.Information == sizeof(Read) &&
         RtlCompareMemory(Read, Write, sizeof(Read)) == sizeof(Read);
    QzCheck("ZwWriteFile/ZwReadFile (direct I/O, MDLs): data round-trips, file position advances", Ok);

    Status = ZwReadFile(File, NULL, NULL, NULL, &IoStatus, Read, 100, &Offset, NULL);
    QzCheck("a driver error comes back as the status (misaligned read)",
            Status == STATUS_INVALID_PARAMETER && IoStatus.Status == STATUS_INVALID_PARAMETER);

    RtlZeroMemory(&Geometry, sizeof(Geometry));
    Status = ZwDeviceIoControlFile(File, NULL, NULL, NULL, &IoStatus, IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                   NULL, 0, &Geometry, sizeof(Geometry));
    QzCheck("ZwDeviceIoControlFile (METHOD_BUFFERED): IOCTL_DISK_GET_DRIVE_GEOMETRY",
            NT_SUCCESS(Status) && IoStatus.Information == sizeof(Geometry) &&
            Geometry.BytesPerSector == 512 && Geometry.SectorsPerTrack == QZT_DISK_SIZE / 512);

    ZwClose(File);
    QzCheck("ZwClose: IRP_MJ_CLEANUP then IRP_MJ_CLOSE, device reference dropped",
            Disk->Cleanups == 1 && Disk->Closes == 1 && QztDiskDevice->ReferenceCount == 0);
}

static VOID
QztTestAsynchronousIo(VOID)
{
    PQZT_DISK Disk = QztDiskDevice->DeviceExtension;
    UCHAR Read[512];
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Offset;
    HANDLE File, Event;
    NTSTATUS Status, WaitStatus;
    ULONG Value = 20;
    PIRP Irp;
    KEVENT KEvent;
    BOOLEAN Ok;

    /* Asynchronous handle: the read pends and completes from a DPC */
    Status = QztOpen(&File, L"\\Device\\QzTestDisk0", 0, NULL);
    if (!NT_SUCCESS(Status))
    {
        QzCheck("asynchronous open", FALSE);
        return;
    }
    ZwCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    Disk->PendReads = TRUE;
    RtlZeroMemory(Read, sizeof(Read));
    IoStatus.Status = STATUS_PENDING;
    Offset.QuadPart = 4096;
    Status = ZwReadFile(File, Event, NULL, NULL, &IoStatus, Read, sizeof(Read), &Offset, NULL);
    Ok = Status == STATUS_PENDING;
    WaitStatus = ZwWaitForSingleObject(Event, FALSE, NULL);
    Ok = Ok && WaitStatus == STATUS_WAIT_0 && IoStatus.Status == STATUS_SUCCESS &&
         IoStatus.Information == sizeof(Read) && Read[1] == 10;
    QzCheck("async ZwReadFile: STATUS_PENDING, completed at DISPATCH_LEVEL, event + IOSB set", Ok);

    /* The same, waiting on the file handle itself */
    IoStatus.Status = STATUS_PENDING;
    Status = ZwReadFile(File, NULL, NULL, NULL, &IoStatus, Read, sizeof(Read), &Offset, NULL);
    Ok = Status == STATUS_PENDING && ZwWaitForSingleObject(File, FALSE, NULL) == STATUS_WAIT_0 &&
         IoStatus.Status == STATUS_SUCCESS;
    Disk->PendReads = FALSE;
    QzCheck("waiting on a file handle waits for its I/O (FILE_OBJECT.Event)", Ok);
    ZwClose(Event);
    ZwClose(File);

    /* A kernel-built IOCTL IRP */
    Irp = QztBuildIoctl(IOCTL_QZT_PING, &Value, sizeof(Value), &Value, sizeof(Value), &KEvent,
                        &IoStatus);
    Status = Irp ? QzIoSynchronousCall(QzIoGetAttachedDevice(QztDiskDevice), Irp, &KEvent, &IoStatus)
                 : STATUS_INSUFFICIENT_RESOURCES;
    QzCheck("IoBuildDeviceIoControlRequest + IoCallDriver: buffered output copied back",
            NT_SUCCESS(Status) && Value == 41 && IoStatus.Information == sizeof(ULONG));

    /* Cancellation of a held IRP */
    Irp = QztBuildIoctl(IOCTL_QZT_HOLD, NULL, 0, NULL, 0, &KEvent, &IoStatus);
    Status = IoCallDriver(QzIoGetAttachedDevice(QztDiskDevice), Irp);
    Ok = Status == STATUS_PENDING;
    QzSleepMs(10);
    Ok = Ok && KeReadStateEvent(&KEvent) == 0 && IoCancelIrp(Irp);
    Ok = Ok && KeReadStateEvent(&KEvent) == 1 && IoStatus.Status == STATUS_CANCELLED;
    QzCheck("IoCancelIrp runs the driver's cancel routine; the IRP completes STATUS_CANCELLED", Ok);
}

static VOID
QztTestStartIoAndWork(VOID)
{
    PQZT_DISK Disk = QztDiskDevice->DeviceExtension;
    IO_STATUS_BLOCK IoStatus[3];
    KEVENT Events[3], Done;
    PVOID Result[3];
    PIO_WORKITEM WorkItem;
    PIRP Irp;
    ULONG i;
    BOOLEAN Ok = TRUE;

    Disk->StartIoCount = 0;
    for (i = 0; i < 3; i++)
    {
        Irp = QztBuildIoctl(IOCTL_QZT_STARTIO, (PVOID)(ULONG_PTR)(i + 1), 0, NULL, 0, &Events[i],
                            &IoStatus[i]);
        Ok = Ok && Irp && IoCallDriver(QzIoGetAttachedDevice(QztDiskDevice), Irp) == STATUS_PENDING;
    }
    for (i = 0; i < 3 && Ok; i++)
        Ok = KeWaitForSingleObject(&Events[i], Executive, KernelMode, FALSE, NULL) == STATUS_WAIT_0 &&
             NT_SUCCESS(IoStatus[i].Status);
    Ok = Ok && Disk->StartIoCount == 3 && Disk->StartIoOrder[0] == 1 && Disk->StartIoOrder[1] == 2 &&
         Disk->StartIoOrder[2] == 3;
    QzCheck("IoStartPacket/IoStartNextPacket: 3 requests through StartIo, one at a time, in order", Ok);

    KeInitializeEvent(&Done, NotificationEvent, FALSE);
    Result[0] = NULL;
    Result[1] = (PVOID)(ULONG_PTR)0xFF;
    Result[2] = &Done;
    WorkItem = IoAllocateWorkItem(QztDiskDevice);
    if (WorkItem)
    {
        IoQueueWorkItem(WorkItem, QztWorkRoutine, DelayedWorkQueue, Result);
        KeWaitForSingleObject(&Done, Executive, KernelMode, FALSE, NULL);
        IoFreeWorkItem(WorkItem);
    }
    QzCheck("IoQueueWorkItem: routine runs on a worker thread at PASSIVE_LEVEL with its device",
            WorkItem && Result[0] == QztDiskDevice && Result[1] == (PVOID)PASSIVE_LEVEL);

    IoStartTimer(QztDiskDevice);
    QzSleepMs(1300);
    IoStopTimer(QztDiskDevice);
    QzCheck("IoInitializeTimer/IoStartTimer: the device timer fires once a second",
            Disk->TimerTicks >= 1 && Disk->TimerTicks <= 2);
}

static VOID
QztTestFilter(VOID)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Driver\\QzTestFilter");
    PQZT_DISK Disk = QztDiskDevice->DeviceExtension;
    PQZT_FILTER Filter;
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Offset;
    UCHAR Read[512];
    HANDLE File, Event;
    NTSTATUS Status;
    LONG Reads;
    BOOLEAN Ok;

    Status = IoCreateDriver(&Name, QztFilterEntry);
    Ok = NT_SUCCESS(Status) && QztFilterDevice && QztFilterDevice->StackSize == 2 &&
         QztDiskDevice->AttachedDevice == QztFilterDevice &&
         IoGetAttachedDevice(QztDiskDevice) == QztFilterDevice;
    QzCheck("IoAttachDevice: a filter on top of the disk (stack size 2)", Ok);
    if (!Ok)
        return;
    Filter = QztFilterDevice->DeviceExtension;

    Reads = Disk->Reads;
    Status = QztOpen(&File, L"\\??\\QZDISK", 0, NULL);
    ZwCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    Disk->PendReads = TRUE;
    Offset.QuadPart = 4096;
    Status = NT_SUCCESS(Status) ? ZwReadFile(File, Event, NULL, NULL, &IoStatus, Read, sizeof(Read),
                                             &Offset, NULL)
                                : Status;
    if (Status == STATUS_PENDING)
        ZwWaitForSingleObject(Event, FALSE, NULL);
    Disk->PendReads = FALSE;
    Ok = NT_SUCCESS(IoStatus.Status) && Disk->Reads == Reads + 1 && Filter->Irps >= 2 &&
         Filter->Completions >= 2 && Filter->RightDevice == Filter->Completions &&
         Filter->SawPending >= 1 && Read[1] == 10;
    QzCheck("I/O through the filter: completion routines see their own device and PendingReturned", Ok);
    if (!Ok)
        QzPrint("    status %08lx/%08lx reads %ld->%ld irps %ld completions %ld right %ld pending %ld data %u\n",
                Status, IoStatus.Status, Reads, Disk->Reads, Filter->Irps, Filter->Completions,
                Filter->RightDevice, Filter->SawPending, Read[1]);
    ZwClose(Event);
    ZwClose(File);
}

static VOID
QztTestFileSystem(VOID)
{
    static const CHAR Text[] = "Hello from the QuarziteOS I/O manager!";
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Driver\\QzTestFs");
    UCHAR Sectors[1536];
    QZT_FS_SUPER *Super = (QZT_FS_SUPER *)Sectors;
    PQZT_FS_ENTRY Entries = (PQZT_FS_ENTRY)(Sectors + 512);
    FILE_STANDARD_INFORMATION Standard;
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Offset;
    CHAR Read[64];
    HANDLE Raw, File, Root, Relative;
    PFILE_OBJECT FileObject;
    NTSTATUS Status;
    PQZT_VOLUME Volume;
    BOOLEAN Ok;

    /* Format the disk while no file system claims it */
    RtlZeroMemory(Sectors, sizeof(Sectors));
    Super->Magic = QZT_FS_MAGIC;
    Super->Files = 2;
    RtlCopyMemory(Entries[0].Name, "hello.txt", 9);
    Entries[0].Sector = 2;
    Entries[0].Size = sizeof(Text) - 1;
    RtlCopyMemory(Entries[1].Name, "empty.dat", 9);
    Entries[1].Sector = 3;
    Entries[1].Size = 0;
    RtlCopyMemory(Sectors + 1024, Text, sizeof(Text) - 1);
    Status = QztOpen(&Raw, L"\\??\\QZDISK", FILE_SYNCHRONOUS_IO_NONALERT, NULL);
    if (NT_SUCCESS(Status))
    {
        Offset.QuadPart = 0;
        Status = ZwWriteFile(Raw, NULL, NULL, NULL, &IoStatus, Sectors, sizeof(Sectors), &Offset,
                             NULL);
        ZwClose(Raw);
    }

    Status = NT_SUCCESS(Status) ? IoCreateDriver(&Name, QztFsEntry) : Status;
    QzCheck("IoRegisterFileSystem: \\FileSystem\\QzTestFs registered as a disk file system",
            NT_SUCCESS(Status) && QzIoFileSystemCount() >= 1);
    if (!NT_SUCCESS(Status))
        return;

    Status = QztOpen(&File, L"\\??\\QZDISK\\hello.txt", FILE_SYNCHRONOUS_IO_NONALERT, NULL);
    FileObject = NT_SUCCESS(Status) ? QztFileOf(File) : NULL;
    Ok = NT_SUCCESS(Status) && QztVolumeDevice && (QztDiskDevice->Vpb->Flags & VPB_MOUNTED) &&
         QztDiskDevice->Vpb->DeviceObject == QztVolumeDevice && FileObject &&
         FileObject->Vpb == QztDiskDevice->Vpb && QztMountRequests == 1 &&
         IoGetRelatedDeviceObject(FileObject) == QztVolumeDevice;
    QzCheck("opening \\??\\QZDISK\\hello.txt mounts the volume (IRP_MN_MOUNT_VOLUME via the VPB)", Ok);
    if (!NT_SUCCESS(Status))
        return;

    RtlZeroMemory(Read, sizeof(Read));
    Status = ZwReadFile(File, NULL, NULL, NULL, &IoStatus, Read, sizeof(Read), NULL, NULL);
    Ok = NT_SUCCESS(Status) && IoStatus.Information == sizeof(Text) - 1 &&
         RtlCompareMemory(Read, Text, sizeof(Text) - 1) == sizeof(Text) - 1;
    Status = ZwReadFile(File, NULL, NULL, NULL, &IoStatus, Read, sizeof(Read), NULL, NULL);
    Ok = Ok && Status == STATUS_END_OF_FILE;
    QzCheck("ZwReadFile on the file: data via the FS and a nested IRP to the disk; then EOF", Ok);

    Status = ZwQueryInformationFile(File, &IoStatus, &Standard, sizeof(Standard),
                                    FileStandardInformation);
    QzCheck("ZwQueryInformationFile (FileStandardInformation): size from the directory",
            NT_SUCCESS(Status) && Standard.EndOfFile.QuadPart == sizeof(Text) - 1 &&
            !Standard.Directory);

    Status = QztOpen(&Relative, L"\\??\\QZDISK\\missing.txt", FILE_SYNCHRONOUS_IO_NONALERT, NULL);
    QzCheck("a name the file system does not know: STATUS_OBJECT_NAME_NOT_FOUND",
            Status == STATUS_OBJECT_NAME_NOT_FOUND);

    Status = QztOpen(&Root, L"\\??\\QZDISK\\", FILE_SYNCHRONOUS_IO_NONALERT, NULL);
    Ok = NT_SUCCESS(Status);
    if (Ok)
    {
        Status = QztOpen(&Relative, L"empty.dat", FILE_SYNCHRONOUS_IO_NONALERT, Root);
        Ok = NT_SUCCESS(Status) && QztFileOf(Relative)->RelatedFileObject == QztFileOf(Root);
        if (NT_SUCCESS(Status))
            ZwClose(Relative);
        ZwClose(Root);
    }
    QzCheck("relative open (RootDirectory = an open directory handle)", Ok);

    ZwClose(File);
    Volume = QztVolumeDevice->DeviceExtension;
    QzCheck("every open on the volume is closed again (IRP_MJ_CLOSE per create)",
            Volume->Opens == 3 && Volume->Closes == 3);
}

VOID
QzRunIoTests(VOID)
{
    static BOOLEAN Ran;
    ULONG Irps = QzIoIrpsOutstanding(), Files;

    if (Ran)
    {
        QzPrint("  iotest: already ran (its test drivers stay loaded); reboot to run again\n");
        return;
    }
    Ran = TRUE;
    QzPassed = QzFailed = 0;
    Files = IoFileObjectType->TotalNumberOfObjects;

    QztTestDriverAndDevice();
    if (QztDiskDevice)
    {
        QztTestSynchronousIo();
        QztTestAsynchronousIo();
        QztTestStartIoAndWork();
        QztTestFilter();
        QztTestFileSystem();
    }
    QzSleepMs(20);   /* let deferred deletions finish */
    QzCheck("no IRPs or file objects leaked",
            QzIoIrpsOutstanding() == Irps && IoFileObjectType->TotalNumberOfObjects == Files);
    QzPrint("  iotest: %lu passed, %lu failed\n", QzPassed, QzFailed);
}
