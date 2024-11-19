#!/bin/bash

CHECKPOINTS_DIR="checkpoints"
RESULTS_DIR="results"
GCPT_RESTORER="/home/zybzzz/proj/openxiangshan/tools/restorer/gcpt.fromlibcptalpha"
NEMU_REF="/home/zybzzz/proj/openxiangshan/tools/ref_design/riscv64-nemu-interpreter-so.86ca32.tmprelease"

if [ ! -d "$CHECKPOINTS_DIR" ]; then
    echo "checkpoints not exist"
    exit 1
fi

if [ ! -d "$RESULTS_DIR" ]; then
    echo "'$RESULTS_DIR' not exist, now create ..."
    mkdir -p "$RESULTS_DIR"
fi

if [ -f "$GCPT_RESTORER" ]; then
    echo "copy '$GCPT_RESTORER'"
    cp -f "$GCPT_RESTORER" "./gcpt"
fi

if [ -f "$NEMU_REF" ]; then
    echo "copy '$NEMU_REF'"
    cp -f "$NEMU_REF" "./nemu_ref"
fi

echo "done all!"


