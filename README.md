# One ROM USB Throughput Plugin

A One ROM user plugin that streams binary frames to the host over the USB
plugin's CDC serial port.  Can be used to check the reliability and maximum
throughput of USB data.

Requires One ROM firmware v0.8.0+ and USB plugin v0.3.2+.

Supports all One ROM Fire boards with USB.

## Quick Start

On a 28 pin One ROM using the latest published plugin version:

```bash
onerom program --plugin usb \
               --plugin usb-throughput \
               --slot file=<rom>,type=27256
```

## Building

```bash
make
```

Builds `build/plugin_user.bin`.

## Programming

For example, on a 28 pin One ROM:

```bash
onerom program --plugin usb --plugin file=build/plugin_user.bin \
               --slot file=<rom>,type=27256
```

## Running

You must run the provided host-side python script to receive and check the
data sent by the plugin.

```bash
pip install -r requirements.txt
python3 throughput.py --seconds 30
```

Options:

- `--seconds` - How long to run. 0, the default, runs until Ctrl-C.
- `--rate` - Stream rate in bytes per second. 0, the default, streams as fast
  as possible.
- `--size` - Frame size in bytes, a multiple of 4 from 32 to 256. The default
  is 64.
- `--cmd` - A plugin command to send before the stream starts. May be repeated.
- `--at` - A plugin command to send during the run, as `SECONDS:COMMAND`. May
  be repeated.
- `--port` - The script automatically detects One ROM's serial port by VID and
  PID. Use `--port` if more than one One ROM is connected.
- `--serial` - Selects a One ROM by serial number.
- `--raw <file>` - Saves all received data.

One line is printed per second:

- `rx` - all bytes received
- `frames` - bytes in good frames
- `lost` - frames missing from the sequence
- `bad` - frames that failed the CRC or had an invalid size
- `junk` - bytes that are neither frames nor text
- `full/s` - writes per second that failed because channel 0 was full

A more complex example:

```bash
python3 throughput.py --seconds 60 --cmd "led cycle" --at "20:serve off" --at "40:led off"
```

## Commands

The plugin can be driven interactively using the USB serial port.  The
supported commands are printed upon connection and by running `help`.

## Frames Layout

The frame layout is in the comment at the top of `plugin_main.c`.

## One ROM Lab Tests

The scripts in `bench/` stream from a 28 pin One ROM connected to a One ROM
Lab instance while Lab reads it continuously using its `x` command.

- `drun.sh <label> [throughput.py options]` - streams for 60s.
- `lrun.sh <image> <label>` - programs `build/plugin_user.bin` and `<image>`,
  then streams for 60s.
- `dtest.sh <drive>-<slew>` - programs `build/drive-<drive>-<slew>/plugin_user.bin`
  and `images/alt_00_f8.rom`, then streams for 60s.
- `gate.sh <plugin> <label>` - programs `<plugin>` and `images/fill_00.rom`.
  Passes if the stream reaches 840 KB/s with Lab idle and under Lab reads.

The environment variables `C` and `LAB` set the One ROM's serial number and
Lab's serial port:

```bash
C=952FFC01836FBD89 LAB=/dev/cu.usbmodem1203 bench/drun.sh test
```

Output files are written to `bench/results/`.

## Other

`make DRIVE=<mA> SLEW=<slow|fast>` builds a variant that sets the data pins'
drive strength and slew rate at start-up, in `build/drive-<mA>-<slew>/`.

