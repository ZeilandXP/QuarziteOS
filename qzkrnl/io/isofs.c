/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     A small read-only ISO 9660 file system (\FileSystem\QzIso)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * The boot media's file system until stage B7 brings the cache manager
 * and with it the real CDFS driver. It mounts CD-ROM volumes through the
 * VPB like any file system, walks the primary volume descriptor's
 * directories (relaxed ISO 9660 names, as the QuarziteOS media are built:
 * mixed case, no ";1" versions -- both are tolerated), and serves
 * non-cached reads, file and volume information and directory listings.
 * Everything is read-only.
 */

#include "qzio.h"

#define QZ_ISO_TAG          'osIQ'
#define QZ_ISO_SECTOR       2048
#define QZ_ISO_CHUNK        (32 * QZ_ISO_SECTOR)

#include <pshpack1.h>
typedef struct _QZ_ISO_RECORD
{
    UCHAR Length;
    UCHAR ExtendedLength;
    ULONG Extent;            /* little-endian half of the both-endian pair */
    ULONG ExtentBigEndian;
    ULONG Size;
    ULONG SizeBigEndian;
    UCHAR Date[7];           /* years since 1900, month, day, hour, minute, second, GMT offset */
    UCHAR Flags;             /* 2: directory */
    UCHAR UnitSize;
    UCHAR Gap;
    USHORT Volume;
    USHORT VolumeBigEndian;
    UCHAR NameLength;
    CHAR Name[1];
} QZ_ISO_RECORD, *PQZ_ISO_RECORD;
#include <poppack.h>

typedef struct _QZ_ISO_VOLUME
{
    PDEVICE_OBJECT Target;
    ULONG RootExtent;
    ULONG RootSize;
    ULONG VolumeBlocks;
    ULONG Serial;
    WCHAR Label[32];
    USHORT LabelLength;      /* bytes */
    volatile LONG OpenFiles;
} QZ_ISO_VOLUME, *PQZ_ISO_VOLUME;

/* One per open (nothing is shared: the media cannot change under us) */
typedef struct _QZ_ISO_FCB
{
    FSRTL_COMMON_FCB_HEADER Header;    /* what file system filters expect at FsContext */
    ULONG Extent;
    ULONG Size;
    BOOLEAN Directory;
    BOOLEAN Volume;                    /* the volume itself was opened */
    LARGE_INTEGER Time;
    ULONG DirectoryOffset;             /* the next entry of a directory listing */
    BOOLEAN ListingStarted;
    UNICODE_STRING Pattern;
    WCHAR Name[260];                   /* the full path, for FileNameInformation */
    USHORT NameLength;
} QZ_ISO_FCB, *PQZ_ISO_FCB;

static PDEVICE_OBJECT QzIsoControlDevice, QzIsoDiskControlDevice;   /* CD-ROMs; disks (USB sticks) */

/* ---------------------------------------------------------------------- */
/* Reading the media                                                       */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzIsoReadSectors(PDEVICE_OBJECT Target, ULONG Sector, ULONG Count, PVOID Buffer)
{
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Offset;
    KEVENT Event;
    PIRP Irp;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Offset.QuadPart = (LONGLONG)Sector * QZ_ISO_SECTOR;
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ, Target, Buffer, Count * QZ_ISO_SECTOR, &Offset,
                                       &Event, &IoStatus);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    IoGetNextIrpStackLocation(Irp)->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    return QzIoSynchronousCall(Target, Irp, &Event, &IoStatus);
}

/* Copy [Offset, Offset + Length) of an extent into Buffer */
static NTSTATUS
QzIsoReadExtent(PQZ_ISO_VOLUME Volume, ULONG Extent, ULONG Offset, ULONG Length, PUCHAR Buffer)
{
    PUCHAR Bounce = ExAllocatePoolWithTag(NonPagedPool, QZ_ISO_CHUNK, QZ_ISO_TAG);
    NTSTATUS Status = STATUS_SUCCESS;

    if (!Bounce)
        return STATUS_INSUFFICIENT_RESOURCES;
    while (Length && NT_SUCCESS(Status))
    {
        ULONG First = Offset / QZ_ISO_SECTOR, Skip = Offset % QZ_ISO_SECTOR;
        ULONG Sectors = min((Skip + Length + QZ_ISO_SECTOR - 1) / QZ_ISO_SECTOR, QZ_ISO_CHUNK / QZ_ISO_SECTOR);
        ULONG Bytes = min(Sectors * QZ_ISO_SECTOR - Skip, Length);

        Status = QzIsoReadSectors(Volume->Target, Extent + First, Sectors, Bounce);
        if (NT_SUCCESS(Status))
        {
            RtlCopyMemory(Buffer, Bounce + Skip, Bytes);
            Buffer += Bytes;
            Offset += Bytes;
            Length -= Bytes;
        }
    }
    ExFreePoolWithTag(Bounce, QZ_ISO_TAG);
    return Status;
}

static LARGE_INTEGER
QzIsoTime(const UCHAR Date[7])
{
    TIME_FIELDS Fields;
    LARGE_INTEGER Time;

    Fields.Year = (CSHORT)(1900 + Date[0]);
    Fields.Month = Date[1] ? Date[1] : 1;
    Fields.Day = Date[2] ? Date[2] : 1;
    Fields.Hour = Date[3];
    Fields.Minute = Date[4];
    Fields.Second = Date[5];
    Fields.Milliseconds = 0;
    Fields.Weekday = 0;
    if (!RtlTimeFieldsToTime(&Fields, &Time))
        Time.QuadPart = 0;
    else
        Time.QuadPart -= (LONGLONG)(CHAR)Date[6] * 15 * 60 * 10000000LL;   /* to UTC */
    return Time;
}

