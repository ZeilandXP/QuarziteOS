/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     I/O manager: file objects, opening devices and files (the
 *              device parse procedure), volume mounting, file systems,
 *              and the Zw*File services
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Opening "\Device\Foo\dir\file" walks the object namespace to the device,
 * whose parse procedure (QzIoParseDevice) takes the rest of the name. A
 * storage device (one with a VPB) is mounted first: each registered file
 * system of the right type gets IRP_MN_MOUNT_VOLUME until one claims the
 * volume. The create IRP then goes to the top of the volume's stack, with
 * the remaining name in the new FILE_OBJECT.
 *
 * Until a RAW file system exists, opening an unrecognized volume by its
 * bare device name opens the device directly, as needed for sector I/O.
 */

#include "qzio.h"

static LIST_ENTRY QzIoDiskFileSystems = { &QzIoDiskFileSystems, &QzIoDiskFileSystems };
static LIST_ENTRY QzIoCdFileSystems = { &QzIoCdFileSystems, &QzIoCdFileSystems };
static LIST_ENTRY QzIoTapeFileSystems = { &QzIoTapeFileSystems, &QzIoTapeFileSystems };
static LIST_ENTRY QzIoNetFileSystems = { &QzIoNetFileSystems, &QzIoNetFileSystems };
static ULONG QzIoFileSystems;
static KMUTEX QzIoMountLock;
static BOOLEAN QzIoMountLockReady;

typedef struct _QZ_FS_NOTIFY
{
    LIST_ENTRY Link;
    PDRIVER_OBJECT DriverObject;
    PDRIVER_FS_NOTIFICATION Routine;
} QZ_FS_NOTIFY, *PQZ_FS_NOTIFY;

static LIST_ENTRY QzIoFsNotifyList = { &QzIoFsNotifyList, &QzIoFsNotifyList };

ULONG
QzIoFileSystemCount(VOID)
{
    return QzIoFileSystems;
}

static VOID
QzIoLockMount(VOID)
{
    if (!QzIoMountLockReady)
    {
        KeInitializeMutex(&QzIoMountLock, 0);
        QzIoMountLockReady = TRUE;
    }
    KeWaitForSingleObject(&QzIoMountLock, Executive, KernelMode, FALSE, NULL);
}

static VOID
QzIoUnlockMount(VOID)
{
    KeReleaseMutex(&QzIoMountLock, FALSE);
}

/* ---------------------------------------------------------------------- */
/* File systems                                                            */
/* ---------------------------------------------------------------------- */

static PLIST_ENTRY
QzIoFileSystemList(DEVICE_TYPE Type)
{
    switch (Type)
    {
        case FILE_DEVICE_DISK_FILE_SYSTEM:
        case FILE_DEVICE_DISK:
        case FILE_DEVICE_VIRTUAL_DISK:
            return &QzIoDiskFileSystems;
        case FILE_DEVICE_CD_ROM_FILE_SYSTEM:
        case FILE_DEVICE_CD_ROM:
            return &QzIoCdFileSystems;
        case FILE_DEVICE_TAPE_FILE_SYSTEM:
        case FILE_DEVICE_TAPE:
            return &QzIoTapeFileSystems;
        case FILE_DEVICE_NETWORK_FILE_SYSTEM:
            return &QzIoNetFileSystems;
        default:
            return NULL;
    }
}

static VOID
QzIoNotifyFileSystem(PDEVICE_OBJECT DeviceObject, BOOLEAN Active)
{
    PLIST_ENTRY Entry;
    for (Entry = QzIoFsNotifyList.Flink; Entry != &QzIoFsNotifyList; Entry = Entry->Flink)
        CONTAINING_RECORD(Entry, QZ_FS_NOTIFY, Link)->Routine(DeviceObject, Active);
}

VOID
NTAPI
IoRegisterFileSystem(IN PDEVICE_OBJECT DeviceObject)
{
    PLIST_ENTRY List = QzIoFileSystemList(DeviceObject->DeviceType);

    if (!List)
        return;
    QzIoLockMount();
    ObReferenceObject(DeviceObject);
    /* Newest first, as on NT, so a later file system can claim a volume */
    InsertHeadList(List, &DeviceObject->Queue.ListEntry);
    QzIoFileSystems++;
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    QzIoNotifyFileSystem(DeviceObject, TRUE);
    QzIoUnlockMount();
}

VOID
NTAPI
IoUnregisterFileSystem(IN PDEVICE_OBJECT DeviceObject)
{
    QzIoLockMount();
    RemoveEntryList(&DeviceObject->Queue.ListEntry);
    QzIoFileSystems--;
    QzIoNotifyFileSystem(DeviceObject, FALSE);
    QzIoUnlockMount();
    ObDereferenceObject(DeviceObject);
}

NTSTATUS
NTAPI
IoRegisterFsRegistrationChange(IN PDRIVER_OBJECT DriverObject,
                               IN PDRIVER_FS_NOTIFICATION DriverNotificationRoutine)
{
    static PLIST_ENTRY Lists[] = { &QzIoDiskFileSystems, &QzIoCdFileSystems,
                                   &QzIoTapeFileSystems, &QzIoNetFileSystems };
    PQZ_FS_NOTIFY Notify = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Notify), QZ_IO_TAG);
    PLIST_ENTRY Entry;
    ULONG i;

    if (!Notify)
        return STATUS_INSUFFICIENT_RESOURCES;
    Notify->DriverObject = DriverObject;
    Notify->Routine = DriverNotificationRoutine;
    ObReferenceObject(DriverObject);

    QzIoLockMount();
    InsertTailList(&QzIoFsNotifyList, &Notify->Link);
    /* Tell the newcomer about the file systems already registered */
    for (i = 0; i < RTL_NUMBER_OF(Lists); i++)
        for (Entry = Lists[i]->Flink; Entry != Lists[i]; Entry = Entry->Flink)
            DriverNotificationRoutine(CONTAINING_RECORD(Entry, DEVICE_OBJECT, Queue.ListEntry), TRUE);
    QzIoUnlockMount();
    return STATUS_SUCCESS;
}

VOID
NTAPI
IoUnregisterFsRegistrationChange(IN PDRIVER_OBJECT DriverObject,
                                 IN PDRIVER_FS_NOTIFICATION FSDNotificationProc)
{
    PLIST_ENTRY Entry;

    QzIoLockMount();
    for (Entry = QzIoFsNotifyList.Flink; Entry != &QzIoFsNotifyList; Entry = Entry->Flink)
    {
        PQZ_FS_NOTIFY Notify = CONTAINING_RECORD(Entry, QZ_FS_NOTIFY, Link);
        if (Notify->DriverObject == DriverObject && Notify->Routine == FSDNotificationProc)
        {
            RemoveEntryList(Entry);
            ObDereferenceObject(DriverObject);
            ExFreePoolWithTag(Notify, QZ_IO_TAG);
            break;
        }
    }
    QzIoUnlockMount();
}

/* IRP_MN_LOAD_FILE_SYSTEM to a recognizer */
static VOID
QzIoLoadFileSystem(PDEVICE_OBJECT Recognizer)
{
    PDEVICE_OBJECT Target = QzIoGetAttachedDevice(Recognizer);
    IO_STATUS_BLOCK IoStatus;
    PIO_STACK_LOCATION Stack;
    KEVENT Event;
    PIRP Irp = IoAllocateIrp(Target->StackSize, FALSE);
    NTSTATUS Status;

    if (!Irp)
        return;
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp->UserEvent = &Event;
    Irp->UserIosb = &IoStatus;
    Irp->Flags = IRP_SYNCHRONOUS_API;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = IRP_MJ_FILE_SYSTEM_CONTROL;
    Stack->MinorFunction = IRP_MN_LOAD_FILE_SYSTEM;
    Status = QzIoSynchronousCall(Target, Irp, &Event, &IoStatus);
    DbgPrint(QZ_KERNEL_NAME ": a file system recognizer loaded its driver: 0x%08lx\n",
             Status == STATUS_PENDING ? IoStatus.Status : Status);
}

