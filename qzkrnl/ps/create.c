/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     User processes: the system DLL, PEB and TEB, process and
 *              thread creation, the initial thread APC, user exception
 *              dispatch, and the first process (smss.exe)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * As on NT, every user process gets ntdll.dll mapped by the kernel, a PEB
 * and the NLS tables. A new user thread does not start at its start
 * address: the kernel first delivers a user APC to ntdll's
 * LdrInitializeThunk (through KiUserApcDispatcher). The loader initializes
 * the process (or the thread) and NtContinue()s into the thread's real
 * context. Exceptions raised in ring 3 go to KiUserExceptionDispatcher
 * with an EXCEPTION_RECORD and a CONTEXT on the user stack.
 */

#include "qzio.h"
#include "qzps.h"

#define QZ_PS_TAG       'cPzQ'
#define QZ_PEB_ADDRESS  0x7FFDF000UL
#define QZ_FIRST_TEB    0x7FFDE000UL

static PQZ_SECTION QzSystemDllSection;
static ULONG_PTR QzSystemDllBase;
static ULONG_PTR QzLdrInitializeThunk, QzKiUserApcDispatcher, QzKiUserExceptionDispatcher;
static ULONG_PTR QzKiUserCallbackDispatcher, QzKiRaiseUserExceptionDispatcher;

/* ke/apc.c: where user APCs enter ring 3 */
ULONG_PTR
QzPsUserApcDispatcher(VOID)
{
    return QzKiUserApcDispatcher;
}

/* ke/callback.c: where KeUserModeCallback enters ring 3 */
ULONG_PTR
QzPsUserCallbackDispatcher(VOID)
{
    return QzKiUserCallbackDispatcher;
}
static PQZ_SECTION QzNlsSection;
static ULONG QzNlsAnsiOffset, QzNlsOemOffset, QzNlsCaseOffset;

/* ---------------------------------------------------------------------- */
/* Pages at fixed places (PEB, TEBs)                                       */
/* ---------------------------------------------------------------------- */

