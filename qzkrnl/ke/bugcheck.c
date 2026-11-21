/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Bug checks (stop screens), exceptions raised by stubs, asserts
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qz.h"

#define EXCEPTION_WINE_STUB 0x80000100

static volatile LONG QzInBugCheck;

static PCSTR
QzBugCheckName(ULONG Code)
{
    switch (Code)
    {
        case IRQL_NOT_LESS_OR_EQUAL:           return "IRQL_NOT_LESS_OR_EQUAL";
        case KMODE_EXCEPTION_NOT_HANDLED:      return "KMODE_EXCEPTION_NOT_HANDLED";
        case PAGE_FAULT_IN_NONPAGED_AREA:      return "PAGE_FAULT_IN_NONPAGED_AREA";
        case UNEXPECTED_KERNEL_MODE_TRAP:      return "UNEXPECTED_KERNEL_MODE_TRAP";
        case MANUALLY_INITIATED_CRASH:         return "MANUALLY_INITIATED_CRASH";
        case PHASE0_INITIALIZATION_FAILED:     return "PHASE0_INITIALIZATION_FAILED";
        case NO_MORE_SYSTEM_PTES:              return "NO_MORE_SYSTEM_PTES";
        case INACCESSIBLE_BOOT_DEVICE:         return "INACCESSIBLE_BOOT_DEVICE";
        case BAD_POOL_CALLER:                  return "BAD_POOL_CALLER";
        case BAD_POOL_HEADER:                  return "BAD_POOL_HEADER";
        case INSTALL_MORE_MEMORY:              return "INSTALL_MORE_MEMORY";
        default:                               return "UNKNOWN_BUGCHECK";
    }
}

DECLSPEC_NORETURN
VOID
QzBugCheckWithText(ULONG Code, ULONG_PTR P1, ULONG_PTR P2,
                   ULONG_PTR P3, ULONG_PTR P4, PCSTR Text)
{
    __asm__ __volatile__("cli");

    /* A crash while crashing: just stop */
    if (InterlockedIncrement(&QzInBugCheck) > 1)
    {
        for (;;) QzHalt();
    }

    QzVgaBlueScreen();
    QzPrint("\n"
            "  " QZ_PRODUCT_NAME " has stopped to prevent damage to your computer.\n"
            "\n"
            "  %s\n"
            "\n",
            QzBugCheckName(Code));

    if (Text)
        QzPrint("  %s\n\n", Text);

    QzPrint("  If this is the first time you have seen this stop screen, restart\n"
            "  your computer. If it appears again, report the stop code and the\n"
            "  parameters below to the " QZ_PRODUCT_NAME " developers.\n"
            "\n"
            "  Technical information:\n"
            "\n"
            "  *** STOP: 0x%08lX (0x%08lX, 0x%08lX, 0x%08lX, 0x%08lX)\n"
            "\n"
            "  " QZ_KERNEL_NAME " " QZ_VERSION_STR " -- system halted.\n",
            Code, (ULONG)P1, (ULONG)P2, (ULONG)P3, (ULONG)P4);

    for (;;)
        QzHalt();
}

DECLSPEC_NORETURN
VOID
NTAPI
KeBugCheckEx(IN ULONG BugCheckCode,
             IN ULONG_PTR BugCheckParameter1,
             IN ULONG_PTR BugCheckParameter2,
             IN ULONG_PTR BugCheckParameter3,
             IN ULONG_PTR BugCheckParameter4)
{
    QzBugCheckWithText(BugCheckCode, BugCheckParameter1, BugCheckParameter2,
                       BugCheckParameter3, BugCheckParameter4, NULL);
}

DECLSPEC_NORETURN
VOID
NTAPI
KeBugCheck(IN ULONG BugCheckCode)
{
    QzBugCheckWithText(BugCheckCode, 0, 0, 0, 0, NULL);
}

/*
 * Exception dispatching does not exist yet, so any raised exception is
 * fatal. The common case is a call into an unimplemented export: the
 * stubs generated from qzkrnl.spec raise EXCEPTION_WINE_STUB with the
 * module and function name, which we put on the stop screen.
 */
VOID
NTAPI
RtlRaiseException(IN PEXCEPTION_RECORD ExceptionRecord)
{
    CHAR Text[128];
    PVOID Caller = __builtin_return_address(0);

    if (ExceptionRecord->ExceptionCode == EXCEPTION_WINE_STUB &&
        ExceptionRecord->NumberParameters >= 2)
    {
        QzSnprintf(Text, sizeof(Text), "Unimplemented kernel export: %s!%s (called from %p)",
                   (PCSTR)ExceptionRecord->ExceptionInformation[0],
                   (PCSTR)ExceptionRecord->ExceptionInformation[1], Caller);
    }
    else
    {
        QzSnprintf(Text, sizeof(Text), "Exception 0x%08lx raised from %p (no handler)",
                   ExceptionRecord->ExceptionCode, Caller);
    }
    DbgPrint("qzkrnl: %s\n", Text);
    QzBugCheckWithText(KMODE_EXCEPTION_NOT_HANDLED,
                       ExceptionRecord->ExceptionCode,
                       (ULONG_PTR)Caller,
                       ExceptionRecord->ExceptionInformation[0],
                       ExceptionRecord->ExceptionInformation[1],
                       Text);
}

DECLSPEC_NORETURN
VOID
NTAPI
RtlRaiseStatus(IN NTSTATUS Status)
{
    EXCEPTION_RECORD Record;

    RtlZeroMemory(&Record, sizeof(Record));
    Record.ExceptionCode = Status;
    Record.ExceptionAddress = __builtin_return_address(0);
    RtlRaiseException(&Record);
    for (;;);
}

VOID
NTAPI
RtlAssert(IN PVOID FailedAssertion,
          IN PVOID FileName,
          IN ULONG LineNumber,
          IN PCHAR Message OPTIONAL)
{
    CHAR Text[160];

    QzSnprintf(Text, sizeof(Text), "Assertion failed: %s (%s:%lu)%s%s",
               (PCSTR)FailedAssertion, (PCSTR)FileName, LineNumber,
               Message ? " - " : "", Message ? Message : "");
    QzBugCheckWithText(MANUALLY_INITIATED_CRASH, 0, 0, 0, LineNumber, Text);
}
