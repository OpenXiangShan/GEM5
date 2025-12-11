#!/usr/bin/env bash

# Parallel version of count_traces_from_list.sh
#
# Usage:
#   bash count_traces_from_list_parallel.sh LIST_FILE TRACE_ROOT OUT_FILE [JOBS]
#
# LIST_FILE  为 workload 列表，例如 champsim_traces.lst：
#   name  rel_path_prefix  skip  fw  dw  sample
# TRACE_ROOT 为 trace 根目录，例如：/nfs/home/share/glr/champsim_traces
# OUT_FILE   为统计结果输出文件路径。
# JOBS       为并行任务数（可选，默认: $(nproc) 或 8）。
# 环境变量（可选）：
#   TRACE_FORMAT   champsim | cbp2025（默认 champsim）
#
# 输出格式（TSV）：
#   name  rel_path_prefix  trace_path  inst_count

set -euo pipefail

if [[ $# -lt 3 || $# -gt 4 ]]; then
    echo "Usage: $0 LIST_FILE TRACE_ROOT OUT_FILE [JOBS]" >&2
    exit 1
fi

LIST_FILE=$1
TRACE_ROOT=$2
OUT_FILE=$3
JOBS=${4:-}
TRACE_FORMAT=${TRACE_FORMAT:-champsim}

if [[ ! -f "${LIST_FILE}" ]]; then
    echo "Error: LIST_FILE not found: ${LIST_FILE}" >&2
    exit 1
fi

if [[ ! -d "${TRACE_ROOT}" ]]; then
    echo "Error: TRACE_ROOT is not a directory: ${TRACE_ROOT}" >&2
    exit 1
fi

TRACE_ROOT=$(readlink -f "${TRACE_ROOT}")
ROOT_DIR=$(dirname -- "$( readlink -f -- "$0"; )")
GEM5_HOME=${GEM5_HOME:-$(readlink -f "${ROOT_DIR}/../..")}

COUNT_SCRIPT="${GEM5_HOME}/util/trace/count_champsim_trace_insts.py"
if [[ ! -f "${COUNT_SCRIPT}" ]]; then
    echo "Error: count_champsim_trace_insts.py not found at ${COUNT_SCRIPT}" >&2
    exit 1
fi

# 默认并行度：优先用 nproc，退化为 8
if [[ -z "${JOBS}" ]]; then
    if command -v nproc >/dev/null 2>&1; then
        JOBS=$(nproc)
    else
        JOBS=8
    fi
fi

echo "Using GEM5_HOME=${GEM5_HOME}" >&2
echo "Reading list from ${LIST_FILE}, trace root=${TRACE_ROOT}, format=${TRACE_FORMAT}, jobs=${JOBS}" >&2

export TRACE_ROOT
export COUNT_SCRIPT
export TRACE_FORMAT

WORKER_SCRIPT="${ROOT_DIR}/count_traces_worker.sh"
if [[ ! -x "${WORKER_SCRIPT}" ]]; then
    # 确保 worker 可执行
    chmod +x "${WORKER_SCRIPT}" || true
fi

# 过滤空行和注释行，然后用 GNU parallel 并行处理。
# 注意：workload 列表中字段之间可能存在多个空格/Tab；
# 使用 "--colsep '[[:space:]]+'" 把连续空白视为一个分隔符，
# 从而确保 {1} 和 {2} 分别对应 name 与 rel_path_prefix。
grep -v '^#' "${LIST_FILE}" | grep -v '^[[:space:]]*$' | \
    parallel -j "${JOBS}" --colsep '[[:space:]]+' "${WORKER_SCRIPT}" {1} {2} > "${OUT_FILE}"

echo "Done. Results written to ${OUT_FILE}" >&2
