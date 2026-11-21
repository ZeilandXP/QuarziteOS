/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel entry point and phase-0 initialization
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qzio.h"

#define QZ_BOOT_STACK_SIZE (16 * 1024)

PLOADER_PARAMETER_BLOCK QzLoaderBlock;
UCHAR QzBootStack[QZ_BOOT_STACK_SIZE] __attribute__((aligned(16)));

/* Referenced by the data-export stubs in qzkrnl.spec */
ULONG_PTR QzUnimplementedData[64];

DECLSPEC_NORETURN VOID NTAPI QzKernelMain(PLOADER_PARAMETER_BLOCK LoaderBlock);

/*
 * KiSystemStartup -- the image entry point FreeLoader jumps to, with the
 * loader parameter block as its only (stdcall) argument. Move onto our own
 * stack straight away; the loader's stack is not ours to keep.
 */
__asm__(
    ".text\n"
    ".globl _KiSystemStartup@4\n"
    "_KiSystemStartup@4:\n"
    "    cli\n"
    "    movl 4(%esp), %eax\n"
    "    movl $_QzBootStack + 16384, %esp\n"
    "    xorl %ebp, %ebp\n"
    "    pushl %eax\n"
    "    call _QzKernelMain@4\n"
    "1:  hlt\n"
    "    jmp 1b\n"
);

static VOID NTAPI
QzStatusThread(PVOID Context)
{
    CHAR Line[96];
    ULONG Seconds, Total, Free;

    UNREFERENCED_PARAMETER(Context);
    for (;;)
    {
        Seconds = (ULONG)(QzInterruptTime / 10000000);
        QzMmGetStats(&Total, &Free);
        QzSnprintf(Line, sizeof(Line),
                   QZ_PRODUCT_NAME " " QZ_VERSION_STR " | " QZ_KERNEL_NAME
                   " | NT %d.%d.%d SP1 | up %02lu:%02lu:%02lu | %lu thr | %lu MB free",
                   QZ_NT_MAJOR, QZ_NT_MINOR, QZ_NT_BUILD,
                   Seconds / 3600, (Seconds / 60) % 60, Seconds % 60,
                   QzThreadCount(), Free / 256);
        QzVgaSetStatus(Line);
        QzDelay(-5000000);   /* 500 ms */
    }
}

VOID
QzPrintModules(VOID)
{
    PLIST_ENTRY Entry;
    PQZ_LDR_ENTRY Module;
    ULONG Count = 0;

    QzPrint("  BASE      SIZE      NAME\n");
    for (Entry = QzLoaderBlock->LoadOrderListHead.Flink;
         Entry != &QzLoaderBlock->LoadOrderListHead;
         Entry = Entry->Flink)
    {
        Module = CONTAINING_RECORD(Entry, QZ_LDR_ENTRY, InLoadOrderLinks);
        QzPrint("  %p  %08lx  %wZ\n", Module->DllBase, Module->SizeOfImage,
                &Module->BaseDllName);
        Count++;
    }
    QzPrint("  %lu modules loaded by FreeLoader\n", Count);
}

ULONG
QzCountBootDrivers(VOID)
{
    PLIST_ENTRY Entry;
    ULONG Count = 0;

    for (Entry = QzLoaderBlock->BootDriverListHead.Flink;
         Entry != &QzLoaderBlock->BootDriverListHead;
         Entry = Entry->Flink)
    {
        Count++;
    }
    return Count;
}

static ULONG
QzCountModules(VOID)
{
    PLIST_ENTRY Entry;
    ULONG Count = 0;

    for (Entry = QzLoaderBlock->LoadOrderListHead.Flink;
         Entry != &QzLoaderBlock->LoadOrderListHead;
         Entry = Entry->Flink)
    {
        Count++;
    }
    return Count;
}

