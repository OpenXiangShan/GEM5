#!/usr/bin/env bash
# DO NOT track your local updates in this script!

function print_help() {
    printf "Usage:
    bash $0 <config_file_or_script> workload_list.lst checkpoint_top_dir task_tag [benchmark_filters] [extra_gem5_args]

Arguments:
    config_file_or_script:  Config file (*.py) or wrapper script (*.sh).
                            If relative, it is resolved relative to the repo root (gem5_home).
    workload_list.lst:      List of workloads to run
    checkpoint_top_dir:     Root directory for checkpoints
    task_tag:               Tag for this experiment
    benchmark_filters:      Optional comma-separated benchmark filters, case-insensitive.
                            Empty means no filter (run all workloads in workload_list).
    extra_gem5_args:        Optional extra arguments for gem5 (only for .py mode)

Examples:
    # Legacy mode (using .sh script)
    bash $0 kmh_v3_btb.sh workload.lst /cpt/dir my_exp

    # New mode (using .py config)
    bash $0 configs/example/idealkmhv3.py workload.lst /cpt/dir my_exp

    # New mode with benchmark filters
    bash $0 configs/example/idealkmhv3.py workload.lst /cpt/dir my_exp_subset \"mcf,gcc\"

    # New mode with extra args
    bash $0 configs/example/idealkmhv3.py workload.lst /cpt/dir my_exp_nosc \"\" \"--disable-mgsc\"

    # New mode with filters + extra args
    bash $0 configs/example/idealkmhv3.py workload.lst /cpt/dir my_exp_nosc \"mcf,gcc\" \"--disable-mgsc\"
\n"
    exit 1
}

if [[ -z "$4" ]]; then   # $1 is not set
    echo "Arguments not provided!"
    print_help
fi

set -x

script_dir=$(dirname -- "$( readlink -f -- "$0"; )")
source "$script_dir/common.sh"

