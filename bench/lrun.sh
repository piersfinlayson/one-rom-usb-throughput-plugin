#!/bin/bash
# lrun.sh <image> <label> - programs build/plugin_user.bin and <image>, then
# streams for 60s while Lab reads the One ROM
B=$(cd "$(dirname "$0")" && pwd); P=$(dirname "$B"); R=$B/results; mkdir -p $R
C=${C:-952FFC01836FBD89}; LAB=${LAB:-/dev/cu.usbmodem1203}
onerom program --serial $C --plugin usb --plugin file=$P/build/plugin_user.bin --slot file=$1,type=27256 --yes 2>&1 | tail -1; sleep 4
python3 $B/lab.py $LAB "x:27256:16" 65 > $R/x-$2.log 2>&1 &
sleep 3; python3 $P/throughput.py --serial $C --seconds 60 --raw $R/$2.raw > $R/t-$2.log 2>&1; wait
grep -E "s rx=" $R/t-$2.log | awk 'NR<=3 || NR%20==0'; grep -E "lost=[1-9]|bad=[1-9]|junk=[1-9]" $R/t-$2.log | head -3; tail -3 $R/t-$2.log
grep SHA1 $R/x-$2.log; grep -A3 Stopped $R/x-$2.log; onerom scan 2>&1 | grep "$C" || echo "One ROM not found by scan"
