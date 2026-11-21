/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Built-in kernel debug shell (VGA/keyboard and COM1)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Until qzkrnl can start the native subsystem (smss.exe), this shell is how
 * you poke at the kernel. Commands are deliberately tiny and self-checking.
 */

#include "qzio.h"
#include "qzps.h"

unsigned long __cdecl strtoul(const char *s, char **End, int Base);
#include <ntdddisk.h>
#include <ntddcdrm.h>

VOID QzPrintModules(VOID);
ULONG QzCountBootDrivers(VOID);

static PCHAR
QzNextToken(PCHAR *Cursor)
{
    PCHAR p = *Cursor, Start;
    while (*p == ' ') p++;
    if (!*p) { *Cursor = p; return NULL; }
    Start = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    *Cursor = p;
    return Start;
}

static ULONG
QzParseUlong(PCSTR s)
{
    ULONG v = 0;
    while (s && *s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

static VOID
QzCmdHelp(VOID)
{
    QzPrint("  help              this list\n"
            "  ver               version information (via RtlGetVersion/PsGetVersion)\n"
            "  mem               physical memory and loader descriptors\n"
            "  pool              pool statistics\n"
            "  pooltest          allocate/free stress test of the pool\n"
            "  mods              modules loaded by FreeLoader\n"
            "  drivers           boot drivers: started, ready or waiting for exports\n"
            "  threads           kernel threads\n"
            "  spawn [n]         start n demo threads that count and exit\n"
            "  sleep <ms>        KeDelayExecutionThread for <ms> milliseconds\n"
            "  uptime            tick count and uptime\n"
            "  cpu               processor identification\n"
            "  exports           kernel export coverage\n"
            "  synctest          self-test: IRQL, DPCs, spinlocks, events, mutexes,\n"
            "                    semaphores, waits, timers, system time\n"
            "  obtest            self-test: object manager (types, names, handles,\n"
            "                    directories, symbolic links, executive objects)\n"
            "  obj [path]        list an object directory (default \\)\n"
            "  iotest            self-test: I/O manager (drivers, devices, IRPs, files,\n"
            "                    filters, mounting a test file system)\n"
            "  pnp               start the device tree (HAL, then PCI and below)\n"
            "  devtree           the device tree with resources\n"
            "  start <service>   start a boot driver (DriverEntry)\n"
            "  reg [path]        list a registry key (default \\Registry\\Machine\\System)\n"
            "  vmtest            self-test: process address spaces (reserve, commit,\n"
            "                    protect, guard pages, isolation, handles)\n"
            "  usertest          self-test: ring 3 (system calls, exceptions, preemption,\n"
            "                    FPU state)\n"
            "  syscalls [on|off] system service statistics; trace every call\n"
            "  ps                list the processes\n"
            "  vm [pid]          a process's address space (VADs)\n"
            "  regtest           self-test: registry (keys, values, volatile keys, links,\n"
            "                    deletion, change notification, Rtl helpers)\n"
            "  pcicfg            PCI bus 0 configuration space through the HAL\n"
            "  read <dev> <sec>  read a sector through a device stack (hex dump)\n"
            "  irptrace [off]    log every IRP dispatch to the debug output\n"
            "  verbose [off]     show every DbgPrintEx level, not just errors\n"
            "  drvobj            driver objects\n"
            "  devices           device objects\n"
            "  irql              current IRQL, DPC and timer statistics\n"
            "  hal               HAL clock and interrupt statistics\n"
            "  time              date and time (CMOS clock + KeQuerySystemTime)\n"
            "  call <export>     call a kernel export by name (stubs bug check)\n"
            "  bugcheck          KeBugCheckEx(MANUALLY_INITIATED_CRASH)\n"
            "  crash             touch an unmapped address (page fault)\n"
            "  reboot            reset the machine\n");
}

static VOID
QzCmdVer(VOID)
{
    RTL_OSVERSIONINFOEXW Info;
    ULONG Major, Minor, Build;
    UNICODE_STRING Csd;
    BOOLEAN Checked;

    Info.dwOSVersionInfoSize = sizeof(Info);
    RtlGetVersion((PRTL_OSVERSIONINFOW)&Info);
    Checked = PsGetVersion(&Major, &Minor, &Build, &Csd);

    QzPrint("  " QZ_PRODUCT_NAME " " QZ_VERSION_STR " (" QZ_KERNEL_NAME ")\n");
    QzPrint("  RtlGetVersion: %lu.%lu.%lu %S, SP %u.%u, product type %u\n",
            Info.dwMajorVersion, Info.dwMinorVersion, Info.dwBuildNumber,
            Info.szCSDVersion, Info.wServicePackMajor, Info.wServicePackMinor,
            Info.wProductType);
    QzPrint("  PsGetVersion:  %lu.%lu.%lu %wZ (%s build)\n",
            Major, Minor, Build, &Csd, Checked ? "checked" : "free");
    QzPrint("  NtBuildNumber: 0x%08lx\n", NtBuildNumber);
}

static VOID
QzCmdPoolTest(VOID)
{
    PVOID Blocks[64];
    ULONG i, Round, Errors = 0;

    for (Round = 0; Round < 8; Round++)
    {
        for (i = 0; i < RTL_NUMBER_OF(Blocks); i++)
        {
            ULONG Size = 16 + ((i * 37 + Round * 101) % 2000);
            Blocks[i] = ExAllocatePoolWithTag(NonPagedPool, Size, 'tsTQ');
            if (!Blocks[i]) { Errors++; continue; }
            RtlFillMemory(Blocks[i], Size, (UCHAR)i);
        }
        for (i = 0; i < RTL_NUMBER_OF(Blocks); i += 2)
            if (Blocks[i]) ExFreePoolWithTag(Blocks[i], 'tsTQ');
        for (i = 1; i < RTL_NUMBER_OF(Blocks); i += 2)
        {
            if (Blocks[i] && *(PUCHAR)Blocks[i] != (UCHAR)i)
                Errors++;
            if (Blocks[i]) ExFreePoolWithTag(Blocks[i], 'tsTQ');
        }
    }
    QzPrint("  pooltest: %lu allocations, %lu errors -> %s\n",
            Round * (ULONG)RTL_NUMBER_OF(Blocks), Errors, Errors ? "FAIL" : "PASS");
    QzPoolDumpInfo();
}

static VOID NTAPI
QzDemoThread(PVOID Context)
{
    ULONG Id = (ULONG)(ULONG_PTR)Context, i;
    LARGE_INTEGER Delay;

    for (i = 1; i <= 3; i++)
    {
        QzPrint("  [demo %lu] tick %lu (thread %lu)\n", Id, i, QzCurrentThread->Id);
        Delay.QuadPart = -(LONGLONG)(100 + Id * 50) * 10000;   /* ms -> 100ns */
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    QzPrint("  [demo %lu] done\n", Id);
}

static VOID
QzExecute(PCHAR Line)
{
    PCHAR Cursor = Line;
    PCHAR Cmd = QzNextToken(&Cursor);
    PCHAR Arg = QzNextToken(&Cursor);
    PCHAR Rest = Cursor;                 /* everything after the first argument */

    if (!Cmd)
        return;

    if (!strcmp(Cmd, "help") || !strcmp(Cmd, "?"))
        QzCmdHelp();
    else if (!strcmp(Cmd, "ver"))
        QzCmdVer();
    else if (!strcmp(Cmd, "mem"))
        QzMmDumpInfo();
    else if (!strcmp(Cmd, "pool"))
        QzPoolDumpInfo();
    else if (!strcmp(Cmd, "pooltest"))
        QzCmdPoolTest();
    else if (!strcmp(Cmd, "mods"))
        QzPrintModules();
    else if (!strcmp(Cmd, "drivers"))
        QzIoDumpBootDrivers();
    else if (!strcmp(Cmd, "threads"))
        QzDumpThreads();
    else if (!strcmp(Cmd, "spawn"))
    {
        ULONG n = Arg ? QzParseUlong(Arg) : 2, i;
        if (n == 0 || n > 8) n = 2;
        for (i = 0; i < n; i++)
        {
            CHAR Name[16];
            QzSnprintf(Name, sizeof(Name), "demo%lu", i + 1);
            if (!QzCreateThread(Name, QzDemoThread, (PVOID)(ULONG_PTR)(i + 1)))
                QzPrint("  spawn: out of memory\n");
        }
    }
    else if (!strcmp(Cmd, "sleep"))
    {
        LARGE_INTEGER Delay;
        ULONG Ms = QzParseUlong(Arg), StartTicks = QzTicks;
        ULONGLONG Start = KeQueryInterruptTime();
        Delay.QuadPart = -(LONGLONG)Ms * 10000;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
        QzPrint("  slept %lu ms (%lu ms elapsed, %lu clock ticks)\n", Ms,
                (ULONG)((KeQueryInterruptTime() - Start) / 10000), QzTicks - StartTicks);
    }
    else if (!strcmp(Cmd, "uptime"))
    {
        LARGE_INTEGER Ticks;
        ULONG Ms = (ULONG)(KeQueryInterruptTime() / 10000);
        KeQueryTickCount(&Ticks);
        QzPrint("  KeTickCount = %I64u, increment %lu x 100ns, uptime %lu.%03lu s\n",
                Ticks.QuadPart, KeQueryTimeIncrement(), Ms / 1000, Ms % 1000);
    }
    else if (!strcmp(Cmd, "hal"))
    {
        QzPrint("  HAL: clock tick %lu x 100ns, stall factor %lu, IDR %08lx, IRR %08lx\n"
                "  %lu clock interrupts, %lu device interrupts, %lu dispatch interrupts,\n"
                "  %lu interrupt objects connected\n",
                QzTimeIncrement, QzPcr.StallScaleFactor, QzPcr.IDR, QzPcr.IRR,
                QzPrcb->ClockInterrupts, QzPrcb->InterruptCount,
                QzPrcb->DispatchInterrupts, QzConnectedInterruptCount());
    }
    else if (!strcmp(Cmd, "cpu"))
    {
        CHAR Vendor[13], Brand[49];
        QzCpuGetVendor(Vendor, Brand);
        QzPrint("  %s  %s\n", Vendor, Brand);
    }
    else if (!strcmp(Cmd, "exports"))
    {
        QzPrint("  ntoskrnl.exe export surface: %u implemented by qzkrnl,\n"
                "  %u function stubs and %u data stubs (see qzkrnl/exports.spec)\n",
                QZ_EXPORTS_IMPLEMENTED, QZ_EXPORTS_STUB_FUNCS, QZ_EXPORTS_STUB_DATA);
    }
    else if (!strcmp(Cmd, "synctest"))
        QzRunSyncTests();
    else if (!strcmp(Cmd, "obtest"))
        QzRunObjectTests();
    else if (!strcmp(Cmd, "iotest"))
        QzRunIoTests();
    else if (!strcmp(Cmd, "regtest"))
        QzRunRegistryTests();
    else if (!strcmp(Cmd, "vmtest"))
        QzRunVmTests();
    else if (!strcmp(Cmd, "ps"))
        QzPsDumpProcesses();
    else if (!strcmp(Cmd, "usertest"))
        QzRunUserTests();
    else if (!strcmp(Cmd, "syscalls"))
    {
        if (Arg)
            QzSyscallTrace = !strcmp(Arg, "on");
        QzSyscallStats();
        QzPrint("  tracing %s\n", QzSyscallTrace ? "on" : "off");
    }
    else if (!strcmp(Cmd, "vm"))
    {
        ULONG Pid = Arg ? strtoul(Arg, NULL, 0) : 4;
        PQZ_PROCESS Process = QzPsLookupProcess(Pid);
        if (Process)
        {
            QzMmDumpAddressSpace(Process);
            ObDereferenceObject(Process);
        }
        else
            QzPrint("  vm: no process %lu\n", Pid);
    }
    else if (!strcmp(Cmd, "pnp"))
    {
        NTSTATUS Status = QzPnpStart();
        ULONG Started, Nodes = QzPnpDeviceCount(&Started);
        QzPrint("  PnP: 0x%08lx, %lu device node(s), %lu started\n", Status, Nodes, Started);
    }
    else if (!strcmp(Cmd, "devtree"))
        QzPnpDumpTree(TRUE);
    else if (!strcmp(Cmd, "start") || !strcmp(Cmd, "reg"))
    {
        WCHAR Wide[128];
        UNICODE_STRING Name;
        PDRIVER_OBJECT Driver;
        ULONG i = 0;
        PCSTR Source = Arg ? Arg : (Cmd[0] == 'r' ? "\\Registry\\Machine\\System" : "");
        while (Source[i] && i < RTL_NUMBER_OF(Wide) - 1)
        {
            Wide[i] = (UCHAR)Source[i];
            i++;
        }
        Wide[i] = UNICODE_NULL;
        if (Cmd[0] == 'r')
        {
            QzCmDumpKey(Wide);
        }
        else
        {
            RtlInitUnicodeString(&Name, Wide);
            QzPrint("  %S: 0x%08lx\n", Wide, QzIoStartBootDriver(&Name, &Driver));
        }
    }
    else if (!strcmp(Cmd, "pcicfg"))
    {
        /* PCI bus 0 through the HAL's bus handlers */
        ULONG Device, Function, Found = 0;
        for (Device = 0; Device < 32; Device++)
        {
            for (Function = 0; Function < 8; Function++)
            {
                PCI_SLOT_NUMBER Slot;
                ULONG Id = 0xFFFFFFFF, Class = 0, Length;
                Slot.u.AsULONG = 0;
                Slot.u.bits.DeviceNumber = Device;
                Slot.u.bits.FunctionNumber = Function;
                Length = HalGetBusDataByOffset(PCIConfiguration, 0, Slot.u.AsULONG, &Id, 0, 4);
                if (Length < 4 || (Id & 0xFFFF) == 0xFFFF || !Id)
                {
                    if (Function == 0 && Device == 0)
                        QzPrint("  00:00.0 -> length %lu, id %08lx\n", Length, Id);
                    if (Function == 0)
                        break;
                    continue;
                }
                HalGetBusDataByOffset(PCIConfiguration, 0, Slot.u.AsULONG, &Class, 8, 4);
                QzPrint("  00:%02lx.%lu  %04lx:%04lx  class %06lx\n", Device, Function, Id & 0xFFFF,
                        Id >> 16, Class >> 8);
                Found++;
            }
        }
        QzPrint("  %lu PCI function(s) on bus 0\n", Found);
    }
    else if (!strcmp(Cmd, "read"))
    {
        /* read <device path> <sector> [sector size]: raw I/O through the
           device stack, as a file system would do it */
        CHAR Path[96];
        ULONG Sector = 0, SectorSize = 0, i = 0;
        PCSTR p = Arg ? Arg : "";
        WCHAR Wide[96];
        UNICODE_STRING Name;
        OBJECT_ATTRIBUTES Attributes;
        IO_STATUS_BLOCK IoStatus;
        LARGE_INTEGER Offset;
        HANDLE File;
        NTSTATUS Status;
        PUCHAR Buffer;

        while (*p && *p != ' ' && i < sizeof(Path) - 1)
            Path[i++] = *p++;
        Path[i] = 0;
        p = Rest;
        while (*p == ' ')
            p++;
        while (*p >= '0' && *p <= '9')
            Sector = Sector * 10 + (*p++ - '0');
        while (*p == ' ')
            p++;
        while (*p >= '0' && *p <= '9')
            SectorSize = SectorSize * 10 + (*p++ - '0');
        for (i = 0; Path[i]; i++)
            Wide[i] = (UCHAR)Path[i];
        Wide[i] = 0;
        if (!SectorSize)
            SectorSize = (strstr(Path, "CdRom") ? 2048 : 512);

        RtlInitUnicodeString(&Name, Wide);
        InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        Status = ZwOpenFile(&File, FILE_READ_DATA | SYNCHRONIZE, &Attributes, &IoStatus,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
        if (!NT_SUCCESS(Status))
        {
            QzPrint("  read: cannot open %s (0x%08lx)\n", Path, Status);
        }
        else if (!(Buffer = ExAllocatePoolWithTag(NonPagedPool, SectorSize, 'daeR')))
        {
            ZwClose(File);
        }
        else
        {
            Offset.QuadPart = (LONGLONG)Sector * SectorSize;
            Status = ZwReadFile(File, NULL, NULL, NULL, &IoStatus, Buffer, SectorSize, &Offset, NULL);
            QzPrint("  %s sector %lu (%lu bytes): status 0x%08lx, %lu bytes read\n", Path, Sector,
                    SectorSize, Status, (ULONG)IoStatus.Information);
            if (!NT_SUCCESS(Status))
            {
                PFILE_OBJECT FileObject;
                if (NT_SUCCESS(ObReferenceObjectByHandle(File, 0, IoFileObjectType, KernelMode,
                                                         (PVOID *)&FileObject, NULL)))
                {
                    PDEVICE_OBJECT Target = IoGetRelatedDeviceObject(FileObject);
                    QzPrint("    file object flags %08lx, device %p (%wZ), sent to %p (%wZ), vpb %p\n",
                            FileObject->Flags, FileObject->DeviceObject,
                            &FileObject->DeviceObject->DriverObject->DriverName, Target,
                            &Target->DriverObject->DriverName, FileObject->Vpb);
                    ObDereferenceObject(FileObject);
                }
                {
                    DISK_GEOMETRY Geometry;
                    RtlZeroMemory(&Geometry, sizeof(Geometry));
                    Status = ZwDeviceIoControlFile(File, NULL, NULL, NULL, &IoStatus,
                                                   IOCTL_CDROM_GET_DRIVE_GEOMETRY, NULL, 0,
                                                   &Geometry, sizeof(Geometry));
                    QzPrint("    geometry: status 0x%08lx, %lu bytes/sector, %lu sectors/track, "
                            "%I64u cylinders, media %u\n", Status, Geometry.BytesPerSector,
                            Geometry.SectorsPerTrack, Geometry.Cylinders.QuadPart, Geometry.MediaType);
                }
            }
            if (NT_SUCCESS(Status))
            {
                ULONG Row, k;
                for (Row = 0; Row < 64 && Row < IoStatus.Information; Row += 16)
                {
                    QzPrint("    %04lx ", Row);
                    for (k = 0; k < 16; k++)
                        QzPrint(" %02x", Buffer[Row + k]);
                    QzPrint("  ");
                    for (k = 0; k < 16; k++)
                        QzPrint("%c", (Buffer[Row + k] >= 0x20 && Buffer[Row + k] < 0x7F) ? Buffer[Row + k] : '.');
                    QzPrint("\n");
                }
                if (IoStatus.Information >= 512)
                    QzPrint("    ... last two bytes of the first 512: %02x %02x\n", Buffer[510], Buffer[511]);
            }
            ExFreePoolWithTag(Buffer, 'daeR');
            ZwClose(File);
        }
    }
    else if (!strcmp(Cmd, "verbose"))
    {
        extern BOOLEAN QzDebugVerbose;
        QzDebugVerbose = !(Arg && !strcmp(Arg, "off"));
        QzPrint("  all DbgPrintEx levels %s\n", QzDebugVerbose ? "shown" : "filtered (errors only)");
    }
    else if (!strcmp(Cmd, "irptrace"))
    {
        extern BOOLEAN QzIoTraceIrps;
        QzIoTraceIrps = !(Arg && !strcmp(Arg, "off"));
        QzPrint("  IRP tracing %s (on the debug output)\n", QzIoTraceIrps ? "on" : "off");
    }
    else if (!strcmp(Cmd, "drvobj"))
        QzIoDumpDrivers();
    else if (!strcmp(Cmd, "devices"))
        QzIoDumpDevices();
    else if (!strcmp(Cmd, "obj"))
    {
        WCHAR Path[128];
        ULONG i = 0;
        PCSTR Source = Arg ? Arg : "\\";
        while (Source[i] && i < RTL_NUMBER_OF(Path) - 1)
        {
            Path[i] = (UCHAR)Source[i];
            i++;
        }
        Path[i] = UNICODE_NULL;
        QzObDumpDirectory(Path);
        QzPrint("  %lu objects, %lu handles in the system handle table\n",
                QzObObjectCount(), QzObHandleCount());
    }
    else if (!strcmp(Cmd, "irql"))
    {
        QzPrint("  IRQL %u (PCR FS:0x24 = %u, HAL KeGetCurrentIrql = %u), %lu DPCs retired, %lu queued, %lu active timers\n",
                QzGetCurrentIrql(), __readfsbyte(0x24), KeGetCurrentIrql(), QzPrcb->DpcCount,
                QzPrcb->DpcQueueDepth, QzActiveTimerCount());
    }
    else if (!strcmp(Cmd, "time"))
    {
        static PCSTR Days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        LARGE_INTEGER Now;
        TIME_FIELDS Tf;
        KeQuerySystemTime(&Now);
        RtlTimeToTimeFields(&Now, &Tf);
        QzPrint("  %s %04d-%02d-%02d %02d:%02d:%02d.%03d (system time %I64u)\n",
                Days[Tf.Weekday % 7], Tf.Year, Tf.Month, Tf.Day, Tf.Hour, Tf.Minute,
                Tf.Second, Tf.Milliseconds, Now.QuadPart);
    }
    else if (!strcmp(Cmd, "call"))
    {
        PVOID Address = Arg ? QzFindKernelExport(Arg) : NULL;
        if (!Address)
            QzPrint("  call: no such export\n");
        else
        {
            QzPrint("  calling %s at %p ...\n", Arg, Address);
            ((VOID (__cdecl *)(VOID))Address)();
            QzPrint("  %s returned\n", Arg);
        }
    }
    else if (!strcmp(Cmd, "bugcheck"))
        KeBugCheckEx(MANUALLY_INITIATED_CRASH, 0, 0, 0, 0);
    else if (!strcmp(Cmd, "crash"))
        (VOID)*(volatile ULONG *)0xDEADBEEF;
    else if (!strcmp(Cmd, "reboot"))
    {
        QzPrint("  Resetting...\n");
        HalReturnToFirmware(HalRebootRoutine);
    }
    else
        QzPrint("  unknown command '%s' -- try 'help'\n", Cmd);
}

DECLSPEC_NORETURN
VOID
QzShell(VOID)
{
    CHAR Line[128];

    QzPrint("  Type 'help' for a list of commands.\n\n");
    for (;;)
    {
        QzSetConsoleColor(0x0A);
        QzPutString("qz> ");
        QzSetConsoleColor(0x07);
        QzReadLine(Line, sizeof(Line));
        QzExecute(Line);
    }
}
