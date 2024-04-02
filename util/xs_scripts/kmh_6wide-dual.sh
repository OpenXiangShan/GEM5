#!/usr/bin/env bash

script_dir=$(dirname -- "$( readlink -f -- "$0"; )")
source $script_dir/common.sh

GCB_RESTORER=$(realpath ~/ready-to-run-zyy/withoutH_gcpt.bin)
GCBV_RESTORER=$(realpath ~/ready-to-run-zyy/withoutH_gcpt.bin)

for var in GCB_REF_SO GCB_RESTORER gem5_home; do
    checkForVariable $var
done

$gem5 \
    --debug-flags=CommitTrace \
    $gem5_home/configs/example/xiangshan-dual.py --generic-rv-cpt=$1 --enable-riscv-vector
    # --debug-flags=CommitTrace,Cache,MSHR,Fetch,Commit,IEW,Decode \
# debug_flags=XSCompositePrefetcher,HWPrefetch,WorkerPref,Cache,MemCtrl,CoherentXBar,PacketQueue,CachePort,MSHR,CommitTrace,Commit,IEW,LSQUnit,PageTableWalker,TLB,RiscvMisc,BOPOffsets,BertiPrefetcher,CacheVerbose
# --debug-flags=CommitTrace,Cache,MemCtrl,CoherentXBar,PacketQueue,CachePort,MSHR,TLB \
