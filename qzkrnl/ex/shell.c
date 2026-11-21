/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Built-in kernel debug shell (VGA/keyboard and COM1)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Until qzkrnl can start the native subsystem (smss.exe), this shell is how
 * you poke at the kernel. Commands are deliberately tiny and self-checking.
 */

#include "qz.h"

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
            "  drivers           boot drivers listed by FreeLoader\n"
            "  threads           kernel threads\n"
            "  spawn [n]         start n demo threads that count and exit\n"
            "  sleep <ms>        KeDelayExecutionThread for <ms> milliseconds\n"
            "  uptime            tick count and uptime\n"
            "  cpu               processor identification\n"
            "  exports           kernel export coverage\n"
            "  synctest          self-test: IRQL, DPCs, spinlocks, events, mutexes,\n"
            "                    semaphores, waits, timers, system time\n"
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
QzCmdDrivers(VOID)
{
    PLIST_ENTRY Entry;
    PBOOT_DRIVER_LIST_ENTRY Driver;

    for (Entry = QzLoaderBlock->BootDriverListHead.Flink;
         Entry != &QzLoaderBlock->BootDriverListHead;
         Entry = Entry->Flink)
    {
        Driver = CONTAINING_RECORD(Entry, BOOT_DRIVER_LIST_ENTRY, Link);
        QzPrint("  %wZ\n", &Driver->FilePath);
    }
    QzPrint("  %lu boot drivers (loaded and bound to qzkrnl's exports, not started)\n",
            QzCountBootDrivers());
}

static VOID
QzExecute(PCHAR Line)
{
    PCHAR Cursor = Line;
    PCHAR Cmd = QzNextToken(&Cursor);
    PCHAR Arg = QzNextToken(&Cursor);

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
        QzCmdDrivers();
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
