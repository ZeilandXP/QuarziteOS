/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     i386 trap/interrupt entry, KTRAP_FRAME handling, KiEoiHelper
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Every IDT vector gets a 32-byte stub. The common path builds a genuine NT
 * KTRAP_FRAME (the same layout the HAL's own entry code builds), because
 * the HAL ends interrupts by tail-jumping into its software-interrupt
 * handlers with the trap frame and leaves through the kernel's KiEoiHelper.
 * Every interrupt and trap therefore exits through KiEoiHelper below.
 *
 * Vectors the HAL takes over (the clock at 0x30, the RTC at 0x38) are
 * re-pointed by the HAL itself through PCR->IDT (KeRegisterInterruptHandler).
 */

#include "qz.h"
#include "qzps.h"

/* KTRAP_FRAME offsets, checked against the SDK definition */
#define TF_DBGEBP   0x00
#define TF_DBGEIP   0x04
#define TF_DR7      0x2C
#define TF_GS       0x30
#define TF_ES       0x34
#define TF_DS       0x38
#define TF_EDX      0x3C
#define TF_ECX      0x40
#define TF_EAX      0x44
#define TF_EXLIST   0x4C
#define TF_FS       0x50
#define TF_EDI      0x54
#define TF_ESI      0x58
#define TF_EBX      0x5C
#define TF_EBP      0x60
#define TF_ERRCODE  0x64
#define TF_EIP      0x68
#define TF_CS       0x6C
#define TF_ESP      0x74

C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, DbgEbp) == TF_DBGEBP);
C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, DbgEip) == TF_DBGEIP);
C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, Dr7) == TF_DR7);
C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, SegGs) == TF_GS);
C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, SegEs) == TF_ES);
C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, SegDs) == TF_DS);
C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, Edx) == TF_EDX);
C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, Ecx) == TF_ECX);
C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, Eax) == TF_EAX);
C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, ExceptionList) == TF_EXLIST);
C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, SegFs) == TF_FS);
C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, Edi) == TF_EDI);
C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, Esi) == TF_ESI);
C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, Ebx) == TF_EBX);
C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, Ebp) == TF_EBP);
C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, ErrCode) == TF_ERRCODE);
C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, Eip) == TF_EIP);
C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, SegCs) == TF_CS);
C_ASSERT(FIELD_OFFSET(KTRAP_FRAME, HardwareEsp) == TF_ESP);

#define STR2(x) #x
#define STR(x) STR2(x)

extern UCHAR QzTrapStubTable[];
VOID FASTCALL QzTrapDispatch(PKTRAP_FRAME TrapFrame, ULONG Vector);

