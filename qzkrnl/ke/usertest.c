/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     User mode self-test (shell command "usertest")
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Small ring-3 programs, written in position-independent assembly below,
 * are copied into new processes and run as user threads. They make system
 * calls the way ntdll does (int 2Eh, EAX = service, EDX = arguments),
 * print through int 2Dh as ntdll's DbgPrint does, and end with
 * NtTerminateProcess; the test checks how each process ended.
 */

#include "qzio.h"
#include "qzps.h"
#include "systable.h"

#define STR2(x) #x
#define STR(x) STR2(x)

#define QZUT_CODE_BASE  0x00400000UL

extern UCHAR QzUtHello[], QzUtHelloEnd[];
extern UCHAR QzUtCrash[], QzUtCrashEnd[];
extern UCHAR QzUtPrivileged[], QzUtPrivilegedEnd[];
extern UCHAR QzUtSpin[], QzUtSpinEnd[];
extern UCHAR QzUtFpu[], QzUtFpuEnd[];
extern UCHAR QzUtFast[], QzUtFastRet[], QzUtFastEnd[];
extern BOOLEAN QzSysenterAvailable;

/* syscall(service, n arguments already pushed): edx = esp; int 2Eh */
#define QZUT_SYSCALL(Service, Bytes) \
    "    movl %esp, %edx\n" \
    "    movl $" STR(Service) ", %eax\n" \
    "    int $0x2e\n" \
    "    addl $" #Bytes ", %esp\n"

#define QZUT_EXIT_WITH_EAX \
    "    pushl %eax\n" \
    "    pushl $-1\n" \
    "    movl %esp, %edx\n" \
    "    movl $" STR(QZ_SVCNUM_TerminateProcess) ", %eax\n" \
    "    int $0x2e\n" \
    "    ud2\n"

__asm__(
    ".data\n"
    /* ---- hello: print, allocate, touch, bad pointer, bad service, exit ---- */
    ".globl _QzUtHello\n"
    "_QzUtHello:\n"
    "    call 1f\n"
    "1:  popl %ebx\n"
    "    leal (9f - 1b)(%ebx), %ecx\n"
    "    movl $(8f - 9f), %edx\n"
    "    movl $1, %eax\n"
    "    int $0x2d\n"
    "    int3\n"
    /* NtAllocateVirtualMemory(-1, &base, 0, &size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) */
    "    subl $8, %esp\n"
    "    movl $0, (%esp)\n"
    "    movl $0x2000, 4(%esp)\n"
    "    movl %esp, %esi\n"
    "    pushl $4\n"
    "    pushl $0x3000\n"
    "    leal 4(%esi), %eax\n"
    "    pushl %eax\n"
    "    pushl $0\n"
    "    pushl %esi\n"
    "    pushl $-1\n"
    QZUT_SYSCALL(QZ_SVCNUM_AllocateVirtualMemory, 24)
    "    testl %eax, %eax\n"
    "    jnz 7f\n"
    "    movl (%esi), %edi\n"
    "    movl $0x51515151, (%edi)\n"
    "    movl $0x52525252, 0x1000(%edi)\n"
    "    movl $0xC0000001, %eax\n"
    "    cmpl $0x51515151, (%edi)\n"
    "    jne 7f\n"
    /* the same call with a kernel address for the BaseAddress pointer */
    "    pushl $4\n"
    "    pushl $0x3000\n"
    "    leal 4(%esi), %eax\n"
    "    pushl %eax\n"
    "    pushl $0\n"
    "    pushl $0x80000000\n"
    "    pushl $-1\n"
    QZUT_SYSCALL(QZ_SVCNUM_AllocateVirtualMemory, 24)
    "    cmpl $0xC0000005, %eax\n"
    "    jne 6f\n"
    /* a service number that does not exist */
    "    movl $0xFFF, %eax\n"
    "    int $0x2e\n"
    "    cmpl $0xC000001C, %eax\n"
    "    jne 6f\n"
    /* NtClose of a bad handle */
    "    pushl $0x1234\n"
    QZUT_SYSCALL(QZ_SVCNUM_Close, 4)
    "    cmpl $0xC0000008, %eax\n"
    "    jne 6f\n"
    "    movl $0x600DC0DE, %eax\n"
    QZUT_EXIT_WITH_EAX
    "6:  movl $0xC0000002, %eax\n"
    "7:\n"
    QZUT_EXIT_WITH_EAX
    "9:  .ascii \"usertest: hello from ring 3\\n\"\n"
    "8:\n"
    ".globl _QzUtHelloEnd\n"
    "_QzUtHelloEnd:\n"

    /* ---- crash: read address 0 ---- */
    ".globl _QzUtCrash\n"
    "_QzUtCrash:\n"
    "    xorl %eax, %eax\n"
    "    movl (%eax), %eax\n"
    QZUT_EXIT_WITH_EAX
    ".globl _QzUtCrashEnd\n"
    "_QzUtCrashEnd:\n"

    /* ---- privileged: hlt in ring 3 ---- */
    ".globl _QzUtPrivileged\n"
    "_QzUtPrivileged:\n"
    "    hlt\n"
    "    xorl %eax, %eax\n"
    QZUT_EXIT_WITH_EAX
    ".globl _QzUtPrivilegedEnd\n"
    "_QzUtPrivilegedEnd:\n"

    /* ---- spin: a long loop, preempted by the clock in ring 3 ---- */
    ".globl _QzUtSpin\n"
    "_QzUtSpin:\n"
    "    movl $40000000, %ecx\n"
    "    xorl %eax, %eax\n"
    "1:  addl %ecx, %eax\n"
    "    decl %ecx\n"
    "    jnz 1b\n"
    "    movl $0x5917, %eax\n"
    QZUT_EXIT_WITH_EAX
    ".globl _QzUtSpinEnd\n"
    "_QzUtSpinEnd:\n"

    /* ---- fpu: x87 state survives the other thread's FPU use ---- */
    ".globl _QzUtFpu\n"
    "_QzUtFpu:\n"
    "    movl %esp, %ebp\n"
    "    subl $16, %esp\n"
    "    fninit\n"
    "    fld1\n"                    /* st0 = 1 */
    "    movl $20000000, %ecx\n"
    "1:  decl %ecx\n"
    "    jnz 1b\n"                  /* get preempted with st0 loaded */
    "    fld1\n"
    "    faddp\n"                   /* st0 = 2 */
    "    fistpl (%esp)\n"
    "    movl (%esp), %eax\n"
    "    addl $0xF0F0, %eax\n"      /* 0xF0F2 if the state was kept */
    QZUT_EXIT_WITH_EAX
    ".globl _QzUtFpuEnd\n"
    "_QzUtFpuEnd:\n"

    /* ---- fast: the same service through sysenter, as KiFastSystemCall ---- */
    ".globl _QzUtFast\n"
    "_QzUtFast:\n"
    "    subl $8, %esp\n"
    "    movl $0, (%esp)\n"
    "    movl $0x1000, 4(%esp)\n"
    "    movl %esp, %esi\n"
    "    pushl $4\n"
    "    pushl $0x3000\n"
    "    leal 4(%esi), %eax\n"
    "    pushl %eax\n"
    "    pushl $0\n"
    "    pushl %esi\n"
    "    pushl $-1\n"
    "    call 2f\n"                     /* the Nt stub */
    "    addl $24, %esp\n"
    "    testl %eax, %eax\n"
    "    jnz 7f\n"
    "    movl (%esi), %edi\n"
    "    movl $0x77, (%edi)\n"
    "    movl $0xFA57, %eax\n"
    "7:\n"
    QZUT_EXIT_WITH_EAX
    "2:  movl $" STR(QZ_SVCNUM_AllocateVirtualMemory) ", %eax\n"
    "    call 3f\n"                     /* KiFastSystemCall */
    "    ret\n"
    "3:  movl %esp, %edx\n"
    "    sysenter\n"
    ".globl _QzUtFastRet\n"
    "_QzUtFastRet:\n"                   /* KiFastSystemCallRet */
    "    ret\n"
    ".globl _QzUtFastEnd\n"
    "_QzUtFastEnd:\n"
    ".text\n"
);

