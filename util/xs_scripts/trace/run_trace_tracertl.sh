#!/usr/bin/env bash

# Trace-driven XiangShan simulation for a single TRACERTL/NEMU trace
# (48-byte records, zstd-compressed or raw).
#
# Usage:
#   bash run_trace_tracertl.sh [OPTIONS] <trace_file>
#
# Options:
#   -n, --maxinsts N   Total instructions to simulate (default: auto from log,
#                      else 1000000)
#   -f, --format FMT   Trace format (default: $TRACE_FORMAT or tracertl)
#   -w, --warmup N     Warmup instructions before stats reset; default: auto
#                      from the sibling .trace.log, else $XS_WARMUP_INSTS_NO_SWITCH
#   -h, --help         Show this help message
#
# Warm-up auto-detection: the TRACERTL simpoint dumps ship a sibling log
#   <trace>.log   (i.e.  _128_0.6_.trace.zstd  ->  _128_0.6_.trace.log)
# whose lines give the precise transformed boundaries:
#   "Warmup Interval from 20000000 to 22511010"      -> warmup = 22511010
#   "Sampling Interval from 20000000 to 21490133"    -> sample = 21490133
# Both numbers count TRANSFORMED (RISC-V) records, matching the gem5 trace
# reader's instruction counter, so they can be used directly with
# --warmup-insts-no-switch / --maxinsts.
#
# Notes:
#   - For performance experiments, prefer decompressing first
#     (zstd -d -k file.trace.zstd -o file.trace): raw files support cheap
#     fseek-based rollback; compressed streams replay from the beginning.
#   - Intended for use with util/xs_scripts/trace/parallel_trace_sim.sh
#     (TRACE_FORMAT=tracertl) and gen_tracertl_workloads.sh.

set -euo pipefail

script_dir=$(dirname -- "$( readlink -f -- "$0"; )")
source "${script_dir}/../common.sh"

MAX_INSTS=${XS_MAX_INSTS:-0}   # 0 = auto-detect from log
TRACE_FORMAT=${TRACE_FORMAT:-"tracertl"}
DEBUG_FLAGS=${XS_DEBUG_FLAGS:-""}
DEBUG_START=${XS_DEBUG_START:-""}
DEBUG_END=${XS_DEBUG_END:-""}
WARMUP_NO_SWITCH=${XS_WARMUP_INSTS_NO_SWITCH:-0}
USE_SYNTHETIC_ENC=${XS_USE_SYNTHETIC_ENC:-0}

usage() {
    cat << EOF
Usage: $0 [OPTIONS] <trace_file>

Options:
  -n, --maxinsts N   Total instructions (default: auto from sibling log, else 1000000)
  -f, --format FMT   Trace format (default: ${TRACE_FORMAT})
  -w, --warmup N     Warmup instructions (default: auto from sibling log, else 0)
  -h, --help         Show this help message

Environment:
  gem5_home          Root of XS-GEM5 tree (auto-detected via common.sh)
  GEM5_BUILD_TYPE    gem5 build type (opt/debug), default: opt
  XS_MAX_INSTS               Override total instructions (default: auto)
  XS_WARMUP_INSTS_NO_SWITCH  Override warmup instructions (default: auto)
  XS_USE_SYNTHETIC_ENC       1 = force synthetic encoding (A/B diagnostic;
                             default: use the real encoding from the trace)
  XS_DEBUG_FLAGS / XS_DEBUG_START / XS_DEBUG_END  debug-flags passthrough
  TRACE_FORMAT       tracertl (default) | nemu alias
EOF
}

TRACE_FILE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -n|--maxinsts|--max-insts)
            [[ $# -lt 2 ]] && { echo "Error: $1 requires an argument" >&2; exit 1; }
            MAX_INSTS="$2"; shift 2 ;;
        -f|--format)
            [[ $# -lt 2 ]] && { echo "Error: $1 requires an argument" >&2; exit 1; }
            TRACE_FORMAT="$2"; shift 2 ;;
        -w|--warmup)
            [[ $# -lt 2 ]] && { echo "Error: $1 requires an argument" >&2; exit 1; }
            WARMUP_NO_SWITCH="$2"; shift 2 ;;
        -h|--help)
            usage; exit 0 ;;
        -*)
            echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
        *)
            if [[ -z "${TRACE_FILE}" ]]; then
                TRACE_FILE="$1"
            else
                echo "Error: multiple trace files specified" >&2; exit 1
            fi
            shift ;;
    esac
done

[[ -z "${TRACE_FILE}" ]] && { echo "Error: no trace file specified" >&2; usage >&2; exit 1; }
[[ -f "${TRACE_FILE}" ]] || { echo "Error: trace file not found: ${TRACE_FILE}" >&2; exit 1; }

# Sibling log: strip one compression suffix, append .log.
sibling_log="${TRACE_FILE}"
case "${TRACE_FILE}" in
    *.zstd|*.gz|*.xz) sibling_log="${TRACE_FILE%.*}.log" ;;
    *)                 sibling_log="${TRACE_FILE}.log" ;;
