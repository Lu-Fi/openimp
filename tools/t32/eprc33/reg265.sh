#!/bin/bash
# reg265.sh SO OUTDIR [N]: 7 N / 2 (N default 48) random H.265 scenarios: diff265.py seeds 1 (synthetic
# statistics), 2 (random statistics, per-row IDR sizes), 3 (as 2, random rate-control attributes) and the
# rand265p.py poke scenarios (seeds 1..4), NJ (4) at a time; TASKSET="" for all cores
T=$(dirname $(readlink -f $0)); SO=$(readlink -f $1); O=$2; N=${3:-48}; mkdir -p $O; rm -f $O/*.txt
i=0
for k in $(seq 0 $((N / 2 - 1))); do
  for s in 1 2 3; do
    E33SO=$SO ${TASKSET-taskset -c 4-15} nice -n 19 python3 $T/diff265.py $s 1 $k 2 > $O/s${s}_$k.txt 2>&1 &
    i=$((i+1)); if [ $((i % ${NJ:-4})) -eq 0 ]; then wait; fi
  done
  for s in 1 2 3 4; do   # rand265p.py poke scenarios (P1/P3: synthetic, P2/P4: random statistics; 3/4: FRAME_START knobs)
    E33SO=$SO ${TASKSET-taskset -c 4-15} nice -n 19 python3 $T/rand265p.py $s 1 $k 2 > $O/p${s}_$k.txt 2>&1 &
    i=$((i+1)); if [ $((i % ${NJ:-4})) -eq 0 ]; then wait; fi
  done
done
wait
echo "scenarios with differences: $(grep -h '^scen' $O/*.txt | grep -vc 'diff 0 fault 0')  (of $(grep -h '^scen' $O/*.txt | wc -l))"; grep -L '^scen' $O/*.txt
