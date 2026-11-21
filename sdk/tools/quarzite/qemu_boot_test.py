#!/usr/bin/env python3
#
# PROJECT:     QuarziteOS
# LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
# PURPOSE:     Boot a QuarziteOS ISO in QEMU and check it end to end.
#
# Scenarios:
#   qzkrnl  - pick "QuarziteOS Kernel (qzkrnl)" in FreeLoader, drive the
#             kernel shell over COM1 and check every command's output, then
#             call an unimplemented export and check the stop screen.
#   live    - boot the regular live environment (ntoskrnl) with serial
#             debugging and check it reaches the desktop shell without a
#             bug check.
#
# Usage:
#   qemu_boot_test.py --iso build/bootcd.iso --scenario qzkrnl --out results/
#
# Needs qemu-system-i386. Screen text is read straight from VGA memory
# through the QEMU monitor, so no OCR is involved.
#
import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time


class Qemu:
    def __init__(self, iso, workdir, memory=512, extra=None, media=None):
        self.workdir = workdir
        self.mon_path = os.path.join(workdir, "mon.sock")
        self.ser_path = os.path.join(workdir, "ser.sock")
        args = [
            "qemu-system-i386", "-m", str(memory),
            *(media if media is not None else ["-cdrom", iso, "-boot", "d"]),
            "-display", "none", "-vga", "std", "-no-reboot",
            "-monitor", f"unix:{self.mon_path},server,nowait",
            "-serial", f"unix:{self.ser_path},server,nowait",
        ]
        if os.path.exists("/dev/kvm") and os.access("/dev/kvm", os.W_OK):
            args += ["-enable-kvm"]
        args += extra or []
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE)
        self.mon = self._connect(self.mon_path)
        self.ser = self._connect(self.ser_path)
        self.ser.setblocking(False)
        self.serial_log = bytearray()
        self._monitor_read()  # banner

    def _connect(self, path):
        for _ in range(100):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                return s
            except OSError:
                time.sleep(0.1)
        raise RuntimeError(f"could not connect to {path}")

    def _monitor_read(self, timeout=5.0):
        self.mon.settimeout(timeout)
        data = b""
        try:
            while not data.endswith(b"(qemu) "):
                chunk = self.mon.recv(65536)
                if not chunk:
                    break
                data += chunk
        except socket.timeout:
            pass
        return data.decode("latin-1")

    def monitor(self, command):
        self.mon.sendall(command.encode() + b"\n")
        return self._monitor_read()

    def screen_text(self, cols=80, rows=25):
        """Read the VGA text buffer through the monitor."""
        out = self.monitor(f"xp /{cols * rows}hx 0xb8000")
        cells = [int(v, 16) for v in re.findall(r"0x([0-9a-f]{4})\b", out)]
        chars = "".join(chr(c & 0xFF) if 0x20 <= (c & 0xFF) < 0x7F else " "
                        for c in cells)
        return "\n".join(chars[i:i + cols].rstrip()
                         for i in range(0, len(chars), cols))

    def screenshot(self, path_png):
        ppm = path_png[:-4] + ".ppm"
        self.monitor(f"screendump {ppm}")
        time.sleep(0.5)
        try:
            from PIL import Image
            Image.open(ppm).save(path_png)
            os.remove(ppm)
            return path_png
        except Exception:
            return ppm

    def sendkey(self, key):
        self.monitor(f"sendkey {key}")
        time.sleep(0.15)

    def pump_serial(self):
        try:
            while True:
                chunk = self.ser.recv(65536)
                if not chunk:
                    break
                self.serial_log += chunk
        except BlockingIOError:
            pass

    def serial_text(self):
        self.pump_serial()
        return self.serial_log.decode("latin-1").replace("\r", "")

    def wait_serial(self, pattern, timeout, start=0):
        deadline = time.time() + timeout
        rx = re.compile(pattern)
        while time.time() < deadline:
            text = self.serial_text()
            if rx.search(text, start):
                return text
            if self.proc.poll() is not None:
                break
            time.sleep(0.2)
        return None

    def wait_screen(self, needle, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.screen_text():
                return True
            time.sleep(0.5)
        return False

    def send_serial(self, text):
        for ch in text.encode():
            self.ser.sendall(bytes([ch]))
            time.sleep(0.01)

    def quit(self):
        try:
            self.monitor("quit")
        except Exception:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def select_boot_entry(vm, title, index_from_top):
    if not vm.wait_screen("Please select the operating system", 90):
        raise RuntimeError("FreeLoader menu did not appear")
    vm.sendkey("home")
    for _ in range(index_from_top):
        vm.sendkey("down")
    time.sleep(0.5)
    screen = vm.screen_text()
    vm.sendkey("ret")
    return screen


class Checker:
    def __init__(self):
        self.results = []

    def check(self, name, ok, detail=""):
        self.results.append((name, bool(ok), detail))
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}{(' - ' + detail) if detail and not ok else ''}")
        return ok

    @property
    def failed(self):
        return [r for r in self.results if not r[1]]


