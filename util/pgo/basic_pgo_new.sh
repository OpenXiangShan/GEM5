#!/bin/bash

function checkForVariable() {
    local env_var=
    env_var=$(declare -p "$1")
    if !  [[ -v $1 && $env_var =~ ^declare\ -x ]]; then
        echo "$1 environment variable is not defined, please define!"
        exit 1
    else
        var_value=$(echo $env_var | cut -d'=' -f2)
        echo "\$$1 environment variable is $var_value"
    fi
}

function check() {
    if [ $1 -ne 0 ]; then
        echo FAIL
        exit 1
    fi
}

load=$(cat /proc/loadavg | awk '{print $2}' | cut -d. -f1)
build_threads=${build_threads:-$(( $(nproc) - $load ))}

# locate GEM5
script_dir="$( cd -- "$(dirname "$0")" >/dev/null 2>&1 ; pwd -P )"
gem5_home=$(realpath $script_dir/../..)
echo $script_dir
echo $gem5_home

# check checkpoint used for pgo profiling
export GEM5_PGO_CPT=/nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin
checkForVariable GEM5_PGO_CPT

# build gem5 with pgo instrumentation
CC=clang CXX=clang++ scons build/RISCV/gem5.fast -j $build_threads --gold-linker --pgo-prof
check $?

# run gem5 with pgo instrumentation
mkdir -p llvm-pgo
cd llvm-pgo

# run gem5 and let it to generate profile here
export LLVM_PROFILE_FILE=$(pwd)/default.profraw

$gem5_home/build/RISCV/gem5.fast \
     $gem5_home/configs/example/xiangshan.py \
     --ideal-kmhv3 --raw-cpt \
     --generic-rv-cpt=$GEM5_PGO_CPT
check $?

# TODO: only one profile file is generated, provide multiple
llvm-profdata merge default.profraw --output=gem5_single.profdata
check $?

cd ..

# build gem5 with pgo instrumentation
CC=clang CXX=clang++ scons build/RISCV/gem5.fast -j $build_threads --gold-linker --pgo-use=llvm-pgo/gem5_single.profdata
check $?

printf "PGO build is done!\n"
