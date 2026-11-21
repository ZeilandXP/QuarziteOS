/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Address space self-test (shell command "vmtest")
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Two processes with nothing in them but memory: reserving, committing,
 * touching (demand zero), protecting, guard pages, decommitting and
 * releasing, through the same code the Nt*VirtualMemory services use;
 * isolation between the address spaces; the fault-safe user copies;
 * handles made inside a process; no pages lost when the processes go.
 */

#include "qzio.h"
#include "qzps.h"

static ULONG QzPassed, QzFailed;

static VOID
QzCheck(PCSTR Name, BOOLEAN Condition)
{
    if (Condition)
        QzPassed++;
    else
        QzFailed++;
    QzPrint("  [%s] %s\n", Condition ? "PASS" : "FAIL", Name);
}

static BOOLEAN
QzvQuery(PQZ_PROCESS Process, ULONG_PTR Address, ULONG State, ULONG Protect, SIZE_T Size)
{
    MEMORY_BASIC_INFORMATION Info;
    if (!NT_SUCCESS(QzMmQuery(Process, (PVOID)Address, &Info)))
        return FALSE;
    if (Info.State != State || (Protect != (ULONG)-1 && Info.Protect != Protect))
        return FALSE;
    return Size == 0 || Info.RegionSize == Size;
}

