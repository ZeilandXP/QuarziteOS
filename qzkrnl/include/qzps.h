/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Processes, address spaces, user mode (private to qzkrnl)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#pragma once

#include "qz.h"

/* ---------------------------------------------------------------------- */
/* User address space layout (x86, 2 GB user / 2 GB system, as NT)         */
/* ---------------------------------------------------------------------- */

#define QZ_USER_LOWEST          0x00010000UL
#define QZ_USER_HIGHEST         0x7FFEFFFFUL   /* MmHighestUserAddress */
#define QZ_USER_PROBE_ADDRESS   0x7FFF0000UL   /* MmUserProbeAddress */
#define QZ_SYSTEM_RANGE_START   0x80000000UL
#define QZ_USER_SHARED_DATA     0x7FFE0000UL   /* read-only alias of KI_USER_SHARED_DATA */
#define QZ_ALLOCATION_GRANULARITY 0x10000UL

#define QZ_PTE_USER             0x004
#define QZ_PTE_ACCESSED         0x020
#define QZ_PTE_DIRTY            0x040
#define QZ_PTE_PRIVATE          0x200          /* present: a page this process owns */
#define QZ_PTE_COMMITTED        0x400          /* not present: committed, Win32 protection << 12 */
#define QZ_PTE_COPY_ON_WRITE    0x800          /* present, read-only: a write gets a private copy */
#define QZ_SWPTE(Protect)       (QZ_PTE_COMMITTED | ((ULONG)(Protect) << 12))
#define QZ_SWPTE_PROTECT(Pte)   (((Pte) >> 12) & 0x7FF)

/* Access to the current address space's page tables (the self-map) */
#define QzPdeOf(Va)  ((volatile ULONG *)(QZ_PDE_BASE + ((ULONG_PTR)(Va) >> 22) * 4))
#define QzPteOf(Va)  ((volatile ULONG *)(QZ_PTE_BASE + ((ULONG_PTR)(Va) >> 12) * 4))

/* ---------------------------------------------------------------------- */
/* Virtual address descriptors                                             */
/* ---------------------------------------------------------------------- */

struct _QZ_SECTION;

typedef struct _QZ_VAD
{
    LIST_ENTRY Link;                /* the process's VadList, by address */
    ULONG_PTR Start;                /* first byte */
    ULONG_PTR End;                  /* one past the last byte (page aligned) */
    ULONG Type;                     /* MEM_PRIVATE, MEM_MAPPED or MEM_IMAGE */
    ULONG AllocationProtect;
    struct _QZ_SECTION *Section;    /* mapped views: referenced */
    ULONG_PTR SectionOffset;        /* bytes into the section */
} QZ_VAD, *PQZ_VAD;

/* ---------------------------------------------------------------------- */
/* Sections (mm/section.c)                                                 */
/* ---------------------------------------------------------------------- */

typedef struct _QZ_SECTION
{
    ULONG AllocationAttributes;     /* SEC_IMAGE, SEC_COMMIT, SEC_RESERVE, ... */
    ULONG Protect;                  /* the section's page protection (data) */
    ULONGLONG MaximumSize;
    ULONG PageCount;
    PULONG Pfns;                    /* the section's pages, shared by every view */
    PUSHORT PageProtect;            /* images: each page's Win32 protection */
    PFILE_OBJECT FileObject;        /* referenced, if file-backed */
    BOOLEAN Image;
    ULONG_PTR ImageBase;            /* images: the preferred base */
    SECTION_IMAGE_INFORMATION ImageInformation;
    volatile LONG Views;
    /* Mapped files (stage B7): the views map the cache's pages */
    PVOID CacheMap;                 /* cc/cache.c's map of the file, held */
    LIST_ENTRY FileLink;            /* QzMmFileSections */
    LIST_ENTRY ViewList;            /* QZ_SECTION_VIEW */
} QZ_SECTION, *PQZ_SECTION;

extern POBJECT_TYPE MmSectionObjectType;
VOID QzMmSectionInit(VOID);
NTSTATUS QzMmCreateSection(PQZ_SECTION *Result, PLARGE_INTEGER MaximumSize, ULONG Protect, ULONG Attributes,
                           PFILE_OBJECT FileObject);
NTSTATUS QzMmMapView(PQZ_SECTION Section, struct _QZ_PROCESS *Process, PVOID *BaseAddress, ULONG_PTR ZeroBits,
                     PLARGE_INTEGER SectionOffset, PSIZE_T ViewSize, ULONG AllocationType, ULONG Protect);
