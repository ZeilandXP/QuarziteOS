/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Configuration manager self-test (shell command "regtest")
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Exercises the writable SYSTEM hive the way drivers use it, through the
 * exported Zw and Rtl entry points only: keys created, reopened and
 * deleted, values of every storage size set, replaced, enumerated and
 * deleted, volatile keys, the CurrentControlSet link, change notification
 * and the RTL_REGISTRY_* helpers. Everything it creates lives below
 * Services\QzRegTest and is deleted again.
 */

#include "qzio.h"

#define QZR_TAG 'tRzQ'

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

static NTSTATUS
QzrCreate(PCWSTR Path, HANDLE Root, ULONG Options, PHANDLE Key, PULONG Disposition)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    ULONG Ignored;

    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, Root, NULL);
    return ZwCreateKey(Key, KEY_ALL_ACCESS, &Attributes, 0, NULL, Options,
                       Disposition ? Disposition : &Ignored);
}

static NTSTATUS
QzrOpen(PCWSTR Path, HANDLE Root, PHANDLE Key)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;

    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, Root, NULL);
    return ZwOpenKey(Key, KEY_ALL_ACCESS, &Attributes);
}

static NTSTATUS
QzrSet(HANDLE Key, PCWSTR Name, ULONG Type, const VOID *Data, ULONG Length)
{
    UNICODE_STRING ValueName;
    RtlInitUnicodeString(&ValueName, Name);
    return ZwSetValueKey(Key, &ValueName, 0, Type, (PVOID)Data, Length);
}

/* Query a value into Info (a caller buffer of InfoSize bytes) */
static NTSTATUS
QzrGet(HANDLE Key, PCWSTR Name, PKEY_VALUE_PARTIAL_INFORMATION Info, ULONG InfoSize)
{
    UNICODE_STRING ValueName;
    ULONG Result;
    RtlInitUnicodeString(&ValueName, Name);
    return ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Info, InfoSize, &Result);
}

static BOOLEAN
QzrValueIs(HANDLE Key, PCWSTR Name, ULONG Type, const VOID *Data, ULONG Length,
           PKEY_VALUE_PARTIAL_INFORMATION Info, ULONG InfoSize)
{
    return NT_SUCCESS(QzrGet(Key, Name, Info, InfoSize)) && Info->Type == Type &&
           Info->DataLength == Length && RtlCompareMemory(Info->Data, Data, Length) == Length;
}

static ULONG
QzrCountValues(HANDLE Key)
{
    UCHAR Buffer[256];
    ULONG Index, Result;

    for (Index = 0; Index < 1000; Index++)
        if (!NT_SUCCESS(ZwEnumerateValueKey(Key, Index, KeyValueBasicInformation, Buffer,
                                            sizeof(Buffer), &Result)))
            break;
    return Index;
}

static BOOLEAN
QzrSignaled(HANDLE Event)
{
    LARGE_INTEGER Zero;
    Zero.QuadPart = 0;
    return ZwWaitForSingleObject(Event, FALSE, &Zero) == STATUS_SUCCESS;
}

static NTSTATUS NTAPI
QzrQueryRoutine(PWSTR ValueName, ULONG ValueType, PVOID ValueData, ULONG ValueLength,
                PVOID Context, PVOID EntryContext)
{
    UNREFERENCED_PARAMETER(ValueName);
    UNREFERENCED_PARAMETER(Context);
    if (ValueType == REG_DWORD && ValueLength == sizeof(ULONG))
        *(PULONG)EntryContext = *(PULONG)ValueData;
    return STATUS_SUCCESS;
}

