/*
 * enumerate_parallel.cpp — Parallel state space enumerator (v2 — optimized)
 *
 * Optimizations over v1:
 *   1. Huge pages (2MB) for hash table — eliminates TLB misses
 *   2. Software prefetching — hides DRAM latency on hash probes
 *   3. Work stealing — keeps all cores busy until the end
 *   4. Batched successor processing — prefetch all slots, then probe
 *
 * Usage: enumerate_parallel [--mem-gb N] [--threads T]
 *
 * Before running, enable huge pages:
 *   echo 65536 > /proc/sys/vm/nr_hugepages
 * (or however many 2MB pages you need: mem_gb * 512 + some margin)
 */

#include "bao.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>

#ifdef __linux__
#include <sys/mman.h>
#endif

// ---------------------------------------------------------------------------
// Lock-free hash set with huge pages + prefetch support
// ---------------------------------------------------------------------------

class AtomicHashSet {
public:
    // No power-of-2 constraint! Uses Fastrange for slot mapping.
    // This lets us use EVERY byte of available RAM.
    explicit AtomicHashSet(size_t capacity, bool use_hugepages = true) {
        capacity_ = capacity;
        count_.store(0, std::memory_order_relaxed);

        size_t bytes = capacity_ * sizeof(uint64_t);
        table_ = nullptr;

#ifdef __linux__
        if (use_hugepages) {
            void* ptr = mmap(nullptr, bytes,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                             -1, 0);
            if (ptr != MAP_FAILED) {
                table_ = (std::atomic<uint64_t>*)ptr;
                alloc_bytes_ = bytes;
                use_mmap_ = true;
                fprintf(stderr, "  Hash table: huge pages (%zu GB)\n",
                        bytes / (1024ULL*1024*1024));
            } else {
                fprintf(stderr, "  Huge pages unavailable, falling back to calloc\n");
            }
        }
#else
        (void)use_hugepages;
#endif

        if (!table_) {
            table_ = (std::atomic<uint64_t>*)calloc(capacity_, sizeof(uint64_t));
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

    // Fastrange: maps hash to [0, capacity) without modulo.
    // One 128-bit multiply + shift. Works for ANY capacity.
    inline size_t slot_for(uint64_t h) const {
        return (size_t)(((__uint128_t)h * capacity_) >> 64);
    }

    inline size_t next_slot(size_t slot) const {
        size_t s = slot + 1;
        return s < capacity_ ? s : 0;
    }

    // Insert: returns true if newly inserted. Lock-free via CAS.
    bool insert(uint64_t h) {
        if (h == 0) h = 1;
        size_t slot = slot_for(h);
        while (true) {
            uint64_t expected = 0;
            if (table_[slot].compare_exchange_strong(
                    expected, h,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                count_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            if (expected == h)
                return false;
            slot = next_slot(slot);
        }
    }

    // Prefetch the cache line for a future probe
    void prefetch(uint64_t h) const {
        if (h == 0) h = 1;
        __builtin_prefetch(&table_[slot_for(h)], 1, 0);
    }

    size_t count()    const { return count_.load(std::memory_order_relaxed); }
    size_t capacity() const { return capacity_; }
    double load()     const { return (double)count() / capacity_; }

private:
    std::atomic<uint64_t>* table_;
    size_t capacity_;
    std::atomic<size_t> count_;
    size_t alloc_bytes_ = 0;
    bool use_mmap_ = false;
};

// ---------------------------------------------------------------------------
// Canonicalize
// ---------------------------------------------------------------------------

static void canonicalize_state(BaoState& s) {
    bool do_reflect = false;
    for (int pos = 0; pos < TOTAL_PITS; ++pos) {
        int si = pos >> 4, i = pos & 15;
        uint8_t orig = s.pits[pos];
        uint8_t refl = s.pits[(si << 4) + REFLECT[i]];
        if (refl < orig) { do_reflect = true; break; }
        if (refl > orig) break;
    }
    if (do_reflect) s.reflect_lr();
    s.rehash();
}

// ---------------------------------------------------------------------------
// Per-thread stats (cache-line padded to avoid false sharing)
// ---------------------------------------------------------------------------

struct alignas(64) ThreadStats {
    size_t states       = 0;
    size_t terminal     = 0;
    size_t moves        = 0;
    size_t infinite     = 0;
    size_t inner_row_end = 0;
    size_t stack_peak   = 0;
    size_t steals       = 0;
    char _pad[8];
};

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

static std::atomic<size_t> g_states{0};
static std::atomic<size_t> g_terminal{0};
static std::atomic<bool>   g_table_full{false};
static std::atomic<bool>   g_done{false};
static time_t              g_start_time;

// Per-thread stacks + mutexes for work stealing
struct alignas(64) ThreadWork {
    std::vector<BaoState> stack;
    std::mutex mtx;           // only locked during steal operations
    std::atomic<bool> active{true};
};

static int g_num_threads;
static ThreadWork* g_work; // array of per-thread work

// ---------------------------------------------------------------------------
// Worker thread with prefetching + work stealing
// ---------------------------------------------------------------------------

static void worker(AtomicHashSet& visited, ThreadStats& stats, int tid) {
    stats = {};
    ThreadWork& my_work = g_work[tid];

    // Simple LCG for random victim selection during stealing
    uint32_t rng = (uint32_t)(tid * 2654435761u + 1);
    auto next_rng = [&]() -> int {
        rng = rng * 1103515245u + 12345u;
        return (int)((rng >> 16) % g_num_threads);
    };

    while (!g_table_full.load(std::memory_order_relaxed)) {
        // Get work from local stack
        BaoState state;
        {
            // Fast path: no lock needed if we're just popping our own stack
            if (my_work.stack.empty()) {
                // Try to steal from another thread
                bool stolen = false;
                for (int attempts = 0; attempts < g_num_threads * 2; ++attempts) {
                    int victim = next_rng();
                    if (victim == tid) continue;
                    if (!g_work[victim].active.load(std::memory_order_relaxed)) continue;

                    std::unique_lock<std::mutex> lock(g_work[victim].mtx, std::try_to_lock);
                    if (!lock.owns_lock()) continue;

                    size_t victim_size = g_work[victim].stack.size();
                    if (victim_size < 2) continue;

                    // Steal half
                    size_t steal_count = victim_size / 2;
                    auto begin = g_work[victim].stack.end() - steal_count;
                    auto end = g_work[victim].stack.end();

                    my_work.stack.insert(my_work.stack.end(), begin, end);
                    g_work[victim].stack.erase(begin, end);

                    stats.steals++;
                    stolen = true;
                    break;
                }

                if (!stolen) {
                    // Check if all threads are idle
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

                    // Brief backoff then retry
                    std::this_thread::yield();
                    my_work.active.store(true, std::memory_order_relaxed);
                    continue;
                }
            }

            state = my_work.stack.back();
            my_work.stack.pop_back();
        }

        if (my_work.stack.size() > stats.stack_peak)
            stats.stack_peak = my_work.stack.size();

        if (state.is_terminal()) {
            stats.terminal++;
            g_terminal.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // Generate all moves
        Move moves[MAX_MOVES];
        int n = state.generate_moves(moves);

        // --- Batched successor generation + prefetch ---
        // Phase 1: Generate successors, canonicalize, collect hashes
        struct Successor {
            BaoState state;
            uint64_t hash;
        };
        Successor succs[MAX_MOVES];
        int valid = 0;

        for (int i = 0; i < n; ++i) {
            stats.moves++;

            BaoState succ = state;
            MoveResult r = succ.make_move(moves[i]);

            if (r == MoveResult::INFINITE)        { stats.infinite++; continue; }
            if (r == MoveResult::INNER_ROW_EMPTY) { stats.inner_row_end++; continue; }

            canonicalize_state(succ);
            succs[valid].state = succ;
            succs[valid].hash = succ.hash;
            valid++;
        }

        // Phase 2: Prefetch all hash slots
        for (int i = 0; i < valid; ++i)
            visited.prefetch(succs[i].hash);

        // Phase 3: Probe (now cache-warm) and push new states
        for (int i = 0; i < valid; ++i) {
            if (!visited.insert(succs[i].hash))
                continue;

            stats.states++;
            size_t total = g_states.fetch_add(1, std::memory_order_relaxed) + 1;

            if ((total & 0xFFFFF) == 0) {
                if (visited.load() > 0.75) {
                    g_table_full.store(true, std::memory_order_relaxed);
                    return;
                }
            }

            if (succs[i].state.is_terminal()) {
                stats.terminal++;
                g_terminal.fetch_add(1, std::memory_order_relaxed);
            } else {
                my_work.stack.push_back(succs[i].state);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    size_t mem_gb = 100;
    int num_threads = (int)std::thread::hardware_concurrency();
    if (num_threads < 1) num_threads = 8;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mem-gb") == 0 && i + 1 < argc)
            mem_gb = (size_t)atol(argv[++i]);
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            num_threads = atoi(argv[++i]);
    }

    g_num_threads = num_threads;

    size_t table_bytes = mem_gb * (size_t)1024 * 1024 * 1024;
    size_t table_cap   = table_bytes / sizeof(uint64_t);
    size_t max_states  = (size_t)(table_cap * 0.70);

    printf("Bao la Kujifunza — Parallel State Enumerator v2\n");
    printf("================================================\n");
    printf("Hash table:  %zu GB (~%zu M max states)\n", mem_gb, max_states / 1000000);
    printf("Threads:     %d\n", num_threads);
    printf("Features:    huge pages, prefetch, work stealing\n");
    printf("\n");

    zobrist_init();

    fprintf(stderr, "Allocating hash table...\n");
    AtomicHashSet visited(table_cap, true);

    // --- Warmup ---
    size_t warmup_target = (size_t)num_threads * 10000;
    fprintf(stderr, "Warmup (target %zu stack entries)...\n", warmup_target);

    std::vector<BaoState> warmup_stack;
    warmup_stack.reserve(warmup_target * 2);

    BaoState start;
    start.init_start();
    canonicalize_state(start);
    visited.insert(start.hash);
    g_states.store(1, std::memory_order_relaxed);
    warmup_stack.push_back(start);

    size_t warmup_states = 1;
    while (warmup_stack.size() < warmup_target && !warmup_stack.empty()) {
        BaoState st = warmup_stack.back();
        warmup_stack.pop_back();

        if (st.is_terminal()) {
            g_terminal.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        Move moves[MAX_MOVES];
        int n = st.generate_moves(moves);
        for (int i = 0; i < n; ++i) {
            BaoState succ = st;
            MoveResult r = succ.make_move(moves[i]);
            if (r != MoveResult::OK) continue;
            canonicalize_state(succ);
            if (!visited.insert(succ.hash)) continue;
            warmup_states++;
            g_states.fetch_add(1, std::memory_order_relaxed);
            warmup_stack.push_back(succ);
        }
    }

    fprintf(stderr, "Warmup: %zu states, %zu stack entries\n\n",
            warmup_states, warmup_stack.size());

    // --- Split and launch ---
    g_work = new ThreadWork[num_threads];
    for (size_t i = 0; i < warmup_stack.size(); ++i)
        g_work[i % num_threads].stack.push_back(warmup_stack[i]);
    warmup_stack.clear();
    warmup_stack.shrink_to_fit();

    g_start_time = time(nullptr);

    std::vector<ThreadStats> thread_stats(num_threads);
    std::vector<std::thread> threads;

    // Reporter
    std::thread report_thread([&]() {
        while (!g_done.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            size_t s = g_states.load(std::memory_order_relaxed);
            size_t t = g_terminal.load(std::memory_order_relaxed);
            double elapsed = difftime(time(nullptr), g_start_time);
            double rate = elapsed > 0 ? s / elapsed : 0;
            fprintf(stderr,
                "\r  States: %12zu | Terminal: %10zu | Load: %.4f | "
                "%.1fM st/s | %.0fs     ",
                s, t, visited.load(), rate / 1e6, elapsed);
            fflush(stderr);
        }
    });

    // Workers
    for (int t = 0; t < num_threads; ++t)
        threads.emplace_back(worker, std::ref(visited),
                             std::ref(thread_stats[t]), t);

    for (auto& th : threads) th.join();
    g_done.store(true, std::memory_order_relaxed);
    report_thread.join();

    // --- Results ---
    size_t total_states   = g_states.load();
    size_t total_terminal = g_terminal.load();
    size_t total_moves = 0, total_infinite = 0, total_inner = 0;
    size_t peak_stack = 0, total_steals = 0;

    for (int t = 0; t < num_threads; ++t) {
        total_moves    += thread_stats[t].moves;
        total_infinite += thread_stats[t].infinite;
        total_inner    += thread_stats[t].inner_row_end;
        total_steals   += thread_stats[t].steals;
        if (thread_stats[t].stack_peak > peak_stack)
            peak_stack = thread_stats[t].stack_peak;
    }

    bool complete = !g_table_full.load();
    double elapsed = difftime(time(nullptr), g_start_time);

    fprintf(stderr, "\n\n%s\n", complete ? "COMPLETE." : "INCOMPLETE.");

    printf("\nResults\n-------\n");
    printf("Complete:          %s\n", complete ? "YES" : "NO");
    printf("Canonical states:  %zu\n", total_states);
    printf("Terminal states:   %zu\n", total_terminal);
    printf("Moves explored:    %zu\n", total_moves);
    printf("Infinite moves:    %zu\n", total_infinite);
    printf("Inner-row wins:    %zu\n", total_inner);
    printf("Work steals:       %zu\n", total_steals);
    printf("Peak stack/thread: %zu (%zu MB)\n",
           peak_stack, peak_stack * sizeof(BaoState) / (1024*1024));
    printf("Hash load:         %.4f\n", visited.load());
    printf("Elapsed:           %.1f sec\n", elapsed);
    if (elapsed > 0)
        printf("Rate:              %.1fM states/sec\n", total_states / elapsed / 1e6);

    delete[] g_work;
    return complete ? 0 : 1;
}
