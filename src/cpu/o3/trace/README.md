# O3CPU Trace-Driven Simulation

This directory contains the trace-driven simulation infrastructure for the GEM5 O3CPU (XiangShan). This system enables performance modeling using external instruction traces while maintaining the full pipeline timing model.

## Overview

The trace-driven simulation system allows you to:
- Replay ChampSim binary instruction traces on the XiangShan O3CPU
- Evaluate branch predictors using trace outcomes
- Analyze memory system behavior with trace memory patterns
- Compare performance across different microarchitectural configurations

## Key Features

- **Pipeline Preservation**: Full O3CPU pipeline timing model maintained
- **Zero Impact**: No performance impact when trace mode disabled
- **Modular Design**: Easy to add new trace formats
- **Branch Prediction**: Integration with XiangShan's sophisticated branch predictors
- **Memory Integration**: Works with existing cache hierarchy and LSQ

## Architecture

### Core Components

1. **TraceInstruction.hh** - Unified instruction representation
2. **TraceReader.hh/cc** - Abstract base class for trace readers
3. **ChampSimTraceReader.hh/cc** - ChampSim binary trace parser
4. **CBP2025TraceReader.hh/cc** - CBP2025 trace parser (stub)

### Integration Points

- **Fetch Stage**: Modified to optionally source instructions from traces
- **BaseO3CPU.py**: Added trace mode parameters
- **SConscript**: Build system integration

## Usage

### Basic Example

```bash
# Build GEM5 with trace support
scons build/RISCV/gem5.opt -j8

# Run trace-driven simulation
./build/RISCV/gem5.opt configs/example/xiangshan_trace.py \
    --trace-file=/path/to/champsim_trace.bin \
    --trace-format=champsim \
    --max-insts=1000000 \
    --debug-flags=Fetch,TraceReader
```

### Configuration Parameters

```python
# Enable trace mode in configuration
cpu.enableTraceMode = True
cpu.traceFile = "/path/to/trace.bin"
cpu.traceFormat = "champsim"  # or "cbp2025"
```

## Trace Formats

### ChampSim Format

ChampSim traces contain:
- Instruction PC
- Branch information (taken/not taken)
- Memory addresses for loads/stores
- Register dependencies

**Structure** (from ChampSim's `input_instr`):
```cpp
struct input_instr {
    uint64_t ip;                    // Program counter
    uint8_t is_branch;              // Branch flag
    uint8_t branch_taken;           // Branch outcome
    uint8_t destination_registers[2]; // Output registers
    uint8_t source_registers[4];    // Input registers
    uint64_t destination_memory[2]; // Store addresses
    uint64_t source_memory[4];      // Load addresses
};
```

### CBP2025 Format (Future)

CBP2025 traces will contain:
- Detailed branch information
- Instruction classes
- Cycle-accurate timing (ignored by O3CPU)

## Implementation Details

### Instruction Creation

The system creates appropriate RISC-V instructions based on trace types:

```cpp
// Examples of generated instructions
LOAD     -> LW (0x00002003)      // lw x0, 0(x0)
STORE    -> SW (0x00002023)      // sw x0, 0(x0)
BRANCH   -> BEQ (0x00000063)     // beq x0, x0, 0
ALU      -> ADDI (0x00000013)    // addi x0, x0, 0 (NOP)
```

### O3CPU Integration

The fetch stage checks for trace mode and optionally sources instructions from traces:

```cpp
void Fetch::performInstructionFetch(ThreadID tid) {
    if (traceMode) {
        // Fetch from trace reader
        while (numInst < fetchWidth && fetchQueue[tid].size() < fetchQueueSize) {
            if (!fetchInstructionFromTrace(tid)) break;
        }
        return;
    }
    // Normal memory-based fetch...
}
```

## Performance Modeling

### What's Preserved

- **Pipeline Timing**: All 5 stages (fetch, decode, rename, IEW, commit)
- **Resource Constraints**: ROB, IQ, LSQ sizing and conflicts
- **Cache Hierarchy**: Full memory system simulation
- **Branch Prediction**: Predictor evaluation and misprediction penalties
- **Dependencies**: Register and memory dependency tracking

### What's Simplified

- **Instruction Decoding**: Uses placeholder RISC-V instructions
- **Functional Execution**: No actual computation, focus on timing
- **Exception Handling**: Simplified for trace instructions

## Debugging

### Useful Debug Flags

```bash
--debug-flags=Fetch,TraceReader,O3CPU
```

- **Fetch**: Fetch stage operation and trace integration
- **TraceReader**: Trace file parsing and instruction creation
- **O3CPU**: Overall CPU pipeline operation

### Common Issues

1. **Trace File Not Found**: Verify file path and permissions
2. **Format Mismatch**: Ensure trace format matches file type
3. **Empty Statistics**: Check trace file validity and instruction limits

## Extending the System

### Adding New Trace Formats

1. Create new trace reader class inheriting from `TraceReader`
2. Implement required virtual methods:
   - `init()`: Initialize trace file reading
   - `parseInstruction()`: Convert trace entry to TraceInstruction
   - `validateTraceFile()`: Check file validity
3. Add to factory in `TraceReader.cc`:

```cpp
std::unique_ptr<TraceReader> createTraceReader(const std::string &format, ...) {
    if (format == "myformat") {
        return std::make_unique<MyTraceReader>(trace_file, name);
    }
    // ...
}
```

### Performance Analysis

The system generates standard GEM5 statistics plus trace-specific metrics:

```
# Key statistics to monitor
cpu.fetch.insts                    # Instructions fetched from trace
cpu.fetch.branches                 # Branch instructions processed  
cpu.ipc                           # Instructions per cycle
system.cpu.dcache.overall_hits    # Cache performance
```

## Limitations

- **RISC-V Only**: Currently targets RISC-V ISA (XiangShan)
- **Placeholder Instructions**: Uses simplified instruction encodings
- **Single Thread**: Focuses on single-threaded workloads
- **ChampSim Primary**: CBP2025 support is incomplete

## Future Enhancements

1. **Improved Decoding**: More accurate instruction generation from traces
2. **CBP2025 Support**: Complete CBP2025 trace format implementation  
3. **Multi-threading**: Support for multi-threaded trace replay
4. **Validation**: Cross-validation with reference simulators

## Contributor Guides

- Trace capabilities and development log: [CLAUDE.md](./CLAUDE.md)
- Contributor checklist and style for this module: [AGENTS.md](./AGENTS.md)
- Local workflow and environment notes: [CLAUDE.local.md](/CLAUDE.local.md)
5. **Memory Prefetching**: Integration with trace-driven prefetcher evaluation

## References

- [ChampSim Simulator](https://github.com/ChampSim/ChampSim)
- [CBP-2025 Competition](https://www.microarch.org/JWAC1/2025/)
- [XiangShan Processor](https://github.com/OpenXiangShan/XiangShan)
- [GEM5 Simulator](https://www.gem5.org/)
