/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     System services: int 2Eh, the service table, int 2Dh
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * ntdll's stubs load the service number into EAX and call through
 * SharedUserData->SystemCall: KiIntSystemCall points EDX at the arguments
 * on the user stack and executes int 2Eh. The dispatcher copies the
 * arguments (their count comes from the table) onto the kernel stack and
 * calls the service with PreviousMode = UserMode.
 *
 * Services are called with the user's pointers. A service that touches a
 * bad user address without using the fault-safe copies is aborted: the
 * #PF handler resumes at the dispatcher, which returns
 * STATUS_ACCESS_VIOLATION (in place of the SEH frame NT services have).
 */

#include "qzio.h"
#include "qzps.h"

typedef struct _QZ_SERVICE
{
    PVOID Handler;
    UCHAR Arguments;     /* DWORDs */
    UCHAR ViaZw;         /* the kernel-side Zw implementation */
    PCSTR Name;
} QZ_SERVICE;

/* The services' symbols, without prototypes (they would clash with the SDK's) */
#define QZ_SVC_IMPL(Name, Args, Symbol, Zw) extern VOID QzSvc_##Name(VOID) __asm__(Symbol);
#define QZ_SVC_NONE(Name, Args)
#include "systable.h"
#undef QZ_SVC_IMPL
#undef QZ_SVC_NONE

#define QZ_SVC_IMPL(Name, Args, Symbol, Zw) { (PVOID)QzSvc_##Name, Args, Zw, #Name },
#define QZ_SVC_NONE(Name, Args) { NULL, Args, 0, #Name },
static const QZ_SERVICE QzServiceTable[QZ_SERVICE_COUNT] =
{
#include "systable.h"
};
#undef QZ_SVC_IMPL
#undef QZ_SVC_NONE

BOOLEAN QzSyscallTrace;
static ULONG QzSyscallCount, QzSyscallUnimplemented;

/*
 * Push the arguments and call the stdcall service. A recovery point lets
 * the #PF handler abandon the service if it faults on a user address: it
 * resumes at "9:" with the stack pointer saved here.
 */
static NTSTATUS
QzCallService(PVOID Handler, PULONG Arguments, ULONG Count)
{
    NTSTATUS Status;
    __asm__ __volatile__(
        "movl %%fs:0x124, %%eax\n\t"
        "movl %%esp, %c[esp](%%eax)\n\t"
        "movl %%fs:0, %%ebx\n\t"
        "movl %%ebx, %c[exl](%%eax)\n\t"
        "movl $9f, %c[rec](%%eax)\n\t"
        "leal 0(,%%ecx,4), %%eax\n\t"
        "subl %%eax, %%esp\n\t"
        "movl %%esp, %%edi\n\t"
        "cld\n\t"
        "rep movsl\n\t"
        "call *%%edx\n\t"
        "jmp 8f\n"
        "9:\n\t"
        "movl %%fs:0x124, %%ecx\n\t"
        "movl %c[esp](%%ecx), %%esp\n\t"
        "movl %c[exl](%%ecx), %%ebx\n\t"
        "movl %%ebx, %%fs:0\n\t"
        "movl $0xC0000005, %%eax\n"            /* STATUS_ACCESS_VIOLATION */
        "8:\n\t"
        "movl %%fs:0x124, %%ecx\n\t"
        "movl $0, %c[rec](%%ecx)\n\t"
        : "=a"(Status), "+c"(Count), "+S"(Arguments), "+d"(Handler)
        : [esp] "i"(FIELD_OFFSET(QZ_THREAD, SyscallEsp)),
          [exl] "i"(FIELD_OFFSET(QZ_THREAD, SyscallExceptionList)),
          [rec] "i"(FIELD_OFFSET(QZ_THREAD, SyscallRecovery))
        : "ebx", "edi", "memory", "cc");
    return Status;
}

