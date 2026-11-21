/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Object manager: directory and symbolic link objects, their
 *              services, and the standard namespace
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qz.h"

#define QZ_OB_LINK_TAG  'nLbO'

POBJECT_TYPE ObpDirectoryObjectType;
POBJECT_TYPE ObpSymbolicLinkObjectType;

/* I/O manager types. Created here so drivers can already name them; the
   I/O manager (stage B4) gives them their procedures. */
POBJECT_TYPE IoAdapterObjectType;
POBJECT_TYPE IoDeviceObjectType;
POBJECT_TYPE IoDriverObjectType;
POBJECT_TYPE IoFileObjectType;


static GENERIC_MAPPING QzDirectoryMapping =
{
    STANDARD_RIGHTS_READ | DIRECTORY_QUERY | DIRECTORY_TRAVERSE,
    STANDARD_RIGHTS_WRITE | DIRECTORY_CREATE_OBJECT | DIRECTORY_CREATE_SUBDIRECTORY,
    STANDARD_RIGHTS_EXECUTE | DIRECTORY_QUERY | DIRECTORY_TRAVERSE,
    DIRECTORY_ALL_ACCESS
};

static GENERIC_MAPPING QzSymbolicLinkMapping =
{
    STANDARD_RIGHTS_READ | SYMBOLIC_LINK_QUERY,
    STANDARD_RIGHTS_WRITE,
    STANDARD_RIGHTS_EXECUTE | SYMBOLIC_LINK_QUERY,
    SYMBOLIC_LINK_ALL_ACCESS
};

static GENERIC_MAPPING QzTypeMapping =
{
    STANDARD_RIGHTS_READ,
    STANDARD_RIGHTS_WRITE,
    STANDARD_RIGHTS_EXECUTE,
    0x000F0001
};

static GENERIC_MAPPING QzIoMapping =
{
    STANDARD_RIGHTS_READ | FILE_READ_DATA,
    STANDARD_RIGHTS_WRITE | FILE_WRITE_DATA,
    STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
    FILE_ALL_ACCESS
};

static VOID NTAPI
QzObpDeleteSymbolicLink(PVOID Object)
{
    POBJECT_SYMBOLIC_LINK Link = Object;
    if (Link->LinkTarget.Buffer)
        ExFreePoolWithTag(Link->LinkTarget.Buffer, QZ_OB_LINK_TAG);
}

static NTSTATUS
QzObpCreateType(PCWSTR Name, ULONG ValidAccess, PGENERIC_MAPPING Mapping, BOOLEAN UseDefault,
                OB_DELETE_METHOD DeleteProcedure, POBJECT_TYPE *Type)
{
    OBJECT_TYPE_INITIALIZER Init;
    UNICODE_STRING TypeName;

    RtlZeroMemory(&Init, sizeof(Init));
    Init.Length = sizeof(Init);
    Init.ValidAccessMask = ValidAccess;
    Init.GenericMapping = *Mapping;
    Init.PoolType = NonPagedPool;
    Init.UseDefaultObject = UseDefault;
    Init.CaseInsensitive = TRUE;
    Init.InvalidAttributes = OBJ_OPENLINK;
    Init.DeleteProcedure = DeleteProcedure;
    RtlInitUnicodeString(&TypeName, Name);
    return ObCreateObjectType(&TypeName, &Init, NULL, Type);
}

/* ---------------------------------------------------------------------- */
/* Directory services                                                      */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
ZwCreateDirectoryObject(OUT PHANDLE DirectoryHandle,
                        IN ACCESS_MASK DesiredAccess,
                        IN POBJECT_ATTRIBUTES ObjectAttributes)
{
    PVOID Directory;
    NTSTATUS Status;

    *DirectoryHandle = NULL;
    Status = ObCreateObject(KernelMode, ObpDirectoryObjectType, ObjectAttributes, KernelMode,
                            NULL, sizeof(OBJECT_DIRECTORY), 0, 0, &Directory);
    if (!NT_SUCCESS(Status))
        return Status;
    return ObInsertObject(Directory, NULL, DesiredAccess, 0, NULL, DirectoryHandle);
}

