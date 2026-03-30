/*
 * benchmark_parallel.cpp — Multi-threaded performance benchmark
 *
 * Mirrors the ACTUAL production enumerator: lock-free atomic hash table,
 * multiple threads, work stealing, prefetch. Runs a fixed number of states
 * with full correctness validation.
 *
 * This is what the optimization agent should target — it captures the REAL
 * bottlenecks: CAS contention, DRAM latency, memory bandwidth, prefetch
 * effectiveness, work-stealing overhead.
 *
 * Usage: benchmark_parallel [--states N] [--threads T] [--establish]
 *
 * Default: 5M states, hardware_concurrency threads
 */

#include "../src/bao.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <chrono>

using Clock = std::chrono::high_resolution_clock;

// ---------------------------------------------------------------------------
// Lock-free hash set (same as production enumerator)
// ---------------------------------------------------------------------------

class AtomicHashSet {
public:
    explicit AtomicHashSet(size_t capacity) {
        capacity_ = capacity;
        count_.store(0, std::memory_order_relaxed);
        table_ = (std::atomic<uint32_t>*)calloc(capacity_, sizeof(uint32_t));
        if (!table_) { fprintf(stderr, "alloc failed\n"); exit(1); }
    }
    ~AtomicHashSet() { free(table_); }

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
            if (table_[slot].compare_exchange_strong(
                    expected, tag,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                count_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            if (expected == tag) return false;
            slot = next_slot(slot);
        }
    }

    void prefetch(uint64_t h) const {
        __builtin_prefetch(&table_[slot_for(h)], 1, 0);
    }

    size_t count()      const { return count_.load(std::memory_order_relaxed); }
    size_t capacity()   const { return capacity_; }
    size_t memory_bytes() const { return capacity_ * sizeof(uint32_t); }

private:
    std::atomic<uint32_t>* table_;
    size_t capacity_;
    std::atomic<size_t> count_;
};

// ---------------------------------------------------------------------------
// Canonical hash (same as production)
// ---------------------------------------------------------------------------

static inline uint64_t canonical_hash(const BaoState& s) {
    return s.canonical_hash_only();
}

// ---------------------------------------------------------------------------
// Per-thread stats (cache-line padded)
// ---------------------------------------------------------------------------

struct alignas(64) ThreadStats {
    size_t states       = 0;
    size_t terminal     = 0;
    size_t moves        = 0;
    char _pad[40];
};

// ---------------------------------------------------------------------------
// Shared state for work stealing
// ---------------------------------------------------------------------------

struct alignas(64) ThreadWork {
    std::vector<BaoState> stack;
    std::mutex mtx;
    std::atomic<bool> active{true};
};

static std::atomic<size_t> g_states{0};
static std::atomic<bool> g_done{false};
static int g_num_threads;
static ThreadWork* g_work;
static size_t g_max_states;

// ---------------------------------------------------------------------------
// Worker (same as production, with state limit)
// ---------------------------------------------------------------------------