static ULONG QzPassed, QzFailed;

static VOID
QzCheck(PCSTR Name, BOOLEAN Condition)
{
    if (Condition)
        QzPassed++;
    else
        QzFailed++;
    QzPrint("  [%s] %s\n", Condition ? "PASS" : "FAIL", Name);
}

typedef struct _QZUT_RUN
{
    PQZ_PROCESS Process;
    PQZ_PS_THREAD Thread;
} QZUT_RUN;

/* A process with Code at 0x400000, a 64 KB stack and a TEB page */
static NTSTATUS
QzutStart(PCSTR Name, PUCHAR Code, PUCHAR CodeEnd, QZUT_RUN *Run)
{
    PVOID Base = (PVOID)QZUT_CODE_BASE, Stack = NULL, Teb = NULL;
    SIZE_T Size = PAGE_SIZE, StackSize = 0x10000, TebSize = PAGE_SIZE;
    PQZ_PROCESS Saved;
    CONTEXT Context;
    NTSTATUS Status;

    RtlZeroMemory(Run, sizeof(*Run));
    Status = QzPsCreateProcess(Name, QzSystemProcess, &Run->Process);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = QzMmAllocate(Run->Process, &Base, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (NT_SUCCESS(Status))
        Status = QzMmAllocate(Run->Process, &Stack, 0, &StackSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (NT_SUCCESS(Status))
        Status = QzMmAllocate(Run->Process, &Teb, 0, &TebSize, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN,
                              PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
        return Status;

    QzPsAttach(Run->Process, &Saved);
    QzCopyToUser(Base, Code, CodeEnd - Code);
    {
        NT_TIB Tib;
        RtlZeroMemory(&Tib, sizeof(Tib));
        Tib.ExceptionList = (PVOID)-1;
        Tib.StackBase = (PUCHAR)Stack + StackSize;
        Tib.StackLimit = Stack;
        Tib.Self = Teb;
        QzCopyToUser(Teb, &Tib, sizeof(Tib));
    }
    QzPsDetach(Saved);

    RtlZeroMemory(&Context, sizeof(Context));
    Context.Eip = QZUT_CODE_BASE;
    Context.Esp = (ULONG)(ULONG_PTR)Stack + StackSize - 16;
    Context.EFlags = 0x202;
    return QzPsCreateUserThread(Run->Process, &Context, Teb, FALSE, &Run->Thread);
}

/* Wait for the process to end; returns its exit status, or STATUS_TIMEOUT */
static NTSTATUS
QzutFinish(QZUT_RUN *Run, ULONG TimeoutMs)
{
    LARGE_INTEGER Timeout;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    Timeout.QuadPart = -10000LL * TimeoutMs;
    if (Run->Process)
    {
        if (KeWaitForSingleObject(&Run->Process->Exited, Executive, KernelMode, FALSE, &Timeout) ==
            STATUS_SUCCESS)
            Status = Run->Process->ExitStatus;
        else
            Status = STATUS_TIMEOUT;
    }
    if (Run->Thread)
        ObDereferenceObject(Run->Thread);
    if (Run->Process)
        ObDereferenceObject(Run->Process);
    return Status;
}

static NTSTATUS
QzutRun(PCSTR Name, PUCHAR Code, PUCHAR CodeEnd)
{
    QZUT_RUN Run;
    NTSTATUS Status = QzutStart(Name, Code, CodeEnd, &Run);
    if (!NT_SUCCESS(Status))
    {
        QzutFinish(&Run, 1);
        return Status;
    }
    return QzutFinish(&Run, 5000);
}

static VOID
QzutTests(VOID)
{
    QZUT_RUN A, B;
    NTSTATUS Status, Other;
    ULONG Total, FreeBefore, FreeAfter;

    QzMmGetStats(&Total, &FreeBefore);

    Status = QzutRun("ut-hello", QzUtHello, QzUtHelloEnd);
    QzCheck("ring 3: system calls, int 2Dh print, bad pointers and services, exit", Status == 0x600DC0DE);
    if (Status != 0x600DC0DE)
        QzPrint("    (exit status 0x%08lx)\n", Status);

    Status = QzutRun("ut-crash", QzUtCrash, QzUtCrashEnd);
    QzCheck("a NULL read ends the process with STATUS_ACCESS_VIOLATION", Status == STATUS_ACCESS_VIOLATION);

    Status = QzutRun("ut-hlt", QzUtPrivileged, QzUtPrivilegedEnd);
    QzCheck("hlt in ring 3 is STATUS_PRIVILEGED_INSTRUCTION", Status == STATUS_PRIVILEGED_INSTRUCTION);

    /* Two spinning processes: both make progress under the clock */
    Status = QzutStart("ut-spin-a", QzUtSpin, QzUtSpinEnd, &A);
    Other = QzutStart("ut-spin-b", QzUtSpin, QzUtSpinEnd, &B);
    Status = NT_SUCCESS(Status) ? QzutFinish(&A, 20000) : (QzutFinish(&A, 1), Status);
    Other = NT_SUCCESS(Other) ? QzutFinish(&B, 20000) : (QzutFinish(&B, 1), Other);
    QzCheck("two processes spinning in ring 3 are both preempted and finish", Status == 0x5917 && Other == 0x5917);

    Status = QzutStart("ut-fpu-a", QzUtFpu, QzUtFpuEnd, &A);
    Other = QzutStart("ut-fpu-b", QzUtFpu, QzUtFpuEnd, &B);
    Status = NT_SUCCESS(Status) ? QzutFinish(&A, 20000) : (QzutFinish(&A, 1), Status);
    Other = NT_SUCCESS(Other) ? QzutFinish(&B, 20000) : (QzutFinish(&B, 1), Other);
    QzCheck("FPU state is per thread", Status == 0xF0F2 && Other == 0xF0F2);

    if (QzSysenterAvailable)
    {
        ULONG SavedReturn = SharedUserData->SystemCallReturn;
        SharedUserData->SystemCallReturn = QZUT_CODE_BASE + (ULONG)(QzUtFastRet - QzUtFast);
        Status = QzutRun("ut-sysenter", QzUtFast, QzUtFastEnd);
        SharedUserData->SystemCallReturn = SavedReturn;
        QzCheck("system calls through sysenter", Status == 0xFA57);
    }
    else
    {
        QzPrint("  (no sysenter on this processor: int 2Eh only)\n");
    }

    QzMmGetStats(&Total, &FreeAfter);
    QzCheck("the processes' memory is returned", FreeAfter + 64 >= FreeBefore);
    if (FreeAfter + 64 < FreeBefore)
        QzPrint("    (%lu page(s) before, %lu after)\n", FreeBefore, FreeAfter);
}

VOID
QzRunUserTests(VOID)
{
    QzPassed = QzFailed = 0;
    QzutTests();
    QzPrint("  usertest: %lu passed, %lu failed\n", QzPassed, QzFailed);
}
