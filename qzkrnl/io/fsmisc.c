/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     What file systems ask of the I/O manager, Mm, Ex and Se besides
 *              FsRtl and the cache: per-thread top-level IRP and
 *              device-to-verify, the VPB lock, hard errors, local time,
 *              resource owner pointers, section checks
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qzio.h"
#include "qzps.h"

static KSPIN_LOCK QzVpbLock;

VOID
NTAPI
IoAcquireVpbSpinLock(OUT PKIRQL Irql)
{
    KeAcquireSpinLock(&QzVpbLock, Irql);
}

VOID
NTAPI
IoReleaseVpbSpinLock(IN KIRQL Irql)
{
    KeReleaseSpinLock(&QzVpbLock, Irql);
}

PIRP
NTAPI
IoGetTopLevelIrp(VOID)
{
    return QzCurrentThread->TopLevelIrp;
}

VOID
NTAPI
IoSetTopLevelIrp(IN PIRP Irp)
{
    QzCurrentThread->TopLevelIrp = Irp;
}

PDEVICE_OBJECT
NTAPI
IoGetDeviceToVerify(IN PETHREAD Thread)
{
    UNREFERENCED_PARAMETER(Thread);
    return QzCurrentThread->DeviceToVerify;
}

VOID
NTAPI
IoSetDeviceToVerify(IN PETHREAD Thread, IN PDEVICE_OBJECT DeviceObject)
{
    UNREFERENCED_PARAMETER(Thread);
    QzCurrentThread->DeviceToVerify = DeviceObject;
}

BOOLEAN
NTAPI
IoIsSystemThread(IN PETHREAD Thread)
{
    return PsIsSystemThread(Thread);
}

/* No one to show a hard error to: it is logged, and the request fails */
VOID
NTAPI
IoRaiseHardError(IN PIRP Irp, IN PVPB Vpb OPTIONAL, IN PDEVICE_OBJECT RealDeviceObject)
{
    UNREFERENCED_PARAMETER(Vpb);
    DbgPrint(QZ_KERNEL_NAME ": hard error 0x%08lx on device %p\n", Irp->IoStatus.Status, RealDeviceObject);
    IoCompleteRequest(Irp, IO_DISK_INCREMENT);
}

BOOLEAN
NTAPI
IoRaiseInformationalHardError(IN NTSTATUS ErrorStatus, IN PUNICODE_STRING String OPTIONAL,
                              IN PKTHREAD Thread OPTIONAL)
{
    UNREFERENCED_PARAMETER(Thread);
    DbgPrint(QZ_KERNEL_NAME ": informational hard error 0x%08lx %wZ\n", ErrorStatus, String);
    return FALSE;
}

/* IRP_MJ_QUERY_INFORMATION to the file's own driver, waiting for it */
NTSTATUS
NTAPI
IoQueryFileInformation(IN PFILE_OBJECT FileObject, IN FILE_INFORMATION_CLASS FileInformationClass, IN ULONG Length,
                       OUT PVOID FileInformation, OUT PULONG ReturnedLength)
{
    PDEVICE_OBJECT Device = IoGetRelatedDeviceObject(FileObject);
    PIO_STACK_LOCATION Stack;
    IO_STATUS_BLOCK Io;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp = IoAllocateIrp(Device->StackSize, FALSE);

    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    ObReferenceObject(FileObject);
    Irp->AssociatedIrp.SystemBuffer = FileInformation;
    Irp->UserEvent = &Event;
    Irp->UserIosb = &Io;
    Irp->Flags = IRP_SYNCHRONOUS_API;
    Irp->RequestorMode = KernelMode;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->Tail.Overlay.OriginalFileObject = FileObject;
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = IRP_MJ_QUERY_INFORMATION;
    Stack->FileObject = FileObject;
    Stack->Parameters.QueryFile.Length = Length;
    Stack->Parameters.QueryFile.FileInformationClass = FileInformationClass;
    Status = IoCallDriver(Device, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Io.Status;
    }
    if (ReturnedLength)
        *ReturnedLength = (ULONG)Io.Information;
    return Status;
}

/* ---------------------------------------------------------------------- */
/* Ex                                                                      */
/* ---------------------------------------------------------------------- */

/* The time zone is UTC until there is one to set */
VOID
NTAPI
ExLocalTimeToSystemTime(IN PLARGE_INTEGER LocalTime, OUT PLARGE_INTEGER SystemTime)
{
    *SystemTime = *LocalTime;
}

VOID
NTAPI
ExSystemTimeToLocalTime(IN PLARGE_INTEGER SystemTime, OUT PLARGE_INTEGER LocalTime)
{
    *LocalTime = *SystemTime;
}

/* ---------------------------------------------------------------------- */
/* Mm: the file systems' questions about sections of a file                */
/* ---------------------------------------------------------------------- */

BOOLEAN QzMmIsImageInUse(PSECTION_OBJECT_POINTERS Sop);          /* mm/section.c */
BOOLEAN QzMmIsDataMappedBeyond(PSECTION_OBJECT_POINTERS Sop, LONGLONG Size);
BOOLEAN QzMmPurgeImage(PSECTION_OBJECT_POINTERS Sop);

BOOLEAN
NTAPI
MmFlushImageSection(IN PSECTION_OBJECT_POINTERS SectionPointer, IN MMFLUSH_TYPE FlushType)
{
    UNREFERENCED_PARAMETER(FlushType);
    return QzMmPurgeImage(SectionPointer);  /* drops the shared image unless it is mapped */
}

