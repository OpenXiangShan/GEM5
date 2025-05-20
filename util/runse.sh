#!/bin/bash

if [ $# -ne 1 ]; then
    echo "Usage: $0 <workload_path>"
    exit 1
fi

WORKLOAD_PATH=$1

./build/RISCV/gem5.opt \
    ./configs/example/se.py \
    --cpu-type=DerivO3CPU \
    --mem-size=8GB \
    --caches --cacheline_size=64 \
    --l1i_size=64kB --l1i_assoc=8 \
    --l1d_size=64kB --l1d_assoc=4 \
    --l1d-hwp-type=XSCompositePrefetcher \
    --l2cache --l2_size=1MB --l2_assoc=8 \
    --l3cache --l3_size=16MB --l3_assoc=16 \
    --l1-to-l2-pf-hint \
    --l2-hwp-type=WorkerPrefetcher \
    --l2-to-l3-pf-hint \
    --l3-hwp-type=WorkerPrefetcher \
    --mem-type=DRAMsim3 \
    --dramsim3-ini=./ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini \
    --bp-type=DecoupledBPUWithFTB \
    --enable-loop-predictor \
    --enable-jump-ahead-predictor \
    --cmd "${WORKLOAD_PATH}"

# if options is provided
# add -o "options" after the command
# see ./build/RISCV/gem5.opt ./configs/example/se.py --help