/* A record's name as Unicode, without ";1" and a trailing dot; FALSE for "." and ".." */
static BOOLEAN
QzIsoRecordName(PQZ_ISO_RECORD Record, PWCHAR Name, PUSHORT NameBytes)
{
    ULONG Length = Record->NameLength, i;

    if (Length == 1 && (Record->Name[0] == 0 || Record->Name[0] == 1))
        return FALSE;
    for (i = 0; i < Length; i++)
        if (Record->Name[i] == ';')
            break;
    Length = i;
    if (Length > 1 && Record->Name[Length - 1] == '.')
        Length--;
    if (Length > 255)
        Length = 255;
    for (i = 0; i < Length; i++)
        Name[i] = (UCHAR)Record->Name[i];
    *NameBytes = (USHORT)(Length * sizeof(WCHAR));
    return TRUE;
}

/*
 * Walk a directory: Callback sees each named record; return TRUE from it to
 * stop. *Offset is where to start, and where the next record begins after.
 */
typedef BOOLEAN (*QZ_ISO_VISIT)(PQZ_ISO_RECORD Record, PCWSTR Name, USHORT NameBytes, PVOID Context);

static NTSTATUS
QzIsoWalk(PQZ_ISO_VOLUME Volume, ULONG Extent, ULONG Size, PULONG Offset, QZ_ISO_VISIT Visit, PVOID Context)
{
    PUCHAR Sector = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, QZ_ISO_TAG);
    ULONG Loaded = (ULONG)-1;
    WCHAR Name[256];
    USHORT NameBytes;
    NTSTATUS Status = STATUS_NO_MORE_FILES;

    if (!Sector)
        return STATUS_INSUFFICIENT_RESOURCES;
    while (*Offset < Size)
    {
        ULONG Block = *Offset / QZ_ISO_SECTOR, Within = *Offset % QZ_ISO_SECTOR;
        PQZ_ISO_RECORD Record;

        if (Block != Loaded)
        {
            Status = QzIsoReadSectors(Volume->Target, Extent + Block, 1, Sector);
            if (!NT_SUCCESS(Status))
                break;
            Status = STATUS_NO_MORE_FILES;
            Loaded = Block;
        }
        Record = (PQZ_ISO_RECORD)(Sector + Within);
        if (Record->Length == 0 || Within + Record->Length > QZ_ISO_SECTOR)
        {
            /* Records do not cross sectors: the rest of this one is padding */
            *Offset = (Block + 1) * QZ_ISO_SECTOR;
            continue;
        }
        *Offset += Record->Length;
        if (QzIsoRecordName(Record, Name, &NameBytes) && Visit(Record, Name, NameBytes, Context))
        {
            Status = STATUS_SUCCESS;
            break;
        }
    }
    ExFreePoolWithTag(Sector, QZ_ISO_TAG);
    return Status;
}

typedef struct _QZ_ISO_FIND
{
    PCWSTR Name;
    USHORT NameBytes;
    ULONG Extent, Size;
    BOOLEAN Directory;
    LARGE_INTEGER Time;
} QZ_ISO_FIND;

static BOOLEAN
QzIsoMatchName(PQZ_ISO_RECORD Record, PCWSTR Name, USHORT NameBytes, PVOID Context)
{
    QZ_ISO_FIND *Find = Context;
    UNICODE_STRING A, B;

    A.Buffer = (PWSTR)Name;
    A.Length = A.MaximumLength = NameBytes;
    B.Buffer = (PWSTR)Find->Name;
    B.Length = B.MaximumLength = Find->NameBytes;
    if (!RtlEqualUnicodeString(&A, &B, TRUE))
        return FALSE;
    Find->Extent = Record->Extent;
    Find->Size = Record->Size;
    Find->Directory = (Record->Flags & 2) != 0;
    Find->Time = QzIsoTime(Record->Date);
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Mounting                                                                */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzIsoMount(PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_OBJECT Target = Stack->Parameters.MountVolume.DeviceObject, VolumeDevice;
    PVPB Vpb = Stack->Parameters.MountVolume.Vpb;
    PUCHAR Descriptor;
    PQZ_ISO_VOLUME Volume;
    NTSTATUS Status;
    ULONG i;

    Descriptor = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, QZ_ISO_TAG);
    if (!Descriptor)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = QzIsoReadSectors(Target, 16, 1, Descriptor);
    if (NT_SUCCESS(Status) && (Descriptor[0] != 1 || RtlCompareMemory(Descriptor + 1, "CD001", 5) != 5))
        Status = STATUS_UNRECOGNIZED_VOLUME;
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Descriptor, QZ_ISO_TAG);
        return Status == STATUS_UNRECOGNIZED_VOLUME || NT_SUCCESS(Status) ? STATUS_UNRECOGNIZED_VOLUME : Status;
    }

    Status = IoCreateDevice(QzIsoControlDevice->DriverObject, sizeof(QZ_ISO_VOLUME), NULL,
                            Target->DeviceType == FILE_DEVICE_CD_ROM ? FILE_DEVICE_CD_ROM_FILE_SYSTEM
                                                                     : FILE_DEVICE_DISK_FILE_SYSTEM,
                            0, FALSE, &VolumeDevice);
    if (NT_SUCCESS(Status))
    {
        PQZ_ISO_RECORD Root = (PQZ_ISO_RECORD)(Descriptor + 156);
        Volume = VolumeDevice->DeviceExtension;
        RtlZeroMemory(Volume, sizeof(*Volume));
        Volume->Target = Target;
        Volume->RootExtent = Root->Extent;
        Volume->RootSize = Root->Size;
        Volume->VolumeBlocks = *(PULONG)(Descriptor + 80);
        for (i = 0; i < 2048 / 4; i++)
            Volume->Serial = Volume->Serial * 31 + ((PULONG)Descriptor)[i];
        for (i = 0; i < 32 && Descriptor[40 + i]; i++)
            Volume->Label[i] = Descriptor[40 + i];
        while (i && Volume->Label[i - 1] == L' ')
            i--;
        Volume->LabelLength = (USHORT)(i * sizeof(WCHAR));

        VolumeDevice->StackSize = Target->StackSize + 1;
        VolumeDevice->Flags |= DO_BUFFERED_IO;
        VolumeDevice->Flags &= ~DO_DEVICE_INITIALIZING;
        Vpb->DeviceObject = VolumeDevice;
        Vpb->SerialNumber = Volume->Serial;
        Vpb->VolumeLabelLength = min(Volume->LabelLength, sizeof(Vpb->VolumeLabel));
        RtlCopyMemory(Vpb->VolumeLabel, Volume->Label, Vpb->VolumeLabelLength);
        DbgPrint(QZ_KERNEL_NAME ": QzIso: mounted \"%.*ls\" (%lu MB)\n", Volume->LabelLength / 2,
                 Volume->Label, Volume->VolumeBlocks / 512);
    }
    ExFreePoolWithTag(Descriptor, QZ_ISO_TAG);
    return Status;
}