NTSTATUS QzMmUnmapView(struct _QZ_PROCESS *Process, PVOID BaseAddress);
VOID QzMmReleaseViewPages(struct _QZ_PROCESS *Process, PQZ_VAD Vad);   /* attached */
PVOID QzMmMapHyper(ULONG Pfn);       /* interrupts off around the use */
VOID QzMmUnmapHyper(VOID);
NTSTATUS QzMmCreateSectionFromMemory(PVOID Data, ULONG Size, PQZ_SECTION *Result);

/* ---------------------------------------------------------------------- */
/* Processes                                                               */
/* ---------------------------------------------------------------------- */

typedef struct _QZ_HANDLE_TABLE QZ_HANDLE_TABLE, *PQZ_HANDLE_TABLE;

typedef struct _QZ_PROCESS
{
    KEVENT Exited;                  /* must be first: waits on the process use it */
    LIST_ENTRY ProcessLink;         /* all processes */
    ULONG UniqueId;
    ULONG InheritedFromUniqueId;
    ULONG DirectoryTableBase;       /* physical address of the page directory */
    PQZ_HANDLE_TABLE HandleTable;
    LIST_ENTRY VadList;
    FAST_MUTEX AddressLock;         /* VadList and the page tables */
    ULONG PrivatePages;
    ULONG PageTablePages;
    LIST_ENTRY ThreadList;          /* QZ_PS_THREAD.ProcessLink */
    ULONG ActiveThreads;
    PVOID Peb;                      /* user address */
    PVOID SectionBaseAddress;       /* where the image is mapped */
    struct _QZ_SECTION *ImageSection;
    NTSTATUS ExitStatus;
    BOOLEAN Exiting;
    BOOLEAN AddressSpaceDeleted;
    ULONG HardErrorMode;
    ULONG_PTR Affinity;
    LONG BasePriority;
    PVOID Token;
    PVOID DebugPort;
    PVOID ExceptionPort;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER ExitTime;
    ULONG Cookie;
    CHAR ImageFileName[16];
    UNICODE_STRING ImagePathName;   /* \Device\...\smss.exe, for queries */
    PVOID SystemDllBase;            /* ntdll.dll, if mapped */
    ULONG_PTR EntryPoint;           /* the image's, once relocated */
    BOOLEAN RanImage;               /* the loader handed over to the image */
    ULONG_PTR NextTeb;              /* where the next TEB goes (downwards) */
} QZ_PROCESS, *PQZ_PROCESS;

/* The Thread object body (ps/thread.c) */
typedef struct _QZ_PS_THREAD
{
    KEVENT Terminated;              /* must be first: waits use it */
    PQZ_THREAD Thread;
    PQZ_PROCESS Process;
    LIST_ENTRY ProcessLink;
    PKSTART_ROUTINE StartRoutine;
    PVOID StartContext;
    NTSTATUS ExitStatus;
    ULONG UniqueId;
    PVOID Teb;                      /* user threads */
    BOOLEAN UserThread;
    LONG SuspendCount;
    KSEMAPHORE SuspendSemaphore;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER ExitTime;
    PVOID Win32StartAddress;
    BOOLEAN HardErrorsAreDisabled;
    BOOLEAN TerminatePending;       /* dies on its way back to user mode */
    NTSTATUS PendingExitStatus;
    PCONTEXT InitialContext;        /* user threads, until they first run */
    BOOLEAN ExitedThread;
} QZ_PS_THREAD, *PQZ_PS_THREAD;

extern PQZ_PROCESS QzSystemProcess;

/* ps/process.c */
VOID QzPsInitProcesses(VOID);
NTSTATUS QzPsCreateProcess(PCSTR Name, PQZ_PROCESS Parent, PQZ_PROCESS *Result);
PQZ_PROCESS QzPsLookupProcess(ULONG UniqueId);   /* referenced */
VOID QzPsDumpProcesses(VOID);
VOID QzPsAttach(PQZ_PROCESS Process, PQZ_PROCESS *Saved);
VOID QzPsDetach(PQZ_PROCESS Saved);
ULONG QzPsAllocateId(PVOID Object);
VOID QzPsFreeId(ULONG Id);
PVOID QzPsLookupId(ULONG Id);

/* ob/obhandle.c */
PQZ_HANDLE_TABLE QzObKernelHandleTable(VOID);
PQZ_HANDLE_TABLE QzObCreateHandleTable(VOID);
VOID QzObKillHandleTable(PQZ_HANDLE_TABLE Table);
ULONG QzObTableHandleCount(PQZ_HANDLE_TABLE Table);
NTSTATUS QzObInheritHandles(PQZ_HANDLE_TABLE Parent, PQZ_HANDLE_TABLE Child);

