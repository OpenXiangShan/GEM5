# Half-Aligned BTB Design Documentation

## Overview

The Half-Aligned BTB (Branch Target Buffer) is a design enhancement that allows for 64-byte predictions while maintaining 32-byte alignment for index and tag calculations. This modification enables the BTB to read from two adjacent 32-byte blocks based on a given `startAddr`, effectively doubling the prediction window while maintaining the efficiency of the existing indexing scheme.

## Motivation

### Problem Statement

In the original BTB design:
- Predictions are limited to a single 32-byte block
- Branch targets that cross 32-byte boundaries require multiple lookups
- This limitation reduces prediction coverage and can impact performance
- blockSize is 32
- stream.startPC is not 32-byte aligned

### Solution

The Half-Aligned BTB design:
- Maintains 32-byte alignment for index and tag calculations (efficient hardware implementation)
- Allows predictions to span 64 bytes (two adjacent 32-byte blocks)
- Improves prediction coverage without significantly increasing hardware complexity

## Design Details

### Key Components

1. **Configuration Option**
   - Added a `halfAligned` boolean parameter to the BTB constructor
   - When enabled, the BTB operates in 64-byte prediction mode

2. **Lookup Mechanism**
   - Modified the `lookup` function to handle 64-byte predictions
   - Introduced a helper function `lookupSingleBlock` to lookup entries in a single 32-byte block
   - When `halfAligned` is enabled:
     - Calculate 32-byte aligned address
     - Lookup first 32-byte block
     - Lookup next 32-byte block
     - Merge results
     - Sort entries by PC order

3. **Update Mechanism**
   - Modified the `update` function to handle entries in different 32-byte blocks
   - Each entry uses its own PC to calculate the correct index and tag
   - This ensures branches are stored in the correct BTB set based on their location
   - Prevents issues when updating multiple branches across different 32-byte blocks

4. **Index and Tag Calculation**
   - Maintained the original `getIndex` and `getTag` functions
   - In halfAligned mode, each branch entry calculates its own index and tag based on its PC
   - For non-halfAligned mode, all entries use the same index and tag based on startPC

## Implementation Progress

### Completed Work

1. **Core Functionality**
   - Added `halfAligned` configuration option to `DefaultBTB` class
   - Implemented `lookupSingleBlock` helper function
   - Modified `lookup` function to support half-aligned mode
   - Moved `getIndex` and `getTag` functions to public section for testing
   - Modified `update` function to calculate index and tag per entry in halfAligned mode

2. **Testing**
   - Created `HalfAlignedBasicTest` to verify basic functionality
   - Created `HalfAlignedUnalignedTest` to verify handling of unaligned addresses
   - Added `HalfAlignedUpdateSecondBlock` to test updating branches in the second block
   - Added `HalfAlignedBothBlocks` to test updating branches in both blocks simultaneously
   - Added `HalfAlignedUnalignedStart` to test unaligned start addresses
   - Added `HalfAlignedMultipleUpdates` to test multiple updates to the same branch
   - Tests follow the prediction-update pattern:
     - Initial prediction to get metadata
     - Update with branch(es)
     - Final prediction to verify results

### Key Improvements

1. **Per-Entry Index and Tag Calculation**
   - Previously: Used a single index and tag for all entries based on either startPC or controlPC
   - Now: Each entry calculates its own index and tag based on its PC
   - Benefits:
     - Correctly handles branches in different 32-byte blocks
     - Prevents branches from being stored in incorrect BTB sets
     - Maintains proper ordering of branches in prediction results

2. **Enhanced Testing**
   - Added comprehensive tests for various halfAligned scenarios
   - Verified correct handling of branches in different blocks
   - Ensured proper updating of branch targets and counters

## Testing Strategy

The testing strategy for the Half-Aligned BTB follows the standard prediction-update pattern:

1. **Prediction Phase**
   - Call `putPCHistory` to make a prediction
   - Get prediction metadata via `getPredictionMeta`

2. **Update Phase**
   - Set up update stream with branch information
   - Set prediction metadata from previous phase
   - Call `updateWithBranchUpdateContext` to update the BTB

3. **Verification**
   - Make a final prediction to verify results
   - Check that branches from both 32-byte blocks are correctly predicted
   - Verify branch ordering and target addresses

## Future Considerations

1. **Integration with Other Predictors**
   - Ensure compatibility with other branch prediction components
   - Consider impact on overall prediction accuracy

2. **Hardware Implementation**
   - Evaluate hardware cost of implementing half-aligned mode
   - Consider power and area implications

3. **Configuration Options**
   - Consider making the prediction window size configurable
   - Explore adaptive prediction window sizing based on workload characteristics
