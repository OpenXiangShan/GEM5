#!/usr/bin/env bash

# Micro-corpus trace regression driver (L4).
#
# Runs every synthetic micro trace through the RISCV gem5.opt trace mode
# and asserts a clean exit (exit 0 + a clean-exit marker, never a
# panic/abort). Note on the EOF reconciliation invariant: it is enforced
# at runtime (Commit::traceMaybeExitOnEofDrainFromTick panic_if) but only
# on the "reached EOF and drained" exit path; micro corpora usually exit
# earlier via "committed last traced instruction", so the reconciliation
# itself is exercised by long traces, not by this sweep.
#
# Each corpus trace replays a historical bug's trigger sequence (see
# gen_micro_corpus.py); a full sweep finishes in seconds-to-minutes.
# fetch-stall-2000 is a known-failing corpus (pre-existing branch bug,
# reproduced on the baseline binary): it is expected to fail and reported
# as XFAIL; an XPASS report means the bug got fixed — flip its expectation.
#
# Usage:
#   bash run_micro_corpus.sh [GEM5_OPT] [CORPUS_DIR] [OUT_DIR]

set -uo pipefail
shopt -s nullglob

GEM5=${1:-./build/RISCV/gem5.opt}
CORPUS_DIR=${2:-/tmp/micro_corpus}
OUT_DIR=${3:-/tmp/micro_corpus_out}

script_dir=$(dirname -- "$(readlink -f -- "$0")")
# common.sh locates gem5_home for run_trace_champsim.sh; a missing source
# is a real error (the driver cannot run without it).
source "${script_dir}/../common.sh"

if [ ! -x "${GEM5}" ]; then
    echo "gem5 binary not found or not executable: ${GEM5}" >&2
    exit 2
fi
if [ -z "$(ls -A "${CORPUS_DIR}"/*.champsimtrace 2>/dev/null)" ]; then
    echo "no .champsimtrace corpus files in ${CORPUS_DIR}" >&2
    exit 2
fi

mkdir -p "${OUT_DIR}"

pass=0
fail=0
known_failing="fetch-stall-2000"  # pre-existing branch bug; expected to FAIL
failed=()

for trace in "${CORPUS_DIR}"/*.champsimtrace; do
    name=$(basename "${trace}" .champsimtrace)
    log="${OUT_DIR}/${name}.log"
    # Small instruction budget: corpora are <100 records.
    if XS_MAX_INSTS=100000 TRACE_FORMAT=champsim \
        bash "${script_dir}/run_trace_champsim.sh" -n 100000 "${trace}" \
        > "${log}" 2>&1; then
        # Two clean exit markers are acceptable:
        #   "committed last traced instruction" — the commit-side last-record
        #       path fires before the EOF-drain check (micro corpora fit in
        #       the pipeline window);
        #   "reached EOF and drained"/"drained (EOF or maxinsts)" — the
        #       tick-side EOF+drain path, which additionally enforces the
        #       L3 EOF reconciliation invariant (committed == reader idx).
        if grep -q "committed last traced instruction\|Trace-driven CPU reached EOF and drained\|Trace-driven CPU drained" "${log}"; then
            if [[ " ${known_failing} " == *" ${name} "* ]]; then
                echo "XPASS ${name}: known-failing corpus now passes — flip its expectation"
                pass=$((pass + 1))
            else
                echo "PASS  ${name}"
                pass=$((pass + 1))
            fi
        else
            if [[ " ${known_failing} " == *" ${name} "* ]]; then
                echo "XFAIL ${name}: expected failure (pre-existing fetch stall)"
                pass=$((pass + 1))
            else
                echo "FAIL  ${name}: exit 0 but no clean-exit marker (maxinsts?)"
                fail=$((fail + 1))
                failed+=("${name}")
            fi
        fi
    else
        rc=$?
        if [[ " ${known_failing} " == *" ${name} "* ]]; then
            echo "XFAIL ${name}: expected failure (pre-existing fetch stall), exit ${rc}"
            pass=$((pass + 1))
        else
            echo "FAIL  ${name}: exit ${rc}"
            # Surface the failure signature for triage.
            grep -m3 -E "panic|abort|fatal|Error" "${log}" || true
            fail=$((fail + 1))
            failed+=("${name}")
        fi
    fi
done

echo
echo "micro-corpus result: ${pass} passed, ${fail} failed"
if [ ${fail} -gt 0 ]; then
    printf 'failed: %s\n' "${failed[@]}"
    exit 1
fi
exit 0
