---
name: Bao benchmark optimization results
description: Performance optimization history for the Bao state enumerator benchmark. Tracks what worked, what didn't, and current best numbers for both single-threaded and parallel.
type: project
---

## Current Best Performance (2026-03-31)

### make_move benchmark (make bench_move) — PRIMARY for sow/relay optimization
- Throughput: ~20M calls/sec (peak 20.67M), up from 15.68M baseline
- Improvement: **+28-32% over baseline**
- Corpus: 5000 deep states (depth >= 20, max_pit >= 16 seeds), 16795 moves

### Parallel benchmark (make bench_par) — PRIMARY for enumeration
- Production score: ~65M median (21-run), range 61-72M
- Throughput: ~65M states/sec
- Stack entry: 32 bytes (CompactState)
- Hash entry: 4 bytes
- 16 threads, Intel i7-1360P (hybrid P+E core architecture)
- Previous baseline: ~8.6-11.3M states/sec
- Improvement: **~6x production score increase**

### Single-threaded benchmark (make bench)
- Throughput: ~5.3M states/sec
- Memory: 33.6 bytes/state
- Correctness: PASS (exact ground truth)

## make_move Optimizations (2026-03-31 session)

### What Worked

#### 1. SIMD lookup table for small-count sow — **+12%** (biggest win)
Replaced the scalar loop `for(i=0..count-1) p[idx]++; idx=(idx+step)&15` with a precomputed table:
- `SowEntry` struct: 16-byte mask (which pits to increment) + landing index
- Table: `SOW_TABLE[2][16][16]` = 512 entries * 32 bytes = 16KB (fits L1)
- Each sow becomes: load pits, load mask from table, paddb, store pits, return landing
- This eliminates the data-dependent loop entirely

#### 2. SIMD batch add for count >= 16 — **+3.8%**
Replaced `for(i=0..15) p[i] += q` with `_mm_add_epi8(v, set1(q))`.

#### 3. Interleaved SowEntry struct — **+3.6%**
Packing mask[16] and landing in same 32-byte struct (same cache line) improves locality vs separate arrays.

#### 4. Template specialization on is_capturing — **+2.4%**
`make_move_impl<true>` / `make_move_impl<false>` eliminates the capture branch from the non-capturing (takasa) relay loop via `if constexpr`.

#### 5. Combined batch+remainder SIMD — **+2.4%**
For count >= 16 with remainder r > 0: merge batch add and remainder into single `paddb(paddb(v, set1(q)), mask[r])` instead of two separate load/add/store pairs.

#### 6. Reorder checks — landing_count before inner_empty — **+2.5%**
Check `pits[landing] == 1` (single byte read) before `inner_empty(0)` (8-byte read). On last relay iteration, only do inner_empty if about to break.

#### 7. Hoist step computation from sow to make_move — **+1.8%**
Precompute `step = (dir + 16) & 15` once per make_move, pass to sow instead of recomputing each call.

#### 8. Reuse landing_count — ~0% (compiler CSE)
Use already-loaded `landing_count` instead of re-reading `pits[landing]` for relay continuation seeds. Compiler likely already did this.

### What Didn't Work (2026-03-31 session)

- **alignas(16) on pits**: -4.9%, struct padded to 48 bytes → increased copy cost
- **count==2 scalar fast path with SIMD table**: -9%, extra branch worse than uniform SIMD path
- **PGO (profile-guided optimization)**: -7.7%, profile data mismatch between profiling and optimized binary
- **SIMD inner_empty from sow result vector**: -4.3%, SSE2 cmpeq+movemask slower than uint64 compare
- **-fno-exceptions -fno-rtti**: -1.8%, doesn't help single-threaded
- **-fwhole-program**: ~0%, redundant with -flto
- **Store-forwarding avoidance (extract landing from SIMD)**: ~0%, tmp array overhead cancels benefit
- **Fast path for first-sow-no-relay**: -3.5%, extra branch overhead, fast path rarely fires on deep states
- **__builtin_expect on landing_count==1**: ~0%, branch predictor already handles this

## Key Optimizations for Parallel Benchmark (previous sessions)

### 1. Remove count_.fetch_add(1) from hash table insert — **5x improvement**
### 2. Compiler flags: -fomit-frame-pointer -fno-stack-protector — **+62%**
### 3. Thread pinning (SetThreadAffinityMask on Windows) — **+4-10%**
### 4. Batch g.terminal atomic updates — moderate improvement
### 5. Cold noinline work-stealing function — marginal
### 6. Spin-pause before yield — marginal

## What Didn't Work (previous sessions)

- **-fno-plt**: -19%, significant regression
- **SIMD swap_sides (SSE2)**: -7%, compiler generates equivalent uint64 code
- **4-way parallel XOR chains in hash**: -16%, extra register pressure
- **Branchless first_sow_captures**: -6%, branches are well-predicted
- **Zobrist prefetch within canonical_hash_only**: ~0%, table already in L1
- **alignas(64) on ZOBRIST_TABLE**: -10%, changes cache-line alignment negatively
- **sow(count==1) special case**: -6%, extra branch overhead
- **SWAR is_terminal check**: -15%
- **-funroll-loops for ST**: -8% (but helps parallel!)
- **Custom copy constructor (32 vs 40 bytes)**: -21%
- **alignas(32) on BaoState**: terrible (padded to 64 bytes)
- **Check-before-CAS in hash table**: added latency
- **-fno-exceptions -fno-rtti**: -6%, breaks std::mutex performance
- **Loop fission (separate make_move and hash loops)**: -13%
- **Early prefetch (per-hash instead of batch)**: -11%
- **24-byte CompactState (6-bit encoding)**: ~0%, encode/decode overhead cancels cache benefit
- **__builtin_expect on hash insert**: -4%, wrong prediction direction
- **P-cores-only pinning**: Much worse
- **Stack prefetch**: 0%
- **PGO for parallel**: 0%

## Build Configuration
- CXXFLAGS_FAST: `-std=c++17 -O3 -march=native -Wall -Wextra -flto -fomit-frame-pointer -fno-stack-protector`
- bench_move additionally uses: `-funroll-loops`
- Parallel benchmark additionally uses: `-funroll-loops`
- Tests built with: `-std=c++17 -O2` (no fast flags)

## Architecture Notes
- All hot path functions defined inline in bao.h with __attribute__((hot))
- BaoState is 40 bytes (32 pits + 8 hash). Cannot remove hash field (tests depend on it)
- CompactState is 32 bytes (pits only, no hash)
- Intel i7-1360P: 4 P-cores (HT=8 threads) + 8 E-cores, 16 total logical cores
- L1 cache: 48KB, L2: 18432KB, cache line: 64 bytes
- SOW_TABLE: 16KB precomputed sow masks+landing (32-byte aligned SowEntry structs)
- sow() uses SIMD lookup table for count<16, SIMD batch+remainder for count>=16
- make_move uses template specialization on is_capturing (IS_CAPTURING template param)

## System Notes
- Windows 11, MSYS2/MinGW g++ 15.2.0 at /c/msys64/mingw64/bin/g++
- -march=native resolves to -march=alderlake -mtune=alderlake
- AVX2 available but SSE2 sufficient for 16-byte pit operations
- Windows `windows.h` defines INFINITE macro — need `#undef INFINITE` after include
- _mm_testz_si128 (SSE4.1) not available in test builds (-O2 without -march=native)

**Why:** Tracking this helps avoid re-trying failed optimizations.
**How to apply:** Reference before attempting new optimizations. The biggest remaining opportunity may be software pipelining (processing 2+ states simultaneously) or reducing the store-forwarding stall after SIMD sow writes.