__asm__(
    ".text\n"
    ".balign 32\n"
    ".globl _QzTrapStubTable\n"
    "_QzTrapStubTable:\n"
    ".set qzvec, 0\n"
    ".rept 256\n"
    "    .balign 32\n"
    "    .if (qzvec == 8) || ((qzvec >= 10) && (qzvec <= 14)) || (qzvec == 17) || (qzvec == 21) || (qzvec == 29) || (qzvec == 30)\n"
    "    .else\n"
    "        pushl $0\n"                         /* fake error code */
    "    .endif\n"
    "    subl $" STR(TF_ERRCODE) ", %esp\n"       /* rest of the KTRAP_FRAME */
    "    movl %eax, " STR(TF_EAX) "(%esp)\n"
    "    movl $qzvec, %eax\n"
    "    jmp QzTrapCommon\n"
    "    .set qzvec, qzvec + 1\n"
    ".endr\n"
    "\n"
    /* esp = KTRAP_FRAME, eax = vector */
    "QzTrapCommon:\n"
    "    movl %ebp, " STR(TF_EBP) "(%esp)\n"
    "    movl %ebx, " STR(TF_EBX) "(%esp)\n"
    "    movl %esi, " STR(TF_ESI) "(%esp)\n"
    "    movl %edi, " STR(TF_EDI) "(%esp)\n"
    "    movl %ecx, " STR(TF_ECX) "(%esp)\n"
    "    movl %edx, " STR(TF_EDX) "(%esp)\n"
    "    xorl %ecx, %ecx\n"
    "    movw %ds, %cx\n"
    "    movl %ecx, " STR(TF_DS) "(%esp)\n"
    "    movw %es, %cx\n"
    "    movl %ecx, " STR(TF_ES) "(%esp)\n"
    "    movw %fs, %cx\n"
    "    movl %ecx, " STR(TF_FS) "(%esp)\n"
    "    movw %gs, %cx\n"
    "    movl %ecx, " STR(TF_GS) "(%esp)\n"
    "    movw $0x23, %cx\n"                      /* KGDT_R3_DATA | RPL 3, as NT */
    "    movw %cx, %ds\n"
    "    movw %cx, %es\n"
    "    movw $0x30, %cx\n"                      /* KGDT_R0_PCR */
    "    movw %cx, %fs\n"
    "    movl %fs:0, %ecx\n"
    "    movl %ecx, " STR(TF_EXLIST) "(%esp)\n"
    "    movl $0, " STR(TF_DR7) "(%esp)\n"
    "    movl " STR(TF_EIP) "(%esp), %ecx\n"     /* keep the frame chain */
    "    movl %ecx, " STR(TF_DBGEIP) "(%esp)\n"
    "    movl %ebp, " STR(TF_DBGEBP) "(%esp)\n"
    "    movl %esp, %ebp\n"
    "    cld\n"
    "    movl %esp, %ecx\n"
    "    movl %eax, %edx\n"
    "    call @QzTrapDispatch@8\n"
    "    movl %ebp, %ecx\n"
    "    jmp @KiEoiHelper@4\n"
    "\n"
    /*
     * KiEoiHelper(TrapFrame): leave a kernel-mode trap frame. Exported:
     * the HAL ends its clock and software interrupts with it.
     */
    ".globl @KiEoiHelper@4\n"
    "@KiEoiHelper@4:\n"
    "    movl %ecx, %esp\n"
    "    movl " STR(TF_EXLIST) "(%esp), %eax\n"
    "    movl %eax, %fs:0\n"
    "    testb $1, " STR(TF_CS) "(%esp)\n"       /* back to ring 3? */
    "    jz 1f\n"
    "    movl %esp, %ecx\n"
    "    call @QzExitToUserMode@4\n"              /* pending work for the thread */
    "    movl " STR(TF_DS) "(%esp), %eax\n"
    "    movw %ax, %ds\n"
    "    movl " STR(TF_ES) "(%esp), %eax\n"
    "    movw %ax, %es\n"
    "    movl " STR(TF_FS) "(%esp), %eax\n"
    "    movw %ax, %fs\n"
    "    movl " STR(TF_GS) "(%esp), %eax\n"
    "    movw %ax, %gs\n"
    "1:\n"
    "    movl " STR(TF_EDX) "(%esp), %edx\n"
    "    movl " STR(TF_ECX) "(%esp), %ecx\n"
    "    movl " STR(TF_EAX) "(%esp), %eax\n"
    "    movl " STR(TF_EDI) "(%esp), %edi\n"
    "    movl " STR(TF_ESI) "(%esp), %esi\n"
    "    movl " STR(TF_EBX) "(%esp), %ebx\n"
    "    movl " STR(TF_EBP) "(%esp), %ebp\n"
    "    addl $" STR(TF_EIP) ", %esp\n"
    "    iretl\n"
);

