# Performance Optimization Agent — Bao make_move

## Objective

You are a performance optimization agent. Your sole job is to **maximize `make_move` calls/sec** in `src/bao.h` and `src/bao.cpp`, while maintaining **perfect correctness**.

The codebase implements Bao la Kujifunza, a 4-row mancala game from East Africa. We are strongly solving it by enumerating all ~48 billion reachable game states. The enumeration's tail phase is bottlenecked on `make_move()`: the relay-sowing loop is slow for deep game states where seeds have concentrated into a few pits, causing long relay chains.

### The problem in numbers
- Enumerator tail: **~26K pops/sec** across 16 threads (1.6K/thread)
- Each pop calls `make_move` ~3× (average moves per state)
- Total: **~80K make_move calls/sec** on the 64-core cloud VM
- Benchmark baseline: **13M calls/sec** (single-threaded, laptop)
- Estimated speedup needed to complete in reasonable time: **≥ 2×**

Every 10% improvement in `make_move` throughput directly reduces the enumeration tail time by 10%. The cloud VM costs $3.65/hour; cutting the tail from 35 hours to 17 hours saves ~$65.

## Ground Rules

### CORRECTNESS IS NON-NEGOTIABLE
ALL THREE must pass after every change:

1. `make test` — 21 unit tests (seed conservation, relay, captures, 5000-game stress)
2. `make bench` — single-threaded DFS with **exact ground truth state counts**
3. `make bench_move` — make_move benchmark, must print `Correctness: PASS`

The `make bench` ground truth is the ultimate correctness oracle. It checks the **exact** number of canonical states and terminals from DFS. Any change to sowing, capture logic, relay, or terminal detection will change these counts → fail → revert.

### Workflow for each optimization
```
1. Read the relevant code (bao.h is the hot path — all hot functions are inline there)
2. Identify the optimization
3. Record baseline: make bench_move → note make_move calls/sec
4. Apply the change
5. make test         → ALL 21 must pass
6. make bench        → must print "Correctness: PASS" with identical state/terminal counts
7. make bench_move   → must print "Correctness: PASS" then record new calls/sec
8. If calls/sec improved → KEEP. If decreased or correctness failed → REVERT.
```

### Primary metric
```
make_move calls/sec   (from make bench_move — MAXIMIZE THIS)
```

Current baseline (Windows laptop, -O3 -march=native):
```
make_move calls/sec: 13.086M
Latency:             0.08 us/call
```

## Where the time is spent

### make_move() — the relay loop (src/bao.h ~line 274)
```cpp
for (;;) {
    int landing = sow(sow_start, seeds, current_dir);  // distribute seeds
    total_sown += seeds;
    if (total_sown > 800) return INFINITE;              // rare — almost never hit
    if (inner_empty(0))   return INNER_ROW_EMPTY;
    if (pits[landing] == 1) break;                      // relay ends

    if (is_capturing_move && landing < 8) {             // capture branch
        // ... capture logic ...
    }
    seeds = pits[landing];
    pits[landing] = 0;
    sow_start = (landing + dir + 16) & 15;
}
swap_sides();
```

Each iteration calls `sow()`. The relay continues until the last seed lands in an empty pit (`pits[landing] == 1`). Deep states can require 5–20 relay iterations.

### sow() — the inner loop (src/bao.h ~line 235)
```cpp
inline int sow(int start_idx, int count, int dir) {
    uint8_t* p = &pits[0];
    int step = (dir + 16) & 15;

    if (count == 2) { /* fast path: 2 increments */ }
    if (count < 16) { /* one-by-one loop */ }

    // LARGE COUNT PATH — hit when seeds are concentrated:
    int q = count >> 4;   // full laps (each lap: 1 seed to all 16 pits)
    int r = count & 15;   // remainder seeds
    for (int i = 0; i < 16; ++i)  // <-- SIMD TARGET: 16 scalar adds
        p[i] += (uint8_t)q;
    // distribute r remainder seeds one-by-one
}
```

