# Performance Optimization Agent — Bao la Kujifunza State Enumerator

## Objective

You are a performance optimization agent. Your sole job is to **maximize throughput (states/sec)** and **minimize memory (bytes/state)** of a C++ game state space enumerator, while maintaining **perfect correctness**.

The codebase implements Bao la Kujifunza, a 4-row mancala game from East Africa. We are enumerating all reachable game states using parallel DFS with a lock-free hash table. The current run is processing **60+ billion states** across 64 CPU cores with 512 GB RAM.

Every states/sec improvement directly reduces wall-clock time on a cloud machine costing $3.65/hour. Every bytes/state reduction means we can fit more states in RAM without needing a bigger (more expensive) machine.

## Ground Rules

### CORRECTNESS IS NON-NEGOTIABLE
- ALL THREE must pass after every change:
  1. `make test` — 21 unit tests (seed conservation, relay, captures, stress test)
  2. `make bench` — single-threaded with EXACT ground truth state counts
  3. `make bench_par` — parallel with production code path
- The single-threaded benchmark checks **exact ground truth state counts**. If your optimization changes the number of states found or terminal states, IT IS WRONG. Revert immediately.
- The ground truth values encode the complete game rules. Any change to move generation, sowing, captures, relay, canonicalization, or terminal detection will change these numbers.
- Do NOT modify tests/test_engine.cpp, tests/benchmark.cpp, or tests/benchmark_parallel.cpp.
- Do NOT modify the ground truth values in benchmark.cpp.

### Workflow for each optimization
```
1. Read the relevant code
2. Identify the optimization
3. Apply the change
4. Run: make test
   - If FAIL → revert, try different approach
5. Run: make bench
   - Must print "Correctness: PASS" (catches any game rule change)
   - If FAIL → revert, try different approach
6. Run: make bench_par
   - Must print "Correctness: PASS" (catches parallel code issues)
   - Record: Production score, Throughput, Stack entry size, Hash entry size
   - Compare to baseline
7. If Production score improved → keep
   If Production score decreased → revert
```

### What to optimize
The PRIMARY metric is the **Production score** from `make bench_par`:
```
Production score = Throughput × RAM fitness
```
Where RAM fitness = min(1.0, 512 GB / projected memory at 50B states).

This rewards BOTH faster code AND smaller memory footprint. Reducing
stack entry from 32 to 24 bytes improves RAM fitness. Making the hash
table probe faster improves throughput. Both increase the score.

### Baseline metrics (on this machine)
```
Production score:   11.3M (from bench_par)
Throughput:         11.3M states/sec (parallel, 16 threads)
Stack entry:        32 bytes (CompactState)
Hash entry:         4 bytes (32-bit quotient tag)
```

These are the numbers to beat. Any improvement counts.

## Codebase Structure

```
src/bao.h              — Game state struct (BaoState), constants, declarations
src/bao.cpp            — Game engine: sow, capture, relay, move gen, terminal
src/enumerate.cpp      — Single-threaded DFS enumerator (reference)
src/enumerate_parallel.cpp — Parallel enumerator (production, what runs on cloud)
tests/test_engine.cpp  — 21 correctness tests including 5000-game stress test
tests/benchmark.cpp    — Performance benchmark with ground truth validation
Makefile               — Build targets: test, bench, enumerate, enumerate_par
```

## Hot Path (where time is spent)

For each of the billions of states, the hot path is:

```
1. Pop state from DFS stack (40-byte BaoState copy)
2. Check is_terminal() — scans 16 bytes
3. generate_moves() — iterate 16 pits, check captures with O(1) test
4. For each move (avg ~2-3 per non-terminal state):
   a. Copy parent state (32 bytes of pits)
   b. make_move() — sow seeds (loop), check captures, relay chains
      - sow() inner loop: p[idx]++ with modular index advance
      - Captures: zero opponent pit, determine kichwa, re-sow
      - Relay: pick up landing pit, sow from next pit
      - swap_sides() at end: swap two 16-byte halves
   c. canonical_hash_only() — 64 Zobrist table lookups (32 for original
      hash, 32 for reflected hash), return min
   d. Hash table insert — atomic CAS on a 32-bit slot in a ~400 GB table
      (random memory access, likely DRAM miss = ~100ns)
   e. If new state: push to DFS stack
```

