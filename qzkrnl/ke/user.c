/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     User mode: exceptions raised in ring 3, the way back to it
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qzio.h"
#include "qzps.h"

/* An exception in user mode (from the trap handler, interrupts off) */
VOID
QzUserException(PKTRAP_FRAME TrapFrame, NTSTATUS Code, ULONG_PTR Address, ULONG_PTR Write)
{
    DbgPrint(QZ_KERNEL_NAME ": user-mode exception 0x%08lx at %08lx (address %08lx, %s)\n",
             Code, TrapFrame->Eip, Address, Write ? "write" : "read");
    QzDumpTrapFrame(TrapFrame);
    KeBugCheckEx(KMODE_EXCEPTION_NOT_HANDLED, (ULONG_PTR)Code, TrapFrame->Eip, Address, 1);
}

/* Every return to ring 3 passes here, with the user trap frame */
VOID
FASTCALL
QzExitToUserMode(PKTRAP_FRAME TrapFrame)
{
    UNREFERENCED_PARAMETER(TrapFrame);
}
