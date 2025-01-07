set -x

gem5_home=$(pwd)
ref_so_path=/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so
export GCBV_REF_SO=$ref_so_path

raw_cpt=/nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin
# ideal config, $1 means ideal-kmhv3
IDEAL_CONFIG=""
if [ "$1" ]; then
    IDEAL_CONFIG="--ideal-kmhv3"
fi
echo "IDEAL_CONFIG: $IDEAL_CONFIG"

mkdir -p $gem5_home/valgrind-test
cd $gem5_home/valgrind-test

valgrind -s --track-origins=yes --log-file=valgrind-out.txt --error-limit=no \
    --suppressions=$gem5_home/util/valgrind-suppressions \
    $gem5_home/build/RISCV/gem5.debug \
    $gem5_home/configs/example/xiangshan.py \
    $IDEAL_CONFIG \
    --raw-cpt \
    --generic-rv-cpt=$raw_cpt

# if $? != 0, exit
if [ $? -ne 0 ]; then
    echo "Valgrind test failed"
    exit 1
fi

python3 $gem5_home/util/memory_check/check-memory-error.py $gem5_home/valgrind-test/valgrind-out.txt

