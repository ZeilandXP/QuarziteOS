/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Object manager self-test (shell command "obtest")
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Exercises the stage B3 exit criterion through the same exports drivers
 * use: object types, creation and insertion, the namespace with
 * directories and symbolic links, handles, reference counting and
 * deletion, and handle-based waits on executive objects.
 */

#include "qz.h"

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

static LARGE_INTEGER
QzMs(LONG Milliseconds)
{
    LARGE_INTEGER Interval;
    Interval.QuadPart = -10000LL * Milliseconds;
    return Interval;
}

static VOID
QzSleepMs(LONG Milliseconds)
{
    LARGE_INTEGER Interval = QzMs(Milliseconds);
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

static VOID
QzInitAttributes(POBJECT_ATTRIBUTES Attributes, PUNICODE_STRING Name, PCWSTR Text,
                 ULONG Flags, HANDLE Root)
{
    if (Text)
        RtlInitUnicodeString(Name, Text);
    InitializeObjectAttributes(Attributes, Text ? Name : NULL, Flags, Root, NULL);
}

static NTSTATUS
QzCreateEvent(PHANDLE Handle, PCWSTR Name, ULONG Flags, HANDLE Root)
{
    UNICODE_STRING String;
    OBJECT_ATTRIBUTES Attributes;
    QzInitAttributes(&Attributes, &String, Name, Flags, Root);
    return ZwCreateEvent(Handle, EVENT_ALL_ACCESS, &Attributes, NotificationEvent, FALSE);
}

static NTSTATUS
QzOpenEvent(PHANDLE Handle, PCWSTR Name, ULONG Flags)
{
    UNICODE_STRING String;
    OBJECT_ATTRIBUTES Attributes;
    QzInitAttributes(&Attributes, &String, Name, Flags, NULL);
    return ZwOpenEvent(Handle, EVENT_ALL_ACCESS, &Attributes);
}

static PVOID
QzObjectOf(HANDLE Handle)
{
    PVOID Object = NULL;
    if (NT_SUCCESS(ObReferenceObjectByHandle(Handle, 0, NULL, KernelMode, &Object, NULL)))
        ObDereferenceObject(Object);
    return Object;
}

static PVOID
QzLookup(PCWSTR Path, ULONG Attributes, POBJECT_TYPE Type, NTSTATUS *Status)
{
    UNICODE_STRING Name;
    PVOID Object = NULL;
    RtlInitUnicodeString(&Name, Path);
    *Status = ObReferenceObjectByName(&Name, Attributes, NULL, 0, Type, KernelMode, NULL, &Object);
    if (NT_SUCCESS(*Status))
        ObDereferenceObject(Object);
    return Object;
}

/* ---- Handles and references ---- */

static VOID
QzTestHandles(VOID)
{
    HANDLE Event, Dup;
    PVOID Object, Object2;
    NTSTATUS Status, Status2;
    POBJECT_HEADER Header;
    ULONG Objects = ExEventObjectType->TotalNumberOfObjects;
    union { OBJECT_BASIC_INFORMATION Basic; OBJECT_TYPE_INFORMATION Type; UCHAR Raw[256]; } Info;
    ULONG Length;
    BOOLEAN Ok;

    Status = QzCreateEvent(&Event, NULL, OBJ_KERNEL_HANDLE, NULL);
    Status2 = NT_SUCCESS(Status) ? ObReferenceObjectByHandle(Event, EVENT_ALL_ACCESS,
                                                             ExEventObjectType, KernelMode,
                                                             &Object, NULL)
                                 : Status;
    Ok = NT_SUCCESS(Status2);
    if (Ok)
    {
        Header = OBJECT_TO_OBJECT_HEADER(Object);
        Ok = Header->HandleCount == 1 && Header->PointerCount == 2 &&
             ((ULONG_PTR)Event & 0x80000003) == 0x80000000;
        Ok = Ok && KeReadStateEvent(Object) == 0;
        ZwSetEvent(Event, NULL);
        Ok = Ok && KeReadStateEvent(Object) == 1;
    }
    QzCheck("unnamed event: kernel handle, HandleCount 1, PointerCount 2, ZwSetEvent", Ok);

    Status = ObReferenceObjectByHandle(Event, 0, ExSemaphoreObjectType, KernelMode, &Object2, NULL);
    QzCheck("referencing with the wrong type fails (OBJECT_TYPE_MISMATCH)",
            Status == STATUS_OBJECT_TYPE_MISMATCH);

    Ok = NT_SUCCESS(ZwQueryObject(Event, ObjectBasicInformation, &Info, sizeof(Info), &Length)) &&
         Info.Basic.HandleCount == 1;
    Ok = Ok && NT_SUCCESS(ZwQueryObject(Event, ObjectTypeInformation, &Info, sizeof(Info), &Length));
    if (Ok)
    {
        UNICODE_STRING EventName = RTL_CONSTANT_STRING(L"Event");
        Ok = RtlEqualUnicodeString(&Info.Type.TypeName, &EventName, FALSE);
    }
    QzCheck("ZwQueryObject: basic (handle count) and type (\"Event\") information", Ok);

    Ok = NT_SUCCESS(ZwDuplicateObject(NtCurrentProcess(), Event, NtCurrentProcess(), &Dup, 0, 0,
                                      DUPLICATE_SAME_ACCESS));
    Ok = Ok && Dup != Event && QzObjectOf(Dup) == Object && Header->HandleCount == 2;
    Ok = Ok && NT_SUCCESS(ZwDuplicateObject(NtCurrentProcess(), Dup, NtCurrentProcess(), &Dup, 0,
                                            0, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE));
    Ok = Ok && Header->HandleCount == 2 && NT_SUCCESS(ZwClose(Dup));
    QzCheck("ZwDuplicateObject: same object, SAME_ACCESS, CLOSE_SOURCE", Ok && Header->HandleCount == 1);

    Status = ZwClose(Event);
    Status2 = ZwClose(Event);
    Ok = NT_SUCCESS(Status) && Status2 == STATUS_INVALID_HANDLE;
    Ok = Ok && Header->HandleCount == 0 && Header->PointerCount == 1 &&
         ExEventObjectType->TotalNumberOfObjects == Objects + 1;
    QzCheck("ZwClose: second close of a handle fails; our pointer reference keeps the object", Ok);

    ObDereferenceObject(Object);
    QzCheck("last ObDereferenceObject deletes the object",
            ExEventObjectType->TotalNumberOfObjects == Objects);

    Status = ObReferenceObjectByHandle(NtCurrentProcess(), 0, PsProcessType, KernelMode, &Object, NULL);
    if (NT_SUCCESS(Status))
    {
        Ok = (Object == (PVOID)PsGetCurrentProcess());
        ObDereferenceObject(Object);
    }
    else
        Ok = FALSE;
    Status2 = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)0x80000ffc, 0, NULL, KernelMode,
                                        &Object, NULL);
    QzCheck("NtCurrentProcess() is the process; unknown handles are STATUS_INVALID_HANDLE",
            Ok && Status2 == STATUS_INVALID_HANDLE);
}