BOOLEAN
NTAPI
MmCanFileBeTruncated(IN PSECTION_OBJECT_POINTERS SectionPointer, IN PLARGE_INTEGER NewFileSize)
{
    return !QzMmIsImageInUse(SectionPointer) &&
           !QzMmIsDataMappedBeyond(SectionPointer, NewFileSize ? NewFileSize->QuadPart : 0);
}

MM_SYSTEMSIZE
NTAPI
MmQuerySystemSize(VOID)
{
    ULONG Total, Free;
    QzMmGetStats(&Total, &Free);
    if (Total < 4096)                       /* under 16 MB */
        return MmSmallSystem;
    return Total < 16384 ? MmMediumSystem : MmLargeSystem;
}

/* ---------------------------------------------------------------------- */
/* Se                                                                      */
/* ---------------------------------------------------------------------- */

BOOLEAN
NTAPI
SePrivilegeCheck(IN OUT PPRIVILEGE_SET RequiredPrivileges, IN PSECURITY_SUBJECT_CONTEXT SubjectContext,
                 IN KPROCESSOR_MODE AccessMode)
{
    ULONG i;
    UNREFERENCED_PARAMETER(SubjectContext);
    UNREFERENCED_PARAMETER(AccessMode);
    for (i = 0; i < RequiredPrivileges->PrivilegeCount; i++)
        RequiredPrivileges->Privilege[i].Attributes |= SE_PRIVILEGE_USED_FOR_ACCESS;
    return TRUE;                            /* LocalSystem holds them all */
}

/* Filtering the one token gives the one token */
NTSTATUS
NTAPI
SeFilterToken(IN PACCESS_TOKEN ExistingToken, IN ULONG Flags, IN PTOKEN_GROUPS SidsToDisable OPTIONAL,
              IN PTOKEN_PRIVILEGES PrivilegesToDelete OPTIONAL, IN PTOKEN_GROUPS RestrictedSids OPTIONAL,
              OUT PACCESS_TOKEN *FilteredToken)
{
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(SidsToDisable);
    UNREFERENCED_PARAMETER(PrivilegesToDelete);
    UNREFERENCED_PARAMETER(RestrictedSids);
    ObReferenceObject(ExistingToken);
    *FilteredToken = ExistingToken;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Rtl                                                                     */
/* ---------------------------------------------------------------------- */

LARGE_INTEGER
NTAPI
RtlExtendedLargeIntegerDivide(IN LARGE_INTEGER Dividend, IN ULONG Divisor, OUT PULONG Remainder OPTIONAL)
{
    LARGE_INTEGER Quotient;
    Quotient.QuadPart = (LONGLONG)((ULONGLONG)Dividend.QuadPart / Divisor);
    if (Remainder)
        *Remainder = (ULONG)((ULONGLONG)Dividend.QuadPart % Divisor);
    return Quotient;
}

BOOLEAN
NTAPI
RtlAreBitsClear(IN PRTL_BITMAP BitMapHeader, IN ULONG StartingIndex, IN ULONG Length)
{
    ULONG i;
    if (StartingIndex + Length > BitMapHeader->SizeOfBitMap || StartingIndex + Length < StartingIndex)
        return FALSE;
    for (i = StartingIndex; i < StartingIndex + Length; i++)
        if (BitMapHeader->Buffer[i / 32] & (1UL << (i % 32)))
            return FALSE;
    return TRUE;
}

BOOLEAN
NTAPI
RtlAreBitsSet(IN PRTL_BITMAP BitMapHeader, IN ULONG StartingIndex, IN ULONG Length)
{
    ULONG i;
    if (!Length || StartingIndex + Length > BitMapHeader->SizeOfBitMap || StartingIndex + Length < StartingIndex)
        return FALSE;
    for (i = StartingIndex; i < StartingIndex + Length; i++)
        if (!(BitMapHeader->Buffer[i / 32] & (1UL << (i % 32))))
            return FALSE;
    return TRUE;
}

ULONG
NTAPI
RtlFindLongestRunClear(IN PRTL_BITMAP BitMapHeader, OUT PULONG StartingIndex)
{
    ULONG i, Run = 0, RunStart = 0, Best = 0, BestStart = 0;
    for (i = 0; i < BitMapHeader->SizeOfBitMap; i++)
    {
        if (BitMapHeader->Buffer[i / 32] & (1UL << (i % 32)))
        {
            Run = 0;
            continue;
        }
        if (!Run)
            RunStart = i;
        if (++Run > Best)
        {
            Best = Run;
            BestStart = RunStart;
        }
    }
    *StartingIndex = BestStart;
    return Best;
}

ULONG
NTAPI
RtlFindLongestRunSet(IN PRTL_BITMAP BitMapHeader, OUT PULONG StartingIndex)
{
    ULONG i, Run = 0, RunStart = 0, Best = 0, BestStart = 0;
    for (i = 0; i < BitMapHeader->SizeOfBitMap; i++)
    {
        if (!(BitMapHeader->Buffer[i / 32] & (1UL << (i % 32))))
        {
            Run = 0;
            continue;
        }
        if (!Run)
            RunStart = i;
        if (++Run > Best)
        {
            Best = Run;
            BestStart = RunStart;
        }
    }
    *StartingIndex = BestStart;
    return Best;
}
