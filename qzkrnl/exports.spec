#
# Exports actually implemented by the QuarziteOS kernel (qzkrnl).
#
# Same syntax as ntoskrnl/ntoskrnl.spec. Every other ntoskrnl export is
# generated as a stub into qzkrnl.spec by tools/genspec.py -- re-run it after
# editing this file.
#

# Debug output / crash
@ varargs DbgPrint(str)
@ varargs DbgPrintEx(long long str)
@ stdcall KeBugCheck(long)
@ stdcall KeBugCheckEx(long ptr ptr ptr ptr)
@ stdcall RtlRaiseException(ptr)
@ stdcall ExRaiseException(ptr) RtlRaiseException
@ stdcall RtlAssert(str str long str)

# Versioning (reports Windows 7 SP1: NT 6.1.7601)
@ extern NtBuildNumber
@ stdcall PsGetVersion(ptr ptr ptr ptr)
@ stdcall RtlGetVersion(ptr)

# Time
@ extern -arch=i386,arm KeTickCount
@ stdcall -arch=i386,arm KeQueryTickCount(ptr)
@ stdcall KeQueryTimeIncrement()
@ stdcall KeDelayExecutionThread(long long ptr)

# Memory
@ stdcall ExAllocatePool(long long)
@ stdcall ExAllocatePoolWithTag(long long long)
@ stdcall ExFreePool(ptr)
@ stdcall ExFreePoolWithTag(ptr long)
@ stdcall MmMapIoSpace(long long long long)
@ stdcall MmUnmapIoSpace(ptr long)
@ stdcall MmGetPhysicalAddress(ptr)

# Runtime library
@ stdcall RtlInitAnsiString(ptr str)
@ stdcall RtlInitUnicodeString(ptr wstr)
@ stdcall RtlCompareMemory(ptr ptr long)
@ stdcall RtlFillMemory(ptr long long)
@ stdcall RtlMoveMemory(ptr ptr long)
@ stdcall RtlZeroMemory(ptr long)
@ cdecl memcpy()
@ cdecl memmove()
@ cdecl memset()
@ cdecl strcmp()
@ cdecl strlen()
@ cdecl strncmp()
@ cdecl strncpy()
@ cdecl wcslen()
@ cdecl _vsnprintf()
@ cdecl _snprintf()
@ varargs sprintf(ptr str)

# IRQL / DPCs (KeRaiseIrql & co. are HAL exports on x86)
@ stdcall KeInitializeDpc(ptr ptr ptr)
@ stdcall KeInitializeThreadedDpc(ptr ptr ptr)
@ stdcall KeInsertQueueDpc(ptr ptr ptr)
@ stdcall KeRemoveQueueDpc(ptr)
@ stdcall KeSetImportanceDpc(ptr long)
@ stdcall KeSetTargetProcessorDpc(ptr long)
@ stdcall -arch=i386,arm KeIsExecutingDpc()
@ stdcall KeFlushQueuedDpcs()

# Spinlocks
@ stdcall -arch=i386,arm KeInitializeSpinLock(ptr) QzKeInitializeSpinLock
@ stdcall KeAcquireSpinLockAtDpcLevel(ptr) QzKeAcquireSpinLockAtDpcLevel
@ stdcall KeReleaseSpinLockFromDpcLevel(ptr) QzKeReleaseSpinLockFromDpcLevel
@ fastcall -arch=i386,arm KefAcquireSpinLockAtDpcLevel(ptr)
@ fastcall -arch=i386,arm KefReleaseSpinLockFromDpcLevel(ptr)
@ fastcall KeTryToAcquireSpinLockAtDpcLevel(ptr)
@ fastcall KeTestSpinLock(ptr)
@ fastcall KeAcquireInStackQueuedSpinLockAtDpcLevel(ptr ptr)
@ fastcall KeReleaseInStackQueuedSpinLockFromDpcLevel(ptr)

# Dispatcher objects and waits
@ stdcall KeInitializeEvent(ptr long long)
@ stdcall KeSetEvent(ptr long long)
@ stdcall KeResetEvent(ptr)
@ stdcall KeClearEvent(ptr)
@ stdcall KePulseEvent(ptr long long)
@ stdcall KeReadStateEvent(ptr)
@ stdcall KeInitializeMutant(ptr long)
@ stdcall KeInitializeMutex(ptr long)
@ stdcall KeReleaseMutant(ptr long long long)
@ stdcall KeReleaseMutex(ptr long)
@ stdcall KeReadStateMutant(ptr)
@ stdcall KeReadStateMutex(ptr) KeReadStateMutant
@ stdcall KeInitializeSemaphore(ptr long long)
@ stdcall KeReleaseSemaphore(ptr long long long)
@ stdcall KeReadStateSemaphore(ptr)
@ stdcall KeWaitForSingleObject(ptr long long long ptr)
@ stdcall KeWaitForMutexObject(ptr long long long ptr) KeWaitForSingleObject
@ stdcall KeWaitForMultipleObjects(long ptr long long long long ptr ptr)

# Timers and time
@ stdcall KeInitializeTimer(ptr)
@ stdcall KeInitializeTimerEx(ptr long)
@ stdcall KeSetTimer(ptr long long ptr)
@ stdcall KeSetTimerEx(ptr long long long ptr)
@ stdcall KeCancelTimer(ptr)
@ stdcall KeReadStateTimer(ptr)
@ stdcall -arch=i386,arm KeQuerySystemTime(ptr)
@ stdcall -arch=i386,arm KeQueryInterruptTime()
@ stdcall RtlTimeToTimeFields(ptr ptr)
@ stdcall RtlTimeFieldsToTime(ptr ptr)

