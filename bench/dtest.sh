#!/bin/bash
# dtest.sh <drive>-<slew> - programs build/drive-<drive>-<slew>/plugin_user.bin
# and images/alt_00_f8.rom, then streams for 60s while Lab reads the One ROM
B=$(cd "$(dirname "$0")" && pwd); P=$(dirname "$B"); R=$B/results; mkdir -p $R
C=${C:-952FFC01836FBD89}; LAB=${LAB:-/dev/cu.usbmodem1203}; L=t-$1
onerom program --serial $C --plugin usb --plugin file=$P/build/drive-$1/plugin_user.bin --slot file=$B/images/alt_00_f8.rom,type=27256 --yes >/dev/null 2>&1; sleep 4
python3 $B/lab.py $LAB "x:27256:16" 65 > $R/x-$L.log 2>&1 & sleep 3
python3 $P/throughput.py --serial $C --seconds 60 --raw $R/$L.raw > $R/$L.log 2>&1; wait
last=$(grep -E "s rx=" $R/$L.log | grep -v "rx=    0.0" | tail -1 | awk '{print $1}')
avg=$(grep -E "s rx=" $R/$L.log | sed -E 's/.*rx= *([0-9.]+)KB.*/\1/' | awk '$1>0{s+=$1;n++} END {printf "%.0f", s/n}')
alive=$(onerom scan 2>&1 | grep -q "$C" && echo alive || echo "OFF USB")
echo "$1: $(grep '^frames=' $R/$L.log), streaming until $last, average $avg KB/s while streaming, One ROM $alive, Lab: $(grep -A1 Stopped $R/x-$L.log | tail -1 | sed 's/^ *//')"