/* Offer the volume on Device to each file system until one mounts it */
static NTSTATUS
QzIoMountVolume(PDEVICE_OBJECT Device)
{
    PLIST_ENTRY List = QzIoFileSystemList(Device->DeviceType), Entry;
    NTSTATUS Status = STATUS_UNRECOGNIZED_VOLUME;
    PVPB Vpb = Device->Vpb;

    if (!List)
        return STATUS_UNRECOGNIZED_VOLUME;

    QzIoLockMount();
    if (Vpb->Flags & VPB_MOUNTED)
    {
        QzIoUnlockMount();
        return STATUS_SUCCESS;
    }

    for (Entry = List->Flink; Entry != List; Entry = Entry->Flink)
    {
        PDEVICE_OBJECT FileSystem = CONTAINING_RECORD(Entry, DEVICE_OBJECT, Queue.ListEntry);
        ULONG Loads = 0;
        PDEVICE_OBJECT Target = QzIoGetAttachedDevice(FileSystem);
        IO_STATUS_BLOCK IoStatus;
        PIO_STACK_LOCATION Stack;
        KEVENT Event;
        PIRP Irp;

        Irp = IoAllocateIrp(Target->StackSize, FALSE);
        if (!Irp)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        KeInitializeEvent(&Event, NotificationEvent, FALSE);
        Irp->UserEvent = &Event;
        Irp->UserIosb = &IoStatus;
        Irp->Flags = IRP_MOUNT_COMPLETION | IRP_SYNCHRONOUS_API;
        Irp->Tail.Overlay.Thread = PsGetCurrentThread();
        Stack = IoGetNextIrpStackLocation(Irp);
        Stack->MajorFunction = IRP_MJ_FILE_SYSTEM_CONTROL;
        Stack->MinorFunction = IRP_MN_MOUNT_VOLUME;
        Stack->Parameters.MountVolume.Vpb = Vpb;
        Stack->Parameters.MountVolume.DeviceObject = QzIoGetAttachedDevice(Device);

        IoStatus.Status = STATUS_UNRECOGNIZED_VOLUME;
        Status = QzIoSynchronousCall(Target, Irp, &Event, &IoStatus);
        if (Status != STATUS_PENDING)
            Status = IoStatus.Status;

        if (NT_SUCCESS(Status) && Vpb->DeviceObject)
        {
            Vpb->Flags |= VPB_MOUNTED;
            Vpb->DeviceObject->Vpb = Vpb;
            break;
        }
        if (Status == STATUS_FS_DRIVER_REQUIRED && Loads++ < 4)
        {
            /* A recognizer (fs_rec) knows the format: it loads the real
               file system (which registers, newest first) and leaves the
               list; the volume is offered to everyone again */
            QzIoLoadFileSystem(FileSystem);
            Entry = List;
            continue;
        }
        if (Status != STATUS_UNRECOGNIZED_VOLUME && Status != STATUS_FS_DRIVER_REQUIRED &&
            Status != STATUS_WRONG_VOLUME)
        {
            break;   /* a real error (no media, device not ready, ...) */
        }
        Status = STATUS_UNRECOGNIZED_VOLUME;
    }
    QzIoUnlockMount();
    return Status;
}

NTSTATUS
NTAPI
IoVerifyVolume(IN PDEVICE_OBJECT DeviceObject,
               IN BOOLEAN AllowRawMount)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(AllowRawMount);
    return STATUS_SUCCESS;   /* no removable-media verification yet */
}

VOID
NTAPI
IoSetHardErrorOrVerifyDevice(IN PIRP Irp,
                             IN PDEVICE_OBJECT DeviceObject)
{
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(DeviceObject);
}

/* ---------------------------------------------------------------------- */
/* File objects                                                            */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzIoCreateFileObject(PDEVICE_OBJECT Device, PVPB Vpb, ULONG Flags, PFILE_OBJECT *Result)
{
    PFILE_OBJECT FileObject;
    NTSTATUS Status;

    Status = ObCreateObject(KernelMode, IoFileObjectType, NULL, KernelMode, NULL,
                            sizeof(FILE_OBJECT), 0, 0, (PVOID *)&FileObject);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlZeroMemory(FileObject, sizeof(FILE_OBJECT));
    FileObject->Type = IO_TYPE_FILE;
    FileObject->Size = sizeof(FILE_OBJECT);
    FileObject->DeviceObject = Device;
    FileObject->Vpb = Vpb;
    FileObject->Flags = Flags;
    KeInitializeEvent(&FileObject->Event, NotificationEvent, FALSE);
    KeInitializeEvent(&FileObject->Lock, SynchronizationEvent, FALSE);

    /* The file object keeps its device (and volume) alive */
    ObReferenceObject(Device);
    InterlockedIncrement(&Device->ReferenceCount);
    if (Vpb)
        InterlockedIncrement((PLONG)&Vpb->ReferenceCount);
    *Result = FileObject;
    return STATUS_SUCCESS;
}

/* Drop what QzIoCreateFileObject took; the object is then inert */
static VOID
QzIoReleaseFileObjectDevice(PFILE_OBJECT FileObject)
{
    PDEVICE_OBJECT Device = FileObject->DeviceObject;

    if (!Device)
        return;
    if (FileObject->Vpb)
        InterlockedDecrement((PLONG)&FileObject->Vpb->ReferenceCount);
    InterlockedDecrement(&Device->ReferenceCount);
    FileObject->DeviceObject = NULL;
    ObDereferenceObject(Device);
}

/* Send a cleanup or close IRP for FileObject and wait for it */
static VOID
QzIoSendFileIrp(PFILE_OBJECT FileObject, UCHAR MajorFunction)
{
    PDEVICE_OBJECT Device = IoGetRelatedDeviceObject(FileObject);
    IO_STATUS_BLOCK IoStatus;
    PIO_STACK_LOCATION Stack;
    KEVENT Event;
    PIRP Irp;

    Irp = IoAllocateIrp(Device->StackSize, FALSE);
    if (!Irp)
        return;
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp->UserEvent = &Event;
    Irp->UserIosb = &IoStatus;
    Irp->Tail.Overlay.OriginalFileObject = FileObject;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->Flags = IRP_SYNCHRONOUS_API | IRP_CLOSE_OPERATION;
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = MajorFunction;
    Stack->FileObject = FileObject;
    QzIoSynchronousCall(Device, Irp, &Event, &IoStatus);
}

VOID NTAPI
QzIoCloseFile(PEPROCESS Process, PVOID Object, ACCESS_MASK GrantedAccess,
              ULONG ProcessHandleCount, ULONG SystemHandleCount)
{
    PFILE_OBJECT FileObject = Object;

    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(GrantedAccess);
    UNREFERENCED_PARAMETER(ProcessHandleCount);

    /* The last handle: the driver cleans up (the object may live on) */
    if (SystemHandleCount == 1 && FileObject->DeviceObject &&
        !(FileObject->Flags & FO_CLEANUP_COMPLETE))
    {
        QzIoSendFileIrp(FileObject, IRP_MJ_CLEANUP);
        FileObject->Flags |= FO_CLEANUP_COMPLETE;
    }
}