def make_test_disk(path):
    """A 16 MB disk with an MBR and two partitions, for the disk stack.

    The first sector of partition 1 holds a marker the test reads back
    through \\Device\\Harddisk0\\Partition1.
    """
    import struct
    size = 16 * 1024 * 1024
    with open(path, "wb") as f:
        f.truncate(size)
        mbr = bytearray(512)
        mbr[0x1B8:0x1BC] = struct.pack("<I", 0x51A2E0F5)

        def entry(boot, kind, start, count):
            return struct.pack("<B3sB3sII", boot, b"\xfe\xff\xff", kind, b"\xfe\xff\xff",
                               start, count)

        mbr[0x1BE:0x1CE] = entry(0x80, 0x06, 2048, 8192)
        mbr[0x1CE:0x1DE] = entry(0x00, 0x06, 10240, 20480)
        mbr[510:512] = b"\x55\xaa"
        f.seek(0)
        f.write(mbr)
        f.seek(2048 * 512)
        f.write(b"QZPART1-SECTOR-0")
    make_fat_partition(path)


FAT_OFFSET = 10240 * 512        # partition 2 of the test disk
HELLO = b"Hello from the host, read by fastfat on qzkrnl.\r\n"


def mtools(*args, check=True):
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    return subprocess.run(list(args), env=env, capture_output=True, check=check)


def make_fat_partition(path):
    """Partition 2: a FAT16 volume with files for fstest (needs mtools)."""
    if not shutil.which("mformat"):
        return False
    image = f"{path}@@{FAT_OFFSET}"
    mtools("mformat", "-i", image, "-T", "20480", "-h", "64", "-s", "32", "-H", "10240", "-c", "1",
           "-v", "QZFAT", "::")
    with tempfile.TemporaryDirectory() as tmp:
        hello, big = os.path.join(tmp, "hello.txt"), os.path.join(tmp, "big.bin")
        open(hello, "wb").write(HELLO)
        open(big, "wb").write(bytes((i * 7) % 251 for i in range(200000)))
        mtools("mcopy", "-i", image, hello, "::HELLO.TXT")
        mtools("mmd", "-i", image, "::SUBDIR")
        mtools("mcopy", "-i", image, hello, "::SUBDIR/NESTED.TXT")
        mtools("mcopy", "-i", image, big, "::BIG.BIN")
    return True


def check_fat_partition(path, chk):
    """What fstest changed, read back on the host after the VM is gone."""
    if not shutil.which("mtype"):
        return
    image = f"{path}@@{FAT_OFFSET}"

    def read(name):
        r = mtools("mtype", "-i", image, name, check=False)
        return r.stdout if r.returncode == 0 else None

    new = read("::NEWFILE.TXT")
    expected = bytearray((i * 13) % 253 for i in range(300000))
    expected[150000:150004] = b"QZQZ"
    expected[200000:200008] = b"MAPPED!!"       # written through a mapped view
    expected[250000:250008] = b"WRITTEN!"
    chk.check("host: NEWFILE.TXT on disk, 300 000 bytes as written (also through a view)", new == bytes(expected),
              f"{len(new) if new is not None else 'missing'}")
    chk.check("host: long file name on disk", read("::A long file name.txt") == b"lfn\r\n")
    chk.check("host: NEWDIR\\INNER.TXT on disk", read("::NEWDIR/INNER.TXT") == b"inner\r\n")
    chk.check("host: the rename reached the disk", read("::SUBDIR/MOVED.TXT") == HELLO and
              read("::SUBDIR/NESTED.TXT") is None)
    chk.check("host: the delete reached the disk", read("::HELLO.TXT") is None)
    chk.check("host: BIG.BIN untouched", read("::BIG.BIN") == bytes((i * 7) % 251 for i in range(200000)))


