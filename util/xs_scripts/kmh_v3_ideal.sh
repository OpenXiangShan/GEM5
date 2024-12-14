#!/usr/bin/env bash

script_dir=$(dirname -- "$( readlink -f -- "$0"; )")
source $script_dir/common.sh

for var in GCBV_REF_SO GCB_RESTORER gem5_home; do
    checkForVariable $var
done

$gem5 $gem5_home/configs/example/xiangshan.py --ideal-kmhv3 --generic-rv-cpt=$1