/* ---------------------------------------------------------------------- */
/* Create                                                                  */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzIsoCreate(PQZ_ISO_VOLUME Volume, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = Stack->FileObject, Related = FileObject->RelatedFileObject;
    ULONG Disposition = Stack->Parameters.Create.Options >> 24;
    ULONG Options = Stack->Parameters.Create.Options & 0xFFFFFF;
    UNICODE_STRING Path = FileObject->FileName;
    PQZ_ISO_FCB Fcb, Base = Related ? Related->FsContext : NULL;
    QZ_ISO_FIND Find;
    USHORT Done;
    NTSTATUS Status;

    if (Disposition != FILE_OPEN && Disposition != FILE_OPEN_IF)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (Stack->Parameters.Create.SecurityContext->DesiredAccess &
        (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | DELETE))
        return STATUS_MEDIA_WRITE_PROTECTED;

    Fcb = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Fcb), QZ_ISO_TAG);
    if (!Fcb)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Fcb, sizeof(*Fcb));

    /* Where the walk starts: the root, or a directory opened before */
    if (Base && !Base->Volume)
    {
        if (!Base->Directory)
        {
            ExFreePoolWithTag(Fcb, QZ_ISO_TAG);
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        Find.Extent = Base->Extent;
        Find.Size = Base->Size;
        Find.Directory = TRUE;
        Find.Time = Base->Time;
        RtlCopyMemory(Fcb->Name, Base->Name, Base->NameLength);
        Fcb->NameLength = Base->NameLength;
    }
    else
    {
        Find.Extent = Volume->RootExtent;
        Find.Size = Volume->RootSize;
        Find.Directory = TRUE;
        Find.Time.QuadPart = 0;
        /* Opening the volume itself */
        if (!Path.Length && !Related)
        {
            Fcb->Volume = TRUE;
            goto Opened;
        }
    }

    for (Done = 0; Done < Path.Length; )
    {
        UNICODE_STRING Component;
        ULONG Offset = 0;

        while (Done < Path.Length && Path.Buffer[Done / 2] == L'\\')
            Done += sizeof(WCHAR);
        if (Done >= Path.Length)
            break;
        Component.Buffer = Path.Buffer + Done / 2;
        Component.Length = 0;
        while (Done + Component.Length < Path.Length && Component.Buffer[Component.Length / 2] != L'\\')
            Component.Length += sizeof(WCHAR);
        Done += Component.Length;
        if (!Find.Directory)
        {
            ExFreePoolWithTag(Fcb, QZ_ISO_TAG);
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        Find.Name = Component.Buffer;
        Find.NameBytes = Component.Length;
        Status = QzIsoWalk(Volume, Find.Extent, Find.Size, &Offset, QzIsoMatchName, &Find);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(Fcb, QZ_ISO_TAG);
            if (Status != STATUS_NO_MORE_FILES)
                return Status;
            return Done < Path.Length ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND;
        }
        if (Fcb->NameLength + sizeof(WCHAR) + Component.Length <= sizeof(Fcb->Name))
        {
            Fcb->Name[Fcb->NameLength / 2] = L'\\';
            RtlCopyMemory(Fcb->Name + Fcb->NameLength / 2 + 1, Component.Buffer, Component.Length);
            Fcb->NameLength += sizeof(WCHAR) + Component.Length;
        }
    }

    if ((Options & FILE_DIRECTORY_FILE) && !Find.Directory)
    {
        ExFreePoolWithTag(Fcb, QZ_ISO_TAG);
        return STATUS_NOT_A_DIRECTORY;
    }
    if ((Options & FILE_NON_DIRECTORY_FILE) && Find.Directory)
    {
        ExFreePoolWithTag(Fcb, QZ_ISO_TAG);
        return STATUS_FILE_IS_A_DIRECTORY;
    }
    Fcb->Extent = Find.Extent;
    Fcb->Size = Find.Size;
    Fcb->Directory = Find.Directory;
    Fcb->Time = Find.Time;
    if (!Fcb->NameLength)
    {
        Fcb->Name[0] = L'\\';
        Fcb->NameLength = sizeof(WCHAR);
    }

Opened:
    Fcb->Header.NodeTypeCode = 0x0502;
    Fcb->Header.NodeByteSize = sizeof(*Fcb);
    Fcb->Header.AllocationSize.QuadPart = ((LONGLONG)Fcb->Size + QZ_ISO_SECTOR - 1) & ~(QZ_ISO_SECTOR - 1);
    Fcb->Header.FileSize.QuadPart = Fcb->Size;
    Fcb->Header.ValidDataLength.QuadPart = Fcb->Size;
    FileObject->FsContext = Fcb;
    InterlockedIncrement(&Volume->OpenFiles);
    Irp->IoStatus.Information = FILE_OPENED;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Read, information                                                       */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzIsoRead(PQZ_ISO_VOLUME Volume, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = Stack->FileObject;
    PQZ_ISO_FCB Fcb = FileObject->FsContext;
    LONGLONG Offset = Stack->Parameters.Read.ByteOffset.QuadPart;
    ULONG Length = Stack->Parameters.Read.Length;
    PUCHAR Buffer = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS Status;

    if (!Buffer && Irp->MdlAddress)
        Buffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
    if (Fcb->Volume)
    {
        /* The volume: raw reads, passed to the device as they are */
        IO_STATUS_BLOCK IoStatus;
        LARGE_INTEGER ByteOffset;
        KEVENT Event;
        PIRP Raw;
        ByteOffset.QuadPart = Offset;
        KeInitializeEvent(&Event, NotificationEvent, FALSE);
        Raw = IoBuildSynchronousFsdRequest(IRP_MJ_READ, Volume->Target, Buffer, Length, &ByteOffset, &Event,
                                           &IoStatus);
        if (!Raw)
            return STATUS_INSUFFICIENT_RESOURCES;
        IoGetNextIrpStackLocation(Raw)->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
        Status = QzIoSynchronousCall(Volume->Target, Raw, &Event, &IoStatus);
        Irp->IoStatus.Information = NT_SUCCESS(Status) ? IoStatus.Information : 0;
        return Status;
    }
    if (Fcb->Directory)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (Offset == -2 /* FILE_USE_FILE_POINTER_POSITION */ ||
        (Stack->Parameters.Read.ByteOffset.LowPart == FILE_USE_FILE_POINTER_POSITION &&
         Stack->Parameters.Read.ByteOffset.HighPart == -1))
        Offset = FileObject->CurrentByteOffset.QuadPart;
    if (Offset >= Fcb->Size)
    {
        Irp->IoStatus.Information = 0;
        return Length ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    }
    if (Offset + Length > Fcb->Size)
        Length = (ULONG)(Fcb->Size - Offset);
    Status = QzIsoReadExtent(Volume, Fcb->Extent, (ULONG)Offset, Length, Buffer);
    if (NT_SUCCESS(Status))
        Irp->IoStatus.Information = Length;   /* the I/O manager moves the file position */
    return Status;
}

static ULONG
QzIsoAttributes(PQZ_ISO_FCB Fcb)
{
    return FILE_ATTRIBUTE_READONLY | (Fcb->Directory || Fcb->Volume ? FILE_ATTRIBUTE_DIRECTORY : 0);
}

static NTSTATUS
QzIsoQueryInformation(PQZ_ISO_VOLUME Volume, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = Stack->FileObject;
    PQZ_ISO_FCB Fcb = FileObject->FsContext;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG Length = Stack->Parameters.QueryFile.Length, Needed;

    UNREFERENCED_PARAMETER(Volume);
    switch (Stack->Parameters.QueryFile.FileInformationClass)
    {
        case FileBasicInformation:
        {
            PFILE_BASIC_INFORMATION Info = Buffer;
            Needed = sizeof(*Info);
            if (Length < Needed)
                return STATUS_INFO_LENGTH_MISMATCH;
            Info->CreationTime = Info->LastAccessTime = Info->LastWriteTime = Info->ChangeTime = Fcb->Time;
            Info->FileAttributes = QzIsoAttributes(Fcb);
            break;
        }
        case FileStandardInformation:
        {
            PFILE_STANDARD_INFORMATION Info = Buffer;
            Needed = sizeof(*Info);
            if (Length < Needed)
                return STATUS_INFO_LENGTH_MISMATCH;
            Info->AllocationSize = Fcb->Header.AllocationSize;
            Info->EndOfFile = Fcb->Header.FileSize;
            Info->NumberOfLinks = 1;
            Info->DeletePending = FALSE;
            Info->Directory = Fcb->Directory || Fcb->Volume;
            break;
        }
        case FilePositionInformation:
        {
            PFILE_POSITION_INFORMATION Info = Buffer;
            Needed = sizeof(*Info);
            if (Length < Needed)
                return STATUS_INFO_LENGTH_MISMATCH;
            Info->CurrentByteOffset = FileObject->CurrentByteOffset;
            break;
        }
        case FileInternalInformation:
        {
            PFILE_INTERNAL_INFORMATION Info = Buffer;
            Needed = sizeof(*Info);
            if (Length < Needed)
                return STATUS_INFO_LENGTH_MISMATCH;
            Info->IndexNumber.QuadPart = Fcb->Extent;
            break;
        }
        case FileEaInformation:
        {
            PFILE_EA_INFORMATION Info = Buffer;
            Needed = sizeof(*Info);
            if (Length < Needed)
                return STATUS_INFO_LENGTH_MISMATCH;
            Info->EaSize = 0;
            break;
        }
        case FileAttributeTagInformation:
        {
            PFILE_ATTRIBUTE_TAG_INFORMATION Info = Buffer;
            Needed = sizeof(*Info);
            if (Length < Needed)
                return STATUS_INFO_LENGTH_MISMATCH;
            Info->FileAttributes = QzIsoAttributes(Fcb);
            Info->ReparseTag = 0;
            break;
        }
        case FileNetworkOpenInformation:
        {
            PFILE_NETWORK_OPEN_INFORMATION Info = Buffer;
            Needed = sizeof(*Info);
            if (Length < Needed)
                return STATUS_INFO_LENGTH_MISMATCH;
            Info->CreationTime = Info->LastAccessTime = Info->LastWriteTime = Info->ChangeTime = Fcb->Time;
            Info->AllocationSize = Fcb->Header.AllocationSize;
            Info->EndOfFile = Fcb->Header.FileSize;
            Info->FileAttributes = QzIsoAttributes(Fcb);
            break;
        }
        case FileNameInformation:
        {
            PFILE_NAME_INFORMATION Info = Buffer;
            ULONG Copy;
            if (Length < FIELD_OFFSET(FILE_NAME_INFORMATION, FileName))
                return STATUS_INFO_LENGTH_MISMATCH;
            Info->FileNameLength = Fcb->NameLength;
            Copy = min(Fcb->NameLength, Length - FIELD_OFFSET(FILE_NAME_INFORMATION, FileName));
            RtlCopyMemory(Info->FileName, Fcb->Name, Copy);
            Irp->IoStatus.Information = FIELD_OFFSET(FILE_NAME_INFORMATION, FileName) + Copy;
            return Copy < Fcb->NameLength ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
        }
        case FileAlignmentInformation:
        {
            PFILE_ALIGNMENT_INFORMATION Info = Buffer;
            Needed = sizeof(*Info);
            if (Length < Needed)
                return STATUS_INFO_LENGTH_MISMATCH;
            Info->AlignmentRequirement = FILE_BYTE_ALIGNMENT;
            break;
        }
        default:
            return STATUS_INVALID_PARAMETER;
    }
    Irp->IoStatus.Information = Needed;
    return STATUS_SUCCESS;
}

static NTSTATUS
QzIsoSetInformation(PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = Stack->FileObject;

    switch (Stack->Parameters.SetFile.FileInformationClass)
    {
        case FilePositionInformation:
            FileObject->CurrentByteOffset =
                ((PFILE_POSITION_INFORMATION)Irp->AssociatedIrp.SystemBuffer)->CurrentByteOffset;
            return STATUS_SUCCESS;
        case FileBasicInformation:
            return STATUS_SUCCESS;   /* nothing to keep on read-only media */
        default:
            return STATUS_MEDIA_WRITE_PROTECTED;
    }
}

static NTSTATUS
QzIsoQueryVolume(PQZ_ISO_VOLUME Volume, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG Length = Stack->Parameters.QueryVolume.Length, Needed, Copy;

    switch (Stack->Parameters.QueryVolume.FsInformationClass)
    {
        case FileFsVolumeInformation:
        {
            PFILE_FS_VOLUME_INFORMATION Info = Buffer;
            if (Length < FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel))
                return STATUS_INFO_LENGTH_MISMATCH;
            Info->VolumeCreationTime.QuadPart = 0;
            Info->VolumeSerialNumber = Volume->Serial;
            Info->VolumeLabelLength = Volume->LabelLength;
            Info->SupportsObjects = FALSE;
            Copy = min(Volume->LabelLength, Length - FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel));
            RtlCopyMemory(Info->VolumeLabel, Volume->Label, Copy);
            Irp->IoStatus.Information = FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel) + Copy;
            return Copy < Volume->LabelLength ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
        }
        case FileFsSizeInformation:
        {
            PFILE_FS_SIZE_INFORMATION Info = Buffer;
            Needed = sizeof(*Info);
            if (Length < Needed)
                return STATUS_INFO_LENGTH_MISMATCH;
            Info->TotalAllocationUnits.QuadPart = Volume->VolumeBlocks;
            Info->AvailableAllocationUnits.QuadPart = 0;
            Info->SectorsPerAllocationUnit = 1;
            Info->BytesPerSector = QZ_ISO_SECTOR;
            break;
        }
        case FileFsFullSizeInformation:
        {
            PFILE_FS_FULL_SIZE_INFORMATION Info = Buffer;
            Needed = sizeof(*Info);
            if (Length < Needed)
                return STATUS_INFO_LENGTH_MISMATCH;
            Info->TotalAllocationUnits.QuadPart = Volume->VolumeBlocks;
            Info->CallerAvailableAllocationUnits.QuadPart = 0;
            Info->ActualAvailableAllocationUnits.QuadPart = 0;
            Info->SectorsPerAllocationUnit = 1;
            Info->BytesPerSector = QZ_ISO_SECTOR;
            break;
        }
        case FileFsDeviceInformation:
        {
            PFILE_FS_DEVICE_INFORMATION Info = Buffer;
            Needed = sizeof(*Info);
            if (Length < Needed)
                return STATUS_INFO_LENGTH_MISMATCH;
            Info->DeviceType = FILE_DEVICE_CD_ROM;
            Info->Characteristics = FILE_READ_ONLY_DEVICE | FILE_REMOVABLE_MEDIA;
            break;
        }
        case FileFsAttributeInformation:
        {
            static const WCHAR Name[] = L"CDFS";
            PFILE_FS_ATTRIBUTE_INFORMATION Info = Buffer;
            Needed = FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName) + sizeof(Name) - sizeof(WCHAR);
            if (Length < Needed)
                return STATUS_BUFFER_OVERFLOW;
            Info->FileSystemAttributes = FILE_CASE_PRESERVED_NAMES | FILE_READ_ONLY_VOLUME;
            Info->MaximumComponentNameLength = 255;
            Info->FileSystemNameLength = sizeof(Name) - sizeof(WCHAR);
            RtlCopyMemory(Info->FileSystemName, Name, sizeof(Name) - sizeof(WCHAR));
            break;
        }
        default:
            return STATUS_INVALID_PARAMETER;
    }
    Irp->IoStatus.Information = Needed;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Directory listings                                                      */
