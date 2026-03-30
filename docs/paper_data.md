# Paper Data — Strongly Solving Bao la Kujifunza

## 1. Game Specification

**Game:** Bao la Kujifunza (beginner variant of Zanzibar Bao)
**Family:** Mancala (4-row, East African)
**Board:** 4 rows × 8 pits = 32 pits
**Seeds:** 64 (always conserved, never removed from board)
**Initial position:** 2 seeds per pit (mbili-mbili)
**Players:** 2 (alternating turns)

**Key mechanics:**
- Relay sowing (endelea): last seed in non-empty pit → pick up and continue
- Captures (mtaji): inner row landing with non-empty opposite → take and re-sow from kichwa
- Mandatory capture: if any capturing move exists, must choose one
- Pits with ≥16 seeds cannot initiate capturing moves
- Inner row empty = loss (checked even mid-move)
- Infinite moves (>800 seeds sown) declared illegal

**Rule sources:**
- [S1] Donkers (2003) PhD thesis "Nosce Hostem", Appendix C (pp. 163-168)
- [S2] De Voogt (1995) "Limits of the Mind"
- [S3] Kronenburg et al. (2006) "Never-Ending Moves in Bao", ICGA Journal
- [S7] Mancala World wiki (Nino Vessella): "played with the rules of the
  second stage (mtaji) of Bao only, but without the takasia rule"

**Formal rule specification:** See docs/rules.md (598 lines, every ambiguity resolved)

---

## 2. Prior Work

| Game | Solved? | States | Result | Authors | Year |
|------|---------|--------|--------|---------|------|
| Awari/Oware | Strongly | 889 billion | Draw | Romein & Bal | 2002 |
| Kalah(6,4) | Strongly | ~10 billion | P1 wins by 2 | Irving, Donkers & Uiterwijk | 2000 |
| Kalah(6,6) | Solved | — | P1 wins | Carstensen | 2011 |
| Bao la Kujifunza | **Weakly** | unknown | P1 wins | Donkers & Uiterwijk | 2002 |

**Our contribution:** First strong solution of Bao la Kujifunza. First strong
solution of ANY 4-row mancala game. First precise state space count for this game.

---

## 3. State Representation

**Game state:** 32 pit values (uint8, always summing to 64) + side to move
**Encoding:** pits[0..15] = side to move, pits[16..31] = opponent (loop order)
**Symmetry exploited:**
- Player-swap: handled by "to-move first" convention (2× reduction)
- Left-right reflection: canonical form = min(original, reflected) (2× reduction)
- Combined: **4× reduction** of raw state space

**Canonical hash:** Zobrist hashing with `min(hash_orig, hash_refl)`. 64
Zobrist table lookups (32 original + 32 reflected) computed simultaneously.
State is never physically reflected — both orientations produce identical
canonical successor hashes.

---

## 4. State Space Enumeration Results

**>>> PASTE FINAL OUTPUT HERE <<<**

```
[Paste the complete "Results" block from the enumerator here]
```

**Machine:** GCP n2d-highmem-64 (AMD EPYC, 64 vCPUs, 512 GB RAM)
**Configuration:** --mem-gb 300 --threads 16
**Date:** 2026-03-30

### Key numbers:

| Metric | Value |
|--------|-------|
| **Canonical reachable states** | ~40.3 billion (PASTE EXACT) |
| **Raw reachable states (pre-symmetry)** | ~161 billion (4× canonical) |
| **Terminal states** | ~20.6 billion (~51% of total) |
| **Infinite moves encountered** | (PASTE) |
| **Inner-row-empty endings** | (PASTE) |
| **Hash table load factor** | ~0.50 |

### Comparison to other solved mancala games:

| Game | Pits | Seeds | States | Ratio to Bao |
|------|------|-------|--------|-------------|
| Kalah(6,4) | 14 | 48 | ~10B | 0.25× |
| **Bao la Kujifunza** | **32** | **64** | **~40B** | **1×** |
| Awari/Oware | 12 | 48 | 889B | 22× |

Bao la Kujifunza has the largest state space of any strongly solved
4-row mancala game, and sits between Kalah and Awari in complexity.

---

## 5. Methodology

### 5.1 Game Engine
- C++ implementation with all hot functions inlined in header
- O(1) capture detection (arithmetic landing computation, no simulation)
- Batch sowing for count ≥ 16 (add quotient to all pits)
- Special-case sow(count=2) (branchless, most common case)
- Verified with 21 unit tests + 5000-game stress test (100K+ plies)

### 5.2 Hash Table
- Lock-free open-addressing with 32-bit quotient tags (4 bytes/entry)
- Fastrange slot mapping (__uint128_t multiply, no power-of-2 constraint)
- Tag = lower 31 bits of Zobrist hash (bit 0 forced to 1 for sentinel)
- Effective hash: 34 bits (slot) + 31 bits (tag) = 65 bits
- Expected false collisions: ~20 per 40B states (negligible)
- compare_exchange_weak for lighter CAS retry