/* One committed read-write page at Address exactly */
static NTSTATUS
QzPsAllocatePage(PQZ_PROCESS Process, ULONG_PTR Address)
{
    PVOID Base = (PVOID)Address;
    SIZE_T Size = PAGE_SIZE;
    PQZ_VAD Vad;
    NTSTATUS Status;

    ExAcquireFastMutex(&Process->AddressLock);
    Status = QzMmInsertVad(Process, Address, PAGE_SIZE, MEM_PRIVATE, PAGE_READWRITE, FALSE, 0, &Vad);
    ExReleaseFastMutex(&Process->AddressLock);
    if (!NT_SUCCESS(Status))
        return Status;
    return QzMmAllocate(Process, &Base, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
}

/* The next free page below the PEB, for a TEB */
static NTSTATUS
QzPsAllocateTebPage(PQZ_PROCESS Process, PULONG_PTR Teb)
{
    ULONG_PTR Address;

    for (Address = Process->NextTeb ? Process->NextTeb : QZ_FIRST_TEB; Address >= 0x7FF00000UL;
         Address -= PAGE_SIZE)
    {
        if (NT_SUCCESS(QzPsAllocatePage(Process, Address)))
        {
            Process->NextTeb = Address - PAGE_SIZE;
            *Teb = Address;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_NO_MEMORY;
}

/* ---------------------------------------------------------------------- */
/* The system DLL and the NLS tables                                       */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzPsOpenFile(PCWSTR Path, PFILE_OBJECT *FileObject)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK IoStatus;
    HANDLE Handle;
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenFile(&Handle, FILE_READ_DATA | FILE_EXECUTE | SYNCHRONIZE, &Attributes, &IoStatus,
                        FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = ObReferenceObjectByHandle(Handle, 0, IoFileObjectType, KernelMode, (PVOID *)FileObject, NULL);
    ZwClose(Handle);
    return Status;
}

/* The NLS tables the loader read, as one section each process maps */
static NTSTATUS
QzPsInitNls(VOID)
{
    PNLS_DATA_BLOCK Nls = QzLoaderBlock->NlsData;
    PLIST_ENTRY Entry;
    ULONG_PTR Base;
    ULONG Pages = 0;

    if (!Nls || !Nls->AnsiCodePageData)
        return STATUS_UNSUCCESSFUL;
    for (Entry = QzLoaderBlock->MemoryDescriptorListHead.Flink;
         Entry != &QzLoaderBlock->MemoryDescriptorListHead; Entry = Entry->Flink)
    {
        PMEMORY_ALLOCATION_DESCRIPTOR Md = CONTAINING_RECORD(Entry, MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);
        if (Md->MemoryType == LoaderNlsData)
            Pages += Md->PageCount;
    }
    Base = (ULONG_PTR)Nls->AnsiCodePageData;
    if ((ULONG_PTR)Nls->OemCodePageData < Base)
        Base = (ULONG_PTR)Nls->OemCodePageData;
    if ((ULONG_PTR)Nls->UnicodeCodePageData < Base)
        Base = (ULONG_PTR)Nls->UnicodeCodePageData;
    QzNlsAnsiOffset = (ULONG)((ULONG_PTR)Nls->AnsiCodePageData - Base);
    QzNlsOemOffset = (ULONG)((ULONG_PTR)Nls->OemCodePageData - Base);
    QzNlsCaseOffset = (ULONG)((ULONG_PTR)Nls->UnicodeCodePageData - Base);
    if (!Pages)
        Pages = (max(QzNlsAnsiOffset, max(QzNlsOemOffset, QzNlsCaseOffset)) >> PAGE_SHIFT) + 64;
    return QzMmCreateSectionFromMemory((PVOID)Base, Pages * PAGE_SIZE, &QzNlsSection);
}

/* ntdll.dll: an image section every process maps, and its entry points */
static NTSTATUS
QzPsInitSystemDll(VOID)
{
    PFILE_OBJECT FileObject;
    PQZ_PROCESS Scratch, Saved;
    PVOID Base = NULL;
    SIZE_T Size = 0;
    ULONG_PTR FastCall, FastCallRet, IntCall;
    extern BOOLEAN QzSysenterAvailable;
    NTSTATUS Status;

    if (QzSystemDllSection)
        return STATUS_SUCCESS;
    Status = QzPsOpenFile(L"\\SystemRoot\\system32\\ntdll.dll", &FileObject);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = QzMmCreateSection(&QzSystemDllSection, NULL, PAGE_EXECUTE, SEC_IMAGE, FileObject);
    ObDereferenceObject(FileObject);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Read its exports from a view (at its base, where every process has it) */
    Status = QzPsCreateProcess("ntdll-scan", QzSystemProcess, &Scratch);
    if (NT_SUCCESS(Status))
        Status = QzMmMapView(QzSystemDllSection, Scratch, &Base, 0, NULL, &Size, 0, PAGE_EXECUTE_WRITECOPY);
    if (!NT_SUCCESS(Status))
        return Status;
    QzSystemDllBase = (ULONG_PTR)Base;
    QzPsAttach(Scratch, &Saved);
    QzLdrInitializeThunk = (ULONG_PTR)QzFindImageExport(Base, "LdrInitializeThunk");
    QzKiUserApcDispatcher = (ULONG_PTR)QzFindImageExport(Base, "KiUserApcDispatcher");
    QzKiUserExceptionDispatcher = (ULONG_PTR)QzFindImageExport(Base, "KiUserExceptionDispatcher");
    QzKiUserCallbackDispatcher = (ULONG_PTR)QzFindImageExport(Base, "KiUserCallbackDispatcher");
    QzKiRaiseUserExceptionDispatcher = (ULONG_PTR)QzFindImageExport(Base, "KiRaiseUserExceptionDispatcher");
    FastCall = (ULONG_PTR)QzFindImageExport(Base, "KiFastSystemCall");
    FastCallRet = (ULONG_PTR)QzFindImageExport(Base, "KiFastSystemCallRet");
    IntCall = (ULONG_PTR)QzFindImageExport(Base, "KiIntSystemCall");
    QzPsDetach(Saved);
    ObDereferenceObject(Scratch);

    if (!QzLdrInitializeThunk || !QzKiUserApcDispatcher || !QzKiUserExceptionDispatcher || !IntCall)
        return STATUS_PROCEDURE_NOT_FOUND;

    /* How ntdll's stubs enter the kernel */
    SharedUserData->SystemCall = (QzSysenterAvailable && FastCall) ? FastCall : IntCall;
    SharedUserData->SystemCallReturn = FastCallRet;
    SharedUserData->TestRetInstruction = 0xC3;
    DbgPrint(QZ_KERNEL_NAME ": ps: ntdll.dll at %p, LdrInitializeThunk %p, system calls via %s\n", Base,
             (PVOID)QzLdrInitializeThunk, SharedUserData->SystemCall == FastCall ? "sysenter" : "int 2Eh");

    Status = QzPsInitNls();
    if (!NT_SUCCESS(Status))
        DbgPrint(QZ_KERNEL_NAME ": ps: no NLS tables from the loader (0x%08lx)\n", Status);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Process creation                                                        */
/* ---------------------------------------------------------------------- */

/* The PEB the kernel fills in (MmCreatePeb's part) */
static NTSTATUS
QzPsCreatePeb(PQZ_PROCESS Process, PVOID NlsBase, PSECTION_IMAGE_INFORMATION Image)
{
    PQZ_PROCESS Saved;
    PEB Peb;
    BOOLEAN Ok;
    NTSTATUS Status = QzPsAllocatePage(Process, QZ_PEB_ADDRESS);

    if (!NT_SUCCESS(Status))
        return Status;
    RtlZeroMemory(&Peb, sizeof(Peb));
    Peb.ImageBaseAddress = Process->SectionBaseAddress;
    Peb.Mutant = (HANDLE)-1;
    if (NlsBase)
    {
        Peb.AnsiCodePageData = (PCHAR)NlsBase + QzNlsAnsiOffset;
        Peb.OemCodePageData = (PCHAR)NlsBase + QzNlsOemOffset;
        Peb.UnicodeCaseTableData = (PCHAR)NlsBase + QzNlsCaseOffset;
    }
    Peb.OSMajorVersion = QZ_NT_MAJOR;
    Peb.OSMinorVersion = QZ_NT_MINOR;
    Peb.OSBuildNumber = QZ_NT_BUILD;
    Peb.OSPlatformId = VER_PLATFORM_WIN32_NT;
    Peb.OSCSDVersion = QZ_NT_CSD_VERSION;
    Peb.NumberOfProcessors = 1;
    Peb.ImageProcessAffinityMask = 1;
    Peb.NtGlobalFlag = 0;
    Peb.HeapSegmentReserve = 0x100000;
    Peb.HeapSegmentCommit = 0x2000;
    Peb.HeapDeCommitTotalFreeThreshold = 0x10000;
    Peb.HeapDeCommitFreeBlockThreshold = 0x1000;
    Peb.CriticalSectionTimeout.QuadPart = -2592000LL * 10000000LL;   /* 30 days */
    Peb.MaximumNumberOfHeaps = (PAGE_SIZE - sizeof(PEB)) / sizeof(PVOID);
    Peb.ProcessHeaps = (PVOID *)(QZ_PEB_ADDRESS + sizeof(PEB));
    Peb.SessionId = 0;
    if (Image)
    {
        Peb.ImageSubsystem = Image->SubSystemType;
        Peb.ImageSubsystemMajorVersion = Image->SubSystemMajorVersion;
        Peb.ImageSubsystemMinorVersion = Image->SubSystemMinorVersion;
    }

    QzPsAttach(Process, &Saved);
    Ok = QzCopyToUser((PVOID)QZ_PEB_ADDRESS, &Peb, sizeof(Peb));
    QzPsDetach(Saved);
    if (!Ok)
        return STATUS_ACCESS_VIOLATION;
    Process->Peb = (PVOID)QZ_PEB_ADDRESS;
    return STATUS_SUCCESS;
}

/* A user process running the image in Section, with ntdll and a PEB */
static NTSTATUS
QzPsCreateUserProcess(PQZ_PROCESS Parent, PQZ_SECTION Section, BOOLEAN InheritHandles, PCSTR Name,
                      PQZ_PROCESS *Result)
{
    PQZ_PROCESS Process;
    PVOID ImageBase = NULL, DllBase = NULL, NlsBase = NULL;
    SIZE_T Size = 0;
    NTSTATUS Status;

    *Result = NULL;
    Status = QzPsInitSystemDll();
    if (!NT_SUCCESS(Status))
        return Status;
    if (!Section || !Section->Image)
        return STATUS_INVALID_PARAMETER_6;   /* fork-style processes: not yet */
    Status = QzPsCreateProcess(Name, Parent, &Process);
    if (!NT_SUCCESS(Status))
        return Status;

    /* The image (an EXE without relocations needs its base), ntdll, NLS */
    Status = QzMmMapView(Section, Process, &ImageBase, 0, NULL, &Size, 0, PAGE_EXECUTE_WRITECOPY);
    if (NT_SUCCESS(Status))
    {
        Process->SectionBaseAddress = ImageBase;
        Process->EntryPoint = (ULONG_PTR)Section->ImageInformation.TransferAddress +
                              ((ULONG_PTR)ImageBase - Section->ImageBase);
        Process->ImageSection = Section;
        ObReferenceObject(Section);
        Size = 0;
        Status = QzMmMapView(QzSystemDllSection, Process, &DllBase, 0, NULL, &Size, 0, PAGE_EXECUTE_WRITECOPY);
        if (Status != STATUS_SUCCESS)
            Status = NT_SUCCESS(Status) ? STATUS_CONFLICTING_ADDRESSES : Status;   /* ntdll must be at its base */
    }
    if (NT_SUCCESS(Status))
    {
        Process->SystemDllBase = DllBase;
        if (QzNlsSection)
        {
            /* High, as on NT: the low megabyte is for RTL to reserve */
            Size = 0;
            NlsBase = (PVOID)0x7F6F0000;
            if (!NT_SUCCESS(QzMmMapView(QzNlsSection, Process, &NlsBase, 0, NULL, &Size, 0, PAGE_READONLY)))
            {
                NlsBase = NULL;
                Size = 0;
                QzMmMapView(QzNlsSection, Process, &NlsBase, 0, NULL, &Size, MEM_TOP_DOWN, PAGE_READONLY);
            }
        }
        Status = QzPsCreatePeb(Process, NlsBase, &Section->ImageInformation);
    }
    if (NT_SUCCESS(Status) && InheritHandles && Parent && Parent != QzSystemProcess)
        Status = QzObInheritHandles(Parent->HandleTable, Process->HandleTable);
    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(Process);
        return Status;
    }
    *Result = Process;
    return STATUS_SUCCESS;
}

/* "smss.exe" from a section's file name, for ps */
static VOID
QzPsNameFromSection(PQZ_SECTION Section, PCHAR Name, ULONG Size)
{
    PUNICODE_STRING FileName = Section->FileObject ? &Section->FileObject->FileName : NULL;
    ULONG i, Start = 0, Out = 0;

    Name[0] = 0;
    if (!FileName || !FileName->Length)
        return;
    for (i = 0; i < FileName->Length / sizeof(WCHAR); i++)
        if (FileName->Buffer[i] == L'\\')
            Start = i + 1;
    for (i = Start; i < FileName->Length / sizeof(WCHAR) && Out + 1 < Size; i++)
        Name[Out++] = (CHAR)FileName->Buffer[i];
    Name[Out] = 0;
}

static NTSTATUS
QzPsCreateProcessService(PHANDLE UProcessHandle, ACCESS_MASK DesiredAccess, HANDLE ParentProcess,
                         BOOLEAN InheritHandles, HANDLE SectionHandle)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PQZ_PROCESS Parent, Process;
    PQZ_SECTION Section = NULL;
    CHAR Name[16];
    HANDLE Handle;
    NTSTATUS Status;

    Status = ObReferenceObjectByHandle(ParentProcess, PROCESS_CREATE_PROCESS, PsProcessType, Mode,
                                       (PVOID *)&Parent, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    if (SectionHandle)
        Status = ObReferenceObjectByHandle(SectionHandle, SECTION_MAP_EXECUTE, MmSectionObjectType, Mode,
                                           (PVOID *)&Section, NULL);
    if (NT_SUCCESS(Status))
    {
        if (Section)
            QzPsNameFromSection(Section, Name, sizeof(Name));
        Status = QzPsCreateUserProcess(Parent, Section, InheritHandles, Section && Name[0] ? Name : "process",
                                       &Process);
        if (Section)
            ObDereferenceObject(Section);
    }
    ObDereferenceObject(Parent);
    if (!NT_SUCCESS(Status))
        return Status;
    if (Section && Section->FileObject && Section->FileObject->FileName.Length)
    {
        PUNICODE_STRING From = &Section->FileObject->FileName;
        Process->ImagePathName.Buffer = ExAllocatePoolWithTag(NonPagedPool, From->Length, QZ_PS_TAG);
        if (Process->ImagePathName.Buffer)
        {
            RtlCopyMemory(Process->ImagePathName.Buffer, From->Buffer, From->Length);
            Process->ImagePathName.Length = Process->ImagePathName.MaximumLength = From->Length;
        }
    }
    /* As ReactOS's kernel does, the creator's TEB gets the new PEB's address:
       kernel32 reads it from NtTib.ArbitraryUserPointer */
    if (Mode == UserMode && Process->Peb && QzCurrentThread->PsThread &&
        ((PQZ_PS_THREAD)QzCurrentThread->PsThread)->Teb)
    {
        PTEB Teb = ((PQZ_PS_THREAD)QzCurrentThread->PsThread)->Teb;
        QzCopyToUser(&Teb->NtTib.ArbitraryUserPointer, &Process->Peb, sizeof(PVOID));
    }
    Status = ObOpenObjectByPointer(Process, 0, NULL, DesiredAccess ? DesiredAccess : PROCESS_ALL_ACCESS,
                                   PsProcessType, Mode, &Handle);
    ObDereferenceObject(Process);
    if (NT_SUCCESS(Status) && !QzWriteArg(Mode, UProcessHandle, &Handle, sizeof(Handle)))
        Status = STATUS_ACCESS_VIOLATION;
    return Status;
}

NTSTATUS
NTAPI
NtCreateProcess(OUT PHANDLE ProcessHandle,
                IN ACCESS_MASK DesiredAccess,
                IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL,
                IN HANDLE ParentProcess,
                IN BOOLEAN InheritObjectTable,
                IN HANDLE SectionHandle OPTIONAL,
                IN HANDLE DebugPort OPTIONAL,
                IN HANDLE ExceptionPort OPTIONAL)
{
    UNREFERENCED_PARAMETER(ObjectAttributes);
    UNREFERENCED_PARAMETER(DebugPort);
    UNREFERENCED_PARAMETER(ExceptionPort);
    return QzPsCreateProcessService(ProcessHandle, DesiredAccess, ParentProcess, InheritObjectTable,
                                    SectionHandle);
}

NTSTATUS
NTAPI
NtCreateProcessEx(OUT PHANDLE ProcessHandle,
                  IN ACCESS_MASK DesiredAccess,
                  IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL,
                  IN HANDLE ParentProcess,
                  IN ULONG Flags,
                  IN HANDLE SectionHandle OPTIONAL,
                  IN HANDLE DebugPort OPTIONAL,
                  IN HANDLE ExceptionPort OPTIONAL,
                  IN BOOLEAN InJob)
{
    UNREFERENCED_PARAMETER(ObjectAttributes);
    UNREFERENCED_PARAMETER(DebugPort);
    UNREFERENCED_PARAMETER(ExceptionPort);
    UNREFERENCED_PARAMETER(InJob);
    return QzPsCreateProcessService(ProcessHandle, DesiredAccess, ParentProcess, (Flags & 4) != 0,
                                    SectionHandle);
}

/* ---------------------------------------------------------------------- */
/* Threads                                                                 */
/* ---------------------------------------------------------------------- */

/* The TEB the kernel fills in (MmCreateTeb's part) */
static NTSTATUS
QzPsCreateTeb(PQZ_PROCESS Process, PQZ_PS_THREAD PsThread, PINITIAL_TEB InitialTeb, PULONG_PTR Result)
{
    PQZ_PROCESS Saved;
    ULONG_PTR Address;
    PTEB Teb;
    BOOLEAN Ok;
    NTSTATUS Status;

    Status = QzPsAllocateTebPage(Process, &Address);
    if (!NT_SUCCESS(Status))
        return Status;
    Teb = ExAllocatePoolWithTag(NonPagedPool, sizeof(TEB), QZ_PS_TAG);
    if (!Teb)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Teb, sizeof(TEB));
    Teb->NtTib.ExceptionList = EXCEPTION_CHAIN_END;
    Teb->NtTib.Self = (PNT_TIB)Address;
    if (InitialTeb)
    {
        if (InitialTeb->PreviousStackBase || InitialTeb->PreviousStackLimit)
        {
            Teb->NtTib.StackBase = InitialTeb->PreviousStackBase;
            Teb->NtTib.StackLimit = InitialTeb->PreviousStackLimit;
        }
        else
        {
            Teb->NtTib.StackBase = InitialTeb->StackBase;
            Teb->NtTib.StackLimit = InitialTeb->StackLimit;
        }
        Teb->DeallocationStack = InitialTeb->AllocatedStackBase;
    }
    Teb->ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)Process->UniqueId;
    Teb->ClientId.UniqueThread = (HANDLE)(ULONG_PTR)PsThread->UniqueId;
    Teb->RealClientId = Teb->ClientId;
    Teb->ProcessEnvironmentBlock = Process->Peb;
    Teb->CurrentLocale = 0x0409;
    Teb->StaticUnicodeString.Buffer = (PWSTR)(Address + FIELD_OFFSET(TEB, StaticUnicodeBuffer));
    Teb->StaticUnicodeString.MaximumLength = sizeof(Teb->StaticUnicodeBuffer);

    QzPsAttach(Process, &Saved);
    Ok = QzCopyToUser((PVOID)Address, Teb, sizeof(TEB));
    QzPsDetach(Saved);
    ExFreePoolWithTag(Teb, QZ_PS_TAG);
    *Result = Address;
    return Ok ? STATUS_SUCCESS : STATUS_ACCESS_VIOLATION;
}

/* A user thread of Process starting with Context (it begins in ntdll's loader) */
static NTSTATUS
QzPsCreateThreadInProcess(PQZ_PROCESS Process, PCONTEXT Context, PINITIAL_TEB InitialTeb, BOOLEAN Suspended,
                          PQZ_PS_THREAD *Result)
{
    PQZ_PS_THREAD PsThread;
    ULONG_PTR Teb;
    NTSTATUS Status;

    /* Made suspended, so the TEB (which needs the thread's ID) exists before it runs */
    Status = QzPsCreateUserThread(Process, Context, NULL, TRUE, &PsThread);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = QzPsCreateTeb(Process, PsThread, InitialTeb, &Teb);
    if (!NT_SUCCESS(Status))
    {
        /* It never ran: let it end on its first scheduling */
        PsThread->TerminatePending = TRUE;
        PsThread->PendingExitStatus = Status;
        PsThread->SuspendCount = 0;
        QzReadyNewThread(PsThread->Thread);
        ObDereferenceObject(PsThread);
        return Status;
    }
    PsThread->Teb = (PVOID)Teb;
    if (!Suspended)
    {
        PsThread->SuspendCount = 0;
        QzReadyNewThread(PsThread->Thread);
    }
    *Result = PsThread;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NtCreateThread(OUT PHANDLE ThreadHandle,
               IN ACCESS_MASK DesiredAccess,
               IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL,
               IN HANDLE ProcessHandle,
               OUT PCLIENT_ID ClientId OPTIONAL,
               IN PCONTEXT ThreadContext,
               IN PINITIAL_TEB InitialTeb,
               IN BOOLEAN CreateSuspended)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PCONTEXT Context;
    INITIAL_TEB Initial;
    PQZ_PROCESS Process;
    PQZ_PS_THREAD PsThread;
    CLIENT_ID Cid;
    HANDLE Handle;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ObjectAttributes);
    if (!ThreadContext || !InitialTeb)
        return STATUS_INVALID_PARAMETER;
    Context = ExAllocatePoolWithTag(NonPagedPool, sizeof(CONTEXT), QZ_PS_TAG);
    if (!Context)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (!QzReadArg(Mode, Context, ThreadContext, sizeof(CONTEXT)) ||
        !QzReadArg(Mode, &Initial, InitialTeb, sizeof(Initial)))
    {
        ExFreePoolWithTag(Context, QZ_PS_TAG);
        return STATUS_ACCESS_VIOLATION;
    }
    Status = ObReferenceObjectByHandle(ProcessHandle, PROCESS_CREATE_THREAD, PsProcessType, Mode,
                                       (PVOID *)&Process, NULL);
    if (NT_SUCCESS(Status))
    {
        Status = QzPsCreateThreadInProcess(Process, Context, &Initial, CreateSuspended, &PsThread);
        ObDereferenceObject(Process);
    }
    ExFreePoolWithTag(Context, QZ_PS_TAG);
    if (!NT_SUCCESS(Status))
        return Status;
    Cid.UniqueProcess = (HANDLE)(ULONG_PTR)PsThread->Process->UniqueId;
    Cid.UniqueThread = (HANDLE)(ULONG_PTR)PsThread->UniqueId;
    Status = ObOpenObjectByPointer(PsThread, 0, NULL, DesiredAccess ? DesiredAccess : THREAD_ALL_ACCESS,
                                   PsThreadType, Mode, &Handle);
    ObDereferenceObject(PsThread);
    if (NT_SUCCESS(Status) &&
        (!QzWriteArg(Mode, ThreadHandle, &Handle, sizeof(Handle)) ||
         (ClientId && !QzWriteArg(Mode, ClientId, &Cid, sizeof(Cid)))))
        Status = STATUS_ACCESS_VIOLATION;
    return Status;
}

