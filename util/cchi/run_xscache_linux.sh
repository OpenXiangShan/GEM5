#!/usr/bin/env bash
# run_xscache_linux.sh — boot linux on XS-GEM5 with the XSCache
# (Oceanus L2 + OpenLLC + OpenNCB) RTL as the CCHI downstream endpoint.
#
# Usage (builds the RISCV_CCHI_XSCACHE variant on first run):
#   util/cchi/run_xscache_linux.sh --xscache-dir=<path> --chiron-dir=<path> \
#       --ready-to-run=<path> [options]
#
# Required paths (CLI flag wins over the environment variable; there are
# no built-in defaults — the script errors out when one is missing):
#   --xscache-dir=PATH   XSCache checkout    (env: XSCACHE_DIR)
#   --chiron-dir=PATH    CHIron checkout     (env: CHIRON_DIR)
#   --ready-to-run=PATH  workload directory  (env: READY_TO_RUN)
# Derived from --ready-to-run unless given explicitly:
#   --gcbv-ref-so=PATH   difftest reference .so (env: GCBV_REF_SO;
#                        default: $READY_TO_RUN/riscv64-nemu-interpreter-so)
#   --linux-bin=PATH     raw linux image        (env: LINUX_BIN;
#                        default: $READY_TO_RUN/linux.bin)
# Optional:
#   --outdir=PATH        gem5 output directory  (env: OUTDIR;
#                        default: <repo>/m5out_linux_xscache)
#
# Options:
#   --build            force a rebuild of the RISCV_CCHI_XSCACHE variant
#   -I N               cap at N committed instructions (gem5 -I)
#   --difftest         run WITH difftest (default is --disable-difftest; the
#                      NEMU/gem5 interrupt-timing divergence on linux boots
#                      is pre-existing and unrelated to CCHI)
#   --flit-trace       attach the CCHI flit logger (very verbose, to stdout)
#   --debug-flags=F    gem5 debug flags (e.g. --debug-flags=CCHI)
#   --vcd=PATH         dump the downstream RTL waveform (VCD) to PATH
#   --vcd-start=TICK   open the waveform at gem5 tick TICK (default: 0)
#   -- ...             everything after a bare -- is passed to gem5 verbatim
#
# Prerequisites the script checks for you:
#   - generated XSCache RTL ($XSCACHE_DIR/build/l2openllc/TestTop_L2OpenLLC.sv);
#     if missing, run `make test-top-l2openllc` in the XSCache checkout
set -euo pipefail

GEM5_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$GEM5_ROOT"

XSCACHE_DIR="${XSCACHE_DIR:-}"
CHIRON_DIR="${CHIRON_DIR:-}"
READY_TO_RUN="${READY_TO_RUN:-}"
GCBV_REF_SO="${GCBV_REF_SO:-}"
LINUX_BIN="${LINUX_BIN:-}"
OUTDIR="${OUTDIR:-$GEM5_ROOT/m5out_linux_xscache}"

VARIANT="build/RISCV_CCHI_XSCACHE"
GEM5_BIN="$GEM5_ROOT/$VARIANT/gem5.opt"
SCONS="$GEM5_ROOT/.venv/bin/scons"
[ -x "$SCONS" ] || SCONS="scons"

BUILD=0
MAXINSTS=""
DIFFTEST_OFF=1
FLIT_TRACE=0
DEBUG_FLAGS=""
VCD=""
VCD_START=0
EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        --xscache-dir=*)  XSCACHE_DIR="${1#*=}" ;;
        --chiron-dir=*)   CHIRON_DIR="${1#*=}" ;;
        --ready-to-run=*) READY_TO_RUN="${1#*=}" ;;
        --gcbv-ref-so=*)  GCBV_REF_SO="${1#*=}" ;;
        --linux-bin=*)    LINUX_BIN="${1#*=}" ;;
        --outdir=*)       OUTDIR="${1#*=}" ;;
        --build)       BUILD=1 ;;
        -I)            MAXINSTS="$2"; shift ;;
        --difftest)    DIFFTEST_OFF=0 ;;
        --flit-trace)  FLIT_TRACE=1 ;;
        --debug-flags=*) DEBUG_FLAGS="${1#*=}" ;;
        --vcd=*)       VCD="${1#*=}" ;;
        --vcd-start=*) VCD_START="${1#*=}" ;;
        --)            shift; EXTRA+=("$@"); break ;;
        *) echo "unknown option: $1 (see header)" >&2; exit 2 ;;
    esac
    shift
