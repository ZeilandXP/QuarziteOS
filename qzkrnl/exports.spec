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
