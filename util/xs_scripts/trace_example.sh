#!/bin/bash

# Copyright (c) 2024 The Regents of The University of Michigan
# All rights reserved.
#
# Example script for running XiangShan trace-driven simulation
# This script demonstrates how to use the trace replay functionality
# with the XiangShan O3CPU model.

set -e

# Configuration variables
GEM5_BUILD="${gem5_home:-$(pwd)}/build/RISCV/gem5.opt"
CONFIG_SCRIPT="configs/example/xiangshan_trace.py"
OUTPUT_DIR="m5out/trace_sim"
MAX_INSTS=1000000
TRACE_FORMAT="champsim"

# Default debug flags for trace simulation
DEBUG_FLAGS="Fetch,TraceReader"

# Help function
show_help() {
    cat << EOF
XiangShan Trace-Driven Simulation Example

Usage: $0 [OPTIONS] <trace_file>

OPTIONS:
    -h, --help          Show this help message
    -o, --output DIR    Output directory (default: $OUTPUT_DIR)
    -n, --max-insts N   Maximum instructions to simulate (default: $MAX_INSTS)
    -f, --format FMT    Trace format: champsim|cbp2025 (default: $TRACE_FORMAT)
    -d, --debug FLAGS   Comma-separated debug flags (default: $DEBUG_FLAGS)
    -v, --verbose       Enable verbose output
    --no-debug          Disable debug output

EXAMPLES:
    # Basic ChampSim trace replay
    $0 /path/to/champsim_trace.bin

    # Trace replay with specific parameters
    $0 -n 500000 -f champsim -d "Fetch,TraceReader,O3CPU" trace.bin

    # CBP2025 trace replay (when supported)
    $0 -f cbp2025 /path/to/cbp_trace.gz

ENVIRONMENT VARIABLES:
    gem5_home          Path to GEM5 installation (default: current directory)

TRACE FORMATS:
    champsim           ChampSim binary instruction traces
    cbp2025            CBP2025 branch prediction traces (future)

OUTPUT:
    The simulation will generate detailed statistics in the output directory,
    including:
    - stats.txt        Performance statistics
    - config.ini       Configuration used
    - trace_info.txt   Trace replay information

EOF
}

# Parse command line arguments
VERBOSE=false
NO_DEBUG=false

while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        -o|--output)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        -n|--max-insts)
            MAX_INSTS="$2"
            shift 2
            ;;
        -f|--format)
            TRACE_FORMAT="$2"
            shift 2
            ;;
        -d|--debug)
            DEBUG_FLAGS="$2"
            shift 2
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        --no-debug)
            NO_DEBUG=true
            DEBUG_FLAGS=""
            shift
            ;;
        -*)
            echo "Unknown option: $1" >&2
            echo "Use --help for usage information." >&2
            exit 1
            ;;
        *)
            if [[ -z "${TRACE_FILE:-}" ]]; then
                TRACE_FILE="$1"
            else
                echo "Error: Multiple trace files specified" >&2
                exit 1
            fi
            shift
            ;;
    esac
done

# Validate arguments
if [[ -z "${TRACE_FILE:-}" ]]; then
    echo "Error: No trace file specified" >&2
    echo "Use --help for usage information." >&2
    exit 1
fi

if [[ ! -f "$TRACE_FILE" ]]; then
    echo "Error: Trace file not found: $TRACE_FILE" >&2
    exit 1
fi

if [[ ! -f "$GEM5_BUILD" ]]; then
    echo "Error: GEM5 binary not found: $GEM5_BUILD" >&2
    echo "Please build GEM5 first or set gem5_home environment variable" >&2
    exit 1
fi

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Prepare GEM5 command
GEM5_CMD="$GEM5_BUILD"

# Add debug flags if specified
if [[ -n "$DEBUG_FLAGS" && "$NO_DEBUG" == false ]]; then
    GEM5_CMD="$GEM5_CMD --debug-flags=$DEBUG_FLAGS"
fi

# Set output directory
GEM5_CMD="$GEM5_CMD --outdir=$OUTPUT_DIR"