static VOID
QzvTests(VOID)
{
    PQZ_PROCESS A = NULL, B = NULL, Saved;
    PVOID Base = NULL, Other, Top;
    SIZE_T Size = 1024 * 1024, Pages;
    ULONG Old, Value = 0, FreeBefore, FreeAfter, Total;
    ULONG_PTR Page;
    NTSTATUS Status;

    QzMmGetStats(&Total, &FreeBefore);
    QzCheck("create two processes", NT_SUCCESS(QzPsCreateProcess("vmtest-a", QzSystemProcess, &A)) &&
            NT_SUCCESS(QzPsCreateProcess("vmtest-b", QzSystemProcess, &B)));
    if (!A || !B)
        goto Out;
    QzCheck("each has its own page directory", A->DirectoryTableBase && B->DirectoryTableBase &&
            A->DirectoryTableBase != B->DirectoryTableBase &&
            A->DirectoryTableBase != QzSystemProcess->DirectoryTableBase);

    /* Reserve and commit */
    Status = QzMmAllocate(A, &Base, 0, &Size, MEM_RESERVE, PAGE_READWRITE);
    QzCheck("reserve 1 MB", NT_SUCCESS(Status) && Base && !((ULONG_PTR)Base & 0xFFFF) &&
            Size == 1024 * 1024 && (ULONG_PTR)Base < QZ_SYSTEM_RANGE_START);
    if (!NT_SUCCESS(Status))
        goto Out;
    QzCheck("the range is reserved", QzvQuery(A, (ULONG_PTR)Base, MEM_RESERVE, 0, 1024 * 1024));
    Page = (ULONG_PTR)Base + 0x10000;
    Other = (PVOID)Page;
    Pages = 3 * PAGE_SIZE;
    Status = QzMmAllocate(A, &Other, 0, &Pages, MEM_COMMIT, PAGE_READWRITE);
    QzCheck("commit 3 pages inside it", NT_SUCCESS(Status) && Other == (PVOID)Page && Pages == 3 * PAGE_SIZE &&
            QzvQuery(A, Page, MEM_COMMIT, PAGE_READWRITE, 3 * PAGE_SIZE) &&
            QzvQuery(A, (ULONG_PTR)Base, MEM_RESERVE, 0, 0x10000));

    /* Touch: demand-zero, then our data */
    QzPsAttach(A, &Saved);
    Value = 1;
    QzCheck("a new page reads as zero", QzCopyFromUser(&Value, (PVOID)Page, sizeof(Value)) && Value == 0);
    Value = 0xA11CE;
    QzCheck("write into process A", QzCopyToUser((PVOID)(Page + 8), &Value, sizeof(Value)));
    QzCheck("an uncommitted page is refused", !QzCopyFromUser(&Value, Base, sizeof(Value)));
    QzCheck("a kernel address is refused", !QzCopyFromUser(&Value, (PVOID)QZ_POOL_BASE, sizeof(Value)));
    Value = 0;
    QzCheck("shared user data at 0x7FFE0000", QzCopyFromUser(&Value, (PVOID)(QZ_USER_SHARED_DATA + 0x26C),
            sizeof(Value)) && Value == SharedUserData->NtMajorVersion);
    QzPsDetach(Saved);
    QzCheck("A owns one private page", A->PrivatePages == 1);

    /* The same address in B is another page */
    Other = (PVOID)Page;
    Pages = PAGE_SIZE;
    Status = QzMmAllocate(B, &Other, 0, &Pages, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    QzCheck("B allocates at the same address", NT_SUCCESS(Status) && (ULONG_PTR)Other == (Page & ~0xFFFFUL));
    QzPsAttach(B, &Saved);
    Value = 0xB0B;
    QzCopyToUser((PVOID)(Page + 8), &Value, sizeof(Value));
    QzPsDetach(Saved);
    QzPsAttach(A, &Saved);
    Value = 0;
    QzCopyFromUser(&Value, (PVOID)(Page + 8), sizeof(Value));
    QzPsDetach(Saved);
    QzCheck("address spaces are isolated", Value == 0xA11CE);

    /* Protection */
    Other = (PVOID)Page;
    Pages = PAGE_SIZE;
    Status = QzMmProtect(A, &Other, &Pages, PAGE_READONLY, &Old);
    QzCheck("protect a page read-only", NT_SUCCESS(Status) && Old == PAGE_READWRITE &&
            QzvQuery(A, Page, MEM_COMMIT, PAGE_READONLY, PAGE_SIZE));
    QzPsAttach(A, &Saved);
    Value = 0;
    QzCheck("a read-only page reads", QzCopyFromUser(&Value, (PVOID)(Page + 8), sizeof(Value)) && Value == 0xA11CE);
    QzPsDetach(Saved);
    Other = (PVOID)Base;
    Pages = PAGE_SIZE;
    QzCheck("protecting reserved memory fails", QzMmProtect(A, &Other, &Pages, PAGE_READONLY, &Old) ==
            STATUS_NOT_COMMITTED);

    /* A guard page fires once */
    Other = (PVOID)(Page + 2 * PAGE_SIZE);
    Pages = PAGE_SIZE;
    QzMmProtect(A, &Other, &Pages, PAGE_READWRITE | PAGE_GUARD, &Old);
    QzCheck("a guard page", QzvQuery(A, Page + 2 * PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD, PAGE_SIZE));
    QzPsAttach(A, &Saved);
    QzCopyFromUser(&Value, (PVOID)(Page + 2 * PAGE_SIZE), sizeof(Value));
    QzPsDetach(Saved);
    QzCheck("touching it turns it into a normal page",
            QzvQuery(A, Page + 2 * PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE, PAGE_SIZE));

    /* Decommit and release */
    Other = (PVOID)Page;
    Pages = PAGE_SIZE;
    Status = QzMmFree(A, &Other, &Pages, MEM_DECOMMIT);
    QzCheck("decommit a page", NT_SUCCESS(Status) && QzvQuery(A, Page, MEM_RESERVE, 0, PAGE_SIZE));
    Other = (PVOID)((ULONG_PTR)Base + 0x80000);
    Pages = 0x10000;
    Status = QzMmFree(A, &Other, &Pages, MEM_RELEASE);
    QzCheck("release the middle of a range splits it", NT_SUCCESS(Status) &&
            QzvQuery(A, (ULONG_PTR)Base + 0x80000, MEM_FREE, -1, 0x10000) &&
            QzvQuery(A, (ULONG_PTR)Base + 0x90000, MEM_RESERVE, 0, 0));
    Other = Base;
    Pages = 0;
    QzCheck("release at the base frees the first part", NT_SUCCESS(QzMmFree(A, &Other, &Pages, MEM_RELEASE)) &&
            Pages == 0x80000 && QzvQuery(A, (ULONG_PTR)Base, MEM_FREE, -1, 0));
    Other = (PVOID)((ULONG_PTR)Base + 0x100);
    Pages = 0;
    QzCheck("release not at a base fails", QzMmFree(A, &Other, &Pages, MEM_RELEASE) != STATUS_SUCCESS);

    /* Top down, and a conflicting address */
    Top = NULL;
    Pages = 0x20000;
    Status = QzMmAllocate(A, &Top, 0, &Pages, MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE);
    QzCheck("MEM_TOP_DOWN allocates high", NT_SUCCESS(Status) && (ULONG_PTR)Top >= 0x7FF00000UL &&
            (ULONG_PTR)Top + Pages <= QZ_USER_SHARED_DATA);
    Other = Top;
    Pages = PAGE_SIZE;
    QzCheck("reserving over a reservation fails", QzMmAllocate(A, &Other, 0, &Pages, MEM_RESERVE, PAGE_READWRITE) ==
            STATUS_CONFLICTING_ADDRESSES);
    Other = NULL;
    Pages = PAGE_SIZE;
    QzCheck("a bad protection is refused", QzMmAllocate(A, &Other, 0, &Pages, MEM_COMMIT, 0x1234) ==
            STATUS_INVALID_PAGE_PROTECTION);

    /* Handles in a process */
    {
        OBJECT_ATTRIBUTES Attributes;
        HANDLE Event = NULL, Kernel = NULL;
        PVOID Object;

        QzPsAttach(A, &Saved);
        InitializeObjectAttributes(&Attributes, NULL, 0, NULL, NULL);
        ZwCreateEvent(&Event, EVENT_ALL_ACCESS, &Attributes, NotificationEvent, FALSE);
        InitializeObjectAttributes(&Attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        ZwCreateEvent(&Kernel, EVENT_ALL_ACCESS, &Attributes, NotificationEvent, FALSE);
        QzCheck("a handle made in a process is that process's",
                Event && !((ULONG_PTR)Event & 0x80000000) && QzObTableHandleCount(A->HandleTable) == 1);
        QzCheck("user mode cannot use a kernel handle",
                ObReferenceObjectByHandle(Kernel, 0, NULL, UserMode, &Object, NULL) == STATUS_INVALID_HANDLE);
        QzPsDetach(Saved);
        QzCheck("a kernel handle works from System",
                Kernel && ((ULONG_PTR)Kernel & 0x80000000) &&
                NT_SUCCESS(ObReferenceObjectByHandle(Kernel, 0, NULL, KernelMode, &Object, NULL)));
        if (Kernel)
        {
            ObDereferenceObject(Object);
            ZwClose(Kernel);
        }
        /* Event stays in A's table: it goes with the process */
    }

    {
        PEPROCESS Found = NULL;
        QzCheck("look the process up by its ID",
                NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)A->UniqueId, &Found)) &&
                Found == (PEPROCESS)A && PsGetProcessId(Found) == (HANDLE)(ULONG_PTR)A->UniqueId);
        if (Found)
            ObDereferenceObject(Found);
    }

Out:
    if (A)
        ObDereferenceObject(A);
    if (B)
        ObDereferenceObject(B);
    QzMmGetStats(&Total, &FreeAfter);
    QzCheck("no pages are lost when the processes go", FreeAfter + 2 >= FreeBefore);
    if (FreeAfter + 2 < FreeBefore)
        QzPrint("    (%lu page(s) before, %lu after)\n", FreeBefore, FreeAfter);
}

VOID
QzRunVmTests(VOID)
{
    QzPassed = QzFailed = 0;
    QzvTests();
    QzPrint("  vmtest: %lu passed, %lu failed\n", QzPassed, QzFailed);
}