**The SIMD target**: for `count >= 16`, the loop `for(i=0..15) p[i] += q` adds the same value `q` to all 16 contiguous `uint8_t` pits. This is exactly `_mm_add_epi8` — replace 16 scalar increments with a single 128-bit SIMD add.

## Optimization Ideas

### HIGH IMPACT — try these first

**1. SIMD batch add in sow() — the primary target**

Replace the 16-scalar-add loop in the `count >= 16` branch:
```cpp
// Current (scalar):
for (int i = 0; i < PITS_PER_SIDE; ++i)
    p[i] += (uint8_t)q;

// Optimized (SIMD, SSE2 — available everywhere with -march=native):
#include <immintrin.h>
__m128i v  = _mm_loadu_si128((__m128i*)p);
__m128i vq = _mm_set1_epi8((char)q);
_mm_storeu_si128((__m128i*)p, _mm_add_epi8(v, vq));
```
This turns 16 dependent read-modify-writes into 1 SIMD load + 1 SIMD add + 1 SIMD store.
`p` points to `BaoState::pits[0]`, 16 consecutive bytes, naturally 16-byte aligned for `_mm_load_si128`.

**2. SIMD swap_sides()**

`swap_sides()` swaps `pits[0..15]` with `pits[16..31]`. Currently uses scalar or 64-bit swaps.
```cpp
// SIMD version:
__m128i a = _mm_loadu_si128((__m128i*)(pits + 0));
__m128i b = _mm_loadu_si128((__m128i*)(pits + 16));
_mm_storeu_si128((__m128i*)(pits + 0),  b);
_mm_storeu_si128((__m128i*)(pits + 16), a);
```
Called once per `make_move`, but at ~80K calls/sec this is a small gain.

**3. Reduce inner_empty checks in relay loop**

Currently `inner_empty(0)` is checked every relay iteration. But the inner row can only become empty when a sow LANDS in the inner row or a CAPTURE empties it. Skip the check if last sow didn't touch pits 0–7:
```cpp
if (landing < INNER_PITS || capture_happened)
    if (inner_empty(0)) return MoveResult::INNER_ROW_EMPTY;
```
Avoids 8 byte reads on every relay iteration that doesn't touch inner row.

**4. Hoist direction step computation**

In `sow()`, `int step = (dir + PITS_PER_SIDE) & 15` is computed every call. Since `dir` is either `CW=1` or `ACW=-1`, the step is either `1` or `15`. Precompute in `make_move` and pass directly:
```cpp
// In make_move, before the relay loop:
const int step = (current_dir + PITS_PER_SIDE) & 15;  // 1 or 15
// Pass to a sow_fast(start, count, step) variant
```

**5. Branchless landing-is-empty check**

The relay exit `if (pits[landing] == 1) break` can become:
```cpp
if (pits[landing] <= 1) break;   // since pits are uint8_t, this is safe
```
This is semantically identical (count was just incremented from 0 to 1), but the `<= 1` form may generate better code (fewer branch mispredictions).

**6. Unroll the small-count sow loop**

For `count < 16` (the one-by-one path), the loop runs 3–15 iterations. Manual unrolling to 4-wide removes loop overhead and allows the compiler to pipeline:
```cpp
while (remaining >= 4) {
    p[idx]++; idx = (idx + step) & 15;
    p[idx]++; idx = (idx + step) & 15;
    p[idx]++; idx = (idx + step) & 15;
    p[idx]++; idx = (idx + step) & 15;
    remaining -= 4;
}
while (remaining-- > 0) { p[idx]++; idx = (idx + step) & 15; }
```

### MEDIUM IMPACT

**7. alignas(16) on BaoState::pits**