/* ---- Names ---- */

static VOID
QzTestNames(VOID)
{
    HANDLE A, B, C, D;
    NTSTATUS Status;
    PVOID Object;
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    BOOLEAN Ok;

    Status = QzCreateEvent(&A, L"\\BaseNamedObjects\\QzTestEvent", OBJ_CASE_INSENSITIVE, NULL);
    Ok = NT_SUCCESS(Status) &&
         NT_SUCCESS(QzOpenEvent(&B, L"\\basenamedobjects\\QZTESTEVENT", OBJ_CASE_INSENSITIVE)) &&
         QzObjectOf(A) == QzObjectOf(B);
    QzCheck("named event: a case-insensitive open finds the same object", Ok);

    Status = QzCreateEvent(&C, L"\\BaseNamedObjects\\QzTestEvent", OBJ_CASE_INSENSITIVE, NULL);
    QzCheck("creating an existing name fails (OBJECT_NAME_COLLISION)",
            Status == STATUS_OBJECT_NAME_COLLISION);

    Status = QzCreateEvent(&C, L"\\BaseNamedObjects\\QzTestEvent",
                           OBJ_CASE_INSENSITIVE | OBJ_OPENIF, NULL);
    Ok = Status == STATUS_OBJECT_NAME_EXISTS && QzObjectOf(C) == QzObjectOf(A);
    QzCheck("OBJ_OPENIF opens the existing object (OBJECT_NAME_EXISTS)", Ok);

    RtlInitUnicodeString(&Name, L"\\BaseNamedObjects\\QzTestEvent");
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_OPENIF, NULL, NULL);
    Status = NtCreateSemaphore(&D, SEMAPHORE_ALL_ACCESS, &Attributes, 0, 1);
    QzCheck("OBJ_OPENIF on a name of another type fails (OBJECT_TYPE_MISMATCH)",
            Status == STATUS_OBJECT_TYPE_MISMATCH);

    Object = QzLookup(L"\\BaseNamedObjects\\qztestevent", 0, NULL, &Status);
    Ok = Status == STATUS_OBJECT_NAME_NOT_FOUND;
    Object = QzLookup(L"\\BaseNamedObjects\\qztestevent", 0, ExEventObjectType, &Status);
    Ok = Ok && NT_SUCCESS(Status) && Object == QzObjectOf(A);
    QzCheck("lookups are case-sensitive unless asked or the type is case-insensitive", Ok);

    ZwClose(A);
    ZwClose(B);
    Ok = NT_SUCCESS(QzOpenEvent(&B, L"\\BaseNamedObjects\\QzTestEvent", OBJ_CASE_INSENSITIVE));
    ZwClose(B);
    ZwClose(C);
    Status = QzOpenEvent(&B, L"\\BaseNamedObjects\\QzTestEvent", OBJ_CASE_INSENSITIVE);
    QzCheck("a temporary object's name goes away with its last handle",
            Ok && Status == STATUS_OBJECT_NAME_NOT_FOUND);

    /* Permanent objects keep their name until made temporary */
    Status = QzCreateEvent(&A, L"\\BaseNamedObjects\\QzPermanent",
                           OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL);
    ZwClose(A);
    Ok = NT_SUCCESS(Status) &&
         NT_SUCCESS(QzOpenEvent(&A, L"\\BaseNamedObjects\\QzPermanent", OBJ_CASE_INSENSITIVE));
    Ok = Ok && NT_SUCCESS(ZwMakeTemporaryObject(A));
    ZwClose(A);
    Status = QzOpenEvent(&A, L"\\BaseNamedObjects\\QzPermanent", OBJ_CASE_INSENSITIVE);
    QzCheck("OBJ_PERMANENT survives its last close; ZwMakeTemporaryObject undoes it",
            Ok && Status == STATUS_OBJECT_NAME_NOT_FOUND);

    Status = QzOpenEvent(&A, L"\\NoSuchDirectory\\X", OBJ_CASE_INSENSITIVE);
    Ok = Status == STATUS_OBJECT_PATH_NOT_FOUND;
    Status = QzOpenEvent(&A, L"NoLeadingSlash", OBJ_CASE_INSENSITIVE);
    Ok = Ok && Status == STATUS_OBJECT_PATH_SYNTAX_BAD;
    QzCheck("bad paths: OBJECT_PATH_NOT_FOUND and OBJECT_PATH_SYNTAX_BAD", Ok);
}

