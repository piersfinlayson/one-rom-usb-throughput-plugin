#!/usr/bin/env python3
# Copyright (C) 2026 Piers Finlayson <piers@piers.rocks>
#
# MIT License

"""Host side of the One ROM USB throughput test.

Starts the plugin's stream and checks each frame's sequence number and CRC.
Prints rates and counts each second, then totals at the end. If the One ROM
stops replying, waits for a replug or port reset to read its final counts.
Exits 1 if a frame was lost or bad or the port disconnected.
"""

import argparse
import struct
import sys
import time
import zlib

import serial
import serial.tools.list_ports

ONEROM_VID = 0x1209
ONEROM_PID = 0xF542

MAGIC = struct.pack("<I", 0x3CC3A55A)
HEADER = struct.Struct("<IHHIII")
USB_ERRORS = struct.Struct("<II")
FRAME_MIN_SIZE = 32
FRAME_MAX_SIZE = 256


def onerom_ports(serial_number):
    return [p for p in serial.tools.list_ports.comports()
            if p.vid == ONEROM_VID and p.pid == ONEROM_PID
            and serial_number in (None, p.serial_number)]


def find_port(serial_number):
    ports = onerom_ports(serial_number)
    if not ports:
        sys.exit("Couldn't find a One ROM serial port.")
    if len(ports) > 1:
        sys.exit(f"Found {len(ports)} One ROM serial ports. Use --serial or --port.")
    return ports[0].device, ports[0].serial_number


class Stats:
    def __init__(self):
        self.rx_bytes = 0
        self.frames = 0
        self.frame_bytes = 0
        self.lost = 0
        self.bad = 0
        self.junk = 0
        self.next_seq = None
        self.last_uptime = None
        self.last_full = 0
        self.last_crc = None
        self.last_stuff = None


class Parser:
    """Splits the byte stream into frames and text lines."""

    def __init__(self, stats, on_line):
        self.stats = stats
        self.on_line = on_line
        self.buf = bytearray()
        self.text = bytearray()

    def feed(self, data):
        self.stats.rx_bytes += len(data)
        self.buf += data
        while self._step():
            pass

    def _text(self, data):
        for b in data:
            if b == 0x0A:
                self.on_line(self.text.decode("ascii").rstrip("\r"))
                self.text.clear()
            elif b == 0x0D or 0x20 <= b < 0x7F:
                self.text.append(b)
            else:
                self.stats.junk += 1

    def _step(self):
        i = self.buf.find(MAGIC)
        if i < 0:
            # The end of the buffer may be the start of a magic split across
            # reads.
            keep = next(k for k in range(len(MAGIC) - 1, -1, -1)
                        if self.buf.endswith(MAGIC[:k]))
            cut = len(self.buf) - keep
            self._text(self.buf[:cut])
            del self.buf[:cut]
            return False
        if i > 0:
            self._text(self.buf[:i])
            del self.buf[:i]
        if len(self.buf) < HEADER.size:
            return False

        _, size, layout, seq, uptime, full = HEADER.unpack_from(self.buf)
        if size < FRAME_MIN_SIZE or size > FRAME_MAX_SIZE or size % 4:
            self._bad()
            return True
        if len(self.buf) < size:
            return False

        (crc,) = struct.unpack_from("<I", self.buf, size - 4)
        if zlib.crc32(self.buf[:size - 4]) != crc:
            self._bad()
            return True

        s = self.stats
        if s.next_seq is not None and seq > s.next_seq:
            s.lost += seq - s.next_seq
        s.next_seq = seq + 1
        s.frames += 1
        s.frame_bytes += size
        s.last_uptime = uptime
        s.last_full = full
        if layout >= 1:
            s.last_crc, s.last_stuff = USB_ERRORS.unpack_from(self.buf, HEADER.size)
        del self.buf[:size]
        return True

    def _bad(self):
        self.stats.bad += 1
        self.stats.junk += 1
        del self.buf[:1]