# Add configuration script and parameters
GEM5_CMD="$GEM5_CMD $CONFIG_SCRIPT"
GEM5_CMD="$GEM5_CMD --trace-file=$TRACE_FILE"
GEM5_CMD="$GEM5_CMD --trace-format=$TRACE_FORMAT"
GEM5_CMD="$GEM5_CMD --max-insts=$MAX_INSTS"
GEM5_CMD="$GEM5_CMD --stats-file=$OUTPUT_DIR/stats.txt"

# Print simulation information
echo "============================================="
echo "XiangShan Trace-Driven Simulation"
echo "============================================="
echo "Trace file:      $TRACE_FILE"
echo "Trace format:    $TRACE_FORMAT"
echo "Max instructions: $MAX_INSTS"
echo "Output directory: $OUTPUT_DIR"
echo "Debug flags:     ${DEBUG_FLAGS:-none}"
echo "GEM5 binary:     $GEM5_BUILD"
echo "============================================="

if [[ "$VERBOSE" == true ]]; then
    echo "Full command: $GEM5_CMD"
    echo "============================================="
fi

# Create trace info file
cat > "$OUTPUT_DIR/trace_info.txt" << EOF
XiangShan Trace-Driven Simulation Information
==============================================

Simulation Parameters:
  Trace File: $TRACE_FILE
  Trace Format: $TRACE_FORMAT
  Max Instructions: $MAX_INSTS
  Debug Flags: ${DEBUG_FLAGS:-none}
  
System Configuration:
  CPU Model: XiangShan O3CPU
  Pipeline: 5-stage out-of-order
  Trace Mode: Enabled
  
Simulation Date: $(date)
Command: $GEM5_CMD
EOF

# Run the simulation
echo "Starting simulation..."
echo "This may take several minutes depending on trace size..."

if [[ "$VERBOSE" == true ]]; then
    $GEM5_CMD
else
    $GEM5_CMD 2>&1 | tee "$OUTPUT_DIR/simulation.log"
fi

SIMULATION_RESULT=$?

echo "============================================="
if [[ $SIMULATION_RESULT -eq 0 ]]; then
    echo "Simulation completed successfully!"
    
    # Extract key statistics if available
    if [[ -f "$OUTPUT_DIR/stats.txt" ]]; then
        echo ""
        echo "Key Performance Metrics:"
        echo "------------------------"
        
        # Extract IPC
        IPC=$(grep "system.cpu.ipc" "$OUTPUT_DIR/stats.txt" 2>/dev/null | head -1 | awk '{print $2}' || echo "N/A")
        echo "Instructions Per Cycle (IPC): $IPC"
        
        # Extract instruction count  
        INSTS=$(grep "system.cpu.committedInsts" "$OUTPUT_DIR/stats.txt" 2>/dev/null | head -1 | awk '{print $2}' || echo "N/A")
        echo "Instructions Committed: $INSTS"
        
        # Extract cycle count
        CYCLES=$(grep "system.cpu.numCycles" "$OUTPUT_DIR/stats.txt" 2>/dev/null | head -1 | awk '{print $2}' || echo "N/A")
        echo "Simulation Cycles: $CYCLES"
        
        # Extract cache statistics
        L1I_MISS_RATE=$(grep "system.cpu.icache.overall_miss_rate::total" "$OUTPUT_DIR/stats.txt" 2>/dev/null | awk '{print $2}' || echo "N/A")
        L1D_MISS_RATE=$(grep "system.cpu.dcache.overall_miss_rate::total" "$OUTPUT_DIR/stats.txt" 2>/dev/null | awk '{print $2}' || echo "N/A")
        echo "L1 I-Cache Miss Rate: $L1I_MISS_RATE"
        echo "L1 D-Cache Miss Rate: $L1D_MISS_RATE"
    fi
    
    echo ""
    echo "Output files:"
    echo "  Statistics: $OUTPUT_DIR/stats.txt"
    echo "  Configuration: $OUTPUT_DIR/config.ini"
    echo "  Trace Info: $OUTPUT_DIR/trace_info.txt"
    
else
    echo "Simulation failed with exit code: $SIMULATION_RESULT"
    echo "Check the simulation log for details: $OUTPUT_DIR/simulation.log"
fi

echo "============================================="

exit $SIMULATION_RESULT