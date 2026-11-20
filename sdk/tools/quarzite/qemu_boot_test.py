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
import socket
import subprocess
import sys
import tempfile
import time


class Qemu:
    def __init__(self, iso, workdir, memory=512, extra=None):
        self.workdir = workdir
        self.mon_path = os.path.join(workdir, "mon.sock")
        self.ser_path = os.path.join(workdir, "ser.sock")
        args = [
            "qemu-system-i386", "-m", str(memory), "-cdrom", iso, "-boot", "d",
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


def run_qzkrnl(iso, out):
    chk = Checker()
    with tempfile.TemporaryDirectory() as tmp:
        vm = Qemu(iso, tmp)
        try:
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
                ("drivers", [r"boot drivers \(loaded and bound"]),
                ("pooltest", [r"pooltest: 512 allocations, 0 errors -> PASS"]),
                ("spawn 3", []),
                ("sleep 1000", [r"slept 1000 ms \((9[5-9]|10[0-9]|11[0-9]) ticks"]),
                ("threads", [r"idle", r"status", r"main"]),
                ("uptime", [r"KeTickCount = \d+"]),
                ("cpu", [r"\w+"]),
                ("exports", [r"\d+ implemented by qzkrnl"]),
            ]
            for cmd, patterns in commands:
                mark = len(vm.serial_text())
                vm.send_serial(cmd + "\r")
                text = vm.wait_serial(r"qz> ", 30, start=mark + len(cmd))
                output = (text or "")[mark:]
                ok = text is not None and all(re.search(p, output) for p in patterns)
                missing = [p for p in patterns if not re.search(p, output)]
                chk.check(f"shell: {cmd}", ok, f"missing {missing}" if missing else "")

            demo = vm.wait_serial(r"\[demo 3\] done", 15)
            chk.check("scheduler: 3 demo threads ran to completion",
                      demo and all(f"[demo {i}] done" in demo for i in (1, 2, 3)))

            screen = vm.screen_text()
            open(os.path.join(out, "qzkrnl-screen.txt"), "w").write(screen)
            chk.check("VGA console + status line are live",
                      "QuarziteOS 0.1.0 | qzkrnl" in screen and "qz>" in screen)
            vm.screenshot(os.path.join(out, "qzkrnl-shell.png"))

            # Calling an unimplemented export must produce a clean stop screen
            vm.send_serial("call IoCreateDevice\r")
            text = vm.wait_serial(r"\*\*\* STOP: 0x0000001E", 20)
            chk.check("unimplemented export -> KMODE_EXCEPTION_NOT_HANDLED stop",
                      text and re.search(r"Unimplemented kernel export: (ntoskrnl|qzkrnl)\.exe!IoCreateDevice", text))
            time.sleep(1)
            screen = vm.screen_text()
            open(os.path.join(out, "qzkrnl-bugcheck.txt"), "w").write(screen)
            chk.check("stop screen rendered on VGA",
                      "has stopped to prevent damage" in screen)
            vm.screenshot(os.path.join(out, "qzkrnl-bugcheck.png"))
        finally:
            open(os.path.join(out, "qzkrnl-serial.log"), "w").write(vm.serial_text())
            vm.quit()
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
    """Capture the QuarziteOS boot splash and the shutdown screen."""
    chk = Checker()
    with tempfile.TemporaryDirectory() as tmp:
        vm = Qemu(iso, tmp, memory=1024)
        try:
            # "Live Environment (Graphics Mode)": no /SOS, so the splash shows
            select_boot_entry(vm, "QuarziteOS Live Environment", 1)

            splash = []
            deadline = time.time() + 300
            while time.time() < deadline:
                time.sleep(3)
                shot = vm.screenshot(os.path.join(tmp, f"s{len(splash):03d}.png"))
                try:
                    from PIL import Image
                    im = Image.open(shot).convert("RGB")
                except Exception:
                    continue
                if im.size != (640, 480) or not _is_purple(im.getpixel((320, 170))):
                    if splash:
                        break          # splash is gone: desktop is coming up
                    continue
                # Progress = how much of the bar row is lit
                lit = sum(1 for x in range(225, 415) if sum(im.getpixel((x, 368))) > 150)
                splash.append((lit, shot))

            chk.check("boot splash (QuarziteOS artwork) displayed", bool(splash))
            if splash:
                import shutil
                lit_values = [s[0] for s in splash]
                mid = min(splash, key=lambda s: abs(s[0] - 95))
                shutil.copy(mid[1], os.path.join(out, "boot-splash.png"))
                shutil.copy(splash[-1][1], os.path.join(out, "boot-splash-late.png"))
                chk.check("progress bar fills as boot progresses",
                          max(lit_values) > min(lit_values),
                          f"lit pixels seen: {lit_values}")

            # Let the desktop settle, then shut down
            time.sleep(120)
            vm.screenshot(os.path.join(out, "live-desktop.png"))

            def shutdown_screen_up():
                shot = vm.screenshot(os.path.join(tmp, "shutdown.png"))
                try:
                    return (_is_purple(_pixel(shot, (320, 170))) and
                            sum(_pixel(shot, (204, 300))) > 400, shot)   # "QuarziteOS" text
                except Exception:
                    return (False, shot)

            vm.monitor("system_powerdown")
            ok, shot = False, None
            for _ in range(20):
                time.sleep(3)
                ok, shot = shutdown_screen_up()
                if ok:
                    break
            if not ok:
                # Fall back to Win+R -> shutdown -s -t 0
                vm.sendkey("meta_l-r")
                time.sleep(3)
                for ch in "shutdown -s -t 0":
                    vm.sendkey({" ": "spc", "-": "minus"}.get(ch, ch))
                vm.sendkey("ret")
                for _ in range(60):
                    time.sleep(3)
                    ok, shot = shutdown_screen_up()
                    if ok:
                        break
            chk.check("shutdown screen (QuarziteOS artwork) displayed", ok)
            if shot:
                import shutil
                shutil.copy(shot, os.path.join(out, "shutdown-screen.png"))
        finally:
            vm.quit()
    return chk


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--scenario", choices=["qzkrnl", "live", "screens"], default="qzkrnl")
    ap.add_argument("--out", default=".")
    ap.add_argument("--timeout", type=int, default=600)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    print(f"QuarziteOS boot test: {args.scenario} ({args.iso})")
    if args.scenario == "qzkrnl":
        chk = run_qzkrnl(args.iso, args.out)
    elif args.scenario == "screens":
        chk = run_screens(args.iso, args.out, args.timeout)
    else:
        chk = run_live(args.iso, args.out, args.timeout)

    total = len(chk.results)
    print(f"{total - len(chk.failed)}/{total} checks passed")
    return 1 if chk.failed or not total else 0


if __name__ == "__main__":
    sys.exit(main())
