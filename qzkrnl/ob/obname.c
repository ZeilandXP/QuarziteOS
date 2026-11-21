/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Object namespace: directories, name lookup, symbolic link
 *              reparsing, parse procedures
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * The namespace is protected by one mutex. It is released around parse
 * procedures, which call out into other components (the I/O manager will
 * parse \Device\Foo\path this way in stage B4).
 */

#include "qz.h"

#define QZ_OB_NAME_TAG  'mNbO'
#define QZ_OB_DIR_TAG   'iDbO'
#define QZ_MAX_REPARSE  32

POBJECT_DIRECTORY QzObRootDirectory;
static KMUTEX QzObNamespaceLock;
static BOOLEAN QzObNamespaceLockReady;

VOID
QzObAcquireNamespace(VOID)
{
    if (!QzObNamespaceLockReady)
    {
        KeInitializeMutex(&QzObNamespaceLock, 0);
        QzObNamespaceLockReady = TRUE;
    }
    KeWaitForSingleObject(&QzObNamespaceLock, Executive, KernelMode, FALSE, NULL);
}

VOID
QzObReleaseNamespace(VOID)
{
    KeReleaseMutex(&QzObNamespaceLock, FALSE);
}

static ULONG
QzObpHashName(PCUNICODE_STRING Name)
{
    ULONG Hash = 0, i;
    for (i = 0; i < Name->Length / sizeof(WCHAR); i++)
        Hash = Hash * 31 + RtlUpcaseUnicodeChar(Name->Buffer[i]);
    return Hash;
}

static PVOID
QzObpFindEntry(POBJECT_DIRECTORY Directory, PCUNICODE_STRING Name, BOOLEAN CaseInsensitive)
{
    ULONG Hash = QzObpHashName(Name);
    POBJECT_DIRECTORY_ENTRY Entry;
    POBJECT_HEADER_NAME_INFO NameInfo;

    for (Entry = Directory->HashBuckets[Hash % NUMBER_HASH_BUCKETS]; Entry; Entry = Entry->ChainLink)
    {
        if (Entry->HashValue != Hash)
            continue;
        NameInfo = OBJECT_HEADER_TO_NAME_INFO(OBJECT_TO_OBJECT_HEADER(Entry->Object));
        if (RtlEqualUnicodeString(&NameInfo->Name, Name, CaseInsensitive))
            return Entry->Object;
    }
    return NULL;
}

