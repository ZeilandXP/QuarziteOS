/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     VGA text-mode console with a status line
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qz.h"
#include <ndk/inbvfuncs.h>

static volatile USHORT *QzVram;
static ULONG QzCols = 80, QzRows = 25;
static ULONG QzFirstRow = 1;           /* row 0 is the status line */
static ULONG QzCurX, QzCurY = 1;
static UCHAR QzAttr = 0x07;
static BOOLEAN QzStatusEnabled = TRUE;

/*
 * Display ownership (INBV): the console writes to text memory only while
 * the kernel owns the display. A video miniport takes it by registering a
 * reset callback (videoprt does, when win32k opens it); a bugcheck takes
 * it back, asking the callback to put the adapter back into text mode.
 */
static BOOLEAN QzDisplayOwned = TRUE;
static INBV_RESET_DISPLAY_PARAMETERS QzDisplayReset;

/*
 * The text mode FreeLoader left, saved at boot: registers, the DAC and the
 * font in plane 2. A bugcheck after a video miniport took the display puts
 * it back when the miniport cannot reset the adapter itself (bochsmp has no
 * HwResetHw, and the graphics mode overwrote the font).
 */
#define QZ_VGA_FONT_BYTES   (256 * 32)

typedef struct _QZ_VGA_STATE
{
    BOOLEAN Saved;
    UCHAR Misc;
    UCHAR Seq[5];
    UCHAR Crtc[25];
    UCHAR Gc[9];
    UCHAR Attr[21];
    UCHAR Dac[256 * 3];
    UCHAR Font[QZ_VGA_FONT_BYTES];
} QZ_VGA_STATE;

static QZ_VGA_STATE QzVgaState;
static volatile UCHAR *QzVgaWindow;     /* A0000, mapped once at boot (a bugcheck maps nothing) */

static UCHAR QzVgaRead(USHORT IndexPort, UCHAR Index)
{
    QzOutB(IndexPort, Index);
    return QzInB(IndexPort + 1);
}

static VOID QzVgaWrite(USHORT IndexPort, UCHAR Index, UCHAR Value)
{
    QzOutB(IndexPort, Index);
    QzOutB(IndexPort + 1, Value);
}

/* Plane 2 at A0000, for the font; QzVgaClosePlane2 puts the registers back */
static volatile UCHAR *
QzVgaOpenPlane2(BOOLEAN Write)
{
    if (Write)
        QzVgaWrite(0x3C4, 2, 0x04);         /* map mask: plane 2 */
    QzVgaWrite(0x3C4, 4, 0x06);             /* no chain-4, no odd/even */
    QzVgaWrite(0x3CE, 4, 0x02);             /* read map: plane 2 */
    QzVgaWrite(0x3CE, 5, 0x00);             /* read/write mode 0, no odd/even */
    QzVgaWrite(0x3CE, 6, 0x04);             /* A0000-AFFFF, graphics addressing */
    if (!QzVgaWindow)
        QzVgaWindow = QzMapPhysical(0xA0000, QZ_VGA_FONT_BYTES, TRUE);
    return QzVgaWindow;
}

static VOID
QzVgaClosePlane2(VOID)
{
    QzVgaWrite(0x3C4, 2, QzVgaState.Seq[2]);
    QzVgaWrite(0x3C4, 4, QzVgaState.Seq[4]);
    QzVgaWrite(0x3CE, 4, QzVgaState.Gc[4]);
    QzVgaWrite(0x3CE, 5, QzVgaState.Gc[5]);
    QzVgaWrite(0x3CE, 6, QzVgaState.Gc[6]);
}

static VOID
QzVgaSaveState(VOID)
{
    volatile UCHAR *Plane;
    ULONG i;

    QzVgaState.Misc = QzInB(0x3CC);
    for (i = 0; i < RTL_NUMBER_OF(QzVgaState.Seq); i++)
        QzVgaState.Seq[i] = QzVgaRead(0x3C4, (UCHAR)i);
    for (i = 0; i < RTL_NUMBER_OF(QzVgaState.Crtc); i++)
        QzVgaState.Crtc[i] = QzVgaRead(0x3D4, (UCHAR)i);
    for (i = 0; i < RTL_NUMBER_OF(QzVgaState.Gc); i++)
        QzVgaState.Gc[i] = QzVgaRead(0x3CE, (UCHAR)i);
    for (i = 0; i < RTL_NUMBER_OF(QzVgaState.Attr); i++)
    {
        (VOID)QzInB(0x3DA);                 /* the attribute flip-flop to "index" */
        QzOutB(0x3C0, (UCHAR)i);
        QzVgaState.Attr[i] = QzInB(0x3C1);
    }
    (VOID)QzInB(0x3DA);
    QzOutB(0x3C0, 0x20);                    /* palette back on */
    QzOutB(0x3C7, 0);
    for (i = 0; i < sizeof(QzVgaState.Dac); i++)
        QzVgaState.Dac[i] = QzInB(0x3C9);

    Plane = QzVgaOpenPlane2(FALSE);
    if (Plane)
    {
        for (i = 0; i < QZ_VGA_FONT_BYTES; i++)
            QzVgaState.Font[i] = Plane[i];
        QzVgaState.Saved = TRUE;
    }
    QzVgaClosePlane2();
}

