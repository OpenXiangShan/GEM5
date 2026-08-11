#!/usr/bin/env bash

script_dir=$(dirname -- "$( readlink -f -- "$0"; )")
source $script_dir/common.sh

for var in GCBV_REF_SO GCB_RESTORER gem5_home; do
    checkForVariable $var
done

$gem5 $gem5_home/configs/example/xiangshanCHI.py --generic-rv-cpt=$1 \
  --bp-type=DecoupledBPUWithBTB \
  --shadow-l2-enable --shadow-l2-count=1 \
  --shadow-attach-points=mesh8.local0 \
  --shadow-src-bases=0x80000000 \
  --shadow-window-sizes=0x80000000 \
  --shadow-dst-bases=0x100000000 \
  --chi-topology=L2L3DramSys_3x3 \
  --chi-voq-depth=2