VOID NTAPI
QzIoDeleteFile(PVOID Object)
{
    PFILE_OBJECT FileObject = Object;

    if (FileObject->DeviceObject)
    {
        /* Stream file objects never had a handle, hence no cleanup yet */
        if (!(FileObject->Flags & FO_CLEANUP_COMPLETE) && (FileObject->Flags & FO_STREAM_FILE))
            QzIoSendFileIrp(FileObject, IRP_MJ_CLEANUP);
        QzIoSendFileIrp(FileObject, IRP_MJ_CLOSE);
        QzIoReleaseFileObjectDevice(FileObject);
    }
    if (FileObject->FileName.Buffer)
        ExFreePoolWithTag(FileObject->FileName.Buffer, QZ_IO_TAG);
}

PFILE_OBJECT
NTAPI
IoCreateStreamFileObjectEx(IN PFILE_OBJECT FileObject OPTIONAL,
                           IN PDEVICE_OBJECT DeviceObject OPTIONAL,
                           OUT PHANDLE FileObjectHandle OPTIONAL)
{
    PDEVICE_OBJECT Device = FileObject ? FileObject->DeviceObject : DeviceObject;
    PFILE_OBJECT Stream;

    if (FileObjectHandle)
        *FileObjectHandle = NULL;
    if (!NT_SUCCESS(QzIoCreateFileObject(Device, Device->Vpb, FO_STREAM_FILE, &Stream)))
        ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    return Stream;
}

PFILE_OBJECT
NTAPI
IoCreateStreamFileObject(IN PFILE_OBJECT FileObject OPTIONAL,
                         IN PDEVICE_OBJECT DeviceObject OPTIONAL)
{
    return IoCreateStreamFileObjectEx(FileObject, DeviceObject, NULL);
}

PFILE_OBJECT
NTAPI
IoCreateStreamFileObjectLite(IN PFILE_OBJECT FileObject OPTIONAL,
                             IN PDEVICE_OBJECT DeviceObject OPTIONAL)
{
    return IoCreateStreamFileObjectEx(FileObject, DeviceObject, NULL);
}

