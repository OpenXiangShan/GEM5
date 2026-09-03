#!/usr/bin/env bash
# Idempotent bootstrap for the XS-GEM5 Cloud Agent environment.
# Prepares system toolchains, builds the bundled DRAMSim3 memory model, and
# builds the optimized RISC-V gem5 binary. Safe to re-run: every step is
# guarded so an incremental rerun only rebuilds what actually changed.
set -euo pipefail

GEM5_HOME="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$GEM5_HOME"
export GEM5_HOME

JOBS="$(nproc)"

# 1. System dependencies (per README "Setup on Ubuntu 24.04"). Skip the apt
#    round-trip when the key toolchain is already present (e.g. baked into the
#    environment snapshot).
if ! command -v scons >/dev/null 2>&1 || ! command -v protoc >/dev/null 2>&1; then
    sudo apt-get update -y
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential git m4 scons zlib1g zlib1g-dev \
        libprotobuf-dev protobuf-compiler libprotoc-dev libgoogle-perftools-dev \
        python3-dev libboost-all-dev pkg-config libsqlite3-dev zstd libzstd-dev cmake
fi

# 2. DRAMSim3 backs the XiangShan (Kunminghu v3) memory model. Build it once;
#    the shared library is what gem5 links against, so its presence is the
#    idempotence guard. The default `c++` alternative is clang++, which cannot
#    locate libstdc++ here, so pin gcc/g++ explicitly (matches CI).
if [ ! -f ext/dramsim3/DRAMsim3/libdramsim3.so ]; then
    (
        cd ext/dramsim3
        if [ ! -d DRAMsim3 ]; then
            git clone --depth 1 https://github.com/umd-memsys/DRAMsim3.git DRAMsim3
        fi
        cd DRAMsim3
        mkdir -p build
        cd build
        CC=gcc CXX=g++ cmake -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ ..
        make -j"$JOBS"
    )
fi

# 3. Build the optimized RISC-V binary. Pin gcc/g++ (project/CI compiler) and
#    use the gold linker as documented. `--install-hooks` installs gem5's git
#    hooks non-interactively so the build never blocks on the style-hook prompt.
CC=gcc CXX=g++ scons build/RISCV/gem5.opt --linker=gold --install-hooks -j"$JOBS"

echo "XS-GEM5 environment bootstrap complete: build/RISCV/gem5.opt is ready."