/* ---- Directories and symbolic links ---- */

static VOID
QzTestDirectories(VOID)
{
    HANDLE Dir, Ev, Link, Link2, Open;
    UNICODE_STRING Name, Target;
    OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;
    PVOID Event = NULL, Object;
    WCHAR Buffer[64];
    union { OBJECT_NAME_INFORMATION Info; UCHAR Raw[256]; } NameBuffer;
    ULONG Length;
    BOOLEAN Ok;
    ULONG DirObjects = ObpDirectoryObjectType->TotalNumberOfObjects;

    RtlInitUnicodeString(&Name, L"\\QzTestDir");
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateDirectoryObject(&Dir, DIRECTORY_ALL_ACCESS, &Attributes);
    Ok = NT_SUCCESS(Status) &&
         NT_SUCCESS(QzCreateEvent(&Ev, L"Item", OBJ_CASE_INSENSITIVE, Dir));
    if (Ok)
        Event = QzObjectOf(Ev);
    Object = QzLookup(L"\\QzTestDir\\Item", OBJ_CASE_INSENSITIVE, NULL, &Status);
    QzCheck("directory + name relative to its handle, found by absolute path",
            Ok && Event && Object == Event);

    Ok = NT_SUCCESS(ObQueryNameString(Event, &NameBuffer.Info, sizeof(NameBuffer), &Length));
    RtlInitUnicodeString(&Name, L"\\QzTestDir\\Item");
    Ok = Ok && RtlEqualUnicodeString(&NameBuffer.Info.Name, &Name, FALSE);
    Ok = Ok && NT_SUCCESS(ObQueryNameString(QzObRootDirectory, &NameBuffer.Info,
                                            sizeof(NameBuffer), &Length)) &&
         NameBuffer.Info.Name.Length == sizeof(WCHAR) && NameBuffer.Info.Name.Buffer[0] == L'\\';
    QzCheck("ObQueryNameString: \"\\QzTestDir\\Item\" and \"\\\"", Ok);

    /* \QzTestLink -> \QzTestDir */
    RtlInitUnicodeString(&Name, L"\\QzTestLink");
    RtlInitUnicodeString(&Target, L"\\QzTestDir");
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateSymbolicLinkObject(&Link, SYMBOLIC_LINK_ALL_ACCESS, &Attributes, &Target);
    Object = QzLookup(L"\\QzTestLink\\Item", OBJ_CASE_INSENSITIVE, NULL, &Status);
    QzCheck("symbolic link in the middle of a path is followed", NT_SUCCESS(Status) && Object == Event);

    Target.Buffer = Buffer;
    Target.Length = 0;
    Target.MaximumLength = sizeof(Buffer);
    Ok = NT_SUCCESS(ZwQuerySymbolicLinkObject(Link, &Target, &Length));
    RtlInitUnicodeString(&Name, L"\\QzTestDir");
    Ok = Ok && RtlEqualUnicodeString(&Target, &Name, FALSE) && Length == Name.Length + sizeof(WCHAR);
    RtlInitUnicodeString(&Name, L"\\QzTestLink");
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Ok = Ok && NT_SUCCESS(ZwOpenSymbolicLinkObject(&Open, SYMBOLIC_LINK_QUERY, &Attributes)) &&
         QzObjectOf(Open) == QzObjectOf(Link);
    ZwClose(Open);
    Ok = Ok && QzLookup(L"\\QzTestLink", OBJ_CASE_INSENSITIVE, NULL, &Status) ==
               QzObjectOf(Dir);
    QzCheck("ZwQuerySymbolicLinkObject; opening a link vs. following it at the end", Ok);

    /* \GLOBAL??\QZ: -> \QzTestDir, reached through \DosDevices -> \?? -> \GLOBAL?? */
    RtlInitUnicodeString(&Name, L"\\GLOBAL??\\QZ:");
    RtlInitUnicodeString(&Target, L"\\QzTestDir");
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateSymbolicLinkObject(&Link2, SYMBOLIC_LINK_ALL_ACCESS, &Attributes, &Target);
    Object = QzLookup(L"\\DosDevices\\QZ:\\Item", OBJ_CASE_INSENSITIVE, NULL, &Status);
    QzCheck("chained links: \\DosDevices\\QZ:\\Item -> \\??\\ -> \\GLOBAL??\\ -> \\QzTestDir\\Item",
            NT_SUCCESS(Status) && Object == Event);

    ZwClose(Link2);
    ZwClose(Link);
    ZwClose(Ev);
    ZwClose(Dir);
    Object = QzLookup(L"\\QzTestDir", OBJ_CASE_INSENSITIVE, NULL, &Status);
    Ok = Status == STATUS_OBJECT_NAME_NOT_FOUND;
    Object = QzLookup(L"\\GLOBAL??\\QZ:", OBJ_OPENLINK | OBJ_CASE_INSENSITIVE, NULL, &Status);
    Ok = Ok && Status == STATUS_OBJECT_NAME_NOT_FOUND;
    QzCheck("temporary directory, links and contents are all deleted on close",
            Ok && ObpDirectoryObjectType->TotalNumberOfObjects == DirObjects);
}