/* ---------------------------------------------------------------------- */

/* "*" and "?" as Windows matches them (with the DOS forms of both) */
static BOOLEAN
QzIsoWildcardMatch(PCWSTR Pattern, ULONG PatternLength, PCWSTR Name, ULONG NameLength)
{
    while (PatternLength)
    {
        WCHAR p = *Pattern;
        if (p == L'*' || p == L'<')
        {
            ULONG Skip;
            if (PatternLength == 1)
                return TRUE;
            for (Skip = 0; Skip <= NameLength; Skip++)
                if (QzIsoWildcardMatch(Pattern + 1, PatternLength - 1, Name + Skip, NameLength - Skip))
                    return TRUE;
            return FALSE;
        }
        if (p == L'"' && (!NameLength || *Name != L'.'))
        {
            Pattern++;
            PatternLength--;
            continue;   /* DOS_DOT matches a dot or the end */
        }
        if (!NameLength)
            return (p == L'>' || p == L'"') ? QzIsoWildcardMatch(Pattern + 1, PatternLength - 1, Name, 0) : FALSE;
        if (p != L'?' && p != L'>' && p != L'"' && RtlUpcaseUnicodeChar(p) != RtlUpcaseUnicodeChar(*Name))
            return FALSE;
        Pattern++;
        PatternLength--;
        Name++;
        NameLength--;
    }
    return NameLength == 0;
}

