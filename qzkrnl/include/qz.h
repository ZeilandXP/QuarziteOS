/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private kernel header
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * qzkrnl is a from-scratch NT-style kernel. It speaks the NT boot protocol
 * (FreeLoader hands it a LOADER_PARAMETER_BLOCK exactly like it does for
 * ntoskrnl.exe) and reuses the public SDK headers for binary-compatible
 * structures, but none of its code is derived from ntoskrnl.
 */

#pragma once

#include <ntifs.h>
#include <arc/arc.h>
#include <bugcodes.h>
#include "exportstats.h"

/* ---------------------------------------------------------------------- */
/* Product identity                                                        */
/* ---------------------------------------------------------------------- */

#define QZ_PRODUCT_NAME        "QuarziteOS"
#define QZ_KERNEL_NAME         "qzkrnl"
#define QZ_VERSION_STR         "0.1.0"

/* The Windows version QuarziteOS reports and targets: Windows 7 SP1 */
#define QZ_NT_MAJOR            6
#define QZ_NT_MINOR            1
#define QZ_NT_BUILD            7601
#define QZ_NT_CSD_VERSION      0x0100          /* SP 1.0 */
#define QZ_NT_CSD_STRING       L"Service Pack 1"

/* ---------------------------------------------------------------------- */
/* i386 memory layout (non-PAE, as set up by FreeLoader)                  */
/* ---------------------------------------------------------------------- */

#define QZ_PTE_BASE            0xC0000000UL    /* page-table self-map */
#define QZ_PDE_BASE            0xC0300000UL
#define QZ_POOL_BASE           0xA0000000UL    /* nonpaged pool window */
#define QZ_POOL_SIZE           (64UL * 1024 * 1024)
#define QZ_IOSPACE_BASE        0xB0000000UL    /* MmMapIoSpace window */
#define QZ_IOSPACE_SIZE        (64UL * 1024 * 1024)

#define QZ_PAGE_SIZE           4096UL
#define QZ_PAGE_SHIFT          12

#define QZ_PTE_PRESENT         0x001
#define QZ_PTE_WRITE           0x002
#define QZ_PTE_PWT             0x008
#define QZ_PTE_PCD             0x010
#define QZ_PTE_LARGE           0x080

/* GDT selectors -- identical to the NT x86 layout */
#define QZ_SEL_R0_CODE         0x08
#define QZ_SEL_R0_DATA         0x10
#define QZ_SEL_R3_CODE         0x18
#define QZ_SEL_R3_DATA         0x20
#define QZ_SEL_TSS             0x28
#define QZ_SEL_R0_PCR          0x30

/* Interrupt vectors -- the 8259 PIC is remapped to the NT vector base */
#define QZ_VECTOR_IRQ_BASE     0x30
#define QZ_VECTOR_YIELD        0x2F
#define QZ_IRQ_TIMER           0
#define QZ_IRQ_KEYBOARD        1
#define QZ_IRQ_COM1            4

#define QZ_TIMER_HZ            100
#define QZ_TIME_INCREMENT      (10000000UL / QZ_TIMER_HZ)  /* 100ns units */

/* ---------------------------------------------------------------------- */
/* Trap frame (layout fixed by the assembly stubs in ke/i386/trap.c)       */
/* ---------------------------------------------------------------------- */

typedef struct _QZ_TRAP_FRAME
{
    ULONG Edi, Esi, Ebp, EspDummy, Ebx, Edx, Ecx, Eax;   /* pushal */
    ULONG Gs, Fs, Es, Ds;
    ULONG Vector;
    ULONG ErrorCode;
    ULONG Eip, Cs, EFlags;                              /* pushed by CPU */
} QZ_TRAP_FRAME, *PQZ_TRAP_FRAME;

/* ---------------------------------------------------------------------- */
/* Threads                                                                 */
/* ---------------------------------------------------------------------- */

typedef VOID (NTAPI *PQZ_THREAD_ROUTINE)(PVOID Context);

typedef enum _QZ_THREAD_STATE
{
    QzThreadReady,
    QzThreadWaiting,
    QzThreadTerminated
} QZ_THREAD_STATE;

typedef struct _QZ_THREAD
{
    PQZ_TRAP_FRAME Frame;          /* saved context while not running */
    PVOID StackBase;               /* lowest address of the stack */
    ULONG StackSize;
    ULONG Id;
    QZ_THREAD_STATE State;
    ULONG WakeTick;
    ULONG Switches;
    CHAR Name[16];
    struct _QZ_THREAD *Next;       /* circular run list */
} QZ_THREAD, *PQZ_THREAD;

/* ---------------------------------------------------------------------- */
/* Globals                                                                 */
/* ---------------------------------------------------------------------- */

extern PLOADER_PARAMETER_BLOCK QzLoaderBlock;
extern volatile ULONG QzTicks;
extern PQZ_THREAD QzCurrentThread;
extern ULONG NtBuildNumber;

/* ---------------------------------------------------------------------- */
/* CPU helpers                                                             */
/* ---------------------------------------------------------------------- */