/* Enter Object in Directory under Component; namespace lock held */
static NTSTATUS
QzObpInsertEntry(POBJECT_DIRECTORY Directory, PVOID Object, PCUNICODE_STRING Component)
{
    POBJECT_HEADER_NAME_INFO NameInfo = OBJECT_HEADER_TO_NAME_INFO(OBJECT_TO_OBJECT_HEADER(Object));
    POBJECT_DIRECTORY_ENTRY Entry;
    PWCHAR Buffer;
    ULONG Hash;

    if (!NameInfo)
        return STATUS_OBJECT_NAME_INVALID;

    Entry = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Entry), QZ_OB_DIR_TAG);
    Buffer = ExAllocatePoolWithTag(NonPagedPool, Component->Length + sizeof(WCHAR), QZ_OB_NAME_TAG);
    if (!Entry || !Buffer)
    {
        if (Entry) ExFreePoolWithTag(Entry, QZ_OB_DIR_TAG);
        if (Buffer) ExFreePoolWithTag(Buffer, QZ_OB_NAME_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* The name info keeps only the last component */
    RtlCopyMemory(Buffer, Component->Buffer, Component->Length);
    Buffer[Component->Length / sizeof(WCHAR)] = UNICODE_NULL;
    if (NameInfo->Name.Buffer)
        ExFreePoolWithTag(NameInfo->Name.Buffer, QZ_OB_NAME_TAG);
    NameInfo->Name.Buffer = Buffer;
    NameInfo->Name.Length = Component->Length;
    NameInfo->Name.MaximumLength = Component->Length + sizeof(WCHAR);

    Hash = QzObpHashName(Component);
    Entry->Object = Object;
    Entry->HashValue = Hash;
    Entry->ChainLink = Directory->HashBuckets[Hash % NUMBER_HASH_BUCKETS];
    Directory->HashBuckets[Hash % NUMBER_HASH_BUCKETS] = Entry;

    /* The name references its directory; the directory references the object */
    NameInfo->Directory = Directory;
    ObfReferenceObject(Directory);
    ObfReferenceObject(Object);
    return STATUS_SUCCESS;
}

/* Remove Object's name from its directory; namespace lock held */
static VOID
QzObpRemoveEntry(PVOID Object)
{
    POBJECT_HEADER_NAME_INFO NameInfo = OBJECT_HEADER_TO_NAME_INFO(OBJECT_TO_OBJECT_HEADER(Object));
    POBJECT_DIRECTORY Directory = NameInfo->Directory;
    POBJECT_DIRECTORY_ENTRY *Link, Entry;
    ULONG Hash = QzObpHashName(&NameInfo->Name);

    for (Link = &Directory->HashBuckets[Hash % NUMBER_HASH_BUCKETS]; (Entry = *Link); Link = &Entry->ChainLink)
    {
        if (Entry->Object == Object)
        {
            *Link = Entry->ChainLink;
            ExFreePoolWithTag(Entry, QZ_OB_DIR_TAG);
            break;
        }
    }
    NameInfo->Directory = NULL;
    ObfDereferenceObject(Directory);
    ObfDereferenceObject(Object);
}

/* A temporary object with no handles loses its name */
VOID
QzObpDeleteNameCheck(PVOID Object)
{
    POBJECT_HEADER Header = OBJECT_TO_OBJECT_HEADER(Object);
    POBJECT_HEADER_NAME_INFO NameInfo = OBJECT_HEADER_TO_NAME_INFO(Header);

    if (!NameInfo || !NameInfo->Directory)
        return;
    if (Header->HandleCount != 0 || (Header->Flags & OB_FLAG_PERMANENT))
        return;

    /* Keep the object alive while we take its name away */
    ObfReferenceObject(Object);
    QzObAcquireNamespace();
    if (NameInfo->Directory && Header->HandleCount == 0 && !(Header->Flags & OB_FLAG_PERMANENT))
        QzObpRemoveEntry(Object);
    QzObReleaseNamespace();
    ObfDereferenceObject(Object);
}

/*
 * Walk ObjectName (absolute, or relative to RootHandle). Namespace lock
 * held on entry and exit; released around parse procedures.
 *  - InsertObject == NULL: *FoundObject receives a referenced object.
 *  - InsertObject != NULL: it is entered under the final component, or
 *    STATUS_OBJECT_NAME_COLLISION is returned with *FoundObject referenced.
 * Symbolic links are followed, except the final component when
 * OBJ_OPENLINK is set or when looking for a symbolic link itself.
 */
NTSTATUS
QzObpLookupObjectName(HANDLE RootHandle,
                      PUNICODE_STRING ObjectName,
                      ULONG Attributes,
                      POBJECT_TYPE ObjectType,
                      KPROCESSOR_MODE AccessMode,
                      PVOID ParseContext,
                      PVOID InsertObject,
                      PVOID *FoundObject)
{
    UNICODE_STRING Remaining, Component, Complete;
    POBJECT_DIRECTORY Directory;
    PVOID Object, RootObject = NULL, ReparseBuffer = NULL;
    BOOLEAN CaseInsensitive;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Reparses = 0;

    *FoundObject = NULL;
    CaseInsensitive = (Attributes & OBJ_CASE_INSENSITIVE) ||
                      (ObjectType && ObjectType->TypeInfo.CaseInsensitive);
    Complete = *ObjectName;

Restart:
    Remaining = Complete;

    if (RootHandle && Reparses == 0)
    {
        QzObReleaseNamespace();
        Status = ObReferenceObjectByHandle(RootHandle, 0, NULL, AccessMode, &RootObject, NULL);
        QzObAcquireNamespace();
        if (!NT_SUCCESS(Status))
            goto Done;
        if (Remaining.Length && Remaining.Buffer[0] == OBJ_NAME_PATH_SEPARATOR)
        {
            Status = STATUS_OBJECT_PATH_SYNTAX_BAD;
            goto Done;
        }
        if (OBJECT_TO_OBJECT_HEADER(RootObject)->Type != ObpDirectoryObjectType)
        {
            Object = RootObject;
            goto ParseObject;   /* e.g. a relative open below a file (B4) */
        }
        Directory = RootObject;
    }
    else
    {
        if (!Remaining.Length || Remaining.Buffer[0] != OBJ_NAME_PATH_SEPARATOR)
        {
            Status = STATUS_OBJECT_PATH_SYNTAX_BAD;
            goto Done;
        }
        Directory = QzObRootDirectory;

        /* "\" names the root itself */
        if (Remaining.Length == sizeof(WCHAR))
        {
            if (InsertObject)
            {
                Status = STATUS_OBJECT_NAME_INVALID;
                goto Done;
            }
            ObfReferenceObject(Directory);
            *FoundObject = Directory;
            goto Done;
        }
    }

    for (;;)
    {
        /* Split off the next component */
        if (Remaining.Length && Remaining.Buffer[0] == OBJ_NAME_PATH_SEPARATOR)
        {
            Remaining.Buffer++;
            Remaining.Length -= sizeof(WCHAR);
            Remaining.MaximumLength -= sizeof(WCHAR);
        }
        Component.Buffer = Remaining.Buffer;
        Component.Length = 0;
        while (Component.Length < Remaining.Length &&
               Remaining.Buffer[Component.Length / sizeof(WCHAR)] != OBJ_NAME_PATH_SEPARATOR)
        {
            Component.Length += sizeof(WCHAR);
        }
        Component.MaximumLength = Component.Length;
        if (Component.Length == 0)
        {
            Status = STATUS_OBJECT_NAME_INVALID;
            goto Done;
        }
        Remaining.Buffer += Component.Length / sizeof(WCHAR);
        Remaining.Length -= Component.Length;
        Remaining.MaximumLength -= Component.Length;

        Object = QzObpFindEntry(Directory, &Component, CaseInsensitive);

        if (!Object)
        {
            if (Remaining.Length == 0 && InsertObject)
            {
                Status = QzObpInsertEntry(Directory, InsertObject, &Component);
                if (NT_SUCCESS(Status))
                    *FoundObject = InsertObject;
            }
            else
            {
                Status = Remaining.Length ? STATUS_OBJECT_PATH_NOT_FOUND
                                          : STATUS_OBJECT_NAME_NOT_FOUND;
            }
            goto Done;
        }

        /* Symbolic link: substitute its target and start over */
        if (OBJECT_TO_OBJECT_HEADER(Object)->Type == ObpSymbolicLinkObjectType &&
            (Remaining.Length ||
             (!(Attributes & OBJ_OPENLINK) && ObjectType != ObpSymbolicLinkObjectType &&
              !InsertObject)))
        {
            POBJECT_SYMBOLIC_LINK Link = Object;
            ULONG NewLength = Link->LinkTarget.Length + Remaining.Length;
            PWCHAR NewBuffer;

            if (++Reparses > QZ_MAX_REPARSE || NewLength > MAXUSHORT)
            {
                Status = STATUS_OBJECT_PATH_NOT_FOUND;
                goto Done;
            }
            NewBuffer = ExAllocatePoolWithTag(NonPagedPool, NewLength + sizeof(WCHAR), QZ_OB_NAME_TAG);
            if (!NewBuffer)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto Done;
            }
            RtlCopyMemory(NewBuffer, Link->LinkTarget.Buffer, Link->LinkTarget.Length);
            RtlCopyMemory((PUCHAR)NewBuffer + Link->LinkTarget.Length, Remaining.Buffer,
                          Remaining.Length);
            NewBuffer[NewLength / sizeof(WCHAR)] = UNICODE_NULL;
            if (ReparseBuffer)
                ExFreePoolWithTag(ReparseBuffer, QZ_OB_NAME_TAG);
            ReparseBuffer = NewBuffer;
            Complete.Buffer = NewBuffer;
            Complete.Length = (USHORT)NewLength;
            Complete.MaximumLength = (USHORT)(NewLength + sizeof(WCHAR));
            goto Restart;
        }

        if (Remaining.Length == 0)
        {
            if (InsertObject)
            {
                Status = STATUS_OBJECT_NAME_COLLISION;
                ObfReferenceObject(Object);
                *FoundObject = Object;
                goto Done;
            }
            if (OBJECT_TO_OBJECT_HEADER(Object)->Type->TypeInfo.ParseProcedure &&
                OBJECT_TO_OBJECT_HEADER(Object)->Type != ObpDirectoryObjectType)
            {
                goto ParseObject;
            }
            ObfReferenceObject(Object);
            *FoundObject = Object;
            goto Done;
        }

        if (OBJECT_TO_OBJECT_HEADER(Object)->Type == ObpDirectoryObjectType)
        {
            Directory = Object;
            continue;
        }

ParseObject:
        /* Hand the rest of the name to the object's parse procedure */
        {
            OB_PARSE_METHOD Parse = OBJECT_TO_OBJECT_HEADER(Object)->Type->TypeInfo.ParseProcedure;
            PVOID Result = NULL;

            if (!Parse || InsertObject)
            {
                Status = STATUS_OBJECT_PATH_NOT_FOUND;
                goto Done;
            }

            ObfReferenceObject(Object);
            QzObReleaseNamespace();
            Status = Parse(Object, ObjectType, NULL, AccessMode, Attributes,
                           &Complete, &Remaining, ParseContext, NULL, &Result);
            QzObAcquireNamespace();
            ObfDereferenceObject(Object);

            if (Status == STATUS_REPARSE)
            {
                if (++Reparses > QZ_MAX_REPARSE)
                {
                    Status = STATUS_OBJECT_PATH_NOT_FOUND;
                    goto Done;
                }
                goto Restart;   /* the parse routine rewrote Complete */
            }
            if (NT_SUCCESS(Status))
                *FoundObject = Result;
            goto Done;
        }
    }

Done:
    if (RootObject)
        ObfDereferenceObject(RootObject);
    if (ReparseBuffer)
        ExFreePoolWithTag(ReparseBuffer, QZ_OB_NAME_TAG);
    return Status;
}

