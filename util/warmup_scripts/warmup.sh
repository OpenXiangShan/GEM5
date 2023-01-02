#!/bin/bash

set -x
export nemu=/nfs-nvme/home/zhouyaoyang/projects/rv-linux/NEMU/build/riscv64-nemu-interpreter
export gem5_home=$n/projects/xs-gem5
export gem5=$gem5_home/build/RISCV/gem5.opt
export cpt_dir='/nfs-nvme/home/share/checkpoints_profiles/nemu_take_simpoint_cpt_06'
export cpt_restorer=/nfs-nvme/home/zhouyaoyang/projects/rv-linux/NEMU/resource/gcpt_restore/build/gcpt.bin
export raw_cpt='rcpt.bin'
export log_file='log.txt'
export desc_dir=$n/projects/BatchTaskTemplate/resources/simpoint_cpt_desc

export cache_gem5=/nfs-nvme/home/share/zyy/testpoint/gem5.opt
export dumper=/nfs-nvme/home/zhouyaoyang/projects/func-warmup/tl-dumper-1t
export REPLAYER=/nfs-nvme/home/zhouyaoyang/projects/sram-replayer/sram-replayer/replayer
export replay_gen=/nfs-nvme/home/zhouyaoyang/projects/func-warmup/replayer.sh
export xsemu=/nfs-nvme/home/zhouyaoyang/projects/func-warmup/emu-restore-8t
export ref_so=/nfs-nvme/home/zhouyaoyang/projects/xs-ref-nemu/build/riscv64-nemu-interpreter-so

# export workload_list='/simpoints06int_cover0.60_top4.lst.0'
export exp_id=exp5

export config_tag='95M'
export workload_list=$desc_dir/spec06_rv64gc_o2_50m__cover0.50_top1-gen-dw-len-find-best-${config_tag}.lst  #single conf
# export workload_list=$desc_dir/spec06_rv64gc_o2_50m__cover0.50_top1-gen-dw-len-find-best.lst  # full configs
# export workload_list=$desc_dir/spec06_rv64gc_o2_50m__cover1.00_top100.lst.0

# export top_work_dir='bp-warmup-5M-sample-short-warmup'
export top_work_dir="compare-warmup-xs-8t-vs-gem5-${config_tag}"
export ds=$dz  # data storage
mkdir -p $ds/exec-storage/$top_work_dir
ln -sf $ds/exec-storage/$top_work_dir .

check() {
    if [ $1 -ne 0 ]; then
        echo FAIL
        touch abort
        exit
    fi
}

create_rcpt_from_gz() {
    export LD_PRELOAD=/nfs-nvme/home/share/debug/zhouyaoyang/libz.so.1.2.11.zlib-ng
    cpt_sub_path=$1
    echo $cpt_dir/$cpt_sub_path
    gz=$(find -L $cpt_dir -wholename "*$cpt_sub_path*gz" | head -n 1)
    echo $gz
    icount=$2
    $nemu $gz -I $icount -b -r $cpt_restorer --restore -D . --map-cpt $raw_cpt
    check $?
        # 2> ./err.log 1> ./out.log
        # --etrace-inst ./inst.pbuf --etrace-data ./data.pbuf \
}

function warmup_length_test() {
    set -x
    # args=($1)
    all_args=("$@")
    args=(${all_args[0]})

    slot=${all_args[1]}
    numa_node=$(($slot / 8))
    core_start=$(($slot * 8))
    core_end=$(($slot * 8 + 7))
    echo "--cpunodebind=${numa_node} --membind=${numa_node} -C $core_start-$core_end" > $log_file 2>&1
    sleep 1

    k=1000
    M=$((1000 * $k))
    # M=k
    task=${args[0]}
    task_path=${args[1]}

    fw_M=0
    # skip_M=$(($args[2] + $args[3]))

    dw_M=${args[4]}
    sample_M=${args[5]}

    unified_end_point_M=100
    skip_M=$(($unified_end_point_M - $fw_M - $dw_M - $sample_M))

    fw_len=0
    dw_len=$(($dw_M * $M + 10))
    sample_len=$(($sample_M * $M))
    total_detail_len=$(($dw_len + $sample_len))
    skip_len=$(($skip_M * $M + 800))

    # exit 0

    work_dir=$top_work_dir/$task-${skip_M}-${fw_M}-${dw_M}-${sample_M}
    echo $work_dir
    mkdir -p $work_dir
    cd $work_dir
    mkdir -p build

    # skip and create raw cpt with NEMU
    create_rcpt_from_gz $task $skip_len >> $log_file 2>&1
    echo "skip_len: $skip_len, dw_len: $dw_len, sample_len: $sample_len, total_detail_len: $total_detail_len" \
        >> $log_file 2>&1

    $gem5 /nfs-nvme/home/zhouyaoyang/projects/xs-gem5/configs/example/fs.py \
    --caches --l2cache --enable-difftest --xiangshan --cpu-type=DerivO3CPU \
    --mem-type=DDR3_1600_8x8 --mem-size=8GB --cacheline_size=64 --l1i_size=64kB \
    --l1i_assoc=8 --l1d_size=64kB --l1d_assoc=8 --l2_size=1MB --l2_assoc=8 \
    --raw-cpt --generic-rv-cpt=$raw_cpt \
    --gcpt-restorer=/nfs-nvme/home/zhouyaoyang/projects/gem5-ref-sd-nemu/resource/gcpt_restore/build/gcpt.bin \
    --l3cache --l3_size=8MB --l3_assoc=8 \
    --bp-type=LTAGE --indirect-bp-type=ITTAGE \
    --warmup-insts-no-switch=$dw_len \
    --maxinsts=$total_detail_len >> $log_file 2>&1
    # -s 1 --warmup-insts=$fw_len \

    check $? >> $log_file 2>&1

    # create empty trace
    date +%Y-%m-%d--%H:%M:%S >> $log_file
    echo 'Creating dummy trace files' >> $log_file
    echo '80001f00' > system.cpu.dcache.tags.hex.txt
    touch system.l2.tags.hex.txt
    touch system.l3.tags.hex.txt
    touch system.cpu.icache.tags.hex.txt

    # load dummy trace with tl-tester
    date +%Y-%m-%d--%H:%M:%S >> $log_file
    echo "Replay traces with XS cache subsystem" >> $log_file
    rm -rf ./build/trace && mkdir -p ./build/trace

    set +x
    source $replay_gen
    check $? >> $log_file 2>&1
    set -x

    $dumper -i $raw_cpt >> $log_file 2>&1
    check $? >> $log_file 2>&1


    echo "numa node: $numa_node"
    numactl --cpunodebind=${numa_node} --membind=${numa_node} -C $core_start-$core_end -- $xsemu \
        -W $dw_len -I $total_detail_len -i $raw_cpt --diff $ref_so -- +LOAD_SRAM > emu_out.txt 2> emu_err.txt
    check $? >> $log_file 2>&1

    touch completed

    set +x
}

export -f check
export -f create_rcpt_from_gz
export -f warmup_length_test

cat $workload_list | \
    parallel --arg-file - -j 14 --jl job_log.txt  warmup_length_test "{}" {%} \$PARALLEL_JOBSLOT

# arg=("perlbench_checkspam_697850000000" "perlbench_checkspam_697850000000_0.142824/0/" "0" "0" "50" "50")
# warmup_length_test "${arg[@]}"
