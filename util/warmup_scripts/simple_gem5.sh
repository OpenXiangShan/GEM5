# DO NOT track your local updates in this script!
set -x

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
export cpt_dir='/nfs-nvme/home/share/checkpoints_profiles/spec06_rv64gcb_o3_20m_gcc12-fpcontr-off/take_cpt'

# A tag to identify current batch run
export tag="an-example-to-run-gem5-with-composite-prefetcher"

export log_file='log.txt'

export ds=$(pwd)  # data storage. It is specific for BOSC machines, you can ignore it

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
    arch_db=${5:-0}

    cd $work_dir

    if test -f "completed"; then
        echo "Already completed; skip $cpt"
        return
    fi

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

    if [[ "$arch_db" -eq "0" ]]; then
        arch_db_args=
    else
        arch_db_args="--enable-arch-db --arch-db-file=mem_trace.db --arch-db-fromstart=True"
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
        echo "No debug flag set"
        debug_flag_args=
        start_end=
    fi
    # --debug-flags=CommitTrace \

    if [[ $crash_tick = -1 ]]; then
        start_end=
        debug_flag_args=
    fi

    # gdb -ex run --args \

    # Note 1: Use DecoupledBPUWithFTB to enable nanhu's decoupled frontend
    # Note 2: MUST use DRAMsim3, or performance is skewed
    # To enable DRAMSim3, follow ext/dramsim3/README
    # Note 3: By default use MultiPrefetcher (SMS + BOP) as L2 prefetcher
    # Note 4: Recommend to enable Difftest
    ######## Some additional args:
    $gem5 $debug_flag_args $start_end \
        $gem5_home/configs/example/fs.py \
        --xiangshan-system --cpu-type=DerivO3CPU \
        --mem-size=8GB \
        --caches --cacheline_size=64 \
        --l1i_size=64kB --l1i_assoc=8 \
        --l1d_size=64kB --l1d_assoc=8 \
        --l1d-hwp-type=XSCompositePrefetcher \
        --short-stride-thres=0 \
        --l2cache --l2_size=1MB --l2_assoc=8 \
        --l3cache --l3_size=16MB --l3_assoc=16 \
        --l1-to-l2-pf-hint \
        --l2-hwp-type=WorkerPrefetcher \
        --l2-to-l3-pf-hint \
        --l3-hwp-type=WorkerPrefetcher \
        --mem-type=DRAMsim3 \
        --dramsim3-ini=$gem5_home/ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini \
        --bp-type=DecoupledBPUWithFTB --enable-loop-predictor \
        --enable-difftest \
        $arch_db_args $cpt_option \
        --warmup-insts-no-switch=$dw_len \
        --maxinsts=$total_detail_len
    check $?

    # Here is a scratchpad for frequently used options

        # Enable complex stride component or SPP component in composite prefetcher
        # --l1d-enable-cplx \
        # --l1d-enable-spp \

        # Record arch db traces only after warmup
        #  --arch-db-fromstart=False 

        # Enable loop predictor and loop buffer
        # --enable-loop-predictor \
        # --enable-loop-buffer \

        # Employ an ideal L2 cache with nearly-perfetch hit rate and low-access latency
        # --mem-type=SimpleMemory \
        # --ideal-cache \

    # Debugging memory corruption or memory leak
    # valgrind -s --track-origins=yes --leak-check=full --show-leak-kinds=all --log-file=valgrind-out-2.txt --error-limit=no -v \

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

    run $gz $dw_M $total_M $work_dir 0 >$work_dir/$log_file 2>&1
}

function single_run() {
    # run /nfs-nvme/home/zhouyaoyang/projects/nexus-am/apps/cachetest_i/build/cachetest_i-riscv64-xs.bin
    task=$tag
    work_dir=$full_work_dir
    mkdir -p $work_dir

    # Note: If you are debugging with single run, following 3 variables are mandatory.
    # - It prints debug info in tick range: (crash_tick - 500 * capture_cycles, crash_tick + 500 * capture_cycles)
    # - If you want to print debug info from beginning, set crash_tick to 0, and set capture_cycles to a large number

    # crash_tick=$(( 0 ))
    # capture_cycles=$(( 250000 ))
    # debug_flags=CommitTrace  # If you unset debug_flags, no debug print will be there

    # If you unset debug_flags or crash_tick, no debug print will be there
    # Common used flags for debug/tuning
    # debug_flags=CommitTrace,IEW,Fetch,LSQUnit,Cache,Commit,IQ,LSQ,PageTableWalker,TLB,MSHR
    warmup_inst=$(( 20 * 10**6 ))
    max_inst=$(( 40 * 10**6 ))


    # debug_gz=/nfs-nvme/home/share/checkpoints_profiles/spec06_rv64gcb_o2_20m/take_cpt/mcf_191500000000_0.105600/0/_191500000000_.gz
    debug_gz=/nfs-nvme/home/share/checkpoints_profiles/spec06_rv64gcb_o2_20m/take_cpt/libquantum_1006500000000_0.149838/0/_1006500000000_.gz
    rm -f $work_dir/completed
    rm -f $work_dir/abort
    run $debug_gz $warmup_inst $max_inst $work_dir 1 > $work_dir/$log_file 2>&1
}

export -f check
export -f run
export -f single_run
export -f arg_wrapper
export -f prepare_env


function parallel_run() {
    # We use gnu parallel to control the parallelism.
    # If your server has 32 core and 64 SMT threads, we suggest to run with no more than 32 threads.
    export num_threads=30
    cat $workload_list | parallel -a - -j $num_threads arg_wrapper {}
}

# Usually, I use paralell run to benchmark, and use single_run to debug
parallel_run
# single_run
