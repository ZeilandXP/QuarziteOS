/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     I/O manager and PnP manager private definitions
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#pragma once

#include "qz.h"
#include <ndk/iotypes.h>
#include <ndk/iofuncs.h>

/* Export drivers' initialization entry point */
typedef NTSTATUS (NTAPI *QZ_DLL_INITIALIZE)(PUNICODE_STRING RegistryPath);

int __cdecl _snwprintf(wchar_t *Buffer, size_t Count, const wchar_t *Format, ...);
int __cdecl _vsnwprintf(wchar_t *Buffer, size_t Count, const wchar_t *Format, va_list Args);

#define QZ_IO_TAG       'oIzQ'
#define QZ_IRP_TAG      'prIQ'
#define QZ_PNP_TAG      'pnPQ'

/* ---------------------------------------------------------------------- */
/* Driver objects (io/driver.c)                                            */
/* ---------------------------------------------------------------------- */

/* Follows the DRIVER_OBJECT in the same allocation; starts with the
   public DRIVER_EXTENSION layout drivers dereference */
typedef struct _QZ_DRIVER_EXTENSION
{
    EXTENDED_DRIVER_EXTENSION Public;
    LIST_ENTRY Link;                  /* QzIoDriverList */
    LIST_ENTRY ReinitList;            /* QZ_REINIT entries */
    LIST_ENTRY BootReinitList;
    PVOID ImageBase;                  /* NULL for IoCreateDriver drivers */
    BOOLEAN EntryCalled;
    NTSTATUS EntryStatus;
    WCHAR NameBuffer[64];             /* "\Driver\xxx" */
    WCHAR ServiceBuffer[32];
} QZ_DRIVER_EXTENSION, *PQZ_DRIVER_EXTENSION;

#define QzDriverExtension(Driver) ((PQZ_DRIVER_EXTENSION)(Driver)->DriverExtension)

extern LIST_ENTRY QzIoDriverList;
VOID QzIoInit(VOID);
PDRIVER_OBJECT QzIoFindDriver(PCUNICODE_STRING ServiceName);
NTSTATUS QzIoLoadDriver(PCUNICODE_STRING ServiceName, PDRIVER_OBJECT *DriverObject);
NTSTATUS QzIoUnloadDriver(PCUNICODE_STRING ServiceName);
BOOLEAN QzIoIsBootService(PCUNICODE_STRING ServiceName);
NTSTATUS QzIoInitializeImports(PVOID ImageBase, ULONG Depth);
VOID QzIoForgetDll(PVOID ImageBase);

/* mm/sysload.c: kernel images loaded from disk */
VOID QzMmInitSystemImages(VOID);
NTSTATUS QzMmLoadSystemImage(PCUNICODE_STRING Path, PVOID *Entry);
VOID QzMmUnloadSystemImage(PVOID Entry);
BOOLEAN QzMmIsDiskImage(PVOID Entry);
ULONG QzMmSystemImagePages(VOID);
PVOID QzMmFindModuleExport(PVOID Entry, PCSTR Name);
VOID QzIoDumpDrivers(VOID);
NTSTATUS NTAPI QzIoInvalidDeviceRequest(PDEVICE_OBJECT DeviceObject, PIRP Irp);
VOID QzIoCallReinitRoutines(BOOLEAN Boot);
BOOLEAN QzIsExportImplemented(PCSTR Name);
ULONG QzIoCheckImports(PVOID ImageBase, PCSTR *FirstMissing, ULONG Depth);
VOID QzIoDumpBootDrivers(VOID);

/* ---------------------------------------------------------------------- */
/* Device objects (io/device.c)                                            */
/* ---------------------------------------------------------------------- */

#define QzDeviceExtension(Device) \
    ((PEXTENDED_DEVOBJ_EXTENSION)(Device)->DeviceObjectExtension)

ULONG QzIoDeviceCount(VOID);
VOID QzIoDumpDevices(VOID);
PDEVICE_OBJECT QzIoGetAttachedDevice(PDEVICE_OBJECT DeviceObject);
PDEVICE_OBJECT QzIoGetLowestDevice(PDEVICE_OBJECT DeviceObject);
VOID QzIoCallShutdownNotifications(VOID);

/* ---------------------------------------------------------------------- */
/* IRPs (io/irp.c)                                                         */
/* ---------------------------------------------------------------------- */