def run_qzkrnl(iso, out):
    chk = Checker()
    with tempfile.TemporaryDirectory() as tmp:
        disk = os.path.join(tmp, "disk.img")
        make_test_disk(disk)
        # A USB 2.0 controller with a tablet, as most VMs have: the USB stack
        # (usbport, a miniport, usbhub) must come up and enumerate it
        vm = Qemu(iso, tmp, extra=["-drive", f"file={disk},format=raw,if=ide,index=0",
                                   "-device", "usb-ehci,id=ehci", "-device", "usb-tablet,bus=ehci.0"])
        try:
            # With a hard disk that has an MBR, isoboot asks before booting the CD
            if vm.wait_screen("Press any key", 60):
                vm.sendkey("ret")
            menu = select_boot_entry(vm, "QuarziteOS Kernel", 2)
            open(os.path.join(out, "freeldr-menu.txt"), "w").write(menu)
            chk.check("FreeLoader menu lists the qzkrnl entry",
                      "QuarziteOS Kernel (qzkrnl" in menu)

            text = vm.wait_serial(r"qz> ", 120)
            chk.check("qzkrnl reaches its shell prompt", text is not None)
            if text is None:
                return chk
            chk.check("entered via KiSystemStartup with a loader block",
                      "entered at KiSystemStartup" in text)
            chk.check("banner shows QuarziteOS 0.1.0", "QuarziteOS 0.1.0" in text)
            chk.check("reports the Windows 7 SP1 target",
                      "NT 6.1 build 7601" in text)
            chk.check("runs on HAL.DLL (phase 0 and phase 1 initialized)",
                      "HAL phase 0 done" in text and "HAL phase 1 done" in text)
            m = re.search(r"Loader:\s+(\d+) modules, (\d+) boot drivers", text)
            chk.check("FreeLoader bound HAL/KD/boot drivers to qzkrnl exports",
                      m and int(m.group(1)) >= 4 and int(m.group(2)) >= 1,
                      m.group(0) if m else "no loader line")

            commands = [
                ("ver", [r"RtlGetVersion: 6\.1\.7601 Service Pack 1, SP 1\.0",
                         r"PsGetVersion:  6\.1\.7601 Service Pack 1",
                         r"NtBuildNumber: 0x[cf]0001db1"]),
                ("mem", [r"Physical memory: \d+ KB usable", r"Free\s+\d+ pages"]),
                ("mods", [r"ntoskrnl\.exe", r"hal\.dll", r"kdcom\.dll",
                          r"modules loaded by FreeLoader"]),
                ("drivers", [r"29 boot drivers: 0 started"]),
                ("pooltest", [r"pooltest: 512 allocations, 0 errors -> PASS"]),
                ("spawn 3", []),
                ("sleep 1000", [r"slept 1000 ms \((99\d|10[0-4]\d) ms elapsed"]),
                ("threads", [r"idle", r"status", r"main"]),
                ("uptime", [r"KeTickCount = \d+"]),
                ("cpu", [r"\w+"]),
                ("exports", [r"\d+ implemented by qzkrnl"]),
                ("time", [r"(Mon|Tue|Wed|Thu|Fri|Sat|Sun) (19|20)\d\d-\d\d-\d\d \d\d:\d\d:\d\d"]),
                ("synctest", [r"synctest: \d+ passed, 0 failed"]),
                ("obtest", [r"obtest: \d+ passed, 0 failed"]),
                ("obj", [r"Directory \\", r"ObjectTypes\s+Directory",
                         r"SystemRoot\s+SymbolicLink\s+-> \\ArcName\\multi\(0\)",
                         r"DosDevices\s+SymbolicLink\s+-> \\\?\?"]),
                ("obj \\ObjectTypes", [r"Event\s+Type", r"SymbolicLink\s+Type",
                                        r"File\s+Type"]),
                ("iotest", [r"iotest: \d+ passed, 0 failed"]),
                ("devices", [r"\\Device\\QzTestDisk0\s+\\Driver\\QzTestDisk",
                             r"\\FileSystem\\QzTestFs", r"\(mounted\)"]),
                # Stage B5: the writable SYSTEM hive
                ("regtest", [r"regtest: \d+ passed, 0 failed"]),
                # Stage B6: processes and their address spaces
                ("vmtest", [r"vmtest: \d+ passed, 0 failed"]),
                ("ps", [r"\s4\s+0\s+System\s"]),
                ("usertest", [r"usertest: hello from ring 3", r"usertest: \d+ passed, 0 failed"]),
                ("syscalls", [r"System services: 296 in the table"]),
                ("reg", [r"\[ControlSet001\]", r"\[CurrentControlSet\]"]),
                ("irql", [r"IRQL 0 \(PCR FS:0x24 = 0, HAL KeGetCurrentIrql = 0\), \d+ DPCs retired"]),
                # Stage B4: the real ReactOS boot drivers through qzkrnl's PnP manager
                ("pnp", [r"PnP: 0x00000000, \d+ device node\(s\), \d+ started",
                         r"AddDevice \\Driver\\pci for PCI_HAL\\PNP0A03",
                         r"DriverEntry \\Driver\\Wdf01000 -> 0x00000000"]),
                ("devtree", [r"PCI_HAL\\PNP0A03\\0 \[started, pci\]",
                             r"PCI\\VEN_8086&DEV_7010\S+ \[started, pciide\]",
                             r"PCIIDE\\IDEChannel\S+ \[started, atapi\]",
                             r"IDE\\DiskQEMU_HARDDISK\S+ \[started, disk\]",
                             r"STORAGE\\Partition\S+ \[started\]",
                             r"IDE\\CdRomQEMU\S+ \[started, cdrom\]",
                             r"PCI\\VEN_8086&DEV_24CD\S+ \[started, usbehci\]",
                             r"USB\\ROOT_HUB20\S+ \[started, usbhub\]",
                             # Stage B7: drivers loaded from disk (the HID stack)
                             r"USB\\Vid_0627&Pid_0001\S+ \[started, hidusb\]",
                             r"HID\\Vid_0627&Pid_0001\S+ \[started, mouhid\]"]),
                ("input mouse 20000", [r"input: mouse event: flags 0x1, buttons 0x1"], "click"),
                ("read \\Device\\CdRom0 16", [r"2048 bytes read", r"\.CD001\."]),
                ("read \\Device\\Harddisk0\\Partition0 0", [r"512 bytes read",
                                                         r"last two bytes of the first 512: 55 aa"]),
                ("read \\Device\\Harddisk0\\Partition1 0", [r"QZPART1-SECTOR-0"]),
                ("drivers", [r"MountMgr\s+started", r"cdrom\s+started \(PnP\)",
                             r"29 boot drivers: \d+ started"]),
                ("obj \\GLOBAL??", [r"PhysicalDrive0\s+SymbolicLink\s+-> \\Device\\Harddisk0\\DR0",
                                     r"Volume\{[0-9A-F-]+\}\s+SymbolicLink\s+-> \\Device\\HarddiskVolume1",
                                     r"CdRom0\s+SymbolicLink",
                                     # Stage B5: drive letters from the mount manager
                                     r"C:\s+SymbolicLink\s+-> \\Device\\HarddiskVolume1",
                                     r"[D-Z]:\s+SymbolicLink\s+-> \\Device\\CdRom0"]),
                ("reg \\Registry\\Machine\\System\\MountedDevices",
                 [r"\\DosDevices\\C:\s+type 3", r"\\\?\?\\Volume\{[0-9A-F-]+\}\s+type 3"]),
                ("reg \\Registry\\Machine\\System\\CurrentControlSet\\Enum\\PCI",
                 [r"\[VEN_8086&DEV_7010\S*\]"]),
                ("regtest", [r"regtest: \d+ passed, 0 failed"]),
                # Stage B6: the boot volume through \SystemRoot, and sections
                ("dir \\SystemRoot\\system32\\config", [r"\d+ system\r?\n", r"5 file\(s\)"]),
                ("type \\SystemRoot\\system32\\smss.exe", [r"smss\.exe: (\d+) bytes \(size \1\)",
                                                             r"first line: MZ"]),
                ("secttest", [r"secttest: \d+ passed, 0 failed"]),
                ("lpctest", [r"lpctest: \d+ passed, 0 failed"]),
                ("ldrtest", [r"ldrtest: \d+ passed, 0 failed"]),
                ("fstest", [r"fstest: \d+ passed, 0 failed"]),
                # The exit criterion of stage B6: smss.exe starts; stage B7: it
                # loads win32k.sys, which starts, and csrss.exe runs its image
                ("smss 30000", [r"ps: started \\SystemRoot\\System32\\smss\.exe",
                                r"smss\.exe \(pid \d+\): ntdll's loader is done; running the image",
                                r"loaded \\SystemRoot\\System32\\win32k\.sys at",
                                r"DriverEntry win32k\.sys -> 0x00000000",
                                r"csrss\.exe \(pid \d+\): ntdll's loader is done; running the image",
                                r"smss: pid \d+, ran its own code"]),
                ("hal", [r"HAL: clock tick \d+ x 100ns, stall factor [1-9]\d*",
                         r"[1-9]\d* clock interrupts, [1-9]\d* device interrupts",
                         r"[1-9]\d* interrupt objects connected"]),
            ]
            def check_console():
                screen = vm.screen_text()
                open(os.path.join(out, "qzkrnl-screen.txt"), "w").write(screen)
                chk.check("VGA console + status line are live",
                          "QuarziteOS 0.1.0 | qzkrnl" in screen and "qz>" in screen)
                vm.screenshot(os.path.join(out, "qzkrnl-shell.png"))

            for cmd, patterns, *action in commands:
                if cmd.startswith("smss"):
                    # Before the native subsystem: once csrss opens the video
                    # miniport, it owns the display and the console is serial only
                    check_console()
                mark = len(vm.serial_text())
                vm.send_serial(cmd + "\r")
                if action and action[0] == "click":
                    # A left click on the USB tablet while the shell waits for it
                    vm.wait_serial(r"input: waiting", 30, start=mark)
                    time.sleep(1)
                    vm.monitor("mouse_button 1")
                    time.sleep(0.3)
                    vm.monitor("mouse_button 0")
                text = vm.wait_serial(r"qz> ", 90 if cmd.split()[0] in ("synctest", "obtest", "iotest", "regtest", "vmtest", "usertest", "secttest", "lpctest", "ldrtest", "fstest", "smss", "pnp") else 30,
                                      start=mark + len(cmd))
                output = (text or "")[mark:]
                ok = text is not None and all(re.search(p, output) for p in patterns)
                missing = [p for p in patterns if not re.search(p, output)]
                chk.check(f"shell: {cmd}", ok, f"missing {missing}" if missing else "")

            demo = vm.wait_serial(r"\[demo 3\] done", 15)
            chk.check("scheduler: 3 demo threads ran to completion",
                      demo and all(f"[demo {i}] done" in demo for i in (1, 2, 3)))

            # Calling an unimplemented export must produce a clean stop screen
            # (on VGA again: the bugcheck takes the display back from videoprt)
            vm.send_serial("call KeSetDmaIoCoherency\r")
            text = vm.wait_serial(r"\*\*\* STOP: 0x0000001E", 20)
            chk.check("unimplemented export -> KMODE_EXCEPTION_NOT_HANDLED stop",
                      text and re.search(r"Unimplemented kernel export: (ntoskrnl|qzkrnl)\.exe!KeSetDmaIoCoherency", text))
            time.sleep(1)
            screen = vm.screen_text()
            open(os.path.join(out, "qzkrnl-bugcheck.txt"), "w").write(screen)
            chk.check("stop screen rendered on VGA",
                      "has stopped to prevent damage" in screen)
            vm.screenshot(os.path.join(out, "qzkrnl-bugcheck.png"))
        finally:
            open(os.path.join(out, "qzkrnl-serial.log"), "w").write(vm.serial_text())
            vm.quit()
        # Stage B7: what fstest wrote through fastfat, read on the host
        check_fat_partition(disk, chk)
    return chk


