# qzkrnl: the QuarziteOS kernel

qzkrnl is an NT-style kernel written from scratch for QuarziteOS. It runs
next to the inherited ReactOS `ntoskrnl.exe`, on the same unmodified
`hal.dll`, and is meant to replace ntoskrnl over time.

None of its code comes from ntoskrnl. It reuses only the public SDK headers,
for the binary layouts the NT boot protocol and drivers depend on
(`LOADER_PARAMETER_BLOCK`, `KSYSTEM_TIME`, `RTL_OSVERSIONINFOEXW` and so on).

## How it boots

FreeLoader (`boot/freeldr`) treats qzkrnl exactly like ntoskrnl:

```
[QzKernel]
BootType=Windows2003
SystemPath=\reactos
Options=/KERNEL=qzkrnl.exe /FASTDETECT /MININT
```

FreeLoader loads `qzkrnl.exe` and registers it as `ntoskrnl.exe`. It then
loads `hal.dll`, `kdcom.dll`, `bootvid.dll`, the registry hive and every boot
driver, binds all of their imports against qzkrnl's export table, builds the
page tables (KSEG0 at `0x80000000`, PDE self-map at `0xC0000000`), and jumps
to `KiSystemStartup(LoaderBlock)`.

Binding has to succeed, so qzkrnl exports the **same 1,600+ names as
ntoskrnl.exe**:

* [`exports.spec`](exports.spec) lists what qzkrnl actually implements.
* [`tools/genspec.py`](tools/genspec.py) turns `ntoskrnl/ntoskrnl.spec`
  plus `exports.spec` into [`qzkrnl.spec`](qzkrnl.spec). Every export not
  implemented yet becomes a stub. Each stub logs its name and raises
  `EXCEPTION_WINE_STUB`, which qzkrnl turns into a stop screen naming
  the missing export. Data exports point at a shared zero block.

Re-run `python3 qzkrnl/tools/genspec.py` after editing `exports.spec`.

## What works