/* ---------------------------------------------------------------------- */
/* Opening: the parse procedures                                           */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
QzIoParseDevice(PVOID ParseObject, PVOID ObjectType, PACCESS_STATE AccessState,
                KPROCESSOR_MODE AccessMode, ULONG Attributes,
                PUNICODE_STRING CompleteName, PUNICODE_STRING RemainingName,
                PVOID Context, PSECURITY_QUALITY_OF_SERVICE SecurityQos, PVOID *Object)
{
    PQZ_OPEN_PACKET Packet = Context;
    PDEVICE_OBJECT Device = ParseObject, Target;
    PFILE_OBJECT FileObject;
    PVPB Vpb = NULL;
    ACCESS_STATE LocalAccessState;
    IO_SECURITY_CONTEXT SecurityContext;
    IO_STATUS_BLOCK IoStatus;
    PIO_STACK_LOCATION Stack;
    KEVENT Event;
    ULONG Flags = 0;
    NTSTATUS Status;
    PIRP Irp;

    UNREFERENCED_PARAMETER(ObjectType);
    UNREFERENCED_PARAMETER(AccessMode);
    UNREFERENCED_PARAMETER(CompleteName);
    UNREFERENCED_PARAMETER(SecurityQos);

    *Object = NULL;
    if (!Packet || Packet->Type != IO_TYPE_OPEN_PACKET)
        return STATUS_OBJECT_TYPE_MISMATCH;
    Packet->ParseCalled = TRUE;

    if (Packet->RelatedFileObject)
        Device = Packet->RelatedFileObject->DeviceObject;
    if ((QzDeviceExtension(Device)->ExtensionFlags & (DOE_DELETE_PENDING | DOE_UNLOAD_PENDING)) ||
        (Device->Flags & DO_DEVICE_INITIALIZING))
    {
        return STATUS_NO_SUCH_DEVICE;
    }
    if ((Device->Flags & DO_EXCLUSIVE) && Device->ReferenceCount && !Packet->RelatedFileObject)
        return STATUS_ACCESS_DENIED;

    /* Storage: find (or mount) the volume */
    if (Packet->RelatedFileObject)
    {
        Vpb = Packet->RelatedFileObject->Vpb;
        Target = IoGetRelatedDeviceObject(Packet->RelatedFileObject);
        if (Packet->RelatedFileObject->Flags & FO_DIRECT_DEVICE_OPEN)
            Flags |= FO_DIRECT_DEVICE_OPEN;
    }
    else if (Device->Vpb)
    {
        Status = QzIoMountVolume(Device);
        if (NT_SUCCESS(Status))
        {
            Vpb = Device->Vpb;
            Target = QzIoGetAttachedDevice(Vpb->DeviceObject);
        }
        else if (Status == STATUS_UNRECOGNIZED_VOLUME && !RemainingName->Length)
        {
            Target = QzIoGetAttachedDevice(Device);
            Flags |= FO_DIRECT_DEVICE_OPEN;
        }
        else
        {
            return Status;
        }
    }
    else
    {
        Target = QzIoGetAttachedDevice(Device);
    }

    if (Packet->CreateOptions & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT))
        Flags |= FO_SYNCHRONOUS_IO;
    if (Packet->CreateOptions & FILE_SYNCHRONOUS_IO_ALERT)
        Flags |= FO_ALERTABLE_IO;
    if (Packet->CreateOptions & FILE_NO_INTERMEDIATE_BUFFERING)
        Flags |= FO_NO_INTERMEDIATE_BUFFERING;
    if (Packet->CreateOptions & FILE_WRITE_THROUGH)
        Flags |= FO_WRITE_THROUGH;
    if (Packet->CreateOptions & FILE_SEQUENTIAL_ONLY)
        Flags |= FO_SEQUENTIAL_ONLY;
    if (Packet->CreateOptions & FILE_RANDOM_ACCESS)
        Flags |= FO_RANDOM_ACCESS;

    Status = QzIoCreateFileObject(Device, Vpb, Flags, &FileObject);
    if (!NT_SUCCESS(Status))
        return Status;
    FileObject->RelatedFileObject = Packet->RelatedFileObject;

    if (RemainingName->Length)
    {
        FileObject->FileName.Buffer = ExAllocatePoolWithTag(NonPagedPool,
                                                            RemainingName->Length + sizeof(WCHAR),
                                                            QZ_IO_TAG);
        if (!FileObject->FileName.Buffer)
        {
            QzIoReleaseFileObjectDevice(FileObject);
            ObDereferenceObject(FileObject);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlCopyMemory(FileObject->FileName.Buffer, RemainingName->Buffer, RemainingName->Length);
        FileObject->FileName.Buffer[RemainingName->Length / sizeof(WCHAR)] = UNICODE_NULL;
        FileObject->FileName.Length = RemainingName->Length;
        FileObject->FileName.MaximumLength = RemainingName->Length + sizeof(WCHAR);
    }

    /* The create IRP */
    Irp = IoAllocateIrp(Target->StackSize, FALSE);
    if (!Irp)
    {
        QzIoReleaseFileObjectDevice(FileObject);
        ObDereferenceObject(FileObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(&LocalAccessState, sizeof(LocalAccessState));
    LocalAccessState.OriginalDesiredAccess = Packet->DesiredAccess;
    LocalAccessState.RemainingDesiredAccess = Packet->DesiredAccess;
    if (!AccessState)
        AccessState = &LocalAccessState;
    SecurityContext.SecurityQos = NULL;
    SecurityContext.AccessState = AccessState;
    SecurityContext.DesiredAccess = Packet->DesiredAccess;
    SecurityContext.FullCreateOptions = Packet->CreateOptions;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp->Flags = IRP_CREATE_OPERATION | IRP_SYNCHRONOUS_API;
    Irp->RequestorMode = KernelMode;
    Irp->UserEvent = &Event;
    Irp->UserIosb = &IoStatus;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->Tail.Overlay.OriginalFileObject = FileObject;
    Irp->AssociatedIrp.SystemBuffer = Packet->EaBuffer;
    if (Packet->AllocationSize)
        Irp->Overlay.AllocationSize = *Packet->AllocationSize;

    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = (Packet->CreateFileType == CreateFileTypeNamedPipe) ? IRP_MJ_CREATE_NAMED_PIPE :
                           (Packet->CreateFileType == CreateFileTypeMailslot) ? IRP_MJ_CREATE_MAILSLOT :
                           IRP_MJ_CREATE;
    Stack->Flags = (Attributes & OBJ_CASE_INSENSITIVE) ? 0 : SL_CASE_SENSITIVE;
    if (Packet->Options & IO_OPEN_TARGET_DIRECTORY)
        Stack->Flags |= SL_OPEN_TARGET_DIRECTORY;
    Stack->FileObject = FileObject;
    Stack->Parameters.Create.SecurityContext = &SecurityContext;
    Stack->Parameters.Create.Options = (Packet->Disposition << 24) | (Packet->CreateOptions & 0xFFFFFF);
    Stack->Parameters.Create.FileAttributes = (USHORT)Packet->FileAttributes;
    Stack->Parameters.Create.ShareAccess = (USHORT)Packet->ShareAccess;
    Stack->Parameters.Create.EaLength = Packet->EaLength;

    IoStatus.Status = STATUS_SUCCESS;
    IoStatus.Information = 0;
    Status = QzIoSynchronousCall(Target, Irp, &Event, &IoStatus);
    if (Status != STATUS_PENDING)
        Status = IoStatus.Status;

    Packet->FinalStatus = Status;
    Packet->Information = IoStatus.Information;

    if (Status == STATUS_REPARSE)
        Status = STATUS_NOT_SUPPORTED;   /* reparse points arrive with real file systems */

    if (!NT_SUCCESS(Status))
    {
        /* Never opened: no cleanup or close for the driver */
        QzIoReleaseFileObjectDevice(FileObject);
        ObDereferenceObject(FileObject);
        Packet->FinalStatus = Status;
        return Status;
    }

    Packet->FileObject = FileObject;
    *Object = FileObject;
    return STATUS_SUCCESS;
}

/* Relative open below an open file: "dir handle" + "name" */
NTSTATUS
NTAPI
QzIoParseFile(PVOID ParseObject, PVOID ObjectType, PACCESS_STATE AccessState,
              KPROCESSOR_MODE AccessMode, ULONG Attributes,
              PUNICODE_STRING CompleteName, PUNICODE_STRING RemainingName,
              PVOID Context, PSECURITY_QUALITY_OF_SERVICE SecurityQos, PVOID *Object)
{
    PQZ_OPEN_PACKET Packet = Context;
    PFILE_OBJECT Related = ParseObject;

    if (!Packet || Packet->Type != IO_TYPE_OPEN_PACKET || !Related->DeviceObject)
        return STATUS_OBJECT_TYPE_MISMATCH;
    Packet->RelatedFileObject = Related;
    return QzIoParseDevice(Related->DeviceObject, ObjectType, AccessState, AccessMode, Attributes,
                           CompleteName, RemainingName, Context, SecurityQos, Object);
}

NTSTATUS
NTAPI
IoCreateFile(OUT PHANDLE FileHandle,
             IN ACCESS_MASK DesiredAccess,
             IN POBJECT_ATTRIBUTES ObjectAttributes,
             OUT PIO_STATUS_BLOCK IoStatusBlock,
             IN PLARGE_INTEGER AllocationSize OPTIONAL,
             IN ULONG FileAttributes,
             IN ULONG ShareAccess,
             IN ULONG Disposition,
             IN ULONG CreateOptions,
             IN PVOID EaBuffer OPTIONAL,
             IN ULONG EaLength,
             IN CREATE_FILE_TYPE CreateFileType,
             IN PVOID ExtraCreateParameters OPTIONAL,
             IN ULONG Options)
{
    QZ_OPEN_PACKET Packet;
    NTSTATUS Status;

    RtlZeroMemory(&Packet, sizeof(Packet));
    Packet.Type = IO_TYPE_OPEN_PACKET;
    Packet.Size = sizeof(Packet);
    Packet.AllocationSize = AllocationSize;
    Packet.FileAttributes = FileAttributes;
    Packet.ShareAccess = ShareAccess;
    Packet.Disposition = Disposition;
    Packet.CreateOptions = CreateOptions;
    Packet.EaBuffer = EaBuffer;
    Packet.EaLength = EaLength;
    Packet.DesiredAccess = DesiredAccess;
    Packet.Options = Options;
    Packet.CreateFileType = CreateFileType;
    Packet.ExtraCreateParameters = ExtraCreateParameters;

    Status = ObOpenObjectByName(ObjectAttributes, NULL, KernelMode, NULL, DesiredAccess,
                                &Packet, FileHandle);
    if (Packet.ParseCalled)
    {
        IoStatusBlock->Status = Packet.FinalStatus;
        IoStatusBlock->Information = Packet.Information;
        if (NT_SUCCESS(Status))
            Status = Packet.FinalStatus;
    }
    else if (NT_SUCCESS(Status))
    {
        /* The name led to something other than a device or a file */
        ZwClose(*FileHandle);
        *FileHandle = NULL;
        Status = STATUS_OBJECT_TYPE_MISMATCH;
    }
    return Status;
}

NTSTATUS
NTAPI
IoCreateFileSpecifyDeviceObjectHint(OUT PHANDLE FileHandle,
                                    IN ACCESS_MASK DesiredAccess,
                                    IN POBJECT_ATTRIBUTES ObjectAttributes,
                                    OUT PIO_STATUS_BLOCK IoStatusBlock,
                                    IN PLARGE_INTEGER AllocationSize OPTIONAL,
                                    IN ULONG FileAttributes,
                                    IN ULONG ShareAccess,
                                    IN ULONG Disposition,
                                    IN ULONG CreateOptions,
                                    IN PVOID EaBuffer OPTIONAL,
                                    IN ULONG EaLength,
                                    IN CREATE_FILE_TYPE CreateFileType,
                                    IN PVOID ExtraCreateParameters OPTIONAL,
                                    IN ULONG Options,
                                    IN PVOID DeviceObject)
{
    UNREFERENCED_PARAMETER(DeviceObject);   /* no file system filters yet */
    return IoCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize,
                        FileAttributes, ShareAccess, Disposition, CreateOptions, EaBuffer,
                        EaLength, CreateFileType, ExtraCreateParameters, Options);
}

NTSTATUS
NTAPI
ZwCreateFile(OUT PHANDLE FileHandle,
             IN ACCESS_MASK DesiredAccess,
             IN POBJECT_ATTRIBUTES ObjectAttributes,
             OUT PIO_STATUS_BLOCK IoStatusBlock,
             IN PLARGE_INTEGER AllocationSize OPTIONAL,
             IN ULONG FileAttributes,
             IN ULONG ShareAccess,
             IN ULONG CreateDisposition,
             IN ULONG CreateOptions,
             IN PVOID EaBuffer OPTIONAL,
             IN ULONG EaLength)
{
    return IoCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize,
                        FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer,
                        EaLength, CreateFileTypeNone, NULL, 0);
}

