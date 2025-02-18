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
gem5_home=${gem5_home:-$(realpath $script_dir/../..)}

# setup env
checkForVariable AOCC_SH
source $AOCC_SH

# check checkpoint used for pgo profiling
checkForVariable GEM5_PGO_CPT
checkForVariable GCPT_RESTORER

printf "\n\nWARNING: I will delete current build directory in 30s!!!\n\n"
sleep 30

# link to pgo gen
rm -f build
mkdir -p ./build_pgo_gen
ln -sf ./build_pgo_gen build
check $?

# build gem5 with pgo instrumentation
CC=clang CXX=clang++ scons build/RISCV/gem5.opt -j $build_threads --gold-linker --pgo-prof
check $?

# run gem5 with pgo instrumentation
mkdir -p llvm-pgo
cd llvm-pgo

# run gem5 and let it to generate profile here
export LLVM_PROFILE_FILE=$(pwd)/default.profraw

$gem5_home/build/RISCV/gem5.opt \
 $gem5_home/configs/example/fs.py \
 --xiangshan-system --cpu-type=DerivO3CPU \
 --mem-size=8GB --caches --cacheline_size=64 \
 --l1i_size=64kB --l1i_assoc=8 --l1d_size=64kB --l1d_assoc=4 \
 --l1d-hwp-type=XSCompositePrefetcher --short-stride-thres=0 \
 --l2cache --l2_size=1MB --l2_assoc=8 \
 --l3cache --l3_size=16MB --l3_assoc=16 \
 --l1-to-l2-pf-hint --l2-hwp-type=WorkerPrefetcher \
 --l2-to-l3-pf-hint --l3-hwp-type=WorkerPrefetcher \
 --mem-type=DRAMsim3 \
 --dramsim3-ini=$gem5_home/ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini \
 --bp-type=DecoupledBPUWithFTB --enable-loop-predictor \
 --generic-rv-cpt=$GEM5_PGO_CPT \
 --gcpt-restorer=$GCPT_RESTORER \
 --warmup-insts-no-switch=1000000 --maxinsts=3000000
check $?

# TODO: only one profile file is generated, provide multiple
llvm-profdata merge default.profraw --output=gem5_single.profdata
check $?

cd ..

# link build to pgo use
rm -f build
mkdir -p ./build_pgo_use
ln -sf ./build_pgo_use build
check $?

# build gem5 with pgo instrumentation
CC=clang CXX=clang++ scons build/RISCV/gem5.opt -j $build_threads --gold-linker --pgo-use=llvm-pgo/gem5_single.profdata
check $?

printf "PGO build is done!\n"
