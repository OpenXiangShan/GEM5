#!/usr/bin/env bash

# Worker script: count instructions for a single trace given
#   $1 = workload name
#   $2 = rel_path_prefix (相对 TRACE_ROOT 的前缀，不含压缩后缀)
#
# 依赖环境变量：
#   TRACE_ROOT   -> trace 根目录
#   COUNT_SCRIPT -> util/trace/count_champsim_trace_insts.py 的绝对路径

set -euo pipefail

name=${1:-}
rel=${2:-}

if [[ -z "${name}" || -z "${rel}" ]]; then
    exit 0
fi

if [[ -z "${TRACE_ROOT:-}" || -z "${COUNT_SCRIPT:-}" ]]; then
    echo "[ERROR] TRACE_ROOT or COUNT_SCRIPT not set in worker" >&2
    exit 1
fi

trace_path=""
fmt=${TRACE_FORMAT:-champsim}

# 1) 精确匹配 .xz/.gz
if [[ -f "${TRACE_ROOT}/${rel}.xz" ]]; then
    trace_path="${TRACE_ROOT}/${rel}.xz"
elif [[ -f "${TRACE_ROOT}/${rel}.gz" ]]; then
    trace_path="${TRACE_ROOT}/${rel}.gz"
else
    # 2) 回退到模糊匹配（basename 前缀 + 压缩后缀）
    base_rel=$(basename "${rel}")
    trace_path=$(find "${TRACE_ROOT}" -type f \
        \( -name "${base_rel}*.xz" -o -name "${base_rel}*.gz" \) \
        | head -n 1 || true)
fi

if [[ -z "${trace_path}" || ! -f "${trace_path}" ]]; then
    echo "[WARN] Trace file not found for ${name}: rel=${rel}" >&2
    exit 0
fi

echo "[COUNT] ${name}: ${trace_path} (fmt=${fmt})" >&2
count=$(python3 "${COUNT_SCRIPT}" --trace "${trace_path}" --format "${fmt}" || echo "ERR")

printf "%s\t%s\t%s\t%s\n" "${name}" "${rel}" "${trace_path}" "${count}"