/* mm/vm.c */
VOID QzVmInit(VOID);
NTSTATUS QzMmCreateAddressSpace(PQZ_PROCESS Process);
VOID QzMmDeleteAddressSpace(PQZ_PROCESS Process);
VOID QzMmSwitchAddressSpace(PQZ_PROCESS Process);
NTSTATUS QzMmAccessFault(ULONG_PTR Address, ULONG ErrorCode, KPROCESSOR_MODE Mode);
NTSTATUS QzMmAllocate(PQZ_PROCESS Process, PVOID *BaseAddress, ULONG_PTR ZeroBits, PSIZE_T RegionSize,
                      ULONG AllocationType, ULONG Protect);
NTSTATUS QzMmFree(PQZ_PROCESS Process, PVOID *BaseAddress, PSIZE_T RegionSize, ULONG FreeType);
NTSTATUS QzMmProtect(PQZ_PROCESS Process, PVOID *BaseAddress, PSIZE_T RegionSize, ULONG NewProtect,
                     PULONG OldProtect);
NTSTATUS QzMmQuery(PQZ_PROCESS Process, PVOID Address, PMEMORY_BASIC_INFORMATION Info);
PQZ_VAD QzMmFindVad(PQZ_PROCESS Process, ULONG_PTR Address);
NTSTATUS QzMmInsertVad(PQZ_PROCESS Process, ULONG_PTR Start, SIZE_T Size, ULONG Type, ULONG Protect,
                       BOOLEAN TopDown, ULONG_PTR Limit, PQZ_VAD *Result);
VOID QzMmRemoveVad(PQZ_PROCESS Process, PQZ_VAD Vad);
ULONG QzMmProtectToPte(ULONG Protect);
NTSTATUS QzMmEnsurePageTable(ULONG_PTR Va);
VOID QzMmDumpAddressSpace(PQZ_PROCESS Process);

/* Run a kernel-side call with PreviousMode = KernelMode (the Zw* entry points) */
#define QZ_AS_KERNEL(Expression) \
    ({ KPROCESSOR_MODE _QzOld = QzCurrentThread->PreviousMode; NTSTATUS _QzStatus; \
       QzCurrentThread->PreviousMode = KernelMode; _QzStatus = (Expression); \
       QzCurrentThread->PreviousMode = _QzOld; _QzStatus; })

/* Arguments of a service: user callers' pointers are checked and copied */
BOOLEAN QzReadArg(KPROCESSOR_MODE Mode, PVOID Destination, const VOID *Source, SIZE_T Length);
BOOLEAN QzWriteArg(KPROCESSOR_MODE Mode, PVOID Destination, const VOID *Source, SIZE_T Length);

/* Copy to and from user memory: a fault on a bad address returns FALSE */
BOOLEAN QzCopyFromUser(PVOID Destination, const VOID *UserSource, SIZE_T Length);
BOOLEAN QzCopyToUser(PVOID UserDestination, const VOID *Source, SIZE_T Length);
BOOLEAN QzProbe(const VOID *Address, SIZE_T Length, BOOLEAN Write);

/* ke/user.c: user mode */
VOID QzUserException(PKTRAP_FRAME TrapFrame, NTSTATUS Code, ULONG_PTR Address, ULONG_PTR Write);
VOID FASTCALL QzExitToUserMode(PKTRAP_FRAME TrapFrame);
NTSTATUS QzPsCreateUserThread(PQZ_PROCESS Process, PCONTEXT Context, PVOID Teb, BOOLEAN Suspended,
                              PQZ_PS_THREAD *Result);
VOID QzSetTebSelector(PVOID Teb);           /* ke/i386/cpu.c */
VOID QzContextToTrapFrame(PCONTEXT Context, PKTRAP_FRAME TrapFrame, ULONG Flags);
VOID QzTrapFrameToContext(PKTRAP_FRAME TrapFrame, PCONTEXT Context);

/* ps/create.c */
NTSTATUS QzPsStartSmss(PQZ_PROCESS *Result);

/* ps/thread.c */
DECLSPEC_NORETURN VOID QzPsTerminateCurrentThread(NTSTATUS ExitStatus);
VOID QzPsTerminateProcess(PQZ_PROCESS Process, NTSTATUS ExitStatus);

/* ke/syscall.c */
extern BOOLEAN QzSyscallTrace;
VOID QzSyscallStats(VOID);
ULONG QzServiceNumber(PCSTR Name);

/* mm/vmtest.c, ke/usertest.c */
VOID QzRunVmTests(VOID);
VOID QzRunUserTests(VOID);
VOID QzRunSectionTests(VOID);
VOID QzRunLpcTests(VOID);
VOID QzRunLoaderTests(VOID);
VOID QzRunFsTests(PCSTR Volume);
VOID QzIoShellInput(PCSTR Which, ULONG Milliseconds);
