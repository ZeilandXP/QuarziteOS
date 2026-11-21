# QuarziteOS roadmap: toward Windows 7 SP1 compatibility

QuarziteOS has two tracks that eventually meet:

* **Track A (system)** retargets the inherited ReactOS system from NT 5.2
  to NT 6.1, so Windows 7 applications and drivers find the APIs,
  structures and behaviour they expect.
* **Track B (kernel)** grows `qzkrnl` until it can replace `ntoskrnl.exe`
  under the same HAL, drivers and user mode.

## 0.1.0 (this release)

- [x] Fork, rebrand, version 0.1.0.
- [x] Report NT 6.1.7601 SP1; Windows 7 export surface (`DLL_EXPORT_VERSION=0x601`).
- [x] Builds with stock Ubuntu MinGW-w64 (GCC 13).
- [x] qzkrnl boots through FreeLoader: CPU tables, traps, stop screens,
      clock, preemptive threads, memory, pool, console, shell.
- [x] Automated QEMU boot test.
- [x] Install to `C:\Windows`; Win7 processor/group query APIs in ntoskrnl.
- [x] qzkrnl stage B1: IRQL, DPCs, spinlocks, dispatcher objects, waits,
      timers, system time (`synctest`: 27/27).
- [x] qzkrnl stage B2: runs on the unmodified `hal.dll` (clock, IRQL,
      interrupt objects through the HAL).
- [x] qzkrnl stage B3: object manager, namespace, handles, executive
      objects (`obtest`: 31/31).
- [x] qzkrnl stage B4: I/O and PnP managers; the unmodified ReactOS boot
      drivers bring up PCI, IDE, the CD-ROM (on KMDF) and the hard disk
      with its partitions (boot test 38/38).
- [x] qzkrnl stage B5: writable SYSTEM hive, `CurrentControlSet` link,
      registry change notification; `mountmgr` assigns drive letters and
      the PnP manager records devices under `Enum` (`regtest`: 40/40,
      boot test 43/43).
- [x] qzkrnl stage B6: processes, user address spaces, sections, ring 3
      with `int 2Eh`/`sysenter`, ntdll's loader; `smss.exe` starts and runs
      its own code (`vmtest` 30/30, `usertest` 7/7, `secttest` 14/14,
      boot test 51/51).

## 0.2.0 (next release, after 0.1.0 is done)

> A minor-version bump, not a patch: this release is a very large
> milestone (amd64, qzkrnl replacing ntoskrnl at B8, USB 3, WDDM 2,
> Vulkan).

### 1. amd64 (x86_64)

Windows 7 SP1 x64 is where most real Windows 7 software and drivers live,
so this comes first.

- [ ] **System.** Bring the inherited experimental amd64 build (FreeLoader,
      HAL, drivers, ntoskrnl) to parity with i386.
- [ ] **qzkrnl on x64** (it is i386-only today):
  - [ ] x64 trap entry and context switching;
  - [ ] 4-level paging and the x64 calling convention;
  - [ ] the x64 loader parameter block layout;
  - [ ] the PCR through `GS` instead of `FS`.
- [ ] **WOW64.** Running 32-bit programs is what makes x64 Win7
      compatibility useful in practice. It is a sizeable project of its own.

### 2. qzkrnl stages B2-B8

- [ ] Complete Track B (table below). At B8 qzkrnl is the default kernel
      and ntoskrnl is retired.

### 3. USB 1.x / 2.0 / 3.x / USB4

- [ ] **USB 1.x/2.0.** Harden the existing UHCI, OHCI and EHCI drivers.
- [ ] **USB 3.x (xHCI).** Newer work in the tree (`ucx01000.sys` is part
      of it). Windows 7 shipped no in-box USB 3 driver; Microsoft's came
      with Windows 8. Native xHCI goes beyond Win7 parity on purpose.
- [ ] **USB4.** Windows supports it only from Windows 11, so there is no
      Win7-era driver model to match. A QuarziteOS extension built on the
      xHCI and PCIe tunneling work.

### 4. WDDM 2 backport and modern graphics drivers

