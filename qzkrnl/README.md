# qzkrnl: the QuarziteOS kernel

qzkrnl is an NT-style kernel written from scratch for QuarziteOS. It runs
next to the inherited ReactOS `ntoskrnl.exe` and is meant to replace it
over time.

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
| Traps | Common trap-frame path. Exceptions produce NT-style stop screens (`0x1E`, `0x50`, `0x7F`). |
| Interrupts | 8259 PIC remapped to NT's vector base `0x30`; 8254 PIT at 100 Hz. |
| Time | CMOS RTC at boot plus a 100 Hz clock: `KeQuerySystemTime`, `KeQueryInterruptTime`, `KeTickCount`/`KeQueryTickCount`, `RtlTimeToTimeFields`/`RtlTimeFieldsToTime`. Interrupt time, system time, tick count and NT version are published in `KUSER_SHARED_DATA` (`0xFFDF0000`), where x86 drivers read them. |
| IRQL | Lives in the PCR at `FS:0x24`, as the HAL and drivers expect. At DISPATCH_LEVEL and above, DPCs and thread switches are deferred. Lowering below DISPATCH_LEVEL delivers the DISPATCH software interrupt: expired timers, then queued DPCs, then a pending reschedule. |
| PCR | Binary layout of the NT x86 `KIPCR` for the fields read directly: `SelfPcr`, `Prcb`, `Irql`, `Number`; PRCB at `0x120` with `CurrentThread` at `FS:0x124`. |
| DPCs | `KeInitialize[Threaded]Dpc`, `KeInsertQueueDpc` (importance-aware), `KeRemoveQueueDpc`, `KeFlushQueuedDpcs`, `KeIsExecutingDpc`. |
| Spinlocks | `Ke[f]AcquireSpinLockAtDpcLevel`, release, try, test, in-stack queued. On one CPU, re-acquiring a held lock is reported (`SPIN_LOCK_ALREADY_OWNED`). |
| Dispatcher | `KEVENT` (notification/synchronization, set/reset/clear/pulse), `KMUTANT`/`KMUTEX` (recursive ownership, `THREAD_NOT_MUTEX_OWNER`), `KSEMAPHORE`, all with the public NT layouts. `KeWaitForSingleObject`/`KeWaitForMultipleObjects` (WaitAny/WaitAll, timeouts, `Wait=TRUE` chaining) with NT-style direct handoff. |
| Timers | `KTIMER` one-shot/periodic, notification/synchronization, optional DPC; `KeSetTimer[Ex]`, `KeCancelTimer`, `KeReadStateTimer`. |
| Scheduler | Preemptive round-robin kernel threads, each with its own saved IRQL; `KeDelayExecutionThread`, critical regions, `KeGetCurrentThread`. |
| Memory | Physical page bitmap built from the loader's memory descriptors; page tables managed through the self-map; `MmMapIoSpace`, `MmUnmapIoSpace`, `MmGetPhysicalAddress`. |
| Pool | `ExAllocatePool[WithTag]` and `ExFreePool[WithTag]`, with tag and caller checks (`BAD_POOL_CALLER`). |
| Version | `RtlGetVersion`, `PsGetVersion` and `NtBuildNumber` report **6.1.7601 Service Pack 1**. |
| Rtl | Formatted output with the NT format dialect (`%wZ`, `%Z`, `%S`, `%I64u`...), `DbgPrint[Ex]`, `_vsnprintf`, string and memory routines, counted strings. |
| Console | VGA text console with a live status line, PS/2 keyboard, COM1 (115200 8N1) input and output. |
| Shell | `help`, `ver`, `mem`, `pool`, `pooltest`, `mods`, `drivers`, `threads`, `spawn`, `sleep`, `uptime`, `cpu`, `exports`, `synctest`, `irql`, `time`, `call <export>`, `bugcheck`, `crash`, `reboot`. |

## Layout

```
qzkrnl/
  ex/        init (entry, phase 0), version reporting, debug shell
  ke/        bug checks, clock/PIC, IRQL/DPCs/spinlocks, dispatcher objects and
             waits, timers, scheduler, self-test; ke/i386: CPU tables, traps
  mm/        physical pages, page tables, pool
  drv/       serial, VGA text, PS/2 keyboard, input queue, CMOS clock
  rtl/       formatted output, string/memory runtime, time conversion, export lookup
  include/   qz.h (private header), exportstats.h (generated)
  exports.spec, qzkrnl.spec (generated), tools/genspec.py
```

## Next steps

See [docs/quarzite/ROADMAP.md](../docs/quarzite/ROADMAP.md). In short:

1. ~~IRQL and DPCs, spinlocks, dispatcher objects and waits~~ (stage B1,
   done; see `ke/irql.c`, `ke/wait.c`, `ke/timerobj.c`, `ke/synctest.c`).
2. Host `HAL.DLL` instead of driving the PIC/PIT directly: implement the
   exports the HAL imports and call `HalInitSystem`.
3. Object manager, handles, I/O manager and driver loading, then start
   FreeLoader's boot drivers.
4. Configuration manager on the loader's SYSTEM hive; `smss.exe`.
