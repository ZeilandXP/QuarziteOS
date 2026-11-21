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