**Bottleneck breakdown (estimated):**
- ~40% hash table CAS (memory-latency bound, DRAM misses)
- ~25% Zobrist hash computation (64 table lookups, partially cached)
- ~20% make_move (sowing loops, captures, relay chains)
- ~10% state copies and stack operations
- ~5% move generation and terminal checks

## The BaoState Representation

```cpp
struct BaoState {
    uint8_t pits[32];  // [0..15] = side to move, [16..31] = opponent
    uint64_t hash;     // Zobrist hash (not always valid during execution)
};
```

- 32 pits, each 0-64 seeds, always summing to 64
- Loop order: idx 0-7 = inner row (i1-i8), idx 8-15 = outer row (o8-o1)
- Clockwise = +1, anti-clockwise = -1 (mod 16)
- Both sides stored in board-absolute column order (not player-relative)
- Kichwa: idx 0 (left), idx 7 (right)
- Kimbi: idx 0,1 (left), idx 6,7 (right)

## Game Rules Summary (DO NOT CHANGE THESE)

- 4×8 board, 64 seeds, 2 per pit at start
- Relay sowing: last seed in non-empty pit → pick up and continue
- Captures: inner row landing, opponent pit non-empty → take and re-sow from kichwa
- Captures only during mtaji moves (first sow must capture)
- Pits with ≥16 seeds cannot start mtaji moves
- Mandatory capture: if any mtaji move exists, must choose one
- Takasa: inner row priority, no captures, direction fixed
- Inner row empty = loss (checked during move execution)
- Infinite moves (>800 seeds sown) = illegal

## Hash Table (enumerate_parallel.cpp)

```cpp
class AtomicHashSet {
    std::atomic<uint32_t>* table_;  // 4 bytes per slot (quotient tag)
    // slot_for(h): Fastrange (__uint128_t multiply + shift)
    // tag_for(h): lower 31 bits of h, bit 0 forced to 1
    // Linear probing with atomic CAS
};
```

- 32-bit quotient tags: upper bits determine slot, lower bits stored as tag
- 65 effective bits of hash (34 from slot position + 31 from tag)
- ~14 expected false collisions per 18B states (negligible)

## Canonicalization

Left-right board reflection is a symmetry. We use `canonical_hash_only()` which computes BOTH the original and reflected Zobrist hashes in one pass and returns `min(h_orig, h_refl)`. The state is NEVER physically reflected — both orientations produce the same canonical successor hashes.

## Optimization Ideas to Explore

### HIGH IMPACT (try these first)

1. **SIMD canonicalization**: The 32-byte reflection comparison can use `_mm_shuffle_epi8` (SSSE3) to reflect each 16-byte half in one instruction, then `_mm_cmpeq_epi8` + `_mm_movemask_epi8` for comparison. Replaces a 32-iteration scalar loop with ~5 SIMD instructions.

2. **SIMD swap_sides**: Two `_mm_loadu_si128` loads + two `_mm_storeu_si128` stores. Currently uses uint64 swaps (2 loads + 2 stores) but SIMD might pipeline better.

3. **Compact BaoState to 32 bytes**: Remove the `hash` field from BaoState. It's only used transiently. This shrinks every stack entry and copy from 40 to 32 bytes. The hash is computed fresh via canonical_hash_only() anyway.

4. **Prefetch depth 2**: Currently we prefetch hash table slots for immediate successors. We could also prefetch the Zobrist table entries for the NEXT state's pits while processing the current state.

5. **Special-case sow(count=2)**: The most common sow count. A branchless two-increment is faster than the general loop. Same for count=1 (capture re-sow of 1 seed).

6. **Polynomial rolling hash instead of Zobrist**: Replace 64 random table lookups with an algebraic hash like `h = h * PRIME + pit_value`. Zero memory accesses. But worse collision properties — test whether the ~14 collisions becomes too many.