NTSTATUS
NTAPI
NtCreateFile(OUT PHANDLE FileHandle,
             IN ACCESS_MASK DesiredAccess,
             IN POBJECT_ATTRIBUTES ObjectAttributes,
             OUT PIO_STATUS_BLOCK IoStatusBlock,
             IN PLARGE_INTEGER AllocationSize OPTIONAL,
             IN ULONG FileAttributes,
             IN ULONG ShareAccess,
             IN ULONG CreateDisposition,
             IN ULONG CreateOptions,
             IN PVOID EaBuffer OPTIONAL,
             IN ULONG EaLength)
{
    return ZwCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize,
                        FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer,
                        EaLength);
}

NTSTATUS
NTAPI
ZwOpenFile(OUT PHANDLE FileHandle,
           IN ACCESS_MASK DesiredAccess,
           IN POBJECT_ATTRIBUTES ObjectAttributes,
           OUT PIO_STATUS_BLOCK IoStatusBlock,
           IN ULONG ShareAccess,
           IN ULONG OpenOptions)
{
    return IoCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, NULL, 0,
                        ShareAccess, FILE_OPEN, OpenOptions, NULL, 0, CreateFileTypeNone, NULL, 0);
}

NTSTATUS
NTAPI
NtOpenFile(OUT PHANDLE FileHandle,
           IN ACCESS_MASK DesiredAccess,
           IN POBJECT_ATTRIBUTES ObjectAttributes,
           OUT PIO_STATUS_BLOCK IoStatusBlock,
           IN ULONG ShareAccess,
           IN ULONG OpenOptions)
{
    return ZwOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess,
                      OpenOptions);
}

/* ---------------------------------------------------------------------- */
/* Handle-based requests                                                   */
/* ---------------------------------------------------------------------- */

typedef struct _QZ_FILE_REQUEST
{
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT Device;
    PKEVENT UserEvent;          /* referenced, from the caller's handle */
    BOOLEAN Synchronous;
    KEVENT LocalEvent;
    IO_STATUS_BLOCK LocalIosb;
    PIRP Irp;
} QZ_FILE_REQUEST, *PQZ_FILE_REQUEST;

/* Reference the file (and event), allocate an IRP for its device */
static NTSTATUS
QzIoBeginRequest(PQZ_FILE_REQUEST Request, HANDLE FileHandle, ACCESS_MASK Access, HANDLE Event,
                 PIO_APC_ROUTINE ApcRoutine, BOOLEAN ForceSynchronous)
{
    NTSTATUS Status;

    RtlZeroMemory(Request, sizeof(*Request));
    if (ApcRoutine)
        return STATUS_NOT_IMPLEMENTED;   /* completion APCs need APC delivery (B6) */

    Status = ObReferenceObjectByHandle(FileHandle, Access, IoFileObjectType, KernelMode,
                                       (PVOID *)&Request->FileObject, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Event)
    {
        Status = ObReferenceObjectByHandle(Event, EVENT_MODIFY_STATE, ExEventObjectType,
                                           KernelMode, (PVOID *)&Request->UserEvent, NULL);
        if (!NT_SUCCESS(Status))
        {
            ObDereferenceObject(Request->FileObject);
            return Status;
        }
        KeClearEvent(Request->UserEvent);
    }

    Request->Synchronous = ForceSynchronous || (Request->FileObject->Flags & FO_SYNCHRONOUS_IO);
    Request->Device = IoGetRelatedDeviceObject(Request->FileObject);
    Request->Irp = IoAllocateIrp(Request->Device->StackSize, FALSE);
    if (!Request->Irp)
    {
        if (Request->UserEvent)
            ObDereferenceObject(Request->UserEvent);
        ObDereferenceObject(Request->FileObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Request->Irp->Tail.Overlay.OriginalFileObject = Request->FileObject;
    Request->Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Request->Irp->RequestorMode = KernelMode;
    KeClearEvent(&Request->FileObject->Event);
    return STATUS_SUCCESS;
}

/* Undo QzIoBeginRequest when the IRP never reaches a driver */
static NTSTATUS
QzIoAbortRequest(PQZ_FILE_REQUEST Request, NTSTATUS Status)
{
    PIRP Irp = Request->Irp;

    if (Irp->Flags & IRP_DEALLOCATE_BUFFER)
        ExFreePool(Irp->AssociatedIrp.SystemBuffer);
    while (Irp->MdlAddress)
    {
        PMDL Next = Irp->MdlAddress->Next;
        IoFreeMdl(Irp->MdlAddress);
        Irp->MdlAddress = Next;
    }
    IoFreeIrp(Irp);
    if (Request->UserEvent)
        ObDereferenceObject(Request->UserEvent);
    ObDereferenceObject(Request->FileObject);
    return Status;
}

/* Send the IRP; synchronous requests wait and report the final status */
static NTSTATUS
QzIoCallRequest(PQZ_FILE_REQUEST Request, PIO_STATUS_BLOCK IoStatusBlock, BOOLEAN UpdateOffset)
{
    PIRP Irp = Request->Irp;
    PFILE_OBJECT FileObject = Request->FileObject;
    NTSTATUS Status;

    if (Request->Synchronous)
    {
        KeInitializeEvent(&Request->LocalEvent, NotificationEvent, FALSE);
        Irp->UserEvent = &Request->LocalEvent;
        Irp->UserIosb = &Request->LocalIosb;
        Irp->Flags |= IRP_SYNCHRONOUS_API;
        ObReferenceObject(FileObject);   /* the IRP's reference goes at completion */

        Status = IoCallDriver(Request->Device, Irp);
        if (Status == STATUS_PENDING)
            KeWaitForSingleObject(&Request->LocalEvent, Executive, KernelMode, FALSE, NULL);
        Status = Request->LocalIosb.Status;
        *IoStatusBlock = Request->LocalIosb;
        /* A file system moves a synchronous file's position itself; a
           device opened directly (no mounted volume) has us do it */
        if (UpdateOffset && NT_SUCCESS(Status) && !(FileObject->Vpb && (FileObject->Vpb->Flags & VPB_MOUNTED)))
            FileObject->CurrentByteOffset.QuadPart += Request->LocalIosb.Information;
        if (Request->UserEvent)
        {
            KeSetEvent(Request->UserEvent, 0, FALSE);
            ObDereferenceObject(Request->UserEvent);
        }
        ObDereferenceObject(FileObject);
        return Status;
    }

    /* Asynchronous: the completion stores the status and signals */
    Irp->UserEvent = Request->UserEvent;
    Irp->UserIosb = IoStatusBlock;
    return IoCallDriver(Request->Device, Irp);
}

static NTSTATUS
QzIoReadWrite(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
              PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
              PLARGE_INTEGER ByteOffset, PULONG Key, BOOLEAN Write)
{
    QZ_FILE_REQUEST Request;
    PIO_STACK_LOCATION Stack;
    LARGE_INTEGER Offset;
    NTSTATUS Status;

    Status = QzIoBeginRequest(&Request, FileHandle, Write ? FILE_WRITE_DATA : FILE_READ_DATA,
                              Event, ApcRoutine, FALSE);
    if (!NT_SUCCESS(Status))
        return Status;

    if (ByteOffset && !(ByteOffset->HighPart == -1 &&
                        ByteOffset->LowPart == FILE_USE_FILE_POINTER_POSITION))
    {
        Offset = *ByteOffset;
    }
    else if (Request.Synchronous)
    {
        Offset = Request.FileObject->CurrentByteOffset;
    }
    else
    {
        return QzIoAbortRequest(&Request, STATUS_INVALID_PARAMETER);
    }
    if (Request.Synchronous && ByteOffset)
        Request.FileObject->CurrentByteOffset = Offset;

    Request.Irp->Flags |= Write ? IRP_WRITE_OPERATION : IRP_READ_OPERATION;
    if (Request.FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING)
        Request.Irp->Flags |= IRP_NOCACHE;
    if (!QzIoAttachBuffer(Request.Irp, Request.Device, Buffer, Length, !Write))
        return QzIoAbortRequest(&Request, STATUS_INSUFFICIENT_RESOURCES);
    Request.Irp->UserBuffer = Buffer;

    Stack = IoGetNextIrpStackLocation(Request.Irp);
    Stack->MajorFunction = Write ? IRP_MJ_WRITE : IRP_MJ_READ;
    Stack->FileObject = Request.FileObject;
    Stack->Parameters.Read.Length = Length;
    Stack->Parameters.Read.ByteOffset = Offset;
    Stack->Parameters.Read.Key = Key ? *Key : 0;

    return QzIoCallRequest(&Request, IoStatusBlock, TRUE);
}

NTSTATUS
NTAPI
ZwReadFile(IN HANDLE FileHandle,
           IN HANDLE Event OPTIONAL,
           IN PIO_APC_ROUTINE ApcRoutine OPTIONAL,
           IN PVOID ApcContext OPTIONAL,
           OUT PIO_STATUS_BLOCK IoStatusBlock,
           OUT PVOID Buffer,
           IN ULONG Length,
           IN PLARGE_INTEGER ByteOffset OPTIONAL,
           IN PULONG Key OPTIONAL)
{
    UNREFERENCED_PARAMETER(ApcContext);
    return QzIoReadWrite(FileHandle, Event, ApcRoutine, IoStatusBlock, Buffer, Length,
                         ByteOffset, Key, FALSE);
}

NTSTATUS
NTAPI
ZwWriteFile(IN HANDLE FileHandle,
            IN HANDLE Event OPTIONAL,
            IN PIO_APC_ROUTINE ApcRoutine OPTIONAL,
            IN PVOID ApcContext OPTIONAL,
            OUT PIO_STATUS_BLOCK IoStatusBlock,
            IN PVOID Buffer,
            IN ULONG Length,
            IN PLARGE_INTEGER ByteOffset OPTIONAL,
            IN PULONG Key OPTIONAL)
{
    UNREFERENCED_PARAMETER(ApcContext);
    return QzIoReadWrite(FileHandle, Event, ApcRoutine, IoStatusBlock, Buffer, Length,
                         ByteOffset, Key, TRUE);
}

NTSTATUS
NTAPI
NtReadFile(IN HANDLE FileHandle, IN HANDLE Event OPTIONAL, IN PIO_APC_ROUTINE ApcRoutine OPTIONAL,
           IN PVOID ApcContext OPTIONAL, OUT PIO_STATUS_BLOCK IoStatusBlock, OUT PVOID Buffer,
           IN ULONG Length, IN PLARGE_INTEGER ByteOffset OPTIONAL, IN PULONG Key OPTIONAL)
{
    return ZwReadFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length,
                      ByteOffset, Key);
}

NTSTATUS
NTAPI
NtWriteFile(IN HANDLE FileHandle, IN HANDLE Event OPTIONAL, IN PIO_APC_ROUTINE ApcRoutine OPTIONAL,
            IN PVOID ApcContext OPTIONAL, OUT PIO_STATUS_BLOCK IoStatusBlock, IN PVOID Buffer,
            IN ULONG Length, IN PLARGE_INTEGER ByteOffset OPTIONAL, IN PULONG Key OPTIONAL)
{
    return ZwWriteFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length,
                       ByteOffset, Key);
}

