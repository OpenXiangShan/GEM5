#!/bin/bash

export GEM5_HOME=$(pwd)

# build DRAMSim
cd ext/dramsim3
git clone https://github.com/umd-memsys/DRAMsim3.git DRAMsim3
cd DRAMsim3 && mkdir -p build
cd build
cmake ..
make -j 48
cd $GEM5_HOME
