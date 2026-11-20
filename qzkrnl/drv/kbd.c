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

VOID
QzKbdInit(VOID)
{
    ULONG Spin;

    /* Drain anything left in the controller's output buffer */
    for (Spin = 0; Spin < 32 && (QzInB(0x64) & 1); Spin++)
        (VOID)QzInB(0x60);
    QzPicUnmask(QZ_IRQ_KEYBOARD);
}

VOID
QzKbdIsr(VOID)
{
    UCHAR Scan;
    BOOLEAN Release;
    CHAR c;

    if (!(QzInB(0x64) & 1))
        return;
    Scan = QzInB(0x60);

    if (Scan == 0xE0)
    {
        QzExtended = TRUE;
        return;
    }

    Release = (Scan & 0x80) != 0;
    Scan &= 0x7F;

    if (QzExtended)
    {
        QzExtended = FALSE;
        if (!Release && Scan == 0x1C)       /* keypad Enter */
            QzInputPush('\n');
        return;
    }

    if (Scan == 0x2A || Scan == 0x36)
    {
        QzShift = !Release;
        return;
    }
    if (Release)
        return;
    if (Scan == 0x3A)
    {
        QzCaps = !QzCaps;
        return;
    }
    if (Scan >= sizeof(QzKeymap))
        return;

    c = QzShift ? QzKeymapShift[Scan] : QzKeymap[Scan];
    if (QzCaps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c ^= 0x20;
    if (c)
        QzInputPush(c);
}