/* ---------------------------------------------------------------------- */
/* Into ntdll: the initial APC, NtContinue, exceptions                     */
/* ---------------------------------------------------------------------- */

/*
 * Before a new user thread first enters ring 3: run LdrInitializeThunk
 * as a user APC (KiInitializeUserApc's frame: NormalRoutine,
 * NormalContext, SystemArgument1 = ntdll's base, SystemArgument2, then
 * the thread's CONTEXT, which the loader NtContinue()s into).
 */
VOID
QzInitialUserApc(PQZ_PS_THREAD PsThread, PKTRAP_FRAME TrapFrame)
{
    PQZ_PROCESS Process = PsThread->Process;
    PCONTEXT Context;
    ULONG_PTR Stack, Arguments[4];

    if (!Process->SystemDllBase || !QzLdrInitializeThunk)
        return;   /* a bare test process */
    Context = ExAllocatePoolWithTag(NonPagedPool, sizeof(CONTEXT), QZ_PS_TAG);
    if (!Context)
        return;
    RtlZeroMemory(Context, sizeof(CONTEXT));
    QzTrapFrameToContext(TrapFrame, Context);
    Context->ContextFlags = CONTEXT_FULL;

    Stack = (((Context->Esp & ~3UL) - 8) & ~3UL) - (CONTEXT_ALIGNED_SIZE + 4 * sizeof(ULONG_PTR));
    Arguments[0] = QzLdrInitializeThunk;
    Arguments[1] = 0;
    Arguments[2] = (ULONG_PTR)Process->SystemDllBase;
    Arguments[3] = 0;
    if (QzCopyToUser((PVOID)(Stack + sizeof(Arguments)), Context, sizeof(CONTEXT)) &&
        QzCopyToUser((PVOID)Stack, Arguments, sizeof(Arguments)))
    {
        TrapFrame->Eip = QzKiUserApcDispatcher;
        TrapFrame->HardwareEsp = Stack;
        TrapFrame->DbgEip = TrapFrame->Eip;
    }
    else
    {
        DbgPrint(QZ_KERNEL_NAME ": ps: no room on the initial stack of %s for the loader APC\n",
                 Process->ImageFileName);
    }
    ExFreePoolWithTag(Context, QZ_PS_TAG);
}