static NTSTATUS
QzIoControl(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
            PIO_STATUS_BLOCK IoStatusBlock, ULONG Code, PVOID InputBuffer, ULONG InputLength,
            PVOID OutputBuffer, ULONG OutputLength, BOOLEAN FileSystem)
{
    QZ_FILE_REQUEST Request;
    PIO_STACK_LOCATION Stack;
    ULONG Method = METHOD_FROM_CTL_CODE(Code);
    PIRP Irp;
    NTSTATUS Status;

    Status = QzIoBeginRequest(&Request, FileHandle, 0, Event, ApcRoutine, FALSE);
    if (!NT_SUCCESS(Status))
        return Status;
    Irp = Request.Irp;

    if (Method == METHOD_BUFFERED || Method == METHOD_IN_DIRECT || Method == METHOD_OUT_DIRECT)
    {
        ULONG Size = (Method == METHOD_BUFFERED) ? max(InputLength, OutputLength) : InputLength;
        if (Size)
        {
            Irp->AssociatedIrp.SystemBuffer = ExAllocatePoolWithTag(NonPagedPool, Size, QZ_IRP_TAG);
            if (!Irp->AssociatedIrp.SystemBuffer)
                return QzIoAbortRequest(&Request, STATUS_INSUFFICIENT_RESOURCES);
            if (InputBuffer && InputLength)
                RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, InputBuffer, InputLength);
            Irp->Flags |= IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER;
            if (Method == METHOD_BUFFERED && OutputLength)
                Irp->Flags |= IRP_INPUT_OPERATION;
        }
        if (Method != METHOD_BUFFERED && OutputBuffer && OutputLength)
        {
            PMDL Mdl = IoAllocateMdl(OutputBuffer, OutputLength, FALSE, FALSE, Irp);
            if (!Mdl)
                return QzIoAbortRequest(&Request, STATUS_INSUFFICIENT_RESOURCES);
            MmProbeAndLockPages(Mdl, KernelMode,
                                Method == METHOD_IN_DIRECT ? IoReadAccess : IoWriteAccess);
        }
    }
    Irp->UserBuffer = OutputBuffer;

    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = FileSystem ? IRP_MJ_FILE_SYSTEM_CONTROL : IRP_MJ_DEVICE_CONTROL;
    Stack->MinorFunction = FileSystem ? IRP_MN_USER_FS_REQUEST : 0;
    Stack->FileObject = Request.FileObject;
    Stack->Parameters.DeviceIoControl.IoControlCode = Code;
    Stack->Parameters.DeviceIoControl.InputBufferLength = InputLength;
    Stack->Parameters.DeviceIoControl.OutputBufferLength = OutputLength;
    if (Method == METHOD_NEITHER)
        Stack->Parameters.DeviceIoControl.Type3InputBuffer = InputBuffer;

    return QzIoCallRequest(&Request, IoStatusBlock, FALSE);
}

NTSTATUS
NTAPI
ZwDeviceIoControlFile(IN HANDLE FileHandle,
                      IN HANDLE Event OPTIONAL,
                      IN PIO_APC_ROUTINE ApcRoutine OPTIONAL,
                      IN PVOID ApcContext OPTIONAL,
                      OUT PIO_STATUS_BLOCK IoStatusBlock,
                      IN ULONG IoControlCode,
                      IN PVOID InputBuffer OPTIONAL,
                      IN ULONG InputBufferLength,
                      OUT PVOID OutputBuffer OPTIONAL,
                      IN ULONG OutputBufferLength)
{
    UNREFERENCED_PARAMETER(ApcContext);
    return QzIoControl(FileHandle, Event, ApcRoutine, IoStatusBlock, IoControlCode, InputBuffer,
                       InputBufferLength, OutputBuffer, OutputBufferLength, FALSE);
}

