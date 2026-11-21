/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     RTL bitmaps (RTL_BITMAP) -- used by the HAL's DMA map buffers
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Straightforward bit-at-a-time implementations, written for clarity;
 * find routines search from the hint to the end, then wrap to the start.
 */

#include "qz.h"

#define QZ_NOT_FOUND 0xFFFFFFFF

static __inline BOOLEAN
QzBit(PRTL_BITMAP Header, ULONG Index)
{
    return (Header->Buffer[Index >> 5] >> (Index & 31)) & 1;
}

VOID
NTAPI
RtlInitializeBitMap(IN PRTL_BITMAP BitMapHeader,
                    IN PULONG BitMapBuffer,
                    IN ULONG SizeOfBitMap)
{
    BitMapHeader->SizeOfBitMap = SizeOfBitMap;
    BitMapHeader->Buffer = BitMapBuffer;
}

VOID
NTAPI
RtlClearAllBits(IN PRTL_BITMAP BitMapHeader)
{
    RtlZeroMemory(BitMapHeader->Buffer, ((BitMapHeader->SizeOfBitMap + 31) / 32) * sizeof(ULONG));
}

VOID
NTAPI
RtlSetAllBits(IN PRTL_BITMAP BitMapHeader)
{
    RtlFillMemory(BitMapHeader->Buffer, ((BitMapHeader->SizeOfBitMap + 31) / 32) * sizeof(ULONG), 0xFF);
}

VOID
NTAPI
RtlClearBit(IN PRTL_BITMAP BitMapHeader,
            IN ULONG BitNumber)
{
    if (BitNumber < BitMapHeader->SizeOfBitMap)
        BitMapHeader->Buffer[BitNumber >> 5] &= ~(1UL << (BitNumber & 31));
}

VOID
NTAPI
RtlSetBit(IN PRTL_BITMAP BitMapHeader,
          IN ULONG BitNumber)
{
    if (BitNumber < BitMapHeader->SizeOfBitMap)
        BitMapHeader->Buffer[BitNumber >> 5] |= (1UL << (BitNumber & 31));
}

BOOLEAN
NTAPI
RtlTestBit(IN PRTL_BITMAP BitMapHeader,
           IN ULONG BitNumber)
{
    return BitNumber < BitMapHeader->SizeOfBitMap && QzBit(BitMapHeader, BitNumber);
}

VOID
NTAPI
RtlClearBits(IN PRTL_BITMAP BitMapHeader,
             IN ULONG StartingIndex,
             IN ULONG NumberToClear)
{
    ULONG i;
    for (i = 0; i < NumberToClear; i++)
        RtlClearBit(BitMapHeader, StartingIndex + i);
}

VOID
NTAPI
RtlSetBits(IN PRTL_BITMAP BitMapHeader,
           IN ULONG StartingIndex,
           IN ULONG NumberToSet)
{
    ULONG i;
    for (i = 0; i < NumberToSet; i++)
        RtlSetBit(BitMapHeader, StartingIndex + i);
}

/* Find a run of NumberToFind bits equal to Value, starting near HintIndex */
static ULONG
QzFindRun(PRTL_BITMAP Header, ULONG NumberToFind, ULONG HintIndex, BOOLEAN Value)
{
    ULONG Size = Header->SizeOfBitMap;
    ULONG Pass, Start, End, i, Run;

    if (NumberToFind == 0 || NumberToFind > Size)
        return (NumberToFind == 0) ? 0 : QZ_NOT_FOUND;
    if (HintIndex >= Size)
        HintIndex = 0;

    /* Pass 0: [Hint, Size); pass 1: [0, Hint + NumberToFind) */
    for (Pass = 0; Pass < 2; Pass++)
    {
        Start = Pass ? 0 : HintIndex;
        End = Pass ? min(Size, HintIndex + NumberToFind - 1) : Size;
        Run = 0;
        for (i = Start; i < End; i++)
        {
            if (QzBit(Header, i) == Value)
            {
                if (++Run == NumberToFind)
                    return i + 1 - NumberToFind;
            }
            else
            {
                Run = 0;
            }
        }
    }
    return QZ_NOT_FOUND;
}

ULONG
NTAPI
RtlFindClearBits(IN PRTL_BITMAP BitMapHeader,
                 IN ULONG NumberToFind,
                 IN ULONG HintIndex)
{
    return QzFindRun(BitMapHeader, NumberToFind, HintIndex, FALSE);
}

ULONG
NTAPI
RtlFindSetBits(IN PRTL_BITMAP BitMapHeader,
               IN ULONG NumberToFind,
               IN ULONG HintIndex)
{
    return QzFindRun(BitMapHeader, NumberToFind, HintIndex, TRUE);
}

ULONG
NTAPI
RtlFindClearBitsAndSet(IN PRTL_BITMAP BitMapHeader,
                       IN ULONG NumberToFind,
                       IN ULONG HintIndex)
{
    ULONG Index = RtlFindClearBits(BitMapHeader, NumberToFind, HintIndex);
    if (Index != QZ_NOT_FOUND)
        RtlSetBits(BitMapHeader, Index, NumberToFind);
    return Index;
}

ULONG
NTAPI
RtlFindSetBitsAndClear(IN PRTL_BITMAP BitMapHeader,
                       IN ULONG NumberToFind,
                       IN ULONG HintIndex)
{
    ULONG Index = RtlFindSetBits(BitMapHeader, NumberToFind, HintIndex);
    if (Index != QZ_NOT_FOUND)
        RtlClearBits(BitMapHeader, Index, NumberToFind);
    return Index;
}

ULONG
NTAPI
RtlNumberOfSetBits(IN PRTL_BITMAP BitMapHeader)
{
    ULONG i, Count = 0;
    for (i = 0; i < BitMapHeader->SizeOfBitMap; i++)
        Count += QzBit(BitMapHeader, i);
    return Count;
}

ULONG
NTAPI
RtlNumberOfClearBits(IN PRTL_BITMAP BitMapHeader)
{
    return BitMapHeader->SizeOfBitMap - RtlNumberOfSetBits(BitMapHeader);
}

CCHAR
NTAPI
RtlFindLeastSignificantBit(IN ULONGLONG Set)
{
    CCHAR i;
    for (i = 0; i < 64; i++)
        if (Set & (1ULL << i))
            return i;
    return -1;
}

CCHAR
NTAPI
RtlFindMostSignificantBit(IN ULONGLONG Set)
{
    CCHAR i;
    for (i = 63; i >= 0; i--)
        if (Set & (1ULL << i))
            return i;
    return -1;
}
