/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     i386 processor setup: GDT, TSS, IDT, PCR selector
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qz.h"

#include <pshpack1.h>
typedef struct _QZ_GDT_ENTRY
{
    USHORT LimitLow;
    USHORT BaseLow;
    UCHAR BaseMid;
    UCHAR Access;
    UCHAR Granularity;     /* flags (high nibble) + limit 16..19 */
    UCHAR BaseHigh;
} QZ_GDT_ENTRY;

typedef struct _QZ_IDT_ENTRY
{
    USHORT OffsetLow;
    USHORT Selector;
    UCHAR Zero;
    UCHAR TypeAttr;
    USHORT OffsetHigh;
} QZ_IDT_ENTRY;

typedef struct _QZ_DESCRIPTOR
{
    USHORT Limit;
    ULONG Base;
} QZ_DESCRIPTOR;

typedef struct _QZ_TSS
{
    ULONG Link, Esp0, Ss0, Esp1, Ss1, Esp2, Ss2;
    ULONG Cr3, Eip, EFlags, Eax, Ecx, Edx, Ebx, Esp, Ebp, Esi, Edi;
    ULONG Es, Cs, Ss, Ds, Fs, Gs, Ldt;
    USHORT Trap, IoMapBase;
} QZ_TSS;
#include <poppack.h>

/* The processor control region: the real NT KIPCR at KIP0PCRADDRESS */
QZ_PRCB_EXT QzPrcbExt;

C_ASSERT(sizeof(KIPCR) <= PAGE_SIZE);   /* the loader maps exactly one page */

/* Offsets drivers and the HAL depend on */
C_ASSERT(FIELD_OFFSET(KIPCR, SelfPcr) == 0x1C);
C_ASSERT(FIELD_OFFSET(KIPCR, Prcb) == 0x20);
C_ASSERT(FIELD_OFFSET(KIPCR, Irql) == 0x24);
C_ASSERT(FIELD_OFFSET(KIPCR, IRR) == 0x28);
C_ASSERT(FIELD_OFFSET(KIPCR, IDR) == 0x30);
C_ASSERT(FIELD_OFFSET(KIPCR, Number) == 0x51);
C_ASSERT(FIELD_OFFSET(KIPCR, PrcbData) == 0x120);
C_ASSERT(FIELD_OFFSET(KIPCR, PrcbData.CurrentThread) == 0x124);

static QZ_GDT_ENTRY QzGdt[16] __attribute__((aligned(16)));
static QZ_IDT_ENTRY QzIdt[256] __attribute__((aligned(16)));
QZ_TSS QzTss __attribute__((aligned(16)));   /* the sysenter entry reads Esp0 */
static QZ_TSS QzDoubleFaultTss __attribute__((aligned(16)));
static UCHAR QzDoubleFaultStack[8192] __attribute__((aligned(16)));
static UCHAR QzTssStack[8192] __attribute__((aligned(16)));

static VOID
QzSetGdtEntry(ULONG Selector, ULONG Base, ULONG Limit, UCHAR Access, UCHAR Flags)
{
    QZ_GDT_ENTRY *e = &QzGdt[Selector / 8];
    e->LimitLow = (USHORT)(Limit & 0xFFFF);
    e->BaseLow = (USHORT)(Base & 0xFFFF);
    e->BaseMid = (UCHAR)((Base >> 16) & 0xFF);
    e->Access = Access;
    e->Granularity = (UCHAR)((Flags & 0xF0) | ((Limit >> 16) & 0x0F));
    e->BaseHigh = (UCHAR)((Base >> 24) & 0xFF);
}

static VOID QzCpuid(ULONG Leaf, ULONG Regs[4]);
BOOLEAN QzFxsrAvailable, QzSysenterAvailable;

VOID
QzSetIdtGate(ULONG Vector, PVOID Handler)
{
    ULONG Offset = (ULONG)(ULONG_PTR)Handler;
    QzIdt[Vector].OffsetLow = (USHORT)(Offset & 0xFFFF);
    QzIdt[Vector].Selector = QZ_SEL_R0_CODE;
    QzIdt[Vector].Zero = 0;
    QzIdt[Vector].TypeAttr = 0x8E;   /* present, DPL0, 32-bit interrupt gate */
    QzIdt[Vector].OffsetHigh = (USHORT)(Offset >> 16);
}