NTSTATUS
NTAPI
NtDeviceIoControlFile(IN HANDLE FileHandle, IN HANDLE Event OPTIONAL,
                      IN PIO_APC_ROUTINE ApcRoutine OPTIONAL, IN PVOID ApcContext OPTIONAL,
                      OUT PIO_STATUS_BLOCK IoStatusBlock, IN ULONG IoControlCode,
                      IN PVOID InputBuffer OPTIONAL, IN ULONG InputBufferLength,
                      OUT PVOID OutputBuffer OPTIONAL, IN ULONG OutputBufferLength)
{
    return ZwDeviceIoControlFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
                                 IoControlCode, InputBuffer, InputBufferLength, OutputBuffer,
                                 OutputBufferLength);
}

NTSTATUS
NTAPI
ZwFsControlFile(IN HANDLE FileHandle,
                IN HANDLE Event OPTIONAL,
                IN PIO_APC_ROUTINE ApcRoutine OPTIONAL,
                IN PVOID ApcContext OPTIONAL,
                OUT PIO_STATUS_BLOCK IoStatusBlock,
                IN ULONG FsControlCode,
                IN PVOID InputBuffer OPTIONAL,
                IN ULONG InputBufferLength,
                OUT PVOID OutputBuffer OPTIONAL,
                IN ULONG OutputBufferLength)
{
    UNREFERENCED_PARAMETER(ApcContext);
    return QzIoControl(FileHandle, Event, ApcRoutine, IoStatusBlock, FsControlCode, InputBuffer,
                       InputBufferLength, OutputBuffer, OutputBufferLength, TRUE);
}

NTSTATUS
NTAPI
NtFsControlFile(IN HANDLE FileHandle, IN HANDLE Event OPTIONAL,
                IN PIO_APC_ROUTINE ApcRoutine OPTIONAL, IN PVOID ApcContext OPTIONAL,
                OUT PIO_STATUS_BLOCK IoStatusBlock, IN ULONG FsControlCode,
                IN PVOID InputBuffer OPTIONAL, IN ULONG InputBufferLength,
                OUT PVOID OutputBuffer OPTIONAL, IN ULONG OutputBufferLength)
{
    return ZwFsControlFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FsControlCode,
                           InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength);
}

/* Query/set requests always complete before returning */
static NTSTATUS
QzIoInformation(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
                ULONG Class, UCHAR MajorFunction)
{
    QZ_FILE_REQUEST Request;
    PIO_STACK_LOCATION Stack;
    BOOLEAN Set = (MajorFunction == IRP_MJ_SET_INFORMATION ||
                   MajorFunction == IRP_MJ_SET_VOLUME_INFORMATION);
    NTSTATUS Status;
    PIRP Irp;

    Status = QzIoBeginRequest(&Request, FileHandle, 0, NULL, NULL, TRUE);
    if (!NT_SUCCESS(Status))
        return Status;
    Irp = Request.Irp;

    /* The I/O manager answers the file position and device type itself */
    if (MajorFunction == IRP_MJ_QUERY_INFORMATION && Class == FilePositionInformation &&
        (Request.FileObject->Flags & FO_SYNCHRONOUS_IO))
    {
        if (Length < sizeof(FILE_POSITION_INFORMATION))
            return QzIoAbortRequest(&Request, STATUS_INFO_LENGTH_MISMATCH);
        ((PFILE_POSITION_INFORMATION)Buffer)->CurrentByteOffset = Request.FileObject->CurrentByteOffset;
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = sizeof(FILE_POSITION_INFORMATION);
        return QzIoAbortRequest(&Request, STATUS_SUCCESS);
    }
    if (MajorFunction == IRP_MJ_SET_INFORMATION && Class == FilePositionInformation &&
        (Request.FileObject->Flags & FO_SYNCHRONOUS_IO))
    {
        if (Length < sizeof(FILE_POSITION_INFORMATION))
            return QzIoAbortRequest(&Request, STATUS_INFO_LENGTH_MISMATCH);
        Request.FileObject->CurrentByteOffset = ((PFILE_POSITION_INFORMATION)Buffer)->CurrentByteOffset;
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
        return QzIoAbortRequest(&Request, STATUS_SUCCESS);
    }
    if (MajorFunction == IRP_MJ_QUERY_VOLUME_INFORMATION && Class == FileFsDeviceInformation)
    {
        PFILE_FS_DEVICE_INFORMATION Info = Buffer;
        if (Length < sizeof(*Info))
            return QzIoAbortRequest(&Request, STATUS_INFO_LENGTH_MISMATCH);
        Info->DeviceType = Request.FileObject->DeviceObject->DeviceType;
        Info->Characteristics = Request.FileObject->DeviceObject->Characteristics;
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = sizeof(*Info);
        return QzIoAbortRequest(&Request, STATUS_SUCCESS);
    }

    if (Length)
    {
        Irp->AssociatedIrp.SystemBuffer = ExAllocatePoolWithTag(NonPagedPool, Length, QZ_IRP_TAG);
        if (!Irp->AssociatedIrp.SystemBuffer)
            return QzIoAbortRequest(&Request, STATUS_INSUFFICIENT_RESOURCES);
        if (Set)
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Buffer, Length);
        Irp->Flags |= IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER | (Set ? 0 : IRP_INPUT_OPERATION);
    }
    Irp->UserBuffer = Buffer;

    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = MajorFunction;
    Stack->FileObject = Request.FileObject;
    /* QueryFile/SetFile/QueryVolume/SetVolume share {Length, Class} */
    Stack->Parameters.QueryFile.Length = Length;
    Stack->Parameters.QueryFile.FileInformationClass = (FILE_INFORMATION_CLASS)Class;

    /* A rename (or link) within the file's directory: no target directory
       object, the file system takes the new name from the buffer */
    if (MajorFunction == IRP_MJ_SET_INFORMATION && (Class == FileRenameInformation || Class == FileLinkInformation))
    {
        PFILE_RENAME_INFORMATION Rename = Buffer;
        ULONG i;
        if (Length < FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) ||
            Length < FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) + Rename->FileNameLength)
            return QzIoAbortRequest(&Request, STATUS_INFO_LENGTH_MISMATCH);
        for (i = 0; i < Rename->FileNameLength / sizeof(WCHAR); i++)
            if (Rename->FileName[i] == L'\\')
                return QzIoAbortRequest(&Request, STATUS_NOT_SUPPORTED);   /* across directories: later */
        if (Rename->RootDirectory)
            return QzIoAbortRequest(&Request, STATUS_NOT_SUPPORTED);
        Stack->Parameters.SetFile.FileObject = NULL;
        Stack->Parameters.SetFile.ReplaceIfExists = Rename->ReplaceIfExists;
    }

    return QzIoCallRequest(&Request, IoStatusBlock, FALSE);
}

NTSTATUS
NTAPI
ZwQueryInformationFile(IN HANDLE FileHandle,
                       OUT PIO_STATUS_BLOCK IoStatusBlock,
                       OUT PVOID FileInformation,
                       IN ULONG Length,
                       IN FILE_INFORMATION_CLASS FileInformationClass)
{
    return QzIoInformation(FileHandle, IoStatusBlock, FileInformation, Length,
                           FileInformationClass, IRP_MJ_QUERY_INFORMATION);
}

NTSTATUS
NTAPI
ZwSetInformationFile(IN HANDLE FileHandle,
                     OUT PIO_STATUS_BLOCK IoStatusBlock,
                     IN PVOID FileInformation,
                     IN ULONG Length,
                     IN FILE_INFORMATION_CLASS FileInformationClass)
{
    return QzIoInformation(FileHandle, IoStatusBlock, FileInformation, Length,
                           FileInformationClass, IRP_MJ_SET_INFORMATION);
}