Ensures `pits[0]` is always 16-byte aligned, allowing `_mm_load_si128` (faster than `_mm_loadu_si128`) for SIMD operations:
```cpp
struct BaoState {
    alignas(16) uint8_t pits[32];
    uint64_t hash;
};
```

**8. Mark relay-exit branch as likely**

The relay exits when `pits[landing] == 1` (last seed in empty pit). For most moves (short relay chains), this happens in the first 1–3 iterations. Hint to the branch predictor:
```cpp
if (__builtin_expect(pits[landing] == 1, 1)) break;
```

**9. Reduce state copy cost**

`make_move` is called on a copy: `BaoState succ = e.state; succ.make_move(m)`. The copy is 40 bytes (32 pits + 8 hash). Use SIMD for the copy:
- If `BaoState` is 40 bytes, two `_mm_loadu_si128` + two stores copies all 32 bytes of pits in 2 instructions
- Or remove the `hash` field from `BaoState` (it's recomputed fresh each time via `canonical_hash_only()`) to reduce copy to 32 bytes

**10. is_terminal() via SIMD**

Currently scans 8 inner-row bytes with a scalar loop checking for `>= 2`. Replace with:
```cpp
__m128i inner = _mm_loadu_si128((__m128i*)pits);  // loads pits[0..15]
// any byte >= 2 iff (byte & 0xFE) != 0
__m128i mask  = _mm_and_si128(inner, _mm_set1_epi8(0xFE));
return _mm_testz_si128(mask, mask) != 0;  // true = all zero = terminal
```

### LOW IMPACT / SPECULATIVE

**11. PGO (Profile-Guided Optimization)**
```bash
g++ -fprofile-generate ... -o build/bao_prof
./build/bao_prof  # run bench_move to profile
g++ -fprofile-use -fprofile-correction ...  # use real branch stats
```
GCC will optimize the relay loop's branches based on actual execution.

**12. Pre-check relay termination**

Before executing a relay step, check if `pits[(sow_start + seeds - 1 + ...) & 15]` is 0. If yes, the last seed will land in an empty pit → no relay → can skip the relay continuation check. This avoids the relay loop entirely for non-relaying moves.

## Codebase

```
src/bao.h           — ALL hot functions inline (make_move, sow, swap_sides, etc.)
src/bao.cpp         — Non-hot functions (init_start, reflect_lr, print)
```

## Files You CAN Modify
- `src/bao.h` — game engine (inline hot functions)
- `src/bao.cpp` — game engine (non-hot functions)
- `Makefile` — compiler flags only (do NOT remove -O3 -march=native -flto)

## Files You CANNOT Modify
- `tests/test_engine.cpp`
- `tests/benchmark.cpp`
- `tests/benchmark_make_move.cpp`
- `tests/benchmark_parallel.cpp`
- `tests/benchmark_solver.cpp`
- `docs/rules.md`

## What NOT to Do
- Do NOT change game rules (sowing mechanics, capture logic, relay conditions)
- Do NOT change `MAX_SOW_THRESHOLD` (800) — alters which moves are legal
- Do NOT change the canonical hash algorithm
- Do NOT change the Zobrist seed values
- Do NOT use `-ffast-math` (unsafe floating point)
- Do NOT add external dependencies

## Build Commands

```bash
make test           # 21 correctness tests (ALL must pass)
make bench          # DFS ground truth (must print Correctness: PASS)
make bench_move     # PRIMARY BENCHMARK — make_move calls/sec (maximize this)
make bench_par      # Parallel enumerator performance (secondary)
make enumerate_par  # Build production enumerator
```

## How to Measure Success

```
BEFORE change: make bench_move → record calls/sec
AFTER change:
  make test      → 21/21 PASS
  make bench     → Correctness: PASS (exact state counts unchanged)
  make bench_move → Correctness: PASS, then record new calls/sec

Improvement = (new - baseline) / baseline × 100%
```

Report for each optimization: what changed, why it helps, measured improvement %.
