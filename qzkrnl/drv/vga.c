/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     VGA text-mode console with a status line
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qz.h"

static volatile USHORT *QzVram;
static ULONG QzCols = 80, QzRows = 25;
static ULONG QzFirstRow = 1;           /* row 0 is the status line */
static ULONG QzCurX, QzCurY = 1;
static UCHAR QzAttr = 0x07;
static BOOLEAN QzStatusEnabled = TRUE;

BOOLEAN
QzVgaAvailable(VOID)
{
    return QzVram != NULL;
}

static VOID
QzVgaUpdateCursor(VOID)
{
    ULONG Pos = QzCurY * QzCols + QzCurX;
    QzOutB(0x3D4, 0x0F);
    QzOutB(0x3D5, (UCHAR)(Pos & 0xFF));
    QzOutB(0x3D4, 0x0E);
    QzOutB(0x3D5, (UCHAR)((Pos >> 8) & 0xFF));
}

static VOID
QzVgaClearRows(ULONG From, ULONG To, UCHAR Attr)
{
    ULONG i;
    for (i = From * QzCols; i < To * QzCols; i++)
        QzVram[i] = (USHORT)((Attr << 8) | ' ');
}

VOID
QzVgaInit(VOID)
{
    volatile UCHAR *Bda;

    /* The BIOS data area tells us the text geometry FreeLoader left behind */
    Bda = QzMapPhysical(0x400, 0x100, FALSE);
    if (Bda)
    {
        ULONG Cols = *(volatile USHORT *)(Bda + 0x4A);
        ULONG Rows = (ULONG)Bda[0x84] + 1;
        if (Cols >= 40 && Cols <= 132) QzCols = Cols;
        if (Rows >= 25 && Rows <= 60) QzRows = Rows;
    }

    QzVram = QzMapPhysical(0xB8000, 0x8000, TRUE);
    if (!QzVram)
        return;

    /* Show the cursor (scanlines 13-14) */
    QzOutB(0x3D4, 0x0A); QzOutB(0x3D5, 0x0D);
    QzOutB(0x3D4, 0x0B); QzOutB(0x3D5, 0x0E);

    QzVgaClearRows(0, QzRows, 0x07);
    QzCurX = 0;
    QzCurY = QzFirstRow;
    QzVgaSetStatus("");
    QzVgaUpdateCursor();
}

VOID
QzVgaSetAttribute(UCHAR Attr)
{
    QzAttr = Attr;
}

static VOID
QzVgaScroll(VOID)
{
    ULONG i;
    for (i = QzFirstRow * QzCols; i < (QzRows - 1) * QzCols; i++)
        QzVram[i] = QzVram[i + QzCols];
    QzVgaClearRows(QzRows - 1, QzRows, QzAttr);
    QzCurY = QzRows - 1;
}

VOID
QzVgaPutChar(CHAR c)
{
    if (!QzVram)
        return;

    switch (c)
    {
        case '\n':
            QzCurX = 0;
            QzCurY++;
            break;
        case '\r':
            QzCurX = 0;
            break;
        case '\b':
            if (QzCurX > 0)
            {
                QzCurX--;
                QzVram[QzCurY * QzCols + QzCurX] = (USHORT)((QzAttr << 8) | ' ');
            }
            break;
        case '\t':
            QzCurX = (QzCurX + 8) & ~7UL;
            break;
        default:
            QzVram[QzCurY * QzCols + QzCurX] = (USHORT)((QzAttr << 8) | (UCHAR)c);
            QzCurX++;
            break;
    }

    if (QzCurX >= QzCols)
    {
        QzCurX = 0;
        QzCurY++;
    }
    if (QzCurY >= QzRows)
        QzVgaScroll();
    QzVgaUpdateCursor();
}

VOID
QzVgaSetStatus(PCSTR Text)
{
    ULONG i;
    ULONG Flags;

    if (!QzVram || !QzStatusEnabled)
        return;

    Flags = QzSaveFlagsAndDisable();
    for (i = 0; i < QzCols; i++)
    {
        CHAR c = (Text && *Text) ? *Text++ : ' ';
        QzVram[i] = (USHORT)((0x1F << 8) | (UCHAR)c);   /* white on blue */
    }
    QzRestoreFlags(Flags);
}

VOID
QzVgaBlueScreen(VOID)
{
    if (!QzVram)
        return;
    QzStatusEnabled = FALSE;
    QzFirstRow = 0;
    QzAttr = 0x1F;
    QzVgaClearRows(0, QzRows, QzAttr);
    QzCurX = 0;
    QzCurY = 0;
    /* Hide the cursor */
    QzOutB(0x3D4, 0x0A);
    QzOutB(0x3D5, 0x20);
}
