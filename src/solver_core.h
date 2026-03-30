#pragma once
/*
 * solver_core.h — Parallel retrograde solver for Bao la Kujifunza
 *
 * Extends enumerate_core.h with label-in-hash resolution.
 * Each 32-bit hash table entry encodes:
 *   bits [31:2] = 30-bit tag
 *   bits [1:0]  = label (01=UNKNOWN, 10=WIN, 11=LOSS, 00=empty)
 *
 * Zero additional memory beyond enumeration.
 */

#include "bao.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>

#include <immintrin.h>

#ifdef _WIN32
#include <windows.h>
#undef INFINITE
#endif

#ifdef __linux__
#include <sys/mman.h>
#endif

// ---------------------------------------------------------------------------
// Labels
// ---------------------------------------------------------------------------

static constexpr uint32_t LABEL_EMPTY   = 0;  // slot is unoccupied
static constexpr uint32_t LABEL_UNKNOWN = 1;  // occupied, not yet resolved
static constexpr uint32_t LABEL_WIN     = 2;  // current player wins
static constexpr uint32_t LABEL_LOSS    = 3;  // current player loses

static constexpr uint32_t LABEL_MASK    = 3;  // bottom 2 bits
static constexpr uint32_t TAG_SHIFT     = 2;  // tag stored in bits [31:2]

// ---------------------------------------------------------------------------
// Label hash table (extends atomic hash set with WIN/LOSS/UNKNOWN labels)
// ---------------------------------------------------------------------------

class LabelHashTable {
public:
    explicit LabelHashTable(size_t capacity, bool use_hugepages = true) {
        capacity_ = capacity;

        size_t bytes = capacity_ * sizeof(uint32_t);
        table_ = nullptr;

#ifdef __linux__
        if (use_hugepages) {
            void* ptr = mmap(nullptr, bytes,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                             -1, 0);
            if (ptr != MAP_FAILED) {
                table_ = (std::atomic<uint32_t>*)ptr;
                alloc_bytes_ = bytes;
                use_mmap_ = true;
                fprintf(stderr, "  Label table: huge pages (%zu GB)\n",
                        bytes / (1024ULL*1024*1024));
            } else {
                fprintf(stderr, "  Huge pages unavailable, falling back to calloc\n");
            }
        }
#else
        (void)use_hugepages;
#endif

        if (!table_) {
            table_ = (std::atomic<uint32_t>*)calloc(capacity_, sizeof(uint32_t));
            alloc_bytes_ = bytes;
            use_mmap_ = false;
        }

        if (!table_) {
            fprintf(stderr, "FATAL: alloc failed (%zu GB)\n",
                    bytes / (1024ULL*1024*1024));
            exit(1);
        }
    }

    ~LabelHashTable() {
#ifdef __linux__
        if (use_mmap_) { munmap(table_, alloc_bytes_); return; }
#endif
        free(table_);
    }

    // --- Slot computation (same as enumerator) ---

    inline size_t slot_for(uint64_t h) const {
        return (size_t)(((__uint128_t)h * capacity_) >> 64);
    }

    // Tag: lower 30 bits of hash, shifted left 2 to make room for label
    static inline uint32_t tag_for(uint64_t h) {
        return ((uint32_t)(h) << TAG_SHIFT);  // label bits will be OR'd in
    }

    inline size_t next_slot(size_t slot) const {
        size_t s = slot + 1;
        return s < capacity_ ? s : 0;
    }

    // --- Insert with initial label (INIT pass) ---