/* int 2Eh from ring 3 (interrupts off on entry) */
VOID
FASTCALL
QzSystemCall(PKTRAP_FRAME TrapFrame)
{
    PQZ_THREAD Thread = QzCurrentThread;
    ULONG Number = TrapFrame->Eax & 0xFFF, Arguments[32];
    const QZ_SERVICE *Service;
    KPROCESSOR_MODE OldMode = Thread->PreviousMode;
    PKTRAP_FRAME OldFrame = Thread->TrapFrame;
    NTSTATUS Status;

    _enable();
    QzSyscallCount++;
    if ((TrapFrame->Eax & 0x3000) || Number >= QZ_SERVICE_COUNT)
    {
        /* Table 1 is win32k's: there is none yet */
        if (QzSyscallTrace)
            DbgPrint("qzkrnl: syscall 0x%lx: no such service\n", TrapFrame->Eax);
        TrapFrame->Eax = (ULONG)STATUS_INVALID_SYSTEM_SERVICE;
        goto Out;
    }
    Service = &QzServiceTable[Number];
    if (!Service->Handler)
    {
        QzSyscallUnimplemented++;
        DbgPrint("qzkrnl: syscall Nt%s is not implemented yet\n", Service->Name);
        TrapFrame->Eax = (ULONG)STATUS_NOT_IMPLEMENTED;
        goto Out;
    }
    if (Service->Arguments > RTL_NUMBER_OF(Arguments) ||
        !QzCopyFromUser(Arguments, (PVOID)TrapFrame->Edx, Service->Arguments * sizeof(ULONG)))
    {
        TrapFrame->Eax = (ULONG)STATUS_ACCESS_VIOLATION;
        goto Out;
    }

    Thread->PreviousMode = UserMode;
    Thread->TrapFrame = TrapFrame;
    Status = QzCallService(Service->Handler, Arguments, Service->Arguments);
    Thread->PreviousMode = OldMode;
    Thread->TrapFrame = OldFrame ? OldFrame : TrapFrame;

    if (QzGetCurrentIrql() != PASSIVE_LEVEL)
    {
        DbgPrint("qzkrnl: Nt%s returned at IRQL %u\n", Service->Name, QzGetCurrentIrql());
        QzLowerIrql(PASSIVE_LEVEL);
    }
    if (QzSyscallTrace)
        DbgPrint("qzkrnl: [%s] Nt%s -> 0x%08lx\n", QzCurrentThread->Name, Service->Name, Status);
    /* A service that set a new user context (NtContinue) leaves EAX alone */
    if (Thread->TrapFrameReplaced)
        Thread->TrapFrameReplaced = FALSE;
    else
        TrapFrame->Eax = (ULONG)Status;
Out:
    _disable();
}

/*
 * int 2Dh: ntdll's DebugService. EAX = service (1 = print), ECX = buffer,
 * EDX = length; an int3 follows, which the kernel steps over.
 */
VOID
FASTCALL
QzDebugService(PKTRAP_FRAME TrapFrame)
{
    CHAR Buffer[512];
    ULONG Length;

    _enable();
    if (TrapFrame->Eax == 1 /* BREAKPOINT_PRINT */)
    {
        Length = min(TrapFrame->Edx, sizeof(Buffer) - 1);
        if (QzCopyFromUser(Buffer, (PVOID)TrapFrame->Ecx, Length))
        {
            Buffer[Length] = 0;
            DbgPrint("%s", Buffer);
        }
    }
    else if (TrapFrame->Eax == 2 /* BREAKPOINT_PROMPT */)
    {
        /* No debugger to answer: show the prompt, reply "i" (ignore) */
        static const CHAR Answer[] = "i";
        Length = min(TrapFrame->Edx, sizeof(Buffer) - 1);
        if (QzCopyFromUser(Buffer, (PVOID)TrapFrame->Ecx, Length))
        {
            Buffer[Length] = 0;
            DbgPrint("%s%s  (no debugger: answering \"i\")\n", Buffer, Answer);
        }
        if (TrapFrame->Edi >= 1 && QzCopyToUser((PVOID)TrapFrame->Ebx, Answer, 1))
        {
            TrapFrame->Eax = 1;   /* the length of the response */
            TrapFrame->Eip++;
            _disable();
            return;
        }
    }
    TrapFrame->Eax = (ULONG)STATUS_SUCCESS;
    TrapFrame->Eip++;   /* over the int3 */
    _disable();
}

/* int 2Ah: the tick count for old user code */
VOID
FASTCALL
QzGetTickCountService(PKTRAP_FRAME TrapFrame)
{
    TrapFrame->Eax = SharedUserData->TickCountLowDeprecated;
    TrapFrame->Edx = 0;
}

VOID
QzSyscallStats(VOID)
{
    QzPrint("  System services: %u in the table, %u implemented; %lu call(s), %lu to unimplemented ones\n",
            QZ_SERVICE_COUNT, QZ_SERVICES_IMPLEMENTED, QzSyscallCount, QzSyscallUnimplemented);
}

/* The service number of a name, for the self-test */
ULONG
QzServiceNumber(PCSTR Name)
{
    ULONG Index;
    for (Index = 0; Index < QZ_SERVICE_COUNT; Index++)
        if (!strcmp(QzServiceTable[Index].Name, Name))
            return Index;
    return (ULONG)-1;
}