VOID
QzCpuInit(VOID)
{
    QZ_DESCRIPTOR Gdtr, Idtr;

    /* Flat code/data for both rings, as NT lays them out */
    QzSetGdtEntry(QZ_SEL_R0_CODE, 0, 0xFFFFF, 0x9A, 0xC0);
    QzSetGdtEntry(QZ_SEL_R0_DATA, 0, 0xFFFFF, 0x92, 0xC0);
    QzSetGdtEntry(QZ_SEL_R3_CODE, 0, 0xFFFFF, 0xFA, 0xC0);
    QzSetGdtEntry(QZ_SEL_R3_DATA, 0, 0xFFFFF, 0xF2, 0xC0);
    QzSetGdtEntry(KGDT_R3_TEB, 0, 0xFFF, 0xF2, 0x40);   /* FS in ring 3: the TEB */

    /* TSS: needed so that future ring-3 -> ring-0 transitions find a stack */
    RtlZeroMemory(&QzTss, sizeof(QzTss));
    QzTss.Ss0 = QZ_SEL_R0_DATA;
    QzTss.Esp0 = (ULONG)(ULONG_PTR)&QzTssStack[sizeof(QzTssStack)];
    QzTss.IoMapBase = sizeof(QzTss);
    QzSetGdtEntry(QZ_SEL_TSS, (ULONG)(ULONG_PTR)&QzTss, sizeof(QzTss) - 1, 0x89, 0x00);

    /* PCR: FS points at the processor control region, like on NT */
    RtlZeroMemory(&QzPcr, sizeof(QzPcr));
    QzPcr.NtTib.ExceptionList = EXCEPTION_CHAIN_END;
    QzPcr.NtTib.Self = NULL;            /* the current TEB: none for system threads */
    QzPcr.SelfPcr = (PKPCR)&QzPcr;
    QzPcr.Prcb = &QzPcr.PrcbData;
    QzPcr.Irql = HIGH_LEVEL;              /* interrupts stay off until init is done */
    QzPcr.MajorVersion = 1;
    QzPcr.MinorVersion = 1;
    QzPcr.SetMember = 1;
    QzPcr.Number = 0;
    QzPcr.TSS = (struct _KTSS *)&QzTss;
    QzPcr.GDT = (PKGDTENTRY)QzGdt;
    QzPcr.IDT = (PKIDTENTRY)QzIdt;

    /* The HAL refuses to run on a kernel with a different PRCB/build type */
    QzPcr.PrcbData.MajorVersion = PRCB_MAJOR_VERSION;
    QzPcr.PrcbData.MinorVersion = PRCB_MINOR_VERSION;
    QzPcr.PrcbData.BuildType = PRCB_BUILD_UNIPROCESSOR;
#if DBG
    QzPcr.PrcbData.BuildType |= PRCB_BUILD_DEBUG;
#endif
    QzPcr.PrcbData.Number = 0;
    QzPcr.PrcbData.SetMember = 1;
    QzPcr.PrcbData.CpuID = 1;             /* CPUID is available */

    RtlZeroMemory(&QzPrcbExt, sizeof(QzPrcbExt));
    InitializeListHead(&QzPrcbExt.DpcListHead);
    QzSetGdtEntry(QZ_SEL_R0_PCR, KIP0PCRADDRESS, sizeof(KIPCR) - 1, 0x92, 0x40);

    Gdtr.Limit = sizeof(QzGdt) - 1;
    Gdtr.Base = (ULONG)(ULONG_PTR)QzGdt;
    __asm__ __volatile__(
        "lgdt %0\n\t"
        "ljmp %1, $1f\n"
        "1:\n\t"
        "movw %w2, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %w3, %%ax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %w4, %%ax\n\t"
        "ltr %%ax\n\t"
        :
        : "m"(Gdtr), "i"(QZ_SEL_R0_CODE), "i"(QZ_SEL_R0_DATA),
          "i"(QZ_SEL_R0_PCR), "i"(QZ_SEL_TSS)
        : "eax", "memory");

    /* Double faults (a kernel stack overflow, mostly) switch to a task of
       their own, with a fresh stack, so they can still be reported */
    {
        extern VOID QzDoubleFaultTask(VOID);
        RtlZeroMemory(&QzDoubleFaultTss, sizeof(QzDoubleFaultTss));
        QzDoubleFaultTss.Esp0 = QzDoubleFaultTss.Esp =
            (ULONG)(ULONG_PTR)&QzDoubleFaultStack[sizeof(QzDoubleFaultStack)];
        QzDoubleFaultTss.Ss0 = QzDoubleFaultTss.Ss = QZ_SEL_R0_DATA;
        QzDoubleFaultTss.Eip = (ULONG)(ULONG_PTR)QzDoubleFaultTask;
        QzDoubleFaultTss.EFlags = 0x2;
        QzDoubleFaultTss.Cs = QZ_SEL_R0_CODE;
        QzDoubleFaultTss.Ds = QzDoubleFaultTss.Es = QzDoubleFaultTss.Gs = QZ_SEL_R3_DATA | 3;
        QzDoubleFaultTss.Fs = QZ_SEL_R0_PCR;
        QzDoubleFaultTss.Cr3 = QzReadCr3();
        QzDoubleFaultTss.IoMapBase = sizeof(QZ_TSS);
        QzSetGdtEntry(KGDT_DF_TSS, (ULONG)(ULONG_PTR)&QzDoubleFaultTss, sizeof(QZ_TSS) - 1, 0x89, 0x00);
    }

    /* IDT: fill every vector with a stub, then load it. Ring 3 may raise
       int3, into, and the system interrupts 2A-2E */
    RtlZeroMemory(QzIdt, sizeof(QzIdt));
    QzInstallTrapStubs();
    QzIdt[0x08].OffsetLow = QzIdt[0x08].OffsetHigh = 0;   /* a task gate */
    QzIdt[0x08].Selector = KGDT_DF_TSS;
    QzIdt[0x08].TypeAttr = 0x85;
    QzIdt[0x03].TypeAttr = 0xEE;
    QzIdt[0x04].TypeAttr = 0xEE;
    QzIdt[0x2A].TypeAttr = 0xEE;
    QzIdt[0x2B].TypeAttr = 0xEE;
    QzIdt[0x2C].TypeAttr = 0xEE;
    QzIdt[0x2D].TypeAttr = 0xEE;
    QzIdt[0x2E].TypeAttr = 0xEE;
    Idtr.Limit = sizeof(QzIdt) - 1;
    Idtr.Base = (ULONG)(ULONG_PTR)QzIdt;
    __asm__ __volatile__("lidt %0" : : "m"(Idtr) : "memory");

    /* sysenter: ring 3 enters at QzFastCallEntry, which takes the stack
       from TSS.Esp0 like an interrupt from ring 3 would */
    {
        ULONG Regs[4];
        QzCpuid(1, Regs);
        if (Regs[3] & (1 << 11))
        {
            extern UCHAR QzFastCallEntry[];
            __asm__ __volatile__("wrmsr" : : "c"(0x174), "a"(QZ_SEL_R0_CODE), "d"(0));
            __asm__ __volatile__("wrmsr" : : "c"(0x175), "a"((ULONG)(ULONG_PTR)&QzTssStack[sizeof(QzTssStack)]), "d"(0));
            __asm__ __volatile__("wrmsr" : : "c"(0x176), "a"((ULONG)(ULONG_PTR)QzFastCallEntry), "d"(0));
            QzSysenterAvailable = TRUE;
        }
    }

    /* The FPU for user threads: native errors, no emulation, FXSAVE and
       SSE if the processor has them (the kernel itself uses neither) */
    {
        ULONG Regs[4], Cr0, Cr4;
        QzCpuid(1, Regs);
        __asm__ __volatile__("movl %%cr0, %0" : "=r"(Cr0));
        /* clear EM, TS; set MP, NE, and WP: the kernel's writes honour
           read-only user pages, which copy-on-write relies on */
        Cr0 = (Cr0 & ~(0x04 | 0x08)) | 0x02 | 0x20 | 0x10000;
        __asm__ __volatile__("movl %0, %%cr0" : : "r"(Cr0));
        __asm__ __volatile__("fninit");
        if (Regs[3] & (1 << 24))
        {
            __asm__ __volatile__("movl %%cr4, %0" : "=r"(Cr4));
            Cr4 |= 0x200;                               /* OSFXSR */
            QzFxsrAvailable = TRUE;
            if (Regs[3] & (1 << 25))
                Cr4 |= 0x400;                           /* OSXMMEXCPT */
            __asm__ __volatile__("movl %0, %%cr4" : : "r"(Cr4));
        }
    }
}

