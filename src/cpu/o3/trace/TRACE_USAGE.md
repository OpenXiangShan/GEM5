# XiangShan Trace-Driven Simulation Guide

This guide explains how to run trace-driven simulations using the XiangShan O3CPU model with ChampSim and CBP2025 traces.

## Quick Start

### 1. Build GEM5 with Trace Support

```bash
# Build optimized version
scons build/RISCV/gem5.opt -j8

# Set environment variable
export gem5_home=$(pwd)
```

### 2. Basic Trace Replay

```bash
# Run ChampSim trace
./build/RISCV/gem5.opt configs/example/xiangshan.py \
    --enable-trace-mode \
    --trace-file=/path/to/your_trace.bin \
    --trace-format=champsim \
    --trace-max-insts=1000000

# Using the helper script
./util/xs_scripts/trace_example.sh /path/to/your_trace.bin
```

### 3. View Results

```bash
# Check key performance metrics
grep "system.cpu.ipc" m5out/stats.txt
grep "system.cpu.committedInsts" m5out/stats.txt
grep "system.cpu.numCycles" m5out/stats.txt
```

## Detailed Usage

### Supported Trace Formats

#### ChampSim Traces (Fully Supported)
- **Format**: Binary instruction traces from ChampSim simulator
- **Content**: PC, branch info, memory addresses, register dependencies
- **File Extension**: Usually `.bin` or `.trace`
- **Source**: ChampSim tracer or compatible tools

```bash
# Example ChampSim trace replay
./util/xs_scripts/trace_example.sh \
    -f champsim \
    -n 5000000 \
    -d "Fetch,TraceReader,O3CPU" \
    /path/to/champsim_trace.bin
```

#### CBP2025 Traces (Framework Ready)
- **Format**: Branch prediction competition traces
- **Status**: Framework implemented, parser needs completion
- **File Extension**: Usually `.gz` or `.trace.gz`

```bash
# CBP2025 trace replay (when implemented)
./util/xs_scripts/trace_example.sh \
    -f cbp2025 \
    /path/to/cbp_trace.gz
```

### Configuration Options

#### Command Line Parameters

```bash
./build/RISCV/gem5.opt configs/example/xiangshan.py [OPTIONS]

Required for trace mode:
  --enable-trace-mode      Enable trace-driven simulation
  --trace-file=PATH        Path to the trace file

Optional:
  --trace-format=FORMAT    Trace format: champsim|cbp2025 (default: champsim)
  --trace-max-insts=N     Maximum instructions to simulate (default: 1M)
  --debug-flags=FLAGS     Debug output flags (default: none)
  --stats-file=PATH       Statistics output file (default: m5out/stats.txt)
```

#### Helper Script Options

```bash
./util/xs_scripts/trace_example.sh [OPTIONS] <trace_file>

Options:
  -h, --help              Show help message
  -o, --output DIR        Output directory (default: m5out/trace_sim)
  -n, --max-insts N       Maximum instructions (default: 1M)
  -f, --format FMT        Trace format: champsim|cbp2025
  -d, --debug FLAGS       Debug flags (comma-separated)
  -v, --verbose           Enable verbose output
  --no-debug              Disable debug output

Examples:
  # Basic replay
  ./util/xs_scripts/trace_example.sh trace.bin
  
  # With specific parameters
  ./util/xs_scripts/trace_example.sh -n 500000 -f champsim trace.bin
  
  # With debug output
  ./util/xs_scripts/trace_example.sh -d "Fetch,TraceReader" trace.bin
```

### Advanced Configuration

#### Python Configuration Script

You can also create custom configuration scripts based on `configs/example/xiangshan.py` with trace mode enabled:

```python
#!/usr/bin/env python3
import m5
from m5.objects import *
from common import XSConfig
from xiangshan import XiangshanCore

# Create system
system = System()
system.clk_domain = SrcClockDomain()
system.clk_domain.clock = '3GHz'
system.clk_domain.voltage_domain = VoltageDomain()

# Create XiangShan CPU with trace mode
system.cpu = XiangshanCore()
system.cpu.enableTraceMode = True
system.cpu.traceFile = "/path/to/your/trace.bin"
system.cpu.traceFormat = "champsim"

# ... rest of system setup
```

#### CPU Parameters

The trace mode adds these parameters to the XiangShan CPU:

```python
# In your configuration
cpu.enableTraceMode = True           # Enable trace-driven simulation
cpu.traceFile = "trace.bin"          # Path to trace file  
cpu.traceFormat = "champsim"         # Trace format
```

## Performance Analysis

### Key Statistics

After simulation, check these important metrics in `m5out/stats.txt`:

#### Core Performance
```bash
# Instructions Per Cycle
grep "system.cpu.ipc" m5out/stats.txt

# Total instructions executed
grep "system.cpu.committedInsts" m5out/stats.txt

# Simulation cycles
grep "system.cpu.numCycles" m5out/stats.txt
```

#### Cache Performance
```bash
# L1 I-Cache miss rate
grep "system.cpu.icache.overall_miss_rate::total" m5out/stats.txt

# L1 D-Cache miss rate  
grep "system.cpu.dcache.overall_miss_rate::total" m5out/stats.txt

# Cache hits/misses
grep "system.cpu.dcache.overall_hits" m5out/stats.txt
grep "system.cpu.dcache.overall_misses" m5out/stats.txt
```

#### Pipeline Statistics
```bash
# Fetch stage performance
grep "system.cpu.fetch" m5out/stats.txt

# Branch prediction accuracy
grep "system.cpu.branchPred" m5out/stats.txt

# ROB and pipeline utilization
grep "system.cpu.rob" m5out/stats.txt
grep "system.cpu.iq" m5out/stats.txt
```

