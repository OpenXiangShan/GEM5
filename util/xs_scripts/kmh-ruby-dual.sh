#!/usr/bin/env bash

script_dir=$(dirname -- "$( readlink -f -- "$0"; )")
source $script_dir/common.sh

for var in GCBV_MULTI_CORE_REF_SO GCB_MULTI_CORE_RESTORER gem5_home; do
    checkForVariable $var
done

$gem5_home/build/RISCV_CHI/gem5.opt \
    $gem5_home/configs/example/xiangshan.py --ruby --num-cpus=2 --generic-rv-cpt=$1 --mem-type=DDR4_2400_8x8
