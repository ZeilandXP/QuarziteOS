/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Runtime library: memory/string primitives, counted strings,
 *              export lookup
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 */

#include "qz.h"
#include <ntimage.h>

#undef RtlFillMemory
#undef RtlMoveMemory
#undef RtlZeroMemory
#undef RtlCopyMemory

/*
 * The primitives use string instructions so the compiler can never turn
 * them back into calls to themselves.
 */
void * __cdecl
memcpy(void *Dest, const void *Src, size_t Count)
{
    void *d = Dest;
    __asm__ __volatile__("rep movsb"
                         : "+D"(d), "+S"(Src), "+c"(Count) : : "memory");
    return Dest;
}

void * __cdecl
memmove(void *Dest, const void *Src, size_t Count)
{
    if ((ULONG_PTR)Dest <= (ULONG_PTR)Src ||
        (ULONG_PTR)Dest >= (ULONG_PTR)Src + Count)
    {
        return memcpy(Dest, Src, Count);
    }
    else
    {
        /* Overlapping, copy backwards */
        void *d = (char *)Dest + Count - 1;
        const void *s = (const char *)Src + Count - 1;
        __asm__ __volatile__("std; rep movsb; cld"
                             : "+D"(d), "+S"(s), "+c"(Count) : : "memory");
        return Dest;
    }
}

void * __cdecl
memset(void *Dest, int Value, size_t Count)
{
    void *d = Dest;
    __asm__ __volatile__("rep stosb"
                         : "+D"(d), "+c"(Count) : "a"(Value) : "memory");
    return Dest;
}

int __cdecl
memcmp(const void *A, const void *B, size_t Count)
{
    const UCHAR *a = A, *b = B;
    for (; Count; Count--, a++, b++)
    {
        if (*a != *b)
            return *a < *b ? -1 : 1;
    }
    return 0;
}

size_t __cdecl
strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int __cdecl
strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (UCHAR)*a - (UCHAR)*b;
}

int __cdecl
strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++)
    {
        if (*a != *b)
            return (UCHAR)*a - (UCHAR)*b;
        if (!*a)
            break;
    }
    return 0;
}

char * __cdecl
strncpy(char *Dest, const char *Src, size_t n)
{
    char *d = Dest;
    for (; n && *Src; n--)
        *d++ = *Src++;
    for (; n; n--)
        *d++ = 0;
    return Dest;
}

size_t __cdecl
wcslen(const wchar_t *s)
{
    const wchar_t *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

VOID
NTAPI
RtlFillMemory(PVOID Destination, SIZE_T Length, UCHAR Fill)
{
    memset(Destination, Fill, Length);
}

VOID
NTAPI
RtlZeroMemory(PVOID Destination, SIZE_T Length)
{
    memset(Destination, 0, Length);
}

VOID
NTAPI
RtlMoveMemory(PVOID Destination, CONST VOID *Source, SIZE_T Length)
{
    memmove(Destination, Source, Length);
}

SIZE_T
NTAPI
RtlCompareMemory(IN const VOID *Source1,
                 IN const VOID *Source2,
                 IN SIZE_T Length)
{
    const UCHAR *a = Source1, *b = Source2;
    SIZE_T i;
    for (i = 0; i < Length && a[i] == b[i]; i++);
    return i;
}

VOID
NTAPI
RtlInitAnsiString(IN OUT PANSI_STRING DestinationString,
                  IN PCSZ SourceString)
{
    SIZE_T Size;

    if (SourceString)
    {
        Size = strlen(SourceString);
        if (Size > (MAXUSHORT - sizeof(CHAR)))
            Size = MAXUSHORT - sizeof(CHAR);
        DestinationString->Length = (USHORT)Size;
        DestinationString->MaximumLength = (USHORT)Size + sizeof(CHAR);
    }
    else
    {
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
    }
    DestinationString->Buffer = (PCHAR)SourceString;
}

VOID
NTAPI
RtlInitUnicodeString(IN OUT PUNICODE_STRING DestinationString,
                     IN PCWSTR SourceString)
{
    SIZE_T Size;
    const SIZE_T MaxSize = (MAXUSHORT & ~1) - sizeof(UNICODE_NULL);

    if (SourceString)
    {
        Size = wcslen(SourceString) * sizeof(WCHAR);
        if (Size > MaxSize)
            Size = MaxSize;
        DestinationString->Length = (USHORT)Size;
        DestinationString->MaximumLength = (USHORT)Size + sizeof(UNICODE_NULL);
    }
    else
    {
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
    }
    DestinationString->Buffer = (PWCHAR)SourceString;
}

/*
 * Look a name up in the kernel image's own export directory. The kernel is
 * always the first entry of the loader's module list.
 */
typedef struct _QZ_LDR_ENTRY
{
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
} QZ_LDR_ENTRY, *PQZ_LDR_ENTRY;

PVOID
QzFindKernelExport(PCSTR Name)
{
    PQZ_LDR_ENTRY Kernel;
    PUCHAR Base;
    PIMAGE_NT_HEADERS Nt;
    PIMAGE_EXPORT_DIRECTORY Exports;
    PULONG Names, Functions;
    PUSHORT Ordinals;
    ULONG i;

    Kernel = CONTAINING_RECORD(QzLoaderBlock->LoadOrderListHead.Flink,
                               QZ_LDR_ENTRY, InLoadOrderLinks);
    Base = Kernel->DllBase;
    Nt = (PIMAGE_NT_HEADERS)(Base + ((PIMAGE_DOS_HEADER)Base)->e_lfanew);
    if (!Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size)
        return NULL;

    Exports = (PIMAGE_EXPORT_DIRECTORY)(Base +
        Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    Names = (PULONG)(Base + Exports->AddressOfNames);
    Ordinals = (PUSHORT)(Base + Exports->AddressOfNameOrdinals);
    Functions = (PULONG)(Base + Exports->AddressOfFunctions);

    for (i = 0; i < Exports->NumberOfNames; i++)
    {
        if (!strcmp((PCSTR)(Base + Names[i]), Name))
            return Base + Functions[Ordinals[i]];
    }
    return NULL;
}
