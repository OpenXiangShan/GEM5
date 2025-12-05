#!/usr/bin/env bash

# Trace-only parallel driver for XiangShan + trace inputs (ChampSim / CBP2025).
#
# 目标：
#   - 保持公用 util/xs_scripts/parallel_sim.sh 原样不动；
#   - 在本脚本中复现之前为 trace 实验量身定制的增强能力：
#       1) 支持通过 XSGEM5_WORK_ROOT 指定批跑工作根目录；
#       2) 支持 .gz/.zstd/.xz 后缀的 checkpoint/trace 文件匹配；
#       3) 从 workload 列表中解析 skip/fw/dw/sample，将其映射为
#          XS_WARMUP_INSTS_NO_SWITCH / XS_MAX_INSTS 传给 arch_script
#          （例如 run_trace_champsim.sh），而不改动公共脚本。
#   - 通过环境变量 TRACE_FORMAT 可传递格式给 arch_script。
#
# 用法（与 parallel_sim.sh 保持一致）：
#   bash util/xs_scripts/trace/parallel_trace_sim.sh \\
#       arch_script.sh workload_list.lst checkpoint_top_dir task_tag

set -euo pipefail

function print_help() {
    printf "Usage:
    bash %s arch_script.sh workload_list.lst checkpoint_top_dir task_tag\n" "$0"
    exit 1
}

if [[ -z "${4:-}" ]]; then
    echo "Arguments not provided!" >&2
    print_help
fi

set -x

TRACE_FORMAT=${TRACE_FORMAT:-champsim}
export TRACE_FORMAT

export arch_script=$(realpath "$1")
export workload_list=$(realpath "$2")
export cpt_dir=$(realpath "$3")
export tag="$4"

export log_file='log.txt'

# 调用者所在目录（通常是实验根目录）。
CALLER_CWD=$(pwd)

# 批跑工作根目录：默认使用调用目录，也可通过环境变量覆盖。
export ds=${XSGEM5_WORK_ROOT:-${CALLER_CWD}}  # data storage root
export full_work_dir="${ds}/${tag}"          # per-tag work dir root
mkdir -p "${full_work_dir}"

# 在调用目录下创建一个指向 FULL_WORK_DIR 的符号链接，便于查看结果。
ln -sfn "${full_work_dir}" "${CALLER_CWD}/${tag}"

check() {
    if [[ $1 -ne 0 ]]; then
        echo FAIL
        rm -f running
        touch abort
        exit 1
    fi
}

trap cleanup SIGINT

function cleanup() {
    echo "Script interrupted, marking tasks as aborted..."
    find "${full_work_dir}" -type f -name running -execdir bash -c 'rm -f running; touch abort' \;
    exit 1
}

function run() {
    set -x
    hostname

    cd "$2"  # work_dir

    if [[ -f "completed" ]]; then
        echo "Already completed; skip $1"
        return
    fi

    rm -f abort completed
    touch running

    bash "${arch_script}" "$1"  # checkpoint/trace path
    check $?

    rm -f running
    touch completed
}

function prepare_env() {
    set -x
    echo "prepare_env $@"

    # workload 行字段：
    #   name path/to/trace_prefix skip fw dw sample
    # 这里只使用 name 与 path，其余字段在 arg_wrapper 中处理。
    local task="$1"
    local task_path="$2"

    # 同时匹配 gz zstd xz 以及未压缩的 champsimtrace 后缀
    local suffixes=("gz" "zstd" "xz" "champsimtrace")
    checkpoint=""
    for suffix in "${suffixes[@]}"; do
        checkpoint=$(find -L "${cpt_dir}" -wholename "*${task_path}*.${suffix}" | head -n 1)
        if [[ -n "${checkpoint}" ]]; then
            break
        fi
    done
    echo "${checkpoint}"

    export work_dir="${full_work_dir}/${task}"
    echo "${work_dir}"
    mkdir -p "${work_dir}"
}

function arg_wrapper() {
    # GNU parallel 将整行作为单个参数传入，这里使用 read 将其
    # 拆分为字段，避免依赖 set/位置参数行为。
    local line="$1"
    local task task_path skip fw dw sample
    IFS=' ' read -r task task_path skip fw dw sample <<< "${line}"

    prepare_env "${task}" "${task_path}" "${skip}" "${fw}" "${dw}" "${sample}"

    # workload 字段：name path skip fw dw sample
    skip=${skip:-0}
    fw=${fw:-0}
    dw=${dw:-0}
    sample=${sample:-0}

    # 将 workload 中的 warmup/sample 参数传递给 arch_script：
    # - warmup:  使用 functional_warmup + detailed_warmup
    # - sample:  使用 sample 字段
    # 在 run_trace_champsim.sh 中，这两者分别映射到：
    #   XS_WARMUP_INSTS_NO_SWITCH -> --warmup-insts-no-switch
    #   XS_MAX_INSTS             -> --maxinsts
    local warmup=$((fw + dw))
    local total=$((warmup + sample))

    XS_MAX_INSTS="${total}" XS_WARMUP_INSTS_NO_SWITCH="${warmup}" \
        run "${checkpoint}" "${work_dir}" >"${work_dir}/${log_file}" 2>&1
}

export -f check
export -f run
export -f arg_wrapper
export -f prepare_env

# xsgem5_para_jobs define in docker compose
num_threads=${xsgem5_para_jobs:-63}

function parallel_run() {
    # 使用 gnu parallel 控制并行度。
    # 每行 workload 作为单个参数传递给 arg_wrapper（用 "{}" 保持整行不被 shell 拆分）。
    parallel -a "${workload_list}" -j "${num_threads}" arg_wrapper "{}"
}

parallel_run