# Detect if first parameter is a script (.sh) or config file (.py)
first_param="$1"
if [[ "$first_param" != /* ]] && [[ -f "$gem5_home/$first_param" ]]; then
    first_param="$gem5_home/$first_param"
fi
export first_param=$(realpath "$first_param")

if [[ "$first_param" == *.sh ]]; then
    # Legacy mode: using wrapper script
    export use_legacy_mode=true
    export arch_script="$first_param"
    export benchmark_filters="${5:-}"  # Optional 5th parameter in legacy mode
    echo "Legacy mode: using script $arch_script"
else
    # New mode: using config file directly
    export use_legacy_mode=false
    export config_file="$first_param"
    export benchmark_filters="${5:-}"  # Optional 5th parameter (new order)
    export extra_gem5_args="${6:-}"    # Optional 6th parameter (new order)
    # Backward compatibility: if only one optional arg and it looks like gem5 args,
    # keep treating it as extra_gem5_args.
    if [ "$#" -eq 5 ] && [[ "${5}" == -* ]]; then
        export extra_gem5_args="${5}"
        export benchmark_filters=""
    fi
    echo "Config mode: using $config_file"
    if [ -n "$extra_gem5_args" ]; then
        echo "Extra gem5 args: $extra_gem5_args"
    fi
fi
if [ -n "${benchmark_filters//[[:space:],]/}" ]; then
    echo "Benchmark filters: $benchmark_filters"
fi

# Note 1: workload list contains the workload name, checkpoint path, and parameters, looks like:
#       astar_biglakes_122060000000 astar_biglakes_122060000000_0.244818/0/ 0 0 20 20
#       bwaves_1003220000000 bwaves_1003220000000_0.036592/0/ 0 0 20 20
# Note 2: The meaning of fields:
# workload_name, checkpoint_path, skip insts(usually 0), functional_warmup insts(usually 0), detailed_warmup insts (usually 20), sample insts
# Note 3: you can write a script to generate such a list accordingly
export workload_list=`realpath $2`

# The checkpoint directory. We will find checkpoint_path in workload_list
# under this directory to get the checkpoint path.
export cpt_dir=`realpath $3`

export tag=$4

export log_file='log.txt'

export ds=$(pwd)  # data storage. It is specific for BOSC machines, you can ignore it
export full_work_dir=$ds/$tag # work dir wheter stats data stored
mkdir -p $full_work_dir
ln -sf $full_work_dir .  # optional, you can customize it yourself

export progress_status_file="$full_work_dir/progress.status"
export progress_lock_file="$full_work_dir/progress.lock"
exec 3>&1

declare -a filtered_workloads=()

function format_progress_line() {
    local progress_line
    printf -v progress_line \
        'issued[%d/%d], running[%d/%d], success[%d/%d], fail[%d/%d], skipped[%d/%d]' \
        "$1" "$6" "$2" "$6" "$3" "$6" "$4" "$6" "$5" "$6"

    if [ -t 3 ]; then
        printf '\033[1;97;44m[PARALLEL_PROGRESS]\033[0m \033[1;33m%s\033[0m\n' \
            "$progress_line"
    else
        printf '[PARALLEL_PROGRESS] %s\n' "$progress_line"
    fi
}

function count_selected_workloads() {
    if [ "${#filtered_workloads[@]}" -gt 0 ]; then
        echo "${#filtered_workloads[@]}"
    else
        grep -cve '^[[:space:]]*$' "$workload_list"
    fi
}

function init_progress() {
    local total="$1"
    printf '%s %s %s %s %s %s\n' "$total" 0 0 0 0 0 > "$progress_status_file"
    format_progress_line 0 0 0 0 0 "$total" >&3
}

function update_progress() {
    local action="$1"
    local xtrace_was_on=0
    local current_total=0
    local current_issued=0
    local current_running=0
    local current_success=0
    local current_fail=0
    local current_skipped=0

    if [[ $- == *x* ]]; then
        xtrace_was_on=1
        set +x
    fi

    (
        flock -x 200
        if ! read -r current_total current_issued current_running current_success current_fail current_skipped < "$progress_status_file"; then
            current_total="${total_workloads:-0}"
            current_issued=0
            current_running=0
            current_success=0
            current_fail=0
            current_skipped=0
        fi

        case "$action" in
            issue)
                ((current_issued++))
                ((current_running++))
                ;;
            success)
                ((current_running--))
                ((current_success++))
                ;;
            fail)
                ((current_running--))
                ((current_fail++))
                ;;
            skip)
                ((current_issued++))
                ((current_skipped++))
                ;;
            *)
                echo "Unknown progress action: $action" >&2
                exit 1
                ;;
        esac

        if [ "$current_running" -lt 0 ]; then
            current_running=0
        fi

        printf '%s %s %s %s %s %s\n' \
            "$current_total" \
            "$current_issued" \
            "$current_running" \
            "$current_success" \
            "$current_fail" \
            "$current_skipped" > "$progress_status_file"
        format_progress_line \
            "$current_issued" \
            "$current_running" \
            "$current_success" \
            "$current_fail" \
            "$current_skipped" \
            "$current_total" >&3
    ) 200>"$progress_lock_file"

    local rc=$?
    if [ "$xtrace_was_on" -eq 1 ]; then
        set -x
    fi
    return "$rc"
}

function apply_benchmark_filter() {
    if [ -z "${benchmark_filters//[[:space:],]/}" ]; then
        echo "No benchmark filter provided, run all workloads."
        return
    fi

    mapfile -t filtered_workloads < <(awk -v filters="$benchmark_filters" '
        BEGIN {
            n = split(filters, raw_filters, ",")
            valid = 0
            for (i = 1; i <= n; i++) {
                token = raw_filters[i]
                gsub(/^[[:space:]]+|[[:space:]]+$/, "", token)
                token = tolower(token)
                if (token != "") {
                    patterns[++valid] = token
                }
            }
        }
        /^[[:space:]]*$/ { next }
        {
            lower_line = tolower($0)
            for (i = 1; i <= valid; i++) {
                if (index(lower_line, patterns[i]) > 0) {
                    print $0
                    next
                }
            }
        }
    ' "$workload_list")

    local selected_count
    selected_count="${#filtered_workloads[@]}"
    if [ "$selected_count" -eq 0 ]; then
        echo "Error: benchmark_filters '$benchmark_filters' matched no workloads in '$workload_list'"
        exit 1
    fi

    echo "Applied benchmark filters: '$benchmark_filters' -> $selected_count workloads selected."
}

check() {
    if [ $1 -ne 0 ]; then
        echo FAIL
        rm -f running
        touch abort
        update_progress fail
        exit "$1"
    fi
}

trap cleanup SIGINT

function cleanup() {
    echo "Script interrupted, marking tasks as aborted..."
    find $full_work_dir -type f -name running -execdir bash -c 'rm -f running; touch abort' \;
    exit 1
}

function run() {
    set -x
    hostname

    cd $2  # work_dir

    if test -f "completed"; then
        echo "Already completed; skip $1"
        update_progress skip
        return
    fi


    rm -f abort
    rm -f completed

    touch running
    update_progress issue

    if [ "$use_legacy_mode" = true ]; then
        # Legacy mode: call wrapper script
        bash "$arch_script" "$1" # checkpoint
        check $?
    else
        # New mode: directly call gem5 with config file
        # config_file is already an absolute path from realpath
        $gem5 "$config_file" --generic-rv-cpt="$1" $extra_gem5_args
        check $?
    fi

    rm -f running
    touch completed
    update_progress success
}

function prepare_env() {
    set -x
    echo "prepare_env $@"
    all_args=("$@")
    task=${all_args[0]}
    task_path=${all_args[1]}
    # 同时匹配 gz zstd 后缀
    suffixes=("gz" "zstd")
    checkpoint=""
    for suffix in "${suffixes[@]}"; do
        checkpoint=$(find -L "$cpt_dir" -wholename "*${task_path}*.$suffix" | head -n 1)
        if [ -n "$checkpoint" ]; then
            break
        fi
    done
    echo $checkpoint

    export work_dir=$full_work_dir/$task
    echo $work_dir
    mkdir -p $work_dir
}

function arg_wrapper() {
    prepare_env $@

    all_args=("$@")
    args=(${all_args[0]})

    skip=${args[2]}
    fw=${args[3]}
    dw=${args[4]}
    sample=${args[5]}

    run $checkpoint $work_dir >$work_dir/$log_file 2>&1
}


export -f check
export -f run
export -f arg_wrapper
export -f prepare_env
export -f format_progress_line
export -f update_progress

# xsgem5_para_jobs define in docker compose
num_threads=${xsgem5_para_jobs:-63}
function parallel_run() {
    # We use gnu parallel to control the parallelism.
    # If your server has 32 core and 64 SMT threads, we suggest to run with no more than 32 threads.
    export total_workloads=$(count_selected_workloads)
    init_progress "$total_workloads"

    if [ "${#filtered_workloads[@]}" -gt 0 ]; then
        printf '%s\n' "${filtered_workloads[@]}" | parallel -a - -j $num_threads arg_wrapper {}
    else
        grep -v '^[[:space:]]*$' "$workload_list" | parallel -a - -j $num_threads arg_wrapper {}
    fi
}

apply_benchmark_filter
parallel_run
