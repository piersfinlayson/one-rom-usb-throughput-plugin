# Copyright (C) 2026 Piers Finlayson <piers@piers.rocks>
#
# MIT License

# lab.py <port> <commands> <seconds> - sends Lab the ;-separated commands, prints
# Lab's output for <seconds>, then sends a key to stop the running command

import serial, sys, time
port, cmds, wait = sys.argv[1], sys.argv[2].split(";"), float(sys.argv[3])
s = serial.Serial(port, 115200, timeout=0.1)
def pump(t):
    end = time.monotonic() + t
    while time.monotonic() < end:
        d = s.read(4096)
        if d: sys.stdout.write(d.decode(errors="replace")); sys.stdout.flush()
pump(1.0); s.write(b"\r"); pump(0.5)
for c in cmds:
    s.write(c.encode() + b"\r"); pump(1.0)
pump(wait)
s.write(b"x"); pump(2)