# Threads and processors
@ stdcall KeGetCurrentThread()
@ stdcall PsGetCurrentThread() KeGetCurrentThread
@ stdcall KeGetPreviousMode()
@ stdcall ExGetPreviousMode()
@ stdcall KeEnterCriticalRegion() QzKeEnterCriticalRegion
@ stdcall KeLeaveCriticalRegion() QzKeLeaveCriticalRegion
@ stdcall KeAreApcsDisabled()
@ stdcall KeQueryActiveProcessors()
@ extern KeNumberProcessors
@ stdcall -version=0x600+ KeQueryActiveProcessorCount(ptr) QzKeQueryActiveProcessorCount
@ stdcall -version=0x601+ KeQueryActiveProcessorCountEx(long) QzKeQueryProcessorCountEx
@ stdcall -version=0x601+ KeQueryActiveGroupCount() QzKeQueryGroupCount
@ stdcall -version=0x601+ KeQueryMaximumGroupCount() QzKeQueryGroupCount
@ stdcall -version=0x600+ KeQueryMaximumProcessorCount() QzKeQueryMaximumProcessorCount
@ stdcall -version=0x601+ KeQueryMaximumProcessorCountEx(long) QzKeQueryProcessorCountEx
@ stdcall -version=0x601+ KeGetCurrentProcessorNumberEx(ptr) QzKeGetCurrentProcessorNumberEx

# HAL interface (stage B2): clock, interrupt exit, software interrupts, tables
@ fastcall KeUpdateSystemTime(ptr long long)
@ stdcall KeSetTimeIncrement(long long)
@ stdcall -arch=i386 Kei386EoiHelper()
@ fastcall -arch=i386 KiEoiHelper(ptr)
@ stdcall -arch=i386,arm KiDeliverApc(long ptr ptr)
@ stdcall -arch=i386 KiDispatchInterrupt()
@ cdecl -arch=i386,arm KiUnexpectedInterrupt()
@ stdcall -arch=i386,arm KeProfileInterrupt(ptr)
@ extern HalDispatchTable
@ extern HalPrivateDispatchTable
@ extern KdDebuggerEnabled
@ extern KdDebuggerNotPresent
@ extern KiBugCheckData
@ extern KeServiceDescriptorTable
@ extern -arch=i386,arm KiEnableTimerWatchdog

# Interrupt objects
@ stdcall KeInitializeInterrupt(ptr ptr ptr ptr long long long long long long long)
@ stdcall KeConnectInterrupt(ptr)
@ stdcall KeDisconnectInterrupt(ptr)
@ stdcall KeSynchronizeExecution(ptr ptr ptr)

# MDLs
@ stdcall IoAllocateMdl(ptr long long long ptr)
@ stdcall IoFreeMdl(ptr)
@ stdcall MmBuildMdlForNonPagedPool(ptr)
@ stdcall MmMapLockedPages(ptr long)
@ stdcall MmMapLockedPagesSpecifyCache(ptr long long ptr long long)
@ stdcall MmUnmapLockedPages(ptr ptr)

# RTL bitmaps, contiguous memory (needed by HAL phase 1)
@ stdcall RtlInitializeBitMap(ptr ptr long)
@ stdcall RtlClearAllBits(ptr)
@ stdcall RtlSetAllBits(ptr)
@ stdcall RtlClearBit(ptr long)
@ stdcall RtlSetBit(ptr long)
@ stdcall RtlTestBit(ptr long)
@ stdcall RtlClearBits(ptr long long)
@ stdcall RtlSetBits(ptr long long)
@ stdcall RtlFindClearBits(ptr long long)
@ stdcall RtlFindSetBits(ptr long long)
@ stdcall RtlFindClearBitsAndSet(ptr long long)
@ stdcall RtlFindSetBitsAndClear(ptr long long)
@ stdcall RtlNumberOfClearBits(ptr)
@ stdcall RtlNumberOfSetBits(ptr)
@ stdcall RtlFindLeastSignificantBit(long long)
@ stdcall RtlFindMostSignificantBit(long long)
@ stdcall MmAllocateContiguousMemorySpecifyCache(long long long long long long long long)
@ stdcall MmFreeContiguousMemorySpecifyCache(ptr long long)

# Object manager (stage B3)
@ fastcall ObfDereferenceObject(ptr)
@ stdcall ObDereferenceObject(ptr)
@ fastcall ObfReferenceObject(ptr)
@ stdcall ObReferenceObjectByPointer(ptr long ptr long)
@ stdcall ObCreateObjectType(ptr ptr ptr ptr)
@ stdcall ObCreateObject(long ptr ptr long ptr long long long ptr)
@ stdcall ObInsertObject(ptr ptr long long ptr ptr)
@ stdcall ObMakeTemporaryObject(ptr)
@ stdcall ObQueryNameString(ptr ptr long ptr)
@ stdcall ObReferenceObjectByName(ptr long ptr long ptr long ptr ptr)
@ stdcall ObOpenObjectByName(ptr ptr long ptr long ptr ptr)
@ stdcall ObReferenceObjectByHandle(ptr long ptr long ptr ptr)
@ stdcall ObOpenObjectByPointer(ptr long ptr long ptr long ptr)
@ stdcall ObCloseHandle(ptr long)
@ stdcall NtClose(ptr)
@ stdcall ZwClose(ptr)
@ stdcall NtDuplicateObject(ptr ptr ptr ptr long long long)
@ stdcall ZwDuplicateObject(ptr ptr ptr ptr long long long)
@ stdcall ZwMakeTemporaryObject(ptr)
@ stdcall NtMakePermanentObject(ptr)
@ stdcall ZwQueryObject(ptr long ptr long ptr)
@ stdcall ZwCreateDirectoryObject(ptr long ptr)
@ stdcall ZwOpenDirectoryObject(ptr long ptr)
@ stdcall ZwQueryDirectoryObject(ptr ptr long long long ptr ptr)
@ stdcall ZwCreateSymbolicLinkObject(ptr long ptr ptr)
@ stdcall ZwOpenSymbolicLinkObject(ptr long ptr)
@ stdcall ZwQuerySymbolicLinkObject(ptr ptr ptr)

