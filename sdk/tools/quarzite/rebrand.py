#!/usr/bin/env python3
#
# PROJECT:     QuarziteOS
# LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
# PURPOSE:     Re-apply the QuarziteOS branding to user-visible strings.
#
# QuarziteOS is a fork of ReactOS. This script renames the *product name*
# shown to users ("ReactOS" -> "QuarziteOS") in a conservative, repeatable
# way, so it can be re-run after merging upstream ReactOS changes.
#
# What it deliberately does NOT touch:
#   * copyright, license and authorship lines ("ReactOS Team", "ReactOS
#     Project", "Copyright", "(C)", file header PROJECT:/LICENSE: tags, ...)
#     -- the GPL requires these notices to stay intact, and the ReactOS
#     developers remain the authors of the code they wrote;
#   * identifiers and macros (REACTOS_*, __REACTOS__, ReactOSSetup, ...);
#   * file names, directory names and registry/file-system paths
#     ("C:\ReactOS", "Software\ReactOS\...", "ReactOS.theme", ...);
#   * INF [Strings] keys and %ReactOS% references, INI section/key names;
#   * URLs and e-mail addresses (reactos.org);
#   * tests, third-party code, host tools, side-by-side manifests (.xml).
#
# Usage:  python3 sdk/tools/quarzite/rebrand.py [--dry-run] [--list]
#
import os
import re
import sys

OLD = "ReactOS"
NEW = "QuarziteOS"

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))

EXCLUDE_DIRS = (
    ".git", ".github", "modules/rostests", "sdk/lib/3rdparty", "sdk/tools",
    "media/doc", "qzkrnl",
)

# Lines carrying attribution / legal / addresses are never modified.
SKIP_LINE = re.compile(
    r"copyright|\(c\)|©|licen[sc]e|\bGPL\b|PROJECT:|PROGRAMMERS?:|"
    r"ReactOS\s+(Team|Project|Foundation|Deutschland|contributors|developers|"
    r"Development|authors)|reactos\.org|https?://|@",
    re.IGNORECASE)

# Lines that must keep the name: identifiers in disguise or attribution
KEEP_LINES = (
    'if (!_wcsnicmp(szValue, L"ReactOS", 7))',       # FreeLoader BootType keyword
    '/* ReactOS additions */',                         # what upstream added
    'STRING_VERSION_HELP7',                            # cmd: list of its authors
)

# A word on its own, or right after a C escape such as \n, \t or \r
TOKEN = re.compile(r"(?:(?<![A-Za-z0-9_])|(?<=\\[ntr]))ReactOS(?![A-Za-z0-9_])")

CODE_EXT = {".c", ".cpp", ".cxx", ".h", ".hpp", ".idl"}
RC_EXT = {".rc", ".mc"}
INF_EXT = {".inf", ".ini", ".sif"}


def token_ok(text, start, end):
    """Reject occurrences that are path components, file names or keys."""
    before = text[start - 1] if start > 0 else ""
    after = text[end] if end < len(text) else ""
    after2 = text[end + 1] if end + 1 < len(text) else ""
    # \" right after the name is an escaped quote in C, not a path separator
    if after == "\\" and after2 == '"':
        after = '"'
    if (before and before in "\\/%$") or (after and after in "\\/%$"):
        return False
    if after == "." and after2.isalnum():
        return False
    return True


def replace_tokens(segment):
    out, last, count = [], 0, 0
    for m in TOKEN.finditer(segment):
        if token_ok(segment, m.start(), m.end()):
            out.append(segment[last:m.start()])
            out.append(NEW)
            last = m.end()
            count += 1
    out.append(segment[last:])
    return "".join(out), count


def replace_in_quotes(line):
    """Replace only inside "..." on this line (INF/INI/SIF values)."""
    out, count, i = [], 0, 0
    while True:
        a = line.find('"', i)
        if a < 0:
            break
        b = line.find('"', a + 1)
        if b < 0:
            break
        out.append(line[i:a + 1])
        seg, n = replace_tokens(line[a + 1:b])
        out.append(seg)
        count += n
        out.append('"')
        i = b + 1
    out.append(line[i:])
    return "".join(out), count


def process_code(text):
    """Replace inside C string literals only (comments/code untouched)."""
    out, count = [], 0
    i, n = 0, len(text)
    line_start = 0
    while i < n:
        c = text[i]
        if c == "\n":
            line_start = i + 1
            out.append(c)
            i += 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(text[i:j])
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(text[i:j])
            i = j
        elif c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            lit = text[i:j]
            line_end = text.find("\n", i)
            line = text[line_start:(n if line_end < 0 else line_end)]
            # #include "..." and attribution lines stay as they are
            if c == '"' and not SKIP_LINE.search(line) and \
                    not any(k in line for k in KEEP_LINES) and \
                    not line.lstrip().startswith("#include"):
                lit, k = replace_tokens(lit)
                count += k
            out.append(lit)
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out), count


def process_lines(text, func):
    out, count = [], 0
    for line in text.splitlines(keepends=True):
        if SKIP_LINE.search(line) or any(k in line for k in KEEP_LINES):
            out.append(line)
            continue
        new, k = func(line)
        out.append(new)
        count += k
    return "".join(out), count


def decode(raw):
    if raw.startswith(b"\xff\xfe") or raw.startswith(b"\xfe\xff"):
        return raw.decode("utf-16"), "utf-16"
    try:
        return raw.decode("utf-8"), "utf-8"
    except UnicodeDecodeError:
        return raw.decode("latin-1"), "latin-1"


def process_file(path, dry_run):
    ext = os.path.splitext(path)[1].lower()
    with open(path, "rb") as f:
        raw = f.read()
    if OLD.encode() not in raw and OLD.encode("utf-16-le") not in raw:
        return 0
    text, enc = decode(raw)

    if ext in CODE_EXT:
        new, count = process_code(text)
    elif ext in RC_EXT:
        new, count = process_lines(text, replace_tokens)
    elif ext in INF_EXT:
        new, count = process_lines(text, replace_in_quotes)
    else:
        return 0

    if count and not dry_run:
        data = new.encode(enc)
        if enc == "utf-16" and not data.startswith((b"\xff\xfe", b"\xfe\xff")):
            data = b"\xff\xfe" + new.encode("utf-16-le")
        with open(path, "wb") as f:
            f.write(data)
    return count


def main():
    dry_run = "--dry-run" in sys.argv
    listing = "--list" in sys.argv
    total = files = 0
    for dirpath, dirnames, filenames in os.walk(ROOT):
        rel = os.path.relpath(dirpath, ROOT).replace(os.sep, "/")
        dirnames[:] = [d for d in dirnames
                       if not any((rel + "/" + d).lstrip("./").startswith(x)
                                  for x in EXCLUDE_DIRS)]
        for name in filenames:
            path = os.path.join(dirpath, name)
            count = process_file(path, dry_run)
            if count:
                files += 1
                total += count
                if listing:
                    print(f"{count:5d}  {os.path.relpath(path, ROOT)}")
    print(f"{'would replace' if dry_run else 'replaced'} {total} occurrences in {files} files")


if __name__ == "__main__":
    main()
