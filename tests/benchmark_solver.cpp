/*
 * benchmark_solver.cpp — Solver benchmark with minimax ground truth
 *
 * 1. Runs minimax on a small state space to establish exact WIN/LOSS counts
 * 2. Runs the iterative parallel solver on the same state space
 * 3. Verifies the solver matches minimax exactly
 * 4. Reports throughput metrics
 *
 * Usage: benchmark_solver [--states N] [--threads T]
 */

#include "../src/solver_core.h"
#include <chrono>
#include <set>

using Clock = std::chrono::high_resolution_clock;

// ---------------------------------------------------------------------------
// Ground truth via minimax (small state spaces only)
// ---------------------------------------------------------------------------

struct MinimaxResult {
    size_t total;
    size_t wins;
    size_t losses;
    size_t draws;  // cycles
};

static MinimaxResult compute_ground_truth(size_t max_states) {
    // Enumerate states up to max_states using DFS, then solve with minimax
    size_t table_cap = max_states * 4;
    LabelHashTable table(table_cap, false);

    // Enumerate first
    std::vector<CompactState> stack;
    BaoState start;
    start.init_start();
    uint64_t h = canonical_hash(start);
    table.insert(h, start.is_terminal() ? LABEL_LOSS : LABEL_UNKNOWN);
    if (!start.is_terminal()) {
        CompactState cs; cs.from_bao(start);
        stack.push_back(cs);
    }

    size_t count = 1;
    while (!stack.empty() && count < max_states) {
        BaoState st;
        stack.back().to_bao(st);
        stack.pop_back();

        Move moves[MAX_MOVES];
        int n = st.generate_moves(moves);
        for (int i = 0; i < n && count < max_states; ++i) {
            BaoState succ = st;
            if (succ.make_move(moves[i]) != MoveResult::OK) continue;
            uint64_t sh = canonical_hash(succ);
            bool is_term = succ.is_terminal();
            if (!table.insert(sh, is_term ? LABEL_LOSS : LABEL_UNKNOWN)) continue;
            count++;
            if (!is_term) {
                CompactState cs; cs.from_bao(succ);
                stack.push_back(cs);
            }
        }
    }

    // Now solve with iterative resolution (same algorithm as production,
    // but single-threaded for determinism)
    bool changed = true;
    int passes = 0;
    while (changed) {
        changed = false;
        passes++;

        // Re-enumerate and resolve
        stack.clear();
        BaoState st2;
        st2.init_start();
        if (!st2.is_terminal()) {
            CompactState cs; cs.from_bao(st2);
            stack.push_back(cs);
        }

        // Simple DFS visited set for this pass
        std::set<uint64_t> visited;
        visited.insert(canonical_hash(st2));

        while (!stack.empty()) {
            BaoState state;
            stack.back().to_bao(state);
            stack.pop_back();

            if (state.is_terminal()) continue;

            uint64_t my_h = canonical_hash(state);
            uint32_t my_label = table.lookup(my_h);

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

                uint64_t sh = canonical_hash(succ);

                // Push for DFS if not visited this pass
                if (visited.find(sh) == visited.end()) {
                    visited.insert(sh);
                    if (!succ.is_terminal()) {
                        CompactState cs; cs.from_bao(succ);
                        stack.push_back(cs);
                    }
                }

                uint32_t sl = table.lookup(sh);
                if (sl == LABEL_LOSS) found_loss = true;
                if (sl != LABEL_WIN) all_win = false;
            }

            if (my_label == LABEL_UNKNOWN) {
                if (found_loss) {
                    table.update_label(my_h, LABEL_WIN);
                    changed = true;
                } else if (all_win) {
                    table.update_label(my_h, LABEL_LOSS);
                    changed = true;
                }
            }
        }
    }

    // Count results
    MinimaxResult res = {};
    res.total = count;

    // Re-enumerate to count labels
    stack.clear();
    BaoState st3;
    st3.init_start();
    {
        CompactState cs; cs.from_bao(st3);
        stack.push_back(cs);
    }
    std::set<uint64_t> visited2;
    visited2.insert(canonical_hash(st3));

    // Count start position
    uint32_t start_label = table.lookup(canonical_hash(st3));
    if (start_label == LABEL_WIN) res.wins++;
    else if (start_label == LABEL_LOSS) res.losses++;
    else res.draws++;

    while (!stack.empty()) {
        BaoState state;
        stack.back().to_bao(state);
        stack.pop_back();

        if (state.is_terminal()) continue;

        Move moves[MAX_MOVES];
        int n = state.generate_moves(moves);
        for (int i = 0; i < n; ++i) {
            BaoState succ = state;
            if (succ.make_move(moves[i]) != MoveResult::OK) continue;
            uint64_t sh = canonical_hash(succ);
            if (visited2.find(sh) == visited2.end()) {
                visited2.insert(sh);
                uint32_t sl = table.lookup(sh);
                if (sl == LABEL_WIN) res.wins++;
                else if (sl == LABEL_LOSS) res.losses++;
                else res.draws++;
                if (!succ.is_terminal()) {
                    CompactState cs; cs.from_bao(succ);
                    stack.push_back(cs);
                }
            }
        }
    }

    printf("Ground truth (%zu states, %d passes):\n", count, passes);
    printf("  WIN:  %zu (%.1f%%)\n", res.wins, 100.0 * res.wins / count);
    printf("  LOSS: %zu (%.1f%%)\n", res.losses, 100.0 * res.losses / count);
    printf("  DRAW: %zu (%.4f%%)\n", res.draws, 100.0 * res.draws / count);
    printf("\n");

    return res;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    size_t max_states = 50000;  // small for minimax feasibility
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

    // Step 1: Compute ground truth via minimax
    printf("--- Computing ground truth (minimax) ---\n");
    auto gt_start = Clock::now();
    MinimaxResult gt = compute_ground_truth(max_states);
    auto gt_end = Clock::now();
    double gt_elapsed = std::chrono::duration<double>(gt_end - gt_start).count();
    printf("Ground truth computed in %.2f sec\n\n", gt_elapsed);

    // Step 2: Run parallel solver on same state space
    printf("--- Running parallel solver ---\n");
    size_t table_cap = max_states * 4;
    LabelHashTable table(table_cap, false);

    SolverGlobals g;
    g.num_threads = num_threads;

    // Init pass
    size_t warmup_target = (size_t)num_threads * 1000;
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

    printf("Init: %zu states (%zu terminal)\n", total_states, total_terminal);

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
        if (pass > 500) { printf("  ERROR: too many passes\n"); break; }
    }

    auto solve_end = Clock::now();
    double solve_elapsed = std::chrono::duration<double>(solve_end - solve_start).count();

    size_t total_draw = total_states - total_win - total_loss;

    printf("\nSolver result (%d passes, %.2f sec):\n", pass, solve_elapsed);
    printf("  WIN:  %zu (%.1f%%)\n", total_win, 100.0 * total_win / total_states);
    printf("  LOSS: %zu (%.1f%%)\n", total_loss, 100.0 * total_loss / total_states);
    printf("  DRAW: %zu (%.4f%%)\n", total_draw, 100.0 * total_draw / total_states);

    // Step 3: Verify against ground truth
    printf("\n--- Verification ---\n");
    int failures = 0;

    if (total_win != gt.wins) {
        printf("FAIL: WIN count %zu != ground truth %zu\n", total_win, gt.wins);
        failures++;
    }
    if (total_loss != gt.losses) {
        printf("FAIL: LOSS count %zu != ground truth %zu\n", total_loss, gt.losses);
        failures++;
    }
    if (total_draw != gt.draws) {
        printf("FAIL: DRAW count %zu != ground truth %zu\n", total_draw, gt.draws);
        failures++;
    }

    // Check starting position
    BaoState start;
    start.init_start();
    uint32_t start_label = table.lookup(canonical_hash(start));
    printf("Starting position: %s\n",
           start_label == LABEL_WIN ? "WIN" :
           start_label == LABEL_LOSS ? "LOSS" :
           start_label == LABEL_UNKNOWN ? "DRAW" : "???");

    bool passed = (failures == 0);
    printf("\nCorrectness:        %s\n", passed ? "PASS" : "FAIL");
    printf("\n=== METRICS ===\n");
    printf("Resolve throughput: %.0f states/sec\n", total_states * pass / solve_elapsed);
    printf("Passes:             %d\n", pass);
    printf("Memory:             %.1f bytes/state\n",
           (double)(table_cap * sizeof(uint32_t)) / total_states);

    return passed ? 0 : 1;
}