# Object types (I/O types are placeholders until stage B4)
@ extern IoAdapterObjectType
@ extern IoDeviceObjectType
@ extern IoDriverObjectType
@ extern IoFileObjectType
@ extern ExEventObjectType
@ extern ExSemaphoreObjectType

# Executive objects and handle waits
@ stdcall NtCreateEvent(ptr long ptr long long)
@ stdcall ZwCreateEvent(ptr long ptr long long)
@ stdcall ZwOpenEvent(ptr long ptr)
@ stdcall NtSetEvent(ptr ptr)
@ stdcall ZwSetEvent(ptr ptr)
@ stdcall ZwResetEvent(ptr ptr)
@ stdcall ZwClearEvent(ptr)
@ stdcall ZwPulseEvent(ptr ptr)
@ stdcall ZwCreateTimer(ptr long ptr long)
@ stdcall ZwOpenTimer(ptr long ptr)
@ stdcall ZwSetTimer(ptr ptr ptr ptr long long ptr)
@ stdcall ZwCancelTimer(ptr ptr)
@ stdcall NtWaitForSingleObject(ptr long ptr)
@ stdcall ZwWaitForSingleObject(ptr long ptr)
@ stdcall ZwWaitForMultipleObjects(long ptr long long ptr)

# Security stubs (everything runs as the system until tokens, B6)
@ extern SeExports
@ stdcall SeCaptureSubjectContext(ptr)
@ stdcall SeReleaseSubjectContext(ptr)
@ stdcall SeLockSubjectContext(ptr)
@ stdcall SeUnlockSubjectContext(ptr)
@ stdcall SeSinglePrivilegeCheck(long long long)
@ stdcall SeAccessCheck(ptr ptr ptr long long ptr ptr long ptr ptr)
@ stdcall RtlMapGenericMask(ptr ptr)
@ stdcall ObGetObjectSecurity(ptr ptr ptr)
@ stdcall ObReleaseObjectSecurity(ptr long)

# Counted strings
@ stdcall RtlUpcaseUnicodeChar(long)
@ stdcall RtlCompareUnicodeString(ptr ptr long)
@ stdcall RtlEqualUnicodeString(ptr ptr long)
@ stdcall RtlPrefixUnicodeString(ptr ptr long)
@ stdcall RtlCopyUnicodeString(ptr ptr)
@ stdcall RtlAppendUnicodeStringToString(ptr ptr)
@ stdcall RtlAppendUnicodeToString(ptr wstr)
@ stdcall RtlCreateUnicodeString(ptr wstr)
@ stdcall RtlFreeUnicodeString(ptr)
@ stdcall RtlUpcaseUnicodeString(ptr ptr long)
@ stdcall RtlInitString(ptr str)
@ stdcall RtlEqualString(ptr ptr long)
@ stdcall RtlCopyString(ptr ptr)
@ stdcall RtlAnsiCharToUnicodeChar(ptr)
@ stdcall RtlAnsiStringToUnicodeString(ptr ptr long)
@ stdcall RtlUnicodeStringToAnsiString(ptr ptr long)
@ stdcall RtlFreeAnsiString(ptr)
@ stdcall RtlIntegerToUnicodeString(long long ptr)

# Runtime: wide formatting, PE helpers
@ cdecl _vsnwprintf()
@ cdecl _snwprintf()
@ cdecl -version=0x600+ _swprintf()
@ cdecl swprintf() _swprintf # Non-conforming swprintf
@ stdcall ExRaiseStatus(long) RtlRaiseStatus
@ stdcall MmIsAddressValid(ptr)
@ stdcall MmProbeAndLockPages(ptr long long)
@ stdcall MmUnlockPages(ptr)
@ stdcall IoBuildPartialMdl(ptr ptr ptr long)

# I/O manager: drivers (stage B4)
@ stdcall IoCreateDriver(ptr ptr)
@ stdcall IoDeleteDriver(ptr)
@ stdcall IoAllocateDriverObjectExtension(ptr ptr long ptr)
@ stdcall IoGetDriverObjectExtension(ptr ptr)
@ stdcall IoRegisterDriverReinitialization(ptr ptr ptr)
@ stdcall IoRegisterBootDriverReinitialization(ptr ptr ptr)

# I/O manager: devices
@ stdcall IoCreateDevice(ptr long ptr long long long ptr)
@ stdcall IoDeleteDevice(ptr)
@ stdcall IoAttachDeviceToDeviceStackSafe(ptr ptr ptr)
@ stdcall IoAttachDeviceToDeviceStack(ptr ptr)
@ stdcall IoAttachDeviceByPointer(ptr ptr)
@ stdcall IoAttachDevice(ptr ptr ptr)
@ stdcall IoDetachDevice(ptr)
@ stdcall IoGetAttachedDevice(ptr)
@ stdcall IoGetAttachedDeviceReference(ptr)
@ stdcall IoGetLowerDeviceObject(ptr)
@ stdcall IoGetDeviceAttachmentBaseRef(ptr)
@ stdcall IoGetRelatedDeviceObject(ptr)
@ stdcall IoGetBaseFileSystemDeviceObject(ptr)
@ stdcall IoGetDeviceObjectPointer(ptr long ptr ptr)
@ stdcall IoCreateSymbolicLink(ptr ptr)
@ stdcall IoCreateUnprotectedSymbolicLink(ptr ptr)
@ stdcall IoDeleteSymbolicLink(ptr)
@ stdcall IoRegisterShutdownNotification(ptr)
@ stdcall IoRegisterLastChanceShutdownNotification(ptr)
@ stdcall IoUnregisterShutdownNotification(ptr)
@ stdcall IoInitializeTimer(ptr ptr ptr)
@ stdcall IoStartTimer(ptr)
@ stdcall IoStopTimer(ptr)

