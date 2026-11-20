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

## Track A: Windows 7 SP1 system

1. **Version plumbing.** Audit every user of `GetVersion*`/`RtlGetVersion`
   that changes behaviour at major version 6, and fix the ones that rely on
   NT 6 features not implemented yet. Check the AppCompat shims
   (`dll/appcompat`) for the new baseline.
2. **NT 6 user-mode APIs.** Fill the `-version=0x600+`/`0x601+` stubs in the
   `.spec` files. The most widely imported ones come first: condition
   variables, SRW locks, `InitOnce`, the thread pool (`Tp*`), `*Ex` file and
   locale APIs, `GetTickCount64`, the KTM-less parts of `advapi32`.
3. **Install layout.** Default install directory `C:\Windows`, with the
   paths and registry keys the rebrand left alone (see FORK.md), plus the
   Windows 7 known-folder layout (`Users`, `ProgramData`).
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
| B1 | IRQL (raise/lower, software interrupts), DPCs, spinlocks, `KTHREAD`/`KPRCB`, dispatcher objects, `KeWaitFor*Object(s)`, kernel timers. | Driver-style synchronization works in-kernel. |
| B2 | Host `HAL.DLL`: implement the imports HAL needs, call `HalInitSystem`, move the clock to `HalInitializeProcessor`/the HAL timer, drop the private PIC/PIT code. | qzkrnl runs on ReactOS's HAL unchanged. |
| B3 | Object manager (types, namespace, handles), security stubs, executive objects. | `ObCreateObject`/`ObReferenceObjectByHandle` work. |
| B4 | I/O manager: driver objects, device objects, IRPs, `IoCallDriver`, PnP root enumerator. Start FreeLoader's boot drivers (`DriverEntry`). | The disk stack and a filesystem mount. |
| B5 | Configuration manager on the loader's SYSTEM hive; `Zw*` registry APIs. | Drivers read their service keys. |
| B6 | Processes, user-mode address spaces, sections and image loading, system calls (`int 2e`/`sysenter`), `ntdll.dll`. | `smss.exe` starts. |
| B7 | Paging, file cache (`Cc*`), memory-mapped files. | The live environment boots on qzkrnl. |
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
