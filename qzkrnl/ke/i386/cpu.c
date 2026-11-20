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

/* Minimal processor control region reachable through FS:[0] */
typedef struct _QZ_PCR
{
    struct _QZ_PCR *Self;
    ULONG Number;
    PVOID CurrentThread;
    ULONG Reserved[13];
} QZ_PCR;

static QZ_GDT_ENTRY QzGdt[16] __attribute__((aligned(16)));
static QZ_IDT_ENTRY QzIdt[256] __attribute__((aligned(16)));
static QZ_TSS QzTss __attribute__((aligned(16)));
static QZ_PCR QzPcr;
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

    /* TSS: needed so that future ring-3 -> ring-0 transitions find a stack */
    RtlZeroMemory(&QzTss, sizeof(QzTss));
    QzTss.Ss0 = QZ_SEL_R0_DATA;
    QzTss.Esp0 = (ULONG)(ULONG_PTR)&QzTssStack[sizeof(QzTssStack)];
    QzTss.IoMapBase = sizeof(QzTss);
    QzSetGdtEntry(QZ_SEL_TSS, (ULONG)(ULONG_PTR)&QzTss, sizeof(QzTss) - 1, 0x89, 0x00);

    /* PCR: FS points at the processor control region, like on NT */
    QzPcr.Self = &QzPcr;
    QzPcr.Number = 0;
    QzSetGdtEntry(QZ_SEL_R0_PCR, (ULONG)(ULONG_PTR)&QzPcr, sizeof(QzPcr) - 1, 0x92, 0x40);

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

    /* IDT: fill every vector with a stub, then load it */
    RtlZeroMemory(QzIdt, sizeof(QzIdt));
    QzInstallTrapStubs();
    Idtr.Limit = sizeof(QzIdt) - 1;
    Idtr.Base = (ULONG)(ULONG_PTR)QzIdt;
    __asm__ __volatile__("lidt %0" : : "m"(Idtr) : "memory");
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
