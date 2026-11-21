/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     User mode: user threads, entering ring 3, exceptions raised
 *              there, the way back to it
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * A user thread is a kernel thread whose stack top holds a KTRAP_FRAME for
 * ring 3 (TSS.Esp0 points just above it). It starts in the kernel, builds
 * that frame from its initial CONTEXT and leaves through KiEoiHelper, the
 * exit every trap and interrupt takes. From then on every entry from ring
 * 3 (int 2Eh, an exception, an interrupt) lands on the same frame.
 */

#include "qzio.h"
#include "qzps.h"

#define QZ_FX_TAG 'xFzQ'

VOID FASTCALL KiEoiHelper(PKTRAP_FRAME TrapFrame);

/* ---------------------------------------------------------------------- */
/* CONTEXT <-> KTRAP_FRAME                                                 */
/* ---------------------------------------------------------------------- */

VOID
QzContextToTrapFrame(PCONTEXT Context, PKTRAP_FRAME TrapFrame, ULONG Flags)
{
    if (Flags & CONTEXT_CONTROL & 0xFFFF)
    {
        TrapFrame->Eip = Context->Eip;
        TrapFrame->HardwareEsp = Context->Esp;
        TrapFrame->Ebp = Context->Ebp;
        /* Ring 3, interrupts on, IOPL 0, no matter what the context says */
        TrapFrame->EFlags = (Context->EFlags & 0x003F0DD5) | EFLAGS_INTERRUPT_MASK | 0x2;
        TrapFrame->SegCs = KGDT_R3_CODE | RPL_MASK;
        TrapFrame->HardwareSegSs = KGDT_R3_DATA | RPL_MASK;
    }
    if (Flags & CONTEXT_INTEGER & 0xFFFF)
    {
        TrapFrame->Eax = Context->Eax;
        TrapFrame->Ebx = Context->Ebx;
        TrapFrame->Ecx = Context->Ecx;
        TrapFrame->Edx = Context->Edx;
        TrapFrame->Esi = Context->Esi;
        TrapFrame->Edi = Context->Edi;
    }
    if (Flags & CONTEXT_SEGMENTS & 0xFFFF)
    {
        TrapFrame->SegDs = KGDT_R3_DATA | RPL_MASK;
        TrapFrame->SegEs = KGDT_R3_DATA | RPL_MASK;
        TrapFrame->SegFs = KGDT_R3_TEB | RPL_MASK;
        TrapFrame->SegGs = 0;
    }
}

VOID
QzTrapFrameToContext(PKTRAP_FRAME TrapFrame, PCONTEXT Context)
{
    Context->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS;
    Context->Eip = TrapFrame->Eip;
    Context->Esp = TrapFrame->HardwareEsp;
    Context->Ebp = TrapFrame->Ebp;
    Context->EFlags = TrapFrame->EFlags;
    Context->SegCs = TrapFrame->SegCs & 0xFFFF;
    Context->SegSs = TrapFrame->HardwareSegSs & 0xFFFF;
    Context->Eax = TrapFrame->Eax;
    Context->Ebx = TrapFrame->Ebx;
    Context->Ecx = TrapFrame->Ecx;
    Context->Edx = TrapFrame->Edx;
    Context->Esi = TrapFrame->Esi;
    Context->Edi = TrapFrame->Edi;
    Context->SegDs = TrapFrame->SegDs & 0xFFFF;
    Context->SegEs = TrapFrame->SegEs & 0xFFFF;
    Context->SegFs = TrapFrame->SegFs & 0xFFFF;
    Context->SegGs = TrapFrame->SegGs & 0xFFFF;
}

/* ---------------------------------------------------------------------- */
/* User threads                                                            */
/* ---------------------------------------------------------------------- */

/* The user frame sits at the very top of the kernel stack */
static PKTRAP_FRAME
QzUserTrapFrame(PQZ_THREAD Thread)
{
    return (PKTRAP_FRAME)(Thread->InitialStack - sizeof(KTRAP_FRAME));
}

VOID QzInitialUserApc(PQZ_PS_THREAD PsThread, PKTRAP_FRAME TrapFrame);   /* ps/create.c */

static VOID NTAPI
QzUserThreadStartup(PVOID Context)
{
    PQZ_PS_THREAD PsThread = Context;
    PQZ_THREAD Thread = QzCurrentThread;
    PKTRAP_FRAME TrapFrame = QzUserTrapFrame(Thread);

    Thread->PsThread = PsThread;
    RtlZeroMemory(TrapFrame, sizeof(*TrapFrame));
    QzContextToTrapFrame(PsThread->InitialContext, TrapFrame,
                         CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS);
    TrapFrame->ExceptionList = EXCEPTION_CHAIN_END;
    TrapFrame->DbgEip = TrapFrame->Eip;
    Thread->TrapFrame = TrapFrame;
    ExFreePool(PsThread->InitialContext);
    PsThread->InitialContext = NULL;

    /* On NT a new thread first runs ntdll's LdrInitializeThunk as a user APC */
    QzInitialUserApc(PsThread, TrapFrame);

    _disable();
    QzSetTebSelector(PsThread->Teb);
    KiEoiHelper(TrapFrame);   /* to ring 3; does not return */
}

extern BOOLEAN QzFxsrAvailable;

static PVOID
QzAllocateFxArea(VOID)
{
    PUCHAR Raw, Area;

    if (!QzFxsrAvailable)
        return NULL;   /* no FXSAVE: user threads share the FPU (no SSE either) */
    Raw = ExAllocatePoolWithTag(NonPagedPool, 512 + 16 + sizeof(PVOID), QZ_FX_TAG);
    if (!Raw)
        return NULL;
    Area = (PUCHAR)(((ULONG_PTR)Raw + sizeof(PVOID) + 15) & ~15UL);
    ((PVOID *)Area)[-1] = Raw;
    RtlZeroMemory(Area, 512);
    *(PUSHORT)(Area + 0) = 0x027F;    /* FCW: all exceptions masked, 53-bit precision */
    *(PULONG)(Area + 24) = 0x1F80;    /* MXCSR: all exceptions masked */
    return Area;
}

