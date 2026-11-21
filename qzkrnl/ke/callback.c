/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     User-mode callbacks: KeUserModeCallback and NtCallbackReturn
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * win32k calls into user32 (window procedures, hooks, ClientThreadSetup)
 * in the middle of a system call. KeUserModeCallback copies the argument
 * block onto the user stack, below what the thread was using, and enters
 * ring 3 at ntdll's KiUserCallbackDispatcher, which calls entry
 * RoutineIndex of PEB->KernelCallbackTable and ends with NtCallbackReturn.
 *
 * The kernel stack of the waiting call stays where it is. The callback
 * runs on the stack below it: the thread's InitialStack (TSS.Esp0) moves
 * down there, so the callback's own system calls build their trap frames
 * under the waiting frames, and callbacks nest the same way. NtCallbackReturn
 * puts InitialStack back and resumes the waiting KeUserModeCallback on its
 * saved stack, abandoning the frames below.
 *
 * The user stack: KiUserCallbackDispatcher expects, at ESP, a dummy return
 * address, RoutineIndex, the argument pointer, its length, and two free
 * slots it uses for an SEH registration; the arguments follow.
 */

#include "qzio.h"
#include "qzps.h"

#define QZ_CALLBACK_MARGIN      512             /* between the waiting frames and the callback's */
#define QZ_CALLBACK_STACK_MIN   (12 * 1024)     /* refuse to nest below this much kernel stack */

typedef struct _QZ_CALLBACK_FRAME
{
    ULONG_PTR KernelEsp;                /* must be first: the assembly stores it */
    struct _QZ_CALLBACK_FRAME *Previous;
    ULONG_PTR InitialStack;             /* the thread's, to put back */
    PKTRAP_FRAME TrapFrame;
    KPROCESSOR_MODE PreviousMode;
    ULONG_PTR FaultRecovery;
    ULONG_PTR SyscallRecovery;
    ULONG_PTR SyscallEsp;
    PVOID SyscallExceptionList;
    PVOID Result;                       /* from NtCallbackReturn */
    ULONG ResultLength;
} QZ_CALLBACK_FRAME, *PQZ_CALLBACK_FRAME;

C_ASSERT(FIELD_OFFSET(QZ_CALLBACK_FRAME, KernelEsp) == 0);

NTSTATUS FASTCALL QzCallUserMode(PQZ_CALLBACK_FRAME Frame, PKTRAP_FRAME NewFrame);
DECLSPEC_NORETURN VOID FASTCALL QzCallbackResume(ULONG_PTR KernelEsp, NTSTATUS Status);

__asm__(
    ".text\n"
    /* ECX = frame, EDX = the callback's trap frame (interrupts off) */
    ".globl @QzCallUserMode@8\n"
    "@QzCallUserMode@8:\n"
    "    pushl %ebp\n"
    "    pushl %ebx\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    pushl %fs:0\n"
    "    movl %esp, (%ecx)\n"
    "    movl %edx, %esp\n"
    "    movl %edx, %ecx\n"
    "    jmp @KiEoiHelper@4\n"                  /* to ring 3 */
    "\n"
    /* ECX = the waiting call's stack, EDX = its result (interrupts off) */
    ".globl @QzCallbackResume@8\n"
    "@QzCallbackResume@8:\n"
    "    movl %ecx, %esp\n"
    "    popl %fs:0\n"
    "    popl %edi\n"
    "    popl %esi\n"
    "    popl %ebx\n"
    "    popl %ebp\n"
    "    movl %edx, %eax\n"
    "    sti\n"
    "    ret\n"
);

static ULONG QzCallbackCount;