typedef struct _QZ_ISO_LIST
{
    PQZ_ISO_FCB Fcb;
    FILE_INFORMATION_CLASS Class;
    PUCHAR Buffer;
    ULONG Length;
    ULONG Used;
    PULONG LastNext;
    BOOLEAN Single;
    BOOLEAN Full;            /* the next entry did not fit */
    ULONG Entries;
    ULONG RecordStart;       /* offset of the record being visited */
} QZ_ISO_LIST;

static BOOLEAN
QzIsoListVisit(PQZ_ISO_RECORD Record, PCWSTR Name, USHORT NameBytes, PVOID Context)
{
    QZ_ISO_LIST *List = Context;
    ULONG Fixed, Size;
    PUCHAR Entry;
    LARGE_INTEGER Time = QzIsoTime(Record->Date), FileSize, Allocation;
    ULONG Attributes = FILE_ATTRIBUTE_READONLY | ((Record->Flags & 2) ? FILE_ATTRIBUTE_DIRECTORY : 0);
    PWCHAR NameField;

    if (List->Fcb->Pattern.Length &&
        !QzIsoWildcardMatch(List->Fcb->Pattern.Buffer, List->Fcb->Pattern.Length / 2, Name, NameBytes / 2))
        return FALSE;

    switch (List->Class)
    {
        case FileDirectoryInformation:     Fixed = FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName); break;
        case FileFullDirectoryInformation: Fixed = FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName); break;
        case FileBothDirectoryInformation: Fixed = FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName); break;
        case FileIdBothDirectoryInformation: Fixed = FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileName); break;
        case FileIdFullDirectoryInformation: Fixed = FIELD_OFFSET(FILE_ID_FULL_DIR_INFORMATION, FileName); break;
        case FileNamesInformation:         Fixed = FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName); break;
        default:                           return TRUE;
    }
    Size = (Fixed + NameBytes + 7) & ~7UL;
    if (List->Used + Fixed + NameBytes > List->Length)
    {
        List->Full = TRUE;
        return TRUE;   /* stop: this one goes in the next call */
    }
    Entry = List->Buffer + List->Used;
    RtlZeroMemory(Entry, Fixed);
    FileSize.QuadPart = Record->Size;
    Allocation.QuadPart = ((LONGLONG)Record->Size + QZ_ISO_SECTOR - 1) & ~(QZ_ISO_SECTOR - 1);

    if (List->Class == FileNamesInformation)
    {
        PFILE_NAMES_INFORMATION Info = (PVOID)Entry;
        Info->FileNameLength = NameBytes;
        NameField = Info->FileName;
    }
    else
    {
        /* The common prefix of the directory classes */
        PFILE_DIRECTORY_INFORMATION Info = (PVOID)Entry;
        Info->CreationTime = Info->LastAccessTime = Info->LastWriteTime = Info->ChangeTime = Time;
        Info->EndOfFile = FileSize;
        Info->AllocationSize = Allocation;
        Info->FileAttributes = Attributes;
        Info->FileNameLength = NameBytes;
        NameField = (PWCHAR)(Entry + Fixed);
        if (List->Class == FileIdBothDirectoryInformation)
            ((PFILE_ID_BOTH_DIR_INFORMATION)Entry)->FileId.QuadPart = Record->Extent;
        if (List->Class == FileIdFullDirectoryInformation)
            ((PFILE_ID_FULL_DIR_INFORMATION)Entry)->FileId.QuadPart = Record->Extent;
    }
    RtlCopyMemory(NameField, Name, NameBytes);
    if (List->LastNext)
        *List->LastNext = (ULONG)(Entry - (PUCHAR)List->LastNext);
    List->LastNext = (PULONG)Entry;   /* NextEntryOffset is the first field of every class */
    List->Used += Size;
    List->Entries++;
    return List->Single;
}

