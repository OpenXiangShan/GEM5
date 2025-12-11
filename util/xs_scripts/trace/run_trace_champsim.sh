#!/usr/bin/env bash

# Trace-driven XiangShan simulation for a single trace (ChampSim / CBP2025).
#
# Usage:
#   bash run_trace_champsim.sh [OPTIONS] <trace_file>
#
# Options:
#   -n, --maxinsts N   Total instructions to simulate (default: 100000000)
#   -f, --format FMT   Trace format (default: $TRACE_FORMAT or champsim)
#   -h, --help         Show this help message

set -euo pipefail

script_dir=$(dirname -- "$( readlink -f -- "$0"; )")
# common.sh 仍位于 util/xs_scripts/ 下，这里从 trace 子目录回到上一级再引用。
source "${script_dir}/../common.sh"

MAX_INSTS=${XS_MAX_INSTS:-1000000}
TRACE_FORMAT=${TRACE_FORMAT:-"champsim"}
DEBUG_FLAGS=${XS_DEBUG_FLAGS:-""}
DEBUG_START=${XS_DEBUG_START:-""}
DEBUG_END=${XS_DEBUG_END:-""}
WARMUP_NO_SWITCH=${XS_WARMUP_INSTS_NO_SWITCH:-0}

usage() {
    cat << EOF
Usage: $0 [OPTIONS] <trace_file>

Options:
  -n, --maxinsts N   Total instructions to simulate (default: ${MAX_INSTS})
  -f, --format FMT   Trace format (default: ${TRACE_FORMAT})
  -h, --help         Show this help message

Environment:
  gem5_home          Root of XS-GEM5 tree (auto-detected via common.sh)
  GEM5_BUILD_TYPE    gem5 build type (opt/debug), default: opt
  XS_MAX_INSTS       Override total instructions (default: 1000000)
  XS_DEBUG_FLAGS     Optional debug flags passed to gem5
  XS_DEBUG_START     Optional debug start tick (requires XS_DEBUG_FLAGS)
  XS_DEBUG_END       Optional debug end tick (requires XS_DEBUG_FLAGS)
  XS_WARMUP_INSTS_NO_SWITCH  Warmup insts before stats reset (no CPU switch)
  TRACE_FORMAT       champsim | cbp2025 (default: champsim)

Notes:
  - This script is intended to be used with util/xs_scripts/parallel_sim.sh.
  - When used with parallel_sim.sh, each workload's work_dir is set as CWD,
    and this script will write all outputs into that directory.
EOF
}

TRACE_FILE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -n|--maxinsts|--max-insts)
            if [[ $# -lt 2 ]]; then
                echo "Error: $1 requires an argument" >&2
                exit 1
            fi
            MAX_INSTS="$2"
            shift 2
            ;;
        -f|--format)
            if [[ $# -lt 2 ]]; then
                echo "Error: $1 requires an argument" >&2
                exit 1
            fi
            TRACE_FORMAT="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
        *)
            if [[ -z "${TRACE_FILE}" ]]; then
                TRACE_FILE="$1"
            else
                echo "Error: multiple trace files specified" >&2
                usage >&2
                exit 1
            fi
            shift
            ;;
    esac
done

if [[ -z "${TRACE_FILE}" ]]; then
    echo "Error: no trace file specified" >&2
    usage >&2
    exit 1
fi

if [[ ! -f "${TRACE_FILE}" ]]; then
    echo "Error: trace file not found: ${TRACE_FILE}" >&2
    exit 1
fi

# Output directory: default to current working directory. When called from
# parallel_sim.sh, this will be each workload's work_dir.
OUTDIR=${OUTDIR:-"$(pwd)"}

echo "============================================="
echo "XiangShan Trace-Driven Simulation (ChampSim/CBP2025)"
echo "============================================="
echo "Trace file:      ${TRACE_FILE}"
echo "Trace format:    ${TRACE_FORMAT}"
echo "Max instructions: ${MAX_INSTS}"
if [[ "${WARMUP_NO_SWITCH}" != "0" ]]; then
    echo "Warmup insts(no switch): ${WARMUP_NO_SWITCH}"
fi
echo "Output directory: ${OUTDIR}"
echo "gem5 binary:     ${gem5}"
echo "Config script:   ${gem5_home}/configs/example/kmhv3.py"
if [[ -n "${DEBUG_FLAGS}" ]]; then
    echo "Debug flags:     ${DEBUG_FLAGS}"
    if [[ -n "${DEBUG_START}" || -n "${DEBUG_END}" ]]; then
        echo "Debug window:    start=${DEBUG_START:-0} end=${DEBUG_END:-"(none)"}"
    fi
fi
echo "============================================="

mkdir -p "${OUTDIR}"

cmd=("${gem5}" "--outdir=${OUTDIR}" "--stats-file=${OUTDIR}/stats.txt")

if [[ -n "${DEBUG_FLAGS}" ]]; then
    cmd+=("--debug-flags=${DEBUG_FLAGS}")
    if [[ -n "${DEBUG_START}" ]]; then
        cmd+=("--debug-start=${DEBUG_START}")
    fi
    if [[ -n "${DEBUG_END}" ]]; then
        cmd+=("--debug-end=${DEBUG_END}")
    fi
fi

cmd+=(
    "${gem5_home}/configs/example/kmhv3.py"
    "--enable-trace-mode"
    "--trace-file=${TRACE_FILE}"
    "--trace-format=${TRACE_FORMAT}"
)

if [[ "${WARMUP_NO_SWITCH}" != "0" ]]; then
    cmd+=("--warmup-insts-no-switch=${WARMUP_NO_SWITCH}")
fi

cmd+=(
    "--maxinsts=${MAX_INSTS}"
    "--trace-enable-decoupled-bp"
)

echo "Running: ${cmd[*]}"
"${cmd[@]}"
