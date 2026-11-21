/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     File system self-test (shell command "fstest [volume]")
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * On a FAT volume prepared by the boot test (HELLO.TXT, SUBDIR\NESTED.TXT
 * and a 200 000 byte BIG.BIN, written on the host with mtools): opening
 * it mounts it (fs_rec recognizes FAT and loads fastfat from the CD);
 * then reads, a directory listing, volume information, and changes that
 * go back to the disk through the cache: a new 300 000 byte file written
 * and read back, an overwrite in its middle, a long file name, a new
 * directory with a file in it, a rename and a delete. The volume is
 * flushed at the end, and the boot test checks the disk image from the
 * host with mtools after the VM is gone.
 */

#include "qzio.h"
#include "qzps.h"

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

static WCHAR QzVolume[64];

static NTSTATUS
QzFsOpen(PCWSTR Name, ACCESS_MASK Access, ULONG Disposition, ULONG Options, PHANDLE Handle)
{
    WCHAR Buffer[160];
    UNICODE_STRING Path;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Io;

    Path.Buffer = Buffer;
    Path.Length = 0;
    Path.MaximumLength = sizeof(Buffer);
    RtlAppendUnicodeToString(&Path, QzVolume);
    RtlAppendUnicodeToString(&Path, Name);
    InitializeObjectAttributes(&Attributes, &Path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    return ZwCreateFile(Handle, Access | SYNCHRONIZE, &Attributes, &Io, NULL, FILE_ATTRIBUTE_NORMAL,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, Disposition,
                        Options | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
}

static ULONG
QzFsRead(HANDLE File, LONGLONG Offset, PVOID Buffer, ULONG Length, PNTSTATUS Status)
{
    IO_STATUS_BLOCK Io;
    LARGE_INTEGER At;
    At.QuadPart = Offset;
    Io.Information = 0;
    *Status = ZwReadFile(File, NULL, NULL, NULL, &Io, Buffer, Length, &At, NULL);
    return NT_SUCCESS(*Status) ? (ULONG)Io.Information : 0;
}

static NTSTATUS
QzFsWrite(HANDLE File, LONGLONG Offset, const VOID *Buffer, ULONG Length)
{
    IO_STATUS_BLOCK Io;
    LARGE_INTEGER At;
    NTSTATUS Status;
    At.QuadPart = Offset;
    Status = ZwWriteFile(File, NULL, NULL, NULL, &Io, (PVOID)Buffer, Length, &At, NULL);
    if (NT_SUCCESS(Status) && Io.Information != Length)
        Status = STATUS_UNSUCCESSFUL;
    return Status;
}

static LONGLONG
QzFsSize(HANDLE File)
{
    FILE_STANDARD_INFORMATION Info;
    IO_STATUS_BLOCK Io;
    if (!NT_SUCCESS(ZwQueryInformationFile(File, &Io, &Info, sizeof(Info), FileStandardInformation)))
        return -1;
    return Info.EndOfFile.QuadPart;
}

/* Is Name in the directory (long or short name)? */
static BOOLEAN
QzFsListed(PCWSTR Directory, PCWSTR Name, PLONGLONG Size)
{
    UCHAR Buffer[2048];
    IO_STATUS_BLOCK Io;
    UNICODE_STRING Wanted;
    HANDLE Dir;
    BOOLEAN Found = FALSE, Restart = TRUE;

    if (!NT_SUCCESS(QzFsOpen(Directory, FILE_LIST_DIRECTORY, FILE_OPEN, FILE_DIRECTORY_FILE, &Dir)))
        return FALSE;
    RtlInitUnicodeString(&Wanted, Name);
    while (!Found && NT_SUCCESS(ZwQueryDirectoryFile(Dir, NULL, NULL, NULL, &Io, Buffer, sizeof(Buffer),
                                                     FileBothDirectoryInformation, FALSE, NULL, Restart)))
    {
        PFILE_BOTH_DIR_INFORMATION Entry = (PVOID)Buffer;
        Restart = FALSE;
        for (;;)
        {
            UNICODE_STRING Long, Short;
            Long.Buffer = Entry->FileName;
            Long.Length = Long.MaximumLength = (USHORT)Entry->FileNameLength;
            Short.Buffer = Entry->ShortName;
            Short.Length = Short.MaximumLength = Entry->ShortNameLength;
            if (RtlEqualUnicodeString(&Long, &Wanted, TRUE) || RtlEqualUnicodeString(&Short, &Wanted, TRUE))
            {
                Found = TRUE;
                if (Size)
                    *Size = Entry->EndOfFile.QuadPart;
                break;
            }
            if (!Entry->NextEntryOffset)
                break;
            Entry = (PFILE_BOTH_DIR_INFORMATION)((PUCHAR)Entry + Entry->NextEntryOffset);
        }
    }
    ZwClose(Dir);
    return Found;
}

static UCHAR
QzBigByte(ULONG i)
{
    return (UCHAR)((i * 7) % 251);
}

static UCHAR
QzNewByte(ULONG i)
{
    return (UCHAR)((i * 13) % 253);
}

VOID
QzRunFsTests(PCSTR Volume)
{
    static const char Hello[] = "Hello from the host, read by fastfat on qzkrnl.\r\n";
    PUCHAR Buffer = ExAllocatePoolWithTag(NonPagedPool, 0x10000, 'tFzQ');
    UCHAR Small[128];
    IO_STATUS_BLOCK Io;
    HANDLE File;
    NTSTATUS Status;
    LONGLONG Size = -1;
    ULONG i, Got, Bad;
    BOOLEAN Ok;

    QzPassed = QzFailed = 0;
    if (!Buffer)
        return;
    for (i = 0; Volume && Volume[i] && i < RTL_NUMBER_OF(QzVolume) - 1; i++)
        QzVolume[i] = (UCHAR)Volume[i];
    QzVolume[i] = 0;
    if (!i)
        wcscpy(QzVolume, L"\\Device\\HarddiskVolume2");
    QzPrint("File system test on %S:\n", QzVolume);

    /* Opening a file mounts the volume */
    Status = QzFsOpen(L"\\HELLO.TXT", FILE_READ_DATA, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &File);
    QzCheck("open a file: the volume mounts (FAT)", NT_SUCCESS(Status));
    if (!NT_SUCCESS(Status))
    {
        QzPrint("  (status 0x%08lx)\n", Status);
        goto Done;
    }
    Got = QzFsRead(File, 0, Small, sizeof(Small), &Status);
    QzCheck("read a file the host wrote", Got == sizeof(Hello) - 1 && !memcmp(Small, Hello, Got));
    ZwClose(File);

    {
        FILE_FS_SIZE_INFORMATION Sizes;
        union { FILE_FS_VOLUME_INFORMATION Info; UCHAR Raw[128]; } Label;
        UNICODE_STRING Name, Expected = RTL_CONSTANT_STRING(L"QZFAT");
        if (NT_SUCCESS(QzFsOpen(L"\\", FILE_LIST_DIRECTORY, FILE_OPEN, FILE_DIRECTORY_FILE, &File)))
        {
            Ok = NT_SUCCESS(ZwQueryVolumeInformationFile(File, &Io, &Sizes, sizeof(Sizes), FileFsSizeInformation)) &&
                 Sizes.TotalAllocationUnits.QuadPart > 0 && Sizes.AvailableAllocationUnits.QuadPart > 0;
            QzCheck("volume size information", Ok);
            Ok = NT_SUCCESS(ZwQueryVolumeInformationFile(File, &Io, &Label, sizeof(Label), FileFsVolumeInformation));
            Name.Buffer = Label.Info.VolumeLabel;
            Name.Length = Name.MaximumLength = (USHORT)Label.Info.VolumeLabelLength;
            QzCheck("volume label QZFAT", Ok && RtlEqualUnicodeString(&Name, &Expected, TRUE));
            ZwClose(File);
        }
    }
    QzCheck("the root lists BIG.BIN with its size", QzFsListed(L"\\", L"BIG.BIN", &Size) && Size == 200000);
    QzCheck("and SUBDIR", QzFsListed(L"\\", L"SUBDIR", NULL));
    QzCheck("SUBDIR lists NESTED.TXT", QzFsListed(L"\\SUBDIR", L"NESTED.TXT", NULL));

    /* Odd-sized reads across pages, clusters and cache views */
    Status = QzFsOpen(L"\\BIG.BIN", FILE_READ_DATA, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &File);
    Bad = 1;
    if (NT_SUCCESS(Status))
    {
        ULONG Offset = 0;
        Bad = 0;
        while (Offset < 200000)
        {
            Got = QzFsRead(File, Offset, Buffer, 7777, &Status);
            if (!Got)
            {
                Bad++;
                break;
            }
            for (i = 0; i < Got; i++)
                if (Buffer[i] != QzBigByte(Offset + i))
                    Bad++;
            Offset += Got;
        }
        QzFsRead(File, 200000, Buffer, 16, &Status);
        QzCheck("reading at the end: end of file", Status == STATUS_END_OF_FILE);
        ZwClose(File);
    }
    QzCheck("200 000 bytes read back in 7777-byte pieces", Bad == 0);

    /* A new file, written in pieces, read back */
    Status = QzFsOpen(L"\\NEWFILE.TXT", FILE_READ_DATA | FILE_WRITE_DATA, FILE_OVERWRITE_IF,
                      FILE_NON_DIRECTORY_FILE, &File);
    QzCheck("create a file", NT_SUCCESS(Status));
    if (NT_SUCCESS(Status))
    {
        ULONG Offset;
        Ok = TRUE;
        for (Offset = 0; Offset < 300000 && Ok; Offset += 10000)
        {
            for (i = 0; i < 10000; i++)
                Buffer[i] = QzNewByte(Offset + i);
            Ok = NT_SUCCESS(QzFsWrite(File, Offset, Buffer, 10000));
        }
        QzCheck("write 300 000 bytes to it", Ok);
        ZwClose(File);
    }
    Status = QzFsOpen(L"\\NEWFILE.TXT", FILE_READ_DATA | FILE_WRITE_DATA, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &File);
    if (NT_SUCCESS(Status))
    {
        ULONG Offset;
        QzCheck("it has its size", QzFsSize(File) == 300000);
        Bad = 0;
        for (Offset = 0; Offset < 300000; Offset += 0x10000)
        {
            Got = QzFsRead(File, Offset, Buffer, 0x10000, &Status);
            for (i = 0; i < Got; i++)
                if (Buffer[i] != QzNewByte(Offset + i))
                    Bad++;
        }
        QzCheck("and reads back", Bad == 0);
        Ok = NT_SUCCESS(QzFsWrite(File, 150000, "QZQZ", 4));
        Got = QzFsRead(File, 149998, Small, 8, &Status);
        QzCheck("overwrite 4 bytes in the middle", Ok && Got == 8 && !memcmp(Small + 2, "QZQZ", 4) &&
                                                   Small[0] == QzNewByte(149998) && Small[7] == QzNewByte(150005));
        ZwClose(File);
    }

    /* The same file mapped: one copy of the data for views, the cache and the file */
    Status = QzFsOpen(L"\\NEWFILE.TXT", FILE_READ_DATA | FILE_WRITE_DATA, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &File);
    if (NT_SUCCESS(Status))
    {
        HANDLE SectionHandle;
        PQZ_SECTION Section = NULL;
        PQZ_PROCESS A = NULL, B = NULL, Saved;
        PVOID ViewA = NULL, ViewB = NULL;
        SIZE_T Size = 0;
        UCHAR Seen[8];

        Status = ZwCreateSection(&SectionHandle, SECTION_ALL_ACCESS, NULL, NULL, PAGE_READWRITE, SEC_COMMIT, File);
        QzCheck("a section of the file", NT_SUCCESS(Status));
        if (NT_SUCCESS(Status))
        {
            ObReferenceObjectByHandle(SectionHandle, 0, MmSectionObjectType, KernelMode, (PVOID *)&Section, NULL);
            ZwClose(SectionHandle);
        }
        QzPsCreateProcess("fstest-a", QzSystemProcess, &A);
        QzPsCreateProcess("fstest-b", QzSystemProcess, &B);
        if (Section && A && B)
        {
            Status = QzMmMapView(Section, A, &ViewA, 0, NULL, &Size, 0, PAGE_READWRITE);
            QzCheck("map all of it", NT_SUCCESS(Status) && Size >= 300000);
            if (NT_SUCCESS(Status))
            {
                QzPsAttach(A, &Saved);
                Ok = QzCopyFromUser(Seen, (PUCHAR)ViewA + 149998, 8) && !memcmp(Seen + 2, "QZQZ", 4) &&
                     QzCopyFromUser(Small, ViewA, 16);
                for (i = 0; Ok && i < 16; i++)
                    Ok = Small[i] == QzNewByte(i);
                QzCheck("the view shows what was written through the cache", Ok);
                QzCheck("write through the view", QzCopyToUser((PUCHAR)ViewA + 200000, "MAPPED!!", 8));
                QzPsDetach(Saved);

                Got = QzFsRead(File, 200000, Seen, 8, &Status);
                QzCheck("a read of the file sees the view's write", Got == 8 && !memcmp(Seen, "MAPPED!!", 8));
                Status = QzFsWrite(File, 250000, "WRITTEN!", 8);
                QzPsAttach(A, &Saved);
                Ok = NT_SUCCESS(Status) && QzCopyFromUser(Seen, (PUCHAR)ViewA + 250000, 8) &&
                     !memcmp(Seen, "WRITTEN!", 8);
                QzPsDetach(Saved);
                QzCheck("the view sees a write to the file", Ok);

                Size = 0;
                Status = QzMmMapView(Section, B, &ViewB, 0, NULL, &Size, 0, PAGE_READONLY);
                Ok = FALSE;
                if (NT_SUCCESS(Status))
                {
                    QzPsAttach(B, &Saved);
                    Ok = QzCopyFromUser(Seen, (PUCHAR)ViewB + 200000, 8) && !memcmp(Seen, "MAPPED!!", 8);
                    QzPsDetach(Saved);
                    QzMmUnmapView(B, ViewB);
                }
                QzCheck("another process's view of it agrees", Ok);
                QzCheck("unmap the views", NT_SUCCESS(QzMmUnmapView(A, ViewA)));
            }
        }
        if (Section)
            ObDereferenceObject(Section);
        if (A)
            ObDereferenceObject(A);
        if (B)
            ObDereferenceObject(B);
        ZwClose(File);
    }

    /* Long names, directories, rename, delete */
    Status = QzFsOpen(L"\\A long file name.txt", FILE_WRITE_DATA, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, &File);
    if (NT_SUCCESS(Status))
    {
        QzFsWrite(File, 0, "lfn\r\n", 5);
        ZwClose(File);
    }
    QzCheck("a long file name", NT_SUCCESS(Status) && QzFsListed(L"\\", L"A long file name.txt", NULL));

    Status = QzFsOpen(L"\\NEWDIR", FILE_LIST_DIRECTORY, FILE_OPEN_IF, FILE_DIRECTORY_FILE, &File);
    if (NT_SUCCESS(Status))
        ZwClose(File);
    QzCheck("create a directory", NT_SUCCESS(Status));
    Status = QzFsOpen(L"\\NEWDIR\\INNER.TXT", FILE_WRITE_DATA, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, &File);
    if (NT_SUCCESS(Status))
    {
        Status = QzFsWrite(File, 0, "inner\r\n", 7);
        ZwClose(File);
    }
    QzCheck("and a file in it", NT_SUCCESS(Status) && QzFsListed(L"\\NEWDIR", L"INNER.TXT", NULL));

    Status = QzFsOpen(L"\\SUBDIR\\NESTED.TXT", DELETE, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &File);
    if (NT_SUCCESS(Status))
    {
        UCHAR Raw[sizeof(FILE_RENAME_INFORMATION) + 32];
        PFILE_RENAME_INFORMATION Rename = (PVOID)Raw;
        RtlZeroMemory(Raw, sizeof(Raw));
        Rename->ReplaceIfExists = FALSE;
        Rename->FileNameLength = 9 * sizeof(WCHAR);
        RtlCopyMemory(Rename->FileName, L"MOVED.TXT", 9 * sizeof(WCHAR));
        Status = ZwSetInformationFile(File, &Io, Rename, sizeof(Raw), FileRenameInformation);
        ZwClose(File);
    }
    QzCheck("rename a file", NT_SUCCESS(Status) && QzFsListed(L"\\SUBDIR", L"MOVED.TXT", NULL) &&
                             !QzFsListed(L"\\SUBDIR", L"NESTED.TXT", NULL));

    Status = QzFsOpen(L"\\HELLO.TXT", DELETE, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &File);
    if (NT_SUCCESS(Status))
    {
        FILE_DISPOSITION_INFORMATION Dispose = { TRUE };
        Status = ZwSetInformationFile(File, &Io, &Dispose, sizeof(Dispose), FileDispositionInformation);
        ZwClose(File);
    }
    QzCheck("delete a file", NT_SUCCESS(Status) &&
                             QzFsOpen(L"\\HELLO.TXT", FILE_READ_DATA, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &File) ==
                             STATUS_OBJECT_NAME_NOT_FOUND);

    /* Everything to the disk: the volume's flush, and the lazy writer */
    {
        UNICODE_STRING Path;
        OBJECT_ATTRIBUTES Attributes;
        RtlInitUnicodeString(&Path, QzVolume);
        InitializeObjectAttributes(&Attributes, &Path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        Status = ZwCreateFile(&File, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE, &Attributes, &Io, NULL, 0,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
        if (NT_SUCCESS(Status))
        {
            Status = ZwFlushBuffersFile(File, &Io);
            ZwClose(File);
        }
        CcWaitForCurrentLazyWriterActivity();
        QzCheck("flush the volume", NT_SUCCESS(Status) && !CcIsThereDirtyData(NULL));
    }

Done:
    ExFreePoolWithTag(Buffer, 'tFzQ');
    QzPrint("  fstest: %lu passed, %lu failed\n", QzPassed, QzFailed);
}
