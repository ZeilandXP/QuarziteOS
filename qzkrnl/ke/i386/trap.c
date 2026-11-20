/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     i386 trap/interrupt entry stubs and dispatcher
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Every IDT vector gets a 16-byte stub that normalises the stack (pushes a
 * dummy error code where the CPU does not supply one, then the vector
 * number) and jumps to a common path building a QZ_TRAP_FRAME. The C
 * dispatcher returns the frame to resume -- which is how the scheduler
 * switches threads: it simply returns another thread's saved frame.
 */

#include "qz.h"

extern UCHAR QzTrapStubTable[];

__asm__(
    ".text\n"
    ".balign 16\n"
    ".globl _QzTrapStubTable\n"
    "_QzTrapStubTable:\n"
    ".set qzvec, 0\n"
    ".rept 256\n"
    "    .balign 16\n"
    "    .if (qzvec == 8) || ((qzvec >= 10) && (qzvec <= 14)) || (qzvec == 17) || (qzvec == 21) || (qzvec == 29) || (qzvec == 30)\n"
    "    .else\n"
    "        pushl $0\n"
    "    .endif\n"
    "    pushl $qzvec\n"
    "    jmp QzTrapCommon\n"
    "    .set qzvec, qzvec + 1\n"
    ".endr\n"
    "\n"
    "QzTrapCommon:\n"
    "    pushl %ds\n"
    "    pushl %es\n"
    "    pushl %fs\n"
    "    pushl %gs\n"
    "    pushal\n"
    "    cld\n"
    "    movw $0x10, %ax\n"
    "    movw %ax, %ds\n"
    "    movw %ax, %es\n"
    "    movw $0x30, %ax\n"
    "    movw %ax, %fs\n"
    "    movl %esp, %eax\n"
    "    pushl %eax\n"
    "    call _QzTrapDispatch\n"
    "    movl %eax, %esp\n"          /* resume the (possibly different) frame */
    "    popal\n"
    "    popl %gs\n"
    "    popl %fs\n"
    "    popl %es\n"
    "    popl %ds\n"
    "    addl $8, %esp\n"            /* vector + error code */
    "    iretl\n"
);

VOID
QzInstallTrapStubs(VOID)
{
    ULONG Vector;
    for (Vector = 0; Vector < 256; Vector++)
        QzSetIdtGate(Vector, &QzTrapStubTable[Vector * 16]);
}

static PCSTR QzExceptionNames[32] =
{
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow", "BOUND Range",
    "Invalid Opcode", "Device Not Available", "Double Fault",
    "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
    "Stack Fault", "General Protection", "Page Fault", "Reserved",
    "x87 FPU Error", "Alignment Check", "Machine Check", "SIMD FP Exception",
    "Virtualization", "Control Protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "VMM Communication", "Security", "Reserved"
};

static VOID
QzDumpFrame(PQZ_TRAP_FRAME Frame)
{
    DbgPrint("  EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx\n",
             Frame->Eax, Frame->Ebx, Frame->Ecx, Frame->Edx);
    DbgPrint("  ESI=%08lx EDI=%08lx EBP=%08lx ESP=%08lx\n",
             Frame->Esi, Frame->Edi, Frame->Ebp,
             (ULONG)(ULONG_PTR)&Frame->EFlags + sizeof(ULONG));
    DbgPrint("  EIP=%08lx CS=%04lx EFLAGS=%08lx ERR=%08lx CR2=%08lx CR3=%08lx\n",
             Frame->Eip, Frame->Cs, Frame->EFlags, Frame->ErrorCode,
             QzReadCr2(), QzReadCr3());
}

