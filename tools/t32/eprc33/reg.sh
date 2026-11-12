#!/bin/bash
# reg.sh SO OUTDIR : rand(1) 0..23, rand_rate(2) 0..23, rand3(3) 0..47, rand4(4) 0..47, rand5(5) 0..31, NJ (12) at a time; TASKSET="" for all cores
cd "${E33DIR:-.}"
SO=$1; O=$2; mkdir -p $O; rm -f $O/*.txt
jobs=()
for k in $(seq 0 23); do jobs+=("rand.py 1 1 $k 2" "rand_rate.py 2 1 $k 2"); done
for k in $(seq 0 47); do jobs+=("rand3.py 3 1 $k 2" "rand4.py 4 1 $k 2"); done
for k in $(seq 0 31); do jobs+=("rand5.py 5 1 $k 2"); done
i=0
for j in "${jobs[@]}"; do
  set -- $j
  E33SO=$SO ${TASKSET-taskset -c 4-15} nice -n 19 python3 $j > $O/${1%.py}_$4.txt 2>&1 &
  i=$((i+1)); if [ $((i % ${NJ:-12})) -eq 0 ]; then wait; fi
done
wait
grep -h "^scen" $O/*.txt | grep -vc "diff 0 " ; grep -L "^scen" $O/*.txt
