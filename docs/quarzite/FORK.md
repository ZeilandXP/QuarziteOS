# QuarziteOS fork notes

QuarziteOS was forked from ReactOS at upstream commit `303d27d6`
("[WSHTCPIP] Fix SO_KEEPALIVE on bound sockets"). This file lists every
deliberate difference, so upstream changes can be merged and the fork's
changes re-applied.

## 1. Branding

Tool: `sdk/tools/quarzite/rebrand.py` (idempotent; re-run after merges).

It renames the product name "ReactOS" to "QuarziteOS" only where users see
it:

| File type | What is renamed |
|-----------|-----------------|
| `.c .cpp .h .idl` | Inside string literals only. Comments and identifiers are left alone. |
| `.rc .mc` | Resource and message text. |
| `.inf .ini .sif` | Inside quoted values only. Keys, `%Strings%` references and INI section names are left alone. |

It never touches:

* copyright, license or authorship lines, including "ReactOS Team",
  "ReactOS Project", `PROJECT:` headers and e-mail addresses;
* identifiers and macros (`__REACTOS__`, `REACTOS_*`, `ReactOSSetup`);
* paths, file names and registry keys (`C:\ReactOS`, `Software\ReactOS\...`,
  `ramdisk(0)\ReactOS`);
* tokens such as `$ReactOS$` (INF signatures);
* tests (`modules/rostests`), third-party code, host tools, docs and
  side-by-side manifests.

These were edited by hand:

* `sdk/include/reactos/version.rc`: CompanyName "QuarziteOS Project",
  LegalCopyright "(C) 1996-2026 ReactOS Project and QuarziteOS contributors".
* `boot/bootdata/*.ini`: `TitleText=` (unquoted, so the script skips it).
* `boot/bootdata/CMakeLists.txt`: live CD shortcut names.
* `boot/boot_images.cmake`: ISO publisher and volume label.
* `ntoskrnl/ex/init.c`: fallback banner `QUARZITEOS (R)`.
* `dll/cpl/sysdm/startrec.c`: kept the `BootType=ReactOS...` keyword match.
* `modules/rosapps/.../rosperf.c`: comparison length for the longer name.

These still say ReactOS on purpose or are not done yet:

* **Default install directory** `\ReactOS`, the CD directory `\reactos`,
  and `HKLM\Software\ReactOS` keys. These are paths. Changing them, and
  moving to `C:\Windows` for Windows 7 application compatibility, is
  planned as one coordinated change.
* **Logo bitmaps** (FreeLoader, boot screen, setup, msgina, shell "About").
  They still show the ReactOS logo and need QuarziteOS artwork.
* Internal identifiers such as `__REACTOS__`, `REACTOS_*`, `rosapps` and
  `rostests`.

## 2. Version: QuarziteOS 0.1.0, reporting Windows 7 SP1

| What | Where | Value |
|------|-------|-------|
| Product version | `sdk/include/reactos/version.cmake` | 0.1.0, no "-dev" suffix |
| Reported NT version | `sdk/include/psdk/ntverp.h` | 6.1, build 7601.17514 |
| Service pack | `boot/bootdata/hivesys.inf` `Control\Windows\CSDVersion` | `0x100` (SP1) |
| DLL export surface | `sdk/cmake/config.cmake` `DLL_EXPORT_VERSION` | `0x601` |

`_WIN32_WINNT`/`WINVER` for compiling the tree are still `0x502`. Raising
them changes structure layouts, including the loader parameter block
FreeLoader builds, so it has to happen component by component (see the
roadmap).

Because the reported version and the compiled-for version now differ,
`ExpIsLoaderValid()` in `ntoskrnl/ex/init.c` compares the loader block
against `_WIN32_WINNT` instead of `VER_PRODUCTMAJORVERSION`. Without this,
the stock kernel would reject FreeLoader's 5.2 loader block.

A side benefit: with the Windows 7 export surface, the tree links with
stock distribution MinGW-w64 GCC 13. GCC 13's `libgcc` threading code imports
Vista condition-variable APIs from `kernel32`, which a `0x502` export set
lacks.

## 3. The qzkrnl kernel

These are new, so they don't conflict with upstream:

* `qzkrnl/`: the kernel and its generated export table.
* `CMakeLists.txt`: `add_subdirectory(qzkrnl)`.
* `boot/bootdata/bootcd.ini`: the `QzKernel` boot entry
  (`/KERNEL=qzkrnl.exe`).

## 4. Tooling and documentation

* `sdk/tools/quarzite/rebrand.py`: branding script.
* `sdk/tools/quarzite/qemu_boot_test.py`: automated QEMU boot tests.
* `README.md` is new. The upstream README is kept as `README.ReactOS.md`.

## Merging upstream ReactOS

```bash
git remote add reactos https://github.com/reactos/reactos.git
```

```bash
git fetch reactos master
```

```bash
git merge reactos/master
```

Resolve conflicts, then re-apply the branding and regenerate the qzkrnl
export table, because `ntoskrnl.spec` may have changed:

```bash
python3 sdk/tools/quarzite/rebrand.py && python3 qzkrnl/tools/genspec.py
```