/*
 * Give a ring-3 exception to ntdll's KiUserExceptionDispatcher(Record,
 * Context): both are copied to the user stack, as KiDispatchException does.
 * FALSE if there is no dispatcher or no room on the stack.
 */
BOOLEAN
QzDispatchUserException(PKTRAP_FRAME TrapFrame, PEXCEPTION_RECORD Record)
{
    PQZ_PS_THREAD PsThread = QzCurrentThread->PsThread;
    PCONTEXT Context;
    ULONG_PTR Stack, NewStack, Pointers[2];
    ULONG RecordSize;
    BOOLEAN Ok;

    if (!PsThread || !PsThread->Process->SystemDllBase || !QzKiUserExceptionDispatcher)
        return FALSE;
    if (TrapFrame->HardwareSegSs != (KGDT_R3_DATA | RPL_MASK))
        return FALSE;
    Context = ExAllocatePoolWithTag(NonPagedPool, sizeof(CONTEXT), QZ_PS_TAG);
    if (!Context)
        return FALSE;
    RtlZeroMemory(Context, sizeof(CONTEXT));
    QzTrapFrameToContext(TrapFrame, Context);
    Context->ContextFlags = CONTEXT_FULL;

    Stack = (Context->Esp & ~3UL) - ((sizeof(CONTEXT) + 3) & ~3UL);
    RecordSize = (sizeof(EXCEPTION_RECORD) - (EXCEPTION_MAXIMUM_PARAMETERS - Record->NumberParameters) *
                  sizeof(ULONG_PTR) + 3) & ~3UL;
    NewStack = Stack - RecordSize;
    Pointers[0] = NewStack;   /* the record */
    Pointers[1] = Stack;      /* the context */
    Ok = QzCopyToUser((PVOID)Stack, Context, sizeof(CONTEXT)) &&
         QzCopyToUser((PVOID)NewStack, Record, RecordSize) &&
         QzCopyToUser((PVOID)(NewStack - sizeof(Pointers)), Pointers, sizeof(Pointers));
    ExFreePoolWithTag(Context, QZ_PS_TAG);
    if (!Ok)
        return FALSE;

    TrapFrame->HardwareEsp = NewStack - sizeof(Pointers);
    TrapFrame->HardwareSegSs = KGDT_R3_DATA | RPL_MASK;
    TrapFrame->SegCs = KGDT_R3_CODE | RPL_MASK;
    TrapFrame->SegDs = TrapFrame->SegEs = KGDT_R3_DATA | RPL_MASK;
    TrapFrame->SegFs = KGDT_R3_TEB | RPL_MASK;
    TrapFrame->SegGs = 0;
    TrapFrame->Eip = QzKiUserExceptionDispatcher;
    return TRUE;
}

