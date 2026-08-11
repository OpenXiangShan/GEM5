#!/usr/bin/env bash

script_dir=$(dirname -- "$( readlink -f -- "$0"; )")
source $script_dir/common.sh

for var in GCBV_REF_SO GCB_RESTORER gem5_home; do
    checkForVariable $var
done

$gem5 $gem5_home/configs/example/xiangshanCHI.py --generic-rv-cpt=$1 \
  --dramsim3-ini="$gem5_home/ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini" \
  --bp-type=DecoupledBPUWithBTB \
  --shadow-l2-enable \
  --shadow-l2-count=3 \
  --shadow-attach-points=mesh14.local0,mesh10.local0,mesh4.local0 \
  --shadow-src-bases=0x80000000,0x80000000,0x80000000 \
  --shadow-window-sizes=0x80000000,0x80000000,0x80000000 \
  --shadow-dst-bases=0x100000000,0x180000000,0x200000000 \
  --chi-topology=L2L3DramSys_5x3 \
  --chi-hn-count=8 \
  --chi-hn-attach-points=mesh1.local0,mesh2.local0,mesh3.local0,mesh6.local0,mesh7.local0,mesh8.local0,mesh11.local0,mesh12.local0 \
  --chi-dram-count=2 \
  --chi-dram-attach-points=mesh5.local0,mesh9.local0 \
  --chi-credit-model=cmn700_rtl \
  --chi-rxbuf-num=3 \
  --chi-skid-depth=1 \
  --chi-ib-depth=2 \
  --chi-initial-credit-count=3 \
  --chi-up-crd-lat-int=1 \
  --chi-up-crd-lat-ext=2 \
  --chi-dn-crd-lat-int=2 \
  --chi-dn-crd-lat-ext=1 \
  --chi-internal-crd-lat=1 \
  --chi-voq-depth=2
