#!/bin/bash

set -x
export trace_nemu=/nfs-nvme/home/zhouyaoyang/projects/func-warmup/trace_nemu
export future_trace_nemu=/nfs-nvme/home/zhouyaoyang/projects/func-warmup/future_trace_nemu
export rcpt_nemu=/nfs-nvme/home/zhouyaoyang/projects/func-warmup/rcpt_gen_nemu
export gem5_home=$n/projects/xs-gem5
export gem5=$gem5_home/build/RISCV/gem5.opt
export cpt_dir='/nfs-nvme/home/share/checkpoints_profiles/nemu_take_simpoint_cpt_06'
export cpt_restorer=/nfs-nvme/home/zhouyaoyang/projects/rv-linux/NEMU/resource/gcpt_restore/build/gcpt.bin
export raw_cpt='rcpt.bin'
export log_file='log.txt'
export desc_dir=$n/projects/BatchTaskTemplate/resources/simpoint_cpt_desc

export trace_gem5=/nfs-nvme/home/zhouyaoyang/projects/func-warmup/trace_gem5
export trace_filter=/nfs-nvme/home/zhouyaoyang/projects/xs-gem5/util/warmup_scripts/filter_lines.py
export dumper=/nfs-nvme/home/zhouyaoyang/projects/func-warmup/tl-dumper-1t
export REPLAYER=/nfs-nvme/home/zhouyaoyang/projects/sram-replayer/sram-replayer/replayer
export replay_gen=/nfs-nvme/home/zhouyaoyang/projects/func-warmup/replayer.sh
export xsemu=/nfs-nvme/home/zhouyaoyang/projects/func-warmup/emu-restore-8t
export ref_so=/nfs-nvme/home/zhouyaoyang/projects/xs-ref-nemu/build/riscv64-nemu-interpreter-so

export func_warmup=1
export finding_sat_point=0
# export workload_list='/simpoints06int_cover0.60_top4.lst.0'
export exp_id=exp5

export workload_list=/nfs-nvme/home/zhouyaoyang/projects/gem5_data_proc/results/ada-warmup.lst
# export config_tag='90M'
# export workload_list=$desc_dir/spec06_rv64gc_o2_50m__cover0.50_top1-gen-naive-func-90M-5M-5M.lst
# export workload_list=$desc_dir/spec06_rv64gc_o2_50m__cover0.50_top1-gen-dw-len-find-best-${config_tag}.lst  #single conf
# export workload_list=$desc_dir/spec06_rv64gc_o2_50m__cover0.50_top1-gen-dw-len-find-best.lst  # full configs
# export workload_list=$desc_dir/spec06_rv64gc_o2_50m__cover1.00_top100.lst.0

# export top_work_dir='bp-warmup-5M-sample-short-warmup'
export tag="ada-warmup-est-cycles"
export top_work_dir=$tag
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
    $rcpt_nemu $gz -I $icount -b -r $cpt_restorer --restore -D . --map-cpt $raw_cpt
    check $?
        # 2> ./err.log 1> ./out.log
        # --etrace-inst ./inst.pbuf --etrace-data ./data.pbuf \
}

