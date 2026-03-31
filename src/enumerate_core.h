#pragma once
/*
 * enumerate_core.h — Shared parallel enumeration infrastructure
 *
 * Contains: AtomicHashSet, CompactState, ThreadStats, ThreadWork,
 * worker function, warmup function. Used by BOTH the production
 * enumerator AND the benchmark. Single source of truth.
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
// Lock-free hash set (32-bit quotient tags, Fastrange, optional huge pages)
// ---------------------------------------------------------------------------

class AtomicHashSet {
public:
    explicit AtomicHashSet(size_t capacity, bool use_hugepages = true) {
        capacity_ = capacity;
        count_.store(0, std::memory_order_relaxed);

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
                fprintf(stderr, "  Hash table: huge pages (%zu GB, 4 bytes/slot)\n",
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

    ~AtomicHashSet() {
#ifdef __linux__
        if (use_mmap_) { munmap(table_, alloc_bytes_); return; }
#endif
        free(table_);
    }

    inline size_t slot_for(uint64_t h) const {
        return (size_t)(((__uint128_t)h * capacity_) >> 64);
    }

    static inline uint32_t tag_for(uint64_t h) {
        return (uint32_t)(h) | 1u;
    }

    inline size_t next_slot(size_t slot) const {
        size_t s = slot + 1;
        return s < capacity_ ? s : 0;
    }

    bool insert(uint64_t h) {
        uint32_t tag = tag_for(h);
        size_t slot = slot_for(h);
        while (true) {
            uint32_t expected = 0;
            if (table_[slot].compare_exchange_weak(
                    expected, tag,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return true;
            }
            if (expected == tag)
                return false;
            slot = next_slot(slot);
        }
    }

    void prefetch(uint64_t h) const {
        __builtin_prefetch(&table_[slot_for(h)], 1, 0);
    }

    size_t count()        const { return count_.load(std::memory_order_relaxed); }
    size_t capacity()     const { return capacity_; }
    size_t memory_bytes() const { return capacity_ * sizeof(uint32_t); }
    double load()         const { return (double)count() / capacity_; }

private:
    std::atomic<uint32_t>* table_;
    size_t capacity_;
    std::atomic<size_t> count_;
    size_t alloc_bytes_ = 0;
    bool use_mmap_ = false;
};

// ---------------------------------------------------------------------------
// Compact stack entry (32 bytes — no hash field)
// ---------------------------------------------------------------------------

struct CompactState {
    uint8_t pits[TOTAL_PITS];
    void from_bao(const BaoState& s) { memcpy(pits, s.pits, TOTAL_PITS); }
    void to_bao(BaoState& s) const   { memcpy(s.pits, pits, TOTAL_PITS); }
};

// ---------------------------------------------------------------------------
// Canonical hash
// ---------------------------------------------------------------------------

static inline uint64_t canonical_hash(const BaoState& s) {
    return s.canonical_hash_only();
}

// ---------------------------------------------------------------------------
// Per-thread stats (cache-line padded)
// ---------------------------------------------------------------------------

struct alignas(64) ThreadStats {
    size_t states        = 0;  // new states inserted into hash table
    size_t terminal      = 0;
    size_t moves         = 0;
    size_t infinite      = 0;
    size_t inner_row_end = 0;
    size_t stack_peak    = 0;
    size_t steals        = 0;
    size_t pops          = 0;  // total states popped from stack
    size_t pushes        = 0;  // total states pushed to stack
    size_t insert_true   = 0;  // insert() returned true (new)
    size_t insert_false  = 0;  // insert() returned false (duplicate)
    char _pad[0];
};

// ---------------------------------------------------------------------------
// Per-thread work (stack + steal mutex)
// ---------------------------------------------------------------------------

struct alignas(64) ThreadWork {
    std::vector<CompactState> stack;
    std::mutex mtx;
    std::atomic<bool> active{true};
};

// ---------------------------------------------------------------------------
// Shared globals
// ---------------------------------------------------------------------------

struct EnumGlobals {
    std::atomic<size_t> states{0};
    std::atomic<size_t> terminal{0};
    std::atomic<bool>   table_full{false};
    std::atomic<bool>   done{false};
    std::atomic<size_t> last_discovery_states{0};  // snapshot for drain detection
    std::atomic<bool>   draining{false};           // true = no new states, just drain stacks
    int num_threads = 0;
    ThreadWork* work = nullptr;
    size_t max_states = 0;
};

// ---------------------------------------------------------------------------
// Work stealing (cold path, extracted to reduce hot-path code size)
// ---------------------------------------------------------------------------

enum class StealResult { STOLEN, ALL_IDLE, RETRY };

__attribute__((noinline, cold))
static StealResult try_steal(ThreadWork& my_work, ThreadStats& stats,
                              int tid, EnumGlobals& g, uint32_t& rng) {
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
    for (int sp = 0; sp < 64; ++sp)
        _mm_pause();
    my_work.active.store(true, std::memory_order_relaxed);
    return StealResult::RETRY;
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

__attribute__((hot))
static void enum_worker(AtomicHashSet& visited, ThreadStats& stats,
                        int tid, EnumGlobals& g) {
    stats = {};
    ThreadWork& my_work = g.work[tid];
    size_t local_terminal_batch = 0;
    my_work.stack.reserve(100000);

    // Pin thread to a specific logical core for consistent scheduling
#ifdef _WIN32
    SetThreadAffinityMask(GetCurrentThread(), 1ULL << (tid % 64));
#endif

    uint32_t rng = (uint32_t)(tid * 2654435761u + 1);

    while (!g.table_full.load(std::memory_order_relaxed) &&
           !g.done.load(std::memory_order_relaxed)) {
        BaoState state;
        {
            if (__builtin_expect(my_work.stack.empty(), 0)) {
                // If draining (no new states being found), don't steal —
                // just let this thread finish. Prevents endless redistribution
                // of stale stack entries between threads.
                if (g.draining.load(std::memory_order_relaxed))
                    return;

                StealResult r = try_steal(my_work, stats, tid, g, rng);
                if (r == StealResult::ALL_IDLE) return;
                if (r == StealResult::RETRY) continue;
                // STOLEN: fall through to pop
            }

            CompactState cs = my_work.stack.back();
            my_work.stack.pop_back();
            cs.to_bao(state);
            stats.pops++;
        }

        // Track stack peak (only check occasionally to reduce overhead)
        if (__builtin_expect((stats.states & 0xFF) == 0, 0)) {
            if (my_work.stack.size() > stats.stack_peak)
                stats.stack_peak = my_work.stack.size();
        }

        // Note: is_terminal() check removed here — all states pushed to
        // the stack are guaranteed non-terminal (checked before push in
        // both worker and warmup).

        Move moves[MAX_MOVES];
        int n = state.generate_moves(moves);

        BaoState succs[MAX_MOVES];
        uint64_t hashes[MAX_MOVES];
        int valid = 0;

        for (int i = 0; i < n; ++i) {
            stats.moves++;
            memcpy(succs[valid].pits, state.pits, TOTAL_PITS);
            MoveResult r = succs[valid].make_move(moves[i]);

            if (__builtin_expect(r != MoveResult::OK, 0)) {
                if (r == MoveResult::INFINITE) stats.infinite++;
                else stats.inner_row_end++;
                continue;
            }

            hashes[valid] = canonical_hash(succs[valid]);
            valid++;
        }

        for (int i = 0; i < valid; ++i)
            visited.prefetch(hashes[i]);

        for (int i = 0; i < valid; ++i) {
            if (!visited.insert(hashes[i])) {
                stats.insert_false++;
                continue;
            }

            stats.insert_true++;
            stats.states++;

            if (__builtin_expect((stats.states & 0x3FF) == 0, 0)) {
                g.states.fetch_add(1024, std::memory_order_relaxed);
                if (local_terminal_batch > 0) {
                    g.terminal.fetch_add(local_terminal_batch, std::memory_order_relaxed);
                    local_terminal_batch = 0;
                }

                size_t total = g.states.load(std::memory_order_relaxed);

                if (__builtin_expect(g.max_states > 0 && total >= g.max_states, 0)) {
                    g.done.store(true, std::memory_order_relaxed);
                    return;
                }

                if ((stats.states & 0xFFFFF) == 0) {
                    if (total > visited.capacity() * 3 / 4) {
                        g.states.fetch_add(stats.states & 0x3FF, std::memory_order_relaxed);
                        g.table_full.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
            }

            if (succs[i].is_terminal()) {
                stats.terminal++;
                local_terminal_batch++;
            } else {
                CompactState cs;
                cs.from_bao(succs[i]);
                my_work.stack.push_back(cs);
                stats.pushes++;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Warmup: single-threaded seed, returns count of states found
// ---------------------------------------------------------------------------

static size_t enum_warmup(AtomicHashSet& visited, EnumGlobals& g,
                          size_t target_stack_entries) {
    std::vector<CompactState> warmup_stack;
    warmup_stack.reserve(target_stack_entries * 2);

    BaoState start;
    start.init_start();
    visited.insert(canonical_hash(start));
    g.states.store(1, std::memory_order_relaxed);
    { CompactState cs; cs.from_bao(start); warmup_stack.push_back(cs); }

    size_t warmup_states = 1;
    while (warmup_stack.size() < target_stack_entries && !warmup_stack.empty()) {
        BaoState st;
        warmup_stack.back().to_bao(st);
        warmup_stack.pop_back();

        if (st.is_terminal()) {
            g.terminal.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        Move moves[MAX_MOVES];
        int n = st.generate_moves(moves);
        for (int i = 0; i < n; ++i) {
            BaoState succ = st;
            MoveResult r = succ.make_move(moves[i]);
            if (r != MoveResult::OK) continue;
            uint64_t h = canonical_hash(succ);
            if (!visited.insert(h)) continue;
            warmup_states++;
            g.states.fetch_add(1, std::memory_order_relaxed);
            if (!succ.is_terminal()) {
                CompactState cs; cs.from_bao(succ);
                warmup_stack.push_back(cs);
            } else {
                g.terminal.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // Distribute to thread stacks
    g.work = new ThreadWork[g.num_threads];
    for (size_t i = 0; i < warmup_stack.size(); ++i)
        g.work[i % g.num_threads].stack.push_back(warmup_stack[i]);

    return warmup_states;
}