def parse_at(value):
    t, sep, cmd = value.partition(":")
    if not sep or not cmd:
        raise argparse.ArgumentTypeError("use SECONDS:COMMAND")
    return float(t), cmd


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", help="serial port, found by VID and PID if omitted")
    ap.add_argument("--serial", help="One ROM serial number, to pick its port")
    ap.add_argument("--size", type=int, default=64, help="frame size in bytes")
    ap.add_argument("--rate", type=int, default=0,
                    help="stream rate in bytes/s, 0 for as fast as possible")
    ap.add_argument("--seconds", type=float, default=0,
                    help="run length, 0 to run until Ctrl-C")
    ap.add_argument("--cmd", action="append", default=[],
                    help="plugin command to send before the stream starts, "
                         "for example 'led cycle'")
    ap.add_argument("--at", action="append", default=[], type=parse_at,
                    help="SECONDS:COMMAND to send during the run, "
                         "for example '10:serve off'")
    ap.add_argument("--raw", help="file to save every received byte to")
    args = ap.parse_args()

    port, serial_number = (args.port, None) if args.port else find_port(args.serial)
    ser = serial.Serial(port, timeout=0.05, write_timeout=1)
    raw = open(args.raw, "wb") if args.raw else None

    stats = Stats()
    status = []

    def on_line(line):
        if line:
            print(f"device: {line}", flush=True)
        if line.startswith("status "):
            status.append(line)

    parser = Parser(stats, on_line)

    def pump(duration):
        end = time.monotonic() + duration
        while time.monotonic() < end:
            data = ser.read(max(1, ser.in_waiting))
            if raw:
                raw.write(data)
            parser.feed(data)

    def send(cmd):
        print(f"host:   {cmd}", flush=True)
        ser.write(cmd.encode("ascii") + b"\n")

    # The USB plugin waits 250ms after the port opens before sending.
    pump(0.5)
    for cmd in ["stop", f"size {args.size}", f"rate {args.rate}", *args.cmd]:
        send(cmd)
        pump(0.1)
    status.clear()

    # Frames left from an earlier run don't count.
    stats.__init__()
    stats.next_seq = 0
    send("start")
    start = time.monotonic()
    pending = sorted(args.at)
    last = (start, 0, 0, None, 0)
    reason = "stopped"
    try:
        while not args.seconds or time.monotonic() - start < args.seconds:
            pump(0.1)
            now = time.monotonic()
            while pending and now - start >= pending[0][0]:
                send(pending.pop(0)[1])
            if now - last[0] >= 1.0:
                t0, rx0, fb0, up0, full0 = last
                dt = now - t0
                dev = ""
                if up0 is not None and stats.last_uptime != up0:
                    dev_ms = stats.last_uptime - up0
                    dev = f" full/s={(stats.last_full - full0) * 1000 // max(1, dev_ms)}"
                if stats.last_crc is not None:
                    dev += f" crc={stats.last_crc} stuff={stats.last_stuff}"
                print(f"{now - start:7.1f}s"
                      f" rx={(stats.rx_bytes - rx0) / dt / 1024:7.1f}KB/s"
                      f" frames={(stats.frame_bytes - fb0) / dt / 1024:7.1f}KB/s"
                      f" total={stats.frames} lost={stats.lost}"
                      f" bad={stats.bad} junk={stats.junk}{dev}", flush=True)
                last = (now, stats.rx_bytes, stats.frame_bytes,
                        stats.last_uptime, stats.last_full)
    except KeyboardInterrupt:
        pass
    except serial.SerialException as e:
        reason = f"disconnected: {e}"

    elapsed = time.monotonic() - start
    if reason == "stopped":
        try:
            send("stop")
            deadline = time.monotonic() + 3
            while not status and time.monotonic() < deadline:
                pump(0.1)
        except serial.SerialException as e:
            reason = f"disconnected: {e}"

    if not status and reason == "stopped":
        reason = "no reply"
    print(f"\n{reason} after {elapsed:.1f}s")
    if not status:
        print("The One ROM isn't replying. Replug its USB or reset its port to read"
              " its counts, or press Ctrl-C.", flush=True)
        # Frames sent after the reconnection don't count.
        parser = Parser(Stats(), on_line)
        try:
            while not status:
                time.sleep(1)
                ser.close()
                ports = [p.device for p in onerom_ports(serial_number)
                         if serial_number or p.device == port]
                if not ports:
                    continue
                try:
                    ser = serial.Serial(ports[0], timeout=0.05, write_timeout=1)
                    pump(0.5)
                    send("stop")
                    deadline = time.monotonic() + 3
                    while not status and time.monotonic() < deadline:
                        pump(0.1)
                except serial.SerialException:
                    pass
        except KeyboardInterrupt:
            pass
    print(f"frames={stats.frames} bytes={stats.frame_bytes}"
          f" average={stats.frame_bytes / max(elapsed, 1e-9) / 1024:.1f}KB/s"
          f" lost={stats.lost} bad={stats.bad} junk={stats.junk}")
    if status:
        fields = dict(f.split("=") for f in status[0].split()[1:])
        sent = int(fields["frames"])
        print(f"device sent {sent} frames, host received {stats.frames}"
              f" and counted {stats.lost} lost, {sent - stats.frames - stats.lost}"
              " unaccounted for")
        if "crc" in fields:
            print(f"device counted {fields['crc']} USB CRC errors and"
                  f" {fields['stuff']} bit-stuff errors")
    if raw:
        raw.close()
    return 0 if reason == "stopped" and not (stats.lost or stats.bad) else 1


if __name__ == "__main__":
    sys.exit(main())
