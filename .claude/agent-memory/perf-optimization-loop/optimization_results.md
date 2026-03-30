---
name: Bao benchmark optimization results
description: Performance optimization history for the Bao state enumerator benchmark. Tracks what worked, what didn't, and current best numbers for both single-threaded and parallel.
type: project
---

## Current Best Performance

### Single-threaded benchmark (make bench)
- Throughput: ~4.0-4.3M states/sec
- Memory: 33.6 bytes/state
- Baseline was: ~2.8-3.0M states/sec
- Improvement: ~50-60% throughput increase

### Parallel benchmark (make bench_par)
- Throughput: ~9.7M states/sec median at 10M states (range 8-11M due to P/E core scheduling)
- 16 threads, Intel i7-1360P (hybrid P+E core architecture)
- Memory: 16.0 bytes/state
- Baseline was: ~8.2M states/sec (10M) without PGO/LTO/unroll
- Improvement: ~18% throughput increase

## Single-threaded Optimizations Applied (in order of impact)
1. **Unrolled Zobrist hash in canonical_hash_only()**: +40% -- biggest single win
2. **Moved ALL hot functions to header as inline**: +33% cumulative
3. **Removed LTO**: +4% after all functions header-inline (LTO was adding overhead for ST)
4. **Special-case sow(count=2)**: +5%
5. **Simplified sow() branch structure**: +2%

## Parallel-specific Optimizations Applied
1. **PGO (Profile-Guided Optimization)**: +10% -- biggest parallel win. PGO training with 5M states.
2. **__attribute__((hot)) on hot functions**: +10% -- canonical_hash_only, generate_moves, make_move, sow
3. **-funroll-loops**: +3-6% -- helps parallel more than ST (memory-bound, keeps CPU busy)
4. **LTO (-flto)**: +4% -- helps cross-file inlining in parallel context
5. **Branchless is_terminal()**: marginal, kept for cleanliness

## Build Configuration
- CXXFLAGS_FAST: `-std=c++17 -O3 -march=native -Wall -Wextra -flto`
- Parallel benchmark additionally uses: `-fprofile-use=build/pgo -fprofile-correction -funroll-loops`
- PGO training: `make pgo_train` runs instrumented build with 5M states
- Tests built with: `-std=c++17 -O2` (no fast flags)

## What Didn't Work for Parallel
- **-fno-exceptions -fno-rtti**: -23%, breaks std::mutex performance
- **-fno-plt**: -19%, significant regression
- **SIMD swap_sides (SSE2)**: -7%, compiler generates equivalent uint64 code
- **4-way parallel XOR chains in hash**: -16%, extra register pressure causes spills
- **Branchless first_sow_captures**: -6%, branches are well-predicted
- **Zobrist prefetch within canonical_hash_only**: ~0%, table already in L1
- **alignas(64) on ZOBRIST_TABLE**: -10%, changes cache-line alignment negatively
- **sow(count==1) special case**: -6%, extra branch overhead
- **sow(count==3) special case**: ~0%, neutral
- **Skip mtaji if opponent inner row empty**: ~0%, rarely triggers
- **Replace multiply with branch in first_sow_captures**: -5%, CPU handles multiply efficiently
- **always_inline on canonical_hash_only**: 0%, wrapper function in benchmark not controlled
- **Larger PGO training (10M vs 5M)**: ~0%, diminishing returns

## What Didn't Work for Single-threaded (from previous session)
- SWAR is_terminal check: -15%
- -funroll-loops flag: -8% (but helps parallel!)
- Custom copy constructor (32 vs 40 bytes): -21%
- alignas(32) on BaoState: terrible (padded to 64 bytes)

## Architecture Notes
- All hot path functions defined inline in bao.h with __attribute__((hot))
- BaoState is 40 bytes (32 pits + 8 hash). Cannot remove hash field (tests depend on it)
- Intel i7-1360P: hybrid P+E core, 16 threads. Huge variance due to core scheduling.
- Bimodal performance distribution: runs get ~8-9M or ~10-11M depending on which cores are used
- benchmark_parallel.cpp CANNOT be modified -- optimizations must go through bao.h or Makefile

## System Notes
- Windows 11, MSYS2/MinGW g++ 15.2.0 at /c/msys64/mingw64/bin/g++
- -march=native resolves to -march=alderlake -mtune=alderlake
- L1 cache: 48KB, L2: 18432KB, cache line: 64 bytes
- Benchmark variance is extreme (50%+ between runs) due to hybrid architecture

**Why:** Tracking this helps avoid re-trying failed optimizations.
**How to apply:** Reference before attempting new optimizations. Focus on PGO setup when deploying.