/* ---------------------------------------------------------------------- */
/* Exports                                                                 */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
ObReferenceObjectByName(IN PUNICODE_STRING ObjectPath,
                        IN ULONG Attributes,
                        IN PACCESS_STATE PassedAccessState OPTIONAL,
                        IN ACCESS_MASK DesiredAccess OPTIONAL,
                        IN POBJECT_TYPE ObjectType,
                        IN KPROCESSOR_MODE AccessMode,
                        IN OUT PVOID ParseContext OPTIONAL,
                        OUT PVOID *Object)
{
    NTSTATUS Status;
    PVOID Found;

    UNREFERENCED_PARAMETER(PassedAccessState);
    UNREFERENCED_PARAMETER(DesiredAccess);

    if (!ObjectPath || !ObjectPath->Length)
        return STATUS_OBJECT_NAME_INVALID;

    QzObAcquireNamespace();
    Status = QzObpLookupObjectName(NULL, ObjectPath, Attributes, ObjectType, AccessMode,
                                   ParseContext, NULL, &Found);
    QzObReleaseNamespace();
    if (!NT_SUCCESS(Status))
        return Status;

    if (ObjectType && OBJECT_TO_OBJECT_HEADER(Found)->Type != ObjectType)
    {
        ObfDereferenceObject(Found);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    *Object = Found;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
ObOpenObjectByName(IN POBJECT_ATTRIBUTES ObjectAttributes,
                   IN POBJECT_TYPE ObjectType,
                   IN KPROCESSOR_MODE AccessMode,
                   IN PACCESS_STATE PassedAccessState,
                   IN ACCESS_MASK DesiredAccess,
                   IN OUT PVOID ParseContext,
                   OUT PHANDLE Handle)
{
    NTSTATUS Status;
    PVOID Found;
    POBJECT_TYPE FoundType;

    UNREFERENCED_PARAMETER(PassedAccessState);

    *Handle = NULL;
    if (!ObjectAttributes || !ObjectAttributes->ObjectName)
        return STATUS_OBJECT_NAME_INVALID;
    if (!ObjectAttributes->ObjectName->Length && !ObjectAttributes->RootDirectory)
        return STATUS_OBJECT_NAME_INVALID;

    QzObAcquireNamespace();
    if (!ObjectAttributes->ObjectName->Length)
    {
        /* Empty name relative to a handle: the root object itself */
        QzObReleaseNamespace();
        Status = ObReferenceObjectByHandle(ObjectAttributes->RootDirectory, 0, NULL,
                                           AccessMode, &Found, NULL);
        QzObAcquireNamespace();
    }
    else
    {
        Status = QzObpLookupObjectName(ObjectAttributes->RootDirectory,
                                       ObjectAttributes->ObjectName,
                                       ObjectAttributes->Attributes, ObjectType,
                                       AccessMode, ParseContext, NULL, &Found);
    }
    QzObReleaseNamespace();
    if (!NT_SUCCESS(Status))
        return Status;

    FoundType = OBJECT_TO_OBJECT_HEADER(Found)->Type;
    if (ObjectType && FoundType != ObjectType)
    {
        ObfDereferenceObject(Found);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }

    Status = QzObpCreateHandle(Found, QzObpMapDesiredAccess(FoundType, DesiredAccess),
                               ObjectAttributes->Attributes & (OBJ_INHERIT | OBJ_KERNEL_HANDLE),
                               Handle);
    if (!NT_SUCCESS(Status))
        ObfDereferenceObject(Found);
    return Status;
}
