export gem5_home=$n/projects/xs-gem5
export gem5=$gem5_home/build/RISCV/gem5.opt
export desc_dir=$n/projects/BatchTaskTemplate/resources/simpoint_cpt_desc
# export workload_list=$desc_dir/spec06_rv64gcb_o2_20m__cover1.00_top100-normal-0-0-20-20.lst
# export workload_list=$desc_dir/bwaves_rv64gcb_o2_20m__cover1.00_top100-normal-0-0-20-20.lst
export workload_list=$desc_dir/Gems_rv64gcb_o2_20m__cover1.00_top100-normal-0-0-20-20.lst
export cpt_dir='/nfs-nvme/home/share/checkpoints_profiles/spec06_rv64gcb_o2_20m'

# export tag="perf_after_calibrate_dramsim"
export tag="no_prefetch_plru"

export log_file='log.txt'
export ds=$dz  # data storage, dz or lz
export top_work_dir=$tag
mkdir -p $ds/exec-storage/$top_work_dir
ln -sf $ds/exec-storage/$top_work_dir .

check() {
    if [ $1 -ne 0 ]; then
        echo FAIL
        touch abort
        exit
    fi
}

function run() {
    set -x
    cpt=$1
    dw_len=${2:-20000000}
    # dw_len=${2:-1525605}
    total_detail_len=${3:-40000000}
    if [[ -n "$4" ]]; then
        work_dir=$4
    else
        work_dir=$PWD
    fi

    cd $work_dir
    rm -f abort
    rm -f completed

    cpt_name=$(basename -- "$cpt")
    extension="${cpt_name##*.}"
    cpt_option="--generic-rv-cpt=$cpt --gcpt-restorer=/nfs-nvme/home/zhouyaoyang/projects/gem5-ref-sd-nemu/resource/gcpt_restore/build/gcpt.bin "
    if [ $extension != "gz" ]; then
        cpt_option="--generic-rv-cpt=$cpt --raw-cpt"
    fi

    if [[ -z "$crash_tick" ]]; then
        crash_tick=-1
    fi
    if [[ -z "$capture_cycles" ]]; then
        capture_cycles=30000
    fi
    # start=$(($crash_tick - 500*$capture_cycles))
    # start=$(($start>0 ? $start : 0))
    start=$crash_tick
    end=$(($crash_tick + 500*$capture_cycles))
    start_end=" --debug-start=$start --debug-end=$end "
    if [[ -n "$debug_flags" ]]; then
        debug_flag_args=" --debug-flag=$debug_flags "
    else
        debug_flag_args=
        start_end=
    fi
    # --debug-flags=CommitTrace \

    if [ $crash_tick = -1 ]; then
        start_end=
        debug_flag_args=
    fi

    $gem5 $debug_flag_args $start_end \
        /nfs-nvme/home/zhouyaoyang/projects/xs-gem5/configs/example/fs.py \
        --caches --l2cache --xiangshan --cpu-type=DerivO3CPU \
        --mem-type=DRAMsim3 --dramsim3-ini=$gem5_home/xiangshan_DDR4_8Gb_x8_2400.ini \
        --mem-size=8GB --cacheline_size=64 \
        --l1i_size=64kB --l1i_assoc=8 \
        --l1d_size=64kB --l1d_assoc=8 \
        --l2_size=1MB --l2_assoc=8 \
        --l3cache --l3_size=8MB --l3_assoc=8 \
        --bp-type=LTAGE --indirect-bp-type=ITTAGE \
        --enable-difftest \
        $cpt_option \
        --warmup-insts-no-switch=$dw_len \
        --maxinsts=$total_detail_len
    check $?
        # --l1d-hwp-type=StridePrefetcher \
        # --l2-hwp-type=StridePrefetcher \

    touch completed
}

function prepare_env() {
    set -x
    echo "prepare_env $@"
    all_args=("$@")
    task=${all_args[0]}
    task_path=${all_args[1]}
    gz=$(find -L $cpt_dir -wholename "*${task_path}*gz" | head -n 1)
    echo $gz
    work_dir=$top_work_dir/$task
    echo $work_dir
    mkdir -p $work_dir
}

function arg_wrapper() {
    prepare_env $@

    all_args=("$@")
    args=(${all_args[0]})

    k=1000
    M=$((1000 * $k))

    skip=${args[2]}
    fw=${args[3]}
    dw=${args[4]}
    sample=${args[5]}

    total_M=$(( ($dw + $sample)*$M ))
    dw_M=$(( $dw*$M ))

    run $gz $dw_M $total_M $work_dir >$work_dir/$log_file 2>&1
}

function single_run() {
    # run /nfs-nvme/home/zhouyaoyang/projects/nexus-am/apps/cachetest_i/build/cachetest_i-riscv64-xs.bin
    top_work_dir=single_top
    task='zeu_382'
    work_dir=$top_work_dir/$task
    mkdir -p $work_dir

    crash_tick=$(( 25434715 * 500 ))
    capture_cycles=50000000
    debug_flags=
    run /nfs-nvme/home/share/checkpoints_profiles/spec06_rv64gcb_o2_20m/take_cpt/zeusmp_382440000000_0.005552/0/_382440000000_.gz \
        20000000 40000000 $work_dir > $work_dir/$log_file 2>&1
}

export -f check
export -f run
export -f single_run
export -f arg_wrapper
export -f prepare_env


function parallel_run() {
    cat $workload_list | parallel -a - -j 70 arg_wrapper {}
}

# parallel_run
single_run