# I/O manager: IRPs
@ stdcall IoInitializeIrp(ptr long long)
@ stdcall IoAllocateIrp(long long)
@ stdcall IoFreeIrp(ptr)
@ stdcall IoReuseIrp(ptr long)
@ stdcall IoMakeAssociatedIrp(ptr long)
@ fastcall IofCallDriver(ptr ptr)
@ stdcall IoCallDriver(ptr ptr)
@ fastcall IofCompleteRequest(ptr long)
@ stdcall IoCompleteRequest(ptr long)
@ stdcall IoAcquireCancelSpinLock(ptr)
@ stdcall IoReleaseCancelSpinLock(long)
@ stdcall IoCancelIrp(ptr)
@ stdcall IoBuildAsynchronousFsdRequest(long ptr ptr long ptr ptr)
@ stdcall IoBuildSynchronousFsdRequest(long ptr ptr long ptr ptr ptr)
@ stdcall IoBuildDeviceIoControlRequest(long ptr ptr long ptr long long ptr ptr)
@ stdcall IoInitializeRemoveLockEx(ptr long long long long)
@ stdcall IoAcquireRemoveLockEx(ptr ptr str long long)
@ stdcall IoReleaseRemoveLockEx(ptr ptr long)
@ stdcall IoReleaseRemoveLockAndWaitEx(ptr ptr long)

# I/O manager: files, file systems, mounting
@ stdcall IoRegisterFileSystem(ptr)
@ stdcall IoUnregisterFileSystem(ptr)
@ stdcall IoRegisterFsRegistrationChange(ptr ptr)
@ stdcall IoUnregisterFsRegistrationChange(ptr ptr)
@ stdcall IoVerifyVolume(ptr long)
@ stdcall IoSetHardErrorOrVerifyDevice(ptr ptr)
@ stdcall IoCreateStreamFileObjectEx(ptr ptr ptr)
@ stdcall IoCreateStreamFileObject(ptr ptr)
@ stdcall IoCreateStreamFileObjectLite(ptr ptr)
@ stdcall IoCreateFile(ptr long ptr ptr ptr long long long long ptr long long ptr long)
@ stdcall IoCreateFileSpecifyDeviceObjectHint(ptr long ptr ptr ptr long long long long ptr long long ptr long ptr)
@ stdcall ZwCreateFile(ptr long ptr ptr ptr long long long long ptr long)
@ stdcall NtCreateFile(ptr long ptr ptr ptr long long long long ptr long)
@ stdcall ZwOpenFile(ptr long ptr ptr long long)
@ stdcall NtOpenFile(ptr long ptr ptr long long)
@ stdcall ZwReadFile(ptr ptr ptr ptr ptr ptr long ptr ptr)
@ stdcall ZwWriteFile(ptr ptr ptr ptr ptr ptr long ptr ptr)
@ stdcall NtReadFile(ptr ptr ptr ptr ptr ptr long ptr ptr)
@ stdcall NtWriteFile(ptr ptr ptr ptr ptr ptr long ptr ptr)
@ stdcall ZwDeviceIoControlFile(ptr ptr ptr ptr ptr long ptr long ptr long)
@ stdcall NtDeviceIoControlFile(ptr ptr ptr ptr ptr long ptr long ptr long)
@ stdcall ZwFsControlFile(ptr ptr ptr ptr ptr long ptr long ptr long)
@ stdcall NtFsControlFile(ptr ptr ptr ptr ptr long ptr long ptr long)
@ stdcall ZwQueryInformationFile(ptr ptr ptr long long)
@ stdcall ZwSetInformationFile(ptr ptr ptr long long)
@ stdcall ZwQueryVolumeInformationFile(ptr ptr ptr long long)
@ stdcall IoIsOperationSynchronous(ptr)
@ stdcall IoCheckShareAccess(long long ptr ptr long)
@ stdcall IoSetShareAccess(long long ptr ptr)
@ stdcall IoRemoveShareAccess(ptr ptr)
@ stdcall IoUpdateShareAccess(ptr ptr)
@ stdcall IoGetFileObjectGenericMapping()

# I/O manager: StartIo, interrupts, error log, misc
@ stdcall IoStartPacket(ptr ptr ptr ptr)
@ stdcall IoStartNextPacket(ptr long)
@ stdcall IoStartNextPacketByKey(ptr long long)
@ stdcall IoConnectInterrupt(ptr ptr ptr ptr long long long long long long long)
@ stdcall IoDisconnectInterrupt(ptr)
@ stdcall IoAllocateErrorLogEntry(ptr long)
@ stdcall IoFreeErrorLogEntry(ptr)
@ stdcall IoWriteErrorLogEntry(ptr)
@ stdcall IoGetConfigurationInformation()
@ stdcall IoIsWdmVersionAvailable(long long)
@ stdcall IoWMIRegistrationControl(ptr long)
@ stdcall IoGetCurrentProcess()
@ stdcall IoGetRequestorProcess(ptr)
@ stdcall IoGetRequestorProcessId(ptr)
@ stdcall PsGetCurrentProcessId()
@ stdcall PsGetCurrentThreadId()
@ stdcall PsGetCurrentProcess() IoGetCurrentProcess
@ stdcall IoGetStackLimits(ptr ptr)