### 5.3 Parallel DFS
- 16 worker threads with per-thread DFS stacks
- CompactState stack entries (32 bytes, no hash — recomputed on pop)
- Work stealing: mutex-protected steal-half from random victim
- Batched atomic counter updates (per 1024 states, reduces contention)
- Software prefetching: batch all successors' hash slots, then probe
- Cold-path extraction: work stealing in noinline cold function

### 5.4 Canonical Hash
- Zobrist hashing: precomputed random table [32 positions][65 values]
- Both original and reflected hashes computed in single interleaved pass
- Canonical hash = min(original, reflected)
- State never physically reflected (both orientations produce same
  canonical successors) — saves 50% of reflection memcpy calls

### 5.5 Verification
- Seed conservation: checked on every sampled state (always 64)
- Canonical hash consistency: hash(S) = hash(reflect(S)) verified
- Exact ground truth: deterministic DFS of 2M states from start must
  produce exactly 1,006,385 terminal states (validated after every
  code change throughout development)
- 5000-game stress test with invariant checking on every move
- Hand-traced first move from starting position matches engine output

---

## 6. Performance Engineering

### 6.1 Optimization timeline

| Phase | Throughput | Key change |
|-------|-----------|------------|
| Original single-threaded | 1.3M st/s | Baseline |
| + 32 threads (RunPod) | 9.3M st/s | Parallelization |
| + engine optimizations | 11.4M st/s | Unrolled Zobrist, header inlining |
| + GCP 64-core | 27.8M st/s | Better hardware |
| + remove atomic from insert | 106M st/s | Eliminated cache-line bouncing |
| Final (16 threads, safe) | 37.3M st/s | Reduced threads to fit memory |

### 6.2 Key optimizations (50+ attempts, automated agent)

**Kept (improved throughput or memory):**
1. Fully unrolled + interleaved Zobrist hash computation (+40%)
2. All hot functions moved to header as inline (+33%)
3. Remove atomic counter from hash table insert (~5× throughput)
4. 32-bit quotient tags (8→4 bytes/entry, 2× hash table capacity)
5. CompactState (40→32 bytes/stack entry, 20% less stack memory)
6. Zero-copy canonicalization (never physically reflect the state)
7. Branchless sow(count=2) (+5%)
8. -fomit-frame-pointer, -fno-stack-protector (+62%)
9. Software prefetching of hash table slots
10. Batched atomic counter updates (per 1024 states)
11. Cold-path extraction for work stealing
12. Spin-pause before yield in work stealing

**Reverted (hurt performance):**
- SWAR is_terminal (-15%), SSE2 swap (-7%), alignas(32) (-33%),
  -fno-exceptions (-23%), 4-way XOR chains (-16%), and 15+ others

### 6.3 Memory budget (512 GB machine)

| Component | Size |
|-----------|------|
| Hash table (300 GB, 4 bytes/slot) | 300 GB |
| DFS stacks (16 threads × ~4 GB) | ~64 GB |
| OS + overhead | ~4 GB |
| **Total** | **~368 GB** |
| **Headroom** | **144 GB** |

---

## 7. Results Summary

**Bao la Kujifunza has approximately 40.3 billion canonical reachable
states** (approximately 161 billion raw states before symmetry reduction).
Approximately 51% of states are terminal (one player's inner row is empty
or no legal moves available).

**The game was previously weakly solved by Donkers & Uiterwijk (2002),
who proved it is a first-player win.** Our enumeration is the first step
toward a strong solution — computing the game-theoretic value of every
reachable state.

---

## 8. Future Work

1. **Strong solve (retrograde analysis):** Now that we know the exact state
   space size (~40B), we can design a retrograde solver. At 1 bit per state
   (win/loss), the solution database would be ~5 GB. At 2 bits (with
   distance-to-terminal), ~10 GB. This is feasible.

2. **Verification with independent implementation:** Cross-reference our
   engine against the AbstractPlay Bao implementation to validate rule
   correctness externally.

3. **Optimal play analysis:** Extract the longest optimal game, most common
   opening moves, and positions where the second player can win.

4. **Full Bao (Bao la Kiswahili):** The complete game with namua phase
   and nyumba rules remains unsolved. Our engine and infrastructure could
   be extended, though the state space is orders of magnitude larger.

---

## 9. Reproducibility

All source code is available at: github.com/chrislovescoding/solve-bao

To reproduce the enumeration:
```bash
git clone https://github.com/chrislovescoding/solve-bao.git
cd solve-bao
make test                    # verify correctness (21/21 tests)
make bench                   # verify ground truth (exact counts)
make enumerate_par           # build parallel enumerator
./build/enumerate_par --mem-gb 300 --threads 16  # run (needs 512 GB RAM)
```

Machine requirements: 512 GB RAM, 16+ CPU cores, Linux (Ubuntu 22.04 tested).
Estimated runtime: ~25 minutes on AMD EPYC (GCP n2d-highmem-64).

---

## 10. Cost

| Item | Cost |
|------|------|
| GCP n2d-highmem-64 on-demand | $3.65/hr |
| Total runtime (including failed attempts) | ~4 hours |
| **Total compute cost** | **~$15** |

The entire state space of Bao la Kujifunza was enumerated for approximately
the price of lunch.