ULONG QzIoIrpsAllocated(VOID);
ULONG QzIoIrpsOutstanding(VOID);
BOOLEAN QzIoAttachBuffer(PIRP Irp, PDEVICE_OBJECT Device, PVOID Buffer, ULONG Length,
                         BOOLEAN Read);
NTSTATUS QzIoSynchronousCall(PDEVICE_OBJECT DeviceObject, PIRP Irp, PKEVENT Event,
                             PIO_STATUS_BLOCK IoStatus);

/* ---------------------------------------------------------------------- */
/* Files (io/file.c)                                                       */
/* ---------------------------------------------------------------------- */

typedef struct _QZ_OPEN_PACKET
{
    CSHORT Type;                      /* IO_TYPE_OPEN_PACKET */
    CSHORT Size;
    PFILE_OBJECT FileObject;
    NTSTATUS FinalStatus;
    ULONG_PTR Information;
    BOOLEAN ParseCalled;
    PLARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG ShareAccess;
    ULONG Disposition;
    ULONG CreateOptions;
    PVOID EaBuffer;
    ULONG EaLength;
    ACCESS_MASK DesiredAccess;
    ULONG Options;
    CREATE_FILE_TYPE CreateFileType;
    PVOID ExtraCreateParameters;
    PDEVICE_OBJECT DeviceObjectHint;
    PFILE_OBJECT RelatedFileObject;
} QZ_OPEN_PACKET, *PQZ_OPEN_PACKET;

NTSTATUS NTAPI QzIoParseDevice(PVOID ParseObject, PVOID ObjectType, PACCESS_STATE AccessState,
                               KPROCESSOR_MODE AccessMode, ULONG Attributes,
                               PUNICODE_STRING CompleteName, PUNICODE_STRING RemainingName,
                               PVOID Context, PSECURITY_QUALITY_OF_SERVICE SecurityQos,
                               PVOID *Object);
VOID NTAPI QzIoCloseFile(PEPROCESS Process, PVOID Object, ACCESS_MASK GrantedAccess,
                         ULONG ProcessHandleCount, ULONG SystemHandleCount);
VOID NTAPI QzIoDeleteFile(PVOID Object);
NTSTATUS NTAPI QzIoParseFile(PVOID ParseObject, PVOID ObjectType, PACCESS_STATE AccessState,
                             KPROCESSOR_MODE AccessMode, ULONG Attributes,
                             PUNICODE_STRING CompleteName, PUNICODE_STRING RemainingName,
                             PVOID Context, PSECURITY_QUALITY_OF_SERVICE SecurityQos,
                             PVOID *Object);
VOID NTAPI QzIoDeleteDevice(PVOID Object);
VOID NTAPI QzIoDeleteDriver(PVOID Object);
ULONG QzIoFileSystemCount(VOID);

/* ---------------------------------------------------------------------- */
/* PnP manager (io/pnp.c) and the read-only registry (cm/registry.c)       */
/* ---------------------------------------------------------------------- */

VOID QzPnpInit(VOID);
NTSTATUS QzPnpStart(VOID);
BOOLEAN QzPnpIsStarted(VOID);
VOID QzPnpDumpTree(BOOLEAN Verbose);
ULONG QzPnpDeviceCount(PULONG Started);
extern POBJECT_TYPE CmKeyObjectType;
VOID QzCmInit(PLOADER_PARAMETER_BLOCK LoaderBlock);
BOOLEAN QzCmHiveReady(VOID);
ULONG QzCmNotificationsFired(VOID);
NTSTATUS QzCmReadValue(PCWSTR KeyPath, PCWSTR ValueName, PVOID Buffer, ULONG Length,
                       PULONG Type, PULONG DataLength);
VOID QzCmDumpKey(PCWSTR Path);

/* ---------------------------------------------------------------------- */
/* Worker threads (ex/worker.c)                                            */
/* ---------------------------------------------------------------------- */

VOID QzExWorkerInit(VOID);
VOID QzPsInit(VOID);

/* ---------------------------------------------------------------------- */
/* Self-test (io/iotest.c)                                                 */
/* ---------------------------------------------------------------------- */

VOID QzRunIoTests(VOID);
VOID QzRunRegistryTests(VOID);   /* cm/regtest.c */
