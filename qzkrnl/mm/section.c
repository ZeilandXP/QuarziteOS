/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Section objects and mapped views
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qzio.h"
#include "qzps.h"

/* A view's VAD is going away (the pages are already unmapped) */
VOID
QzMmDeleteSectionView(PQZ_PROCESS Process, PQZ_VAD Vad)
{
    UNREFERENCED_PARAMETER(Process);
    if (Vad->Section)
        ObDereferenceObject(Vad->Section);
    Vad->Section = NULL;
}
