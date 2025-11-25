#!/usr/bin/env bash

# Generate a workload_list file for parallel_sim.sh from a directory
# containing trace files (ChampSim / CBP2025).
#
# Usage:
#   bash gen_champsim_workloads.sh TRACE_ROOT OUTPUT_LIST
#
# Each line in OUTPUT_LIST will have the form:
#   name  relative_trace_path_without_compression_suffix  0 50000000 0 50000000
#
# 支持的 trace 文件格式（在 TRACE_ROOT 下递归搜索）：
#   - 任意以 .xz 结尾的 trace 文件
#   - 任意以 .gz 结尾的 trace 文件
#   - （可选）未压缩的 *.champsimtrace

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 TRACE_ROOT OUTPUT_LIST" >&2
    exit 1
fi

TRACE_ROOT=$1
OUTPUT_LIST=$2

if [[ ! -d "${TRACE_ROOT}" ]]; then
    echo "Error: TRACE_ROOT is not a directory: ${TRACE_ROOT}" >&2
    exit 1
fi

TRACE_ROOT=$(readlink -f "${TRACE_ROOT}")

echo "Generating workload list from ${TRACE_ROOT} into ${OUTPUT_LIST}" >&2

{
    # 递归搜索 ChampSim trace 文件：
    #  - *.xz / *.gz （常见压缩格式）
    #  - 以及未压缩的 *.champsimtrace
    find "${TRACE_ROOT}" -type f \( -name '*.xz' -o -name '*.gz' -o -name '*.champsimtrace' \) \
        | sort | while read -r trace_path; do
        rel=${trace_path#"${TRACE_ROOT}/"}

        # 去掉压缩后缀，得到逻辑上的 trace 前缀；不强制依赖
        # 文件名中包含 ".champsimtrace"，以兼容更一般的命名。
        case "${rel}" in
            *.xz)
                rel_prefix=${rel%.xz}
                ;;
            *.gz)
                rel_prefix=${rel%.gz}
                ;;
            *)
                rel_prefix=${rel}
                ;;
        esac

        # name 需要在不同子目录下保持唯一，这里将相对路径中的
        # '/' 替换为 '_' 作为 workload 名称，例如：
        #   cvp1_public/compute_int_14 -> cvp1_public_compute_int_14
        name=${rel_prefix//\//_}

        # skip 0, warmup 50M, detailed warmup 0, sample 50M
        echo "${name}  ${rel_prefix}  0 50000000 0 50000000"
    done
} > "${OUTPUT_LIST}"

echo "Done. Wrote $(wc -l < "${OUTPUT_LIST}") entries." >&2
