# Solver Design — Strongly Solving Bao la Kujifunza

## Architecture Overview

```
Pass 0 (INIT):   DFS enumerate → label terminals LOSS, others UNKNOWN
Pass 1..N (SOLVE): DFS scan → resolve UNKNOWN states from successor labels
Pass N+1 (VERIFY): DFS scan → confirm no UNKNOWN, start = WIN
```

All passes use the SAME DFS traversal and SAME hash table (4 bytes/entry).
The label is encoded in the 2 low bits of each hash table entry.
No additional memory needed beyond enumeration.

## Hash Table Entry Encoding

```
32-bit entry:
  0x00000000           = empty slot (sentinel)
  bits [31:2] = tag    = lower 30 bits of Zobrist hash (shifted left 2)
  bits [1:0]  = label:
    01 = UNKNOWN (state exists, not yet resolved)
    10 = WIN     (current player wins with optimal play)
    11 = LOSS    (current player loses against optimal play)
    00 = (reserved / never used for occupied slots)
```

Tag comparison ignores label bits: `(entry >> 2) == (query >> 2)`
Label update via atomic CAS: only UNKNOWN → WIN or UNKNOWN → LOSS

## Resolution Rules

For a non-terminal state S with successors S1, S2, ..., Sk:

```
if ANY Si has label LOSS  →  S is WIN  (we can move to a losing position for opponent)
if ALL Si have label WIN  →  S is LOSS (every move leads to opponent winning)
otherwise                 →  S remains UNKNOWN (not yet resolvable)
```

Note: after swap_sides(), successor labels are from the OPPONENT's perspective.
A successor labeled LOSS means the opponent (who just became to-move) loses.
That's good for us → we WIN.

## Pass Structure

### Pass 0: Initialize
- Full DFS enumeration from starting position
- For each discovered state:
  - Compute canonical hash
  - If terminal → insert with label LOSS (player to move has no moves or empty inner row)
  - If not terminal → insert with label UNKNOWN
- Output: hash table fully populated with ~40.3B entries

### Pass 1..N: Resolve
- Full DFS from starting position (same traversal as enumeration)
- For each state encountered:
  - Look up own label in table
  - If not UNKNOWN → skip (already resolved)
  - If UNKNOWN:
    - Generate all legal moves
    - Execute each move → get successor state
    - Compute successor's canonical hash
    - Look up successor's label in table
    - Apply resolution rules:
      - Any successor LOSS? → CAS own label to WIN
      - All successors WIN? → CAS own label to LOSS
      - Otherwise → leave as UNKNOWN
- Track: states_resolved_this_pass
- Stop when states_resolved_this_pass == 0

### Why does this converge?

Pass 0: ~20.6B terminal states labeled LOSS
Pass 1: States with a terminal successor → WIN (lots of these)
Pass 2: States where all successors are now WIN → LOSS
Pass 3: More WINs propagate from new LOSSes
...and so on.

Each pass resolves more states. Like peeling an onion — the game tree
is resolved from the leaves inward. Convergence is guaranteed because:
- The game graph is finite (40.3B states)
- Each state is resolved at most once (UNKNOWN → WIN or UNKNOWN → LOSS)
- Each pass resolves at least as many states as the "frontier" of newly resolved predecessors

Expected passes: O(game_diameter). For Bao, games typically last
50-500 moves. Expect 50-200 passes. At ~20 min per pass = 17-67 hours.
Could be optimized with a worklist approach.

### Optimization: Worklist-Based Resolution

Instead of full DFS each pass, maintain a worklist of states whose
labels changed. On the next pass, only check predecessors of changed
states. This dramatically reduces work per pass after the first few.

Problem: computing predecessors in Bao is hard (complex relay chains).
Alternative: during each DFS, if we resolve a state, add its parent
(the state we're DFS-ing from) to a "re-check" list. This is a
hybrid approach that focuses work where changes propagate.

## Memory Layout

For 40.3B states on a 512 GB machine:

| Component | Size |
|-----------|------|
| Hash table (4 bytes/entry, ~57B slots at 70% load) | ~228 GB |
| DFS stacks (16 threads × ~4 GB) | ~64 GB |
| OS overhead | ~4 GB |
| **Total** | **~296 GB** |
| **Headroom** | **216 GB** |

The solver uses the EXACT SAME memory as the enumerator. No additional
storage needed — labels are encoded in the existing hash table entries.

## Correctness Guards

### Ground truth for benchmark
- Run exhaustive minimax on a small state space (first 50K states from
  deterministic DFS)
- Record exact WIN/LOSS counts and the label of the starting position
- Any change to resolution logic changes these counts → FAIL

### Invariants checked every pass:
1. No state transitions from WIN/LOSS back to UNKNOWN
2. No state labeled both WIN and LOSS
3. WIN + LOSS + UNKNOWN = total states (conservation)
4. All terminal states remain LOSS throughout
5. Seed conservation on sampled states

### Final verification:
1. Starting position label = WIN (matches Donkers 2002 weak solve)
2. UNKNOWN count = 0 (Bao has no draws)
3. Play out optimal game: follow WIN moves, verify game ends in win
4. Spot-check: random positions verified by depth-limited minimax

## Benchmark Design

```
tests/benchmark_solver.cpp
```

The solver benchmark:
1. Build label table from deterministic DFS (50K states)
2. Initialize: label terminals as LOSS
3. Run resolution passes until convergence
4. Measure: states/sec, passes, labels resolved per pass
5. Verify against minimax ground truth

Reports:
- **Resolve throughput:** states resolved per second (maximize)
- **Passes to converge:** number of full scans needed (minimize via algorithm)
- **Memory:** same as enumerator (4 bytes/entry)
- **Correctness:** PASS/FAIL against exact minimax ground truth

## Implementation Files

```
src/solver_core.h          — LabelTable (extends AtomicHashSet with labels),
                             resolution worker, pass management
src/solver.cpp             — Production solver (multi-pass, parallel)
tests/benchmark_solver.cpp — Solver benchmark with minimax ground truth
```

Both solver.cpp and benchmark_solver.cpp import solver_core.h,
just like enumerate_parallel.cpp and benchmark_parallel.cpp import
enumerate_core.h. Single source of truth.

## API Changes to Hash Table

```cpp
class LabelHashTable : public AtomicHashSet {
    // New entry format: (tag_30 << 2) | label_2

    // Insert with initial label (used during init pass)
    bool insert_with_label(uint64_t hash, uint8_t label);

    // Look up label for a hash (returns UNKNOWN/WIN/LOSS/NOT_FOUND)
    uint8_t lookup_label(uint64_t hash) const;

    // Atomically update label: UNKNOWN → WIN or UNKNOWN → LOSS
    // Returns true if update succeeded (was UNKNOWN)
    bool update_label(uint64_t hash, uint8_t new_label);

    // Count states by label
    size_t count_win() const;
    size_t count_loss() const;
    size_t count_unknown() const;
};
```

## Timeline Estimate

| Phase | Time | Notes |
|-------|------|-------|
| Implement solver | 2-4 hours | Extend hash table, write resolution worker |
| Build benchmark + ground truth | 1-2 hours | Minimax on 50K states |
| Run solver (40.3B states) | 17-67 hours | 50-200 passes × 20 min/pass |
| Verification | 1 hour | Check starting position, play optimal game |
| **Total** | **~1-3 days** | Mostly compute time |

With worklist optimization, the solver could be significantly faster
(later passes only scan changed regions, not all 40.3B states).
