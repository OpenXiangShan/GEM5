#!/usr/bin/env bash

# Collect error reasons for aborted workloads launched via parallel_sim.sh.
#
# Usage:
#   bash collect_parallel_errors.sh WORK_ROOT [OUT_FILE]
#
# WORK_ROOT 是 parallel_sim.sh 的第 4 个参数对应的结果目录，
# 例如：/nfs/home/goulingrui/expri_results/gem5_trace/trace_ipc1_50M_50M。
#
# 本脚本会遍历 WORK_ROOT 下的子目录（每个子目录对应一个 workload），
# 对于包含 "abort" 文件的目录，从 log.txt 中提取一行“看起来像
# 错误原因”的信息，并输出一行摘要：
#
#   <workload_name>  <abort_tick_or_?>  <reason_line>
#
# 如果指定了 OUT_FILE，则结果同时写入该文件。

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 WORK_ROOT [OUT_FILE]" >&2
    exit 1
fi

WORK_ROOT=$1
OUT_FILE=${2:-}

if [[ ! -d "${WORK_ROOT}" ]]; then
    echo "Error: WORK_ROOT is not a directory: ${WORK_ROOT}" >&2
    exit 1
fi

WORK_ROOT=$(readlink -f "${WORK_ROOT}")

collect() {
    local d="$1"
    [[ -d "${d}" ]] || return 0

    local name
    name=$(basename "${d}")

    if [[ ! -f "${d}/abort" ]]; then
        return 0
    fi

    local log_file="${d}/log.txt"
    local abort_tick="?"
    local reason="no log.txt found"

    if [[ -f "${log_file}" ]]; then
        local line
        # 优先提取 panic 行，其次是 Program aborted 行，再其次是包含
        # "error" 或 "fatal" 的行，最后退化为 log 的最后一行。
        line=$(grep -m1 'panic:' "${log_file}" || true)
        if [[ -z "${line}" ]]; then
            line=$(grep -m1 -i 'error:' "${log_file}" || true)
        fi
        if [[ -z "${line}" ]]; then
            line=$(grep -m1 -i 'fatal' "${log_file}" || true)
        fi
        if [[ -z "${line}" ]]; then
            line=$(grep -m1 -i 'Assertion' "${log_file}" || true)
        fi
        if [[ -z "${line}" ]]; then
            line=$(grep -m1 'Program aborted at tick' "${log_file}" || true)
        fi
        if [[ -z "${line}" ]]; then
            line=$(tail -n 1 "${log_file}" || true)
        fi

        reason=${line:-"(empty log)"}

        local abort_line
        abort_line=$(grep -m1 'Program aborted at tick' "${log_file}" || true)
        if [[ -n "${abort_line}" ]]; then
            # 形如：Program aborted at tick 98498736
            abort_tick=$(echo "${abort_line}" | awk '{print $5}')
        fi
    fi

    printf '%s\t%s\t%s\n' "${name}" "${abort_tick}" "${reason}"
}

if [[ -n "${OUT_FILE}" ]]; then
    : > "${OUT_FILE}"
fi

for d in "${WORK_ROOT}"/*; do
    line=$(collect "${d}") || continue
    [[ -z "${line}" ]] && continue
    if [[ -n "${OUT_FILE}" ]]; then
        printf '%s\n' "${line}" | tee -a "${OUT_FILE}"
    else
        printf '%s\n' "${line}"
    fi
done

