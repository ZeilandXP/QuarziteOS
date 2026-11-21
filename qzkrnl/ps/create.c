/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     User processes: the system DLL, initial thread APC, user
 *              exception dispatch
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qzio.h"
#include "qzps.h"

/* Before a new user thread first enters ring 3 */
VOID
QzInitialUserApc(PQZ_PS_THREAD PsThread, PKTRAP_FRAME TrapFrame)
{
    UNREFERENCED_PARAMETER(PsThread);
    UNREFERENCED_PARAMETER(TrapFrame);
}

/* Give a ring-3 exception to the process's own dispatcher; FALSE if none */
BOOLEAN
QzDispatchUserException(PKTRAP_FRAME TrapFrame, PEXCEPTION_RECORD Record)
{
    UNREFERENCED_PARAMETER(TrapFrame);
    UNREFERENCED_PARAMETER(Record);
    return FALSE;
}