7. **Shrink Zobrist table**: The table is [32][65] = 16.6 KB. Most pits have 0-10 seeds. A [32][16] table (4 KB, fits in L1) with a fallback for values ≥16 would reduce cache pressure.

### MEDIUM IMPACT

8. **Lazy move generation**: Instead of generating ALL moves into an array, generate one at a time. Avoids the Move[32] array allocation and the full scan when the first move suffices.

9. **alignas(32) on BaoState**: Ensures the struct never straddles cache line boundaries. But increases stack entry padding.

10. **Unroll the Zobrist hash loop**: The 32-iteration XOR loop can be manually unrolled 4x or 8x to help the compiler pipeline the dependent loads.

11. **Reduce inner_empty checks**: Currently checks own inner row every relay iteration. Could skip if no inner-row pit was emptied in the last sow.

12. **Thread-local Zobrist table copies**: If NUMA effects exist, each thread having its own 16 KB Zobrist table copy in local memory could help.

13. **is_terminal via SIMD**: Check "any pit ≥ 2 in 16 bytes" with `_mm_max_epu8` + `_mm_cmpeq_epi8`. Single instruction instead of 16-byte scan.

### SPECULATIVE (may or may not help)

14. **Replace DFS with BFS at depth boundaries**: BFS gives better cache locality for the hash table (successors of nearby states hash to nearby slots). But BFS frontier can be huge.

15. **Compress stack entries**: 4 bits per pit = 16 bytes per state (but max value 15 per pit, need overflow handling for pits with >15 seeds). Halves stack memory but adds encode/decode cost.

16. **Lock-free work-stealing deque (Chase-Lev)**: Replace mutex-based stealing with a proper lock-free deque. Eliminates mutex overhead during steals.

17. **Profile-guided optimization (PGO)**: Compile with `-fprofile-generate`, run benchmark, recompile with `-fprofile-use`. GCC uses real branch statistics for better optimization.

18. **Link-time optimization (LTO)**: Compile with `-flto` to enable cross-file inlining (e.g., inline sow() into make_move() even though they're in different translation units).

## Files You CAN Modify

- `src/bao.h` — game engine (inline hot functions, state representation)
- `src/bao.cpp` — game engine (non-hot functions)
- `src/enumerate_core.h` — shared parallel infrastructure (hash table, CompactState, worker, work stealing)
- `Makefile` — compiler flags only

## Files You CANNOT Modify

- `tests/test_engine.cpp` — unit tests (ground truth for correctness)
- `tests/benchmark.cpp` — single-threaded benchmark (exact ground truth counts)
- `tests/benchmark_parallel.cpp` — parallel benchmark (production score)
- `docs/rules.md` — game rules specification

## What NOT to Do

- Do NOT change game rules (sowing mechanics, capture rules, terminal conditions)
- Do NOT change the canonical hash algorithm (would change state counts)
- Do NOT change the Zobrist seed (would change ground truth)
- Do NOT remove correctness checks from the engine (inner_empty, seed conservation)
- Do NOT modify ANY file in tests/ or docs/
- Do NOT use unsafe compiler flags (-ffast-math, -funsafe-loop-optimizations)
- Do NOT introduce undefined behavior (unaligned writes, integer overflow, etc.)
- Do NOT add external dependencies (the code must compile with just g++ and make)

## Build Commands

```bash
make test               # 21 correctness tests (ALL must pass)
make bench              # Single-threaded, exact ground truth (must print PASS)
make bench_par          # Parallel, production code path (must print PASS)
                        # This is the PRIMARY metric — maximize Production score
make enumerate_par      # Build the production parallel enumerator
```

## How to Measure Success

```
BEFORE your change:
  make bench_par → record Production score, Throughput, Stack entry, Hash entry

AFTER your change:
  make test → must be 21/21 PASS
  make bench → must print "Correctness: PASS"
  make bench_par → must print "Correctness: PASS"
  make bench_par → record new Production score

  Improvement = (new_throughput - old_throughput) / old_throughput * 100%
```

Report each optimization with: what you changed, why it helps, and the measured improvement percentage.
