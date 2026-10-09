#!/usr/bin/env bash

# Generate a parallel_trace_sim.sh workload list for a TRACERTL simpoint
# interval corpus.
#
# Corpus layout (gwt-v2-nemu-format-simpoint-interval style):
#   <root>/<workload>/<simpoint_id>/_<id>_<weight>_.trace.zstd
#   <root>/<workload>/<simpoint_id>/_<id>_<weight>_.trace.log
#
# The .log files carry the exact transformed (RISC-V) warm-up / sample
# boundaries, which this script parses into the workload-list fields:
#   name path skip fw dw sample
# Log line semantics (verified against the corpus): "Warmup/Sampling
# Interval from X to Y" — X counts the segment's x86 SOURCE items, Y counts
# the segment's TRANSFORMED (RISC-V) records. Only Y is in the reader's
# instruction unit (W_to + S_to == "Total Transformed" == 48-byte record
# count), so dw = warm-up "to" and sample = sampling "to".
# parallel_trace_sim.sh maps fw+dw -> XS_WARMUP_INSTS_NO_SWITCH and
# fw+dw+sample -> XS_MAX_INSTS, matching the reader's record counting.
#
# Usage:
#   bash gen_tracertl_workloads.sh <corpus_root> [output_list]
#
# Output list lines look like:
#   arizona-128 arizona/128/_128_0.6_.trace 0 0 22511010 21490133
# (path keeps the .trace stem; prepare_env re-attaches .zstd to find
#  <prefix>.trace.zstd.)

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <corpus_root> [output_list]" >&2
    exit 1
fi

root=$(realpath "$1")
out=${2:-"${root}/workloads.lst"}

[[ -d "${root}" ]] || { echo "Error: corpus root not found: ${root}" >&2; exit 1; }

echo "workload path skip fw dw sample" > "${out}"
n=0

for trace in $(find -L "${root}" -name '*.trace.zstd' | sort); do
    rel=${trace#"${root}/"}
    # <workload>/<id>/_<id>_<w>_.trace.zstd -> task name + prefix path.
    # prepare_env tries the exact path, then appends .zstd — so keep the
    # ".trace" stem (foo.trace.zstd -> foo.trace) and let the suffix match
    # re-attach .zstd.
    task=$(dirname "${rel}" | tr '/' '-')
    prefix="${rel%.zstd}"              # strip only .zstd

    log="${trace%.zstd}.log"
    if [[ ! -f "${log}" ]]; then
        echo "Warning: no sibling log for ${rel}; dw/sample default to 0" >&2
        echo "${task} ${prefix} 0 0 0 0" >> "${out}"
        n=$((n+1))
        continue
    fi

    # Only the "to" numbers are transformed-record counts (see header).
    # `|| true` keeps a log without interval lines from aborting the whole
    # generation under set -euo pipefail; the :-0 fallbacks apply below.
    dw=$(grep -oE 'Warmup Interval from [0-9]+ to [0-9]+' "${log}" | tail -1 \
        | awk '{print $NF}' || true)
    sample=$(grep -oE 'Sampling Interval from [0-9]+ to [0-9]+' "${log}" \
        | tail -1 | awk '{print $NF}' || true)

    dw=${dw:-0}
    sample=${sample:-0}

    echo "${task} ${prefix} 0 0 ${dw} ${sample}" >> "${out}"
    n=$((n+1))
done

echo "Wrote ${n} workloads to ${out}"
