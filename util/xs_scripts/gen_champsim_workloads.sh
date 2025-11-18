#!/usr/bin/env bash

# Generate a workload_list file for parallel_sim.sh from a directory
# containing ChampSim trace files.
#
# Usage:
#   bash gen_champsim_workloads.sh TRACE_ROOT OUTPUT_LIST
#
# Each line in OUTPUT_LIST will have the form:
#   name  relative_trace_path_without_suffix  0 50000000 0 50000000

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
    find "${TRACE_ROOT}" -type f -name '*.champsimtrace.xz' | sort | while read -r trace_path; do
        rel=${trace_path#"${TRACE_ROOT}/"}
        base=$(basename "${trace_path}")
        name=${base%.champsimtrace.xz}

        # relative path without suffix, used by parallel_sim.sh to locate the file
        dir_part=$(dirname "${rel}")
        trace_prefix=${name}.champsimtrace
        if [[ "${dir_part}" != "." ]]; then
            rel_prefix="${dir_part}/${trace_prefix}"
        else
            rel_prefix="${trace_prefix}"
        fi

        # skip 0, warmup 50M, detailed warmup 0, sample 50M
        echo "${name}  ${rel_prefix}  0 50000000 0 50000000"
    done
} > "${OUTPUT_LIST}"

echo "Done. Wrote $(wc -l < "${OUTPUT_LIST}") entries." >&2

