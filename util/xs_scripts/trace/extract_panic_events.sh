#!/usr/bin/env bash

# Extract gem5 events around PC mismatch panics for debug runs.
#
# Usage:
#   bash extract_panic_events.sh WORK_ROOT [WINDOW]
#
# WORK_ROOT 是 parallel_sim.sh 的结果根目录，例如：
#   /nfs/home/goulingrui/expri_results/gem5_trace/trace_ipc1_50M_50M
#
# WINDOW 为从 abort_tick 向前追溯的 tick 数（默认 1000000），
# 脚本会：
#   1) 在 WORK_ROOT 下递归查找所有包含 debug/log.txt 的目录；
#   2) 对于包含 "Trace stream PC mismatch" 的 debug/log.txt，解析
#      abort_tick（Program aborted at tick）并计算 from_tick = abort_tick-WINDOW；
#   3) 调用 $GEM5_HOME/util/trace/extract_trace_events.py 生成
#        debug/events_panic.txt；
#   4) 调用 util/align_trace_bind_events.py 生成
#        debug/bind_align_panic.tsv。

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 WORK_ROOT [WINDOW]" >&2
    exit 1
fi

WORK_ROOT=$1
WINDOW=${2:-1000000}

if [[ ! -d "${WORK_ROOT}" ]]; then
    echo "Error: WORK_ROOT is not a directory: ${WORK_ROOT}" >&2
    exit 1
fi

if ! [[ "${WINDOW}" =~ ^[0-9]+$ ]]; then
    echo "Error: WINDOW must be a non-negative integer" >&2
    exit 1
fi

script_dir=$(dirname -- "$( readlink -f -- "$0"; )")
GEM5_HOME=${GEM5_HOME:-$(readlink -f "${script_dir}/../..")}

EXTRACT_SCRIPT="${GEM5_HOME}/util/trace/extract_trace_events.py"
ALIGN_SCRIPT="${GEM5_HOME}/util/trace/align_trace_bind_events.py"

if [[ ! -f "${EXTRACT_SCRIPT}" ]]; then
    echo "Error: extract_trace_events.py not found at ${EXTRACT_SCRIPT}" >&2
    exit 1
fi
if [[ ! -f "${ALIGN_SCRIPT}" ]]; then
    echo "Error: align_trace_bind_events.py not found at ${ALIGN_SCRIPT}" >&2
    exit 1
fi

WORK_ROOT=$(readlink -f "${WORK_ROOT}")

echo "Using GEM5_HOME=${GEM5_HOME}" >&2
echo "Scanning for PC mismatch panics under ${WORK_ROOT}, window=${WINDOW}" >&2

# 查找所有 debug/log.txt
find "${WORK_ROOT}" -type d -name debug | while read -r dbg; do
    log_file="${dbg}/log.txt"
    if [[ ! -f "${log_file}" ]]; then
        continue
    fi

    # 只处理包含 PC mismatch panic 的 debug 日志
    panic_line=$(grep -m1 'Trace stream PC mismatch' "${log_file}" || true)
    if [[ -z "${panic_line}" ]]; then
        continue
    fi

    # abort_tick 从 Program aborted at tick 获取
    abort_line=$(grep -m1 'Program aborted at tick' "${log_file}" || true)
    if [[ -z "${abort_line}" ]]; then
        echo "[WARN] No 'Program aborted at tick' in ${log_file}, skip" >&2
        continue
    fi
    abort_tick=$(echo "${abort_line}" | awk '{for (i=1;i<=NF;i++) if ($i=="tick") {print $(i+1); exit}}')
    if ! [[ "${abort_tick}" =~ ^[0-9]+$ ]]; then
        echo "[WARN] Invalid abort_tick '${abort_tick}' in ${log_file}, skip" >&2
        continue
    fi

    from_tick=$(( abort_tick - WINDOW ))
    if (( from_tick < 0 )); then
        from_tick=0
    fi

    echo "[PANIC-EXTRACT] ${dbg}: abort_tick=${abort_tick}, from_tick=${from_tick}" >&2

    events_file="${dbg}/events_panic.txt"
    align_file="${dbg}/bind_align_panic.tsv"

    python3 "${EXTRACT_SCRIPT}" "${log_file}" --from-tick "${from_tick}" > "${events_file}"
    python3 "${ALIGN_SCRIPT}" "${events_file}" > "${align_file}"
done