def run_live(iso, out, timeout):
    chk = Checker()
    with tempfile.TemporaryDirectory() as tmp:
        vm = Qemu(iso, tmp, memory=1024)
        try:
            # Default entry is "Live (Debug)": ntoskrnl with KD output on COM1
            if not vm.wait_screen("Please select the operating system", 90):
                chk.check("FreeLoader menu appears", False)
                return chk
            chk.check("FreeLoader menu appears", True)
            vm.sendkey("ret")

            text = vm.wait_serial(r"QuarziteOS 0\.1\.0", 180)
            chk.check("ntoskrnl debug banner shows QuarziteOS 0.1.0", text is not None)

            deadline = time.time() + timeout
            reached_at = None
            bugcheck = False
            while time.time() < deadline:
                text = vm.serial_text()
                if re.search(r"\*\*\* (Fatal System Error|STOP)|KeBugCheck", text):
                    bugcheck = True
                    break
                if reached_at is None and re.search(r"explorer\.exe|userinit|winlogon", text, re.I):
                    reached_at = time.time()
                # Give the shell a minute to settle once user mode is up
                if reached_at and time.time() - reached_at > 60:
                    break
                time.sleep(2)
            chk.check("no bug check during live boot", not bugcheck)
            chk.check("user mode (winlogon/userinit/explorer) started", reached_at is not None)
            vm.screenshot(os.path.join(out, "live-desktop.png"))
        finally:
            open(os.path.join(out, "live-serial.log"), "w").write(vm.serial_text())
            vm.quit()
    return chk