| Area | Status |
|------|--------|
| Entry | `KiSystemStartup` switches to its own stack and takes over from the loader. |
| CPU | Own GDT (NT selector layout), TSS, IDT with 256 vectors, PCR on `FS`. |
| HAL | Runs on the regular `hal.dll`: `HalInitializeProcessor`, `HalInitSystem` phases 0 and 1, and the HAL's IRQL (`KfRaiseIrql`/`KfLowerIrql`), PIC, PIT clock, CMOS clock, stall and reboot. qzkrnl provides the kernel side of the contract: `KeSetTimeIncrement`, `KeUpdateSystemTime`, `KiEoiHelper`, `KiDispatchInterrupt`, `KiDeliverApc`, `HalDispatchTable`/`HalPrivateDispatchTable`, and a PCR at `KIP0PCRADDRESS` (`0xFFDFF000`) with a genuine `KIPCR`/`KPRCB`. |
| Traps | Every vector builds an NT `KTRAP_FRAME` (the HAL's layout) and leaves through `KiEoiHelper`. Exceptions produce NT-style stop screens (`0x1E`, `0x50`, `0x7F`). Raised exceptions (`ExRaiseStatus`, `RtlRaiseException`) are dispatched to the frame-based handlers on the stack and unwound properly (`ke/except.c`: `RtlDispatchException`, `RtlUnwind` running finally blocks), which is how file systems handle errors; a call to an unimplemented export stays fatal. |
| Interrupts | `KeInitializeInterrupt`, `KeConnectInterrupt`, `KeDisconnectInterrupt`, `KeSynchronizeExecution`. Device interrupts use `HalBeginSystemInterrupt`/`HalEndSystemInterrupt`, including the HAL's lazy-IRQL replays. The keyboard and COM1 are connected this way; vectors come from `HalGetInterruptVector`. |
| Time | HAL real-time clock at boot plus the HAL clock interrupt (about 15 ms): `KeQuerySystemTime`, `KeQueryInterruptTime`, `KeTickCount`/`KeQueryTickCount`, `RtlTimeToTimeFields`/`RtlTimeFieldsToTime`. Interrupt time, system time, tick count and NT version are published in `KUSER_SHARED_DATA` (`0xFFDF0000`), where x86 drivers read them. |
| IRQL | Owned by the HAL and kept in the PCR at `FS:0x24`. The kernel requests the DISPATCH software interrupt (`HalRequestSoftwareInterrupt`). The HAL calls `KiDispatchInterrupt`, which runs expired timers, queued DPCs and a pending reschedule. |
| PCR | The NT x86 `KIPCR` at `0xFFDFF000` (uniprocessor NT 5.x HALs address it there directly), also reachable through `FS`; PRCB at `0x120`, `CurrentThread` at `0x124`, version and build-type fields the HAL checks. |
| DPCs | `KeInitialize[Threaded]Dpc`, `KeInsertQueueDpc` (importance-aware), `KeRemoveQueueDpc`, `KeFlushQueuedDpcs`, `KeIsExecutingDpc`. |
| Spinlocks | `Ke[f]AcquireSpinLockAtDpcLevel`, release, try, test, in-stack queued. On one CPU, re-acquiring a held lock is reported (`SPIN_LOCK_ALREADY_OWNED`). |
| Dispatcher | `KEVENT` (notification/synchronization, set/reset/clear/pulse), `KMUTANT`/`KMUTEX` (recursive ownership, `THREAD_NOT_MUTEX_OWNER`), `KSEMAPHORE`, all with the public NT layouts. `KeWaitForSingleObject`/`KeWaitForMultipleObjects` (WaitAny/WaitAll, timeouts, `Wait=TRUE` chaining) with NT-style direct handoff. |
| Timers | `KTIMER` one-shot/periodic, notification/synchronization, optional DPC; `KeSetTimer[Ex]`, `KeCancelTimer`, `KeReadStateTimer`. |
| Scheduler | Preemptive round-robin threads with kernel-stack switching (`QzSwapContext`), always at DISPATCH_LEVEL. A switch loads the next thread's address space (CR3), its kernel stack for ring 3 entries (`TSS.Esp0`), its TEB selector and, for user threads, its FPU/SSE state (`fxsave`/`fxrstor`). Ended threads are reaped. `KeDelayExecutionThread`, critical regions, `KeGetCurrentThread`. |
| Memory | Physical page bitmap built from the loader's memory descriptors, after the HAL has taken its pages. Page tables are managed through the self-map. Also `MmMapIoSpace`, `MmGetPhysicalAddress`, `MmAllocate/FreeContiguousMemorySpecifyCache`, MDLs (`IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `MmMapLockedPages[SpecifyCache]`). |
| Address spaces | One page directory per process, the system half shared (every system PDE has a page table before the first process exists). User memory is described by VADs: reserve, commit, protect, decommit and release (`Nt{Allocate,Free,Protect,Query}VirtualMemory`, `Nt{Read,Write}VirtualMemory`), bottom-up or top-down, 64 KB granularity. Pages are demand-zero; guard pages fault once, and a thread's stack grows through its guard page as on NT. `KUSER_SHARED_DATA` is mapped read-only at `0x7FFE0000`. No paging file yet: committed memory is RAM when touched. |
| Sections | Image sections lay a PE file out as it runs (headers and sections at their RVAs, per-page protections); data sections are page-file backed or a copy of a file. Views map the section's own pages; writable image pages and `PAGE_WRITECOPY` views are copy-on-write. `Nt{Create,Open,MapViewOf,UnmapViewOf,Query}Section`; an image away from its base is `STATUS_IMAGE_NOT_AT_BASE`, and ntdll relocates it. |
| Processes | `Process` objects (waitable, signaled at exit) with their own handle tables and client IDs; the System process is PID 4. `NtCreateProcess[Ex]` maps the image, `ntdll.dll` and the NLS tables and builds the PEB; `NtCreateThread` builds the TEB. A new thread first runs ntdll's `LdrInitializeThunk` as a user APC, as on NT. `NtTerminateThread`/`NtTerminateProcess`, `NtSuspend/ResumeThread`, `Nt{Query,Set}Information{Process,Thread}`, `KeStackAttachProcess`, the `Ps*` queries. |
| User mode | Ring 3 through `int 2Eh` and `sysenter` (`SharedUserData->SystemCall` points at ntdll's stub). The service table follows ntdll's numbering (generated from `sysfuncs.h` by `tools/gensys.py`); 127 of its 296 services are implemented. User pointers are copied with fault recovery, and a service that faults on a bad user address is aborted with `STATUS_ACCESS_VIOLATION`. Ring-3 exceptions go to ntdll's `KiUserExceptionDispatcher`; `NtContinue`, `NtRaiseException`, `int 2Dh` debug output and prompts. |
| Boot volume | `cdfs.sys`, the boot driver FreeLoader loads, is started before the boot volume is mounted and mounts the CD; `\ArcName\<boot device>` links to it, so `\SystemRoot\...` works. qzkrnl's own small ISO 9660 reader (`\FileSystem\QzIso`) stays registered behind it as a fallback. |
| Cache manager | `cc/cache.c`: each cached stream has views of 256 KB in a 192 MB arena, filled page by page with paging reads (`IoPageRead`) and written back with paging writes (`IoSynchronousPageWrite`); the copy, pin/map (BCBs across views get a contiguous alias mapping), MDL and zeroing interfaces, file sizes and truncation, purges and flushes. A lazy writer thread writes dirty data back about once a second through the file systems' `AcquireForLazyWrite` callbacks, deletes cache maps unused for a couple of passes (releasing the stream file objects), and gives clean pages back when free memory runs low. `cache` shows what is cached. |
| File systems | FsRtl (`fsrtl/`): byte-range locks, large MCBs, NT wildcard matching (with the DOS forms), FAT/HPFS name legality, change notification (completes waiting requests with `STATUS_NOTIFY_ENUM_DIR`), fast I/O copy routines, stack overflow posting; oplocks are never granted and the tunnel cache keeps nothing. Rtl splay trees, generic and prefix tables, OEM code page conversion from the loader's `c_437.nls`, 8.3 name generation. File systems run as NT starts them: cdfs as a boot driver, `fs_rec`, `msfs` (and `npfs` once its exports are there) as system-start drivers once the boot volume is up, and `fastfat` loaded from the CD when `fs_rec` recognizes a FAT volume (`STATUS_FS_DRIVER_REQUIRED`, `IRP_MN_LOAD_FILE_SYSTEM`). `fstest` reads, creates, writes, renames and deletes on a FAT volume, and the boot test checks the disk image afterwards from the host with mtools. |
| Mapped files | A data section of a file maps the cache's own pages, so a view, cached reads and writes, and the file see one copy; writes through a view reach the file when the view goes, when the cache flushes the file, or with `NtFlushVirtualMemory`. Image sections are read through the cache with paging reads. There is no page file yet: anonymous memory stays resident. |
| smss | `smss` starts `\SystemRoot\System32\smss.exe` the way NT starts it (process parameters, environment, stack, PEB argument). ntdll's loader initializes the process and hands over to smss, which runs its whole initialization: its API port and worker threads (LPC), `\KnownDlls` (every known DLL mapped as an image section and verified), the DOS devices, environment and privileges, `NtInitializeRegistry`, and the `Kmode` subsystem's session. It then loads `win32k.sys` and starts csrss.exe, which initializes the Win32 subsystem, and winlogon.exe, which starts services.exe and lsass.exe and draws the desktop (stage B7, milestone 4). Use `smss 240000` to watch it get there; the display belongs to the video miniport from then on, and the shell stays on the serial port. |
| LPC | Classic NT ports (`lpc/port.c`): named connection ports, a server and client communication port per connection, connection data, optional shared-memory views on both sides, `NtListenPort`/`NtAcceptConnectPort`/`NtCompleteConnectPort`, requests with and without replies, `NtReplyWaitReceivePort[Ex]` with the accepted port context, waitable ports, `LPC_PORT_CLOSED` to the server when a client goes away; `Zw[Secure]ConnectPort`, `ZwRequestWaitReplyPort` and `LpcRequest[WaitReply]Port` for drivers. |
| Keyed events | `KeyedEvent` objects (`ex/keyed.c`): a release and a wait on the same key meet, in either order, with timeouts; a NULL handle uses `\KernelObjects\CritSecOutOfMemoryEvent`, as ntdll's critical sections expect. |
| Objects | Object manager with the NT layouts (`OBJECT_HEADER`, name info, `OBJECT_TYPE`, 37-bucket `OBJECT_DIRECTORY`, `OBJECT_SYMBOLIC_LINK`): `ObCreateObjectType`, `ObCreateObject`, `ObInsertObject` (`OBJ_OPENIF`, `OBJ_PERMANENT`, collisions), `Ob[f]Reference/DereferenceObject`, `ObReferenceObjectByHandle/ByName/ByPointer`, `ObOpenObjectByName/ByPointer`, `ObQueryNameString`, `ObMakeTemporaryObject`. Deletion at raised IRQL is deferred to a reaper thread. |
| Namespace | `\`, `\ObjectTypes`, `\ArcName`, `\BaseNamedObjects`, `\Callback`, `\Device`, `\Driver`, `\FileSystem`, `\KernelObjects`, `\GLOBAL??`; links `\??`, `\DosDevices` and `\SystemRoot`. Lookups follow symbolic links (32 reparses at most), honour `OBJ_CASE_INSENSITIVE`/`OBJ_OPENLINK`, and call a type's parse procedure (the hook for the I/O manager). `Zw[Create/Open/Query]DirectoryObject`, `Zw[Create/Open/Query]SymbolicLinkObject`. |
| Handles | A handle table per process; kernel handles (`OBJ_KERNEL_HANDLE`, `0x8000xxxx`) live in the System table and work from any process, and user mode cannot use them. Inherited handles keep their values. `ZwClose`/`ObCloseHandle`, `NtDuplicateObject` across processes, `ZwQueryObject` (basic/name/type), `ZwMakeTemporaryObject`; `NtCurrentProcess()`/`NtCurrentThread()` resolve. Temporary objects lose their name with the last handle. |
| Executive objects | `Event`, `Semaphore`, `Timer` types; `ZwCreate/Open/Set/Reset/Clear/PulseEvent`, `ZwCreate/Open/Set/CancelTimer`, `ZwWaitForSingleObject`, `ZwWaitForMultipleObjects`. `Adapter`, `Device`, `Driver` and `File` types exist for drivers to name; their procedures come with the I/O manager. |
| Security | One system token (`se/token.c`, `Token` type): every process runs as LocalSystem in Administrators and Everyone with every privilege enabled. `Nt[Open]ProcessToken[Ex]`, `NtQueryInformationToken` (user, groups, privileges, owner, primary group, default DACL, source, type, statistics, session), `NtAdjustPrivilegesToken`, `NtDuplicateToken`. Access checks grant the generic-mapped request, privilege checks succeed. `SeExports` is real (privilege LUIDs and well-known SIDs); `Rtl*Sid` helpers, including `RtlConvertSidToUnicodeString`. |
| I/O manager | Driver and device objects (`IoCreateDriver`, `IoCreateDevice`, device stacks, symbolic links, timers, shutdown notification), IRPs (`IofCallDriver`, `IofCompleteRequest` with completion routines, pending propagation and buffered/direct I/O, cancellation, the IRP builders, remove locks, StartIo queues), file objects (`IoCreateFile`/`ZwCreateFile`/`ZwOpenFile`, relative opens, `Zw{Read,Write,DeviceIoControl,FsControl,QueryInformation,SetInformation,QueryVolumeInformation,QueryDirectory}File`), volume mounting through the VPB and the registered file systems, share access, work items and system worker threads, interrupt connection, controller objects, DMA adapters, MBR partition tables (`IoReadPartitionTableEx`). |
| PnP manager | Device tree from the HAL's PnP driver (`HalInitPnpDriver`, `IoReportDetectedDevice`) down: IDs and capabilities, driver matching through `CriticalDeviceDatabase` and class filters, on-demand boot driver start (`DllInitialize`, `DriverEntry`, `AddDevice`), resources from requirements or boot configuration translated by the HAL, `IRP_MN_START_DEVICE`, bus relations, `IoInvalidateDeviceRelations`, `IoGetDeviceProperty`, device interfaces and PnP notifications, power IRP helpers. A boot driver that imports kernel exports qzkrnl only has stubs for is not started (`drivers` lists why). |
| Drivers from disk | `mm/sysload.c` loads kernel images itself: the file is read whole, laid out at its RVAs in a window of its own (`0xB8000000`), relocated, its `/GS` cookie set, and bound to the modules it imports (export drivers such as `hidclass.sys` or `videoprt.sys` are loaded from `System32\drivers` or `System32` first, forwarders followed); it goes on the loaded module list like the boot images. Images are reference counted, and the last reference unloads (`DllUnload` for export drivers, the pages back). A service's driver comes from its `ImagePath` (`System32\...`, `%SystemRoot%\...` or an NT path; `System32\drivers\<service>.sys` without one); `ZwLoadDriver`/`NtLoadDriver`, `ZwUnloadDriver`/`NtUnloadDriver` (`DriverUnload`, then the image), `\FileSystem\` objects for file system services, disabled services refused. The PnP manager loads drivers that are not boot drivers once the boot volume is mounted, as NT does: on QEMU's USB tablet and keyboard, `hidusb`, `hidclass`, `hidparse`, `mouhid`/`mouclass` and `kbdhid`/`kbdclass` all come from the CD, and `input mouse|kbd` reads their events. The import gate applies to them too: `bochsmp` (through `videoprt`) waits for `Ke386CallBios`, and `win32k.sys` loads for smss but does not start yet (375 exports to go). |
| Real drivers | The unmodified ReactOS boot drivers run on it: `pci`, `isapnp`, `pciide`/`pciidex`, `atapi`, `disk`/`classpnp`, `partmgr`, `cdrom` on KMDF (`wdfldr`, `Wdf01000`) and `mountmgr`. On QEMU this gives `\Device\CdRom0`, `\Device\Harddisk0\DR0`, `Partition0..n`, `HarddiskVolume1..n` and the mount manager's volume names, and sectors read through them. |
| Registry | `\REGISTRY\MACHINE\SYSTEM` is the loader's SYSTEM hive, copied into writable bins with ReactOS's hive library (`cmlib`): keys and values are created, replaced and deleted in stable or volatile (`REG_OPTION_VOLATILE`) storage, and deleted keys invalidate their open handles. `CurrentControlSet` is a volatile symbolic link key to the `ControlSetNNN` that `Select\Current` names. `HARDWARE` and the other keys are volatile in-memory keys, and `HARDWARE\DESCRIPTION` is built from the loader's ARC configuration tree (the HAL finds the PCI BIOS there), plus `CentralProcessor\0` and `FloatingPointProcessor\0` from CPUID (identifier, vendor, name, feature set, clock). `Zw{Open,Create,Query,Enumerate,Delete,Flush}Key`, `Zw{Query,Enumerate,Set,Delete}ValueKey`, `ZwNotifyChangeKey` (event or work item), `Rtl{Query,Write,Delete}RegistryValue(s)`. The PnP manager records each device under `Enum\<instance path>` with its software key in `Control\Class\{guid}\NNNN`; `mountmgr` keeps its drive letters in `MountedDevices`. Changes live in memory until there is a file system to write the hive back to (stage B7). |
| Executive | Interlocked lists and S-lists, lookaside lists, rundown protection, ERESOURCE, fast and guarded mutexes, callback objects, `ZwQuerySystemInformation`, system threads as waitable `Thread` objects (`PsCreateSystemThread`), a `Process` object for System. |
| Pool | `ExAllocatePool[WithTag]` and `ExFreePool[WithTag]`, with tag and caller checks (`BAD_POOL_CALLER`); allocations of a page or more are page-aligned, as on NT. |
| Version | `RtlGetVersion`, `PsGetVersion` and `NtBuildNumber` report **6.1.7601 Service Pack 1**. |
| Rtl | Formatted output with the NT format dialect (`%wZ`, `%Z`, `%S`, `%I64u`...), `DbgPrint[Ex]`, `_vsnprintf`, string and memory routines, counted strings, bitmaps (`RtlInitializeBitMap` & co.). |
| Console | VGA text console with a live status line, PS/2 keyboard, COM1 (115200 8N1) input and output. |
| Shell | `help`, `ver`, `mem`, `pool`, `pooltest`, `mods`, `drivers`, `threads`, `spawn`, `sleep`, `uptime`, `cpu`, `exports`, `synctest`, `obtest`, `obj [path]`, `iotest`, `regtest`, `vmtest`, `usertest`, `secttest`, `ps`, `vm [pid]`, `smss`, `syscalls [on|off]`, `dir <path>`, `type <path>`, `pnp`, `devtree`, `devices`, `drvobj`, `read <device> <sector>`, `reg [path]`, `pcicfg`, `start <service>`, `irptrace`, `verbose`, `irql`, `hal`, `time`, `call <export>`, `bugcheck`, `crash`, `reboot`. |

## Layout

```
qzkrnl/
  ex/        init (entry, phase 0), version reporting, executive objects
             (event, semaphore, timer), keyed events, hard errors, system
             information, debug shell
  ke/        bug checks, HAL clock interface, HAL data/dispatch tables,
             interrupt objects, IRQL/DPCs/spinlocks, dispatcher objects and
             waits, timers, scheduler, user threads, system services,
             exception dispatch and unwinding, self-tests (synctest,
             usertest); ke/i386: CPU tables, traps
  mm/        physical pages, page tables, contiguous memory, MDLs, pool,
             process address spaces and page faults, sections, kernel image
             loader (sysload.c), self-test (vmtest, secttest)
  ob/        object manager: objects and types, namespace and lookup,
             handles, standard directories and links, self-test
  se/        the system token, access check stubs, SeExports
  lpc/       LPC ports, self-test (lpctest: ports, keyed events, tokens)
  io/        I/O manager: drivers (boot drivers and from disk), devices,
             IRPs and paging I/O, files and mounting (with recognizers),
             storage helpers, PnP manager, the ISO 9660 fallback file
             system, self-tests (iotest, ldrtest, fstest)
  cc/        the cache manager and its lazy writer
  fsrtl/     file system runtime: locks, MCBs, names, notification, fast I/O
  cm/        registry: the writable SYSTEM hive (cmlib), volatile keys, notification, self-test (regtest)
  ps/        processes and threads, process creation (PEB, TEB, ntdll,
             the first process), information services
  drv/       serial, VGA text, PS/2 keyboard, input queue
  rtl/       formatted output, string/memory runtime, bitmaps, time conversion,
             export lookup, splay trees and generic/prefix tables, OEM
             strings and 8.3 names
  include/   qz.h, qzio.h, qzps.h (private headers); exportstats.h,
             exportnames.h, systable.h (generated)
  tests/     ldrtest/: qzldrtst.sys and the export driver qzldrdll.sys,
             loaded from the CD by ldrtest
  exports.spec, qzkrnl.spec (generated), tools/genspec.py, tools/gensys.py
```

## Next steps

See [docs/quarzite/ROADMAP.md](../docs/quarzite/ROADMAP.md). In short:

1. ~~IRQL and DPCs, spinlocks, dispatcher objects and waits~~ (stage B1,
   done; see `ke/irql.c`, `ke/wait.c`, `ke/timerobj.c`, `ke/synctest.c`).
2. ~~Host `HAL.DLL`~~ (stage B2, done; see `ke/clock.c`, `ke/intobj.c`,
   `ke/halif.c`, `ke/i386/trap.c`).
3. ~~Object manager, handles, executive objects~~ (stage B3, done; see
   `ob/`, `ex/exobj.c`, `se/se.c`, `ob/obtest.c`).
4. ~~I/O manager, PnP manager, the boot drivers' disk stack~~ (stage B4,
   done; see `io/`, `cm/registry.c`, `io/iotest.c`).
5. ~~Configuration manager proper: writable hives, drive letters~~
   (stage B5, done; see `cm/registry.c`, `cm/regtest.c`).
6. ~~Processes, user-mode address spaces, system calls, `ntdll.dll` and
   `smss.exe`~~ (stage B6, done; see `mm/vm.c`, `mm/section.c`, `ke/user.c`,
   `ke/syscall.c`, `ps/create.c`, `io/isofs.c`).
7. LPC ports, tokens and keyed events, so smss initializes (stage B7
   milestone 1, done; see `lpc/`, `se/token.c`, `ex/keyed.c`); drivers
   loaded from disk (milestone 2, done; see `mm/sysload.c`, `io/driver.c`,
   `io/ldrtest.c`); the cache manager, cdfs and fastfat, mapped files
   (milestone 3, done; see `cc/`, `fsrtl/`, `ke/except.c`, `io/fstest.c`);
   win32k.sys, user-mode callbacks, csrss, winlogon, services and lsass,
   up to the first desktop (milestone 4, in progress; see `ps/win32.c`,
   `ke/callback.c`, `mm/sysview.c`, `rtl/heap.c`, `se/srm.c`).
8. The default kernel (stage B8): APCs, completion ports and the other
   hives so the live environment's services and msgina run (milestone 1,
   in progress; see `ke/apc.c`, `ex/iocomp.c`, `cm/registry.c`); next,
   user-mode PnP, then qzkrnl as the default boot entry.
