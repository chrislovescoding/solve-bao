/*
 * enumerate_parallel.cpp — Parallel state space enumerator
 *
 * Lock-free atomic hash table + per-thread DFS stacks.
 * Single-threaded warmup, then splits work across N threads.
 *
 * Usage: enumerate_parallel [--mem-gb N] [--threads T]
 */

#include "bao.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <atomic>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Lock-free hash set (atomic CAS, linear probing)
// ---------------------------------------------------------------------------

class AtomicHashSet {
public:
    explicit AtomicHashSet(size_t capacity) {
        capacity_ = 1;
        while (capacity_ < capacity) capacity_ <<= 1;
        mask_ = capacity_ - 1;
        count_.store(0, std::memory_order_relaxed);

        // calloc gives us zero-initialized memory (0 = empty sentinel)
        table_ = (std::atomic<uint64_t>*)calloc(capacity_, sizeof(uint64_t));
        if (!table_) {
            fprintf(stderr, "FATAL: alloc failed (%zu GB)\n",
                    capacity_ * 8 / (1024ULL*1024*1024));
            exit(1);
        }
    }

    ~AtomicHashSet() { free(table_); }

    // Returns true if newly inserted, false if already present.
    // Thread-safe, lock-free.
    bool insert(uint64_t h) {
        if (h == 0) h = 1;
        size_t slot = h & mask_;
        while (true) {
            uint64_t expected = 0;
            // Try to claim empty slot
            if (table_[slot].compare_exchange_strong(
                    expected, h,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                count_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            // Slot was occupied — check if it's our value
            if (expected == h)
                return false;
            // Collision — probe next slot
            slot = (slot + 1) & mask_;
        }
    }

    size_t count()    const { return count_.load(std::memory_order_relaxed); }
    size_t capacity() const { return capacity_; }
    double load()     const { return (double)count() / capacity_; }

private:
    std::atomic<uint64_t>* table_;
    size_t capacity_, mask_;
    std::atomic<size_t> count_;
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
// Per-thread stats
// ---------------------------------------------------------------------------

struct ThreadStats {
    size_t states;
    size_t terminal;
    size_t moves;
    size_t infinite;
    size_t inner_row_end;
    size_t stack_peak;
};

// ---------------------------------------------------------------------------
// Global atomic stats for progress reporting
// ---------------------------------------------------------------------------

static std::atomic<size_t> g_states{0};
static std::atomic<size_t> g_terminal{0};
static std::atomic<bool>   g_table_full{false};
static time_t              g_start_time;

// ---------------------------------------------------------------------------
// Worker thread: DFS on local stack, shared hash table
// ---------------------------------------------------------------------------

static void worker(AtomicHashSet& visited,
                   std::vector<BaoState> initial_stack,
                   ThreadStats& stats,
                   int thread_id) {
    (void)thread_id;
    stats = {};

    std::vector<BaoState> stack = std::move(initial_stack);

    while (!stack.empty() && !g_table_full.load(std::memory_order_relaxed)) {
        BaoState state = stack.back();
        stack.pop_back();

        if (stack.size() > stats.stack_peak)
            stats.stack_peak = stack.size();

        if (state.is_terminal()) {
            stats.terminal++;
            g_terminal.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        Move moves[MAX_MOVES];
        int n = state.generate_moves(moves);

        for (int i = 0; i < n; ++i) {
            stats.moves++;

            BaoState succ = state;
            MoveResult r = succ.make_move(moves[i]);

            if (r == MoveResult::INFINITE)        { stats.infinite++; continue; }
            if (r == MoveResult::INNER_ROW_EMPTY) { stats.inner_row_end++; continue; }

            canonicalize_state(succ);

            if (!visited.insert(succ.hash))
                continue;

            stats.states++;
            size_t total = g_states.fetch_add(1, std::memory_order_relaxed) + 1;

            // Check load factor periodically
            if ((total & 0xFFFFF) == 0) { // every ~1M states
                if (visited.load() > 0.75) {
                    g_table_full.store(true, std::memory_order_relaxed);
                    fprintf(stderr, "\nTABLE FULL at %zu states\n", visited.count());
                    return;
                }
            }

            stack.push_back(succ);
        }
    }
}

// ---------------------------------------------------------------------------
// Progress reporter thread
// ---------------------------------------------------------------------------

static void reporter(AtomicHashSet& visited) {
    while (!g_table_full.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        size_t s = g_states.load(std::memory_order_relaxed);
        size_t t = g_terminal.load(std::memory_order_relaxed);
        double elapsed = difftime(time(nullptr), g_start_time);
        double rate = elapsed > 0 ? s / elapsed : 0;
        fprintf(stderr,
            "\r  States: %12zu | Terminal: %10zu | Load: %.4f | "
            "%.0f st/s | %.0fs     ",
            s, t, visited.load(), rate, elapsed);
        fflush(stderr);
        if (s == 0) continue;
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    size_t mem_gb = 200;
    int num_threads = (int)std::thread::hardware_concurrency();
    if (num_threads < 1) num_threads = 8;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mem-gb") == 0 && i + 1 < argc)
            mem_gb = (size_t)atol(argv[++i]);
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            num_threads = atoi(argv[++i]);
    }

    size_t table_bytes = mem_gb * (size_t)1024 * 1024 * 1024;
    size_t table_cap   = table_bytes / sizeof(uint64_t);
    size_t max_states  = (size_t)(table_cap * 0.70);

    printf("Bao la Kujifunza — Parallel State Enumerator\n");
    printf("=============================================\n");
    printf("Hash table:  %zu GB (%zu M entries, ~%zu M max)\n",
           mem_gb, table_cap / 1000000, max_states / 1000000);
    printf("Threads:     %d\n", num_threads);
    printf("\n");

    zobrist_init();
    AtomicHashSet visited(table_cap);

    // --- Phase 1: single-threaded warmup ---
    // Build up the stack until we have enough work to split.
    size_t warmup_target = (size_t)num_threads * 10000;

    fprintf(stderr, "Phase 1: warmup (building %zu stack entries)...\n", warmup_target);

    std::vector<BaoState> stack;
    stack.reserve(warmup_target * 2);

    BaoState start;
    start.init_start();
    canonicalize_state(start);
    visited.insert(start.hash);
    g_states.store(1, std::memory_order_relaxed);
    stack.push_back(start);

    size_t warmup_states = 1;

    while (stack.size() < warmup_target && !stack.empty()) {
        BaoState state = stack.back();
        stack.pop_back();

        if (state.is_terminal()) {
            g_terminal.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        Move moves[MAX_MOVES];
        int n = state.generate_moves(moves);

        for (int i = 0; i < n; ++i) {
            BaoState succ = state;
            MoveResult r = succ.make_move(moves[i]);
            if (r != MoveResult::OK) continue;

            canonicalize_state(succ);
            if (!visited.insert(succ.hash)) continue;

            warmup_states++;
            g_states.fetch_add(1, std::memory_order_relaxed);
            stack.push_back(succ);
        }
    }

    fprintf(stderr, "Warmup done: %zu states, %zu stack entries\n",
            warmup_states, stack.size());

    // --- Phase 2: split stack across threads ---
    fprintf(stderr, "Phase 2: parallel DFS with %d threads...\n", num_threads);
    g_start_time = time(nullptr);

    std::vector<std::vector<BaoState>> thread_stacks(num_threads);
    for (size_t i = 0; i < stack.size(); ++i)
        thread_stacks[i % num_threads].push_back(stack[i]);
    stack.clear();
    stack.shrink_to_fit();

    std::vector<ThreadStats> thread_stats(num_threads);
    std::vector<std::thread> threads;

    // Start reporter
    std::atomic<bool> reporter_done{false};
    std::thread report_thread([&]() {
        while (!reporter_done.load(std::memory_order_relaxed)) {
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

    // Launch workers
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back(worker, std::ref(visited),
                             std::move(thread_stacks[t]),
                             std::ref(thread_stats[t]), t);
    }

    for (auto& th : threads)
        th.join();

    reporter_done.store(true, std::memory_order_relaxed);
    report_thread.join();

    // --- Aggregate stats ---
    size_t total_states    = g_states.load();
    size_t total_terminal  = g_terminal.load();
    size_t total_moves     = 0;
    size_t total_infinite  = 0;
    size_t total_inner     = 0;
    size_t peak_stack      = 0;

    for (int t = 0; t < num_threads; ++t) {
        total_moves    += thread_stats[t].moves;
        total_infinite += thread_stats[t].infinite;
        total_inner    += thread_stats[t].inner_row_end;
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
    printf("Peak stack/thread: %zu (%zu MB)\n",
           peak_stack, peak_stack * sizeof(BaoState) / (1024*1024));
    printf("Hash load:         %.4f\n", visited.load());
    printf("Elapsed:           %.1f sec\n", elapsed);
    if (elapsed > 0)
        printf("Rate:              %.1fM states/sec\n", total_states / elapsed / 1e6);

    return complete ? 0 : 1;
}