static NTSTATUS
QzIsoQueryDirectory(PQZ_ISO_VOLUME Volume, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PQZ_ISO_FCB Fcb = Stack->FileObject->FsContext;
    PUNICODE_STRING Pattern = Stack->Parameters.QueryDirectory.FileName;
    QZ_ISO_LIST List;
    ULONG Offset;
    BOOLEAN First = FALSE;
    NTSTATUS Status;

    if (!Fcb->Directory)
        return STATUS_INVALID_PARAMETER;
    if (Stack->Flags & SL_RESTART_SCAN || !Fcb->ListingStarted)
    {
        First = TRUE;
        Fcb->DirectoryOffset = 0;
        if (Fcb->Pattern.Buffer)
            ExFreePoolWithTag(Fcb->Pattern.Buffer, QZ_ISO_TAG);
        RtlZeroMemory(&Fcb->Pattern, sizeof(Fcb->Pattern));
        if (Pattern && Pattern->Length &&
            !(Pattern->Length == sizeof(WCHAR) && Pattern->Buffer[0] == L'*'))
        {
            Fcb->Pattern.Buffer = ExAllocatePoolWithTag(NonPagedPool, Pattern->Length, QZ_ISO_TAG);
            if (Fcb->Pattern.Buffer)
            {
                RtlCopyMemory(Fcb->Pattern.Buffer, Pattern->Buffer, Pattern->Length);
                Fcb->Pattern.Length = Fcb->Pattern.MaximumLength = Pattern->Length;
            }
        }
        Fcb->ListingStarted = TRUE;
    }

    RtlZeroMemory(&List, sizeof(List));
    List.Fcb = Fcb;
    List.Class = Stack->Parameters.QueryDirectory.FileInformationClass;
    List.Buffer = Irp->AssociatedIrp.SystemBuffer;
    if (!List.Buffer && Irp->MdlAddress)
        List.Buffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
    if (!List.Buffer)
        List.Buffer = Irp->UserBuffer;
    List.Length = Stack->Parameters.QueryDirectory.Length;

    /* One entry per step; an entry that does not fit is retried next time */
    Offset = Fcb->DirectoryOffset;
    Status = STATUS_SUCCESS;
    for (;;)
    {
        ULONG Before = Offset;
        ULONG Had = List.Entries;
        List.Single = TRUE;
        Status = QzIsoWalk(Volume, Fcb->Extent, Fcb->Size, &Offset, QzIsoListVisit, &List);
        if (List.Full)
        {
            Offset = Before;
            break;
        }
        if (!NT_SUCCESS(Status) || List.Entries == Had)
            break;
        if (Stack->Flags & SL_RETURN_SINGLE_ENTRY)
            break;
    }

    Fcb->DirectoryOffset = Offset;
    Irp->IoStatus.Information = List.Used;
    if (List.Entries)
        return STATUS_SUCCESS;
    if (List.Full)
        return STATUS_BUFFER_OVERFLOW;
    if (!NT_SUCCESS(Status) && Status != STATUS_NO_MORE_FILES)
        return Status;
    return First ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;
}

