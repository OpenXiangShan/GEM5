#!/usr/bin/env bash

script_dir=$(dirname -- "$( readlink -f -- "$0"; )")
source $script_dir/common.sh

for var in GCBV_REF_SO GCB_RESTORER gem5_home; do
    checkForVariable $var
done

$gem5 $gem5_home/configs/example/xiangshanCHI.py --generic-rv-cpt=$1 \
  --mem-size=8GB \
  --dramsim3-ini="$gem5_home/ext/dramsim3/xiangshan_configs/xiangshan_DDR4_32Gb_x8_3200_8ch.ini" \
  --bp-type=DecoupledBPUWithBTB \
  --chi-topology=L2L3DramSys_6x6 \
  --chi-rn-attach-point=mesh7.local0 \
  --chi-hn-count=16 \
  --chi-hn-attach-points=mesh7.local1,mesh8.local1,mesh9.local1,mesh10.local1,mesh13.local1,mesh14.local1,mesh15.local1,mesh16.local1,mesh19.local1,mesh20.local1,mesh21.local1,mesh22.local1,mesh25.local1,mesh26.local1,mesh27.local1,mesh28.local1 \
  --chi-dram-count=4 \
  --chi-dram-attach-points=mesh1.local0,mesh4.local0,mesh31.local0,mesh34.local0 \
  --l3_size=64MB \
  --l3_mshrs=256 \
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
