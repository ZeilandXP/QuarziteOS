/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Console input queue shared by the keyboard and serial port
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qz.h"

#define QZ_INPUT_SIZE 256

static volatile CHAR QzInputRing[QZ_INPUT_SIZE];
static volatile ULONG QzInputHead, QzInputTail;

/* Called from interrupt context */
VOID
QzInputPush(CHAR c)
{
    ULONG Next = (QzInputHead + 1) % QZ_INPUT_SIZE;
    if (Next == QzInputTail)
        return;                     /* full: drop */
    QzInputRing[QzInputHead] = c;
    QzInputHead = Next;
}

CHAR
QzInputGet(VOID)
{
    CHAR c;
    ULONG Flags;

    for (;;)
    {
        Flags = QzSaveFlagsAndDisable();
        if (QzInputHead != QzInputTail)
        {
            c = QzInputRing[QzInputTail];
            QzInputTail = (QzInputTail + 1) % QZ_INPUT_SIZE;
            QzRestoreFlags(Flags);
            return c;
        }
        QzRestoreFlags(Flags);

        if (QzCurrentThread)
            QzSleepTicks(1);
        else
            __asm__ __volatile__("sti; hlt" : : : "memory");
    }
}

VOID
QzReadLine(PCHAR Buffer, ULONG Size)
{
    ULONG Length = 0;
    CHAR c;
    CHAR Echo[2] = { 0, 0 };

    for (;;)
    {
        c = QzInputGet();
        if (c == '\n')
        {
            QzPutString("\n");
            break;
        }
        if (c == '\b')
        {
            if (Length > 0)
            {
                Length--;
                QzPutString("\b \b");
            }
            continue;
        }
        if ((UCHAR)c < 0x20 || Length + 1 >= Size)
            continue;
        Buffer[Length++] = c;
        Echo[0] = c;
        QzPutString(Echo);
    }
    Buffer[Length] = 0;
}
