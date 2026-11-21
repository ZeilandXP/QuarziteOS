/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Object manager: object types, creation, insertion,
 *              reference counting and deletion
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * Objects use the NT 5.2 layouts from the NDK: an optional
 * OBJECT_HEADER_NAME_INFO, then the OBJECT_HEADER, then the body that
 * callers see. Lifetime rules:
 *  - every handle owns one pointer reference;
 *  - a named object in a directory is referenced by that directory;
 *  - a temporary object loses its name when its last handle closes (or when
 *    it is made temporary with no handles left); a permanent one keeps it;
 *  - the object is deleted when the pointer count drops to zero. Above
 *    APC_LEVEL deletion is deferred to the object reaper thread.
 */

#include "qz.h"

#define QZ_OB_TAG       'jbOQ'
#define QZ_OB_NAME_TAG  'mNbO'

POBJECT_TYPE ObpTypeObjectType;
static ULONG QzObTypeCount;
static LIST_ENTRY QzObTypeList;
static volatile LONG QzObLiveObjects;

/* Waiting on objects of types with UseDefaultObject completes at once */
static KEVENT QzObDefaultObject;

/* Deferred deletion */
static PVOID volatile QzObDeferredList;
static KEVENT QzObReaperEvent;
static BOOLEAN QzObReaperRunning;

#undef ObDereferenceObject
#undef ObReferenceObject

static PVOID
QzObpAllocationBase(POBJECT_HEADER Header)
{
    POBJECT_HEADER_NAME_INFO NameInfo = OBJECT_HEADER_TO_NAME_INFO(Header);
    return NameInfo ? (PVOID)NameInfo : (PVOID)Header;
}

ULONG
QzObObjectCount(VOID)
{
    return (ULONG)QzObLiveObjects;
}

PCUNICODE_STRING
QzObTypeName(PVOID Object)
{
    return &OBJECT_TO_OBJECT_HEADER(Object)->Type->Name;
}

/* ---------------------------------------------------------------------- */
/* Deletion                                                                */
/* ---------------------------------------------------------------------- */

static VOID
QzObpDeleteObject(PVOID Object)
{
    POBJECT_HEADER Header = OBJECT_TO_OBJECT_HEADER(Object);
    POBJECT_TYPE Type = Header->Type;
    POBJECT_HEADER_NAME_INFO NameInfo = OBJECT_HEADER_TO_NAME_INFO(Header);

    ASSERT(!NameInfo || !NameInfo->Directory);

    if (Type->TypeInfo.DeleteProcedure)
        Type->TypeInfo.DeleteProcedure(Object);

    if (NameInfo && NameInfo->Name.Buffer)
        ExFreePoolWithTag(NameInfo->Name.Buffer, QZ_OB_NAME_TAG);

    if ((Header->Flags & OB_FLAG_CREATE_INFO) && Header->ObjectCreateInfo)
        ExFreePoolWithTag(Header->ObjectCreateInfo, QZ_OB_TAG);

    InterlockedDecrement((PLONG)&Type->TotalNumberOfObjects);
    InterlockedDecrement(&QzObLiveObjects);
    ExFreePoolWithTag(QzObpAllocationBase(Header), QZ_OB_TAG);
}

static VOID NTAPI
QzObReaperThread(PVOID Context)
{
    PVOID List, Object;
    POBJECT_HEADER Header;

    UNREFERENCED_PARAMETER(Context);
    for (;;)
    {
        KeWaitForSingleObject(&QzObReaperEvent, Executive, KernelMode, FALSE, NULL);

        List = InterlockedExchangePointer((PVOID *)&QzObDeferredList, NULL);
        while (List)
        {
            Header = List;
            List = Header->NextToFree;
            Object = &Header->Body;
            QzObpDeleteObject(Object);
        }
    }
}

LONG_PTR
FASTCALL
ObfDereferenceObject(IN PVOID Object)
{
    POBJECT_HEADER Header = OBJECT_TO_OBJECT_HEADER(Object);
    LONG_PTR Count = InterlockedDecrement((PLONG)&Header->PointerCount);

    if (Count > 0)
        return Count;
    if (Count < 0)
        KeBugCheckEx(REFERENCE_BY_POINTER, (ULONG_PTR)Header->Type, (ULONG_PTR)Object,
                     (ULONG_PTR)Count, Header->HandleCount);

    if (QzGetCurrentIrql() > APC_LEVEL && QzObReaperRunning)
    {
        /* Too high to run delete procedures here: hand it to the reaper */
        PVOID Old;
        do
        {
            Old = QzObDeferredList;
            Header->NextToFree = Old;
        } while (InterlockedCompareExchangePointer((PVOID *)&QzObDeferredList,
                                                   Header, Old) != Old);
        KeSetEvent(&QzObReaperEvent, 0, FALSE);
        return 0;
    }

    QzObpDeleteObject(Object);
    return 0;
}

