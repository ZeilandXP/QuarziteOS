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
