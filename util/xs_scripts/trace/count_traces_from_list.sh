#!/usr/bin/env bash

# Count instruction records for all traces listed in a workload list.
#
# Usage:
#   bash count_traces_from_list.sh LIST_FILE TRACE_ROOT OUT_FILE
#
# LIST_FILE  为 workload 列表，例如 champsim_traces.lst：
#   name  rel_path_prefix  skip  fw  dw  sample
# TRACE_ROOT 为 trace 根目录，例如：/nfs/home/share/glr/champsim_traces
# OUT_FILE   为统计结果输出文件路径。
# 环境变量（可选）：
#   TRACE_FORMAT   champsim | cbp2025（默认 champsim）
#
# 输出格式（TSV）：
#   name  rel_path_prefix  trace_path  inst_count

set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "Usage: $0 LIST_FILE TRACE_ROOT OUT_FILE" >&2
    exit 1
fi

LIST_FILE=$1
TRACE_ROOT=$2
OUT_FILE=$3

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

echo "Using GEM5_HOME=${GEM5_HOME}" >&2
echo "Reading list from ${LIST_FILE}, trace root=${TRACE_ROOT}, format=${TRACE_FORMAT}" >&2

: > "${OUT_FILE}"

while read -r line; do
    # 跳过空行或注释行
    [[ -z "${line}" ]] && continue
    [[ "${line}" =~ ^# ]] && continue

    # name  rel_path_prefix  skip  fw  dw  sample
    name=""; rel=""; skip=""; fw=""; dw=""; sample=""
    IFS=' ' read -r name rel skip fw dw sample <<< "${line}"
    if [[ -z "${name}" || -z "${rel}" ]]; then
        echo "[WARN] Malformed line (skip): ${line}" >&2
        continue
    fi

    # 尝试在 TRACE_ROOT 下找到对应的 trace 文件，优先 .xz，再 .gz
    trace_path=""
    # 1) 精确匹配 .xz
    if [[ -f "${TRACE_ROOT}/${rel}.xz" ]]; then
        trace_path="${TRACE_ROOT}/${rel}.xz"
    elif [[ -f "${TRACE_ROOT}/${rel}.gz" ]]; then
        trace_path="${TRACE_ROOT}/${rel}.gz"
    else
        # 2) 回退到模糊匹配
        trace_path=$(find "${TRACE_ROOT}" -type f \( -name "$(basename "${rel}")*.xz" -o -name "$(basename "${rel}")*.gz" \) | head -n1 || true)
    fi

    if [[ -z "${trace_path}" || ! -f "${trace_path}" ]]; then
        echo "[WARN] Trace file not found for ${name}: rel=${rel}" >&2
        continue
    fi

    echo "[COUNT] ${name}: ${trace_path} (fmt=${TRACE_FORMAT})" >&2
    count=$(python3 "${COUNT_SCRIPT}" --trace "${trace_path}" --format "${TRACE_FORMAT}" || echo "ERR")

    printf '%s\t%s\t%s\t%s\n' "${name}" "${rel}" "${trace_path}" "${count}" >> "${OUT_FILE}"
done < "${LIST_FILE}"

echo "Done. Results written to ${OUT_FILE}" >&2