NTSTATUS
NTAPI
KeUserModeCallback(IN ULONG RoutineIndex, IN PVOID Argument, IN ULONG ArgumentLength, OUT PVOID *Result,
                   OUT PULONG ResultLength)
{
    PQZ_THREAD Thread = QzCurrentThread;
    PQZ_PS_THREAD PsThread = Thread->PsThread;
    ULONG_PTR Dispatcher = QzPsUserCallbackDispatcher();
    PKTRAP_FRAME UserFrame = Thread->TrapFrame, NewFrame;
    QZ_CALLBACK_FRAME Frame;
    ULONG_PTR UserStack, Esp, NewInitial;
    ULONG Header[4];
    PVOID ExceptionList = NULL;
    PTEB Teb;
    NTSTATUS Status;

    *Result = NULL;
    *ResultLength = 0;
    if (!PsThread || !PsThread->UserThread || !PsThread->Teb || !UserFrame || !Dispatcher ||
        !(UserFrame->SegCs & 1) || QzGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_NOT_SUPPORTED;
    Teb = PsThread->Teb;

    __asm__ __volatile__("movl %%esp, %0" : "=r"(Esp));
    if (Esp - (ULONG_PTR)Thread->StackBase < QZ_CALLBACK_STACK_MIN)
    {
        DbgPrint(QZ_KERNEL_NAME ": [%s] KeUserModeCallback(%lu): kernel stack exhausted\n", Thread->Name,
                 RoutineIndex);
        return STATUS_STACK_OVERFLOW;
    }

    /* The arguments, then the dispatcher's frame, below the user's stack */
    UserStack = (UserFrame->HardwareEsp - ArgumentLength) & ~3UL;
    if (ArgumentLength && !QzCopyToUser((PVOID)UserStack, Argument, ArgumentLength))
        return STATUS_ACCESS_VIOLATION;
    UserStack -= 6 * sizeof(ULONG);
    Header[0] = 0;
    Header[1] = RoutineIndex;
    Header[2] = UserStack + 6 * sizeof(ULONG);
    Header[3] = ArgumentLength;
    if (!QzCopyToUser((PVOID)UserStack, Header, sizeof(Header)))
        return STATUS_ACCESS_VIOLATION;
    QzCopyFromUser(&ExceptionList, &Teb->NtTib.ExceptionList, sizeof(ExceptionList));

    /* What the callback's system calls would overwrite */
    Frame.Previous = Thread->CallbackFrame;
    Frame.InitialStack = Thread->InitialStack;
    Frame.TrapFrame = UserFrame;
    Frame.PreviousMode = Thread->PreviousMode;
    Frame.FaultRecovery = Thread->FaultRecovery;
    Frame.SyscallRecovery = Thread->SyscallRecovery;
    Frame.SyscallEsp = Thread->SyscallEsp;
    Frame.SyscallExceptionList = Thread->SyscallExceptionList;
    Frame.Result = NULL;
    Frame.ResultLength = 0;
    QzCallbackCount++;

    /*
     * The callback's stack starts QZ_CALLBACK_MARGIN below this frame:
     * with interrupts off nothing else uses the stack under ESP, and this
     * function and QzCallUserMode push far less than the margin.
     */
    _disable();
    __asm__ __volatile__("movl %%esp, %0" : "=r"(Esp));
    NewInitial = (Esp - QZ_CALLBACK_MARGIN) & ~15UL;
    NewFrame = (PKTRAP_FRAME)(NewInitial - sizeof(KTRAP_FRAME));
    RtlCopyMemory(NewFrame, UserFrame, sizeof(KTRAP_FRAME));
    NewFrame->Eip = Dispatcher;
    NewFrame->HardwareEsp = UserStack;
    NewFrame->ExceptionList = EXCEPTION_CHAIN_END;
    NewFrame->DbgEip = Dispatcher;
    NewFrame->DbgEbp = 0;
    NewFrame->Ebp = 0;
    Thread->CallbackFrame = &Frame;
    Thread->InitialStack = NewInitial;
    ((PULONG)QzPcr.TSS)[1] = (ULONG)NewInitial - 0x10;
    Thread->TrapFrame = NewFrame;
    Thread->FaultRecovery = 0;
    Thread->SyscallRecovery = 0;

    Status = QzCallUserMode(&Frame, NewFrame);     /* back here from NtCallbackReturn */

    /* The callback ran in ring 3 with its own SEH chain; ours again */
    if (Status != STATUS_CALLBACK_POP_STACK)
        QzCopyToUser(&Teb->NtTib.ExceptionList, &ExceptionList, sizeof(ExceptionList));
    *Result = Frame.Result;
    *ResultLength = Frame.ResultLength;
    QzPsWin32FlushBatch();
    return Status;
}

NTSTATUS
NTAPI
NtCallbackReturn(IN PVOID Result OPTIONAL, IN ULONG ResultLength, IN NTSTATUS CallbackStatus)
{
    PQZ_THREAD Thread = QzCurrentThread;
    PQZ_CALLBACK_FRAME Frame = Thread->CallbackFrame;

    if (!Frame)
        return STATUS_NO_CALLBACK_ACTIVE;
    Frame->Result = Result;
    Frame->ResultLength = ResultLength;

    _disable();
    Thread->CallbackFrame = Frame->Previous;
    Thread->InitialStack = Frame->InitialStack;
    ((PULONG)QzPcr.TSS)[1] = (ULONG)Frame->InitialStack - 0x10;
    Thread->TrapFrame = Frame->TrapFrame;
    Thread->PreviousMode = Frame->PreviousMode;
    Thread->FaultRecovery = Frame->FaultRecovery;
    Thread->SyscallRecovery = Frame->SyscallRecovery;
    Thread->SyscallEsp = Frame->SyscallEsp;
    Thread->SyscallExceptionList = Frame->SyscallExceptionList;
    Thread->TrapFrameReplaced = FALSE;
    QzCallbackResume(Frame->KernelEsp, CallbackStatus);
}

ULONG
QzKeCallbackCount(VOID)
{
    return QzCallbackCount;
}
