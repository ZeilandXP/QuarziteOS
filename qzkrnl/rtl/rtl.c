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
PVOID
QzFindImageExport(PVOID ImageBase, PCSTR Name)
{
    PUCHAR Base = ImageBase;
    PIMAGE_NT_HEADERS Nt;
    PIMAGE_EXPORT_DIRECTORY Exports;
    PULONG Names, Functions;
    PUSHORT Ordinals;
    ULONG i;

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

PVOID
QzFindKernelExport(PCSTR Name)
{
    PQZ_LDR_ENTRY Kernel = CONTAINING_RECORD(QzLoaderBlock->LoadOrderListHead.Flink,
                                             QZ_LDR_ENTRY, InLoadOrderLinks);
    return QzFindImageExport(Kernel->DllBase, Name);
}

/* ---------------------------------------------------------------------- */
/* More C runtime: what drivers import from the kernel                     */
/* ---------------------------------------------------------------------- */

static int QzLower(int c)  { return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c; }
static int QzUpper(int c)  { return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c; }

int __cdecl tolower(int c) { return QzLower(c); }
int __cdecl toupper(int c) { return QzUpper(c); }
wchar_t __cdecl towlower(wchar_t c) { return (wchar_t)((c >= L'A' && c <= L'Z') ? c + 32 : c); }
wchar_t __cdecl towupper(wchar_t c) { return (wchar_t)RtlUpcaseUnicodeChar(c); }
int __cdecl isdigit(int c)  { return c >= '0' && c <= '9'; }
int __cdecl isxdigit(int c) { return isdigit(c) || (QzLower(c) >= 'a' && QzLower(c) <= 'f'); }
int __cdecl isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
int __cdecl isalpha(int c)  { return QzLower(c) >= 'a' && QzLower(c) <= 'z'; }
int __cdecl isalnum(int c)  { return isalpha(c) || isdigit(c); }
int __cdecl isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int __cdecl islower(int c)  { return c >= 'a' && c <= 'z'; }
int __cdecl isprint(int c)  { return c >= 0x20 && c < 0x7F; }

char * __cdecl
strcpy(char *Dest, const char *Src)
{
    char *d = Dest;
    while ((*d++ = *Src++));
    return Dest;
}

char * __cdecl
strcat(char *Dest, const char *Src)
{
    strcpy(Dest + strlen(Dest), Src);
    return Dest;
}

char * __cdecl
strchr(const char *s, int c)
{
    for (; *s; s++)
        if (*s == (char)c)
            return (char *)s;
    return c ? NULL : (char *)s;
}

char * __cdecl
strrchr(const char *s, int c)
{
    const char *Last = NULL;
    do
    {
        if (*s == (char)c)
            Last = s;
    } while (*s++);
    return (char *)Last;
}

char * __cdecl
strstr(const char *Haystack, const char *Needle)
{
    size_t n = strlen(Needle);
    for (; *Haystack; Haystack++)
        if (!strncmp(Haystack, Needle, n))
            return (char *)Haystack;
    return n ? NULL : (char *)Haystack;
}

int __cdecl
_strnicmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++)
    {
        int d = QzLower((unsigned char)*a) - QzLower((unsigned char)*b);
        if (d || !*a)
            return d;
    }
    return 0;
}

int __cdecl
_stricmp(const char *a, const char *b)
{
    return _strnicmp(a, b, (size_t)-1);
}

char * __cdecl
_strupr(char *s)
{
    char *p;
    for (p = s; *p; p++)
        *p = (char)QzUpper(*p);
    return s;
}

char * __cdecl
_strlwr(char *s)
{
    char *p;
    for (p = s; *p; p++)
        *p = (char)QzLower(*p);
    return s;
}

wchar_t * __cdecl
wcscpy(wchar_t *Dest, const wchar_t *Src)
{
    wchar_t *d = Dest;
    while ((*d++ = *Src++));
    return Dest;
}

wchar_t * __cdecl
wcsncpy(wchar_t *Dest, const wchar_t *Src, size_t n)
{
    size_t i;
    for (i = 0; i < n && Src[i]; i++)
        Dest[i] = Src[i];
    for (; i < n; i++)
        Dest[i] = 0;
    return Dest;
}

