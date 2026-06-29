#!/usr/bin/env bash

script_dir=$(dirname -- "$( readlink -f -- "$0"; )")
source "$script_dir/common.sh"

for var in GCBV_MULTI_CORE_REF_SO GCB_MULTI_CORE_RESTORER gem5_home; do
    checkForVariable "$var"
done

"$gem5_home/build/RISCV/gem5.opt" \
    "$gem5_home/configs/example/smt_idealkmhv3.py" \
    --dramsim3-ini="$gem5_home/ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_8ch.ini" \
    --generic-rv-cpt="$1"
