#!/usr/bin/env bash

# Summarize the status of runs launched via util/xs_scripts/parallel_sim.sh.
#
# Usage:
#   bash check_parallel_runs.sh WORK_ROOT [--details]
#
# WORK_ROOT is the tag directory passed as the 4th argument to parallel_sim.sh,
# e.g. trace_champsim_100M.

set -uo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 WORK_ROOT [--details]" >&2
    exit 1
fi

WORK_ROOT=$1
shift || true

DETAILS=false
if [[ "${1:-}" == "--details" ]]; then
    DETAILS=true
fi

if [[ ! -d "${WORK_ROOT}" ]]; then
    echo "Error: WORK_ROOT is not a directory: ${WORK_ROOT}" >&2
    exit 1
fi

WORK_ROOT=$(readlink -f "${WORK_ROOT}")

total=0
completed=0
running=0
aborted=0
pending=0
failed=0

while IFS= read -r -d '' dir; do
    ((total++))
    name=$(basename "${dir}")

    state="PENDING"
    if [[ -f "${dir}/abort" ]]; then
        state="ABORTED"
        ((aborted++))
    elif [[ -f "${dir}/running" ]]; then
        state="RUNNING"
        ((running++))
    elif [[ -f "${dir}/completed" ]]; then
        state="COMPLETED"
        ((completed++))
    else
        state="PENDING"
        ((pending++))
    fi

    stats="missing"
    if [[ -f "${dir}/stats.txt" ]]; then
        stats="ok"
    else
        if [[ "${state}" == "COMPLETED" ]]; then
            ((failed++))
        fi
    fi

    if ${DETAILS}; then
        printf "%-30s %-10s stats:%s\n" "${name}" "${state}" "${stats}"
    fi
done < <(find "${WORK_ROOT}" -mindepth 1 -maxdepth 1 -type d -print0 | sort -z)

echo "Work root: ${WORK_ROOT}"
echo "Total tasks:     ${total}"
echo "  completed:     ${completed}"
echo "  running:       ${running}"
echo "  aborted:       ${aborted}"
echo "  pending:       ${pending}"
echo "Completed without stats.txt (possible failure): ${failed}"