esac

# Auto-detect warmup/sample boundaries from the sibling log unless the
# caller pinned them explicitly (env or -w/-n). Log semantics: "from X"
# counts x86 SOURCE items, "to Y" counts TRANSFORMED records — only "to"
# is in the reader's instruction unit, so warmup = W_to and
# maxinsts = W_to + S_to (== "Total Transformed").
# `|| true` guards against logs without interval lines (set -euo pipefail
# would otherwise abort the script mid-run).
if [[ -f "${sibling_log}" ]]; then
    if [[ "${WARMUP_NO_SWITCH}" == "0" ]]; then
        WARMUP_NO_SWITCH=$(grep -oE 'Warmup Interval from [0-9]+ to [0-9]+' \
            "${sibling_log}" | tail -1 | awk '{print $NF}' || true)
        WARMUP_NO_SWITCH=${WARMUP_NO_SWITCH:-0}
    fi
    if [[ "${MAX_INSTS}" == "0" ]]; then
        sample=$(grep -oE 'Sampling Interval from [0-9]+ to [0-9]+' \
            "${sibling_log}" | tail -1 | awk '{print $NF}' || true)
        if [[ -n "${sample}" && "${WARMUP_NO_SWITCH}" != "0" ]]; then
            MAX_INSTS=$(( WARMUP_NO_SWITCH + sample ))
        fi
    fi
    echo "Warm-up/sample boundaries from ${sibling_log}"
fi
# Fallbacks when no log is present / fields missing.
[[ "${WARMUP_NO_SWITCH}" == "0" ]] && WARMUP_NO_SWITCH=${XS_WARMUP_INSTS_NO_SWITCH:-0}
[[ "${MAX_INSTS}" == "0" ]] && MAX_INSTS=1000000

OUTDIR=${OUTDIR:-"$(pwd)"}
mkdir -p "${OUTDIR}"
OUTDIR=$(realpath "${OUTDIR}")

echo "============================================="
echo "XiangShan Trace-Driven Simulation (TRACERTL/NEMU)"
echo "============================================="
echo "Trace file:      ${TRACE_FILE}"
echo "Trace format:    ${TRACE_FORMAT}"
echo "Max instructions: ${MAX_INSTS}"
[[ "${WARMUP_NO_SWITCH}" != "0" ]] && echo "Warmup insts(no switch): ${WARMUP_NO_SWITCH}"
[[ "${USE_SYNTHETIC_ENC}" == "1" ]] && echo "Encoding:        SYNTHETIC (A/B diagnostic)"
echo "Output directory: ${OUTDIR}"
echo "gem5 binary:     ${gem5}"
echo "============================================="

cmd=("${gem5}" "--outdir=${OUTDIR}" "--stats-file=${OUTDIR}/stats.txt")

if [[ -n "${DEBUG_FLAGS}" ]]; then
    cmd+=("--debug-flags=${DEBUG_FLAGS}")
    [[ -n "${DEBUG_START}" ]] && cmd+=("--debug-start=${DEBUG_START}")
    [[ -n "${DEBUG_END}" ]] && cmd+=("--debug-end=${DEBUG_END}")
fi

cmd+=(
    "${gem5_home}/configs/example/kmhv3.py"
    "--enable-trace-mode"
    "--trace-file=${TRACE_FILE}"
    "--trace-format=${TRACE_FORMAT}"
)

[[ "${WARMUP_NO_SWITCH}" != "0" ]] && cmd+=("--warmup-insts-no-switch=${WARMUP_NO_SWITCH}")
[[ "${USE_SYNTHETIC_ENC}" == "1" ]] && cmd+=("--trace-use-synthetic-enc")

cmd+=(
    "--maxinsts=${MAX_INSTS}"
    "--trace-enable-decoupled-bp"
)

echo "Running: ${cmd[*]}"
"${cmd[@]}"