/* ---- The standard namespace ---- */

static VOID
QzTestNamespace(VOID)
{
    static const PCWSTR Expected[] =
    {
        L"\\ObjectTypes", L"\\BaseNamedObjects", L"\\Device", L"\\Driver", L"\\GLOBAL??",
        L"\\KernelObjects", L"\\Callback", L"\\FileSystem", L"\\ArcName",
    };
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Root;
    NTSTATUS Status;
    PVOID Object;
    ULONG i, Context = 0, Count = 0, Length;
    UCHAR Buffer[1024];
    BOOLEAN Ok = TRUE;

    for (i = 0; i < RTL_NUMBER_OF(Expected); i++)
    {
        Object = QzLookup(Expected[i], OBJ_CASE_INSENSITIVE, ObpDirectoryObjectType, &Status);
        Ok = Ok && NT_SUCCESS(Status);
    }
    QzCheck("standard directories exist (\\ObjectTypes, \\Device, \\GLOBAL??, ...)", Ok);

    Ok = QzLookup(L"\\ObjectTypes\\Event", OBJ_CASE_INSENSITIVE, NULL, &Status) == ExEventObjectType;
    Ok = Ok && QzLookup(L"\\ObjectTypes\\Type", OBJ_CASE_INSENSITIVE, NULL, &Status) ==
               ObpTypeObjectType;
    Ok = Ok && OBJECT_TO_OBJECT_HEADER(ObpTypeObjectType)->Type == ObpTypeObjectType;
    Ok = Ok && QzLookup(L"\\ObjectTypes\\File", OBJ_CASE_INSENSITIVE, NULL, &Status) ==
               IoFileObjectType;
    QzCheck("\\ObjectTypes holds the types; the Type type is its own type", Ok);

    Object = QzLookup(L"\\SystemRoot", OBJ_CASE_INSENSITIVE | OBJ_OPENLINK, NULL, &Status);
    Ok = NT_SUCCESS(Status) && OBJECT_TO_OBJECT_HEADER(Object)->Type == ObpSymbolicLinkObjectType;
    Object = QzLookup(L"\\??", OBJ_CASE_INSENSITIVE, NULL, &Status);
    Ok = Ok && Object == QzLookup(L"\\GLOBAL??", OBJ_CASE_INSENSITIVE, NULL, &Status);
    QzCheck("\\SystemRoot, \\?? and \\DosDevices links", Ok);

    RtlInitUnicodeString(&Name, L"\\");
    InitializeObjectAttributes(&Attributes, &Name, 0, NULL, NULL);
    Ok = NT_SUCCESS(ZwOpenDirectoryObject(&Root, DIRECTORY_QUERY, &Attributes));
    if (Ok)
    {
        while (ZwQueryDirectoryObject(Root, Buffer, sizeof(Buffer), TRUE, Context == 0, &Context,
                                      &Length) == STATUS_SUCCESS)
        {
            Count++;
        }
        ZwClose(Root);
    }
    QzCheck("ZwQueryDirectoryObject enumerates the root directory", Ok && Count >= 12);
}

