set -x

gem5_home=$(pwd)
gcpt_restore_path=/nfs/home/share/gem5_shared_tools/normal-gcb-restorer.bin
ref_so_path=/nfs-nvme/home/share/zhenhao/ref-h/build/riscv64-nemu-interpreter-so
test_cpt=/nfs/home/share/jiaxiaoyu/simpoint_checkpoint_archive/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/checkpoint-0-0-0/GemsFDTD/30385/_30385_0.268180_.gz

export NEMU_HOME=$ref_so_path  # dummy

mkdir -p $gem5_home/valgrind-test
cd $gem5_home/valgrind-test

valgrind -s --track-origins=yes --log-file=valgrind-out.txt --error-limit=no \
    --suppressions=$gem5_home/util/valgrind-suppressions \
    $gem5_home/build/RISCV/gem5.debug $gem5_home/configs/example/kmhv3.py \
    --enable-difftest --difftest-ref-so=$ref_so_path \
    --generic-rv-cpt=$test_cpt \
    --gcpt-restorer=$gcpt_restore_path \
    --warmup-insts-no-switch=40000 --maxinsts=80000

python3 $gem5_home/util/memory_check/check-memory-error.py $gem5_home/valgrind-test/valgrind-out.txt