# Worker threads and work items
@ stdcall ExQueueWorkItem(ptr long)
@ stdcall IoAllocateWorkItem(ptr)
@ stdcall IoFreeWorkItem(ptr)
@ stdcall IoQueueWorkItem(ptr ptr long ptr)

# Kernel device queues
@ stdcall KeInitializeDeviceQueue(ptr)
@ stdcall KeInsertDeviceQueue(ptr ptr)
@ stdcall KeInsertByKeyDeviceQueue(ptr ptr long)
@ stdcall KeRemoveDeviceQueue(ptr)
@ stdcall KeRemoveByKeyDeviceQueue(ptr long)
@ stdcall KeRemoveByKeyDeviceQueueIfBusy(ptr long)
@ stdcall KeRemoveEntryDeviceQueue(ptr ptr)

# Registry (read-only over the loader's SYSTEM hive until stage B5)
@ stdcall ZwOpenKey(ptr long ptr)
@ stdcall ZwCreateKey(ptr long ptr long ptr long ptr)
@ stdcall ZwQueryKey(ptr long ptr long ptr)
@ stdcall ZwEnumerateKey(ptr long long ptr long ptr)
@ stdcall ZwQueryValueKey(ptr ptr long ptr long ptr)
@ stdcall ZwEnumerateValueKey(ptr long long ptr long ptr)
@ stdcall ZwSetValueKey(ptr ptr long long ptr long)
@ stdcall ZwDeleteValueKey(ptr ptr)
@ stdcall ZwDeleteKey(ptr)
@ stdcall ZwFlushKey(ptr)
@ stdcall RtlQueryRegistryValues(long wstr ptr ptr ptr)
@ stdcall RtlWriteRegistryValue(long wstr wstr long ptr long)
@ stdcall RtlDeleteRegistryValue(long wstr wstr)
@ stdcall RtlCheckRegistryKey(long wstr)
@ stdcall RtlCreateRegistryKey(long wstr)
@ stdcall IoSetThreadHardErrorMode(long)

# PnP manager
@ stdcall IoInvalidateDeviceRelations(ptr long)
@ stdcall IoSynchronousInvalidateDeviceRelations(ptr long)
@ stdcall IoInvalidateDeviceState(ptr)
@ stdcall IoRequestDeviceEject(ptr)
@ stdcall IoReportDetectedDevice(ptr long long long ptr ptr long ptr)
@ stdcall IoGetDeviceProperty(ptr long long ptr ptr)
@ stdcall IoOpenDeviceRegistryKey(ptr long long ptr)
@ stdcall IoOpenDeviceInterfaceRegistryKey(ptr long ptr)
@ stdcall IoRegisterDeviceInterface(ptr ptr ptr ptr)
@ stdcall IoSetDeviceInterfaceState(ptr long)
@ stdcall IoGetDeviceInterfaces(ptr ptr long ptr)
@ stdcall IoRegisterPlugPlayNotification(long long ptr ptr ptr ptr ptr)
@ stdcall IoUnregisterPlugPlayNotification(ptr)

# Power (requests go straight down until a power manager exists)
@ stdcall PoCallDriver(ptr ptr)
@ stdcall PoStartNextPowerIrp(ptr)
@ stdcall PoSetPowerState(ptr long long)
@ stdcall PoRequestPowerIrp(ptr long long ptr ptr ptr)
@ stdcall PoRegisterDeviceForIdleDetection(ptr long long long)

# C runtime
@ cdecl tolower()
@ cdecl toupper()
@ cdecl towlower()
@ cdecl towupper()
@ cdecl isdigit()
@ cdecl isxdigit()
@ cdecl isspace()
@ cdecl isupper()
@ cdecl islower()
@ cdecl isprint()
@ cdecl strcpy()
@ cdecl strcat()
@ cdecl strchr()
@ cdecl strrchr()
@ cdecl strstr()
@ cdecl _strnicmp()
@ cdecl _stricmp()
@ cdecl _strupr()
@ cdecl _strlwr()
@ cdecl wcscpy()
@ cdecl wcsncpy()
@ cdecl wcscat()
@ cdecl wcsncat()
@ cdecl wcsncmp()
@ cdecl wcscmp()
@ cdecl _wcsnicmp()
@ cdecl _wcsicmp()
@ cdecl wcschr()
@ cdecl wcsrchr()
@ cdecl wcsstr()
@ cdecl _wcsupr()
@ cdecl _wcslwr()
@ cdecl atoi()