def _pixel(path, xy):
    from PIL import Image
    return Image.open(path).convert("RGB").getpixel(xy)


def _is_purple(rgb):
    r, g, b = rgb
    return r > 90 and b > 70 and g < r * 0.6


def run_screens(iso, out, timeout):
    """Boot the graphical live environment, capture the QuarziteOS boot
    splash (with its progress bar filling), reach the desktop, shut down
    and capture the shutdown screen."""
    import shutil
    from PIL import Image

    def grab(vm, path):
        try:
            return Image.open(vm.screenshot(path)).convert("RGB")
        except Exception:
            return None

    def type_text(vm, text):
        keys = {" ": "spc", "-": "minus"}
        for ch in text:
            vm.sendkey(keys.get(ch, ch))

    chk = Checker()
    with tempfile.TemporaryDirectory() as tmp:
        vm = Qemu(iso, tmp, memory=1024)
        try:
            select_boot_entry(vm, "QuarziteOS Live Environment", 1)

            # 1. Boot splash: sample every second until the desktop mode
            splash, deadline = [], time.time() + 300
            while time.time() < deadline:
                path = os.path.join(tmp, f"s{len(splash):03d}.png")
                im = grab(vm, path)
                if im and im.size == (640, 480) and _is_purple(im.getpixel((320, 170))):
                    lit = sum(1 for x in range(225, 415) if sum(im.getpixel((x, 368))) > 150)
                    splash.append((lit, path))
                elif im and im.size[0] >= 800:
                    break                           # desktop video mode
                time.sleep(1)
            chk.check("boot splash (QuarziteOS artwork) displayed", bool(splash))
            if splash:
                lit = [s[0] for s in splash]
                shutil.copy(max(splash)[1], os.path.join(out, "boot-splash.png"))
                chk.check("progress bar fills as boot progresses",
                          max(lit) > min(lit), f"lit pixels: {lit}")

            # 2. LiveCD dialog -> Next -> "Run QuarziteOS Live CD"
            time.sleep(60)
            vm.screenshot(os.path.join(out, "livecd-dialog.png"))
            vm.sendkey("alt-n")
            time.sleep(10)
            vm.sendkey("ret")
            time.sleep(60)
            vm.screenshot(os.path.join(out, "live-desktop.png"))

            # 3. Shut down via Win+R -> shutdown -s -t 0
            vm.sendkey("meta_l-r")
            time.sleep(3)
            type_text(vm, "shutdown -s -t 0")
            vm.sendkey("ret")
            ok = False
            for _ in range(60):
                time.sleep(3)
                im = grab(vm, os.path.join(tmp, "shutdown.png"))
                if (im and im.size == (640, 480) and _is_purple(im.getpixel((320, 170)))
                        and sum(im.getpixel((204, 300))) > 400):   # "QuarziteOS" text
                    ok = True
                    break
            chk.check("shutdown screen (QuarziteOS artwork) displayed", ok)
            if os.path.exists(os.path.join(tmp, "shutdown.png")):
                shutil.copy(os.path.join(tmp, "shutdown.png"),
                            os.path.join(out, "shutdown-screen.png"))
        finally:
            vm.quit()
    return chk