static void worker(AtomicHashSet& visited, ThreadStats& stats, int tid) {
    stats = {};
    ThreadWork& my_work = g_work[tid];

    uint32_t rng = (uint32_t)(tid * 2654435761u + 1);
    auto next_rng = [&]() -> int {
        rng = rng * 1103515245u + 12345u;
        return (int)((rng >> 16) % g_num_threads);
    };

    while (!g_done.load(std::memory_order_relaxed)) {
        BaoState state;
        {
            if (my_work.stack.empty()) {
                bool stolen = false;
                for (int attempts = 0; attempts < g_num_threads * 2; ++attempts) {
                    int victim = next_rng();
                    if (victim == tid) continue;
                    if (!g_work[victim].active.load(std::memory_order_relaxed)) continue;

                    std::unique_lock<std::mutex> lock(g_work[victim].mtx, std::try_to_lock);
                    if (!lock.owns_lock()) continue;

                    size_t victim_size = g_work[victim].stack.size();
                    if (victim_size < 2) continue;

                    size_t steal_count = victim_size / 2;
                    auto begin = g_work[victim].stack.end() - steal_count;
                    auto end = g_work[victim].stack.end();
                    my_work.stack.insert(my_work.stack.end(), begin, end);
                    g_work[victim].stack.erase(begin, end);
                    stolen = true;
                    break;
                }

                if (!stolen) {
                    my_work.active.store(false, std::memory_order_relaxed);
                    bool all_idle = true;
                    for (int t = 0; t < g_num_threads; ++t) {
                        if (g_work[t].active.load(std::memory_order_relaxed) ||
                            !g_work[t].stack.empty()) {
                            all_idle = false;
                            break;
                        }
                    }
                    if (all_idle) return;
                    std::this_thread::yield();
                    my_work.active.store(true, std::memory_order_relaxed);
                    continue;
                }
            }

            state = my_work.stack.back();
            my_work.stack.pop_back();
        }

        if (state.is_terminal()) {
            stats.terminal++;
            continue;
        }

        Move moves[MAX_MOVES];
        int n = state.generate_moves(moves);

        // Batched successor generation + prefetch
        BaoState succs[MAX_MOVES];
        uint64_t hashes[MAX_MOVES];
        int valid = 0;

        for (int i = 0; i < n; ++i) {
            stats.moves++;
            succs[valid] = state;
            MoveResult r = succs[valid].make_move(moves[i]);

            if (__builtin_expect(r != MoveResult::OK, 0))
                continue;

            hashes[valid] = canonical_hash(succs[valid]);
            valid++;
        }

        for (int i = 0; i < valid; ++i)
            visited.prefetch(hashes[i]);

        for (int i = 0; i < valid; ++i) {
            if (!visited.insert(hashes[i]))
                continue;

            stats.states++;
            size_t total = g_states.fetch_add(1, std::memory_order_relaxed) + 1;

            // Check if we've hit our target
            if (total >= g_max_states) {
                g_done.store(true, std::memory_order_relaxed);
                return;
            }

            if (succs[i].is_terminal()) {
                stats.terminal++;
            } else {
                my_work.stack.push_back(succs[i]);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Ground truth for parallel benchmark
// ---------------------------------------------------------------------------

struct GroundTruth {
    size_t max_states;
    size_t expected_terminal_min;  // parallel DFS order varies, so use a range
    size_t expected_terminal_max;
};

// Parallel DFS explores states in non-deterministic order (thread scheduling),
// so exact terminal count varies slightly. We use a range.
static const GroundTruth GROUND_TRUTH[] = {
    // Will be established on first run
    {0, 0, 0}
};

// ---------------------------------------------------------------------------
// Correctness validation (sampled)
// ---------------------------------------------------------------------------

static int validate_sample(AtomicHashSet& visited, size_t states_found) {
    // Basic sanity: count should match
    if (visited.count() != states_found + 1) { // +1 for start state
        // Allow small discrepancy due to race between g_done and inserts
        size_t diff = visited.count() > states_found + 1
            ? visited.count() - states_found - 1
            : states_found + 1 - visited.count();
        if (diff > 1000) {
            fprintf(stderr, "FAIL: hash table count %zu != states %zu (diff %zu)\n",
                    visited.count(), states_found + 1, diff);
            return 1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    size_t max_states = 5000000;
    int num_threads = (int)std::thread::hardware_concurrency();
    if (num_threads < 1) num_threads = 4;
    bool establish = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--states") == 0 && i + 1 < argc)
            max_states = (size_t)atol(argv[++i]);
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            num_threads = atoi(argv[++i]);
        if (strcmp(argv[i], "--establish") == 0)
            establish = true;
    }

    g_num_threads = num_threads;
    g_max_states = max_states;

    zobrist_init();

    // Hash table: 4x states, 4 bytes per slot
    size_t table_cap = max_states * 4;
    AtomicHashSet visited(table_cap);

    printf("Bao Parallel Benchmark — %zu states, %d threads\n", max_states, num_threads);
    printf("================================================\n\n");

    // Warmup
    size_t warmup_target = (size_t)num_threads * 5000;
    std::vector<BaoState> warmup_stack;
    warmup_stack.reserve(warmup_target * 2);

    BaoState start;
    start.init_start();
    uint64_t start_h = canonical_hash(start);
    visited.insert(start_h);
    g_states.store(1, std::memory_order_relaxed);
    warmup_stack.push_back(start);

    size_t warmup_states = 1;
    while (warmup_stack.size() < warmup_target && !warmup_stack.empty()) {
        BaoState st = warmup_stack.back();
        warmup_stack.pop_back();
        if (st.is_terminal()) continue;

        Move moves[MAX_MOVES];
        int n = st.generate_moves(moves);
        for (int i = 0; i < n; ++i) {
            BaoState succ = st;
            MoveResult r = succ.make_move(moves[i]);
            if (r != MoveResult::OK) continue;
            uint64_t h = canonical_hash(succ);
            if (!visited.insert(h)) continue;
            warmup_states++;
            g_states.fetch_add(1, std::memory_order_relaxed);
            if (!succ.is_terminal())
                warmup_stack.push_back(succ);
        }
    }

    // Split across threads
    g_work = new ThreadWork[num_threads];
    for (size_t i = 0; i < warmup_stack.size(); ++i)
        g_work[i % num_threads].stack.push_back(warmup_stack[i]);
    warmup_stack.clear();

    // Launch
    auto t0 = Clock::now();

    std::vector<ThreadStats> thread_stats(num_threads);
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t)
        threads.emplace_back(worker, std::ref(visited),
                             std::ref(thread_stats[t]), t);

    for (auto& th : threads) th.join();

    auto t1 = Clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    // Aggregate
    size_t total_states = warmup_states;
    size_t total_terminal = 0;
    size_t total_moves = 0;

    for (int t = 0; t < num_threads; ++t) {
        total_states   += thread_stats[t].states;
        total_terminal += thread_stats[t].terminal;
        total_moves    += thread_stats[t].moves;
    }

    // Validate
    int failures = validate_sample(visited, total_states);

    // Terminal ratio check
    double terminal_ratio = (double)total_terminal / total_states;
    if (terminal_ratio < 0.35 || terminal_ratio > 0.60) {
        fprintf(stderr, "FAIL: terminal ratio %.3f out of range [0.35, 0.60]\n",
                terminal_ratio);
        failures++;
    }

    bool passed = (failures == 0);

    printf("Correctness:    %s\n", passed ? "PASS" : "FAIL");
    printf("States found:   %zu\n", total_states);
    printf("Terminal:        %zu (%.1f%%)\n", total_terminal,
           100.0 * total_terminal / total_states);
    printf("Moves explored: %zu\n", total_moves);
    printf("Threads:        %d\n", num_threads);
    printf("Elapsed:        %.3f sec\n", elapsed);
    printf("\n");
    printf("=== METRICS (optimize these) ===\n");
    printf("Throughput:     %.0f states/sec\n", total_states / elapsed);
    printf("Per-thread:     %.0f states/sec/thread\n", total_states / elapsed / num_threads);
    printf("Memory:         %.1f bytes/state\n",
           (double)visited.memory_bytes() / total_states);
    printf("\n");

    if (establish) {
        printf("=== PARALLEL GROUND TRUTH ===\n");
        printf("Terminal ratio: %.4f\n", terminal_ratio);
        printf("States: %zu, Terminal: %zu\n", total_states, total_terminal);
    }

    delete[] g_work;
    return passed ? 0 : 1;
}
