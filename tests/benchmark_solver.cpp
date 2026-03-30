/*
 * benchmark_solver.cpp — Solver benchmark with ground truth verification
 *
 * 1. Enumerates a small state space, stores all states in a vector
 * 2. Runs iterative resolution by scanning the vector (no re-enumeration)
 * 3. Runs parallel solver on same states
 * 4. Verifies exact match
 *
 * Usage: benchmark_solver [--states N] [--threads T]
 */

#include "../src/solver_core.h"
#include <chrono>

using Clock = std::chrono::high_resolution_clock;

// ---------------------------------------------------------------------------
// Ground truth: enumerate once, then resolve by scanning stored states
// ---------------------------------------------------------------------------

struct GroundTruth {
    size_t total, wins, losses, draws;
    int passes;
    uint32_t start_label;
};

static GroundTruth compute_ground_truth(size_t max_states) {
    size_t table_cap = max_states * 4;
    LabelHashTable table(table_cap, false);

    // Step 1: Enumerate ALL states into a vector (once)
    std::vector<CompactState> all_states;
    std::vector<uint64_t> all_hashes;
    all_states.reserve(max_states);
    all_hashes.reserve(max_states);

    std::vector<CompactState> stack;
    BaoState start;
    start.init_start();
    uint64_t sh = canonical_hash(start);
    bool is_term = start.is_terminal();
    table.insert(sh, is_term ? LABEL_LOSS : LABEL_UNKNOWN);
    { CompactState cs; cs.from_bao(start); all_states.push_back(cs); }
    all_hashes.push_back(sh);
    if (!is_term) {
        CompactState cs; cs.from_bao(start);
        stack.push_back(cs);
    }

    while (!stack.empty() && all_states.size() < max_states) {
        BaoState st;
        stack.back().to_bao(st);
        stack.pop_back();

        Move moves[MAX_MOVES];
        int n = st.generate_moves(moves);
        for (int i = 0; i < n && all_states.size() < max_states; ++i) {
            BaoState succ = st;
            if (succ.make_move(moves[i]) != MoveResult::OK) continue;
            uint64_t h = canonical_hash(succ);
            bool term = succ.is_terminal();
            if (!table.insert(h, term ? LABEL_LOSS : LABEL_UNKNOWN)) continue;
            CompactState cs; cs.from_bao(succ);
            all_states.push_back(cs);
            all_hashes.push_back(h);
            if (!term) stack.push_back(cs);
        }
    }

    size_t count = all_states.size();

    // Step 2: Iterative resolution — scan the stored vector each pass
    // NO re-enumeration, no visited sets, no DFS. Just a flat scan.
    bool changed = true;
    int passes = 0;
    while (changed) {
        changed = false;
        passes++;

        for (size_t idx = 0; idx < count; ++idx) {
            uint32_t my_label = table.lookup(all_hashes[idx]);
            if (my_label != LABEL_UNKNOWN) continue;

            BaoState state;
            all_states[idx].to_bao(state);
            if (state.is_terminal()) continue; // should already be LOSS

            Move moves[MAX_MOVES];
            int n = state.generate_moves(moves);

            bool found_loss = false;
            bool all_win = true;

            for (int i = 0; i < n; ++i) {
                BaoState succ = state;
                MoveResult r = succ.make_move(moves[i]);
                if (r == MoveResult::INNER_ROW_EMPTY) {
                    found_loss = true;
                    continue;
                }
                if (r != MoveResult::OK) continue;

                uint32_t sl = table.lookup(canonical_hash(succ));
                if (sl == LABEL_LOSS) found_loss = true;
                if (sl != LABEL_WIN) all_win = false;
            }

            if (found_loss) {
                table.update_label(all_hashes[idx], LABEL_WIN);
                changed = true;
            } else if (all_win) {
                table.update_label(all_hashes[idx], LABEL_LOSS);
                changed = true;
            }
        }
    }

    // Step 3: Count results
    GroundTruth gt = {};
    gt.total = count;
    gt.passes = passes;
    gt.start_label = table.lookup(all_hashes[0]);

    for (size_t i = 0; i < count; ++i) {
        uint32_t l = table.lookup(all_hashes[i]);
        if (l == LABEL_WIN) gt.wins++;
        else if (l == LABEL_LOSS) gt.losses++;
        else gt.draws++;
    }

    return gt;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    size_t max_states = 10000;
    int num_threads = (int)std::thread::hardware_concurrency();
    if (num_threads < 1) num_threads = 4;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--states") == 0 && i + 1 < argc)
            max_states = (size_t)atol(argv[++i]);
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            num_threads = atoi(argv[++i]);
    }

    zobrist_init();

    printf("Bao Solver Benchmark — %zu states, %d threads\n", max_states, num_threads);
    printf("================================================\n\n");

    // Step 1: Ground truth
    printf("--- Computing ground truth ---\n");
    auto t0 = Clock::now();
    GroundTruth gt = compute_ground_truth(max_states);
    auto t1 = Clock::now();
    double gt_sec = std::chrono::duration<double>(t1 - t0).count();

    printf("Ground truth (%zu states, %d passes, %.2f sec):\n",
           gt.total, gt.passes, gt_sec);
    printf("  WIN:   %zu (%.1f%%)\n", gt.wins, 100.0 * gt.wins / gt.total);
    printf("  LOSS:  %zu (%.1f%%)\n", gt.losses, 100.0 * gt.losses / gt.total);
    printf("  DRAW:  %zu (%.4f%%)\n", gt.draws, 100.0 * gt.draws / gt.total);
    printf("  Start: %s\n\n",
           gt.start_label == LABEL_WIN ? "WIN" :
           gt.start_label == LABEL_LOSS ? "LOSS" : "DRAW/UNKNOWN");

    // Step 2: Parallel solver
    printf("--- Running parallel solver ---\n");
    size_t table_cap = max_states * 4;
    LabelHashTable table(table_cap, false);

    SolverGlobals g;
    g.num_threads = num_threads;

    size_t warmup_target = (size_t)num_threads * 500;
    size_t warmup_count = solver_warmup_init(table, g, warmup_target);

    g.done.store(false, std::memory_order_relaxed);
    g.states_scanned.store(warmup_count, std::memory_order_relaxed);

    std::vector<SolverThreadStats> tstats(num_threads);
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t)
        threads.emplace_back(init_worker, std::ref(table),
                             std::ref(tstats[t]), t, std::ref(g));
    for (auto& th : threads) th.join();
    threads.clear();

    size_t total_states = warmup_count;
    size_t total_terminal = 0;
    for (int t = 0; t < num_threads; ++t) {
        total_states += tstats[t].states_scanned;
        total_terminal += tstats[t].terminal;
    }
    printf("  Init: %zu states (%zu terminal)\n", total_states, total_terminal);

    // Resolution passes
    auto solve_start = Clock::now();
    int pass = 0;
    size_t total_win = 0, total_loss = total_terminal;

    while (true) {
        pass++;
        g.states_scanned.store(0, std::memory_order_relaxed);
        g.resolved_this_pass.store(0, std::memory_order_relaxed);
        g.done.store(false, std::memory_order_relaxed);
        g.pass_number = pass;

        solver_warmup_resolve(table, g, warmup_target);

        for (int t = 0; t < num_threads; ++t) {
            tstats[t] = {};
            threads.emplace_back(resolve_worker, std::ref(table),
                                 std::ref(tstats[t]), t, std::ref(g));
        }
        for (auto& th : threads) th.join();
        threads.clear();

        size_t resolved = g.resolved_this_pass.load();
        size_t pass_win = 0, pass_loss = 0;
        for (int t = 0; t < num_threads; ++t) {
            pass_win += tstats[t].resolved_win;
            pass_loss += tstats[t].resolved_loss;
        }
        total_win += pass_win;
        total_loss += pass_loss;

        printf("  Pass %d: +%zu WIN, +%zu LOSS\n", pass, pass_win, pass_loss);
        if (resolved == 0) break;
        if (pass > 500) break;
    }

    auto solve_end = Clock::now();
    double solve_sec = std::chrono::duration<double>(solve_end - solve_start).count();
    size_t total_draw = total_states - total_win - total_loss;

    BaoState start;
    start.init_start();
    uint32_t start_label = table.lookup(canonical_hash(start));

    printf("\nSolver result (%d passes, %.2f sec):\n", pass, solve_sec);
    printf("  WIN:   %zu\n", total_win);
    printf("  LOSS:  %zu\n", total_loss);
    printf("  DRAW:  %zu\n", total_draw);
    printf("  Start: %s\n",
           start_label == LABEL_WIN ? "WIN" :
           start_label == LABEL_LOSS ? "LOSS" : "DRAW/UNKNOWN");

    // Verify
    printf("\n--- Verification ---\n");
    int failures = 0;
    if (total_win != gt.wins) { printf("FAIL: WIN %zu != %zu\n", total_win, gt.wins); failures++; }
    if (total_loss != gt.losses) { printf("FAIL: LOSS %zu != %zu\n", total_loss, gt.losses); failures++; }
    if (total_draw != gt.draws) { printf("FAIL: DRAW %zu != %zu\n", total_draw, gt.draws); failures++; }
    if (start_label != gt.start_label) { printf("FAIL: start mismatch\n"); failures++; }

    bool passed = (failures == 0);
    printf("Correctness:        %s\n", passed ? "PASS" : "FAIL");
    printf("\n=== METRICS ===\n");
    printf("Resolve throughput: %.0f states*passes/sec\n",
           (double)total_states * pass / solve_sec);
    printf("Passes:             %d\n", pass);
    printf("Time (ground truth): %.2f sec\n", gt_sec);
    printf("Time (solver):       %.2f sec\n", solve_sec);

    delete[] g.work;
    return passed ? 0 : 1;
}