/* ---- Handle-based waits ---- */

static HANDLE QzWaitHandle;
static volatile LONG QzWaitResult = -1;

static VOID NTAPI
QzWaiterThread(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    QzWaitResult = ZwWaitForSingleObject(QzWaitHandle, FALSE, NULL);
}

static VOID
QzTestWaits(VOID)
{
    HANDLE Event, Semaphore, Timer, Both[2];
    LARGE_INTEGER Timeout = QzMs(20), Due = QzMs(30);
    NTSTATUS Status;
    LONG Previous = -1;
    BOOLEAN Ok;

    QzCreateEvent(&Event, NULL, 0, NULL);
    QzWaitHandle = Event;
    QzWaitResult = -1;
    QzCreateThread("obwait", QzWaiterThread, NULL);
    QzSleepMs(50);
    Ok = QzWaitResult == -1;
    ZwSetEvent(Event, &Previous);
    QzSleepMs(50);
    QzCheck("another thread's ZwWaitForSingleObject is released by ZwSetEvent",
            Ok && Previous == 0 && QzWaitResult == STATUS_WAIT_0);

    NtCreateSemaphore(&Semaphore, SEMAPHORE_ALL_ACCESS, NULL, 0, 2);
    ZwResetEvent(Event, NULL);
    Both[0] = Event;
    Both[1] = Semaphore;
    Status = ZwWaitForMultipleObjects(2, Both, WaitAny, FALSE, &Timeout);
    Ok = Status == STATUS_TIMEOUT;
    NtReleaseSemaphore(Semaphore, 1, NULL);
    Status = ZwWaitForMultipleObjects(2, Both, WaitAny, FALSE, &Timeout);
    Ok = Ok && Status == STATUS_WAIT_1;
    Ok = Ok && NtReleaseSemaphore(Semaphore, 3, NULL) == STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    QzCheck("ZwWaitForMultipleObjects (WaitAny) on an event and a semaphore", Ok);

    Ok = NT_SUCCESS(ZwCreateTimer(&Timer, TIMER_ALL_ACCESS, NULL, NotificationTimer));
    Ok = Ok && NT_SUCCESS(ZwSetTimer(Timer, &Due, NULL, NULL, FALSE, 0, NULL));
    Ok = Ok && ZwWaitForSingleObject(Timer, FALSE, &Timeout) == STATUS_TIMEOUT;
    Timeout = QzMs(200);
    Ok = Ok && ZwWaitForSingleObject(Timer, FALSE, &Timeout) == STATUS_WAIT_0;
    QzCheck("timer object: ZwSetTimer then a handle wait fires after the due time", Ok);

    ZwClose(Timer);
    ZwClose(Semaphore);
    ZwClose(Event);
}

