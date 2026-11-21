/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel data and dispatch tables the HAL depends on
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * The HAL writes its own routines into HalDispatchTable and
 * HalPrivateDispatchTable during HalInitSystem, and calls through entries
 * the kernel provides -- notably HalVectorToIDTEntry, used by the inlined
 * KeRegisterInterruptHandler when the HAL installs its clock handler.
 * These must be real, separately allocated tables.
 */

#include "qz.h"

/* halfuncs.h maps these names onto the dispatch table itself */
#undef HalHaltSystem
#undef HalVectorToIDTEntry

static UCHAR NTAPI
QzHalVectorToIDTEntry(IN ULONG Vector)
{
    /* x86: the IDT entry is the vector */
    return (UCHAR)Vector;
}

static VOID NTAPI
QzHalHaltSystem(VOID)
{
    for (;;)
        __asm__ __volatile__("cli; hlt");
}

HAL_DISPATCH HalDispatchTable =
{
    HAL_DISPATCH_VERSION,
    /* everything else is filled in by the HAL or not supported yet */
};

HAL_PRIVATE_DISPATCH HalPrivateDispatchTable =
{
    .Version = HAL_PRIVATE_DISPATCH_VERSION,
    .HalHaltSystem = QzHalHaltSystem,
    .HalVectorToIDTEntry = QzHalVectorToIDTEntry,
};

/* No kernel debugger is attached (KD stage not implemented) */
BOOLEAN KdDebuggerEnabled = FALSE;
BOOLEAN KdDebuggerNotPresent = TRUE;

/* Bug check code + 4 parameters, for crash dump tooling */
ULONG_PTR KiBugCheckData[5];

/* System service tables: empty until system calls exist (stage B6) */
KSERVICE_TABLE_DESCRIPTOR KeServiceDescriptorTable[SSDT_MAX_ENTRIES];

VOID
QzHalInterfaceInit(VOID)
{
    /* Nothing yet: the tables above are statically initialized */
}
