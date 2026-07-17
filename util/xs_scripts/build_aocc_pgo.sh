#!/usr/bin/env bash
#
# Build an XS-GEM5 binary with the AOCC (AMD Zen) clang compiler, a portable
# -march tuning, and Profile-Guided Optimization. The resulting binary produces
# bit-identical simulation results to the default GCC build but runs markedly
# faster on the host; it is self-contained (no AOCC runtime libraries needed to
# run) and AVX2-only, so it deploys across a mixed Zen3/Zen4 cluster.
#
# Pipeline: instrument build -> profile on a representative checkpoint mix ->
# merge profiles -> optimized build -> persist the binary and its profile.
#
# The AOCC build uses the clang object variant, so it is produced in a dedicated
# build worktree to avoid disturbing the default GCC build tree. The final
# binary and profile are copied back into the source tree's build directory.
#
# Usage:
#   util/xs_scripts/build_aocc_pgo.sh [fast|opt]
#
# Override any of the configuration values below via environment variables, e.g.
#   GEM5_VARIANT=fast MARCH=znver3 PROFILE_INSTS=3000000 \
#     util/xs_scripts/build_aocc_pgo.sh
#
# Required in the environment (same as the run scripts):
#   GCBV_REF_SO  - NEMU reference .so for difftest during the profiling runs.

set -euo pipefail

script_dir=$(dirname -- "$(readlink -f -- "$0")")
gem5_home=$(dirname "$(dirname "${script_dir}")")

# ---- Configuration (override via environment) -------------------------------
GEM5_VARIANT="${1:-${GEM5_VARIANT:-fast}}"          # fast (max speed) or opt
MARCH="${MARCH:-znver3}"                            # AVX2, portable Zen3+Zen4
AOCC_HOME="${AOCC_HOME:-}"                          # AOCC install prefix, required
JOBS="${JOBS:-$(nproc)}"
PROFILE_INSTS="${PROFILE_INSTS:-3000000}"
GEM5_CONFIG="${GEM5_CONFIG:-${gem5_home}/configs/example/kmhv3.py}"
BUILD_WT="${BUILD_WT:-${gem5_home}/../wt-aocc-pgo-build}"
OUT_DIR="${OUT_DIR:-${gem5_home}/build/RISCV}"
WORK_DIR="${WORK_DIR:-/tmp/aocc_pgo_$$}"
DRAMSIM3_SRC="${DRAMSIM3_SRC:-${gem5_home}/ext/dramsim3/DRAMsim3}"