/* ---------------------------------------------------------------------- */
/* Dispatch                                                                */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzIsoComplete(PIRP Irp, NTSTATUS Status)
{
    Irp->IoStatus.Status = Status;
    if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW)
        Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_DISK_INCREMENT);
    return Status;
}

static NTSTATUS NTAPI
QzIsoDispatch(PDEVICE_OBJECT Device, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PQZ_ISO_VOLUME Volume = Device->DeviceExtension;
    PFILE_OBJECT FileObject = Stack->FileObject;

    if (Device == QzIsoControlDevice || Device == QzIsoDiskControlDevice)
    {
        if (Stack->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL && Stack->MinorFunction == IRP_MN_MOUNT_VOLUME)
            return QzIsoComplete(Irp, QzIsoMount(Irp));
        if (Stack->MajorFunction == IRP_MJ_CREATE || Stack->MajorFunction == IRP_MJ_CLOSE ||
            Stack->MajorFunction == IRP_MJ_CLEANUP)
        {
            Irp->IoStatus.Information = FILE_OPENED;
            return QzIsoComplete(Irp, STATUS_SUCCESS);
        }
        return QzIsoComplete(Irp, STATUS_INVALID_DEVICE_REQUEST);
    }

    switch (Stack->MajorFunction)
    {
        case IRP_MJ_CREATE:
            return QzIsoComplete(Irp, QzIsoCreate(Volume, Irp));
        case IRP_MJ_READ:
            return QzIsoComplete(Irp, QzIsoRead(Volume, Irp));
        case IRP_MJ_WRITE:
            return QzIsoComplete(Irp, STATUS_MEDIA_WRITE_PROTECTED);
        case IRP_MJ_QUERY_INFORMATION:
            return QzIsoComplete(Irp, QzIsoQueryInformation(Volume, Irp));
        case IRP_MJ_SET_INFORMATION:
            return QzIsoComplete(Irp, QzIsoSetInformation(Irp));
        case IRP_MJ_QUERY_VOLUME_INFORMATION:
            return QzIsoComplete(Irp, QzIsoQueryVolume(Volume, Irp));
        case IRP_MJ_DIRECTORY_CONTROL:
            if (Stack->MinorFunction == IRP_MN_QUERY_DIRECTORY)
                return QzIsoComplete(Irp, QzIsoQueryDirectory(Volume, Irp));
            return QzIsoComplete(Irp, STATUS_NOT_SUPPORTED);   /* change notification: later */
        case IRP_MJ_FILE_SYSTEM_CONTROL:
            if (Stack->MinorFunction == IRP_MN_USER_FS_REQUEST &&
                Stack->Parameters.FileSystemControl.FsControlCode == FSCTL_IS_VOLUME_MOUNTED)
                return QzIsoComplete(Irp, STATUS_SUCCESS);
            return QzIsoComplete(Irp, STATUS_INVALID_DEVICE_REQUEST);
        case IRP_MJ_CLEANUP:
        case IRP_MJ_FLUSH_BUFFERS:
        case IRP_MJ_LOCK_CONTROL:
            return QzIsoComplete(Irp, STATUS_SUCCESS);
        case IRP_MJ_CLOSE:
            if (FileObject && FileObject->FsContext)
            {
                PQZ_ISO_FCB Fcb = FileObject->FsContext;
                if (Fcb->Pattern.Buffer)
                    ExFreePoolWithTag(Fcb->Pattern.Buffer, QZ_ISO_TAG);
                ExFreePoolWithTag(Fcb, QZ_ISO_TAG);
                FileObject->FsContext = NULL;
                InterlockedDecrement(&Volume->OpenFiles);
            }
            return QzIsoComplete(Irp, STATUS_SUCCESS);
        default:
            return QzIsoComplete(Irp, STATUS_INVALID_DEVICE_REQUEST);
    }
}