def run_qzkrnl_usb(iso, out):
    """qzkrnl booted from the hybrid image on a USB stick (xHCI), next to an
    internal IDE disk, as on a PC booted from a stick."""
    chk = Checker()
    with tempfile.TemporaryDirectory() as tmp:
        disk = os.path.join(tmp, "disk.img")
        make_test_disk(disk)
        media = ["-drive", f"if=none,id=stick,format=raw,file={iso},snapshot=on",
                 "-device", "qemu-xhci,id=xhci",
                 "-device", "usb-storage,bus=xhci.0,drive=stick,bootindex=0",
                 "-device", "usb-tablet,bus=xhci.0",
                 "-drive", f"file={disk},format=raw,if=ide,index=0"]
        vm = Qemu(iso, tmp, memory=1024, media=media)
        try:
            select_boot_entry(vm, "QuarziteOS Kernel", 2)
            text = vm.wait_serial(r"qz> ", 120)
            chk.check("qzkrnl reaches its shell from a USB stick",
                      text and re.search(r"Boot:\s+multi\(0\)disk\(0\)rdisk\(0\)partition\(1\)", text))
            commands = [
                ("pnp", [r"PnP: 0x00000000",
                         r"\\ArcName\\multi\(0\)disk\(0\)rdisk\(0\)partition\(1\) -> \\Device\\Harddisk1\\Partition1"]),
                ("devtree", [r"\[started, usbxhci\]", r"\[started, usbhub3\]", r"\[started, usbstor\]",
                             r"USBSTOR\\Disk\S+ \[started, disk\]"]),
                ("dir \\SystemRoot\\system32\\config", [r"5 file\(s\)"]),
                ("smss", [r"smss: pid \d+, ran its own code"]),
            ]
            for cmd, patterns in commands:
                mark = len(vm.serial_text())
                vm.send_serial(cmd + "\r")
                text = vm.wait_serial(r"qz> ", 120, start=mark + len(cmd))
                output = (text or "")[mark:]
                missing = [p for p in patterns if not re.search(p, output)]
                chk.check(f"usb boot: {cmd}", text is not None and not missing,
                          f"missing {missing}" if missing else "")
        finally:
            open(os.path.join(out, "qzkrnl-usb-serial.log"), "w").write(vm.serial_text())
            vm.quit()
    return chk


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--scenario", choices=["qzkrnl", "qzkrnl-usb", "live", "screens"], default="qzkrnl")
    ap.add_argument("--out", default=".")
    ap.add_argument("--timeout", type=int, default=600)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    print(f"QuarziteOS boot test: {args.scenario} ({args.iso})")
    if args.scenario == "qzkrnl":
        chk = run_qzkrnl(args.iso, args.out)
    elif args.scenario == "qzkrnl-usb":
        chk = run_qzkrnl_usb(args.iso, args.out)
    elif args.scenario == "screens":
        chk = run_screens(args.iso, args.out, args.timeout)
    else:
        chk = run_live(args.iso, args.out, args.timeout)

    total = len(chk.results)
    print(f"{total - len(chk.failed)}/{total} checks passed")
    return 1 if chk.failed or not total else 0


if __name__ == "__main__":
    sys.exit(main())
