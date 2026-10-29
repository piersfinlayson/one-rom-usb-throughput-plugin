#!/usr/bin/env python3
# Copyright (C) 2026 Piers Finlayson <piers@piers.rocks>
#
# MIT License

# flash.py <label> <set>... - programs an L One ROM with build/plugin_user.bin
# and six 512KB 27C400 slots, then for each set streams for SECONDS (default
# 60) while the plugin reads a slot from flash.  The first chip holds three
# random slots and the second chip a random, an all-0x00 and an all-0xFF slot.
# A set is idle, cs0-rand, cs1-rand, cs1-00 or cs1-ff, optionally followed by
# @<bytes/s> to limit the stream rate, for example cs1-rand@400000.  A set may
# end +r to have Lab read the One ROM, or +d to have Lab drive every data line
# with the One ROM in standby, for example cs1-rand@400000+d.  Lab's continuous
# fast read handles chips up to 64KB, so +r uses its batch read.  BOARD sets
# the board, default fire-40-a, LAB Lab's serial port and CPU the clock, for
# example 266MHz.

import hashlib
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

SERIAL = os.environ.get("C", "D78D5C91EA1A3E84")
BOARD = os.environ.get("BOARD", "fire-40-a")
SECONDS = os.environ.get("SECONDS", "60")
LAB = os.environ.get("LAB", "/dev/cu.usbmodem1203")
CPU = os.environ.get("CPU")
LAB_CMDS = {"r": "b:27C400:0:0:cs:1", "d": "d:27C400:all:0"}
SLOT_BYTES = 512 * 1024
CHIP_BYTES = 2 * 1024 * 1024
CS0 = 0x10000000
CS1 = 0x11000000
B = Path(__file__).resolve().parent
P = B.parent
label, sets = sys.argv[1], sys.argv[2:]
out = B / "results" / f"flash-{label}"
out.mkdir(parents=True, exist_ok=True)
summary = open(out / "summary.txt", "a")


def say(msg):
    print(msg, flush=True)
    summary.write(msg + "\n")
    summary.flush()


def fnv(data):
    h = 0x811C9DC5
    for (w,) in struct.iter_unpack("<I", data):
        h = ((h ^ w) * 0x01000193) & 0xFFFFFFFF
    return h


images = {"rand": os.urandom(SLOT_BYTES), "00": bytes(SLOT_BYTES),
          "ff": b"\xff" * SLOT_BYTES}
slots = []
for name in ["rand", "rand", "rand", "rand", "00", "ff"]:
    path = out / f"{name}.rom"
    path.write_bytes(images[name])
    slots += ["--slot", f"file={path},type=27C400"
              + (f",cpu-freq={CPU}" if CPU else "")]
image = out / "image.bin"
subprocess.run(["onerom", "firmware", "build", "--board", BOARD, "--size", "L",
                "--plugin", "usb", "--plugin", f"file={P / 'build/plugin_user.bin'}",
                *slots, "--out", str(image), "--yes"], check=True,
               capture_output=True)
data = image.read_bytes()

# The firmware stores each slot in the board's pin order, so a slot's place
# is found by its stored contents.
regions = {}
for base in range(CHIP_BYTES, len(data), SLOT_BYTES):
    block = data[base:base + SLOT_BYTES]
    kind = ("00" if block.count(0) == SLOT_BYTES else
            "ff" if block.count(255) == SLOT_BYTES else "rand")
    regions[f"cs1-{kind}"] = (CS1 + base - CHIP_BYTES, block)
start = data.find(regions["cs1-rand"][1][:4096])
if not 0 <= start < CHIP_BYTES:
    sys.exit("can't find a random slot on the first chip")
regions["cs0-rand"] = (CS0 + start, data[start:start + SLOT_BYTES])

say(f"start {label}: {BOARD} at {CPU or 'default clock'}, " + ", ".join(
    f"{k} at {v[0]:#x}" for k, v in sorted(regions.items())))
subprocess.run(["onerom", "program", "--serial", SERIAL, "--firmware", str(image),
                "--yes"], check=True, capture_output=True)
subprocess.run(["sleep", "4"])

for s in sets:
    s, _, lab_mode = s.partition("+")
    region, _, rate = s.partition("@")
    cmds = ["--rate", rate or "0",
            "--cmd", "serve off" if lab_mode == "d" else "serve on"]
    if region == "idle":
        cmds += ["--cmd", "flash off"]
    else:
        addr, block = regions[region]
        cmds += ["--cmd", f"flash {addr:#x} {SLOT_BYTES} {fnv(block):#x}"]
    name = f"{s}+{lab_mode}" if lab_mode else s
    log, xlog = out / f"t-{name}.log", out / f"x-{name}.log"
    lab = None
    if lab_mode:
        lab = subprocess.Popen(["python3", "-u", str(B / "lab.py"), LAB,
                                LAB_CMDS[lab_mode], str(float(SECONDS) + 5)],
                               stdout=open(xlog, "w"), stderr=subprocess.STDOUT)
        subprocess.run(["sleep", "3"])
    t = subprocess.Popen(["python3", "-u", str(P / "throughput.py"), "--serial",
                          SERIAL, "--seconds", SECONDS, *cmds],
                         stdout=open(log, "w"), stderr=subprocess.STDOUT)
    # throughput.py waits for a replug if the One ROM stops replying.
    dead = False
    while t.poll() is None:
        subprocess.run(["sleep", "0.5"])
        rx = re.findall(r"rx= *([0-9.]+)KB/s", log.read_text())
        if len(rx) >= 3 and all(float(x) == 0.0 for x in rx[-3:]):
            dead = True
            t.kill()
    if lab:
        lab.wait()
    text = log.read_text()
    rates = [float(m) for m in re.findall(r"rx= *([0-9.]+)KB/s", text)][2:-1]
    tail = [l for l in text.splitlines()
            if l.startswith(("frames=", "device ", "stopped", "disconnected",
                             "no reply"))]
    lo = f"{min(rates):.1f}" if rates else "?"
    lab_res = ""
    if lab_mode:
        x = xlog.read_text()
        if lab_mode == "d":
            mhz = re.findall(r"([0-9.]+)MHz", x)
            lab_res = f" | Lab {mhz[-1] + ' MHz' if mhz else 'NOT RUNNING'}"
        else:
            # Slot 0 is served, and holds the random image.
            want = hashlib.sha1(images["rand"]).hexdigest()
            shas = re.findall(r"16-bit  SHA1: ([0-9a-f]+)", x)
            wrong = sum(sha != want for sha in shas)
            tri = sum(int(n) for n in re.findall(r"tristate failures: (\d+)", x))
            lab_res = (f" | Lab {len(shas)} reads, {wrong} wrong,"
                       f" {tri} tristate failures")
    say(f"{name}: {'LINK DIED, ' if dead else ''}exit {t.returncode},"
        f" min rx {lo} KB/s{lab_res} | " + " | ".join(tail))
    if dead:
        sys.exit(f"One ROM {SERIAL} stopped replying.  Replug it, then rerun.")