    // Returns true if newly inserted, false if already present.
    bool insert(uint64_t h, uint32_t label) {
        uint32_t entry = tag_for(h) | label;  // tag + label in one word
        uint32_t tag_bits = entry & ~LABEL_MASK;  // tag portion only
        size_t slot = slot_for(h);

        while (true) {
            uint32_t expected = LABEL_EMPTY;
            if (table_[slot].compare_exchange_weak(
                    expected, entry,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return true;  // newly inserted
            }
            // Check if same tag (regardless of label)
            if ((expected & ~LABEL_MASK) == tag_bits)
                return false;  // already present
            slot = next_slot(slot);
        }
    }

    // --- Look up label ---

    // Returns LABEL_EMPTY if not found, or the label (UNKNOWN/WIN/LOSS)
    uint32_t lookup(uint64_t h) const {
        uint32_t tag_bits = tag_for(h);
        size_t slot = slot_for(h);

        while (true) {
            uint32_t entry = table_[slot].load(std::memory_order_relaxed);
            if (entry == LABEL_EMPTY)
                return LABEL_EMPTY;  // not found
            if ((entry & ~LABEL_MASK) == tag_bits)
                return entry & LABEL_MASK;  // found, return label
            slot = next_slot(slot);
        }
    }

    // --- Atomic label update: UNKNOWN → WIN or UNKNOWN → LOSS ---

    // Returns true if the update succeeded (was UNKNOWN).
    bool update_label(uint64_t h, uint32_t new_label) {
        uint32_t tag_bits = tag_for(h);
        uint32_t old_entry = tag_bits | LABEL_UNKNOWN;
        uint32_t new_entry = tag_bits | new_label;
        size_t slot = slot_for(h);

        while (true) {
            uint32_t entry = table_[slot].load(std::memory_order_relaxed);
            if (entry == LABEL_EMPTY)
                return false;  // not found (shouldn't happen)
            if ((entry & ~LABEL_MASK) == tag_bits) {
                // Found our slot. Try CAS: UNKNOWN → new_label
                return table_[slot].compare_exchange_strong(
                    old_entry, new_entry,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed);
            }
            slot = next_slot(slot);
        }
    }

    // --- Prefetch ---

    void prefetch(uint64_t h) const {
        __builtin_prefetch(&table_[slot_for(h)], 0, 0);
    }

    void prefetch_write(uint64_t h) const {
        __builtin_prefetch(&table_[slot_for(h)], 1, 0);
    }

    size_t capacity() const { return capacity_; }

private:
    std::atomic<uint32_t>* table_;
    size_t capacity_;
    size_t alloc_bytes_ = 0;
    bool use_mmap_ = false;
};

// ---------------------------------------------------------------------------
// Canonical hash (same as enumerator)
// ---------------------------------------------------------------------------

static inline uint64_t canonical_hash(const BaoState& s) {
    return s.canonical_hash_only();
}

// ---------------------------------------------------------------------------
// Compact stack entry (same as enumerator)
// ---------------------------------------------------------------------------

struct CompactState {
    uint8_t pits[TOTAL_PITS];
    void from_bao(const BaoState& s) { memcpy(pits, s.pits, TOTAL_PITS); }
    void to_bao(BaoState& s) const   { memcpy(s.pits, pits, TOTAL_PITS); }
};

// ---------------------------------------------------------------------------
// Per-thread stats for solver
// ---------------------------------------------------------------------------

struct alignas(64) SolverThreadStats {
    size_t states_scanned  = 0;
    size_t resolved_win    = 0;
    size_t resolved_loss   = 0;
    size_t already_resolved = 0;
    size_t still_unknown   = 0;
    size_t terminal        = 0;
    size_t moves           = 0;
    size_t stack_peak      = 0;
    size_t steals          = 0;
    char _pad[8];
};

// ---------------------------------------------------------------------------
// Thread work (same structure as enumerator)
// ---------------------------------------------------------------------------

struct alignas(64) SolverThreadWork {
    std::vector<CompactState> stack;
    std::mutex mtx;
    std::atomic<bool> active{true};
};

// ---------------------------------------------------------------------------
// Shared globals for solver
// ---------------------------------------------------------------------------

struct SolverGlobals {
    std::atomic<size_t> states_scanned{0};
    std::atomic<size_t> resolved_this_pass{0};
    std::atomic<bool>   done{false};
    int num_threads = 0;
    SolverThreadWork* work = nullptr;
    int pass_number = 0;  // 0 = init, 1+ = resolution
};

// ---------------------------------------------------------------------------
// Work stealing (cold path)
// ---------------------------------------------------------------------------

enum class StealResult { STOLEN, ALL_IDLE, RETRY };

__attribute__((noinline, cold))
static StealResult solver_try_steal(SolverThreadWork& my_work,
                                     SolverThreadStats& stats,
                                     int tid, SolverGlobals& g,
                                     uint32_t& rng) {
    for (int attempts = 0; attempts < g.num_threads * 2; ++attempts) {
        rng = rng * 1103515245u + 12345u;
        int victim = (int)((rng >> 16) % g.num_threads);
        if (victim == tid) continue;
        if (!g.work[victim].active.load(std::memory_order_relaxed)) continue;

        std::unique_lock<std::mutex> lock(g.work[victim].mtx, std::try_to_lock);
        if (!lock.owns_lock()) continue;

        size_t victim_size = g.work[victim].stack.size();
        if (victim_size < 2) continue;

        size_t steal_count = victim_size / 2;
        auto begin = g.work[victim].stack.end() - steal_count;
        auto end = g.work[victim].stack.end();
        my_work.stack.insert(my_work.stack.end(), begin, end);
        g.work[victim].stack.erase(begin, end);
        stats.steals++;
        return StealResult::STOLEN;
    }

    my_work.active.store(false, std::memory_order_relaxed);
    bool all_idle = true;
    for (int t = 0; t < g.num_threads; ++t) {
        if (g.work[t].active.load(std::memory_order_relaxed) ||
            !g.work[t].stack.empty()) {
            all_idle = false;
            break;
        }
    }
    if (all_idle) return StealResult::ALL_IDLE;
    for (int sp = 0; sp < 64; ++sp) _mm_pause();
    my_work.active.store(true, std::memory_order_relaxed);
    return StealResult::RETRY;
}

// ---------------------------------------------------------------------------
// INIT worker: enumerate all states, label terminals
// ---------------------------------------------------------------------------

__attribute__((hot))
static void init_worker(LabelHashTable& table, SolverThreadStats& stats,
                        int tid, SolverGlobals& g) {
    stats = {};
    SolverThreadWork& my_work = g.work[tid];
    my_work.stack.reserve(100000);

    uint32_t rng = (uint32_t)(tid * 2654435761u + 1);

    while (!g.done.load(std::memory_order_relaxed)) {
        BaoState state;
        {
            if (__builtin_expect(my_work.stack.empty(), 0)) {
                StealResult r = solver_try_steal(my_work, stats, tid, g, rng);
                if (r == StealResult::ALL_IDLE) return;
                if (r == StealResult::RETRY) continue;
            }
            CompactState cs = my_work.stack.back();
            my_work.stack.pop_back();
            cs.to_bao(state);
        }

        if (my_work.stack.size() > stats.stack_peak)
            stats.stack_peak = my_work.stack.size();

        // This state is guaranteed non-terminal (filtered before push)
        Move moves[MAX_MOVES];
        int n = state.generate_moves(moves);

        BaoState succs[MAX_MOVES];
        uint64_t hashes[MAX_MOVES];
        int valid = 0;

        for (int i = 0; i < n; ++i) {
            stats.moves++;
            memcpy(succs[valid].pits, state.pits, TOTAL_PITS);
            MoveResult r = succs[valid].make_move(moves[i]);
            if (__builtin_expect(r != MoveResult::OK, 0)) continue;
            hashes[valid] = canonical_hash(succs[valid]);
            valid++;
        }

        for (int i = 0; i < valid; ++i)
            table.prefetch(hashes[i]);

        for (int i = 0; i < valid; ++i) {
            bool is_term = succs[i].is_terminal();
            uint32_t label = is_term ? LABEL_LOSS : LABEL_UNKNOWN;

            if (!table.insert(hashes[i], label))
                continue;  // already inserted

            stats.states_scanned++;
            if (is_term) {
                stats.terminal++;
            } else {
                CompactState cs;
                cs.from_bao(succs[i]);
                my_work.stack.push_back(cs);
            }

            if (__builtin_expect((stats.states_scanned & 0x3FF) == 0, 0)) {
                g.states_scanned.fetch_add(1024, std::memory_order_relaxed);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// RESOLVE worker: one pass of resolution
// ---------------------------------------------------------------------------

__attribute__((hot))
static void resolve_worker(LabelHashTable& table, SolverThreadStats& stats,
                           int tid, SolverGlobals& g) {
    stats = {};
    SolverThreadWork& my_work = g.work[tid];
    my_work.stack.reserve(100000);
    my_work.active.store(true, std::memory_order_relaxed);

    uint32_t rng = (uint32_t)(tid * 2654435761u + 1);

    while (!g.done.load(std::memory_order_relaxed)) {
        BaoState state;
        {
            if (__builtin_expect(my_work.stack.empty(), 0)) {
                StealResult r = solver_try_steal(my_work, stats, tid, g, rng);
                if (r == StealResult::ALL_IDLE) return;
                if (r == StealResult::RETRY) continue;
            }
            CompactState cs = my_work.stack.back();
            my_work.stack.pop_back();
            cs.to_bao(state);
        }

        if (my_work.stack.size() > stats.stack_peak)
            stats.stack_peak = my_work.stack.size();

        uint64_t my_hash = canonical_hash(state);
        uint32_t my_label = table.lookup(my_hash);

        // Skip already-resolved states
        if (my_label == LABEL_WIN || my_label == LABEL_LOSS) {
            stats.already_resolved++;
            // But we still need to DFS into successors to find UNKNOWN states
        }

        // Terminal states have no successors (already LOSS from init)
        if (state.is_terminal()) {
            stats.terminal++;
            continue;
        }

        Move moves[MAX_MOVES];
        int n = state.generate_moves(moves);

        // Generate successors, compute hashes
        BaoState succs[MAX_MOVES];
        uint64_t hashes[MAX_MOVES];
        int valid = 0;

        for (int i = 0; i < n; ++i) {
            stats.moves++;
            memcpy(succs[valid].pits, state.pits, TOTAL_PITS);
            MoveResult r = succs[valid].make_move(moves[i]);
            if (__builtin_expect(r != MoveResult::OK, 0)) continue;
            hashes[valid] = canonical_hash(succs[valid]);
            valid++;
        }

        // Prefetch successor labels
        for (int i = 0; i < valid; ++i)
            table.prefetch(hashes[i]);

        // Check successor labels for resolution
        bool found_loss = false;     // any successor is LOSS? → we WIN
        bool all_win = true;         // all successors WIN? → we LOSS
        bool any_unknown = false;

        for (int i = 0; i < valid; ++i) {
            uint32_t succ_label = table.lookup(hashes[i]);

            // Note: successor label is from OPPONENT's perspective (after swap)
            // Successor LOSS = opponent loses = WE WIN
            // Successor WIN = opponent wins = WE LOSE
            if (succ_label == LABEL_LOSS) {
                found_loss = true;
                // Don't break — still need to push successors for DFS
            }
            if (succ_label != LABEL_WIN) {
                all_win = false;
            }
            if (succ_label == LABEL_UNKNOWN) {
                any_unknown = true;
            }
        }

        // Try to resolve this state
        if (my_label == LABEL_UNKNOWN) {
            if (found_loss) {
                if (table.update_label(my_hash, LABEL_WIN)) {
                    stats.resolved_win++;
                    g.resolved_this_pass.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (all_win && !any_unknown) {
                if (table.update_label(my_hash, LABEL_LOSS)) {
                    stats.resolved_loss++;
                    g.resolved_this_pass.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                stats.still_unknown++;
            }
        }

        stats.states_scanned++;
        if (__builtin_expect((stats.states_scanned & 0x3FF) == 0, 0)) {
            g.states_scanned.fetch_add(1024, std::memory_order_relaxed);
        }

        // Push non-terminal successors for DFS traversal
        // (We need to visit ALL reachable states each pass)
        for (int i = 0; i < valid; ++i) {
            if (!succs[i].is_terminal()) {
                // Only push if we haven't already visited this state in this pass
                // We use the hash table's existence check — but all states are already
                // in the table from the init pass. So we need a separate "visited this pass"
                // mechanism. For simplicity, we push ALL non-terminal successors and
                // accept some redundant work. The hash table lookup for label is fast.

                // TODO: Add a per-pass visited bit to avoid redundant DFS.
                // For now, rely on the DFS stack's natural dedup (states deep in
                // the tree are unlikely to be reached from multiple parents in
                // the same DFS subtree assigned to this thread).
                CompactState cs;
                cs.from_bao(succs[i]);
                my_work.stack.push_back(cs);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Warmup: single-threaded seed for both init and resolve passes
// ---------------------------------------------------------------------------

static size_t solver_warmup_init(LabelHashTable& table, SolverGlobals& g,
                                  size_t target_stack_entries) {
    std::vector<CompactState> warmup_stack;
    warmup_stack.reserve(target_stack_entries * 2);

    BaoState start;
    start.init_start();
    uint64_t h = canonical_hash(start);
    uint32_t label = start.is_terminal() ? LABEL_LOSS : LABEL_UNKNOWN;
    table.insert(h, label);
    g.states_scanned.store(1, std::memory_order_relaxed);

    if (!start.is_terminal()) {
        CompactState cs; cs.from_bao(start);
        warmup_stack.push_back(cs);
    }

    size_t count = 1;
    while (warmup_stack.size() < target_stack_entries && !warmup_stack.empty()) {
        BaoState st;
        warmup_stack.back().to_bao(st);
        warmup_stack.pop_back();

        Move moves[MAX_MOVES];
        int n = st.generate_moves(moves);
        for (int i = 0; i < n; ++i) {
            BaoState succ = st;
            if (succ.make_move(moves[i]) != MoveResult::OK) continue;
            uint64_t sh = canonical_hash(succ);
            bool is_term = succ.is_terminal();
            uint32_t sl = is_term ? LABEL_LOSS : LABEL_UNKNOWN;
            if (!table.insert(sh, sl)) continue;
            count++;
            g.states_scanned.fetch_add(1, std::memory_order_relaxed);
            if (!is_term) {
                CompactState cs; cs.from_bao(succ);
                warmup_stack.push_back(cs);
            }
        }
    }

    g.work = new SolverThreadWork[g.num_threads];
    for (size_t i = 0; i < warmup_stack.size(); ++i)
        g.work[i % g.num_threads].stack.push_back(warmup_stack[i]);

    return count;
}

// Re-seed the stacks for a resolution pass (DFS from start)
static void solver_warmup_resolve(LabelHashTable& table, SolverGlobals& g,
                                   size_t target_stack_entries) {
    // Clear all thread stacks
    for (int t = 0; t < g.num_threads; ++t) {
        g.work[t].stack.clear();
        g.work[t].active.store(true, std::memory_order_relaxed);
    }

    // Re-seed from starting position
    std::vector<CompactState> warmup_stack;
    warmup_stack.reserve(target_stack_entries * 2);

    BaoState start;
    start.init_start();
    if (!start.is_terminal()) {
        CompactState cs; cs.from_bao(start);
        warmup_stack.push_back(cs);
    }

    // BFS/DFS to build initial stack entries
    // We use a simple visited set (just hashes) to avoid pushing duplicates
    // during warmup. This is small and temporary.
    std::vector<uint64_t> visited_warmup;
    visited_warmup.reserve(target_stack_entries * 2);
    visited_warmup.push_back(canonical_hash(start));

    size_t head = 0;
    while (warmup_stack.size() < target_stack_entries && head < warmup_stack.size()) {
        BaoState st;
        warmup_stack[head].to_bao(st);
        head++;

        if (st.is_terminal()) continue;

        Move moves[MAX_MOVES];
        int n = st.generate_moves(moves);
        for (int i = 0; i < n; ++i) {
            BaoState succ = st;
            if (succ.make_move(moves[i]) != MoveResult::OK) continue;
            if (succ.is_terminal()) continue;
            uint64_t sh = canonical_hash(succ);
            // Simple linear scan for small warmup set
            bool found = false;
            for (size_t j = 0; j < visited_warmup.size(); ++j) {
                if (visited_warmup[j] == sh) { found = true; break; }
            }
            if (found) continue;
            visited_warmup.push_back(sh);
            CompactState cs; cs.from_bao(succ);
            warmup_stack.push_back(cs);
        }
    }

    // Distribute to thread stacks (skip the ones we already processed via BFS head)
    for (size_t i = head; i < warmup_stack.size(); ++i)
        g.work[i % g.num_threads].stack.push_back(warmup_stack[i]);
}

// ---------------------------------------------------------------------------
// Minimax solver (single-threaded, for ground truth on small state spaces)
// ---------------------------------------------------------------------------

// Returns LABEL_WIN or LABEL_LOSS for the given state.
// Uses memoization via the label table.
// Only works for small state spaces (stack depth = game tree depth).
static uint32_t minimax_solve(BaoState& state, LabelHashTable& table) {
    uint64_t h = canonical_hash(state);
    uint32_t cached = table.lookup(h);

    if (cached == LABEL_WIN || cached == LABEL_LOSS)
        return cached;

    if (state.is_terminal()) {
        table.insert(h, LABEL_LOSS);
        return LABEL_LOSS;
    }

    // Insert as UNKNOWN first (prevents infinite recursion on cycles)
    table.insert(h, LABEL_UNKNOWN);

    Move moves[MAX_MOVES];
    int n = state.generate_moves(moves);

    bool found_loss = false;
    for (int i = 0; i < n; ++i) {
        BaoState succ = state;
        MoveResult r = succ.make_move(moves[i]);
        if (r == MoveResult::INNER_ROW_EMPTY) {
            // Opponent's inner row emptied → they lose → we win
            found_loss = true;
            break;
        }
        if (r != MoveResult::OK) continue;

        uint32_t succ_val = minimax_solve(succ, table);
        if (succ_val == LABEL_LOSS) {
            found_loss = true;
            break;
        }
    }

    uint32_t result = found_loss ? LABEL_WIN : LABEL_LOSS;
    table.update_label(h, result);
    return result;
}
