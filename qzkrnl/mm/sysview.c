/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Sections for kernel callers: Mm{Create,Map,Unmap}Section/View
 *              and views in system (and session) space
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * win32k keeps its heaps in sections it maps both into system space and
 * into its GUI processes, and maps font files into system space. A system
 * view maps the section's own pages (or, for a mapped file, the cache's)
 * at addresses of the window QZ_SYSVIEW_BASE. There is one session, so
 * session space is system space.
 */

#include "qzio.h"
#include "qzps.h"

#define QZ_SYSVIEW_TAG      'vSzQ'
#define QZ_SYSVIEW_BASE     0xB4000000UL
#define QZ_SYSVIEW_SIZE     (64UL * 1024 * 1024)
#define QZ_SYSVIEW_PAGES    (QZ_SYSVIEW_SIZE / PAGE_SIZE)

typedef struct _QZ_SYSVIEW
{
    LIST_ENTRY Link;
    ULONG_PTR Base;
    ULONG Pages;
    PQZ_SECTION Section;            /* referenced */
} QZ_SYSVIEW, *PQZ_SYSVIEW;

static ULONG QzSysViewBits[QZ_SYSVIEW_PAGES / 32];
static RTL_BITMAP QzSysViewMap;
static LIST_ENTRY QzSysViews;
static FAST_MUTEX QzSysViewLock;
static BOOLEAN QzSysViewReady;

static VOID
QzMmInitSysViews(VOID)
{
    if (QzSysViewReady)
        return;
    RtlInitializeBitMap(&QzSysViewMap, QzSysViewBits, QZ_SYSVIEW_PAGES);
    RtlClearAllBits(&QzSysViewMap);
    InitializeListHead(&QzSysViews);
    ExInitializeFastMutex(&QzSysViewLock);
    QzSysViewReady = TRUE;
}

NTSTATUS
NTAPI
MmCreateSection(OUT PVOID *SectionObject, IN ACCESS_MASK DesiredAccess, IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL,
                IN PLARGE_INTEGER MaximumSize, IN ULONG SectionPageProtection, IN ULONG AllocationAttributes,
                IN HANDLE FileHandle OPTIONAL, IN PFILE_OBJECT File OPTIONAL)
{
    PFILE_OBJECT FileObject = File;
    PQZ_SECTION Section;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DesiredAccess);
    UNREFERENCED_PARAMETER(ObjectAttributes);
    if (!FileObject && FileHandle)
    {
        Status = ObReferenceObjectByHandle(FileHandle, FILE_READ_DATA, IoFileObjectType, KernelMode,
                                           (PVOID *)&FileObject, NULL);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    else if (FileObject)
    {
        ObReferenceObject(FileObject);
    }
    if (!(AllocationAttributes & (SEC_IMAGE | SEC_COMMIT | SEC_RESERVE)))
        AllocationAttributes |= SEC_COMMIT;
    Status = QzMmCreateSection(&Section, MaximumSize, SectionPageProtection, AllocationAttributes, FileObject);
    if (FileObject)
        ObDereferenceObject(FileObject);
    if (NT_SUCCESS(Status))
        *SectionObject = Section;
    return Status;
}

NTSTATUS
NTAPI
MmMapViewOfSection(IN PVOID SectionObject, IN PEPROCESS Process, IN OUT PVOID *BaseAddress, IN ULONG_PTR ZeroBits,
                   IN SIZE_T CommitSize, IN OUT PLARGE_INTEGER SectionOffset OPTIONAL, IN OUT PSIZE_T ViewSize,
                   IN SECTION_INHERIT InheritDisposition, IN ULONG AllocationType, IN ULONG Protect)
{
    UNREFERENCED_PARAMETER(CommitSize);
    UNREFERENCED_PARAMETER(InheritDisposition);
    if ((PQZ_PROCESS)Process == QzSystemProcess)
        return MmMapViewInSystemSpace(SectionObject, BaseAddress, ViewSize);
    return QzMmMapView(SectionObject, (PQZ_PROCESS)Process, BaseAddress, ZeroBits, SectionOffset, ViewSize,
                       AllocationType & ~SEC_NO_CHANGE, Protect);
}

NTSTATUS
NTAPI
MmUnmapViewOfSection(IN PEPROCESS Process, IN PVOID BaseAddress)
{
    if ((PQZ_PROCESS)Process == QzSystemProcess)
        return MmUnmapViewInSystemSpace(BaseAddress);
    return QzMmUnmapView((PQZ_PROCESS)Process, BaseAddress);
}