# Executive synchronization helpers
@ fastcall -arch=i386 ExfInterlockedInsertHeadList(ptr ptr ptr)
@ fastcall -arch=i386 ExfInterlockedInsertTailList(ptr ptr ptr)
@ fastcall -arch=i386 ExfInterlockedRemoveHeadList(ptr ptr)
@ fastcall -arch=i386 ExfInterlockedPushEntryList(ptr ptr ptr)
@ fastcall -arch=i386 ExfInterlockedPopEntryList(ptr ptr)
@ stdcall ExInterlockedInsertHeadList(ptr ptr ptr)
@ stdcall ExInterlockedInsertTailList(ptr ptr ptr)
@ stdcall ExInterlockedRemoveHeadList(ptr ptr)
@ stdcall ExInterlockedPushEntryList(ptr ptr ptr)
@ stdcall ExInterlockedPopEntryList(ptr ptr)
@ fastcall -arch=i386 ExfInterlockedAddUlong(ptr long ptr)
@ stdcall ExInterlockedAddUlong(ptr long ptr)
@ stdcall ExInterlockedAddLargeInteger(ptr long long ptr)
@ fastcall -arch=i386 ExInterlockedAddLargeStatistic(ptr long)
@ fastcall -arch=i386 InterlockedPushEntrySList(ptr ptr)
@ fastcall -arch=i386 InterlockedPopEntrySList(ptr)
@ fastcall -arch=i386 ExInterlockedFlushSList(ptr)
@ fastcall -arch=i386 ExInterlockedPushEntrySList(ptr ptr ptr)
@ fastcall -arch=i386 ExInterlockedPopEntrySList(ptr ptr)
@ stdcall ExInitializeNPagedLookasideList(ptr ptr ptr long long long long)
@ stdcall ExInitializePagedLookasideList(ptr ptr ptr long long long long)
@ stdcall ExDeleteNPagedLookasideList(ptr)
@ stdcall ExDeletePagedLookasideList(ptr)
@ stdcall ExAllocateFromPagedLookasideList(ptr) ExiAllocateFromPagedLookasideList
@ stdcall ExFreeToPagedLookasideList(ptr ptr) ExiFreeToPagedLookasideList
@ fastcall ExInitializeRundownProtection(ptr) ExfInitializeRundownProtection
@ fastcall ExReInitializeRundownProtection(ptr) ExfReInitializeRundownProtection
@ fastcall ExAcquireRundownProtection(ptr) ExfAcquireRundownProtection
@ fastcall ExAcquireRundownProtectionEx(ptr long) ExfAcquireRundownProtectionEx
@ fastcall ExReleaseRundownProtection(ptr) ExfReleaseRundownProtection
@ fastcall ExReleaseRundownProtectionEx(ptr long) ExfReleaseRundownProtectionEx
@ fastcall ExWaitForRundownProtectionRelease(ptr) ExfWaitForRundownProtectionRelease
@ stdcall ExSizeOfRundownProtectionCacheAware()
@ stdcall ExInitializeRundownProtectionCacheAware(ptr long)
@ stdcall ExAllocateCacheAwareRundownProtection(long long)
@ stdcall ExFreeCacheAwareRundownProtection(ptr)
@ fastcall ExAcquireRundownProtectionCacheAware(ptr) ExfAcquireRundownProtectionCacheAware
@ fastcall ExAcquireRundownProtectionCacheAwareEx(ptr long) ExfAcquireRundownProtectionCacheAwareEx
@ fastcall ExReleaseRundownProtectionCacheAware(ptr) ExfReleaseRundownProtectionCacheAware
@ fastcall ExReleaseRundownProtectionCacheAwareEx(ptr long) ExfReleaseRundownProtectionCacheAwareEx
@ fastcall ExWaitForRundownProtectionReleaseCacheAware(ptr) ExfWaitForRundownProtectionReleaseCacheAware
@ fastcall ExReInitializeRundownProtectionCacheAware(ptr) ExfReInitializeRundownProtectionCacheAware
@ stdcall KeEnterGuardedRegion() _KeEnterGuardedRegion
@ stdcall KeLeaveGuardedRegion() _KeLeaveGuardedRegion
@ stdcall KeAreAllApcsDisabled()
@ fastcall KeInitializeGuardedMutex(ptr)
@ fastcall KeAcquireGuardedMutexUnsafe(ptr)
@ fastcall KeReleaseGuardedMutexUnsafe(ptr)
@ fastcall KeAcquireGuardedMutex(ptr)
@ fastcall KeReleaseGuardedMutex(ptr)
@ fastcall KeTryToAcquireGuardedMutex(ptr)
@ stdcall KeAcquireInterruptSpinLock(ptr)
@ stdcall KeReleaseInterruptSpinLock(ptr long)
@ extern InitSafeBootMode
@ stdcall ExUuidCreate(ptr)

# RTL: generic tables, conversions, GUIDs, unwinding
@ stdcall RtlInitializeGenericTableAvl(ptr ptr ptr ptr ptr)
@ stdcall RtlInsertElementGenericTableAvl(ptr ptr long ptr)
@ stdcall RtlLookupElementGenericTableAvl(ptr ptr)
@ stdcall RtlDeleteElementGenericTableAvl(ptr ptr)
@ stdcall RtlNumberGenericTableElementsAvl(ptr)
@ stdcall RtlIsGenericTableEmptyAvl(ptr)
@ stdcall RtlEnumerateGenericTableWithoutSplayingAvl(ptr ptr)
@ stdcall RtlEnumerateGenericTableAvl(ptr long)
@ stdcall RtlGetElementGenericTableAvl(ptr long)
@ stdcall RtlMultiByteToUnicodeN(ptr long ptr str long)
@ stdcall RtlMultiByteToUnicodeSize(ptr str long)
@ stdcall RtlUnicodeToMultiByteN(ptr long ptr wstr long)
@ stdcall RtlUnicodeToMultiByteSize(ptr wstr long)
@ stdcall RtlUnicodeStringToInteger(ptr long ptr)
@ stdcall RtlStringFromGUID(ptr ptr)
@ stdcall RtlGUIDFromString(ptr ptr)
@ stdcall RtlUnwind(ptr ptr ptr ptr)
@ stdcall RtlCaptureStackBackTrace(long long ptr ptr)
@ stdcall vDbgPrintEx(long long str ptr)
@ stdcall vDbgPrintExWithPrefix(str long long str ptr)