function warmup_all_in_one() {
    set -x
    # args=($1)
    all_args=("$@")
    args=(${all_args[0]})

    slot=${all_args[1]}
    numa_node=$(($slot / 8))
    core_start=$(($slot * 8))
    core_end=$(($slot * 8 + 7))
    echo "--cpunodebind=${numa_node} --membind=${numa_node} -C $core_start-$core_end"
    sleep 1
    # exit 0

    k=1000
    M=$((1000 * $k))
    # M=k
    task=${args[0]}
    task_path=${args[1]}

    using_func_warmup=1

    if [ $finding_sat_point -ne 0 ] || [ ${args[3]} = '0' ];
    then
        using_func_warmup=0
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
        skip_len=$(($skip_M * $M + 1000))
    else
        # skip_M=${$args[2]}
        fw_M=${args[3]}
        dw_M=${args[4]}
        sample_M=${args[5]}

        unified_end_point_M=100
        skip_M=$(($unified_end_point_M - $fw_M - $dw_M - $sample_M))

        fw_len=$(($fw_M * $M))
        dw_len=$(($dw_M * $M + 10))
        sample_len=$(($sample_M * $M))
        total_detail_len=$(($dw_len + $sample_len))
        skip_len=$(($skip_M * $M + 1000))
    fi

    # exit 0

    work_dir=$top_work_dir/$task-${skip_M}-${fw_M}-${dw_M}-${sample_M}
    echo $work_dir
    # mkdir -p $work_dir
    cd $work_dir
    mkdir -p build

    # skip and create raw cpt with NEMU
    create_rcpt_from_gz $task $skip_len
    echo "skip_len: $skip_len, dw_len: $dw_len, sample_len: $sample_len, total_detail_len: $total_detail_len"

    # When comparing saturating point of GEM5 and XS, we use the same raw cpt
    if [ $finding_sat_point -ne 0 ];
    then
        $gem5 /nfs-nvme/home/zhouyaoyang/projects/xs-gem5/configs/example/fs.py \
        --caches --l2cache --enable-difftest --xiangshan --cpu-type=DerivO3CPU \
        --mem-type=DDR3_1600_8x8 --mem-size=8GB --cacheline_size=64 --l1i_size=64kB \
        --l1i_assoc=8 --l1d_size=64kB --l1d_assoc=8 --l2_size=1MB --l2_assoc=8 \
        --raw-cpt --generic-rv-cpt=$raw_cpt \
        --gcpt-restorer=/nfs-nvme/home/zhouyaoyang/projects/gem5-ref-sd-nemu/resource/gcpt_restore/build/gcpt.bin \
        --l3cache --l3_size=8MB --l3_assoc=8 \
        --bp-type=LTAGE --indirect-bp-type=ITTAGE \
        --warmup-insts-no-switch=$dw_len \
        --maxinsts=$total_detail_len
        # -s 1 --warmup-insts=$fw_len \

        check $?
    fi


    if [ $using_func_warmup = 1 ];
    then
        date +%Y-%m-%d--%H:%M:%S
        echo "Generating traces with NEMU"
        export LD_PRELOAD=/nfs-nvme/home/share/debug/zhouyaoyang/libz.so.1.2.11.zlib-ng
        # with map-as-outcpt to advance PC
        $trace_nemu $raw_cpt -I $fw_len -b --restore --map-img-as-outcpt \
            --etrace-inst ./inst.pbuf --etrace-data ./data.pbuf -D .
        check $?

        date +%Y-%m-%d--%H:%M:%S
        echo "Replay traces with GEM5"
        $trace_gem5 /nfs-nvme/home/zhouyaoyang/projects/ff-reshape/configs/example/etrace_replay.py \
            --cpu-type=TraceCPU --caches \
            --inst-trace-file=./inst.pbuf --data-trace-file=./data.pbuf \
            --mem-type=SimpleMemory --mem-size=8GB --l2cache --l3cache --dump-caches
        check $?

        date +%Y-%m-%d--%H:%M:%S
        echo "Get used cache lines in future"
        $future_trace_nemu $raw_cpt -I $total_detail_len -b --restore \
            --etrace-inst ./useless_inst.pbuf --etrace-data ./useless_data.pbuf -D .
        check $?

        date +%Y-%m-%d--%H:%M:%S
        python3 $trace_filter

    else
        # create empty trace
        date +%Y-%m-%d--%H:%M:%S
        echo 'Creating dummy trace files'
        echo '80001f00' > system.cpu.dcache.tags.hex.txt
        touch system.l2.tags.hex.txt
        touch system.l3.tags.hex.txt
        touch system.cpu.icache.tags.hex.txt
    fi

    # load dummy trace with tl-tester
    date +%Y-%m-%d--%H:%M:%S
    echo "Replay traces with XS cache subsystem"
    rm -rf ./build/trace && mkdir -p ./build/trace

    set +x
    source $replay_gen
    check $?
    set -x

    $dumper -i $raw_cpt
    check $?

    date +%Y-%m-%d--%H:%M:%S

    echo "numa node: $numa_node"
    numactl --cpunodebind=${numa_node} --membind=${numa_node} -C $core_start-$core_end -- $xsemu \
        -W $dw_len -I $total_detail_len -i $raw_cpt --diff $ref_so -- +LOAD_SRAM > emu_out.txt 2> emu_err.txt
    check $?

    date +%Y-%m-%d--%H:%M:%S
    touch completed

    set +x
}

function warmup_wrapper() {
    all_args=("$@")
    args=(${all_args[0]})

    k=1000
    M=$((1000 * $k))
    # M=k
    task=${args[0]}
    task_path=${args[1]}

    if [ $finding_sat_point -ne 0 ];
    then
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
        skip_len=$(($skip_M * $M + 1000))
    else
        # skip_M=${$args[2]}
        fw_M=${args[3]}
        dw_M=${args[4]}
        sample_M=${args[5]}

        unified_end_point_M=100
        skip_M=$(($unified_end_point_M - $fw_M - $dw_M - $sample_M))

        fw_len=$(($fw_M * $M))
        dw_len=$(($dw_M * $M + 10))
        sample_len=$(($sample_M * $M))
        total_detail_len=$(($dw_len + $sample_len))
        skip_len=$(($skip_M * $M + 1000))
    fi


    work_dir=$top_work_dir/$task-${skip_M}-${fw_M}-${dw_M}-${sample_M}
    echo $work_dir
    mkdir -p $work_dir

    warmup_all_in_one "$@" > $work_dir/$log_file 2>&1
}

export -f check
export -f create_rcpt_from_gz
export -f warmup_all_in_one
export -f warmup_wrapper

cat $workload_list | \
    parallel --arg-file - -j 14 --jl ${tag}_job_log.txt warmup_wrapper "{}" {%} \$PARALLEL_JOBSLOT

# arg=("perlbench_checkspam_697850000000" "perlbench_checkspam_697850000000_0.142824/0/" "0" "0" "50" "50")
# warmup_wrapper "${arg[@]}"
