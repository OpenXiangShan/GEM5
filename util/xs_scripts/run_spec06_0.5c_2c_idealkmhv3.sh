#!/usr/bin/env bash
#
# 简化版：使用 Xiangshan 配置 (configs/example/idealkmhv3.py) 跑 SPEC06-0.5c checkpoints。
# - 支持双核模式
# - 使用固定的 checkpoint 路径
#
# 用法示例：
#   bash util/xs_scripts/run_spec06_0.5c_2c_idealkmhv3.sh --tag spec_0.5c
#
# 必需环境变量（多核）：
#   - GCB_MULTI_CORE_RESTORER: multi-core GCPT restorer 路径
#   - GCBV_MULTI_CORE_REF_SO : NEMU ref so 路径（如需要 RVV 支持）
#
# 可选环境变量：
#   - GEM5_BUILD_TYPE: opt|fast|debug ...（默认 opt）
#   - xsgem5_para_jobs: GNU parallel 并发数（parallel_sim.sh 默认 63）
#
set -euo pipefail
set -x

script_dir=$(dirname -- "$(readlink -f -- "$0"; )")
gem5_home=$(dirname "$(dirname "$script_dir")")

function usage() {
  cat <<'EOF'
Usage:
  bash util/xs_scripts/run_spec06_0.5c_2c_idealkmhv3.sh [options]

Options:
  --num-cpus <N>
      Simulated core count passed to idealkmhv3.py. Default: 2

  --tag <name>
      Task tag / output directory under util/xs_scripts/test/. Default: spec_0.5c

  -h, --help
      Show this help and exit.

Required env (multi-core):
  GCB_MULTI_CORE_RESTORER, GCBV_MULTI_CORE_REF_SO

Examples:
  bash util/xs_scripts/run_spec06_0.5c_2c_idealkmhv3.sh --tag spec_0.5c
  bash util/xs_scripts/run_spec06_0.5c_2c_idealkmhv3.sh --num-cpus 2 --tag my_run
EOF
}

# Defaults
TAG="spec_0.5c"
NUM_CPUS=2
checkpoint_list="/nfs/home/wuchengkai/benchmark_res/dual_core/checkpoint_dualcore_0.5c.lst"
checkpoint_root_node="/nfs/home/jiaxiaoyu/emu_dual_core/disable_timer/checkpoint-0-0-0"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --num-cpus)
      NUM_CPUS="${2:-}"; shift 2 ;;
    --tag)
      TAG="${2:-}"; shift 2 ;;
    -h|--help)
      usage; exit 0 ;;
    *)
      echo "ERROR: Unknown argument: $1" >&2
      usage
      exit 1 ;;
  esac
done

# Basic sanity checks
if [[ ! -f "$checkpoint_list" ]]; then
  echo "ERROR: checkpoint_list not found: $checkpoint_list" >&2
  exit 1
fi
if [[ ! -d "$checkpoint_root_node" ]]; then
  echo "ERROR: checkpoint_root_node not found: $checkpoint_root_node" >&2
  exit 1
fi

# Multi-core specific env required by configs/common/xiangshan.py
: "${GCB_MULTI_CORE_RESTORER:?Please export GCB_MULTI_CORE_RESTORER=/abs/path/to/multi_core_gcpt_restorer}"
: "${GCBV_MULTI_CORE_REF_SO:?Please export GCBV_MULTI_CORE_REF_SO=/abs/path/to/riscv64-nemu-*-so}"

# Also set the single-core names if your other tooling expects them.
export GCB_RESTORER="${GCB_RESTORER:-$GCB_MULTI_CORE_RESTORER}"
export GCBV_REF_SO="${GCBV_REF_SO:-$GCBV_MULTI_CORE_REF_SO}"

export GEM5_HOME="$gem5_home"
export GEM5_BUILD_TYPE="${GEM5_BUILD_TYPE:-opt}"

gem5_bin="$GEM5_HOME/build/RISCV_CHI/gem5.$GEM5_BUILD_TYPE"
if [[ ! -x "$gem5_bin" ]]; then
  echo "ERROR: gem5 binary not found/executable: $gem5_bin" >&2
  echo "Hint: build it via: scons build/RISCV_CHI/gem5.$GEM5_BUILD_TYPE -j\$(nproc)" >&2
  exit 1
fi

# Work directory (match CI convention under util/xs_scripts/test)
work_root="$GEM5_HOME/util/xs_scripts/test"
mkdir -p "$work_root"
cd "$work_root"

# Create a temporary arch script compatible with parallel_sim.sh:
# it will be called as: bash $arch_script <checkpoint_path>
arch_script_tmp="$(mktemp -p "$work_root" arch_idealkmhv3_2c.XXXXXX.sh)"
chmod +x "$arch_script_tmp"
cat > "$arch_script_tmp" <<EOF
#!/usr/bin/env bash
set -euo pipefail

ckpt="\$1"

: "\${GEM5_HOME:?GEM5_HOME must be set}"
: "\${GEM5_BUILD_TYPE:?GEM5_BUILD_TYPE must be set}"
: "\${GCB_MULTI_CORE_RESTORER:?GCB_MULTI_CORE_RESTORER must be set for multi-core}"
: "\${GCBV_MULTI_CORE_REF_SO:?GCBV_MULTI_CORE_REF_SO must be set for multi-core}"

gem5_bin="\$GEM5_HOME/build/RISCV_CHI/gem5.\$GEM5_BUILD_TYPE"

exec "\$gem5_bin" \\
  --outdir="\$(pwd)" \\
  "\$GEM5_HOME/configs/example/xiangshan_test.py" \\
  --num-cpus="$NUM_CPUS" \\
  --ruby \\
  --generic-rv-cpt="\$ckpt"
EOF

# Run the suite (parallel over checkpoints; each checkpoint uses 2 simulated cores)
bash "$GEM5_HOME/util/xs_scripts/parallel_sim.sh" \
  "$arch_script_tmp" \
  "$checkpoint_list" \
  "$checkpoint_root_node" \
  "$TAG"