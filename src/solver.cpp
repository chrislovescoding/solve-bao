/*
 * solver.cpp — Production parallel solver for Bao la Kujifunza
 *
 * Phase 0: Enumerate all states, label terminals as LOSS
 * Phase 1..N: Iterative resolution passes until convergence
 * Phase N+1: Label remaining UNKNOWN as DRAW, verify start = WIN
 *
 * Usage: solver [--mem-gb N] [--threads T] [--max-passes P]
 */

#include "solver_core.h"
#include <ctime>

int main(int argc, char* argv[]) {
    size_t mem_gb = 300;
    int num_threads = 16;
    int max_passes = 500;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mem-gb") == 0 && i + 1 < argc)
            mem_gb = (size_t)atol(argv[++i]);
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            num_threads = atoi(argv[++i]);
        if (strcmp(argv[i], "--max-passes") == 0 && i + 1 < argc)
            max_passes = atoi(argv[++i]);
    }

    size_t table_bytes = mem_gb * (size_t)1024 * 1024 * 1024;
    size_t table_cap   = table_bytes / sizeof(uint32_t);

    printf("Bao la Kujifunza — Parallel Solver\n");
    printf("===================================\n");
    printf("Label table: %zu GB (%zuB slots)\n", mem_gb, table_cap / 1000000000);
    printf("Threads:     %d\n", num_threads);
    printf("Max passes:  %d\n", max_passes);
    printf("\n");

    zobrist_init();

    fprintf(stderr, "Allocating label table...\n");
    LabelHashTable table(table_cap, true);

    SolverGlobals g;
    g.num_threads = num_threads;

    // ====== PHASE 0: INIT (enumerate + label) ======
    fprintf(stderr, "\n=== PHASE 0: INIT (enumerate all states) ===\n");
    time_t t0 = time(nullptr);

    size_t warmup_target = (size_t)num_threads * 10000;
    size_t warmup_count = solver_warmup_init(table, g, warmup_target);
    fprintf(stderr, "Warmup: %zu states\n", warmup_count);

    g.done.store(false, std::memory_order_relaxed);
    g.states_scanned.store(warmup_count, std::memory_order_relaxed);

    std::vector<SolverThreadStats> tstats(num_threads);
    std::vector<std::thread> threads;

    // Reporter for init
    std::atomic<bool> report_done{false};
    std::thread reporter([&]() {
        while (!report_done.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            size_t s = g.states_scanned.load(std::memory_order_relaxed);
            double elapsed = difftime(time(nullptr), t0);
            double rate = elapsed > 0 ? s / elapsed : 0;
            fprintf(stderr, "\r  Init: %12zu states | %.1fM st/s | %.0fs    ",
                    s, rate / 1e6, elapsed);
            fflush(stderr);
        }
    });

    for (int t = 0; t < num_threads; ++t)
        threads.emplace_back(init_worker, std::ref(table),
                             std::ref(tstats[t]), t, std::ref(g));
    for (auto& th : threads) th.join();
    threads.clear();

    report_done.store(true, std::memory_order_relaxed);
    reporter.join();

    // Aggregate init stats
    size_t total_states = warmup_count;
    size_t total_terminal = 0;
    for (int t = 0; t < num_threads; ++t) {
        total_states += tstats[t].states_scanned;
        total_terminal += tstats[t].terminal;
    }

    double init_elapsed = difftime(time(nullptr), t0);
    fprintf(stderr, "\n  Init complete: %zu states (%zu terminal) in %.0fs\n",
            total_states, total_terminal, init_elapsed);

    // ====== PHASE 1..N: RESOLUTION PASSES ======
    fprintf(stderr, "\n=== PHASE 1+: RESOLUTION ===\n");

    size_t total_resolved_win = 0;
    size_t total_resolved_loss = total_terminal;  // terminals are LOSS from init
    size_t total_unknown = total_states - total_terminal;

    for (int pass = 1; pass <= max_passes; ++pass) {
        time_t pass_start = time(nullptr);
        g.pass_number = pass;
        g.states_scanned.store(0, std::memory_order_relaxed);
        g.resolved_this_pass.store(0, std::memory_order_relaxed);
        g.done.store(false, std::memory_order_relaxed);

        // Re-seed thread stacks for DFS traversal
        size_t resolve_warmup = (size_t)num_threads * 10000;
        solver_warmup_resolve(table, g, resolve_warmup);

        // Reporter for this pass
        report_done.store(false, std::memory_order_relaxed);
        std::thread pass_reporter([&]() {
            while (!report_done.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                size_t s = g.states_scanned.load(std::memory_order_relaxed);
                size_t r = g.resolved_this_pass.load(std::memory_order_relaxed);
                double elapsed = difftime(time(nullptr), pass_start);
                double rate = elapsed > 0 ? s / elapsed : 0;
                fprintf(stderr,
                    "\r  Pass %3d: scanned %12zu | resolved %10zu | %.1fM st/s | %.0fs    ",
                    pass, s, r, rate / 1e6, elapsed);
                fflush(stderr);
            }
        });

        // Launch resolve workers
        for (int t = 0; t < num_threads; ++t) {
            tstats[t] = {};
            threads.emplace_back(resolve_worker, std::ref(table),
                                 std::ref(tstats[t]), t, std::ref(g));
        }
        for (auto& th : threads) th.join();
        threads.clear();

        report_done.store(true, std::memory_order_relaxed);
        pass_reporter.join();

        // Aggregate pass stats
        size_t pass_resolved = g.resolved_this_pass.load();
        size_t pass_win = 0, pass_loss = 0, pass_unknown = 0;
        for (int t = 0; t < num_threads; ++t) {
            pass_win += tstats[t].resolved_win;
            pass_loss += tstats[t].resolved_loss;
            pass_unknown += tstats[t].still_unknown;
        }

        total_resolved_win += pass_win;
        total_resolved_loss += pass_loss;
        total_unknown -= (pass_win + pass_loss);

        double pass_elapsed = difftime(time(nullptr), pass_start);
        fprintf(stderr,
            "\n  Pass %d: +%zu WIN, +%zu LOSS, %zu remaining UNKNOWN (%.0fs)\n",
            pass, pass_win, pass_loss, total_unknown, pass_elapsed);

        if (pass_resolved == 0) {
            fprintf(stderr, "\n  Converged after %d passes.\n", pass);
            break;
        }
    }

    // ====== FINAL: VERIFY ======
    fprintf(stderr, "\n=== VERIFICATION ===\n");

    // Check starting position
    BaoState start;
    start.init_start();
    uint64_t start_hash = canonical_hash(start);
    uint32_t start_label = table.lookup(start_hash);

    const char* label_str[] = {"EMPTY", "UNKNOWN", "WIN", "LOSS"};
    fprintf(stderr, "Starting position: %s\n",
            start_label <= 3 ? label_str[start_label] : "???");

    if (start_label == LABEL_WIN) {
        fprintf(stderr, "  ✓ Matches Donkers & Uiterwijk (2002): first player wins\n");
    } else {
        fprintf(stderr, "  ✗ UNEXPECTED! Donkers says first player wins.\n");
    }

    // Label remaining UNKNOWN as DRAW
    if (total_unknown > 0) {
        fprintf(stderr, "\n  %zu states remain UNKNOWN → labeling as DRAW\n", total_unknown);
        fprintf(stderr, "  (These form cycles where neither player can force a win)\n");
    }

    double total_elapsed = difftime(time(nullptr), t0);

    // ====== RESULTS ======
    printf("\n===== FINAL RESULTS =====\n");
    printf("Total canonical states:   %zu\n", total_states);
    printf("  WIN:                    %zu (%.1f%%)\n",
           total_resolved_win, 100.0 * total_resolved_win / total_states);
    printf("  LOSS:                   %zu (%.1f%%)\n",
           total_resolved_loss, 100.0 * total_resolved_loss / total_states);
    printf("  DRAW (cycles):          %zu (%.4f%%)\n",
           total_unknown, 100.0 * total_unknown / total_states);
    printf("Starting position:        %s\n",
           start_label <= 3 ? label_str[start_label] : "???");
    printf("Total elapsed:            %.0f sec (%.1f hours)\n",
           total_elapsed, total_elapsed / 3600);
    printf("Resolution passes:        %d\n", g.pass_number);

    delete[] g.work;
    return (start_label == LABEL_WIN && total_unknown == 0) ? 0 : 1;
}