# Storage and bus driver services
@ stdcall IoForwardAndCatchIrp(ptr ptr) IoForwardIrpSynchronously
@ stdcall IoSetCompletionRoutineEx(ptr ptr ptr ptr long long long)
@ stdcall IoCreateController(long)
@ stdcall IoDeleteController(ptr)
@ stdcall IoFreeController(ptr)
@ stdcall IoAllocateController(ptr ptr ptr ptr)
@ stdcall IoGetDmaAdapter(ptr ptr ptr)
@ stdcall -arch=i386,arm READ_REGISTER_UCHAR(ptr)
@ stdcall -arch=i386,arm READ_REGISTER_USHORT(ptr)
@ stdcall -arch=i386,arm READ_REGISTER_ULONG(ptr)
@ stdcall -arch=i386,arm WRITE_REGISTER_UCHAR(ptr long)
@ stdcall -arch=i386,arm WRITE_REGISTER_USHORT(ptr long)
@ stdcall -arch=i386,arm WRITE_REGISTER_ULONG(ptr long)
@ stdcall -arch=i386,arm READ_REGISTER_BUFFER_UCHAR(ptr ptr long)
@ stdcall -arch=i386,arm READ_REGISTER_BUFFER_USHORT(ptr ptr long)
@ stdcall -arch=i386,arm READ_REGISTER_BUFFER_ULONG(ptr ptr long)
@ stdcall -arch=i386,arm WRITE_REGISTER_BUFFER_UCHAR(ptr ptr long)
@ stdcall -arch=i386,arm WRITE_REGISTER_BUFFER_USHORT(ptr ptr long)
@ stdcall -arch=i386,arm WRITE_REGISTER_BUFFER_ULONG(ptr ptr long)
@ fastcall HalExamineMBR(ptr long long ptr)
@ stdcall IoReadDiskSignature(ptr long ptr)
@ stdcall IoReadPartitionTableEx(ptr ptr)
@ stdcall IoWritePartitionTableEx(ptr ptr)
@ fastcall IoSetPartitionInformation(ptr long long long)
@ stdcall IoSetPartitionInformationEx(ptr long ptr)
@ stdcall IoCreateDisk(ptr ptr)
@ stdcall IoGetDevicePropertyData(ptr ptr long long long ptr ptr ptr)
@ fastcall IoGetPagingIoPriority(ptr)
@ stdcall IoReportTargetDeviceChangeAsynchronous(ptr ptr ptr ptr)
@ stdcall IoSetStartIoAttributes(ptr long long)
@ stdcall IoWMIWriteEvent(ptr)
@ stdcall IoSetSystemPartition(ptr)
@ stdcall RtlCreateSystemVolumeInformationFolder(ptr)
@ stdcall ZwQueryDirectoryFile(ptr ptr ptr ptr ptr ptr long long long ptr long)

# Memory: reserved mappings, routine lookup
@ stdcall MmAllocateMappingAddress(long long)
@ stdcall MmFreeMappingAddress(ptr long)
@ stdcall MmMapLockedPagesWithReservedMapping(ptr long ptr long)
@ stdcall MmUnmapReservedMapping(ptr long ptr)
@ stdcall MmGetSystemRoutineAddress(ptr)

# Processes and threads (System only until stage B6)
@ stdcall PsCreateSystemThread(ptr long ptr ptr ptr ptr ptr)
@ stdcall PsTerminateSystemThread(long)
@ stdcall PsGetThreadHardErrorsAreDisabled(ptr)
@ stdcall PsSetThreadHardErrorsAreDisabled(ptr long)
@ extern PsThreadType
@ extern PsProcessType
@ extern PsInitialSystemProcess

# IRP forwarding
@ stdcall IoForwardIrpSynchronously(ptr ptr)

# Resource reporting
@ stdcall IoReportHalResourceUsage(ptr ptr ptr long)
@ stdcall IoReportResourceUsage(ptr ptr ptr long ptr ptr long long ptr)
@ stdcall IoReportResourceForDetection(ptr ptr long ptr ptr long ptr)

# Fast mutexes (the kernel's own)
@ fastcall ExAcquireFastMutexUnsafe(ptr)
@ fastcall ExReleaseFastMutexUnsafe(ptr)
@ fastcall -arch=i386 ExiAcquireFastMutex(ptr)
@ fastcall -arch=i386 ExiReleaseFastMutex(ptr)
@ fastcall -arch=i386 ExiTryToAcquireFastMutex(ptr)

