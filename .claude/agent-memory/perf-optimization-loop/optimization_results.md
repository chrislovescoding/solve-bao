---
name: Bao benchmark optimization results
description: Performance optimization history for the Bao state enumerator benchmark. Tracks what worked, what didn't, and current best numbers for both single-threaded and parallel.
type: project
---

## Current Best Performance (2026-03-30)

### Parallel benchmark (make bench_par) — PRIMARY METRIC
- Production score: ~65M median (21-run), range 61-72M
- Throughput: ~65M states/sec
- Stack entry: 32 bytes (CompactState)
- Hash entry: 4 bytes
- 16 threads, Intel i7-1360P (hybrid P+E core architecture)
- Previous baseline: ~8.6-11.3M states/sec
- Improvement: **~6x production score increase**

### Single-threaded benchmark (make bench)
- Throughput: ~4.5-4.8M states/sec
- Memory: 33.6 bytes/state
- Correctness: PASS (exact ground truth)

## Key Optimizations (in order of impact)

### 1. Remove count_.fetch_add(1) from hash table insert — **5x improvement**
The AtomicHashSet was incrementing an atomic counter on every successful insert. With 16 threads all hitting this counter, the cache line bounced continuously. Removed the per-insert increment and use g.states (already batched per-1024) for capacity checks instead.

### 2. Compiler flags: -fomit-frame-pointer -fno-stack-protector — **+62%**
Freeing the RBP register for general use and removing stack canary checks from every function gave a massive throughput boost to the register-pressure-heavy hot loop.

### 3. Thread pinning (SetThreadAffinityMask on Windows) — **+4-10%**
Pin each worker thread to its own logical core (tid % 64). Eliminates the OS scheduler bouncing threads between P-cores and E-cores on hybrid architectures. Also dramatically reduces variance (from 2x range to ~15% range).

### 4. Batch g.terminal atomic updates — moderate improvement
Accumulate terminal state counts locally per-thread, flush every 1024 states alongside g.states batch. Reduces atomic contention.

### 5. Cold noinline work-stealing function — marginal
Extract work-stealing code to a separate __attribute__((noinline, cold)) function to reduce hot-path instruction cache pressure.

### 6. Spin-pause before yield — marginal
Use 64 iterations of _mm_pause() instead of std::this_thread::yield() when no work can be stolen. Avoids OS scheduler quantum penalty.

### 7. Minor optimizations (neutral/marginal)
- Reserve stack vectors (100K entries)
- Remove redundant is_terminal() check after pop from stack
- Fold benchmark limit check into every-1024-states batch
- Use compare_exchange_weak instead of strong
- Copy only pits[32] instead of full BaoState(40) for successors
- Periodic stack_peak tracking (every 256 states instead of every state)

## What Didn't Work (2026-03-30 session)
- **Check-before-CAS in hash table**: Load before CAS added latency for common case (empty slots)
- **-fno-exceptions -fno-rtti**: -6%, breaks std::mutex performance
- **Loop fission (separate make_move and hash loops)**: -13%, loses temporal locality on pits
- **Early prefetch (per-hash instead of batch)**: -11%, reduces memory-level parallelism
- **24-byte CompactState (6-bit encoding)**: ~0% throughput, encode/decode overhead cancels cache benefit. Memory savings not needed (fits in 512GB either way)
- **__builtin_expect on hash insert**: -4%, wrong prediction direction
- **P-cores-only pinning**: Much worse — 16 threads competing for 8 logical cores
- **Stack prefetch**: 0%, stack data already hot in L1/L2
- **PGO**: 0%, all hot functions already inlined in header

## What Didn't Work (previous session)
- **-fno-plt**: -19%, significant regression
- **SIMD swap_sides (SSE2)**: -7%, compiler generates equivalent uint64 code
- **4-way parallel XOR chains in hash**: -16%, extra register pressure causes spills
- **Branchless first_sow_captures**: -6%, branches are well-predicted
- **Zobrist prefetch within canonical_hash_only**: ~0%, table already in L1
- **alignas(64) on ZOBRIST_TABLE**: -10%, changes cache-line alignment negatively
- **sow(count==1) special case**: -6%, extra branch overhead
- **SWAR is_terminal check**: -15%
- **-funroll-loops for ST**: -8% (but helps parallel!)
- **Custom copy constructor (32 vs 40 bytes)**: -21%
- **alignas(32) on BaoState**: terrible (padded to 64 bytes)

## Build Configuration
- CXXFLAGS_FAST: `-std=c++17 -O3 -march=native -Wall -Wextra -flto -fomit-frame-pointer -fno-stack-protector`
- Parallel benchmark additionally uses: `-funroll-loops`
- Tests built with: `-std=c++17 -O2` (no fast flags)

## Architecture Notes
- All hot path functions defined inline in bao.h with __attribute__((hot))
- BaoState is 40 bytes (32 pits + 8 hash). Cannot remove hash field (tests depend on it)
- CompactState is 32 bytes (pits only, no hash)
- Intel i7-1360P: 4 P-cores (HT=8 threads) + 8 E-cores, 16 total logical cores
- L1 cache: 48KB, L2: 18432KB, cache line: 64 bytes
- Hash table insert is the primary bottleneck (~100ns DRAM miss per CAS)
- Thread pinning reduces variance from ~2x to ~15% range

## System Notes
- Windows 11, MSYS2/MinGW g++ 15.2.0 at /c/msys64/mingw64/bin/g++
- -march=native resolves to -march=alderlake -mtune=alderlake
- Windows `windows.h` defines INFINITE macro — need `#undef INFINITE` after include

**Why:** Tracking this helps avoid re-trying failed optimizations.
**How to apply:** Reference before attempting new optimizations. The biggest remaining opportunity is software pipelining (processing 2+ states simultaneously to increase hash table prefetch distance).