/* ---- Bulk handles and deferred deletion ---- */

#define QZ_BULK 300

static VOID
QzTestBulk(VOID)
{
    static HANDLE Handles[QZ_BULK];
    ULONG Before = QzObHandleCount(), Objects = QzObObjectCount(), i, j;
    BOOLEAN Ok = TRUE;
    HANDLE Event;
    PVOID Object;
    KIRQL OldIrql;

    for (i = 0; i < QZ_BULK && Ok; i++)
        Ok = NT_SUCCESS(QzCreateEvent(&Handles[i], NULL, 0, NULL));
    for (i = 0; i < QZ_BULK && Ok; i += 37)
        for (j = i + 1; j < QZ_BULK && Ok; j++)
            Ok = Handles[i] != Handles[j];
    Ok = Ok && QzObHandleCount() == Before + QZ_BULK;
    for (i = 0; i < QZ_BULK; i++)
        ZwClose(Handles[i]);
    QzCheck("300 handles: table grows, values distinct, all closed again",
            Ok && QzObHandleCount() == Before && QzObObjectCount() == Objects);

    /* Dropping the last reference at DISPATCH_LEVEL defers the deletion */
    QzCreateEvent(&Event, NULL, 0, NULL);
    ObReferenceObjectByHandle(Event, 0, NULL, KernelMode, &Object, NULL);
    ZwClose(Event);
    OldIrql = QzRaiseIrql(DISPATCH_LEVEL);
    ObDereferenceObject(Object);
    Ok = QzObObjectCount() == Objects + 1;
    QzLowerIrql(OldIrql);
    QzSleepMs(50);
    QzCheck("last dereference at DISPATCH_LEVEL is deferred to the reaper thread",
            Ok && QzObObjectCount() == Objects);
}

VOID
QzRunObjectTests(VOID)
{
    ULONG Objects = QzObObjectCount(), Handles = QzObHandleCount();

    QzPassed = QzFailed = 0;
    QzTestHandles();
    QzTestNames();
    QzTestDirectories();
    QzTestNamespace();
    QzTestWaits();
    QzTestBulk();
    QzCheck("no objects or handles leaked by the tests",
            QzObObjectCount() == Objects && QzObHandleCount() == Handles);
    QzPrint("  obtest: %lu passed, %lu failed\n", QzPassed, QzFailed);
}
