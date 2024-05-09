#!/usr/bin/env bash

script_dir=$(dirname -- "$( readlink -f -- "$0"; )")
source $script_dir/common.sh

GCB_RESTORER="/nfs/share/zyy/shared_payloads/gcpt_with_single_mtime_0429.bin"
GCB_REF_SO="/nfs/share/zyy/shared_payloads/nemu-multi-core-ref.so"

for var in GCB_REF_SO GCB_RESTORER gem5_home; do
    checkForVariable $var
done

$gem5_home/build/RISCV_CHI/gem5.opt \
    --debug-flags=CommitTrace \
    $gem5_home/configs/example/xiangshan.py --ruby --num-cpus=2 --generic-rv-cpt=$1 --mem-type=DDR4_2400_8x8
