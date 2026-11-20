/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     16550 UART on COM1: debug output and console input
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qz.h"

#define COM1        0x3F8
#define UART_RBR    0
#define UART_THR    0
#define UART_IER    1
#define UART_DLL    0
#define UART_DLM    1
#define UART_FCR    2
#define UART_LCR    3
#define UART_MCR    4
#define UART_LSR    5

static BOOLEAN QzSerialPresent;

VOID
QzSerialInit(VOID)
{
    QzOutB(COM1 + UART_IER, 0x00);
    QzOutB(COM1 + UART_LCR, 0x80);   /* DLAB */
    QzOutB(COM1 + UART_DLL, 0x01);   /* 115200 baud */
    QzOutB(COM1 + UART_DLM, 0x00);
    QzOutB(COM1 + UART_LCR, 0x03);   /* 8N1 */
    QzOutB(COM1 + UART_FCR, 0xC7);   /* FIFO on, cleared, 14-byte threshold */
    QzOutB(COM1 + UART_MCR, 0x0B);   /* DTR, RTS, OUT2 (IRQ gate) */

    /* An absent UART reads back as 0xFF */
    QzSerialPresent = (QzInB(COM1 + UART_LSR) != 0xFF);
}

VOID
QzSerialPutChar(CHAR c)
{
    ULONG Spin;

    if (!QzSerialPresent)
        return;
    if (c == '\n')
        QzSerialPutChar('\r');
    for (Spin = 0; Spin < 100000 && !(QzInB(COM1 + UART_LSR) & 0x20); Spin++)
        ;
    QzOutB(COM1 + UART_THR, (UCHAR)c);
}

VOID
QzSerialEnableRxInterrupt(VOID)
{
    if (!QzSerialPresent)
        return;
    QzOutB(COM1 + UART_IER, 0x01);   /* received data available */
    QzPicUnmask(QZ_IRQ_COM1);
}

VOID
QzSerialIsr(VOID)
{
    CHAR c;

    while (QzInB(COM1 + UART_LSR) & 0x01)
    {
        c = (CHAR)QzInB(COM1 + UART_RBR);
        if (c == '\r')
            c = '\n';
        else if (c == 0x7F)
            c = '\b';
        QzInputPush(c);
    }
}