wchar_t * __cdecl
wcscat(wchar_t *Dest, const wchar_t *Src)
{
    wcscpy(Dest + wcslen(Dest), Src);
    return Dest;
}

wchar_t * __cdecl
wcsncat(wchar_t *Dest, const wchar_t *Src, size_t n)
{
    wchar_t *d = Dest + wcslen(Dest);
    while (n-- && *Src)
        *d++ = *Src++;
    *d = 0;
    return Dest;
}

int __cdecl
wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (; n; n--, a++, b++)
    {
        if (*a != *b)
            return (int)*a - (int)*b;
        if (!*a)
            break;
    }
    return 0;
}

int __cdecl
wcscmp(const wchar_t *a, const wchar_t *b)
{
    return wcsncmp(a, b, (size_t)-1);
}

int __cdecl
_wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (; n; n--, a++, b++)
    {
        int d = (int)RtlUpcaseUnicodeChar(*a) - (int)RtlUpcaseUnicodeChar(*b);
        if (d || !*a)
            return d;
    }
    return 0;
}

int __cdecl
_wcsicmp(const wchar_t *a, const wchar_t *b)
{
    return _wcsnicmp(a, b, (size_t)-1);
}

wchar_t * __cdecl
wcschr(const wchar_t *s, wchar_t c)
{
    for (; *s; s++)
        if (*s == c)
            return (wchar_t *)s;
    return c ? NULL : (wchar_t *)s;
}

wchar_t * __cdecl
wcsrchr(const wchar_t *s, wchar_t c)
{
    const wchar_t *Last = NULL;
    do
    {
        if (*s == c)
            Last = s;
    } while (*s++);
    return (wchar_t *)Last;
}

wchar_t * __cdecl
wcsstr(const wchar_t *Haystack, const wchar_t *Needle)
{
    size_t n = wcslen(Needle);
    for (; *Haystack; Haystack++)
        if (!wcsncmp(Haystack, Needle, n))
            return (wchar_t *)Haystack;
    return n ? NULL : (wchar_t *)Haystack;
}

wchar_t * __cdecl
_wcsupr(wchar_t *s)
{
    wchar_t *p;
    for (p = s; *p; p++)
        *p = RtlUpcaseUnicodeChar(*p);
    return s;
}

wchar_t * __cdecl
_wcslwr(wchar_t *s)
{
    wchar_t *p;
    for (p = s; *p; p++)
        *p = towlower(*p);
    return s;
}

unsigned long __cdecl
strtoul(const char *s, char **End, int Base)
{
    unsigned long Value = 0;
    while (isspace((unsigned char)*s))
        s++;
    if ((Base == 0 || Base == 16) && s[0] == '0' && QzLower(s[1]) == 'x')
    {
        s += 2;
        Base = 16;
    }
    else if (Base == 0)
    {
        Base = (*s == '0') ? 8 : 10;
    }
    for (;; s++)
    {
        int c = QzLower((unsigned char)*s), d;
        if (isdigit(c))
            d = c - '0';
        else if (c >= 'a' && c <= 'z')
            d = c - 'a' + 10;
        else
            break;
        if (d >= Base)
            break;
        Value = Value * Base + d;
    }
    if (End)
        *End = (char *)s;
    return Value;
}

long __cdecl
strtol(const char *s, char **End, int Base)
{
    while (isspace((unsigned char)*s))
        s++;
    if (*s == '-')
        return -(long)strtoul(s + 1, End, Base);
    return (long)strtoul(*s == '+' ? s + 1 : s, End, Base);
}

int __cdecl
atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

unsigned long __cdecl
wcstoul(const wchar_t *s, wchar_t **End, int Base)
{
    char Narrow[40];
    char *NarrowEnd;
    unsigned long Value;
    size_t i;

    for (i = 0; i < sizeof(Narrow) - 1 && s[i] && s[i] < 0x80; i++)
        Narrow[i] = (char)s[i];
    Narrow[i] = 0;
    Value = strtoul(Narrow, &NarrowEnd, Base);
    if (End)
        *End = (wchar_t *)s + (NarrowEnd - Narrow);
    return Value;
}

int __cdecl
_wtoi(const wchar_t *s)
{
    return (int)wcstoul(s, NULL, 10);
}