### Comparison with ChampSim

To validate results, compare key metrics with ChampSim:

| Metric | GEM5 XiangShan | ChampSim | Notes |
|--------|----------------|----------|-------|
| IPC | `system.cpu.ipc` | ChampSim IPC | Should be similar |
| Cache Miss Rate | `dcache.overall_miss_rate` | ChampSim cache stats | Architecture differences expected |
| Branch Accuracy | `branchPred.condPredicted` | ChampSim branch stats | Predictor differences expected |

## Debug and Troubleshooting

### Enable Debug Output

```bash
# Common debug flags
--debug-flags=Fetch,TraceReader          # Trace reading and fetch
--debug-flags=O3CPU,Fetch                # CPU pipeline and fetch
--debug-flags=Cache,LSQ                  # Memory system
--debug-flags=BPred                      # Branch prediction

# Multiple flags
--debug-flags=Fetch,TraceReader,O3CPU,Cache
```

### Common Issues

#### 1. Trace File Not Found
```
Error: Trace file not found: /path/to/trace.bin
```
**Solution**: Verify file path and permissions
```bash
ls -la /path/to/trace.bin
```

#### 2. Empty Statistics
```
All statistics show zero values
```
**Solution**: Check trace file format and max instruction limit
```bash
# Try with higher instruction limit
--max-insts=10000000

# Verify trace format
file /path/to/trace.bin
```

#### 3. Simulation Hangs
**Solution**: Enable debug output to identify bottleneck
```bash
--debug-flags=Fetch,TraceReader
```

#### 4. Memory Errors
```
Segmentation fault or memory corruption
```
**Solution**: Check trace file integrity and format compatibility
```bash
# Verify file is not corrupted
hexdump -C trace.bin | head -n 10
```

### Validation Steps

1. **Check trace file size**: Should be multiple of instruction size
```bash
# ChampSim instruction size is typically 32 bytes
stat -c%s trace.bin
```

2. **Verify instruction count**: Compare with expected trace length
```bash
# Calculate expected instructions
echo $(($(stat -c%s trace.bin) / 32))
```

3. **Monitor progress**: Use verbose mode to track simulation
```bash
./util/xs_scripts/trace_example.sh -v trace.bin
```

## Advanced Features

### Branch Prediction Evaluation

The trace system enables detailed branch predictor analysis:

```bash
# Enable branch prediction debugging
--debug-flags=BPred,Fetch

# Check branch prediction statistics
grep "system.cpu.branchPred" m5out/stats.txt
```

### Memory System Analysis

Analyze cache behavior with trace memory patterns:

```bash
# Memory system debugging
--debug-flags=Cache,LSQ,MemDepUnit

# Cache hierarchy performance
grep -E "(icache|dcache|l2)" m5out/stats.txt
```

### Custom Trace Processing

For custom analysis, access trace metadata in the simulator:

```cpp
// In your custom analysis code
if (cpu->isTraceInstruction(inst->seqNum)) {
    const auto* traceInst = cpu->getTraceInstMetadata(inst->seqNum);
    // Use trace information for analysis
}
```

## Performance Tips

### 1. Optimize Simulation Speed
```bash
# Use optimized build
scons build/RISCV/gem5.opt

# Limit instruction count for faster iterations
--max-insts=100000

# Disable unnecessary debug output
--no-debug
```

### 2. Batch Processing
```bash
# Process multiple traces
for trace in traces/*.bin; do
    ./util/xs_scripts/trace_example.sh "$trace"
done
```

### 3. Parallel Simulation
```bash
# Use the parallel simulation script
./util/xs_scripts/parallel_sim.sh \
    $(realpath ./util/xs_scripts/trace_example.sh) \
    trace_list.txt \
    /path/to/traces \
    simulation_tag
```

## Output Files

After simulation, you'll find these files in the output directory:

```
m5out/trace_sim/
├── config.ini          # Complete system configuration
├── stats.txt           # Detailed performance statistics  
├── trace_info.txt      # Trace simulation metadata
└── simulation.log      # Console output and debug messages
```

### Key Output Information

#### trace_info.txt
```
XiangShan Trace-Driven Simulation Information
==============================================

Simulation Parameters:
  Trace File: /path/to/trace.bin
  Trace Format: champsim
  Max Instructions: 1000000
  
System Configuration:
  CPU Model: XiangShan O3CPU
  Pipeline: 5-stage out-of-order
  Trace Mode: Enabled
```

#### Performance Summary
The helper script automatically extracts key metrics:
```
============================================
Key Performance Metrics:
------------------------
Instructions Per Cycle (IPC): 2.145
Instructions Committed: 1000000
Simulation Cycles: 466321
L1 I-Cache Miss Rate: 0.001245
L1 D-Cache Miss Rate: 0.051234
============================================
```

## Extending the System

### Adding New Trace Formats

1. **Create new trace reader class**:
```cpp
class MyTraceReader : public TraceReader {
    // Implement required virtual methods
};
```

2. **Add to factory in TraceReader.cc**:
```cpp
if (format == "myformat") {
    return std::make_unique<MyTraceReader>(trace_file, name);
}
```

3. **Update configuration options** in xiangshan_trace.py

### Custom Analysis

Implement custom analysis hooks by accessing trace metadata in pipeline stages.

## References

- [ChampSim Simulator](https://github.com/ChampSim/ChampSim)
- [CBP-2025 Competition](https://www.microarch.org/JWAC1/2025/)
- [XiangShan Processor](https://github.com/OpenXiangShan/XiangShan) 
- [GEM5 Simulator](https://www.gem5.org/)
- [Trace System Documentation](README.md)