VOID
QzInstallTrapStubs(VOID)
{
    ULONG Vector;
    for (Vector = 0; Vector < 256; Vector++)
        QzSetIdtGate(Vector, &QzTrapStubTable[Vector * 32]);
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

VOID
QzDumpTrapFrame(PKTRAP_FRAME TrapFrame)
{
    DbgPrint("  EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx\n",
             TrapFrame->Eax, TrapFrame->Ebx, TrapFrame->Ecx, TrapFrame->Edx);
    DbgPrint("  ESI=%08lx EDI=%08lx EBP=%08lx ESP=%08lx\n",
             TrapFrame->Esi, TrapFrame->Edi, TrapFrame->Ebp,
             (ULONG)(ULONG_PTR)&TrapFrame->HardwareEsp);
    DbgPrint("  EIP=%08lx CS=%04lx EFLAGS=%08lx ERR=%08lx CR2=%08lx CR3=%08lx IRQL=%u\n",
             TrapFrame->Eip, TrapFrame->SegCs, TrapFrame->EFlags, TrapFrame->ErrCode,
             QzReadCr2(), QzReadCr3(), QzPcr.Irql);
}

static DECLSPEC_NORETURN VOID
QzUnhandledException(PKTRAP_FRAME TrapFrame, ULONG Vector)
{
    CHAR Text[96];

    QzSnprintf(Text, sizeof(Text), "%s (vector %lu) at EIP %08lx",
               QzExceptionNames[Vector], Vector, TrapFrame->Eip);
    DbgPrint("\n*** qzkrnl: unhandled kernel exception: %s\n", Text);
    QzDumpTrapFrame(TrapFrame);

    switch (Vector)
    {
        case 14: /* #PF: faulting address, write?, EIP, error code */
            QzBugCheckWithText(PAGE_FAULT_IN_NONPAGED_AREA, QzReadCr2(),
                               (TrapFrame->ErrCode & 2) ? 1 : 0, TrapFrame->Eip,
                               TrapFrame->ErrCode, Text);
        case 8:
            QzBugCheckWithText(UNEXPECTED_KERNEL_MODE_TRAP, 8, 0, 0, 0, Text);
        case 0:
            QzBugCheckWithText(KMODE_EXCEPTION_NOT_HANDLED,
                               (ULONG_PTR)STATUS_INTEGER_DIVIDE_BY_ZERO,
                               TrapFrame->Eip, 0, 0, Text);
        case 6:
            QzBugCheckWithText(KMODE_EXCEPTION_NOT_HANDLED,
                               (ULONG_PTR)STATUS_ILLEGAL_INSTRUCTION,
                               TrapFrame->Eip, 0, 0, Text);
        case 13:
            QzBugCheckWithText(KMODE_EXCEPTION_NOT_HANDLED,
                               (ULONG_PTR)STATUS_ACCESS_VIOLATION,
                               TrapFrame->Eip, TrapFrame->ErrCode, 0, Text);
        default:
            QzBugCheckWithText(UNEXPECTED_KERNEL_MODE_TRAP, Vector,
                               TrapFrame->Eip, TrapFrame->ErrCode, 0, Text);
    }
}

/* Called by the common stub with interrupts disabled */
VOID
FASTCALL
QzTrapDispatch(PKTRAP_FRAME TrapFrame, ULONG Vector)
{
    if (Vector == 14)
    {
        /* Page fault: demand-zero and guard pages of the address space */
        ULONG_PTR Address = QzReadCr2();
        BOOLEAN FromUser = (TrapFrame->SegCs & 1) != 0;
        NTSTATUS Status = QzMmAccessFault(Address, TrapFrame->ErrCode, FromUser ? UserMode : KernelMode);

        if (NT_SUCCESS(Status))
            return;
        if (FromUser)
        {
            QzUserException(TrapFrame, Status, Address, (TrapFrame->ErrCode & 2) ? 1 : 0);
            return;
        }
        if (Status == STATUS_GUARD_PAGE_VIOLATION)
            return;   /* the kernel touched a guard page for the user: it is a page now */
        if (Address < QZ_SYSTEM_RANGE_START && QzCurrentThread && QzCurrentThread->FaultRecovery)
        {
            TrapFrame->Eip = QzCurrentThread->FaultRecovery;
            return;
        }
    }
    if (Vector < 32 && (TrapFrame->SegCs & 1))
    {
        /* Exceptions in user mode go to the thread, not the system */
        static const NTSTATUS Codes[20] =
        {
            STATUS_INTEGER_DIVIDE_BY_ZERO, STATUS_SINGLE_STEP, 0, STATUS_BREAKPOINT,
            STATUS_INTEGER_OVERFLOW, STATUS_ARRAY_BOUNDS_EXCEEDED, STATUS_ILLEGAL_INSTRUCTION,
            STATUS_ILLEGAL_INSTRUCTION, 0, 0, 0, STATUS_ACCESS_VIOLATION, STATUS_STACK_OVERFLOW,
            STATUS_ACCESS_VIOLATION, STATUS_ACCESS_VIOLATION, 0, STATUS_FLOAT_INVALID_OPERATION,
            STATUS_DATATYPE_MISALIGNMENT, 0, STATUS_FLOAT_MULTIPLE_TRAPS
        };
        NTSTATUS Code = Vector < 20 ? Codes[Vector] : 0;
        if (Code)
        {
            if (Vector == 3)
                TrapFrame->Eip--;   /* int3: the exception address is the instruction */
            QzUserException(TrapFrame, Code, Vector == 13 ? 0 : (ULONG_PTR)-1, 0);
            return;
        }
    }
    if (Vector < 32)
    {
        if (Vector == 3)
        {
            /* int3: report and continue, like a debugger-less breakpoint */
            DbgPrint("qzkrnl: breakpoint at %08lx (continuing)\n", TrapFrame->Eip - 1);
            return;
        }
        QzUnhandledException(TrapFrame, Vector);
    }

    /* Device interrupts (and HAL-replayed lazy interrupts) */
    if (Vector >= PRIMARY_VECTOR_BASE && Vector < PRIMARY_VECTOR_BASE + 16)
        QzDispatchDeviceInterrupt(TrapFrame, Vector);   /* does not return */

    DbgPrint("qzkrnl: unexpected interrupt 0x%02lx at %08lx\n", Vector, TrapFrame->Eip);
}

/* ---------------------------------------------------------------------- */
/* Exports used by the HAL                                                 */
/* ---------------------------------------------------------------------- */

/* Old (NT 5.0) HAL exit helper: not used by HALs built for this kernel */
VOID
NTAPI
Kei386EoiHelper(VOID)
{
    KeBugCheckEx(MISMATCHED_HAL, 0, 0, 0, 0);
}

/* The HAL found a software interrupt with no handler */
VOID
__cdecl
KiUnexpectedInterrupt(VOID)
{
    DbgPrint("qzkrnl: KiUnexpectedInterrupt from the HAL (IRR=%08lx)\n", QzPcr.IRR);
}
