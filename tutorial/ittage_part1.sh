#! /bin/bash

# set env
export GCBV_REF_SO="/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so"
export GCB_RESTORER="None"

./build/RISCV/gem5.opt --outdir=tutorial/results/disable_ittage configs/example/xiangshan.py --generic-rv-cpt ./tutorial/test/ittage-riscv64-xs.bin --raw-cpt --bp-type=DecoupledBPUWithFTB --ideal-kmhv3 --enable-arch-db --arch-db-file=tutorial/results/disable_ittage/trace.db --disable-ittage

grep "system.cpu.ipc" tutorial/results/disable_ittage/stats.txt