static DECLSPEC_NORETURN VOID
QzUnhandledException(PQZ_TRAP_FRAME Frame)
{
    CHAR Text[96];
    ULONG Vector = Frame->Vector;

    QzSnprintf(Text, sizeof(Text), "%s (vector %lu) at EIP %08lx",
               QzExceptionNames[Vector], Vector, Frame->Eip);
    DbgPrint("\n*** qzkrnl: unhandled kernel exception: %s\n", Text);
    QzDumpFrame(Frame);

    switch (Vector)
    {
        case 14: /* #PF: faulting address, write?, EIP, error code */
            QzBugCheckWithText(PAGE_FAULT_IN_NONPAGED_AREA, QzReadCr2(),
                               (Frame->ErrorCode & 2) ? 1 : 0, Frame->Eip,
                               Frame->ErrorCode, Text);
        case 8:
            QzBugCheckWithText(UNEXPECTED_KERNEL_MODE_TRAP, 8, 0, 0, 0, Text);
        case 0:
            QzBugCheckWithText(KMODE_EXCEPTION_NOT_HANDLED,
                               (ULONG_PTR)STATUS_INTEGER_DIVIDE_BY_ZERO,
                               Frame->Eip, 0, 0, Text);
        case 6:
            QzBugCheckWithText(KMODE_EXCEPTION_NOT_HANDLED,
                               (ULONG_PTR)STATUS_ILLEGAL_INSTRUCTION,
                               Frame->Eip, 0, 0, Text);
        case 13:
            QzBugCheckWithText(KMODE_EXCEPTION_NOT_HANDLED,
                               (ULONG_PTR)STATUS_ACCESS_VIOLATION,
                               Frame->Eip, Frame->ErrorCode, 0, Text);
        default:
            QzBugCheckWithText(UNEXPECTED_KERNEL_MODE_TRAP, Vector,
                               Frame->Eip, Frame->ErrorCode, 0, Text);
    }
}

PQZ_TRAP_FRAME __cdecl
QzTrapDispatch(PQZ_TRAP_FRAME Frame)
{
    ULONG Vector = Frame->Vector;
    KIRQL OldIrql = QzPcr.Pcr.Irql;

    if (Vector < 32)
    {
        if (Vector == 3)
        {
            /* int3: report and continue, like a debugger-less breakpoint */
            DbgPrint("qzkrnl: breakpoint at %08lx (continuing)\n", Frame->Eip - 1);
            return Frame;
        }
        QzUnhandledException(Frame);
    }

    if (Vector >= QZ_VECTOR_IRQ_BASE && Vector < QZ_VECTOR_IRQ_BASE + 16)
    {
        ULONG Irq = Vector - QZ_VECTOR_IRQ_BASE;

        /* Spurious IRQ 7/15: the PIC's in-service bit is not set */
        if (Irq == 7 || Irq == 15)
        {
            QzOutB(Irq == 7 ? 0x20 : 0xA0, 0x0B);
            if (!(QzInB(Irq == 7 ? 0x20 : 0xA0) & 0x80))
            {
                if (Irq == 15) QzPicEoi(2);
                return Frame;
            }
        }

        /* Run the ISR at its device IRQL (interrupts stay disabled) */
        QzPcr.Pcr.Irql = (Irq == QZ_IRQ_TIMER) ? CLOCK2_LEVEL : (KIRQL)(PROFILE_LEVEL - 1 - Irq);
        switch (Irq)
        {
            case QZ_IRQ_TIMER:
            {
                extern VOID QzClockTick(VOID);
                QzClockTick();
                break;
            }
            case QZ_IRQ_KEYBOARD:
                QzKbdIsr();
                break;
            case QZ_IRQ_COM1:
                QzSerialIsr();
                break;
            default:
                break;
        }
        QzPicEoi(Irq);
        QzPcr.Pcr.Irql = OldIrql;

        /* Deliver the DISPATCH software interrupt if it can run now */
        if (OldIrql < DISPATCH_LEVEL)
            return QzDispatchSoftwareInterrupt(Frame, OldIrql);
        return Frame;
    }

    if (Vector == QZ_VECTOR_SWAP)
    {
        /* Voluntary switch: yield, or block in a wait (at DISPATCH_LEVEL) */
        Frame = QzSelectNextThread(Frame, OldIrql);
        if (QzPcr.Pcr.Irql < DISPATCH_LEVEL)
            Frame = QzDispatchSoftwareInterrupt(Frame, QzPcr.Pcr.Irql);
        return Frame;
    }

    DbgPrint("qzkrnl: unexpected interrupt 0x%02lx at %08lx\n", Vector, Frame->Eip);
    return Frame;
}