/* The TEB of the thread about to run: FS's base in ring 3 */
VOID
QzSetTebSelector(PVOID Teb)
{
    QZ_GDT_ENTRY *e = &QzGdt[KGDT_R3_TEB / 8];
    ULONG Base = (ULONG)(ULONG_PTR)Teb;
    e->BaseLow = (USHORT)(Base & 0xFFFF);
    e->BaseMid = (UCHAR)((Base >> 16) & 0xFF);
    e->BaseHigh = (UCHAR)((Base >> 24) & 0xFF);
    e->Access = 0xF2;   /* reloading a selector needs it marked present, not busy */
}

static VOID
QzCpuid(ULONG Leaf, ULONG Regs[4])
{
    __asm__ __volatile__("cpuid"
                         : "=a"(Regs[0]), "=b"(Regs[1]), "=c"(Regs[2]), "=d"(Regs[3])
                         : "a"(Leaf), "c"(0));
}

VOID
QzCpuGetVendor(PCHAR Vendor13, PCHAR Brand49)
{
    ULONG r[4], i;

    QzCpuid(0, r);
    RtlCopyMemory(Vendor13 + 0, &r[1], 4);
    RtlCopyMemory(Vendor13 + 4, &r[3], 4);
    RtlCopyMemory(Vendor13 + 8, &r[2], 4);
    Vendor13[12] = 0;

    Brand49[0] = 0;
    QzCpuid(0x80000000, r);
    if (r[0] >= 0x80000004)
    {
        for (i = 0; i < 3; i++)
        {
            QzCpuid(0x80000002 + i, r);
            RtlCopyMemory(Brand49 + i * 16, r, 16);
        }
        Brand49[48] = 0;
    }
}

