/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minimal i8042 PS/2 keyboard driver (scan code set 1, US layout)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qz.h"

static const CHAR QzKeymap[0x3A] =
{
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t','q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0,   ' '
};

static const CHAR QzKeymapShift[0x3A] =
{
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t','Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0,   ' '
};

static BOOLEAN QzShift, QzCaps, QzExtended;

static KINTERRUPT QzKbdInterrupt;
static BOOLEAN NTAPI QzKbdIsr(PKINTERRUPT Interrupt, PVOID Context);

/* Connect an ISA IRQ through the HAL's bus handler and an interrupt object */
BOOLEAN
QzConnectIsaInterrupt(PKINTERRUPT Interrupt, ULONG Irq,
                      PKSERVICE_ROUTINE Isr, PVOID Context)
{
    KIRQL Irql;
    KAFFINITY Affinity;
    ULONG Vector;

    Vector = HalGetInterruptVector(Isa, 0, Irq, Irq, &Irql, &Affinity);
    if (Vector == 0)
    {
        /* No bus handler: use the PIC HAL's fixed mapping */
        Vector = PRIMARY_VECTOR_BASE + Irq;
        Irql = (KIRQL)(PROFILE_LEVEL - Irq);
    }

    KeInitializeInterrupt(Interrupt, Isr, Context, NULL, Vector, Irql, Irql,
                          Latched, FALSE, 0, FALSE);
    if (!KeConnectInterrupt(Interrupt))
        return FALSE;

    DbgPrint("qzkrnl: IRQ %lu connected: vector 0x%02lx, IRQL %u\n", Irq, Vector, Irql);
    return TRUE;
}

VOID
QzKbdInit(VOID)
{
    ULONG Spin;

    /* Drain anything left in the controller's output buffer */
    for (Spin = 0; Spin < 32 && (QzInB(0x64) & 1); Spin++)
        (VOID)QzInB(0x60);
    QzConnectIsaInterrupt(&QzKbdInterrupt, QZ_IRQ_KEYBOARD, QzKbdIsr, NULL);
}

/*
 * The PS/2 controller goes to i8042prt (the GUI's keyboard and mouse):
 * the console's own handler lets go of IRQ 1. The shell stays on the
 * serial port.
 */
VOID
QzKbdRelease(VOID)
{
    static BOOLEAN Released;
    if (Released)
        return;
    Released = TRUE;
    KeDisconnectInterrupt(&QzKbdInterrupt);
    DbgPrint("qzkrnl: the console keyboard handed IRQ 1 to i8042prt\n");
}

static BOOLEAN NTAPI
QzKbdIsr(PKINTERRUPT Interrupt, PVOID Context)
{
    UCHAR Scan;
    BOOLEAN Release;
    CHAR c;

    UNREFERENCED_PARAMETER(Interrupt);
    UNREFERENCED_PARAMETER(Context);

    if (!(QzInB(0x64) & 1))
        return FALSE;
    Scan = QzInB(0x60);

    if (Scan == 0xE0)
    {
        QzExtended = TRUE;
        return TRUE;
    }

    Release = (Scan & 0x80) != 0;
    Scan &= 0x7F;

    if (QzExtended)
    {
        QzExtended = FALSE;
        if (!Release && Scan == 0x1C)       /* keypad Enter */
            QzInputPush('\n');
        return TRUE;
    }

    if (Scan == 0x2A || Scan == 0x36)
    {
        QzShift = !Release;
        return TRUE;
    }
    if (Release)
        return TRUE;
    if (Scan == 0x3A)
    {
        QzCaps = !QzCaps;
        return TRUE;
    }
    if (Scan >= sizeof(QzKeymap))
        return TRUE;

    c = QzShift ? QzKeymapShift[Scan] : QzKeymap[Scan];
    if (QzCaps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c ^= 0x20;
    if (c)
        QzInputPush(c);
    return TRUE;
}
