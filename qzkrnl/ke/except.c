/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-mode structured exception handling: dispatch and unwind
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * x86 SEH is frame based: FS:[0] heads a list of EXCEPTION_REGISTRATION_RECORDs
 * on the stack, newest first. Dispatching an exception calls each frame's
 * handler until one says what to do; PSEH's handlers run their filters,
 * then unwind the frames below them (RtlUnwind, which calls the same
 * handlers again with EXCEPTION_UNWINDING so that finally blocks run) and
 * jump into the except block themselves. File systems use this for their
 * ordinary error paths (ExRaiseStatus), so it has to work.
 */

#include "qz.h"

/*
 * Call a handler and survive both conventions: the stack pointer is put
 * back however many arguments the handler popped.
 */
EXCEPTION_DISPOSITION NTAPI QzExecuteHandler(PVOID Handler, PEXCEPTION_RECORD ExceptionRecord, PVOID Frame,
                                             PCONTEXT Context, PVOID DispatcherContext);
__asm__(
    ".globl _QzExecuteHandler@20\n"
    "_QzExecuteHandler@20:\n"
    "    pushl %ebp\n"
    "    movl %esp, %ebp\n"
    "    pushl %edi\n"
    "    pushl %esi\n"
    "    pushl %ebx\n"
    "    movl %esp, %edi\n"
    "    pushl 24(%ebp)\n"          /* DispatcherContext */
    "    pushl 20(%ebp)\n"          /* Context */
    "    pushl 16(%ebp)\n"          /* EstablisherFrame */
    "    pushl 12(%ebp)\n"          /* ExceptionRecord */
    "    call *8(%ebp)\n"
    "    movl %edi, %esp\n"
    "    popl %ebx\n"
    "    popl %esi\n"
    "    popl %edi\n"
    "    popl %ebp\n"
    "    ret $20\n");

/* A registration record that can be on this thread's kernel stack */
static BOOLEAN
QzIsFrameOnStack(PEXCEPTION_REGISTRATION_RECORD Frame)
{
    ULONG_PTR Address = (ULONG_PTR)Frame;
    PQZ_THREAD Thread = QzCurrentThread;

    if (Address & 3)
        return FALSE;
    if (!Thread || !Thread->StackBase || !Thread->StackSize)
        return Address >= 0x80000000UL;
    return Address >= (ULONG_PTR)Thread->StackBase &&
           Address + sizeof(*Frame) <= (ULONG_PTR)Thread->StackBase + Thread->StackSize;
}

static PEXCEPTION_REGISTRATION_RECORD
QzGetExceptionList(VOID)
{
    return (PEXCEPTION_REGISTRATION_RECORD)__readfsdword(0);
}

static VOID
QzSetExceptionList(PEXCEPTION_REGISTRATION_RECORD Frame)
{
    __writefsdword(0, (ULONG)(ULONG_PTR)Frame);
}

/* TRUE: a handler asked to continue (with Context as it left it) */
BOOLEAN
NTAPI
RtlDispatchException(IN PEXCEPTION_RECORD ExceptionRecord, IN PCONTEXT Context)
{
    PEXCEPTION_REGISTRATION_RECORD Frame = QzGetExceptionList();
    PVOID DispatcherContext = NULL;

    while (Frame != EXCEPTION_CHAIN_END && Frame)
    {
        EXCEPTION_DISPOSITION Disposition;

        if (!QzIsFrameOnStack(Frame))
        {
            ExceptionRecord->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            return FALSE;
        }
        Disposition = QzExecuteHandler(Frame->Handler, ExceptionRecord, Frame, Context, &DispatcherContext);
        switch (Disposition)
        {
            case ExceptionContinueExecution:
                if (ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE)
                {
                    DbgPrint(QZ_KERNEL_NAME ": a handler continued a noncontinuable exception 0x%08lx\n",
                             ExceptionRecord->ExceptionCode);
                    return FALSE;
                }
                return TRUE;
            case ExceptionContinueSearch:
            case ExceptionNestedException:
                break;
            default:
                DbgPrint(QZ_KERNEL_NAME ": exception handler %p returned %d\n", Frame->Handler, Disposition);
                return FALSE;
        }
        Frame = Frame->Next;
    }
    return FALSE;
}

/*
 * Unwind to TargetFrame (exclusive; NULL: every frame): each frame's
 * handler runs with EXCEPTION_UNWINDING, then the frame leaves the list.
 * PSEH passes no TargetIp and expects RtlUnwind to return.
 */
VOID
NTAPI
RtlUnwind(IN PVOID TargetFrame OPTIONAL, IN PVOID TargetIp OPTIONAL, IN PEXCEPTION_RECORD ExceptionRecord OPTIONAL,
          IN PVOID ReturnValue)
{
    EXCEPTION_RECORD Local;
    CONTEXT Context;
    PEXCEPTION_REGISTRATION_RECORD Frame = QzGetExceptionList();
    PVOID DispatcherContext = NULL;

    UNREFERENCED_PARAMETER(TargetIp);
    UNREFERENCED_PARAMETER(ReturnValue);
    if (!ExceptionRecord)
    {
        RtlZeroMemory(&Local, sizeof(Local));
        Local.ExceptionCode = STATUS_UNWIND;
        Local.ExceptionAddress = __builtin_return_address(0);
        ExceptionRecord = &Local;
    }
    ExceptionRecord->ExceptionFlags |= EXCEPTION_UNWINDING;
    if (!TargetFrame)
        ExceptionRecord->ExceptionFlags |= EXCEPTION_EXIT_UNWIND;
    RtlZeroMemory(&Context, sizeof(Context));
    Context.ContextFlags = CONTEXT_CONTROL;

    while (Frame != EXCEPTION_CHAIN_END && Frame && Frame != TargetFrame)
    {
        PEXCEPTION_REGISTRATION_RECORD Next = Frame->Next;
        if (!QzIsFrameOnStack(Frame))
            break;
        QzExecuteHandler(Frame->Handler, ExceptionRecord, Frame, &Context, &DispatcherContext);
        QzSetExceptionList(Next);
        Frame = Next;
    }
}

VOID
NTAPI
RtlCaptureContext(OUT PCONTEXT ContextRecord)
{
    ULONG_PTR Ebp = (ULONG_PTR)__builtin_frame_address(0);

    RtlZeroMemory(ContextRecord, sizeof(*ContextRecord));
    ContextRecord->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS;
    ContextRecord->Eip = (ULONG_PTR)__builtin_return_address(0);
    ContextRecord->Ebp = *(PULONG_PTR)Ebp;           /* the caller's */
    ContextRecord->Esp = Ebp + 2 * sizeof(ULONG_PTR);
    __asm__ __volatile__("movl %%ebx, %0\n\tmovl %%esi, %1\n\tmovl %%edi, %2"
                         : "=m"(ContextRecord->Ebx), "=m"(ContextRecord->Esi), "=m"(ContextRecord->Edi));
    ContextRecord->SegCs = KGDT_R0_CODE;
    ContextRecord->SegSs = ContextRecord->SegDs = ContextRecord->SegEs = KGDT_R3_DATA | RPL_MASK;
    ContextRecord->SegFs = KGDT_R0_PCR;
}