static VOID
QzrTests(PKEY_VALUE_PARTIAL_INFORMATION Info, ULONG InfoSize, PUCHAR Big, ULONG BigSize)
{
    static const WCHAR Services[] = L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\QzRegTest";
    static const WCHAR Direct[] = L"\\Registry\\Machine\\System\\ControlSet001\\Services\\QzRegTest";
    static const WCHAR Text[] = L"QuarziteOS registry self-test";
    ULONG Dword = 0x12345678, Small = 0xBEEF, Disposition, i, Read;
    HANDLE Key = NULL, Again = NULL, Sub = NULL, Child = NULL, Event = NULL, Watched = NULL;
    IO_STATUS_BLOCK IoStatus;
    OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;

    for (i = 0; i < BigSize; i++)
        Big[i] = (UCHAR)(i * 7 + 3);

    /* Keys */
    Status = QzrCreate(Services, NULL, 0, &Key, &Disposition);
    QzCheck("create Services\\QzRegTest", NT_SUCCESS(Status) && Disposition == REG_CREATED_NEW_KEY);
    if (!NT_SUCCESS(Status))
        return;
    Status = QzrCreate(Services, NULL, 0, &Again, &Disposition);
    QzCheck("create again opens the existing key", NT_SUCCESS(Status) && Disposition == REG_OPENED_EXISTING_KEY);
    if (Again)
        ZwClose(Again), Again = NULL;

    /* Values: inline (<= 4 bytes), a data cell, a large data cell */
    QzCheck("set REG_DWORD", NT_SUCCESS(QzrSet(Key, L"Dword", REG_DWORD, &Dword, sizeof(Dword))));
    QzCheck("set 2-byte binary", NT_SUCCESS(QzrSet(Key, L"Small", REG_BINARY, &Small, 2)));
    QzCheck("set REG_SZ", NT_SUCCESS(QzrSet(Key, L"Text", REG_SZ, Text, sizeof(Text))));
    QzCheck("set 3000-byte binary", NT_SUCCESS(QzrSet(Key, L"Big", REG_BINARY, Big, 3000)));
    QzCheck("set the default value", NT_SUCCESS(QzrSet(Key, L"", REG_DWORD, &Small, sizeof(Small))));
    QzCheck("REG_DWORD reads back", QzrValueIs(Key, L"Dword", REG_DWORD, &Dword, sizeof(Dword), Info, InfoSize));
    QzCheck("small binary reads back", QzrValueIs(Key, L"Small", REG_BINARY, &Small, 2, Info, InfoSize));
    QzCheck("REG_SZ reads back", QzrValueIs(Key, L"Text", REG_SZ, Text, sizeof(Text), Info, InfoSize));
    QzCheck("big binary reads back", QzrValueIs(Key, L"Big", REG_BINARY, Big, 3000, Info, InfoSize));
    QzCheck("value names are case-insensitive",
            QzrValueIs(Key, L"dWORD", REG_DWORD, &Dword, sizeof(Dword), Info, InfoSize));
    QzCheck("five values enumerated", QzrCountValues(Key) == 5);

    /* Replacing a value changes its type, size and data in place */
    QzCheck("replace big with a DWORD", NT_SUCCESS(QzrSet(Key, L"Big", REG_DWORD, &Dword, sizeof(Dword))) &&
            QzrValueIs(Key, L"Big", REG_DWORD, &Dword, sizeof(Dword), Info, InfoSize));
    QzCheck("replace a DWORD with 1000 bytes", NT_SUCCESS(QzrSet(Key, L"Dword", REG_BINARY, Big, 1000)) &&
            QzrValueIs(Key, L"Dword", REG_BINARY, Big, 1000, Info, InfoSize));
    QzCheck("still five values", QzrCountValues(Key) == 5);

    /* CurrentControlSet is a link to ControlSet001 */
    Status = QzrOpen(Direct, NULL, &Again);
    QzCheck("the key is visible through ControlSet001", NT_SUCCESS(Status) &&
            QzrValueIs(Again, L"Text", REG_SZ, Text, sizeof(Text), Info, InfoSize));
    if (Again)
        ZwClose(Again), Again = NULL;
    Status = QzrOpen(L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\ServiceGroupOrder", NULL, &Again);
    QzCheck("CurrentControlSet resolves existing keys", NT_SUCCESS(Status));
    if (Again)
        ZwClose(Again), Again = NULL;

    /* Relative opens, volatile keys */
    Status = QzrCreate(L"Volatile", Key, REG_OPTION_VOLATILE, &Sub, &Disposition);
    QzCheck("create a volatile subkey (relative)", NT_SUCCESS(Status) && Disposition == REG_CREATED_NEW_KEY);
    Status = QzrCreate(L"Stable", Sub, 0, &Child, NULL);
    QzCheck("a stable key below a volatile one is refused", Status == STATUS_CHILD_MUST_BE_VOLATILE);
    if (NT_SUCCESS(Status))
        ZwClose(Child), Child = NULL;
    Status = QzrCreate(L"Inner", Sub, REG_OPTION_VOLATILE, &Child, NULL);
    QzCheck("a volatile key below a volatile one", NT_SUCCESS(Status) &&
            NT_SUCCESS(QzrSet(Child, L"V", REG_DWORD, &Dword, sizeof(Dword))));

    /* Deleting */
    QzCheck("a key with subkeys cannot be deleted", ZwDeleteKey(Sub) == STATUS_CANNOT_DELETE);
    QzCheck("delete the innermost key", Child && NT_SUCCESS(ZwDeleteKey(Child)));
    if (Child)
    {
        QzCheck("a deleted key's handle refuses writes",
                QzrSet(Child, L"V", REG_DWORD, &Dword, sizeof(Dword)) == STATUS_KEY_DELETED);
        ZwClose(Child), Child = NULL;
    }
    QzCheck("a deleted key is gone by name", QzrOpen(L"Volatile\\Inner", Key, &Child) == STATUS_OBJECT_NAME_NOT_FOUND);
    QzCheck("delete the volatile key", NT_SUCCESS(ZwDeleteKey(Sub)));
    ZwClose(Sub), Sub = NULL;
    {
        UNICODE_STRING Name = RTL_CONSTANT_STRING(L"Small");
        QzCheck("delete a value", NT_SUCCESS(ZwDeleteValueKey(Key, &Name)) &&
                QzrGet(Key, L"Small", Info, InfoSize) == STATUS_OBJECT_NAME_NOT_FOUND &&
                QzrCountValues(Key) == 4);
        QzCheck("deleting it twice fails", ZwDeleteValueKey(Key, &Name) == STATUS_OBJECT_NAME_NOT_FOUND);
    }

    /* Change notification */
    InitializeObjectAttributes(&Attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateEvent(&Event, EVENT_ALL_ACCESS, &Attributes, NotificationEvent, FALSE);
    if (NT_SUCCESS(Status))
    {
        Status = ZwNotifyChangeKey(Key, Event, NULL, NULL, &IoStatus, REG_NOTIFY_CHANGE_LAST_SET,
                                   FALSE, NULL, 0, TRUE);
        QzCheck("notify request pends", Status == STATUS_PENDING && !QzrSignaled(Event));
        QzrCreate(L"NotAValue", Key, 0, &Sub, NULL);
        QzCheck("a new subkey does not satisfy a value filter", !QzrSignaled(Event));
        QzrSet(Key, L"Dword", REG_DWORD, &Dword, sizeof(Dword));
        QzCheck("setting a value signals the event", QzrSignaled(Event) && IoStatus.Status != STATUS_PENDING);

        ZwClearEvent(Event);
        Status = QzrOpen(L"\\Registry\\Machine\\System\\CurrentControlSet\\Services", NULL, &Watched);
        Status = NT_SUCCESS(Status) ? ZwNotifyChangeKey(Watched, Event, NULL, NULL, &IoStatus,
                                                        REG_NOTIFY_CHANGE_NAME, TRUE, NULL, 0, TRUE)
                                    : Status;
        QzCheck("watch the Services tree", Status == STATUS_PENDING);
        if (Sub)
            ZwDeleteKey(Sub);
        QzCheck("deleting a key deep in the tree signals it", QzrSignaled(Event));
        if (Sub)
            ZwClose(Sub), Sub = NULL;

        ZwClearEvent(Event);
        Status = ZwNotifyChangeKey(Watched, Event, NULL, NULL, &IoStatus, REG_NOTIFY_CHANGE_NAME, TRUE,
                                   NULL, 0, TRUE);
        ZwClose(Watched), Watched = NULL;
        QzCheck("closing the watched key ends the request", Status == STATUS_PENDING && QzrSignaled(Event) &&
                IoStatus.Status == STATUS_NOTIFY_CLEANUP);
        ZwClose(Event);
    }
    else
    {
        QzCheck("create an event for notification", FALSE);
    }

    /* The RTL_REGISTRY_* helpers, as drivers record their state */
    Status = RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, L"QzRegTest\\Parameters", L"Setting",
                                   REG_DWORD, &Dword, sizeof(Dword));
    QzCheck("RtlWriteRegistryValue creates the key path", NT_SUCCESS(Status) &&
            NT_SUCCESS(RtlCheckRegistryKey(RTL_REGISTRY_SERVICES, L"QzRegTest\\Parameters")));
    {
        RTL_QUERY_REGISTRY_TABLE Table[2];
        Read = 0;
        RtlZeroMemory(Table, sizeof(Table));
        Table[0].QueryRoutine = QzrQueryRoutine;
        Table[0].Name = L"Setting";
        Table[0].EntryContext = &Read;
        Status = RtlQueryRegistryValues(RTL_REGISTRY_SERVICES, L"QzRegTest\\Parameters", Table, NULL, NULL);
        QzCheck("RtlQueryRegistryValues reads it back", NT_SUCCESS(Status) && Read == Dword);
    }
    Status = RtlDeleteRegistryValue(RTL_REGISTRY_SERVICES, L"QzRegTest\\Parameters", L"Setting");
    QzCheck("RtlDeleteRegistryValue", NT_SUCCESS(Status) &&
            RtlDeleteRegistryValue(RTL_REGISTRY_SERVICES, L"QzRegTest\\Parameters", L"Setting") ==
                STATUS_OBJECT_NAME_NOT_FOUND);

    /* The volatile HARDWARE tree is writable too */
    Status = QzrCreate(L"\\Registry\\Machine\\Hardware\\DeviceMap\\QzRegTest", NULL, REG_OPTION_VOLATILE,
                       &Sub, NULL);
    QzCheck("HARDWARE keys: create, set, delete", NT_SUCCESS(Status) &&
            NT_SUCCESS(QzrSet(Sub, L"X", REG_SZ, Text, sizeof(Text))) &&
            QzrValueIs(Sub, L"X", REG_SZ, Text, sizeof(Text), Info, InfoSize) &&
            NT_SUCCESS(ZwDeleteKey(Sub)));
    if (Sub)
        ZwClose(Sub), Sub = NULL;

    /* Clean up: Parameters, then the key itself */
    if (NT_SUCCESS(QzrOpen(L"Parameters", Key, &Sub)))
    {
        ZwDeleteKey(Sub);
        ZwClose(Sub);
    }
    QzCheck("delete Services\\QzRegTest", NT_SUCCESS(ZwDeleteKey(Key)));
    ZwClose(Key);
    QzCheck("it is gone", QzrOpen(Services, NULL, &Key) == STATUS_OBJECT_NAME_NOT_FOUND);
}

VOID
QzRunRegistryTests(VOID)
{
    ULONG InfoSize = sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 4096, BigSize = 4096;
    PKEY_VALUE_PARTIAL_INFORMATION Info;
    PUCHAR Big;

    QzPassed = QzFailed = 0;
    if (!QzCmHiveReady())
    {
        QzPrint("  regtest: the SYSTEM hive is not mounted\n");
        return;
    }
    Info = ExAllocatePoolWithTag(NonPagedPool, InfoSize, QZR_TAG);
    Big = ExAllocatePoolWithTag(NonPagedPool, BigSize, QZR_TAG);
    if (Info && Big)
        QzrTests(Info, InfoSize, Big, BigSize);
    else
        QzCheck("allocate test buffers", FALSE);
    if (Info)
        ExFreePoolWithTag(Info, QZR_TAG);
    if (Big)
        ExFreePoolWithTag(Big, QZR_TAG);
    QzPrint("  regtest: %lu passed, %lu failed (%lu notification(s) delivered)\n", QzPassed, QzFailed,
            QzCmNotificationsFired());
}