done

# --- required paths ------------------------------------------------------------
missing() {
    echo "error: missing required path: $1" >&2
    echo "       pass $2 or set the \$$3 environment variable" >&2
    exit 2
}
[ -n "$XSCACHE_DIR" ]  || missing "XSCache checkout"  "--xscache-dir=PATH"  "XSCACHE_DIR"
[ -n "$CHIRON_DIR" ]   || missing "CHIron checkout"   "--chiron-dir=PATH"   "CHIRON_DIR"
[ -n "$READY_TO_RUN" ] || missing "workload directory" "--ready-to-run=PATH" "READY_TO_RUN"
GCBV_REF_SO="${GCBV_REF_SO:-$READY_TO_RUN/riscv64-nemu-interpreter-so}"
LINUX_BIN="${LINUX_BIN:-$READY_TO_RUN/linux.bin}"

# --- prerequisite checks -------------------------------------------------------
[ -d "$CHIRON_DIR" ]  || { echo "error: not a directory (CHIron checkout): $CHIRON_DIR" >&2; exit 1; }
[ -d "$XSCACHE_DIR" ] || { echo "error: not a directory (XSCache checkout): $XSCACHE_DIR" >&2; exit 1; }
if [ ! -f "$XSCACHE_DIR/build/l2openllc/TestTop_L2OpenLLC.sv" ]; then
    echo "error: $XSCACHE_DIR/build/l2openllc has no generated RTL;" >&2
    echo "       run 'make test-top-l2openllc' in the XSCache checkout first" >&2
    exit 1
fi
for f in "$LINUX_BIN" "$GCBV_REF_SO"; do
    [ -f "$f" ] || { echo "error: not found: $f" >&2; exit 1; }
done

# --- build ---------------------------------------------------------------------
if [ "$BUILD" = 1 ] || [ ! -x "$GEM5_BIN" ]; then
    "$SCONS" "$VARIANT/gem5.opt" --gold-linker -j"$(nproc)" \
        CHIRON_DIR="$CHIRON_DIR" CCHI_XSCACHE_DIR="$XSCACHE_DIR"
fi

# --- run -----------------------------------------------------------------------
ARGS=(--outdir="$OUTDIR" configs/example/kmhv3.py
      --cchi --cchi-downstream=rtl --no-cchi-l2-pf
      --raw-cpt --generic-rv-cpt="$LINUX_BIN")
[ "$DIFFTEST_OFF" = 1 ] && ARGS+=(--disable-difftest)
[ "$FLIT_TRACE" = 1 ]   && ARGS+=(--cchi-flit-trace)
[ -n "$DEBUG_FLAGS" ]   && ARGS+=(--debug-flags="$DEBUG_FLAGS")
[ -n "$MAXINSTS" ]      && ARGS+=(-I "$MAXINSTS")
[ ${#EXTRA[@]} -gt 0 ]  && ARGS+=("${EXTRA[@]}")

if [ -n "$VCD" ]; then
    export CCHI_RTL_TRACE="$VCD"
    export CCHI_RTL_TRACE_START="$VCD_START"
fi

echo "+ GCBV_REF_SO=$GCBV_REF_SO $GEM5_BIN ${ARGS[*]}"
exec env GCBV_REF_SO="$GCBV_REF_SO" "$GEM5_BIN" "${ARGS[@]}"