static __inline UCHAR QzInB(USHORT Port)
{
    UCHAR v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(Port));
    return v;
}

static __inline VOID QzOutB(USHORT Port, UCHAR Value)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(Value), "Nd"(Port));
}

static __inline VOID QzIoWait(VOID)
{
    QzOutB(0x80, 0);
}

static __inline ULONG QzSaveFlagsAndDisable(VOID)
{
    ULONG Flags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(Flags) : : "memory");
    return Flags;
}

static __inline VOID QzRestoreFlags(ULONG Flags)
{
    if (Flags & 0x200)
        __asm__ __volatile__("sti" : : : "memory");
}

static __inline VOID QzHalt(VOID)
{
    __asm__ __volatile__("hlt" : : : "memory");
}

static __inline VOID QzInvlpg(ULONG_PTR Va)
{
    __asm__ __volatile__("invlpg (%0)" : : "r"(Va) : "memory");
}

static __inline ULONG QzReadCr2(VOID)
{
    ULONG v;
    __asm__ __volatile__("movl %%cr2, %0" : "=r"(v));
    return v;
}

static __inline ULONG QzReadCr3(VOID)
{
    ULONG v;
    __asm__ __volatile__("movl %%cr3, %0" : "=r"(v));
    return v;
}

/* ---------------------------------------------------------------------- */
/* Subsystems                                                              */
/* ---------------------------------------------------------------------- */

/* ke/i386/cpu.c */
VOID QzCpuInit(VOID);
VOID QzSetIdtGate(ULONG Vector, PVOID Handler);
VOID QzCpuGetVendor(PCHAR Vendor13, PCHAR Brand49);

/* ke/i386/trap.c */
PQZ_TRAP_FRAME __cdecl QzTrapDispatch(PQZ_TRAP_FRAME Frame);
VOID QzInstallTrapStubs(VOID);

/* ke/irq.c */
VOID QzPicInit(VOID);
VOID QzPicUnmask(ULONG Irq);
VOID QzPicEoi(ULONG Irq);
VOID QzTimerInit(VOID);

/* ke/sched.c */
VOID QzSchedInit(VOID);
PQZ_THREAD QzCreateThread(PCSTR Name, PQZ_THREAD_ROUTINE Routine, PVOID Context);
PQZ_TRAP_FRAME QzSchedule(PQZ_TRAP_FRAME Frame);
VOID QzYield(VOID);
VOID QzSleepTicks(ULONG Ticks);
VOID QzDumpThreads(VOID);
ULONG QzThreadCount(VOID);

/* ke/bugcheck.c */
DECLSPEC_NORETURN VOID QzBugCheckWithText(ULONG Code, ULONG_PTR P1, ULONG_PTR P2,
                                          ULONG_PTR P3, ULONG_PTR P4, PCSTR Text);

/* mm/mm.c */
VOID QzMmInit(PLOADER_PARAMETER_BLOCK LoaderBlock);
ULONG QzAllocPhysicalPage(VOID);
VOID QzFreePhysicalPage(ULONG Pfn);
VOID QzMapPage(ULONG_PTR Va, ULONG Pfn, ULONG Flags);
VOID QzUnmapPage(ULONG_PTR Va);
PVOID QzMapPhysical(ULONG PhysicalAddress, ULONG Size, BOOLEAN Uncached);
VOID QzMmDumpInfo(VOID);
VOID QzMmGetStats(PULONG TotalPages, PULONG FreePages);

/* mm/pool.c */
VOID QzPoolInit(VOID);
VOID QzPoolDumpInfo(VOID);

/* drv/serial.c */
VOID QzSerialInit(VOID);
VOID QzSerialPutChar(CHAR c);
VOID QzSerialEnableRxInterrupt(VOID);
VOID QzSerialIsr(VOID);

/* drv/vga.c */
VOID QzVgaInit(VOID);
VOID QzVgaPutChar(CHAR c);
VOID QzVgaSetStatus(PCSTR Text);
VOID QzVgaBlueScreen(VOID);
BOOLEAN QzVgaAvailable(VOID);

/* drv/kbd.c */
VOID QzKbdInit(VOID);
VOID QzKbdIsr(VOID);

/* drv/input.c */
VOID QzInputPush(CHAR c);
CHAR QzInputGet(VOID);                 /* blocks */
VOID QzReadLine(PCHAR Buffer, ULONG Size);

/* rtl/print.c */
int QzVsnprintf(char *Buffer, size_t Size, const char *Format, va_list Args);
int QzSnprintf(char *Buffer, size_t Size, const char *Format, ...);
VOID QzPrint(PCSTR Format, ...);       /* console (VGA + serial) */
VOID QzPutString(PCSTR String);
VOID QzSetConsoleColor(UCHAR Attribute);

/* ex/shell.c */
DECLSPEC_NORETURN VOID QzShell(VOID);

/* rtl/rtl.c */
PVOID QzFindKernelExport(PCSTR Name);