NTSTATUS
NTAPI
NtContinue(IN PCONTEXT Context,
           IN BOOLEAN TestAlert)
{
    PQZ_THREAD Thread = QzCurrentThread;
    PCONTEXT Local;

    if (!Thread->TrapFrame || ExGetPreviousMode() != UserMode)
        return STATUS_NOT_SUPPORTED;
    Local = ExAllocatePoolWithTag(NonPagedPool, sizeof(CONTEXT), QZ_PS_TAG);
    if (!Local)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (!QzCopyFromUser(Local, Context, sizeof(CONTEXT)))
    {
        ExFreePoolWithTag(Local, QZ_PS_TAG);
        return STATUS_ACCESS_VIOLATION;
    }
    QzContextToTrapFrame(Local, Thread->TrapFrame, Local->ContextFlags);
    ExFreePoolWithTag(Local, QZ_PS_TAG);
    Thread->TrapFrameReplaced = TRUE;
    /* KiUserApcDispatcher asks for the next user APC */
    if (TestAlert)
        QzApcTestAlert();

    /* The loader finished with the process: it now runs its own image */
    {
        PQZ_PROCESS Process = Thread->Process;
        if (Process && !Process->RanImage && Process->EntryPoint && Thread->TrapFrame->Eip == Process->EntryPoint)
        {
            Process->RanImage = TRUE;
            DbgPrint(QZ_KERNEL_NAME ": ps: %s (pid %lu): ntdll's loader is done; running the image at %08lx\n",
                     Process->ImageFileName, Process->UniqueId, Process->EntryPoint);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NtRaiseException(IN PEXCEPTION_RECORD ExceptionRecord,
                 IN PCONTEXT Context,
                 IN BOOLEAN FirstChance)
{
    PQZ_THREAD Thread = QzCurrentThread;
    PQZ_PS_THREAD PsThread = Thread->PsThread;
    EXCEPTION_RECORD Record;
    PCONTEXT Local;

    if (!Thread->TrapFrame || ExGetPreviousMode() != UserMode || !PsThread)
        return STATUS_NOT_SUPPORTED;
    Local = ExAllocatePoolWithTag(NonPagedPool, sizeof(CONTEXT), QZ_PS_TAG);
    if (!Local)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (!QzCopyFromUser(&Record, ExceptionRecord, sizeof(Record)) ||
        !QzCopyFromUser(Local, Context, sizeof(CONTEXT)))
    {
        ExFreePoolWithTag(Local, QZ_PS_TAG);
        return STATUS_ACCESS_VIOLATION;
    }
    if (Record.NumberParameters > EXCEPTION_MAXIMUM_PARAMETERS)
        Record.NumberParameters = EXCEPTION_MAXIMUM_PARAMETERS;
    QzContextToTrapFrame(Local, Thread->TrapFrame, Local->ContextFlags);
    ExFreePoolWithTag(Local, QZ_PS_TAG);
    Thread->TrapFrameReplaced = TRUE;

    /* DbgPrint-style breakpoints with no debugger attached: carry on */
    if (Record.ExceptionCode == STATUS_BREAKPOINT && FirstChance)
    {
        DbgPrint(QZ_KERNEL_NAME ": %s: breakpoint at %p (no debugger; continuing)\n",
                 PsThread->Process->ImageFileName, Record.ExceptionAddress);
        return STATUS_SUCCESS;
    }
    if (FirstChance && QzDispatchUserException(Thread->TrapFrame, &Record))
        return STATUS_SUCCESS;

    /* The second chance: nobody handled it */
    DbgPrint(QZ_KERNEL_NAME ": %s (pid %lu): unhandled exception 0x%08lx at %p\n",
             PsThread->Process->ImageFileName, PsThread->Process->UniqueId, Record.ExceptionCode,
             Record.ExceptionAddress);
    QzPsTerminateProcess(PsThread->Process, Record.ExceptionCode);
    QzPsTerminateCurrentThread(Record.ExceptionCode);
}

NTSTATUS
NTAPI
NtTestAlert(VOID)
{
    return QzApcTestAlert() ? STATUS_ALERTED : STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* The first user process                                                  */
/* ---------------------------------------------------------------------- */

/* Strings laid out after RTL_USER_PROCESS_PARAMETERS, as offsets (denormalized) */
typedef struct _QZ_PARAMS_BUILDER
{
    PUCHAR Buffer;
    ULONG Used, Size;
} QZ_PARAMS_BUILDER;

static VOID
QzPsAddString(QZ_PARAMS_BUILDER *Builder, PUNICODE_STRING Field, PCWSTR Text, USHORT ExtraRoom)
{
    USHORT Bytes = (USHORT)(wcslen(Text) * sizeof(WCHAR));
    ULONG Offset = (Builder->Used + 3) & ~3UL;

    Field->Length = Bytes;
    Field->MaximumLength = Bytes + sizeof(WCHAR) + ExtraRoom;
    if (Offset + Field->MaximumLength > Builder->Size)
        return;
    RtlCopyMemory(Builder->Buffer + Offset, Text, Bytes + sizeof(WCHAR));
    Field->Buffer = (PWSTR)(ULONG_PTR)Offset;   /* denormalized: an offset */
    Builder->Used = Offset + Field->MaximumLength;
}

/* NtSystemRoot: the boot volume's drive letter, and the boot path */
static VOID
QzPsSetSystemRoot(VOID)
{
    WCHAR Letter;
    PCSTR Path = QzLoaderBlock->NtBootPathName ? QzLoaderBlock->NtBootPathName : "\\WINDOWS\\";
    ULONG i, Out = 0;

    Letter = L'C';
    for (WCHAR Try = L'C'; Try <= L'Z'; Try++)
    {
        WCHAR Name[] = L"\\GLOBAL??\\C:";
        WCHAR Target[64];
        UNICODE_STRING Link, TargetString;
        OBJECT_ATTRIBUTES Attributes;
        HANDLE Handle;
        Name[10] = Try;
        RtlInitUnicodeString(&Link, Name);
        InitializeObjectAttributes(&Attributes, &Link, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        if (!NT_SUCCESS(ZwOpenSymbolicLinkObject(&Handle, SYMBOLIC_LINK_QUERY, &Attributes)))
            continue;
        TargetString.Buffer = Target;
        TargetString.Length = 0;
        TargetString.MaximumLength = sizeof(Target);
        if (NT_SUCCESS(ZwQuerySymbolicLinkObject(Handle, &TargetString, NULL)))
        {
            UNICODE_STRING Boot = RTL_CONSTANT_STRING(L"\\Device\\CdRom0");
            if (strstr(QzLoaderBlock->ArcBootDeviceName, "cdrom(") && RtlEqualUnicodeString(&TargetString, &Boot, TRUE))
            {
                Letter = Try;
                ZwClose(Handle);
                break;
            }
        }
        ZwClose(Handle);
    }
    SharedUserData->NtSystemRoot[Out++] = Letter;
    SharedUserData->NtSystemRoot[Out++] = L':';
    for (i = 0; Path[i] && Out < RTL_NUMBER_OF(SharedUserData->NtSystemRoot) - 1; i++)
        SharedUserData->NtSystemRoot[Out++] = (UCHAR)Path[i];
    while (Out > 3 && SharedUserData->NtSystemRoot[Out - 1] == L'\\')
        Out--;
    SharedUserData->NtSystemRoot[Out] = 0;
}

NTSTATUS
QzPsStartSmss(PQZ_PROCESS *Result)
{
    static const WCHAR ImagePath[] = L"\\SystemRoot\\System32\\smss.exe";
    PFILE_OBJECT FileObject;
    PQZ_SECTION Section;
    PQZ_PROCESS Process, Saved;
    PQZ_PS_THREAD Thread;
    PSECTION_IMAGE_INFORMATION Image;
    INITIAL_TEB InitialTeb;
    CONTEXT Context;
    PVOID StackBase = NULL, Params = NULL, Environment = NULL;
    SIZE_T StackReserve, StackCommit, Size;
    QZ_PARAMS_BUILDER Builder;
    PRTL_USER_PROCESS_PARAMETERS P;
    WCHAR Text[300];
    NTSTATUS Status;

    *Result = NULL;
    QzPsSetSystemRoot();
    Status = QzPsOpenFile(ImagePath, &FileObject);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = QzMmCreateSection(&Section, NULL, PAGE_EXECUTE, SEC_IMAGE, FileObject);
    ObDereferenceObject(FileObject);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = QzPsCreateUserProcess(QzSystemProcess, Section, FALSE, "smss.exe", &Process);
    ObDereferenceObject(Section);
    if (!NT_SUCCESS(Status))
        return Status;
    Image = &Section->ImageInformation;

    /* The process parameters, denormalized; ntdll normalizes them */
    Builder.Size = 2 * PAGE_SIZE;
    Builder.Buffer = ExAllocatePoolWithTag(NonPagedPool, Builder.Size, QZ_PS_TAG);
    if (!Builder.Buffer)
    {
        ObDereferenceObject(Process);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Builder.Buffer, Builder.Size);
    P = (PRTL_USER_PROCESS_PARAMETERS)Builder.Buffer;
    Builder.Used = sizeof(*P);
    _snwprintf(Text, RTL_NUMBER_OF(Text), L"%ls\\System32", SharedUserData->NtSystemRoot);
    QzPsAddString(&Builder, &P->DllPath, Text, 0);
    QzPsAddString(&Builder, &P->ImagePathName, ImagePath, 0);
    QzPsAddString(&Builder, &P->CommandLine, ImagePath, 0);
    _snwprintf(Text, RTL_NUMBER_OF(Text), L"%ls\\System32\\", SharedUserData->NtSystemRoot);
    QzPsAddString(&Builder, &P->CurrentDirectory.DosPath, Text, 2 * MAX_PATH);
    QzPsAddString(&Builder, &P->WindowTitle, L"", 0);
    QzPsAddString(&Builder, &P->DesktopInfo, L"", 0);
    QzPsAddString(&Builder, &P->ShellInfo, L"", 0);
    QzPsAddString(&Builder, &P->RuntimeData, L"", 0);
    P->MaximumLength = P->Length = Builder.Used;
    P->Flags = 0;   /* not normalized */

    Size = Builder.Used;
    Status = QzMmAllocate(Process, &Params, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (NT_SUCCESS(Status))
    {
        /* The environment: Path and SystemRoot, as NT gives smss */
        WCHAR Env[300];
        ULONG Chars = (ULONG)_snwprintf(Env, RTL_NUMBER_OF(Env) - 2, L"Path=%ls\\System32", SharedUserData->NtSystemRoot);
        Chars += 1 + (ULONG)_snwprintf(Env + Chars + 1, RTL_NUMBER_OF(Env) - Chars - 3, L"SystemRoot=%ls",
                                       SharedUserData->NtSystemRoot);
        Env[Chars + 1] = 0;
        Env[Chars + 2] = 0;
        Size = (Chars + 3) * sizeof(WCHAR);
        Status = QzMmAllocate(Process, &Environment, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        P->Environment = Environment;
        QzPsAttach(Process, &Saved);
        if (NT_SUCCESS(Status) &&
            (!QzCopyToUser(Environment, Env, (Chars + 3) * sizeof(WCHAR)) ||
             !QzCopyToUser(Params, Builder.Buffer, Builder.Used) ||
             !QzCopyToUser(&((PPEB)Process->Peb)->ProcessParameters, &Params, sizeof(Params))))
            Status = STATUS_ACCESS_VIOLATION;
        QzPsDetach(Saved);
    }
    ExFreePoolWithTag(Builder.Buffer, QZ_PS_TAG);

    /* The stack: reserve, commit the top, a guard page below it */
    StackReserve = max(Image->MaximumStackSize, 0x40000);
    StackCommit = (max(Image->CommittedStackSize, PAGE_SIZE) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (NT_SUCCESS(Status))
    {
        Size = StackReserve;
        Status = QzMmAllocate(Process, &StackBase, 0, &Size, MEM_RESERVE, PAGE_READWRITE);
    }
    if (NT_SUCCESS(Status))
    {
        PVOID Commit = (PUCHAR)StackBase + StackReserve - StackCommit, Guard;
        Size = StackCommit;
        Status = QzMmAllocate(Process, &Commit, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
        Guard = (PUCHAR)Commit - PAGE_SIZE;
        Size = PAGE_SIZE;
        if (NT_SUCCESS(Status))
            Status = QzMmAllocate(Process, &Guard, 0, &Size, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD);
        InitialTeb.PreviousStackBase = InitialTeb.PreviousStackLimit = NULL;
        InitialTeb.StackBase = (PUCHAR)StackBase + StackReserve;
        InitialTeb.StackLimit = Commit;
        InitialTeb.AllocatedStackBase = StackBase;
    }
    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(Process);
        return Status;
    }

    /* RtlInitializeContext: start at the entry point with the PEB as the argument */
    RtlZeroMemory(&Context, sizeof(Context));
    Context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS;
    Context.Ebx = 1;
    Context.Ecx = 2;
    Context.Edx = 3;
    Context.Esi = 4;
    Context.Edi = 5;
    Context.EFlags = EFLAGS_INTERRUPT_MASK;
    Context.Eip = (ULONG)(ULONG_PTR)Image->TransferAddress + ((ULONG_PTR)Process->SectionBaseAddress -
                                                             (ULONG_PTR)Section->ImageBase);
    Context.Esp = (ULONG)(ULONG_PTR)InitialTeb.StackBase - sizeof(PVOID);
    QzPsAttach(Process, &Saved);
    QzCopyToUser((PVOID)Context.Esp, &Process->Peb, sizeof(PVOID));
    QzPsDetach(Saved);
    Context.Esp -= sizeof(PVOID);

    Process->EntryPoint = Context.Eip;
    Status = QzPsCreateThreadInProcess(Process, &Context, &InitialTeb, FALSE, &Thread);
    if (NT_SUCCESS(Status))
    {
        DbgPrint(QZ_KERNEL_NAME ": ps: started %ls: pid %lu, image at %p, entry %08lx\n", ImagePath,
                 Process->UniqueId, Process->SectionBaseAddress, Context.Eip);
        ObDereferenceObject(Thread);
        *Result = Process;   /* the caller's reference */
        return Status;
    }
    ObDereferenceObject(Process);
    return Status;
}
