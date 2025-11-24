#!/usr/bin/env bash

# Extract gem5 events around the final tick for all debug runs under a root.
#
# Usage:
#   bash extract_debug_events.sh ROOT_DIR
#
# ROOT_DIR 为批跑结果的根目录，比如：
#   /nfs/home/goulingrui/expri_results/gem5_trace/trace_ipc1_50M_50M
#
# 脚本会递归查找 ROOT_DIR 下所有名为 "debug" 的子目录，
# 对于每个包含 log.txt 的目录：
#   1) 从 log.txt 中解析 gem5 的最终 tick（优先使用
#      "Exiting @ tick"，否则使用 "Program aborted at tick"，
#      再否则退化为最后一条包含 "tick" 的行）。
#   2) 计算 from_tick = last_tick - 10000（不小于 0）。
#   3) 运行：
#        $GEM5_HOME/util/trace/extract_trace_events.py log.txt --from-tick from_tick
#      并将输出重定向到同一 debug 目录下的 events.txt。

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 ROOT_DIR" >&2
    exit 1
fi

ROOT_DIR=$1

if [[ ! -d "${ROOT_DIR}" ]]; then
    echo "Error: ROOT_DIR is not a directory: ${ROOT_DIR}" >&2
    exit 1
fi

# 推断 GEM5_HOME：优先使用环境变量，否则假定脚本位于
#   $GEM5_HOME/util/xs_scripts/extract_debug_events.sh
script_dir=$(dirname -- "$( readlink -f -- "$0"; )")
GEM5_HOME=${GEM5_HOME:-$(readlink -f "${script_dir}/../..")}

EXTRACT_SCRIPT="${GEM5_HOME}/util/trace/extract_trace_events.py"
if [[ ! -f "${EXTRACT_SCRIPT}" ]]; then
    echo "Error: extract_trace_events.py not found at ${EXTRACT_SCRIPT}" >&2
    exit 1
fi

ROOT_DIR=$(readlink -f "${ROOT_DIR}")

echo "Using GEM5_HOME=${GEM5_HOME}" >&2
echo "Scanning debug directories under ${ROOT_DIR}" >&2

find "${ROOT_DIR}" -type d -name debug | while read -r dbg; do
    log_file="${dbg}/log.txt"
    if [[ ! -f "${log_file}" ]]; then
        echo "[WARN] No log.txt in ${dbg}, skip" >&2
        continue
    fi

    # 尝试解析最终 tick
    last_tick=""

    # 1) Exiting @ tick
    line=$(grep -m1 "Exiting @ tick" "${log_file}" | tail -n1 || true)
    if [[ -n "${line}" ]]; then
        last_tick=$(echo "${line}" | awk '{for (i=1;i<=NF;i++) if ($i=="tick") {print $(i+1); exit}}')
    fi

    # 2) Program aborted at tick
    if [[ -z "${last_tick}" ]]; then
        line=$(grep -m1 "Program aborted at tick" "${log_file}" | tail -n1 || true)
        if [[ -n "${line}" ]]; then
            last_tick=$(echo "${line}" | awk '{for (i=1;i<=NF;i++) if ($i=="tick") {print $(i+1); exit}}')
        fi
    fi

    # 3) 最后一条包含 "tick" 的行
    if [[ -z "${last_tick}" ]]; then
        line=$(grep "tick" "${log_file}" | tail -n1 || true)
        if [[ -n "${line}" ]]; then
            # 尝试提取最后一个数字作为 tick
            last_tick=$(echo "${line}" | awk '{for (i=NF;i>=1;i--) if ($i ~ /^[0-9]+$/) {print $i; exit}}')
        fi
    fi

    if [[ -z "${last_tick}" || ! "${last_tick}" =~ ^[0-9]+$ ]]; then
        echo "[WARN] Could not determine last tick for ${dbg}, skip" >&2
        continue
    fi

    # 计算 from_tick = last_tick - 10000，至少为 0
    from_tick=$(( last_tick - 10000 ))
    if (( from_tick < 0 )); then
        from_tick=0
    fi

    echo "[EXTRACT] ${dbg}: last_tick=${last_tick}, from_tick=${from_tick}" >&2

    out_file="${dbg}/events.txt"
    python3 "${EXTRACT_SCRIPT}" "${log_file}" --from-tick "${from_tick}" > "${out_file}"
done
