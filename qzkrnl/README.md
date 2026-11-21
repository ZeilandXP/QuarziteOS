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
| Traps | Every vector builds an NT `KTRAP_FRAME` (the HAL's layout) and leaves through `KiEoiHelper`. Exceptions produce NT-style stop screens (`0x1E`, `0x50`, `0x7F`). |
| Interrupts | `KeInitializeInterrupt`, `KeConnectInterrupt`, `KeDisconnectInterrupt`, `KeSynchronizeExecution`. Device interrupts use `HalBeginSystemInterrupt`/`HalEndSystemInterrupt`, including the HAL's lazy-IRQL replays. The keyboard and COM1 are connected this way; vectors come from `HalGetInterruptVector`. |
| Time | HAL real-time clock at boot plus the HAL clock interrupt (about 15 ms): `KeQuerySystemTime`, `KeQueryInterruptTime`, `KeTickCount`/`KeQueryTickCount`, `RtlTimeToTimeFields`/`RtlTimeFieldsToTime`. Interrupt time, system time, tick count and NT version are published in `KUSER_SHARED_DATA` (`0xFFDF0000`), where x86 drivers read them. |
| IRQL | Owned by the HAL and kept in the PCR at `FS:0x24`. The kernel requests the DISPATCH software interrupt (`HalRequestSoftwareInterrupt`). The HAL calls `KiDispatchInterrupt`, which runs expired timers, queued DPCs and a pending reschedule. |
| PCR | The NT x86 `KIPCR` at `0xFFDFF000` (uniprocessor NT 5.x HALs address it there directly), also reachable through `FS`; PRCB at `0x120`, `CurrentThread` at `0x124`, version and build-type fields the HAL checks. |
| DPCs | `KeInitialize[Threaded]Dpc`, `KeInsertQueueDpc` (importance-aware), `KeRemoveQueueDpc`, `KeFlushQueuedDpcs`, `KeIsExecutingDpc`. |
| Spinlocks | `Ke[f]AcquireSpinLockAtDpcLevel`, release, try, test, in-stack queued. On one CPU, re-acquiring a held lock is reported (`SPIN_LOCK_ALREADY_OWNED`). |
| Dispatcher | `KEVENT` (notification/synchronization, set/reset/clear/pulse), `KMUTANT`/`KMUTEX` (recursive ownership, `THREAD_NOT_MUTEX_OWNER`), `KSEMAPHORE`, all with the public NT layouts. `KeWaitForSingleObject`/`KeWaitForMultipleObjects` (WaitAny/WaitAll, timeouts, `Wait=TRUE` chaining) with NT-style direct handoff. |
| Timers | `KTIMER` one-shot/periodic, notification/synchronization, optional DPC; `KeSetTimer[Ex]`, `KeCancelTimer`, `KeReadStateTimer`. |
| Scheduler | Preemptive round-robin kernel threads with kernel-stack switching (`QzSwapContext`), always at DISPATCH_LEVEL; `KeDelayExecutionThread`, critical regions, `KeGetCurrentThread`. |
| Memory | Physical page bitmap built from the loader's memory descriptors, after the HAL has taken its pages. Page tables are managed through the self-map. Also `MmMapIoSpace`, `MmGetPhysicalAddress`, `MmAllocate/FreeContiguousMemorySpecifyCache`, MDLs (`IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `MmMapLockedPages[SpecifyCache]`). |
| Objects | Object manager with the NT layouts (`OBJECT_HEADER`, name info, `OBJECT_TYPE`, 37-bucket `OBJECT_DIRECTORY`, `OBJECT_SYMBOLIC_LINK`): `ObCreateObjectType`, `ObCreateObject`, `ObInsertObject` (`OBJ_OPENIF`, `OBJ_PERMANENT`, collisions), `Ob[f]Reference/DereferenceObject`, `ObReferenceObjectByHandle/ByName/ByPointer`, `ObOpenObjectByName/ByPointer`, `ObQueryNameString`, `ObMakeTemporaryObject`. Deletion at raised IRQL is deferred to a reaper thread. |
| Namespace | `\`, `\ObjectTypes`, `\ArcName`, `\BaseNamedObjects`, `\Callback`, `\Device`, `\Driver`, `\FileSystem`, `\KernelObjects`, `\GLOBAL??`; links `\??`, `\DosDevices` and `\SystemRoot`. Lookups follow symbolic links (32 reparses at most), honour `OBJ_CASE_INSENSITIVE`/`OBJ_OPENLINK`, and call a type's parse procedure (the hook for the I/O manager). `Zw[Create/Open/Query]DirectoryObject`, `Zw[Create/Open/Query]SymbolicLinkObject`. |
| Handles | One system handle table (kernel handles, `0x8000xxxx`); `ZwClose`/`ObCloseHandle`, `ZwDuplicateObject`, `ZwQueryObject` (basic/name/type), `ZwMakeTemporaryObject`. Temporary objects lose their name with the last handle. |
| Executive objects | `Event`, `Semaphore`, `Timer` types; `ZwCreate/Open/Set/Reset/Clear/PulseEvent`, `ZwCreate/Open/Set/CancelTimer`, `ZwWaitForSingleObject`, `ZwWaitForMultipleObjects`. `Adapter`, `Device`, `Driver` and `File` types exist for drivers to name; their procedures come with the I/O manager. |
| Security | Stubs until tokens exist: access checks grant the generic-mapped request, privilege checks succeed. `SeExports` is real (privilege LUIDs and well-known SIDs). |
| Pool | `ExAllocatePool[WithTag]` and `ExFreePool[WithTag]`, with tag and caller checks (`BAD_POOL_CALLER`). |
| Version | `RtlGetVersion`, `PsGetVersion` and `NtBuildNumber` report **6.1.7601 Service Pack 1**. |
| Rtl | Formatted output with the NT format dialect (`%wZ`, `%Z`, `%S`, `%I64u`...), `DbgPrint[Ex]`, `_vsnprintf`, string and memory routines, counted strings, bitmaps (`RtlInitializeBitMap` & co.). |
| Console | VGA text console with a live status line, PS/2 keyboard, COM1 (115200 8N1) input and output. |
| Shell | `help`, `ver`, `mem`, `pool`, `pooltest`, `mods`, `drivers`, `threads`, `spawn`, `sleep`, `uptime`, `cpu`, `exports`, `synctest`, `obtest`, `obj [path]`, `irql`, `hal`, `time`, `call <export>`, `bugcheck`, `crash`, `reboot`. |

## Layout

```
qzkrnl/
  ex/        init (entry, phase 0), version reporting, executive objects
             (event, semaphore, timer), debug shell
  ke/        bug checks, HAL clock interface, HAL data/dispatch tables,
             interrupt objects, IRQL/DPCs/spinlocks, dispatcher objects and
             waits, timers, scheduler, self-test; ke/i386: CPU tables, traps
  mm/        physical pages, page tables, contiguous memory, MDLs, pool
  ob/        object manager: objects and types, namespace and lookup,
             handles, standard directories and links, self-test
  se/        security stubs, SeExports
  drv/       serial, VGA text, PS/2 keyboard, input queue
  rtl/       formatted output, string/memory runtime, bitmaps, time conversion,
             export lookup
  include/   qz.h (private header), exportstats.h (generated)
  exports.spec, qzkrnl.spec (generated), tools/genspec.py
```

## Next steps

See [docs/quarzite/ROADMAP.md](../docs/quarzite/ROADMAP.md). In short:

1. ~~IRQL and DPCs, spinlocks, dispatcher objects and waits~~ (stage B1,
   done; see `ke/irql.c`, `ke/wait.c`, `ke/timerobj.c`, `ke/synctest.c`).
2. ~~Host `HAL.DLL`~~ (stage B2, done; see `ke/clock.c`, `ke/intobj.c`,
   `ke/halif.c`, `ke/i386/trap.c`).
3. ~~Object manager, handles, executive objects~~ (stage B3, done; see
   `ob/`, `ex/exobj.c`, `se/se.c`, `ob/obtest.c`).
4. I/O manager and driver loading, then start FreeLoader's boot drivers.
5. Configuration manager on the loader's SYSTEM hive; `smss.exe`.