- [ ] **Own clean-room WDDM 2.0.** Windows 7 shipped WDDM 1.1; 2.0 arrived
      with Windows 10. QuarziteOS writes its own `dxgkrnl` implementing the
      2.x driver interfaces (GPU virtual memory, the newer scheduling
      model) from public documentation (WDK headers, MSDN driver docs).
      Together with Mesa below, it is QuarziteOS's own graphics stack.
- [ ] **Compatibility target: the ChefKiss WDDM 2 port.** That community
      project backports WDDM 2.0 (Windows 10 1507/1703 `dxgkrnl`) and
      DirectX 12 (Windows 10 21H2) onto genuine Windows 7 x64. Its
      known-working GPU drivers and DirectX 11/12 applications on real
      Windows 7 x64 become the test matrix QuarziteOS's own implementation
      has to pass.
      - Black-box behaviour reference only. The port consists of modified
        Microsoft binaries plus unlicensed wrapper DLLs, so it is **not**
        shipped, bundled or redistributed with QuarziteOS.
      - Its binaries are not disassembled and none of its code is copied
        (see Ground rules).
- [ ] **Software graphics first.** Use Mesa (open source) for Direct3D and
      Vulkan in software (llvmpipe/lavapipe), so applications get a working
      stack before any GPU driver model exists.
- [ ] **Modern GPU drivers natively.** A bigger commitment than WDDM
      alone. Current NVIDIA, AMD and Intel drivers import many Windows 10+
      kernel functions, so QuarziteOS needs much of the Win10 kernel export
      surface, a superset of the Win7 target. Vendor installers and drivers
      often check the OS version too, so compatibility handling is likely
      (AppCompat shims, version lie per driver).

### 5. Vulkan 1.2

- [ ] **Software first.** Mesa's lavapipe plus the standard Vulkan loader
      (`vulkan-1.dll`). Works with no GPU driver model at all.
- [ ] **Hardware Vulkan.** Comes from the GPU vendor's driver on top of
      WDDM, so it depends on item 4.

## Track A: Windows 7 SP1 system

1. **Version plumbing.** Audit every user of `GetVersion*`/`RtlGetVersion`
   that changes behaviour at major version 6, and fix the ones that rely on
   NT 6 features not implemented yet. Check the AppCompat shims
   (`dll/appcompat`) for the new baseline.
2. **NT 6 user-mode APIs.** Fill the `-version=0x600+`/`0x601+` stubs in the
   `.spec` files. The most widely imported ones come first: condition
   variables, SRW locks, `InitOnce`, the thread pool (`Tp*`), `*Ex` file and
   locale APIs, `GetTickCount64`, the KTM-less parts of `advapi32`.
3. **Install layout.** Done: `C:\Windows`. Remaining: the Windows 7
   known-folder layout (`ProgramData`, per-user `AppData`) and the registry
   keys the rebrand left alone (see FORK.md).
4. **Structures.** Raise `_WIN32_WINNT`/`NTDDI_VERSION` to Windows 7 one
   component at a time. That includes a FreeLoader `BootType=Windows7`
   loader block with the NT 6.1 `LOADER_PARAMETER_BLOCK` layout.
5. **Driver model.** WDM additions from Vista/7 (`IoConnectInterruptEx`,
   MSI, power framework basics, `KeQueryActiveProcessorCount`), KMDF 1.9+,
   NDIS 6, StorPort.
6. **Graphics.** WDDM/DXGI path alongside the XPDM/win32k path.
7. **x64.** Windows 7 SP1 x64 is the dominant Win7 target. Bring the amd64
   build to parity, including WOW64.

## Track B: qzkrnl