static NTSTATUS NTAPI
QzIsoEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\QzIso");
    ULONG i;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(RegistryPath);
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        Driver->MajorFunction[i] = QzIsoDispatch;
    Status = IoCreateDevice(Driver, 0, &Name, FILE_DEVICE_CD_ROM_FILE_SYSTEM, 0, FALSE, &QzIsoControlDevice);
    if (!NT_SUCCESS(Status))
        return Status;
    IoRegisterFileSystem(QzIsoControlDevice);

    /* The same media written to a disk (the hybrid image on a USB stick) */
    Status = IoCreateDevice(Driver, 0, NULL, FILE_DEVICE_DISK_FILE_SYSTEM, 0, FALSE, &QzIsoDiskControlDevice);
    if (NT_SUCCESS(Status))
        IoRegisterFileSystem(QzIsoDiskControlDevice);
    return STATUS_SUCCESS;
}

VOID
QzIsoInit(VOID)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\FileSystem\\QzIso");
    NTSTATUS Status = IoCreateDriver(&Name, QzIsoEntry);
    if (!NT_SUCCESS(Status))
        DbgPrint(QZ_KERNEL_NAME ": QzIso: not registered (0x%08lx)\n", Status);
}

/* ---------------------------------------------------------------------- */
/* The shell's "dir" and "type": any file system, through the Zw calls     */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzIsoShellOpen(PCSTR Path, BOOLEAN Directory, PHANDLE Handle)
{
    WCHAR Buffer[260];
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK IoStatus;
    ULONG i;

    for (i = 0; Path[i] && i < RTL_NUMBER_OF(Buffer) - 1; i++)
        Buffer[i] = (UCHAR)Path[i];
    Buffer[i] = UNICODE_NULL;
    RtlInitUnicodeString(&Name, Buffer);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    return ZwOpenFile(Handle, (Directory ? FILE_LIST_DIRECTORY : FILE_READ_DATA) | SYNCHRONIZE, &Attributes,
                      &IoStatus, FILE_SHARE_READ,
                      FILE_SYNCHRONOUS_IO_NONALERT | (Directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE));
}

VOID
QzIoShellDir(PCSTR Path)
{
    HANDLE Handle;
    IO_STATUS_BLOCK IoStatus;
    PUCHAR Buffer;
    ULONG Files = 0, Directories = 0;
    BOOLEAN Restart = TRUE;
    NTSTATUS Status = QzIsoShellOpen(Path, TRUE, &Handle);

    if (!NT_SUCCESS(Status))
    {
        QzPrint("  dir: cannot open %s (0x%08lx)\n", Path, Status);
        return;
    }
    Buffer = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, QZ_ISO_TAG);
    while (Buffer)
    {
        PFILE_BOTH_DIR_INFORMATION Info = (PVOID)Buffer;
        Status = ZwQueryDirectoryFile(Handle, NULL, NULL, NULL, &IoStatus, Buffer, PAGE_SIZE,
                                      FileBothDirectoryInformation, FALSE, NULL, Restart);
        Restart = FALSE;
        if (!NT_SUCCESS(Status))
            break;
        for (;;)
        {
            UNICODE_STRING Name;
            Name.Buffer = Info->FileName;
            Name.Length = Name.MaximumLength = (USHORT)Info->FileNameLength;
            if (Info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                QzPrint("    %-12s %wZ\n", "<DIR>", &Name);
                Directories++;
            }
            else
            {
                QzPrint("    %12lu %wZ\n", Info->EndOfFile.LowPart, &Name);
                Files++;
            }
            if (!Info->NextEntryOffset)
                break;
            Info = (PVOID)((PUCHAR)Info + Info->NextEntryOffset);
        }
    }
    QzPrint("  %lu file(s), %lu dir(s) (0x%08lx)\n", Files, Directories, Status);
    if (Buffer)
        ExFreePoolWithTag(Buffer, QZ_ISO_TAG);
    ZwClose(Handle);
}

/* The size, a checksum of every byte, and the first line */
VOID
QzIoShellType(PCSTR Path)
{
    HANDLE Handle;
    IO_STATUS_BLOCK IoStatus;
    FILE_STANDARD_INFORMATION Standard;
    PUCHAR Buffer;
    ULONG Sum = 0, Total = 0, i;
    CHAR First[81];
    NTSTATUS Status = QzIsoShellOpen(Path, FALSE, &Handle);

    if (!NT_SUCCESS(Status))
    {
        QzPrint("  type: cannot open %s (0x%08lx)\n", Path, Status);
        return;
    }
    ZwQueryInformationFile(Handle, &IoStatus, &Standard, sizeof(Standard), FileStandardInformation);
    Buffer = ExAllocatePoolWithTag(NonPagedPool, 0x10000, QZ_ISO_TAG);
    First[0] = 0;
    while (Buffer)
    {
        Status = ZwReadFile(Handle, NULL, NULL, NULL, &IoStatus, Buffer, 0x10000, NULL, NULL);
        if (!NT_SUCCESS(Status) || !IoStatus.Information)
            break;
        if (!Total)
        {
            for (i = 0; i < 80 && i < IoStatus.Information && Buffer[i] != '\n' && Buffer[i] != '\r'; i++)
                First[i] = (Buffer[i] >= 32 && Buffer[i] < 127) ? Buffer[i] : '.';
            First[i] = 0;
        }
        for (i = 0; i < IoStatus.Information; i++)
            Sum = (Sum << 5 | Sum >> 27) ^ Buffer[i];
        Total += (ULONG)IoStatus.Information;
    }
    QzPrint("  %s: %lu bytes (size %lu), checksum %08lx\n  first line: %s\n", Path, Total,
            Standard.EndOfFile.LowPart, Sum, First);
    if (Buffer)
        ExFreePoolWithTag(Buffer, QZ_ISO_TAG);
    ZwClose(Handle);
}