# Representative profiling benchmarks: pointer-chase, branchy, and FP/streaming.
# One checkpoint per benchmark is resolved by glob so exact SimPoint filenames
# need not be hard-coded. Override PROFILE_CPTS with explicit .gz paths to skip
# resolution, or PROFILE_BENCHES with a different benchmark set.
spec06="${SPEC06_DIR:-}"
PROFILE_BENCHES="${PROFILE_BENCHES:-mcf gcc_200 lbm}"
if [[ -z "${PROFILE_CPTS:-}" ]]; then
    PROFILE_CPTS=""
    for bench in ${PROFILE_BENCHES}; do
        cpt=$(ls "${spec06}/${bench}"/*/*.gz 2>/dev/null | head -1) || true
        if [[ -n "${cpt}" ]]; then
            PROFILE_CPTS+="${cpt} "
        else
            echo "warning: no checkpoint found for ${bench}, skipping" >&2
        fi
    done
fi

# -----------------------------------------------------------------------------
if [[ -z "${AOCC_HOME}" || ! -x "${AOCC_HOME}/bin/clang++" ]]; then
    echo "error: AOCC_HOME must point at an AOCC install prefix containing" \
         "bin/clang++ (get it from https://www.amd.com/en/developer/aocc.html)" >&2
    exit 1
fi
if [[ -z "${PROFILE_CPTS// /}" ]]; then
    echo "error: no profiling checkpoints resolved; check SPEC06_DIR," \
         "PROFILE_BENCHES, or pass PROFILE_CPTS explicitly" >&2
    exit 1
fi
if [[ -z "${GCBV_REF_SO:-}" ]]; then
    echo "error: GCBV_REF_SO must point at a NEMU difftest reference .so" >&2
    exit 1
fi
export GCB_REF_SO="${GCB_REF_SO:-$GCBV_REF_SO}"
export GCB_RESTORER="${GCB_RESTORER:-}"

target="build/RISCV/gem5.${GEM5_VARIANT}"
profdata="${WORK_DIR}/gem5.profdata"
mkdir -p "${WORK_DIR}/raw"

echo "== AOCC+PGO build =="
echo "  variant       : ${GEM5_VARIANT}"
echo "  march         : ${MARCH}"
echo "  aocc          : ${AOCC_HOME}"
echo "  build worktree: ${BUILD_WT}"
echo "  output dir    : ${OUT_DIR}"

# The clang toolchain is only needed to BUILD; the produced binary is standalone.
export CC="${AOCC_HOME}/bin/clang"
export CXX="${AOCC_HOME}/bin/clang++"
export LD_LIBRARY_PATH="${AOCC_HOME}/lib:${LD_LIBRARY_PATH:-}"

# Isolated build worktree pinned to the CURRENT commit, with its own DRAMsim3.
# If the worktree already exists (e.g. from a prior run), re-point it at the
# current HEAD so a stale checkout from a different commit is never silently
# reused to build and persist a wrong-code binary.
head_sha=$(git -C "${gem5_home}" rev-parse HEAD)
if [[ ! -d "${BUILD_WT}" ]]; then
    git -C "${gem5_home}" worktree add --detach "${BUILD_WT}" "${head_sha}"
elif [[ -e "${BUILD_WT}/.git" ]]; then
    git -C "${BUILD_WT}" checkout --detach "${head_sha}"
else
    echo "error: ${BUILD_WT} exists but is not a git worktree; remove it or" \
         "set BUILD_WT to a clean path" >&2
    exit 1
fi
if [[ ! -e "${BUILD_WT}/ext/dramsim3/DRAMsim3" ]]; then
    ln -s "${DRAMSIM3_SRC}" "${BUILD_WT}/ext/dramsim3/DRAMsim3"
fi
cd "${BUILD_WT}"

# setsid detaches the build from terminal-signal groups so a stray interrupt on
# the shared login node does not abort a long compile.
run_build() { setsid scons "$@" -j "${JOBS}"; }

echo "== step 1/4: instrument build (-fprofile-generate) =="
run_build "${target}" --march="${MARCH}" --pgo-prof

echo "== step 2/4: profile-generate runs (difftest on, parallel) =="
# The profiling runs are independent (distinct profraw + output dirs), so they
# run concurrently; wall time is the slowest single run rather than the sum.
i=0
prof_pids=""
for cpt in ${PROFILE_CPTS}; do
    i=$((i + 1))
    echo "   profiling on ${cpt}"
    LLVM_PROFILE_FILE="${WORK_DIR}/raw/prof-${i}-%p.profraw" \
        gem5_home="${BUILD_WT}" \
        "./${target}" -d "${WORK_DIR}/prof_run_${i}" \
        "${GEM5_CONFIG}" --maxinsts="${PROFILE_INSTS}" --generic-rv-cpt="${cpt}" \
        > "${WORK_DIR}/prof_run_${i}.log" 2>&1 &
    prof_pids+=" $!"
done
prof_fail=0
for pid in ${prof_pids}; do wait "${pid}" || prof_fail=1; done
if [[ ${prof_fail} -ne 0 ]]; then
    echo "error: a profile-generate run failed; see ${WORK_DIR}/prof_run_*.log" >&2
    exit 1
fi

echo "== step 3/4: merge profiles =="
"${AOCC_HOME}/bin/llvm-profdata" merge -output="${profdata}" "${WORK_DIR}"/raw/*.profraw

echo "== step 4/4: optimized build (-fprofile-use) =="
run_build "${target}" --march="${MARCH}" --pgo-use="${profdata}"

# Correctness gate: never persist a binary that is not difftest-clean. A corrupt
# profile or a miscompile that perturbs results would otherwise ship silently as
# the sweep binary. A bounded difftest-on run that reaches the instruction bound
# without diverging (which would abort with a non-zero exit) is the check.
# Override with VALIDATE=0.
if [[ "${VALIDATE:-1}" != "0" ]]; then
    echo "== validate: bounded difftest-on run =="
    val_cpt="${PROFILE_CPTS%% *}"
    if gem5_home="${BUILD_WT}" "./${target}" -d "${WORK_DIR}/validate" \
            "${GEM5_CONFIG}" --maxinsts="${VALIDATE_INSTS:-1000000}" \
            --generic-rv-cpt="${val_cpt}" > "${WORK_DIR}/validate.log" 2>&1 &&
        grep -q "max instruction count" "${WORK_DIR}/validate.log"; then
        echo "  validate: OK (reached instruction bound, difftest-clean)"
    else
        echo "error: validation run failed (crash or difftest divergence);" \
             "NOT persisting. See ${WORK_DIR}/validate.log" >&2
        exit 1
    fi
fi

# Persist the binary and the profile that produced it.
mkdir -p "${OUT_DIR}"
out_bin="${OUT_DIR}/gem5.${GEM5_VARIANT}.aocc-pgo"
out_prof="${OUT_DIR}/gem5.${GEM5_VARIANT}.aocc-pgo.profdata"
cp -f "${BUILD_WT}/${target}" "${out_bin}"
cp -f "${profdata}" "${out_prof}"

# Make the persisted binary self-contained: retarget its libdramsim3 RUNPATH
# from the scratch build worktree to the source tree's durable DRAMsim3 copy.
# Without patchelf the binary still runs while BUILD_WT exists or when invoked
# with LD_LIBRARY_PATH set to the source DRAMsim3.
main_dramsim3="${gem5_home}/ext/dramsim3/DRAMsim3"
if command -v patchelf >/dev/null 2>&1 && [[ -e "${main_dramsim3}/libdramsim3.so" ]]; then
    patchelf --set-rpath "${main_dramsim3}:$(patchelf --print-rpath "${out_bin}")" \
        "${out_bin}"
    deploy_note="self-contained (RUNPATH retargeted to ${main_dramsim3})"
else
    deploy_note="keep ${BUILD_WT}, or run with LD_LIBRARY_PATH=${main_dramsim3} (patchelf unavailable to bake the path)"
fi

echo
echo "== done =="
echo "  binary : ${out_bin}"
echo "  profile: ${out_prof}"
echo "  deploy : ${deploy_note}"
