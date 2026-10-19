#!/bin/bash
# gate.sh <plugin> <label> - programs <plugin> and images/fill_00.rom.  Passes
# if the stream reaches 810 KB/s with Lab idle and under Lab reads.
B=$(cd "$(dirname "$0")" && pwd); P=$(dirname "$B"); R=$B/results; mkdir -p $R
C=${C:-952FFC01836FBD89}; LAB=${LAB:-/dev/cu.usbmodem1203}
onerom program --serial $C --plugin usb --plugin file=$1 --slot file=$B/images/fill_00.rom,type=27256 --yes >/dev/null 2>&1; sleep 4
avg() { grep -E "^ +([4-9]|1[0-4])\.[0-9]s" | sed -E 's/.*rx= *([0-9.]+)KB.*/\1/' | awk '{s+=$1;n++} END {printf "%.1f", s/n}'; }
idle=$(python3 $P/throughput.py --serial $C --seconds 8 2>&1 | tee $R/g-$2-idle.log | avg)
python3 $B/lab.py $LAB "x:27256:16" 20 > $R/g-$2-x.log 2>&1 & sleep 3
lab=$(python3 $P/throughput.py --serial $C --seconds 15 2>&1 | tee $R/g-$2-lab.log | avg); wait
v=PASS; awk "BEGIN{exit !($idle>=810 && $lab>=810)}" || v=FAIL
echo "$2: idle $idle KB/s, under Lab $lab KB/s -> $v"