# Executive resources, callbacks, system information, driver loading
@ stdcall ExInitializeResourceLite(ptr)
@ stdcall ExDeleteResourceLite(ptr)
@ stdcall ExReinitializeResourceLite(ptr)
@ stdcall ExAcquireResourceExclusiveLite(ptr long)
@ stdcall ExAcquireResourceSharedLite(ptr long)
@ stdcall ExAcquireSharedStarveExclusive(ptr long)
@ stdcall ExAcquireSharedWaitForExclusive(ptr long)
@ fastcall ExReleaseResourceLite(ptr)
@ stdcall ExReleaseResourceForThreadLite(ptr long)
@ stdcall ExConvertExclusiveToSharedLite(ptr)
@ stdcall ExIsResourceAcquiredExclusiveLite(ptr)
@ stdcall ExIsResourceAcquiredSharedLite(ptr)
@ stdcall ExGetExclusiveWaiterCount(ptr)
@ stdcall ExGetSharedWaiterCount(ptr)
@ stdcall ExEnterCriticalRegionAndAcquireResourceExclusive(ptr)
@ fastcall ExReleaseResourceAndLeaveCriticalRegion(ptr)
@ stdcall ExCreateCallback(ptr ptr long long)
@ stdcall ExRegisterCallback(ptr ptr ptr)
@ stdcall ExUnregisterCallback(ptr)
@ stdcall ExNotifyCallback(ptr ptr ptr)
@ stdcall ZwQuerySystemInformation(long ptr long ptr)
@ stdcall ZwLoadDriver(ptr)
@ stdcall ZwUnloadDriver(ptr)
@ stdcall KdRefreshDebuggerNotPresent()
@ stdcall DbgBreakPoint()
@ stdcall DbgBreakPointWithStatus(long)
@ stdcall MmIsDriverVerifying(ptr)
@ stdcall MmLockPagableDataSection(ptr) MmLockPageableDataSection
@ stdcall MmLockPagableSectionByHandle(ptr) MmLockPageableSectionByHandle
@ stdcall MmUnlockPagableImageSection(ptr) MmUnlockPageableImageSection
@ stdcall MmPageEntireDriver(ptr)
@ stdcall MmResetDriverPaging(ptr)
@ stdcall MmSizeOfMdl(ptr long)
@ stdcall ExAllocatePoolWithQuotaTag(long long long)
@ stdcall ExAllocatePoolWithQuota(long long)
@ stdcall RtlImageDirectoryEntryToData(ptr long long ptr)
@ stdcall RtlImageNtHeader(ptr)
@ stdcall IoSetDevicePropertyData(ptr ptr long long long long ptr)
@ stdcall PsIsThreadTerminating(ptr)
@ stdcall ZwPowerInformation(long ptr long ptr long)

# Registry notifications (B5)
@ stdcall ZwNotifyChangeKey(ptr ptr ptr ptr ptr long long ptr long long)

# Processes and address spaces (B6)
@ stdcall PsLookupProcessByProcessId(ptr ptr)
@ stdcall PsLookupThreadByThreadId(ptr ptr)
@ stdcall PsGetProcessId(ptr)
@ stdcall PsGetThreadId(ptr)
@ stdcall PsGetThreadProcessId(ptr)
@ stdcall PsGetProcessImageFileName(ptr)
@ stdcall PsGetProcessPeb(ptr)
@ stdcall PsGetCurrentProcessSessionId()
@ stdcall PsIsSystemThread(ptr)
@ stdcall PsGetThreadProcess(ptr)
@ stdcall PsGetCurrentThreadProcess()
@ stdcall PsGetCurrentThreadProcessId()
@ stdcall PsGetProcessExitStatus(ptr)
@ stdcall PsGetProcessSessionId(ptr)
@ stdcall KeStackAttachProcess(ptr ptr)
@ stdcall KeUnstackDetachProcess(ptr)
@ stdcall KeAttachProcess(ptr)
@ stdcall KeDetachProcess()
@ stdcall PsGetProcessInheritedFromUniqueProcessId(ptr)
@ stdcall PsGetThreadTeb(ptr)
@ stdcall PsGetCurrentThreadTeb()
@ stdcall PsGetProcessWin32Process(ptr)
@ stdcall PsGetThreadWin32Thread(ptr)
@ stdcall PsGetCurrentProcessWin32Process()
@ stdcall PsGetCurrentThreadWin32Thread()
@ stdcall PsGetProcessCreateTimeQuadPart(ptr)
@ stdcall PsIsProcessBeingDebugged(ptr)
@ stdcall PsGetProcessDebugPort(ptr)
@ extern MmUserProbeAddress
@ extern MmHighestUserAddress
@ extern MmSystemRangeStart

# Virtual memory services (B6)
@ stdcall ZwAllocateVirtualMemory(ptr ptr long ptr long long)
@ stdcall ZwFreeVirtualMemory(ptr ptr ptr long)

# Sections (B6)
@ stdcall ZwCreateSection(ptr long ptr ptr long long ptr)
@ stdcall ZwMapViewOfSection(ptr ptr ptr long long ptr ptr long long long)
@ stdcall ZwUnmapViewOfSection(ptr ptr)
@ stdcall ZwOpenSection(ptr long ptr)
@ extern MmSectionObjectType

# Boot driver odds and ends
@ stdcall IoCsqInitialize(ptr ptr ptr ptr ptr ptr ptr)
@ stdcall IoCsqInitializeEx(ptr ptr ptr ptr ptr ptr ptr)
@ stdcall IoCsqInsertIrp(ptr ptr ptr)
@ stdcall IoCsqInsertIrpEx(ptr ptr ptr ptr)
@ stdcall IoCsqRemoveIrp(ptr ptr)
@ stdcall IoCsqRemoveNextIrp(ptr ptr)
@ stdcall IoAllocateAdapterChannel(ptr ptr long ptr ptr)
@ stdcall IoQueryDeviceDescription(ptr ptr ptr ptr ptr ptr ptr ptr)
@ stdcall SeTokenIsAdmin(ptr)
@ stdcall ExIsProcessorFeaturePresent(long)
@ stdcall ProbeForRead(ptr long long)
@ stdcall ProbeForWrite(ptr long long)
@ stdcall KeRegisterBugCheckCallback(ptr ptr ptr long ptr)
@ stdcall KeDeregisterBugCheckCallback(ptr)
@ stdcall KeSetPriorityThread(ptr long)
@ stdcall MmAllocateContiguousMemory(long long long)
@ stdcall MmFreeContiguousMemory(ptr)
@ stdcall MmAllocateNonCachedMemory(long)
@ stdcall MmFreeNonCachedMemory(ptr long)
@ cdecl vsprintf(ptr str ptr)

# Vista interrupt connection
@ stdcall IoConnectInterruptEx(ptr)
@ stdcall IoDisconnectInterruptEx(ptr)
