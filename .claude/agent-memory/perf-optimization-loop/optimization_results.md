---
name: Bao benchmark optimization results
description: Performance optimization history for the Bao state enumerator benchmark. Tracks what worked, what didn't, and current best numbers.
type: project
---

## Current Best Performance (measured at low system load)
- Throughput: ~4.7-4.8M states/sec (5M benchmark), ~5.0M (2M benchmark)
- Under typical load: ~3.5-4.0M states/sec
- Memory: 33.6 bytes/state (2M), 53.7 bytes/state (5M) - fixed by benchmark design
- **Baseline was: ~2.8-3.0M states/sec, 33.6 bytes/state**
- **Improvement: ~60-70% throughput increase**

## Optimizations Applied (in order of impact)
1. **Unrolled Zobrist hash in canonical_hash_only()**: +40% -- biggest single win. Replaced 2 nested loops (32 iterations each) with fully unrolled XOR expressions. Interleaved orig+refl lookups for cache locality.
2. **Moved ALL hot functions to header as inline**: +33% cumulative. Functions moved: canonical_hash_only, is_terminal, swap_sides, first_sow_captures, sow, make_move, generate_moves. Guarantees inlining without LTO.
3. **Removed LTO**: +4% after all functions header-inline (LTO was adding overhead)
4. **Special-case sow(count=2)**: +5% -- branchless two-increment for most common sow count
5. **Simplified sow() branch structure**: +2% -- removed __builtin_expect hints, compiler's own prediction is better
6. **Flattened generate_moves**: Neutral but cleaner code, separate kichwa handling

## What Didn't Work
- SWAR is_terminal check (uint64 mask trick): -15%, compiler optimizes the scalar loop better
- -funroll-loops flag: -8%, icache pressure from larger code
- PGO: Inconclusive due to system noise, adds compile time complexity
- count==3 special case in sow: Neutral/slightly negative
- Custom copy constructor (32-byte instead of 40): -21%, compiler's trivial copy is highly optimized
- alignas(32) on BaoState: Padded struct to 64 bytes, terrible
- __attribute__((flatten)) on make_move: No measurable effect

## Architecture Notes
- All hot path functions are defined inline in bao.h
- bao.cpp only contains: zobrist_init, init_start, total_seeds, compute_hash, canonicalize_and_hash, reflect_lr, canonicalize, print
- BaoState is 40 bytes (32 pits + 8 hash). Cannot remove hash field because tests use s.hash directly
- Build flags: -std=c++17 -O3 -march=native (no LTO, no PGO)

## System Notes
- Windows 11, MSYS2/MinGW g++ at /c/msys64/mingw64/bin/g++
- Significant system load variance: benchmarks can vary 2x between runs
- 5M state benchmark is more stable than 2M

**Why:** Tracking this helps avoid re-trying failed optimizations.
**How to apply:** Reference before attempting new optimizations.
