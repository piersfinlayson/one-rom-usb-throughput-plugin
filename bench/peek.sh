#!/bin/bash
# peek.sh <label> <image> <count> - reads the second chip's slots with onerom
# inspect peek memory <count> times and compares each read with <image>, a
# composed image from flash.py.  Run it with the One ROM running or stopped.
B=$(cd "$(dirname "$0")" && pwd); R=$B/results/peek-$1; mkdir -p $R
C=${C:-D78D5C91EA1A3E84}
CHIP=2097152
LEN=$(( $(stat -f %z "$2") - CHIP ))
tail -c +$((CHIP + 1)) "$2" > $R/expected.bin
for i in $(seq 1 $3); do
    rm -f $R/read.bin
    start=$(date +%s)
    if ! onerom inspect peek memory --serial $C --address 0x11000000 --length $LEN \
        --output $R/read.bin --yes > $R/peek.log 2>&1; then
        echo "$i: peek failed: $(tail -1 $R/peek.log)"; continue
    fi
    if cmp -s $R/read.bin $R/expected.bin; then
        echo "$i: ok, $LEN bytes in $(( $(date +%s) - start ))s"
    else
        echo "$i: MISMATCH, $(cmp -l $R/read.bin $R/expected.bin | wc -l) bytes differ"
        cp $R/read.bin $R/bad-$i.bin
    fi
done | tee -a $R/summary.txt
