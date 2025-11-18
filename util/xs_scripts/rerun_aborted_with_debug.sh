#!/usr/bin/env bash

# Rerun aborted trace-driven workloads with debug flags enabled in a
# narrow tick window around the abort point.
#
# Usage:
#   bash rerun_aborted_with_debug.sh WORK_ROOT [DEBUG_FLAGS]
#
#   WORK_ROOT    Directory containing per-workload subdirectories, e.g.
#                trace_champsim_1M (the tag passed to parallel_sim.sh).
#   DEBUG_FLAGS  Optional comma-separated debug flags. If omitted,
#                defaults to:
#                IEW,Fetch,Commit,CommitTrace,DecoupleBP,TraceReader,Decode

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 WORK_ROOT [DEBUG_FLAGS]" >&2
    exit 1
fi

WORK_ROOT=$1
DEBUG_FLAGS_DEFAULT="IEW,Fetch,Commit,CommitTrace,DecoupleBP,TraceReader,Decode"
DEBUG_FLAGS=${2:-$DEBUG_FLAGS_DEFAULT}

if [[ ! -d "${WORK_ROOT}" ]]; then
    echo "Error: WORK_ROOT is not a directory: ${WORK_ROOT}" >&2
    exit 1
fi

WORK_ROOT=$(readlink -f "${WORK_ROOT}")
script_dir=$(dirname -- "$( readlink -f -- "$0"; )")
run_script="${script_dir}/run_trace_champsim.sh"

if [[ ! -x "${run_script}" ]]; then
    echo "Error: run_trace_champsim.sh not found or not executable: ${run_script}" >&2
    exit 1
fi

echo "Rerunning aborted workloads under ${WORK_ROOT} with debug flags: ${DEBUG_FLAGS}" >&2

num_threads=${xsgem5_para_jobs:-8}

rerun_one() {
    local d="$1"
    local debug_flags="$2"

    [[ -d "${d}" ]] || return 0
    local name
    name=$(basename "${d}")

    if [[ ! -f "${d}/abort" ]]; then
        return 0
    fi

    local log_file="${d}/log.txt"
    if [[ ! -f "${log_file}" ]]; then
        echo "[${name}] skip: no log.txt found" >&2
        return 0
    fi

    local abort_line
    abort_line=$(grep -m1 'Program aborted at tick' "${log_file}" || true)
    if [[ -z "${abort_line}" ]]; then
        echo "[${name}] skip: cannot find abort tick in log.txt" >&2
        return 0
    fi

    local abort_tick
    abort_tick=$(echo "${abort_line}" | awk '{print $5}')
    if ! [[ "${abort_tick}" =~ ^[0-9]+$ ]]; then
        echo "[${name}] skip: invalid abort tick '${abort_tick}'" >&2
        return 0
    fi

    # 默认 debug 窗口: [abort_tick - 1,000,000, abort_tick + 1000]
    local start_tick=$((abort_tick - 1000000))
    if (( start_tick < 0 )); then
        start_tick=0
    fi
    local end_tick=$((abort_tick + 1000))

    # 如果 log 中包含 CommitStuck 给出的建议窗口：
    #   ... suggested --debug-start=22551196563 --debug-end=22551279813
    # 则优先按该窗口重跑，以便对齐原 panic 提供的调试区间。
    local suggest_line
    suggest_line=$(grep -m1 'suggested --debug-start=' "${log_file}" || true)
    if [[ -n "${suggest_line}" ]]; then
        local s_start s_end
        s_start=$(echo "${suggest_line}" | sed -n 's/.*--debug-start=\([0-9]\+\).*/\1/p')
        s_end=$(echo "${suggest_line}"   | sed -n 's/.*--debug-end=\([0-9]\+\).*/\1/p')
        if [[ "${s_start}" =~ ^[0-9]+$ && "${s_end}" =~ ^[0-9]+$ ]]; then
            start_tick=${s_start}
            end_tick=${s_end}
        fi
    fi

    local trace_line
    trace_line=$(grep -m1 'Trace file:' "${log_file}" || true)
    if [[ -z "${trace_line}" ]]; then
        echo "[${name}] skip: cannot find trace file line in log.txt" >&2
        return 0
    fi

    local trace_file
    trace_file=$(echo "${trace_line}" | awk '{print $3}')
    if [[ -z "${trace_file}" || ! -f "${trace_file}" ]]; then
        echo "[${name}] skip: trace file not found: ${trace_file}" >&2
        return 0
    fi

    local maxinsts_line maxinsts
    maxinsts_line=$(grep -m1 'Max instructions:' "${log_file}" || true)
    if [[ -n "${maxinsts_line}" ]]; then
        maxinsts=$(echo "${maxinsts_line}" | awk '{print $3}')
    else
        maxinsts=1000000
    fi

    local debug_dir="${d}/debug"
    mkdir -p "${debug_dir}"

    echo "[${name}] abort_tick=${abort_tick} debug_start=${start_tick} debug_end=${end_tick}" >&2
    echo "[${name}] trace_file=${trace_file} maxinsts=${maxinsts} output=${debug_dir}" >&2

    (
        cd "${debug_dir}"
        XS_MAX_INSTS="${maxinsts}" \
        XS_DEBUG_FLAGS="${debug_flags}" \
        XS_DEBUG_START="${start_tick}" \
        XS_DEBUG_END="${end_tick}" \
        OUTDIR="${debug_dir}" \
        bash "${run_script}" "${trace_file}" > "${debug_dir}/log.txt" 2>&1
    )
}

export -f rerun_one
export run_script DEBUG_FLAGS

# Use GNU parallel to rerun aborted workloads in parallel, similar to
# util/xs_scripts/parallel_sim.sh.
find "${WORK_ROOT}" -mindepth 1 -maxdepth 1 -type d | \
    parallel -j "${num_threads}" rerun_one {} "${DEBUG_FLAGS}"