| Stage | Contents | Exit criterion |
|-------|----------|----------------|
| B1 ✔ | IRQL (raise/lower, DISPATCH software interrupt), DPCs, spinlocks, NT-layout PCR/PRCB, dispatcher objects (events, mutants, semaphores, timers) with NT layouts, `KeWaitFor*Object(s)`, system time. | Done: `synctest` passes 27/27. |
| B2 ✔ | Host `HAL.DLL`: PCR at `KIP0PCRADDRESS`, `KTRAP_FRAME` traps and `KiEoiHelper`, `KeUpdateSystemTime`, `KiDispatchInterrupt`, dispatch tables, interrupt objects, MDLs, contiguous memory, RTL bitmaps; private PIC/PIT/CMOS code removed. | Done: HAL phases 0 and 1 complete, clock and devices run through the HAL, boot test 26/26, `synctest` 27/27. |
| B3 ✔ | Object manager (NT object header/type/directory layouts, types, namespace with symbolic links and parse hooks, system handle table, deferred deletion), standard directories and links, security stubs with a real `SeExports`, executive objects (event, semaphore, timer) and handle waits, counted-string RTL. | Done: `obtest` 31/31, boot test 29/29, export set still identical to ntoskrnl (1600 names, 218 implemented). |
| B4 ✔ | I/O manager (driver/device/file objects, IRPs and completion, cancellation, StartIo, volume mounting, work items), PnP manager (HAL root, enumeration, `CriticalDeviceDatabase` matching, resources, interfaces, notifications), read-only registry over the loader's SYSTEM hive plus a volatile `HARDWARE` tree from the ARC configuration data, and the executive pieces the drivers need (ERESOURCE, lookaside lists, rundown protection, system threads, ...). | Done: `pci`, `pciide`, `atapi`, `disk`/`classpnp`, `partmgr`, `cdrom` (KMDF) and `mountmgr` run unmodified and read sectors from QEMU's CD and disk; `iotest` 25/25 (including a file system mount); boot test 38/38. Real file system mounts (`cdfs`, `fastfat`) need the cache manager and move to B7. |
| B5 ✔ | Configuration manager proper: the SYSTEM hive copied into writable bins (`cmlib`), keys and values created, replaced and deleted (stable and volatile storage), `CurrentControlSet` as a volatile symbolic link key, `ZwNotifyChangeKey`, real `RtlWriteRegistryValue`/`RtlDeleteRegistryValue`, PnP recording in `Enum\<instance>` and `Control\Class\{guid}\NNNN`. | Done: `mountmgr` stores its database in `MountedDevices` and gives the volumes drive letters (`C:`, `D:`, the CD-ROM next); `regtest` 40/40; boot test 43/43; export set still identical to ntoskrnl (574 implemented). Writing dirty hive cells back to `SYSTEM` on disk needs a file system and moves to B7. |
| B6 ✔ | Processes (objects, client IDs, per-process handle tables, attach), per-process address spaces with VADs, demand-zero and guard pages and stack growth, `Nt*VirtualMemory`; sections (image layout, page-file and file-backed data, copy-on-write views); ring 3 with `int 2Eh` and `sysenter`, a service table in ntdll's numbering, fault-safe user copies, user exception dispatch, `NtContinue`; `NtCreateProcess[Ex]`/`NtCreateThread` with PEB, TEB, NLS tables and ntdll mapped, the initial `LdrInitializeThunk` APC; a read-only ISO 9660 file system for the boot CD. | Done: `smss.exe` starts, ntdll's loader initializes it and hands over to its entry point, and smss runs its initialization (creating its own threads) until it needs LPC; `vmtest` 30/30, `usertest` 7/7, `secttest` 14/14, boot test 51/51; export set still identical to ntoskrnl (612 implemented, 94 of 296 system services). |
| B7 | LPC ports, tokens and keyed events, so smss finishes its initialization (KnownDlls, DOS devices, `NtInitializeRegistry`) and starts `csrss.exe`; paging, file cache (`Cc*`), memory-mapped files, file systems (`cdfs`, `fastfat`), loading drivers from disk, flushing registry hives to disk. | The live environment boots on qzkrnl. |
| B8 | Default kernel; ntoskrnl retired. | |

Each stage moves names from `qzkrnl.spec`'s stub section into `exports.spec`.
The stub count printed at boot and by the shell's `exports` command is the
progress meter.

## Ground rules

* Clean-room only. Use public documentation, the WDK/SDK headers' public
  contracts, black-box testing and the ReactOS/Wine code (GPL/LGPL). Never
  use leaked or disassembled Microsoft code.
* Keep the boot tests green (`sdk/tools/quarzite/qemu_boot_test.py`).
* Prefer merging upstream ReactOS improvements over diverging. Keep
  QuarziteOS-specific changes small and listed in FORK.md.
