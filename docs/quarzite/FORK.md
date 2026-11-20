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

* The CD directory `\reactos` and `HKLM\Software\ReactOS` keys. These are
  internal paths that other components look up by name.
* **Logo bitmaps** in FreeLoader, setup, the LiveCD dialog, msgina and the
  shell "About" banner still show the ReactOS logo and need QuarziteOS
  artwork. The kernel boot splash and shutdown screen are done (section 4).
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

### Install folder: `C:\Windows`

QuarziteOS installs to `C:\Windows`, like Windows 7, instead of `C:\ReactOS`:

* `boot/bootdata/txtsetup.sif`: `DefaultPath = \Windows`. Text and GUI
  setup both use this default unless an unattend file says otherwise.
* `base/setup/usetup/usetup.c`: the fallback when no default is given.
* `boot/bootdata/bootcd/unattend.inf`: the template uses `Windows`.
* `base/services/nfsd`, `dll/3rdparty/libtirpc`: configuration paths.
* FreeLoader's custom-boot prompt example.

The CD layout (`\reactos` on the media) is unchanged. Everything else
already locates the system directory at run time (`%SystemRoot%`,
`GetWindowsDirectory`). ReactOS's own test bot installs to `Windows`, so
this layout is exercised upstream too.

### Windows 7 compatibility fixes

* `dll/win32/syssetup/wizard.c`: second-stage setup wrote CSDVersion
  0x200/0x300 (Server 2003 SP2 / XP SP3) per product type. It now writes
  0x100 (SP1) for all types.
* `ntoskrnl`: added the NT 6.0/6.1 processor and group queries
  (`KeQuery{Active,Maximum}ProcessorCount[Ex]`,
  `KeQuery{Active,Maximum}GroupCount`, `KeGetCurrentProcessorNumberEx`),
  version-gated in `ntoskrnl.spec`. KMDF calls these when the system
  reports NT 6.1, and asserted without them.
* `base/setup/lib/bootsup.c`: the main boot entry's display name (written
  as `L"\"ReactOS\""`) is now "QuarziteOS". `rebrand.py` now treats `\"`
  as an escaped quote and keeps a short list of intentional keeps.

## 3. The qzkrnl kernel

These are new, so they don't conflict with upstream:

* `qzkrnl/`: the kernel and its generated export table.
* `CMakeLists.txt`: `add_subdirectory(qzkrnl)`.
* `boot/bootdata/bootcd.ini`: the `QzKernel` boot entry
  (`/KERNEL=qzkrnl.exe`).

## 4. Boot splash and shutdown screen

* Source artwork: `ntoskrnl/inbv/resources/quarzite/{boot,shutdown}.png`
  (640x480).
* `sdk/tools/quarzite/mkbootscreens.py` converts the artwork to 16-color
  RLE4 bitmaps for bootvid: `qz_boot.bmp`, `qz_bootbar.bmp` and
  `qz_shutdown.bmp`.
* Code: `ntoskrnl/inbv/bootanim.c` (`QUARZITEOS_BOOT_SCREEN`), resource IDs
  23-25 in `ntoskrnl/include/resource.h`, bar geometry in `inbv/logo.h`.
  The splash's progress bar is revealed left to right from the
  artwork. The rotation line is not used. Both screens fall back to the
  classic ones if the bitmaps are missing.
* `ntoskrnl/CMakeLists.txt` and `ntkrnlmp/CMakeLists.txt` now declare the
  resource script's dependencies (bitmaps, `bootanim.rc`, `resource.h`).
  Upstream does not, so changing a bitmap did not rebuild the resources.

## 5. Tooling and documentation

* `sdk/tools/quarzite/rebrand.py`: branding script.
* `sdk/tools/quarzite/qemu_boot_test.py`: automated QEMU boot tests
  (`qzkrnl`: the kernel shell; `screens`: boot splash, live desktop and
  shutdown screen).
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
