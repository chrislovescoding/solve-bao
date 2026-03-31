/*
 * enumerate_parallel.cpp — Production parallel enumerator
 *
 * Thin wrapper around enumerate_core.h. All shared logic lives there.
 */

#include "enumerate_core.h"
#include <ctime>

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

    size_t table_bytes = mem_gb * (size_t)1024 * 1024 * 1024;
    size_t table_cap   = table_bytes / sizeof(uint32_t);
    size_t max_states  = (size_t)(table_cap * 0.70);

    printf("Bao la Kujifunza — Parallel State Enumerator\n");
    printf("=============================================\n");
    printf("Hash table:  %zu GB, 4 bytes/slot, %zuB slots, ~%zuB max states\n",
           mem_gb, table_cap / 1000000000, max_states / 1000000000);
    printf("Threads:     %d\n", num_threads);
    printf("Stack entry: %zu bytes\n", sizeof(CompactState));
    printf("\n");

    zobrist_init();

    fprintf(stderr, "Allocating hash table...\n");
    AtomicHashSet visited(table_cap, true);

    EnumGlobals g;
    g.num_threads = num_threads;
    g.max_states = 0; // unlimited

    size_t warmup_target = (size_t)num_threads * 10000;
    fprintf(stderr, "Warmup (target %zu stack entries)...\n", warmup_target);
    size_t warmup_states = enum_warmup(visited, g, warmup_target);
    fprintf(stderr, "Warmup: %zu states\n\n", warmup_states);

    time_t t0 = time(nullptr);

    std::vector<ThreadStats> thread_stats(num_threads);
    std::vector<std::thread> threads;

    // Reporter + drain detector
    std::thread report_thread([&]() {
        size_t prev_states = 0;
        int stall_count = 0;
        while (!g.done.load(std::memory_order_relaxed) &&
               !g.table_full.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            size_t s = g.states.load(std::memory_order_relaxed);
            size_t t = g.terminal.load(std::memory_order_relaxed);
            double elapsed = difftime(time(nullptr), t0);
            double rate = elapsed > 0 ? s / elapsed : 0;
            size_t new_states = s - prev_states;
            bool is_draining = g.draining.load(std::memory_order_relaxed);

            // Sample stack sizes (just reads, no writes to worker data)
            size_t total_stack = 0;
            int active_threads = 0;
            for (int i = 0; i < g.num_threads; ++i) {
                size_t sz = g.work[i].stack.size();
                total_stack += sz;
                if (sz > 0) active_threads++;
            }

            fprintf(stderr,
                "\r  States: %12zu | +%zu/2s | Stack: %zuM (%d thr) | "
                "%.1fM st/s%s | %.0fs     ",
                s, new_states, total_stack / 1000000, active_threads,
                rate / 1e6, is_draining ? " [DRAIN]" : "", elapsed);
            fflush(stderr);

            // Detect stall: if fewer than 1000 new states in 2 seconds,
            // for 3 consecutive checks (6 seconds total), enable drain mode.
            // This stops work stealing so threads can drain their stacks.
            if (new_states < 500000 && s > 1000000000) {
                stall_count++;
                if (stall_count >= 3 && !is_draining) {
                    g.draining.store(true, std::memory_order_relaxed);
                    fprintf(stderr, "\n  >>> Discovery stalled. Enabling drain mode. <<<\n");
                }
            } else {
                stall_count = 0;
            }

            prev_states = s;
        }
    });

    for (int t = 0; t < num_threads; ++t)
        threads.emplace_back(enum_worker, std::ref(visited),
                             std::ref(thread_stats[t]), t, std::ref(g));

    for (auto& th : threads) th.join();
    g.done.store(true, std::memory_order_relaxed);
    report_thread.join();

    // Aggregate from thread-local stats (exact)
    size_t total_states = warmup_states;
    size_t total_terminal = 0, total_moves = 0;
    size_t total_infinite = 0, total_inner = 0;
    size_t peak_stack = 0, total_steals = 0;

    for (int t = 0; t < num_threads; ++t) {
        total_states   += thread_stats[t].states;
        total_terminal += thread_stats[t].terminal;
        total_moves    += thread_stats[t].moves;
        total_infinite += thread_stats[t].infinite;
        total_inner    += thread_stats[t].inner_row_end;
        total_steals   += thread_stats[t].steals;
        if (thread_stats[t].stack_peak > peak_stack)
            peak_stack = thread_stats[t].stack_peak;
    }

    bool complete = !g.table_full.load();
    double elapsed = difftime(time(nullptr), t0);

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
           peak_stack, peak_stack * sizeof(CompactState) / (1024*1024));
    printf("Hash load:         %.4f\n", (double)total_states / visited.capacity());
    printf("Elapsed:           %.1f sec\n", elapsed);
    if (elapsed > 0)
        printf("Rate:              %.1fM states/sec\n", total_states / elapsed / 1e6);

    delete[] g.work;
    return complete ? 0 : 1;
}