VOID
NTAPI
ObDereferenceObject(IN PVOID Object)
{
    ObfDereferenceObject(Object);
}

LONG_PTR
FASTCALL
ObfReferenceObject(IN PVOID Object)
{
    return InterlockedIncrement((PLONG)&OBJECT_TO_OBJECT_HEADER(Object)->PointerCount);
}

NTSTATUS
NTAPI
ObReferenceObjectByPointer(IN PVOID Object,
                           IN ACCESS_MASK DesiredAccess,
                           IN POBJECT_TYPE ObjectType,
                           IN KPROCESSOR_MODE AccessMode)
{
    UNREFERENCED_PARAMETER(DesiredAccess);
    UNREFERENCED_PARAMETER(AccessMode);

    if (ObjectType && OBJECT_TO_OBJECT_HEADER(Object)->Type != ObjectType)
        return STATUS_OBJECT_TYPE_MISMATCH;
    ObfReferenceObject(Object);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Types                                                                   */
/* ---------------------------------------------------------------------- */

static PVOID
QzObpAllocateObject(POBJECT_TYPE Type, ULONG BodySize, BOOLEAN Named,
                    KPROCESSOR_MODE OwnershipMode)
{
    ULONG NameSize = Named ? sizeof(OBJECT_HEADER_NAME_INFO) : 0;
    ULONG Size = NameSize + FIELD_OFFSET(OBJECT_HEADER, Body) + BodySize;
    PUCHAR Base;
    POBJECT_HEADER Header;

    Base = ExAllocatePoolWithTag(Type ? Type->TypeInfo.PoolType : NonPagedPool, Size, QZ_OB_TAG);
    if (!Base)
        return NULL;
    RtlZeroMemory(Base, Size);

    Header = (POBJECT_HEADER)(Base + NameSize);
    Header->PointerCount = 1;
    Header->HandleCount = 0;
    Header->Type = Type;
    Header->NameInfoOffset = (UCHAR)NameSize;
    if (OwnershipMode == KernelMode)
        Header->Flags |= OB_FLAG_KERNEL_MODE;

    InterlockedIncrement(&QzObLiveObjects);
    return &Header->Body;
}

NTSTATUS
NTAPI
ObCreateObjectType(IN PUNICODE_STRING TypeName,
                   IN POBJECT_TYPE_INITIALIZER ObjectTypeInitializer,
                   IN PVOID Reserved,
                   OUT POBJECT_TYPE *ObjectType)
{
    POBJECT_TYPE Type;
    PWCHAR NameBuffer;

    UNREFERENCED_PARAMETER(Reserved);

    if (!TypeName || !TypeName->Length || !ObjectTypeInitializer ||
        ObjectTypeInitializer->Length != sizeof(OBJECT_TYPE_INITIALIZER))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* The type-type describes itself */
    Type = QzObpAllocateObject(ObpTypeObjectType, sizeof(OBJECT_TYPE), TRUE, KernelMode);
    if (!Type)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (!ObpTypeObjectType)
        OBJECT_TO_OBJECT_HEADER(Type)->Type = Type;

    NameBuffer = ExAllocatePoolWithTag(NonPagedPool, TypeName->Length + sizeof(WCHAR),
                                       QZ_OB_NAME_TAG);
    if (!NameBuffer)
    {
        ObfDereferenceObject(Type);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(NameBuffer, TypeName->Buffer, TypeName->Length);
    NameBuffer[TypeName->Length / sizeof(WCHAR)] = UNICODE_NULL;

    Type->Name.Buffer = NameBuffer;
    Type->Name.Length = TypeName->Length;
    Type->Name.MaximumLength = TypeName->Length + sizeof(WCHAR);
    Type->TypeInfo = *ObjectTypeInitializer;
    Type->Index = ++QzObTypeCount;
    Type->DefaultObject = ObjectTypeInitializer->UseDefaultObject ? &QzObDefaultObject : NULL;
    Type->Key = *(PULONG)NameBuffer;   /* NT uses the first four bytes too */
    InsertTailList(&QzObTypeList, &Type->TypeList);

    /* The name info carries the type name for \ObjectTypes */
    {
        POBJECT_HEADER_NAME_INFO NameInfo = OBJECT_HEADER_TO_NAME_INFO(OBJECT_TO_OBJECT_HEADER(Type));
        NameInfo->Name.Buffer = ExAllocatePoolWithTag(NonPagedPool, Type->Name.MaximumLength,
                                                      QZ_OB_NAME_TAG);
        if (NameInfo->Name.Buffer)
        {
            RtlCopyMemory(NameInfo->Name.Buffer, NameBuffer, Type->Name.MaximumLength);
            NameInfo->Name.Length = Type->Name.Length;
            NameInfo->Name.MaximumLength = Type->Name.MaximumLength;
        }
    }

    if (!ObpTypeObjectType)
        ObpTypeObjectType = Type;
    ObpTypeObjectType->TotalNumberOfObjects++;
    OBJECT_TO_OBJECT_HEADER(Type)->Flags |= OB_FLAG_PERMANENT;

    /* Publish under \ObjectTypes once that directory exists */
    if (QzObRootDirectory)
    {
        UNICODE_STRING Path;
        WCHAR Buffer[64];
        PVOID Existing;

        Path.Buffer = Buffer;
        Path.Length = 0;
        Path.MaximumLength = sizeof(Buffer);
        RtlAppendUnicodeToString(&Path, L"\\ObjectTypes\\");
        RtlAppendUnicodeStringToString(&Path, &Type->Name);
        QzObAcquireNamespace();
        QzObpLookupObjectName(NULL, &Path, OBJ_CASE_INSENSITIVE, NULL, KernelMode,
                              NULL, Type, &Existing);
        QzObReleaseNamespace();
    }

    *ObjectType = Type;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Creation and insertion                                                  */
/* ---------------------------------------------------------------------- */

ACCESS_MASK
QzObpMapDesiredAccess(POBJECT_TYPE Type, ACCESS_MASK DesiredAccess)
{
    ACCESS_MASK Access = DesiredAccess;

    if (Access & MAXIMUM_ALLOWED)
        Access = (Access & ~MAXIMUM_ALLOWED) | GENERIC_ALL;
    RtlMapGenericMask(&Access, &Type->TypeInfo.GenericMapping);
    return Access & (Type->TypeInfo.ValidAccessMask | ACCESS_SYSTEM_SECURITY);
}

NTSTATUS
NTAPI
ObCreateObject(IN KPROCESSOR_MODE ProbeMode OPTIONAL,
               IN POBJECT_TYPE Type,
               IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL,
               IN KPROCESSOR_MODE AccessMode,
               IN OUT PVOID ParseContext OPTIONAL,
               IN ULONG ObjectSize,
               IN ULONG PagedPoolCharge OPTIONAL,
               IN ULONG NonPagedPoolCharge OPTIONAL,
               OUT PVOID *Object)
{
    PUNICODE_STRING Name = ObjectAttributes ? ObjectAttributes->ObjectName : NULL;
    BOOLEAN Named = (Name && Name->Length);
    POBJECT_HEADER Header;
    POBJECT_CREATE_INFORMATION CreateInfo;
    PVOID Body;

    UNREFERENCED_PARAMETER(PagedPoolCharge);
    UNREFERENCED_PARAMETER(NonPagedPoolCharge);

    if (!Type)
        return STATUS_INVALID_PARAMETER;
    if (ObjectAttributes && (ObjectAttributes->Attributes & Type->TypeInfo.InvalidAttributes))
        return STATUS_INVALID_PARAMETER;

    CreateInfo = ExAllocatePoolWithTag(NonPagedPool, sizeof(*CreateInfo), QZ_OB_TAG);
    if (!CreateInfo)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(CreateInfo, sizeof(*CreateInfo));
    CreateInfo->ProbeMode = ProbeMode;
    CreateInfo->ParseContext = ParseContext;
    if (ObjectAttributes)
    {
        CreateInfo->Attributes = ObjectAttributes->Attributes & OBJ_VALID_KERNEL_ATTRIBUTES;
        CreateInfo->RootDirectory = ObjectAttributes->RootDirectory;
        CreateInfo->SecurityDescriptor = ObjectAttributes->SecurityDescriptor;
    }

    Body = QzObpAllocateObject(Type, ObjectSize, Named, AccessMode);
    if (!Body)
    {
        ExFreePoolWithTag(CreateInfo, QZ_OB_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Header = OBJECT_TO_OBJECT_HEADER(Body);
    Header->Flags |= OB_FLAG_CREATE_INFO;
    Header->ObjectCreateInfo = CreateInfo;
    if (CreateInfo->Attributes & OBJ_PERMANENT)
        Header->Flags |= OB_FLAG_PERMANENT;
    if (CreateInfo->Attributes & OBJ_EXCLUSIVE)
        Header->Flags |= OB_FLAG_EXCLUSIVE;

    /* Capture the full name; insertion trims it to the last component */
    if (Named)
    {
        POBJECT_HEADER_NAME_INFO NameInfo = OBJECT_HEADER_TO_NAME_INFO(Header);
        NameInfo->Name.Buffer = ExAllocatePoolWithTag(NonPagedPool, Name->Length, QZ_OB_NAME_TAG);
        if (!NameInfo->Name.Buffer)
        {
            ObfDereferenceObject(Body);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlCopyMemory(NameInfo->Name.Buffer, Name->Buffer, Name->Length);
        NameInfo->Name.Length = NameInfo->Name.MaximumLength = Name->Length;
    }

    InterlockedIncrement((PLONG)&Type->TotalNumberOfObjects);
    if (Type->TotalNumberOfObjects > Type->HighWaterNumberOfObjects)
        Type->HighWaterNumberOfObjects = Type->TotalNumberOfObjects;

    *Object = Body;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
ObInsertObject(IN PVOID Object,
               IN PACCESS_STATE PassedAccessState OPTIONAL,
               IN ACCESS_MASK DesiredAccess,
               IN ULONG ObjectPointerBias,
               OUT PVOID *NewObject OPTIONAL,
               OUT PHANDLE Handle OPTIONAL)
{
    POBJECT_HEADER Header = OBJECT_TO_OBJECT_HEADER(Object);
    POBJECT_HEADER_NAME_INFO NameInfo = OBJECT_HEADER_TO_NAME_INFO(Header);
    POBJECT_CREATE_INFORMATION CreateInfo;
    NTSTATUS Status = STATUS_SUCCESS, InsertStatus = STATUS_SUCCESS;
    PVOID Result = Object, Found;
    ULONG Attributes;
    HANDLE RootDirectory;

    UNREFERENCED_PARAMETER(PassedAccessState);

    if (!(Header->Flags & OB_FLAG_CREATE_INFO))
        return STATUS_INVALID_PARAMETER;

    CreateInfo = Header->ObjectCreateInfo;
    Attributes = CreateInfo->Attributes;
    RootDirectory = CreateInfo->RootDirectory;
    Header->Flags &= ~OB_FLAG_CREATE_INFO;
    Header->ObjectCreateInfo = NULL;
    ExFreePoolWithTag(CreateInfo, QZ_OB_TAG);

    if (NameInfo && NameInfo->Name.Length)
    {
        /* On success the directory insertion replaces the captured full
           name with a copy of its last component (and frees the capture) */
        UNICODE_STRING FullName = NameInfo->Name;

        QzObAcquireNamespace();
        InsertStatus = QzObpLookupObjectName(RootDirectory, &FullName, Attributes,
                                             Header->Type, KernelMode, NULL, Object, &Found);
        QzObReleaseNamespace();

        if (!NT_SUCCESS(InsertStatus))
        {
            ExFreePoolWithTag(NameInfo->Name.Buffer, QZ_OB_NAME_TAG);
            NameInfo->Name.Buffer = NULL;
            NameInfo->Name.Length = NameInfo->Name.MaximumLength = 0;
        }

        if (InsertStatus == STATUS_OBJECT_NAME_COLLISION)
        {
            if ((Attributes & OBJ_OPENIF) &&
                OBJECT_TO_OBJECT_HEADER(Found)->Type == Header->Type)
            {
                /* Open the existing object instead */
                ObfDereferenceObject(Object);
                Result = Found;
                Status = STATUS_OBJECT_NAME_EXISTS;
            }
            else
            {
                ObfDereferenceObject(Found);
                ObfDereferenceObject(Object);
                return (Attributes & OBJ_OPENIF) ? STATUS_OBJECT_TYPE_MISMATCH
                                                 : STATUS_OBJECT_NAME_COLLISION;
            }
        }
        else if (!NT_SUCCESS(InsertStatus))
        {
            ObfDereferenceObject(Object);
            return InsertStatus;
        }
    }

    /* Extra references for the caller */
    if (ObjectPointerBias)
        InterlockedExchangeAdd((PLONG)&OBJECT_TO_OBJECT_HEADER(Result)->PointerCount,
                               (LONG)ObjectPointerBias);
    if (NewObject)
        *NewObject = Result;

    if (Handle)
    {
        /* The handle takes over the creation (or lookup) reference */
        NTSTATUS HandleStatus = QzObpCreateHandle(
            Result, QzObpMapDesiredAccess(OBJECT_TO_OBJECT_HEADER(Result)->Type, DesiredAccess),
            Attributes & (OBJ_INHERIT | OBJ_KERNEL_HANDLE), Handle);
        if (!NT_SUCCESS(HandleStatus))
        {
            QzObpDeleteNameCheck(Result);
            ObfDereferenceObject(Result);
            return HandleStatus;
        }
    }
    return Status;
}

/* ---------------------------------------------------------------------- */
/* Permanence, names                                                       */
/* ---------------------------------------------------------------------- */

VOID
NTAPI
ObMakeTemporaryObject(IN PVOID Object)
{
    OBJECT_TO_OBJECT_HEADER(Object)->Flags &= ~OB_FLAG_PERMANENT;
    QzObpDeleteNameCheck(Object);
}

NTSTATUS
NTAPI
ObQueryNameString(IN PVOID Object,
                  OUT POBJECT_NAME_INFORMATION ObjectNameInfo,
                  IN ULONG Length,
                  OUT PULONG ReturnLength)
{
    POBJECT_HEADER Header = OBJECT_TO_OBJECT_HEADER(Object);
    POBJECT_HEADER_NAME_INFO NameInfo;
    PVOID Current;
    ULONG NameLength = 0, Needed;
    PWCHAR Cursor;

    if (Header->Type->TypeInfo.QueryNameProcedure)
    {
        return Header->Type->TypeInfo.QueryNameProcedure(
            Object, OBJECT_HEADER_TO_NAME_INFO(Header) != NULL,
            ObjectNameInfo, Length, ReturnLength, KernelMode);
    }

    QzObAcquireNamespace();

    /* Length of "\a\b\c" (the root alone is "\") */
    for (Current = Object; ; )
    {
        NameInfo = OBJECT_HEADER_TO_NAME_INFO(OBJECT_TO_OBJECT_HEADER(Current));
        if (Current == QzObRootDirectory)
        {
            if (NameLength == 0)
                NameLength = sizeof(WCHAR);
            break;
        }
        if (!NameInfo || !NameInfo->Directory)
        {
            NameLength = (Current == Object) ? 0 : NameLength;
            break;
        }
        NameLength += sizeof(WCHAR) + NameInfo->Name.Length;
        Current = NameInfo->Directory;
    }

    Needed = sizeof(OBJECT_NAME_INFORMATION) + NameLength + sizeof(WCHAR);
    *ReturnLength = Needed;
    if (Length < Needed)
    {
        QzObReleaseNamespace();
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    ObjectNameInfo->Name.Buffer = (PWCHAR)(ObjectNameInfo + 1);
    ObjectNameInfo->Name.Length = (USHORT)NameLength;
    ObjectNameInfo->Name.MaximumLength = (USHORT)(NameLength + sizeof(WCHAR));
    Cursor = (PWCHAR)((PUCHAR)ObjectNameInfo->Name.Buffer + NameLength);
    *Cursor = UNICODE_NULL;

    if (NameLength == sizeof(WCHAR) && Object == QzObRootDirectory)
    {
        ObjectNameInfo->Name.Buffer[0] = OBJ_NAME_PATH_SEPARATOR;
    }
    else
    {
        /* Fill from the end: component, separator, ... */
        for (Current = Object; Current != QzObRootDirectory && NameLength; )
        {
            NameInfo = OBJECT_HEADER_TO_NAME_INFO(OBJECT_TO_OBJECT_HEADER(Current));
            if (!NameInfo || !NameInfo->Directory)
                break;
            Cursor -= NameInfo->Name.Length / sizeof(WCHAR);
            RtlCopyMemory(Cursor, NameInfo->Name.Buffer, NameInfo->Name.Length);
            *--Cursor = OBJ_NAME_PATH_SEPARATOR;
            Current = NameInfo->Directory;
        }
    }

    QzObReleaseNamespace();
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Initialization                                                          */
/* ---------------------------------------------------------------------- */

VOID
QzObCoreInit(VOID)
{
    InitializeListHead(&QzObTypeList);
    KeInitializeEvent(&QzObDefaultObject, NotificationEvent, TRUE);
    KeInitializeEvent(&QzObReaperEvent, SynchronizationEvent, FALSE);
}

VOID
QzObInitPhase1(VOID)
{
    if (QzCreateThread("obreaper", QzObReaperThread, NULL))
        QzObReaperRunning = TRUE;
}