/* A thread of Process that starts in ring 3 with Context */
NTSTATUS
QzPsCreateUserThread(PQZ_PROCESS Process, PCONTEXT Context, PVOID Teb, BOOLEAN Suspended,
                     PQZ_PS_THREAD *Result)
{
    PQZ_PS_THREAD PsThread;
    PQZ_THREAD Thread;
    NTSTATUS Status;

    *Result = NULL;
    if (Process == QzSystemProcess || Process->Exiting)
        return STATUS_PROCESS_IS_TERMINATING;
    Status = ObCreateObject(KernelMode, PsThreadType, NULL, KernelMode, NULL, sizeof(QZ_PS_THREAD), 0, 0,
                            (PVOID *)&PsThread);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlZeroMemory(PsThread, sizeof(*PsThread));
    KeInitializeEvent(&PsThread->Terminated, NotificationEvent, FALSE);
    KeInitializeSemaphore(&PsThread->SuspendSemaphore, 0, MAXLONG);
    PsThread->ExitStatus = STATUS_PENDING;
    PsThread->Process = Process;
    ObReferenceObject(Process);
    PsThread->UserThread = TRUE;
    PsThread->Teb = Teb;
    PsThread->Win32StartAddress = (PVOID)Context->Eip;
    KeQuerySystemTime(&PsThread->CreateTime);
    PsThread->UniqueId = QzPsAllocateId(PsThread);
    PsThread->InitialContext = ExAllocatePoolWithTag(NonPagedPool, sizeof(CONTEXT), QZ_FX_TAG);
    if (!PsThread->InitialContext)
    {
        ObDereferenceObject(PsThread);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *PsThread->InitialContext = *Context;

    Thread = QzCreateThreadEx(Process->ImageFileName, QzUserThreadStartup, PsThread, Process, FALSE);
    if (!Thread)
    {
        ObDereferenceObject(PsThread);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Thread->FxSaveArea = QzAllocateFxArea();
    Thread->PsThread = PsThread;
    PsThread->Thread = Thread;

    /* The thread holds a reference on itself until it terminates */
    ObReferenceObject(PsThread);
    ExAcquireFastMutex(&Process->AddressLock);
    InsertTailList(&Process->ThreadList, &PsThread->ProcessLink);
    ExReleaseFastMutex(&Process->AddressLock);
    InterlockedIncrement((PLONG)&Process->ActiveThreads);

    if (Suspended)
        PsThread->SuspendCount = 1;
    else
        QzReadyNewThread(Thread);
    *Result = PsThread;   /* the creation reference is the caller's */
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Exceptions in ring 3                                                    */
/* ---------------------------------------------------------------------- */

BOOLEAN QzDispatchUserException(PKTRAP_FRAME TrapFrame, PEXCEPTION_RECORD Record);  /* ps/create.c */

/* From the trap handler (interrupts off) */
VOID
QzUserException(PKTRAP_FRAME TrapFrame, NTSTATUS Code, ULONG_PTR Address, ULONG_PTR Write)
{
    EXCEPTION_RECORD Record;
    PQZ_PS_THREAD PsThread = QzCurrentThread->PsThread;

    _enable();
    RtlZeroMemory(&Record, sizeof(Record));
    Record.ExceptionCode = Code;
    Record.ExceptionAddress = (PVOID)TrapFrame->Eip;
    if (Code == STATUS_ACCESS_VIOLATION || Code == STATUS_GUARD_PAGE_VIOLATION)
    {
        Record.NumberParameters = 2;
        Record.ExceptionInformation[0] = Write;
        Record.ExceptionInformation[1] = Address;
    }

    /* Into the process's own handlers (ntdll's KiUserExceptionDispatcher) */
    if (QzDispatchUserException(TrapFrame, &Record))
    {
        _disable();
        return;
    }

    DbgPrint(QZ_KERNEL_NAME ": unhandled user-mode exception 0x%08lx at %08lx in %s (pid %lu)\n",
             Code, TrapFrame->Eip, PsThread && PsThread->Process ? PsThread->Process->ImageFileName : "?",
             PsThread && PsThread->Process ? PsThread->Process->UniqueId : 0);
    if (Code == STATUS_ACCESS_VIOLATION)
        DbgPrint("  (%s of %08lx)\n", Write ? "write" : "read", Address);
    QzDumpTrapFrame(TrapFrame);
    if (!PsThread || !PsThread->UserThread)
        KeBugCheckEx(KMODE_EXCEPTION_NOT_HANDLED, (ULONG_PTR)Code, TrapFrame->Eip, Address, 1);
    QzPsTerminateProcess(PsThread->Process, Code);
    QzPsTerminateCurrentThread(Code);
}

/* Every return to ring 3 passes here, with the user trap frame (interrupts off) */
VOID
FASTCALL
QzExitToUserMode(PKTRAP_FRAME TrapFrame)
{
    PQZ_PS_THREAD PsThread = QzCurrentThread->PsThread;

    if (PsThread && PsThread->TerminatePending && QzGetCurrentIrql() == PASSIVE_LEVEL)
    {
        _enable();
        QzPsTerminateCurrentThread(PsThread->PendingExitStatus);
    }
    if (QzCurrentThread->UserApcPending && QzGetCurrentIrql() == PASSIVE_LEVEL)
        QzApcDeliverUser(TrapFrame);
}