NTSTATUS
NTAPI
NtCreateDirectoryObject(OUT PHANDLE DirectoryHandle,
                        IN ACCESS_MASK DesiredAccess,
                        IN POBJECT_ATTRIBUTES ObjectAttributes)
{
    return ZwCreateDirectoryObject(DirectoryHandle, DesiredAccess, ObjectAttributes);
}

NTSTATUS
NTAPI
ZwOpenDirectoryObject(OUT PHANDLE DirectoryHandle,
                      IN ACCESS_MASK DesiredAccess,
                      IN POBJECT_ATTRIBUTES ObjectAttributes)
{
    return ObOpenObjectByName(ObjectAttributes, ObpDirectoryObjectType, KernelMode, NULL,
                              DesiredAccess, NULL, DirectoryHandle);
}

/*
 * Buffer layout (as in NT): an array of OBJECT_DIRECTORY_INFORMATION ended
 * by an all-zero entry, followed by the name strings. *Context counts the
 * entries returned so far.
 */
NTSTATUS
NTAPI
ZwQueryDirectoryObject(IN HANDLE DirectoryHandle,
                       OUT PVOID Buffer,
                       IN ULONG BufferLength,
                       IN BOOLEAN ReturnSingleEntry,
                       IN BOOLEAN RestartScan,
                       IN OUT PULONG Context,
                       OUT PULONG ReturnLength OPTIONAL)
{
    POBJECT_DIRECTORY Directory;
    POBJECT_DIRECTORY_ENTRY Entry;
    POBJECT_DIRECTORY_INFORMATION Info = Buffer;
    ULONG Skip, Index = 0, Count = 0, Bucket, Used, Total = 0;
    ULONG HeaderBytes, StringBytes = 0;
    BOOLEAN More = FALSE, Stop = FALSE;
    NTSTATUS Status;
    PUCHAR Strings;

    Status = ObReferenceObjectByHandle(DirectoryHandle, DIRECTORY_QUERY, ObpDirectoryObjectType,
                                       KernelMode, (PVOID *)&Directory, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Skip = RestartScan ? 0 : *Context;
    QzObAcquireNamespace();

    /* First pass: how many entries fit? */
    for (Bucket = 0; Bucket < NUMBER_HASH_BUCKETS && !More && !Stop; Bucket++)
    {
        for (Entry = Directory->HashBuckets[Bucket]; Entry; Entry = Entry->ChainLink)
        {
            POBJECT_HEADER Header = OBJECT_TO_OBJECT_HEADER(Entry->Object);
            POBJECT_HEADER_NAME_INFO NameInfo = OBJECT_HEADER_TO_NAME_INFO(Header);
            ULONG Need;

            if (Index++ < Skip)
                continue;
            if (ReturnSingleEntry && Count == 1)
            {
                Stop = TRUE;
                break;
            }
            Need = sizeof(OBJECT_DIRECTORY_INFORMATION) + NameInfo->Name.Length + sizeof(WCHAR) +
                   Header->Type->Name.Length + sizeof(WCHAR);
            if ((Count + 2) * sizeof(OBJECT_DIRECTORY_INFORMATION) +
                    StringBytes + Need - sizeof(OBJECT_DIRECTORY_INFORMATION) > BufferLength)
            {
                More = TRUE;
                Total = Need + sizeof(OBJECT_DIRECTORY_INFORMATION);
                break;
            }
            StringBytes += Need - sizeof(OBJECT_DIRECTORY_INFORMATION);
            Count++;
        }
    }

    if (Count == 0)
    {
        QzObReleaseNamespace();
        ObfDereferenceObject(Directory);
        if (More)
        {
            if (ReturnLength)
                *ReturnLength = Total;
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (ReturnLength)
            *ReturnLength = 0;
        return STATUS_NO_MORE_ENTRIES;
    }

    /* Second pass: fill */
    HeaderBytes = (Count + 1) * sizeof(OBJECT_DIRECTORY_INFORMATION);
    Strings = (PUCHAR)Buffer + HeaderBytes;
    Index = 0;
    Used = 0;
    for (Bucket = 0; Bucket < NUMBER_HASH_BUCKETS && Used < Count; Bucket++)
    {
        for (Entry = Directory->HashBuckets[Bucket]; Entry && Used < Count; Entry = Entry->ChainLink)
        {
            POBJECT_HEADER Header = OBJECT_TO_OBJECT_HEADER(Entry->Object);
            POBJECT_HEADER_NAME_INFO NameInfo = OBJECT_HEADER_TO_NAME_INFO(Header);
            PCUNICODE_STRING Strs[2];
            PUNICODE_STRING Outs[2];
            ULONG k;

            if (Index++ < Skip)
                continue;
            Strs[0] = &NameInfo->Name;
            Strs[1] = &Header->Type->Name;
            Outs[0] = &Info[Used].Name;
            Outs[1] = &Info[Used].TypeName;
            for (k = 0; k < 2; k++)
            {
                Outs[k]->Buffer = (PWCHAR)Strings;
                Outs[k]->Length = Strs[k]->Length;
                Outs[k]->MaximumLength = Strs[k]->Length + sizeof(WCHAR);
                RtlCopyMemory(Strings, Strs[k]->Buffer, Strs[k]->Length);
                Strings += Strs[k]->Length;
                *(PWCHAR)Strings = UNICODE_NULL;
                Strings += sizeof(WCHAR);
            }
            Used++;
        }
    }
    RtlZeroMemory(&Info[Count], sizeof(OBJECT_DIRECTORY_INFORMATION));

    QzObReleaseNamespace();
    ObfDereferenceObject(Directory);

    *Context = Skip + Count;
    if (ReturnLength)
        *ReturnLength = HeaderBytes + StringBytes;
    return More ? STATUS_MORE_ENTRIES : STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NtQueryDirectoryObject(IN HANDLE DirectoryHandle,
                       OUT PVOID Buffer,
                       IN ULONG BufferLength,
                       IN BOOLEAN ReturnSingleEntry,
                       IN BOOLEAN RestartScan,
                       IN OUT PULONG Context,
                       OUT PULONG ReturnLength OPTIONAL)
{
    return ZwQueryDirectoryObject(DirectoryHandle, Buffer, BufferLength, ReturnSingleEntry,
                                  RestartScan, Context, ReturnLength);
}

/* ---------------------------------------------------------------------- */
/* Symbolic link services                                                  */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
ZwCreateSymbolicLinkObject(OUT PHANDLE LinkHandle,
                           IN ACCESS_MASK DesiredAccess,
                           IN POBJECT_ATTRIBUTES ObjectAttributes,
                           IN PUNICODE_STRING LinkTarget)
{
    POBJECT_SYMBOLIC_LINK Link;
    NTSTATUS Status;

    *LinkHandle = NULL;
    if (!LinkTarget || !LinkTarget->Length || (LinkTarget->Length & 1))
        return STATUS_INVALID_PARAMETER;

    Status = ObCreateObject(KernelMode, ObpSymbolicLinkObjectType, ObjectAttributes, KernelMode,
                            NULL, sizeof(OBJECT_SYMBOLIC_LINK), 0, 0, (PVOID *)&Link);
    if (!NT_SUCCESS(Status))
        return Status;

    KeQuerySystemTime(&Link->CreationTime);
    Link->LinkTarget.Buffer = ExAllocatePoolWithTag(NonPagedPool,
                                                    LinkTarget->Length + sizeof(WCHAR),
                                                    QZ_OB_LINK_TAG);
    if (!Link->LinkTarget.Buffer)
    {
        ObfDereferenceObject(Link);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(Link->LinkTarget.Buffer, LinkTarget->Buffer, LinkTarget->Length);
    Link->LinkTarget.Buffer[LinkTarget->Length / sizeof(WCHAR)] = UNICODE_NULL;
    Link->LinkTarget.Length = LinkTarget->Length;
    Link->LinkTarget.MaximumLength = LinkTarget->Length + sizeof(WCHAR);

    return ObInsertObject(Link, NULL, DesiredAccess, 0, NULL, LinkHandle);
}

NTSTATUS
NTAPI
NtCreateSymbolicLinkObject(OUT PHANDLE LinkHandle,
                           IN ACCESS_MASK DesiredAccess,
                           IN POBJECT_ATTRIBUTES ObjectAttributes,
                           IN PUNICODE_STRING LinkTarget)
{
    return ZwCreateSymbolicLinkObject(LinkHandle, DesiredAccess, ObjectAttributes, LinkTarget);
}

NTSTATUS
NTAPI
ZwOpenSymbolicLinkObject(OUT PHANDLE LinkHandle,
                         IN ACCESS_MASK DesiredAccess,
                         IN POBJECT_ATTRIBUTES ObjectAttributes)
{
    return ObOpenObjectByName(ObjectAttributes, ObpSymbolicLinkObjectType, KernelMode, NULL,
                              DesiredAccess, NULL, LinkHandle);
}

NTSTATUS
NTAPI
ZwQuerySymbolicLinkObject(IN HANDLE LinkHandle,
                          IN OUT PUNICODE_STRING LinkTarget,
                          OUT PULONG ResultLength OPTIONAL)
{
    POBJECT_SYMBOLIC_LINK Link;
    NTSTATUS Status;
    ULONG Needed;

    Status = ObReferenceObjectByHandle(LinkHandle, SYMBOLIC_LINK_QUERY, ObpSymbolicLinkObjectType,
                                       KernelMode, (PVOID *)&Link, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Needed = Link->LinkTarget.MaximumLength;
    if (ResultLength)
        *ResultLength = Needed;
    if (LinkTarget->MaximumLength < Link->LinkTarget.Length)
    {
        Status = STATUS_BUFFER_TOO_SMALL;
    }
    else
    {
        RtlCopyMemory(LinkTarget->Buffer, Link->LinkTarget.Buffer, Link->LinkTarget.Length);
        LinkTarget->Length = Link->LinkTarget.Length;
        if (LinkTarget->MaximumLength >= Needed)
            LinkTarget->Buffer[LinkTarget->Length / sizeof(WCHAR)] = UNICODE_NULL;
    }
    ObfDereferenceObject(Link);
    return Status;
}

NTSTATUS
NTAPI
NtQuerySymbolicLinkObject(IN HANDLE LinkHandle,
                          IN OUT PUNICODE_STRING LinkTarget,
                          OUT PULONG ResultLength OPTIONAL)
{
    return ZwQuerySymbolicLinkObject(LinkHandle, LinkTarget, ResultLength);
}

/* ---------------------------------------------------------------------- */
/* Standard namespace                                                      */
/* ---------------------------------------------------------------------- */

static VOID
QzObpMakeDirectory(PCWSTR Path)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Handle;
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_PERMANENT | OBJ_CASE_INSENSITIVE,
                               NULL, NULL);
    Status = ZwCreateDirectoryObject(&Handle, DIRECTORY_ALL_ACCESS, &Attributes);
    if (!NT_SUCCESS(Status))
        QzBugCheckWithText(OBJECT_INITIALIZATION_FAILED, 1, Status, 0, 0,
                           "cannot create a standard object directory");
    ZwClose(Handle);
}

static VOID
QzObpMakeLink(PCWSTR Path, PUNICODE_STRING Target)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Handle;
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_PERMANENT | OBJ_CASE_INSENSITIVE,
                               NULL, NULL);
    Status = ZwCreateSymbolicLinkObject(&Handle, SYMBOLIC_LINK_ALL_ACCESS, &Attributes, Target);
    if (!NT_SUCCESS(Status))
        QzBugCheckWithText(OBJECT_INITIALIZATION_FAILED, 2, Status, 0, 0,
                           "cannot create a standard symbolic link");
    ZwClose(Handle);
}

static VOID
QzObpPublishType(POBJECT_TYPE Type)
{
    WCHAR Buffer[64];
    UNICODE_STRING Path;
    PVOID Found;

    Path.Buffer = Buffer;
    Path.Length = 0;
    Path.MaximumLength = sizeof(Buffer);
    RtlAppendUnicodeToString(&Path, L"\\ObjectTypes\\");
    RtlAppendUnicodeStringToString(&Path, &Type->Name);
    QzObAcquireNamespace();
    QzObpLookupObjectName(NULL, &Path, OBJ_CASE_INSENSITIVE, NULL, KernelMode, NULL, Type, &Found);
    QzObReleaseNamespace();
}

/* "\ArcName\multi(0)disk(0)rdisk(0)partition(1)\WINDOWS" from the loader */
static VOID
QzObpMakeSystemRoot(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    WCHAR Buffer[256];
    UNICODE_STRING Target;
    PCSTR Parts[2];
    ULONG i;

    Target.Buffer = Buffer;
    Target.Length = 0;
    Target.MaximumLength = sizeof(Buffer);
    RtlAppendUnicodeToString(&Target, L"\\ArcName\\");

    Parts[0] = LoaderBlock ? LoaderBlock->ArcBootDeviceName : NULL;
    Parts[1] = LoaderBlock ? LoaderBlock->NtBootPathName : NULL;
    for (i = 0; i < 2; i++)
    {
        PCSTR p = Parts[i] ? Parts[i] : (i ? "\\WINDOWS" : "multi(0)disk(0)rdisk(0)partition(1)");
        for (; *p && Target.Length + sizeof(WCHAR) < Target.MaximumLength; p++)
            Buffer[Target.Length / sizeof(WCHAR)] = (UCHAR)*p, Target.Length += sizeof(WCHAR);
    }

    /* NtBootPathName ends with a backslash ("\WINDOWS\") */
    while (Target.Length > sizeof(WCHAR) && Buffer[Target.Length / sizeof(WCHAR) - 1] == L'\\')
        Target.Length -= sizeof(WCHAR);

    QzObpMakeLink(L"\\SystemRoot", &Target);
}

VOID
QzObInit(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    static const PCWSTR Directories[] =
    {
        L"\\ArcName", L"\\BaseNamedObjects", L"\\Callback", L"\\Device", L"\\Driver",
        L"\\FileSystem", L"\\KernelObjects", L"\\GLOBAL??",
    };
    UNICODE_STRING Target;
    PVOID Root;
    NTSTATUS Status;
    ULONG i;

    QzObCoreInit();

    /* The three object manager types; the first one is the type-type */
    Status = QzObpCreateType(L"Type", 0x000F0001, &QzTypeMapping, TRUE, NULL, &ObpTypeObjectType);
    if (NT_SUCCESS(Status))
        Status = QzObpCreateType(L"Directory", DIRECTORY_ALL_ACCESS, &QzDirectoryMapping, TRUE,
                                 NULL, &ObpDirectoryObjectType);
    if (NT_SUCCESS(Status))
        Status = QzObpCreateType(L"SymbolicLink", SYMBOLIC_LINK_ALL_ACCESS,
                                 &QzSymbolicLinkMapping, TRUE, QzObpDeleteSymbolicLink,
                                 &ObpSymbolicLinkObjectType);
    if (!NT_SUCCESS(Status))
        QzBugCheckWithText(OBJECT_INITIALIZATION_FAILED, 0, Status, 0, 0,
                           "cannot create the object manager types");

    /* The root directory: unnamed, permanent, kept alive by its creation
       reference forever */
    Status = ObCreateObject(KernelMode, ObpDirectoryObjectType, NULL, KernelMode, NULL,
                            sizeof(OBJECT_DIRECTORY), 0, 0, &Root);
    if (NT_SUCCESS(Status))
        Status = ObInsertObject(Root, NULL, 0, 0, NULL, NULL);
    if (!NT_SUCCESS(Status))
        QzBugCheckWithText(OBJECT_INITIALIZATION_FAILED, 0, Status, 1, 0,
                           "cannot create the root directory");
    OBJECT_TO_OBJECT_HEADER(Root)->Flags |= OB_FLAG_PERMANENT;
    QzObRootDirectory = Root;

    /* \ObjectTypes, then publish the types created before it existed */
    QzObpMakeDirectory(L"\\ObjectTypes");
    QzObpPublishType(ObpTypeObjectType);
    QzObpPublishType(ObpDirectoryObjectType);
    QzObpPublishType(ObpSymbolicLinkObjectType);

    for (i = 0; i < RTL_NUMBER_OF(Directories); i++)
        QzObpMakeDirectory(Directories[i]);

    RtlInitUnicodeString(&Target, L"\\GLOBAL??");
    QzObpMakeLink(L"\\??", &Target);
    RtlInitUnicodeString(&Target, L"\\??");
    QzObpMakeLink(L"\\DosDevices", &Target);
    QzObpMakeSystemRoot(LoaderBlock);

    /* I/O manager types (procedures arrive with stage B4) */
    QzObpCreateType(L"Adapter", FILE_ALL_ACCESS, &QzIoMapping, TRUE, NULL, &IoAdapterObjectType);
    QzObpCreateType(L"Device", FILE_ALL_ACCESS, &QzIoMapping, TRUE, NULL, &IoDeviceObjectType);
    QzObpCreateType(L"Driver", FILE_ALL_ACCESS, &QzIoMapping, TRUE, NULL, &IoDriverObjectType);
    QzObpCreateType(L"File", FILE_ALL_ACCESS, &QzIoMapping, TRUE, NULL, &IoFileObjectType);

    QzExObjectsInit();
}

/* ---------------------------------------------------------------------- */
/* WinObj-style listing for the shell's "obj" command                      */
/* ---------------------------------------------------------------------- */

VOID
QzObDumpDirectory(PCWSTR Path)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Directory;
    ULONG Context = 0, Count = 0, Length;
    NTSTATUS Status;
    union
    {
        OBJECT_DIRECTORY_INFORMATION Info[1];
        UCHAR Bytes[512];
    } Buffer;

    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwOpenDirectoryObject(&Directory, DIRECTORY_QUERY, &Attributes);
    if (!NT_SUCCESS(Status))
    {
        QzPrint("  obj: cannot open %wZ (status 0x%08lx)\n", &Name, Status);
        return;
    }

    QzPrint("  Directory %wZ\n", &Name);
    for (;;)
    {
        Status = ZwQueryDirectoryObject(Directory, &Buffer, sizeof(Buffer), TRUE, Context == 0,
                                        &Context, &Length);
        if (!NT_SUCCESS(Status) || Status == STATUS_NO_MORE_ENTRIES)
            break;

        Count++;
        if (RtlEqualUnicodeString(&Buffer.Info[0].TypeName, &ObpSymbolicLinkObjectType->Name,
                                  FALSE))
        {
            /* Show where the link points */
            WCHAR TargetBuffer[128];
            UNICODE_STRING Target;
            HANDLE Link;
            OBJECT_ATTRIBUTES LinkAttributes;

            Target.Buffer = TargetBuffer;
            Target.Length = 0;
            Target.MaximumLength = sizeof(TargetBuffer);
            InitializeObjectAttributes(&LinkAttributes, &Buffer.Info[0].Name,
                                       OBJ_CASE_INSENSITIVE | OBJ_OPENLINK, Directory, NULL);
            if (NT_SUCCESS(ZwOpenSymbolicLinkObject(&Link, SYMBOLIC_LINK_QUERY, &LinkAttributes)))
            {
                ZwQuerySymbolicLinkObject(Link, &Target, NULL);
                ZwClose(Link);
            }
            QzPrint("    %-24wZ %-14wZ -> %wZ\n", &Buffer.Info[0].Name,
                    &Buffer.Info[0].TypeName, &Target);
        }
        else
        {
            QzPrint("    %-24wZ %wZ\n", &Buffer.Info[0].Name, &Buffer.Info[0].TypeName);
        }
    }
    QzPrint("  %lu object(s)\n", Count);
    ZwClose(Directory);
}
