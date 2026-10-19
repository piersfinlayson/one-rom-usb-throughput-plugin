#!/usr/bin/env python3
# Copyright (C) 2026 Piers Finlayson <piers@piers.rocks>
#
# MIT License

# drive.py <label> <set>... - programs build/plugin_user.bin and
# images/fill_00.rom, then for each set streams for 60s with the One ROM in
# standby while Lab drives data lines with the d command.  A set is idle or
# <mask>:<delay>, for example f8:0 for D3-D7 with no delay.  Waits for One ROM
# to be replugged over USB when the link dies.

import os
import re
import subprocess
import sys
import time
from pathlib import Path

import serial
import serial.tools.list_ports

SERIAL = os.environ.get("C", "952FFC01836FBD89")
LAB = os.environ.get("LAB", "/dev/cu.usbmodem1203")
B = Path(__file__).resolve().parent
P = B.parent
label, sets = sys.argv[1], sys.argv[2:]
out = B / "results" / f"drive-{label}"
out.mkdir(parents=True, exist_ok=True)
summary = open(out / "summary.txt", "a")


def say(msg):
    print(msg, flush=True)
    summary.write(msg + "\n")
    summary.flush()


def answers():
    port = next((p.device for p in serial.tools.list_ports.comports()
                 if p.serial_number == SERIAL), None)
    if port is None:
        return False
    try:
        with serial.Serial(port, timeout=0.2, write_timeout=1) as s:
            time.sleep(0.5)
            s.reset_input_buffer()
            s.write(b"stop\n")
            deadline = time.monotonic() + 2
            data = b""
            while time.monotonic() < deadline:
                data += s.read(4096)
                if b"status " in data:
                    return True
    except serial.SerialException:
        pass
    return False


def wait_replug():
    say("REPLUG NEEDED: replug the One ROM's USB, leaving it in Lab")
    while not answers():
        time.sleep(1)
    time.sleep(2)
    say("replug seen, continuing")


def stop_lab():
    try:
        with serial.Serial(LAB, 115200, timeout=0.1, write_timeout=1) as s:
            s.write(b"\r")
    except serial.SerialException:
        pass


def run(lines):
    tag = lines.replace(":", "-")
    tlog, xlog = out / f"t-{tag}.log", out / f"x-{tag}.log"
    if lines == "idle":
        lab_cmd = "?"
    else:
        mask, delay = lines.split(":")
        lab_cmd = f"d:27256:0x{mask}:{delay}"
    lab = subprocess.Popen(["python3", "-u", str(B / "lab.py"), LAB, lab_cmd, "65"],
                           stdout=open(xlog, "w"), stderr=subprocess.STDOUT)
    time.sleep(3)
    t = subprocess.Popen(["python3", "-u", str(P / "throughput.py"), "--serial", SERIAL,
                          "--seconds", "60", "--cmd", "serve off"],
                         stdout=open(tlog, "w"), stderr=subprocess.STDOUT)
    failed_at = None
    while t.poll() is None:
        time.sleep(0.5)
        rates = re.findall(r"^ +([0-9.]+)s rx= +([0-9.]+)KB", tlog.read_text(), re.M)
        if len(rates) >= 3 and all(float(r) == 0.0 for _, r in rates[-3:]):
            failed_at = next(s for s, r in rates if float(r) == 0.0)
            t.kill()
            lab.kill()
            stop_lab()
            break
    lab.wait()
    text = tlog.read_text()
    totals = re.findall(r"total=(\d+)", text)
    errors = re.findall(r"crc=(\d+) stuff=(\d+)", text)
    mhz = re.findall(r"toggles [0-9.]+M +([0-9.]+)MHz", xlog.read_text())
    standby = "host:   serve off\ndevice: ok" in text
    if failed_at:
        outcome = f"FAILED, nothing received from {failed_at}s"
    elif "no reply" in text or "disconnected" in text:
        outcome = "FAILED near the end"
    else:
        outcome = "survived 60s"
    say(f"{lines}: {outcome}, {totals[-1] if totals else 0} frames,"
        f" crc/stuff {errors[-1] if errors else '?'},"
        f" Lab {mhz[-1] + ' MHz' if mhz else 'idle'},"
        f" standby {'yes' if standby else '?'}")
    if outcome.startswith("FAILED"):
        wait_replug()


if not answers():
    wait_replug()
say(f"start {time.strftime('%H:%M:%S')}: {P / 'build/plugin_user.bin'}, image fill_00")
r = subprocess.run(["onerom", "program", "--serial", SERIAL, "--plugin", "usb",
                    "--plugin", f"file={P / 'build/plugin_user.bin'}", "--slot",
                    f"file={B / 'images/fill_00.rom'},type=27256", "--yes"],
                   capture_output=True, text=True)
if r.returncode != 0:
    say(f"PROGRAMMING FAILED: {r.stdout[-300:]} {r.stderr[-300:]}")
    sys.exit(1)
time.sleep(4)
if not answers():
    say("One ROM not found after programming")
    sys.exit(1)
for lines in sets:
    run(lines)
    time.sleep(2)
say(f"DONE {time.strftime('%H:%M:%S')}")
