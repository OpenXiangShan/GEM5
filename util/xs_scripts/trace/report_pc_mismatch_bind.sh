#!/usr/bin/env bash

# Summarize PC mismatch panics and align them with trace bind metadata.
#
# Usage:
#   bash report_pc_mismatch_bind.sh WORK_ROOT [OUT_FILE]
#
# WORK_ROOT 是 parallel_sim.sh 的结果根目录，例如：
#   /nfs/home/goulingrui/expri_results/gem5_trace/trace_ipc1_50M_50M
#
# 对于每个包含 panic "Trace stream PC mismatch" 的 workload 目录，
# 本脚本会：
#   1) 从顶层 log.txt 中解析 built_pc / expect_pc / sn / abort_tick。
#   2) 若存在 debug/bind_align.tsv，则按 sn 查找对应的 build/bind 信息。
#   3) 输出一行 TSV：
#        workload  sn  abort_tick  built_pc  expect_pc  build_tick  build_pc
#        bind_tick trace_pc taken tracesn
#

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

script_dir=$(dirname -- "$( readlink -f -- "$0"; )")
GEM5_HOME=${GEM5_HOME:-$(readlink -f "${script_dir}/../../..")}
DUMP_SCRIPT="${GEM5_HOME}/util/trace/dump_champsim_trace.py"
DEFAULT_TRACE_FORMAT=${TRACE_FORMAT:-champsim}

if [[ ! -f "${DUMP_SCRIPT}" ]]; then
    echo "Error: dump_champsim_trace.py not found at ${DUMP_SCRIPT}" >&2
    exit 1
fi

WORK_ROOT=$(readlink -f "${WORK_ROOT}")

if [[ -n "${OUT_FILE}" ]]; then
    : > "${OUT_FILE}"
fi

print_line() {
    local line="$1"
    if [[ -n "${OUT_FILE}" ]]; then
        printf '%s\n' "$line" | tee -a "${OUT_FILE}"
    else
        printf '%s\n' "$line"
    fi
}

# 输出表头
print_line "workload\tsn\tabort_tick\tbuilt_pc\texpect_pc\tbuild_tick\tbuild_pc\tbind_tick\ttrace_pc\ttaken\ttracesn"

for d in "${WORK_ROOT}"/*; do
    [[ -d "${d}" ]] || continue
    name=$(basename "${d}")

    log_file="${d}/log.txt"
    if [[ ! -f "${log_file}" ]]; then
        continue
    fi

    # 只关注包含 PC mismatch panic 的 workload
    panic_line=$(grep -m1 'Trace stream PC mismatch' "${log_file}" || true)
    if [[ -z "${panic_line}" ]]; then
        continue
    fi

    # 从 panic 行中提取 built_pc / expect_pc / sn
    # 示例：
    #   ... panic: [Fetch][tid:0] Trace stream PC mismatch: built=0x804c2a64 expect=0x804acfb8 (sn:2880172)
    built_pc=$(echo "${panic_line}" | sed -n 's/.*built=\(0x[0-9a-fA-F]\+\).*/\1/p')
    expect_pc=$(echo "${panic_line}" | sed -n 's/.*expect=\(0x[0-9a-fA-F]\+\).*/\1/p')
    sn=$(echo "${panic_line}" | sed -n 's/.*(sn:\([0-9]\+\)).*/\1/p')

    if [[ -z "${sn}" ]]; then
        echo "[WARN] Cannot extract sn from panic line in ${name}, skip" >&2
        continue
    fi

    # abort_tick 从 "Program aborted at tick" 行中获取
    abort_line=$(grep -m1 'Program aborted at tick' "${log_file}" || true)
    abort_tick="?"
    if [[ -n "${abort_line}" ]]; then
        abort_tick=$(echo "${abort_line}" | awk '{for (i=1;i<=NF;i++) if ($i=="tick") {print $(i+1); exit}}')
    fi

    # 默认对齐信息为缺失
    build_tick="-"
    build_pc_aligned="-"
    bind_tick="-"
    trace_pc="-"
    taken="-"
    tracesn="-"

    # 优先使用 panic 窗口生成的对齐文件，其次退回普通 bind_align.tsv
    if [[ -f "${d}/debug/bind_align_panic.tsv" ]]; then
        align_file="${d}/debug/bind_align_panic.tsv"
    else
        align_file="${d}/debug/bind_align.tsv"
    fi

    if [[ -f "${align_file}" ]]; then
        # 跳过表头，按 sn 精确匹配第一列
        # sn\tbuild_tick\tbuild_pc\tbind_tick\ttrace_pc\ttaken\ttracesn
        match=$(awk -v target_sn="$sn" 'NR>1 && $1==target_sn {print; exit}' "$align_file" || true)
        if [[ -n "${match}" ]]; then
            # shell 字段分割
            IFS=$'\t' read -r _sn build_tick build_pc_aligned bind_tick trace_pc taken tracesn <<< "$match"
        fi
    fi

    print_line "${name}\t${sn}\t${abort_tick}\t${built_pc}\t${expect_pc}\t${build_tick}\t${build_pc_aligned}\t${bind_tick}\t${trace_pc}\t${taken}\t${tracesn}"

    # 额外生成 panic 附近的 trace 片段：
    # 选择用于索引 ChampSim trace 的序号：优先使用 tracesn，其次退回 sn。
    trace_idx=""
    if [[ "${tracesn}" != "-" && "${tracesn}" =~ ^[0-9]+$ ]]; then
        trace_idx="${tracesn}"
    elif [[ "${sn}" =~ ^[0-9]+$ ]]; then
        trace_idx="${sn}"
    fi

    if [[ -n "${trace_idx}" ]]; then
        # 从顶层 log.txt 查找 trace 文件路径
        trace_line=$(grep -m1 'Trace file:' "${log_file}" || true)
        trace_file=""
        if [[ -n "${trace_line}" ]]; then
            trace_file=$(echo "${trace_line}" | awk '{print $3}')
        fi

        if [[ -n "${trace_file}" && -f "${trace_file}" ]]; then
            trace_fmt="${DEFAULT_TRACE_FORMAT}"
            fmt_line=$(grep -m1 'Trace format:' "${log_file}" || true)
            if [[ -n "${fmt_line}" ]]; then
                cand=$(echo "${fmt_line}" | awk '{print $3}')
                if [[ -n "${cand}" ]]; then
                    trace_fmt="${cand}"
                fi
            fi

            # 以 panic 对应的 trace_idx 为窗口尾，将前 100 条指令作为上下文
            # start = max(0, trace_idx - 99), count = 100
            start=0
            if (( trace_idx > 99 )); then
                start=$((trace_idx - 99))
            fi
            count=100

            out_snip="${d}/panic_trace_snippet.txt"
            echo "[SNIPPET] ${name}: trace_file=${trace_file}, format=${trace_fmt}, index=${trace_idx}, start=${start}, count=${count}" >&2
            python3 "${DUMP_SCRIPT}" --trace "${trace_file}" \
                --format "${trace_fmt}" \
                --start-index "${start}" --count "${count}" \
                > "${out_snip}" || echo "[WARN] Failed to dump trace snippet for ${name}" >&2
        else
            echo "[WARN] Trace file not found for ${name}: ${trace_file}" >&2
        fi
    fi
done
