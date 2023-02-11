# DO NOT track your local updates in this script!

export gem5_home=$n/projects/xs-gem5 # The root of GEM5 project
export gem5=$gem5_home/build/RISCV/gem5.opt # GEM5 executable


# Note 1: workload list contains the workload name, checkpoint path, and parameters, looks like:
#       astar_biglakes_122060000000 astar_biglakes_122060000000_0.244818/0/ 0 0 20 20
#       bwaves_1003220000000 bwaves_1003220000000_0.036592/0/ 0 0 20 20
# Note 2: The meaning of fields:
# workload_name, checkpoint_path, skip insts(usually 0), functional_warmup insts(usually 0), detailed_warmup insts (usually 20), sample insts
# Note 3: you can write a script to generate such a list accordingly
export desc_dir=$n/projects/BatchTaskTemplate/resources/simpoint_cpt_desc
export workload_list=$desc_dir/spec06_rv64gcb_o2_20m__cover1.00_top100-normal-0-0-20-20.lst


# The checkpoint directory. We will find checkpoint_path in workload_list
# under this directory to get the checkpoint path.
export cpt_dir='/nfs-nvme/home/share/checkpoints_profiles/spec06_rv64gcb_o2_20m'

# A tag to identify current batch run
export tag="an-example-tag-like-test-perf-for-ptw"

export log_file='log.txt'

export ds=$lz  # data storage. It is specific for BOSC machines, you can ignore it
export top_work_dir=$tag
export full_work_dir=$ds/exec-storage/$top_work_dir  # work dir wheter stats data stored

mkdir -p $full_work_dir
ln -sf $full_work_dir .  # optional, you can customize it yourself

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

    # replace the path of gcpt.bin with your gcpt restorer
    # gcpt restorer can be found in https://github.com/OpenXiangShan/NEMU/tree/gem5-ref-main/resource/gcpt_restore
    # Please use gem5-ref-main branch
    cpt_option="--generic-rv-cpt=$cpt --gcpt-restorer=/nfs-nvme/home/zhouyaoyang/projects/gem5-ref-sd-nemu/resource/gcpt_restore/build/gcpt.bin "

    # You can also pass a baremetal bin here
    if [ $extension != "gz" ]; then
        cpt_option="--generic-rv-cpt=$cpt --raw-cpt"
    fi

    if [[ -z "$crash_tick" ]]; then
        crash_tick=-1
    fi
    if [[ -z "$capture_cycles" ]]; then
        capture_cycles=30000
    fi
    start=$(($crash_tick - 500*$capture_cycles))
    # start=$crash_tick
    start=$(($start>0 ? $start : 0))
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

    # Note 1: Use DecoupledBPUWithFTB to enable nanhu's decoupled frontend
    # Note 2: MUST use DRAMsim3, or performance is skewed
    # To enable DRAMSim3, follow ext/dramsim3/README
    # Note 3: By default use SMS or Berti as L2 prefetcher
    # Note 4: Recommend to enable Difftest
    $gem5 $debug_flag_args $start_end \
        $gem5_home/configs/example/fs.py \
        --caches --l2cache --xiangshan-system --cpu-type=DerivO3CPU \
        --mem-type=DRAMsim3 --dramsim3-ini=$gem5_home/xiangshan_DDR4_8Gb_x8_2400.ini \
        --mem-size=8GB --cacheline_size=64 \
        --l1i_size=64kB --l1i_assoc=8 \
        --l1d_size=64kB --l1d_assoc=8 \
        --l2_size=1MB --l2_assoc=8 --l2-hwp-type=SMSPrefetcher\
        --l3cache --l3_size=6MB --l3_assoc=6 \
        --bp-type=DecoupledBPUWithFTB \
        --enable-difftest \
        $cpt_option \
        --warmup-insts-no-switch=$dw_len \
        --maxinsts=$total_detail_len
    check $?

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
    task='GemsFDTD_12966'
    work_dir=$top_work_dir/$task
    mkdir -p $work_dir

    capture_cycles=$(( 250000 ))
    debug_flags=CommitTrace  # If you unset debug_flags, no debug print will be there
    # Common used flags for debug/tuning
    # debug_flags=CommitTrace,IEW,Fetch,LSQUnit,Cache,Commit,IQ,LSQ,PageTableWalker,TLB,MSHR
    warmup_inst=$(( 20 * 10**6 ))
    max_inst=$(( 40 * 10**6 ))
    run /nfs-nvme/home/share/checkpoints_profiles/spec06_rv64gcb_o2_20m/take_cpt/GemsFDTD_1296600000000_0.243608/0/_1296600000000_.gz \
        $warmup_inst $max_inst $work_dir > $work_dir/$log_file 2>&1
}

export -f check
export -f run
export -f single_run
export -f arg_wrapper
export -f prepare_env


function parallel_run() {
    # We use gnu parallel to control the parallelism.
    # If your server has 32 core and 64 SMT threads, we suggest to run with no more than 32 threads.
    export num_threads=90
    cat $workload_list | parallel -a - -j $num_threads arg_wrapper {}
}

# Usually, I use paralell run to benchmark, and use single_run to debug
parallel_run
# single_run
