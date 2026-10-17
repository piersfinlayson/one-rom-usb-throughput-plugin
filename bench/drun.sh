#!/bin/bash
# drun.sh <label> [throughput.py options] - streams for 60s while Lab reads the
# One ROM
B=$(cd "$(dirname "$0")" && pwd); P=$(dirname "$B"); R=$B/results; mkdir -p $R
C=${C:-952FFC01836FBD89}; LAB=${LAB:-/dev/cu.usbmodem1203}; L=$1; shift
python3 $B/lab.py $LAB "x:27256:16" 65 > $R/x-$L.log 2>&1 &
sleep 3; python3 $P/throughput.py --serial $C --seconds 60 --raw $R/$L.raw "$@" > $R/t-$L.log 2>&1; wait
grep -E "s rx=" $R/t-$L.log | grep -v "rx=  84[0-9]\.[0-9]KB/s" | head -4; tail -3 $R/t-$L.log | head -2
grep SHA1 $R/x-$L.log; grep -A1 Stopped $R/x-$L.log | tail -1; onerom scan 2>&1 | grep -q "$C" && echo "One ROM alive" || echo "One ROM NOT FOUND"