DECLSPEC_NORETURN
VOID
NTAPI
QzKernelMain(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    ULONG Total, Free;
    CHAR Vendor[13], Brand[49];

    QzLoaderBlock = LoaderBlock;

    /* Serial first, so that everything below can report progress */
    QzSerialInit();
    DbgPrint("\n\n" QZ_KERNEL_NAME ": entered at KiSystemStartup, loader block %p\n",
             LoaderBlock);

    QzCpuInit();
    DbgPrint(QZ_KERNEL_NAME ": GDT/IDT/TSS/PCR initialized (IRQL HIGH_LEVEL)\n");

    /*
     * HAL phase 0, as NT does it: before memory management, at HIGH_LEVEL.
     * The HAL checks our PRCB, programs the PICs, calibrates the stall
     * loop, sets up the clock (and reports its tick length to us through
     * KeSetTimeIncrement) and may claim low physical pages from the
     * loader's memory descriptors -- so our page allocator comes after.
     */
    HalInitializeProcessor(0, LoaderBlock);
    if (!HalInitSystem(0, LoaderBlock))
        KeBugCheckEx(HAL_INITIALIZATION_FAILED, 0, 0, 0, 0);
    DbgPrint(QZ_KERNEL_NAME ": HAL phase 0 done (stall factor %lu, clock tick %lu.%04lu ms)\n",
             QzPcr.StallScaleFactor, QzTimeIncrement / 10000, QzTimeIncrement % 10000);

    QzMmInit(LoaderBlock);
    QzVgaInit();
    QzPoolInit();
    QzSharedUserDataInit();
    QzTimerObjInit();

    QzSetConsoleColor(0x0B);
    QzPrint("\n  " QZ_PRODUCT_NAME " " QZ_VERSION_STR "  --  " QZ_KERNEL_NAME
            ", the from-scratch QuarziteOS kernel\n");
    QzSetConsoleColor(0x07);
    QzPrint("  Target compatibility: Windows 7 Service Pack 1 (NT %d.%d build %d)\n",
            QZ_NT_MAJOR, QZ_NT_MINOR, QZ_NT_BUILD);
    QzPrint("  Built " __DATE__ " " __TIME__ " with " __VERSION__ "\n\n");

    QzCpuGetVendor(Vendor, Brand);
    QzMmGetStats(&Total, &Free);
    QzPrint("  CPU:      %s %s\n", Vendor, Brand);
    QzPrint("  Memory:   %lu MB usable, %lu MB free\n", Total / 256, Free / 256);
    QzPrint("  Boot:     %s  (%s)\n", LoaderBlock->ArcBootDeviceName,
            LoaderBlock->NtBootPathName);
    QzPrint("  Options:  %s\n", LoaderBlock->LoadOptions ? LoaderBlock->LoadOptions : "");
    QzPrint("  Loader:   %lu modules, %lu boot drivers, %lu KB registry hive\n",
            QzCountModules(), QzCountBootDrivers(), LoaderBlock->RegistryLength / 1024);
    QzPrint("  Exports:  %u implemented, %u function stubs, %u data stubs\n\n",
            QZ_EXPORTS_IMPLEMENTED, QZ_EXPORTS_STUB_FUNCS, QZ_EXPORTS_STUB_DATA);

    QzSchedInit();
    QzCreateThread("status", QzStatusThread, NULL);

    /* Leave HIGH_LEVEL through the HAL and turn interrupts on */
    QzLowerIrql(PASSIVE_LEVEL);
    DbgPrint(QZ_KERNEL_NAME ": IRQL now %u (IRR %08lx, IDR %08lx)\n", QzPcr.Irql, QzPcr.IRR, QzPcr.IDR);
    _enable();
    DbgPrint(QZ_KERNEL_NAME ": interrupts enabled, IRQL %u\n", QzPcr.Irql);

    /* HAL phase 1: bus handlers, the clock and RTC interrupts, x86 BIOS */
    if (!HalInitSystem(1, LoaderBlock))
        KeBugCheckEx(HAL_INITIALIZATION_FAILED, 1, 0, 0, 0);
    DbgPrint(QZ_KERNEL_NAME ": HAL phase 1 done, clock running\n");

    /* Device interrupts through interrupt objects and the HAL */
    QzKbdInit();
    QzSerialConnectInterrupt();

    /* Object manager: the namespace and its mutex need PASSIVE_LEVEL and a
       running scheduler */
    QzObInit(LoaderBlock);
    QzObInitPhase1();
    QzPsInit();
    QzExWorkerInit();
    QzCmInit(LoaderBlock);
    QzIoInit();
    QzPnpInit();
    DbgPrint(QZ_KERNEL_NAME ": object manager up, %lu objects\n", QzObObjectCount());

    DbgPrint(QZ_KERNEL_NAME ": interrupts on, scheduler running\n");
    QzPrint("  HAL:      hal.dll -- PIC, PIT clock (%lu.%04lu ms tick), stall factor %lu\n",
            QzTimeIncrement / 10000, QzTimeIncrement % 10000, QzPcr.StallScaleFactor);
    QzPrint("  Objects:  %lu in the namespace (\\ObjectTypes, \\Device, \\GLOBAL??, ...)\n\n",
            QzObObjectCount());

    QzShell();
}