static VOID
QzVgaRestoreState(VOID)
{
    volatile UCHAR *Plane;
    ULONG i;
    USHORT Id;

    if (!QzVgaState.Saved)
        return;

    /* QEMU's and Bochs's VBE extension: switch it off, if its ID answers */
    __outword(0x1CE, 0);
    Id = __inword(0x1CF);
    if ((Id & 0xFFF0) == 0xB0C0)
    {
        __outword(0x1CE, 4);
        __outword(0x1CF, 0);
    }

    QzOutB(0x3C2, QzVgaState.Misc);
    QzVgaWrite(0x3C4, 0, 0x01);             /* synchronous reset */
    for (i = 1; i < RTL_NUMBER_OF(QzVgaState.Seq); i++)
        QzVgaWrite(0x3C4, (UCHAR)i, QzVgaState.Seq[i]);
    QzVgaWrite(0x3C4, 0, 0x03);
    QzVgaWrite(0x3D4, 0x11, QzVgaRead(0x3D4, 0x11) & 0x7F);    /* unlock CR0-7 */
    for (i = 0; i < RTL_NUMBER_OF(QzVgaState.Crtc); i++)
        QzVgaWrite(0x3D4, (UCHAR)i, (i == 0x11) ? (QzVgaState.Crtc[i] & 0x7F) : QzVgaState.Crtc[i]);
    QzVgaWrite(0x3D4, 0x11, QzVgaState.Crtc[0x11]);
    for (i = 0; i < RTL_NUMBER_OF(QzVgaState.Gc); i++)
        QzVgaWrite(0x3CE, (UCHAR)i, QzVgaState.Gc[i]);
    for (i = 0; i < RTL_NUMBER_OF(QzVgaState.Attr); i++)
    {
        (VOID)QzInB(0x3DA);
        QzOutB(0x3C0, (UCHAR)i);
        QzOutB(0x3C0, QzVgaState.Attr[i]);
    }
    (VOID)QzInB(0x3DA);
    QzOutB(0x3C0, 0x20);
    QzOutB(0x3C8, 0);
    for (i = 0; i < sizeof(QzVgaState.Dac); i++)
        QzOutB(0x3C9, QzVgaState.Dac[i]);

    Plane = QzVgaOpenPlane2(TRUE);
    if (Plane)
    {
        for (i = 0; i < QZ_VGA_FONT_BYTES; i++)
            Plane[i] = QzVgaState.Font[i];
    }
    QzVgaClosePlane2();
}

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
    QzVgaSaveState();

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
    if (!QzVram || !QzDisplayOwned)
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

    if (!QzVram || !QzStatusEnabled || !QzDisplayOwned)
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
    InbvAcquireDisplayOwnership();
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

VOID
NTAPI
InbvAcquireDisplayOwnership(VOID)
{
    if (QzDisplayOwned)
        return;
    /* The owner puts the adapter back into text mode, or we do */
    if (!QzDisplayReset || !QzDisplayReset(QzCols, QzRows))
        QzVgaRestoreState();
    QzDisplayOwned = TRUE;
    QzVgaUpdateCursor();
}

BOOLEAN
NTAPI
InbvCheckDisplayOwnership(VOID)
{
    return QzDisplayOwned;
}

VOID
NTAPI
InbvNotifyDisplayOwnershipLost(IN INBV_RESET_DISPLAY_PARAMETERS Callback)
{
    QzDisplayReset = Callback;
    if (Callback)
        QzDisplayOwned = FALSE;
}

BOOLEAN
NTAPI
InbvIsBootDriverInstalled(VOID)
{
    return FALSE;       /* the console is VGA text, not BOOTVID */
}

BOOLEAN
NTAPI
InbvEnableDisplayString(IN BOOLEAN Enable)
{
    UNREFERENCED_PARAMETER(Enable);
    return TRUE;
}

BOOLEAN
NTAPI
InbvDisplayString(IN PCSTR String)
{
    DbgPrint("%s", String);
    return TRUE;
}