/* The physical page of the section at Page (cache-backed: held, for a view) */
static NTSTATUS
QzMmSectionPage(PQZ_SECTION Section, ULONG Page, PULONG Pfn)
{
    if (Section->CacheMap)
        return QzCcMmGetPage(Section->CacheMap, (LONGLONG)Page << PAGE_SHIFT, Pfn);
    if (!Section->Pfns || Page >= Section->PageCount || !Section->Pfns[Page])
        return STATUS_INVALID_VIEW_SIZE;
    *Pfn = Section->Pfns[Page];
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
MmMapViewInSystemSpace(IN PVOID SectionObject, OUT PVOID *MappedBase, IN OUT PSIZE_T ViewSize)
{
    PQZ_SECTION Section = SectionObject;
    PQZ_SYSVIEW View;
    ULONG Pages, First, i, Pfn;
    NTSTATUS Status = STATUS_SUCCESS;
    SIZE_T Size = *ViewSize;

    QzMmInitSysViews();
    if (!Size || Size > Section->PageCount * PAGE_SIZE)
        Size = Section->Image ? Section->PageCount * PAGE_SIZE : (SIZE_T)Section->MaximumSize;
    Pages = (ULONG)((Size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!Pages || Pages > Section->PageCount)
        return STATUS_INVALID_VIEW_SIZE;
    View = ExAllocatePoolWithTag(NonPagedPool, sizeof(*View), QZ_SYSVIEW_TAG);
    if (!View)
        return STATUS_INSUFFICIENT_RESOURCES;

    ExAcquireFastMutex(&QzSysViewLock);
    First = RtlFindClearBitsAndSet(&QzSysViewMap, Pages, 0);
    ExReleaseFastMutex(&QzSysViewLock);
    if (First == 0xFFFFFFFF)
    {
        ExFreePoolWithTag(View, QZ_SYSVIEW_TAG);
        return STATUS_NO_MEMORY;
    }
    View->Base = QZ_SYSVIEW_BASE + First * PAGE_SIZE;
    View->Pages = Pages;
    View->Section = Section;
    for (i = 0; i < Pages && NT_SUCCESS(Status); i++)
    {
        Status = QzMmSectionPage(Section, i, &Pfn);
        if (NT_SUCCESS(Status))
            QzMapPage(View->Base + i * PAGE_SIZE, Pfn, QZ_PTE_WRITE);
    }
    if (!NT_SUCCESS(Status))
    {
        while (i-- > 1)
        {
            QzUnmapPage(View->Base + (i - 1) * PAGE_SIZE);
            if (Section->CacheMap)
                QzCcMmPutPage(Section->CacheMap, (LONGLONG)(i - 1) << PAGE_SHIFT, FALSE);
        }
        ExAcquireFastMutex(&QzSysViewLock);
        RtlClearBits(&QzSysViewMap, First, Pages);
        ExReleaseFastMutex(&QzSysViewLock);
        ExFreePoolWithTag(View, QZ_SYSVIEW_TAG);
        return Status;
    }
    ObReferenceObject(Section);
    InterlockedIncrement(&Section->Views);
    ExAcquireFastMutex(&QzSysViewLock);
    InsertTailList(&QzSysViews, &View->Link);
    ExReleaseFastMutex(&QzSysViewLock);
    *MappedBase = (PVOID)View->Base;
    *ViewSize = (SIZE_T)Pages * PAGE_SIZE;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
MmUnmapViewInSystemSpace(IN PVOID MappedBase)
{
    PQZ_SYSVIEW View = NULL;
    PLIST_ENTRY Entry;
    ULONG i;

    if (!QzSysViewReady)
        return STATUS_NOT_MAPPED_VIEW;
    ExAcquireFastMutex(&QzSysViewLock);
    for (Entry = QzSysViews.Flink; Entry != &QzSysViews; Entry = Entry->Flink)
    {
        PQZ_SYSVIEW Candidate = CONTAINING_RECORD(Entry, QZ_SYSVIEW, Link);
        if (Candidate->Base == (ULONG_PTR)MappedBase)
        {
            View = Candidate;
            RemoveEntryList(Entry);
            break;
        }
    }
    ExReleaseFastMutex(&QzSysViewLock);
    if (!View)
        return STATUS_NOT_MAPPED_VIEW;

    for (i = 0; i < View->Pages; i++)
    {
        ULONG_PTR Va = View->Base + i * PAGE_SIZE;
        ULONG Pte = *(volatile ULONG *)(QZ_PTE_BASE + (Va >> 12) * 4);
        QzUnmapPage(Va);
        if (View->Section->CacheMap)
            QzCcMmPutPage(View->Section->CacheMap, (LONGLONG)i << PAGE_SHIFT, (Pte & QZ_PTE_DIRTY) != 0);
    }
    ExAcquireFastMutex(&QzSysViewLock);
    RtlClearBits(&QzSysViewMap, (ULONG)((View->Base - QZ_SYSVIEW_BASE) / PAGE_SIZE), View->Pages);
    ExReleaseFastMutex(&QzSysViewLock);
    InterlockedDecrement(&View->Section->Views);
    ObDereferenceObject(View->Section);
    ExFreePoolWithTag(View, QZ_SYSVIEW_TAG);
    return STATUS_SUCCESS;
}

/* One session: session space is system space */
NTSTATUS
NTAPI
MmMapViewInSessionSpace(IN PVOID Section, OUT PVOID *MappedBase, IN OUT PSIZE_T ViewSize)
{
    return MmMapViewInSystemSpace(Section, MappedBase, ViewSize);
}

NTSTATUS
NTAPI
MmUnmapViewInSessionSpace(IN PVOID MappedBase)
{
    return MmUnmapViewInSystemSpace(MappedBase);
}

/* Everything is resident: securing user memory needs no more than a handle to give back */
HANDLE
NTAPI
MmSecureVirtualMemory(IN PVOID Address, IN SIZE_T Size, IN ULONG ProbeMode)
{
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(ProbeMode);
    return (HANDLE)((ULONG_PTR)Address | 1);
}

VOID
NTAPI
MmUnsecureVirtualMemory(IN HANDLE SecureMem)
{
    UNREFERENCED_PARAMETER(SecureMem);
}
