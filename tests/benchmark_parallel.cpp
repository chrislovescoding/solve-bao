/*
 * benchmark_parallel.cpp — Parallel benchmark using EXACT production code
 *
 * Imports enumerate_core.h directly — same hash table, same CompactState,
 * same worker, same work stealing. Any optimization to the production code
 * is automatically tested here. No code duplication.
 *
 * Reports: throughput, memory per state, stack entry size, and a composite
 * production score that penalizes both slow speed AND high memory usage.
 *
 * Usage: benchmark_parallel [--states N] [--threads T]
 */

#include "../src/enumerate_core.h"
#include <chrono>

using Clock = std::chrono::high_resolution_clock;

int main(int argc, char* argv[]) {
    size_t max_states = 5000000;
    int num_threads = (int)std::thread::hardware_concurrency();
    if (num_threads < 1) num_threads = 4;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--states") == 0 && i + 1 < argc)
            max_states = (size_t)atol(argv[++i]);
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            num_threads = atoi(argv[++i]);
    }

    zobrist_init();

    // Hash table: 4x states for comfortable load factor
    size_t table_cap = max_states * 4;
    AtomicHashSet visited(table_cap, false); // no huge pages for benchmark

    printf("Bao Parallel Benchmark — %zu states, %d threads\n", max_states, num_threads);
    printf("================================================\n\n");

    EnumGlobals g;
    g.num_threads = num_threads;
    g.max_states = max_states; // stop after this many

    size_t warmup_target = (size_t)num_threads * 5000;
    size_t warmup_states = enum_warmup(visited, g, warmup_target);

    auto t0 = Clock::now();

    std::vector<ThreadStats> thread_stats(num_threads);
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t)
        threads.emplace_back(enum_worker, std::ref(visited),
                             std::ref(thread_stats[t]), t, std::ref(g));

    for (auto& th : threads) th.join();

    auto t1 = Clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    // Aggregate
    size_t total_states = warmup_states;
    size_t total_terminal = 0, total_moves = 0;
    size_t peak_stack = 0, total_steals = 0;

    for (int t = 0; t < num_threads; ++t) {
        total_states   += thread_stats[t].states;
        total_terminal += thread_stats[t].terminal;
        total_moves    += thread_stats[t].moves;
        total_steals   += thread_stats[t].steals;
        if (thread_stats[t].stack_peak > peak_stack)
            peak_stack = thread_stats[t].stack_peak;
    }

    // Validate
    int failures = 0;
    double terminal_ratio = (double)total_terminal / total_states;
    if (terminal_ratio < 0.35 || terminal_ratio > 0.60) {
        fprintf(stderr, "FAIL: terminal ratio %.3f out of range [0.35, 0.60]\n",
                terminal_ratio);
        failures++;
    }

    bool passed = (failures == 0);
    double throughput = total_states / elapsed;

    // --- Memory projection for 50B states on 512 GB machine ---
    size_t stack_entry_bytes = sizeof(CompactState);
    size_t hash_entry_bytes = sizeof(uint32_t);

    double target_states = 50e9;
    double hash_table_gb = (target_states / 0.7) * hash_entry_bytes / (1024.0*1024*1024);
    double stack_fraction = (peak_stack > 0 && total_states > 0)
        ? (double)(peak_stack * num_threads) / total_states
        : 0.05;
    double stack_gb = target_states * stack_fraction * stack_entry_bytes / (1024.0*1024*1024);
    double total_projected_gb = hash_table_gb + stack_gb + 4;
    double machine_gb = 512.0;
    bool fits = total_projected_gb <= machine_gb;
    double ram_fitness = fits ? 1.0 : machine_gb / total_projected_gb;
    double production_score = throughput * ram_fitness;

    printf("Correctness:        %s\n", passed ? "PASS" : "FAIL");
    printf("States found:       %zu\n", total_states);
    printf("Terminal:            %zu (%.1f%%)\n", total_terminal, 100.0 * terminal_ratio);
    printf("Moves explored:     %zu\n", total_moves);
    printf("Work steals:        %zu\n", total_steals);
    printf("Threads:            %d\n", num_threads);
    printf("Elapsed:            %.3f sec\n", elapsed);
    printf("\n");

    printf("=== THROUGHPUT (maximize) ===\n");
    printf("Throughput:         %.1fM states/sec\n", throughput / 1e6);
    printf("Per-thread:         %.1fK states/sec/thread\n", throughput / num_threads / 1e3);
    printf("\n");

    printf("=== MEMORY (minimize) ===\n");
    printf("Stack entry:        %zu bytes\n", stack_entry_bytes);
    printf("Hash entry:         %zu bytes\n", hash_entry_bytes);
    printf("Peak stack/thread:  %zu entries (%.1f MB)\n",
           peak_stack, peak_stack * (double)stack_entry_bytes / (1024*1024));
    printf("Hash table:         %.1f bytes/state\n",
           (double)visited.memory_bytes() / total_states);
    printf("\n");

    printf("=== PRODUCTION PROJECTION (50B states, 512 GB) ===\n");
    printf("Hash table:         %.0f GB\n", hash_table_gb);
    printf("Stack (est):        %.0f GB (%.1f%% ratio, %zu B/entry)\n",
           stack_gb, stack_fraction * 100, stack_entry_bytes);
    printf("Total projected:    %.0f GB  %s\n", total_projected_gb,
           fits ? "[FITS]" : "[DOES NOT FIT]");
    printf("\n");

    printf("=== COMPOSITE SCORE (maximize) ===\n");
    printf("Production score:   %.1fM\n", production_score / 1e6);
    printf("  = throughput %.1fM x RAM fitness %.2f\n",
           throughput / 1e6, ram_fitness);
    printf("\n");

    delete[] g.work;
    return passed ? 0 : 1;
}