NTSTATUS
NTAPI
ZwQueryVolumeInformationFile(IN HANDLE FileHandle,
                             OUT PIO_STATUS_BLOCK IoStatusBlock,
                             OUT PVOID FsInformation,
                             IN ULONG Length,
                             IN FS_INFORMATION_CLASS FsInformationClass)
{
    return QzIoInformation(FileHandle, IoStatusBlock, FsInformation, Length,
                           FsInformationClass, IRP_MJ_QUERY_VOLUME_INFORMATION);
}

NTSTATUS
NTAPI
ZwFlushBuffersFile(IN HANDLE FileHandle,
                   OUT PIO_STATUS_BLOCK IoStatusBlock)
{
    QZ_FILE_REQUEST Request;
    PIO_STACK_LOCATION Stack;
    NTSTATUS Status;

    Status = QzIoBeginRequest(&Request, FileHandle, 0, NULL, NULL, TRUE);
    if (!NT_SUCCESS(Status))
        return Status;
    Stack = IoGetNextIrpStackLocation(Request.Irp);
    Stack->MajorFunction = IRP_MJ_FLUSH_BUFFERS;
    Stack->FileObject = Request.FileObject;
    return QzIoCallRequest(&Request, IoStatusBlock, FALSE);
}

BOOLEAN
NTAPI
IoIsOperationSynchronous(IN PIRP Irp)
{
    PFILE_OBJECT FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;

    if (Irp->Flags & (IRP_SYNCHRONOUS_API | IRP_SYNCHRONOUS_PAGING_IO))
        return TRUE;
    if (Irp->Flags & IRP_PAGING_IO)
        return FALSE;
    return FileObject && (FileObject->Flags & FO_SYNCHRONOUS_IO);
}

/* ---------------------------------------------------------------------- */
/* Share access (the rules every file system applies to concurrent opens)  */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
IoCheckShareAccess(IN ACCESS_MASK DesiredAccess,
                   IN ULONG DesiredShareAccess,
                   IN PFILE_OBJECT FileObject,
                   IN PSHARE_ACCESS ShareAccess,
                   IN BOOLEAN Update)
{
    BOOLEAN Read = (DesiredAccess & (FILE_EXECUTE | FILE_READ_DATA)) != 0;
    BOOLEAN Write = (DesiredAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    BOOLEAN Delete = (DesiredAccess & DELETE) != 0;
    BOOLEAN SharedRead = (DesiredShareAccess & FILE_SHARE_READ) != 0;
    BOOLEAN SharedWrite = (DesiredShareAccess & FILE_SHARE_WRITE) != 0;
    BOOLEAN SharedDelete = (DesiredShareAccess & FILE_SHARE_DELETE) != 0;

    FileObject->ReadAccess = Read;
    FileObject->WriteAccess = Write;
    FileObject->DeleteAccess = Delete;

    if (Read || Write || Delete)
    {
        FileObject->SharedRead = SharedRead;
        FileObject->SharedWrite = SharedWrite;
        FileObject->SharedDelete = SharedDelete;

        if ((Read && ShareAccess->SharedRead < ShareAccess->OpenCount) ||
            (Write && ShareAccess->SharedWrite < ShareAccess->OpenCount) ||
            (Delete && ShareAccess->SharedDelete < ShareAccess->OpenCount) ||
            (ShareAccess->Readers && !SharedRead) ||
            (ShareAccess->Writers && !SharedWrite) ||
            (ShareAccess->Deleters && !SharedDelete))
        {
            return STATUS_SHARING_VIOLATION;
        }

        if (Update)
        {
            ShareAccess->OpenCount++;
            ShareAccess->Readers += Read;
            ShareAccess->Writers += Write;
            ShareAccess->Deleters += Delete;
            ShareAccess->SharedRead += SharedRead;
            ShareAccess->SharedWrite += SharedWrite;
            ShareAccess->SharedDelete += SharedDelete;
        }
    }
    return STATUS_SUCCESS;
}

VOID
NTAPI
IoSetShareAccess(IN ACCESS_MASK DesiredAccess,
                 IN ULONG DesiredShareAccess,
                 IN PFILE_OBJECT FileObject,
                 OUT PSHARE_ACCESS ShareAccess)
{
    RtlZeroMemory(ShareAccess, sizeof(*ShareAccess));
    IoCheckShareAccess(DesiredAccess, DesiredShareAccess, FileObject, ShareAccess, TRUE);
}

VOID
NTAPI
IoRemoveShareAccess(IN PFILE_OBJECT FileObject,
                    IN OUT PSHARE_ACCESS ShareAccess)
{
    if (FileObject->ReadAccess || FileObject->WriteAccess || FileObject->DeleteAccess)
    {
        ShareAccess->OpenCount--;
        ShareAccess->Readers -= FileObject->ReadAccess;
        ShareAccess->Writers -= FileObject->WriteAccess;
        ShareAccess->Deleters -= FileObject->DeleteAccess;
        ShareAccess->SharedRead -= FileObject->SharedRead;
        ShareAccess->SharedWrite -= FileObject->SharedWrite;
        ShareAccess->SharedDelete -= FileObject->SharedDelete;
    }
}

VOID
NTAPI
IoUpdateShareAccess(IN PFILE_OBJECT FileObject,
                    IN OUT PSHARE_ACCESS ShareAccess)
{
    if (FileObject->ReadAccess || FileObject->WriteAccess || FileObject->DeleteAccess)
    {
        ShareAccess->OpenCount++;
        ShareAccess->Readers += FileObject->ReadAccess;
        ShareAccess->Writers += FileObject->WriteAccess;
        ShareAccess->Deleters += FileObject->DeleteAccess;
        ShareAccess->SharedRead += FileObject->SharedRead;
        ShareAccess->SharedWrite += FileObject->SharedWrite;
        ShareAccess->SharedDelete += FileObject->SharedDelete;
    }
}

PGENERIC_MAPPING
NTAPI
IoGetFileObjectGenericMapping(VOID)
{
    return &IoFileObjectType->TypeInfo.GenericMapping;
}

/* ---------------------------------------------------------------------- */
/* Attributes by name: open, ask, close                                    */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzIoQueryAttributesByName(POBJECT_ATTRIBUTES ObjectAttributes, FILE_INFORMATION_CLASS Class, PVOID Buffer,
                          ULONG Length)
{
    IO_STATUS_BLOCK IoStatus;
    HANDLE Handle;
    NTSTATUS Status;

    /* Opened for attributes only; the name may be a file or a directory */
    Status = NtOpenFile(&Handle, FILE_READ_ATTRIBUTES | SYNCHRONIZE, ObjectAttributes, &IoStatus,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = ZwQueryInformationFile(Handle, &IoStatus, Buffer, Length, Class);
    NtClose(Handle);
    return Status;
}

NTSTATUS
NTAPI
NtQueryAttributesFile(IN POBJECT_ATTRIBUTES ObjectAttributes,
                      OUT PFILE_BASIC_INFORMATION FileInformation)
{
    return QzIoQueryAttributesByName(ObjectAttributes, FileBasicInformation, FileInformation,
                                     sizeof(FILE_BASIC_INFORMATION));
}

NTSTATUS
NTAPI
NtQueryFullAttributesFile(IN POBJECT_ATTRIBUTES ObjectAttributes,
                          OUT PFILE_NETWORK_OPEN_INFORMATION FileInformation)
{
    return QzIoQueryAttributesByName(ObjectAttributes, FileNetworkOpenInformation, FileInformation,
                                     sizeof(FILE_NETWORK_OPEN_INFORMATION));
}