/*
 * The double-fault task: the interrupted context is in QzTss (the CPU saved
 * it there on the task switch). Show where it was, the stack's return
 * addresses (a recursion shows as a repeating pattern) and stop.
 */
DECLSPEC_NORETURN VOID
QzDoubleFaultTask(VOID)
{
    PULONG Stack = (PULONG)(ULONG_PTR)QzTss.Esp;
    ULONG i, Shown = 0;
    CHAR Text[96];

    DbgPrint("\n*** qzkrnl: double fault: EIP %08lx ESP %08lx EBP %08lx CR3 %08lx (thread %p)\n",
             QzTss.Eip, QzTss.Esp, QzTss.Ebp, QzTss.Cr3, QzCurrentThread);
    /* The stack above the fault (the faulting page itself is gone) */
    Stack = (PULONG)(((ULONG_PTR)Stack + 0xFFF) & ~0xFFFUL);
    for (i = 0; i < 4096 && Shown < 48; i++)
    {
        ULONG Value;
        if (!MmIsAddressValid(&Stack[i]))
            break;
        Value = Stack[i];
        if (Value >= 0x80400000 && Value < 0x81000000)
        {
            DbgPrint("  [%08lx] %08lx\n", (ULONG)(ULONG_PTR)&Stack[i], Value);
            Shown++;
        }
    }
    QzSnprintf(Text, sizeof(Text), "Double fault (kernel stack overflow?) at EIP %08lx, ESP %08lx",
               QzTss.Eip, QzTss.Esp);
    QzBugCheckWithText(UNEXPECTED_KERNEL_MODE_TRAP, 8, QzTss.Eip, QzTss.Esp, 0, Text);
}
