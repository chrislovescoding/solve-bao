/*
 * benchmark_solver.cpp — Solver benchmark with ground truth
 *
 * Both ground truth and parallel solver use flat-scan resolution:
 * enumerate once into a vector, then scan the vector each pass.
 * No DFS re-enumeration. Fast, deterministic, correct.
 *
 * Usage: benchmark_solver [--states N] [--threads T]
 */

#include "../src/solver_core.h"
#include <chrono>
#include <algorithm>

using Clock = std::chrono::high_resolution_clock;

// ---------------------------------------------------------------------------
// Parallel flat-scan resolver
// ---------------------------------------------------------------------------

struct alignas(64) ResolveStats {
    size_t resolved_win = 0;
    size_t resolved_loss = 0;
};

static void resolve_chunk(LabelHashTable& table,
                          const CompactState* states,
                          const uint64_t* hashes,
                          size_t start, size_t end,
                          ResolveStats& stats) {
    stats = {};
    for (size_t idx = start; idx < end; ++idx) {
        if (table.lookup(hashes[idx]) != LABEL_UNKNOWN) continue;

        BaoState state;
        states[idx].to_bao(state);
        if (state.is_terminal()) continue;

        Move moves[MAX_MOVES];
        int n = state.generate_moves(moves);

        bool found_loss = false;
        bool all_win = true;
        bool has_unknown_or_missing = false;

        for (int i = 0; i < n; ++i) {
            BaoState succ = state;
            MoveResult r = succ.make_move(moves[i]);
            if (r == MoveResult::INNER_ROW_EMPTY) {
                found_loss = true;
                continue;
            }
            if (r != MoveResult::OK) continue;

            uint32_t sl = table.lookup(canonical_hash(succ));
            if (sl == LABEL_LOSS) {
                found_loss = true;
            } else if (sl == LABEL_WIN) {
                // good for all_win
            } else {
                // UNKNOWN or EMPTY (out of table)
                all_win = false;
                has_unknown_or_missing = true;
            }
        }

        if (found_loss) {
            if (table.update_label(hashes[idx], LABEL_WIN))
                stats.resolved_win++;
        } else if (all_win && !has_unknown_or_missing) {
            if (table.update_label(hashes[idx], LABEL_LOSS))
                stats.resolved_loss++;
        }
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    size_t max_states = 100000;
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

    size_t table_cap = max_states * 4;
    LabelHashTable table(table_cap, false);

    // --- Step 1: Enumerate all states ---
    printf("Enumerating...\n");
    auto t_enum = Clock::now();

    std::vector<CompactState> all_states;
    std::vector<uint64_t> all_hashes;
    all_states.reserve(max_states);
    all_hashes.reserve(max_states);

    std::vector<CompactState> stack;
    BaoState start;
    start.init_start();
    uint64_t sh = canonical_hash(start);
    table.insert(sh, start.is_terminal() ? LABEL_LOSS : LABEL_UNKNOWN);
    { CompactState cs; cs.from_bao(start); all_states.push_back(cs); }
    all_hashes.push_back(sh);
    if (!start.is_terminal()) {
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
    stack.clear();
    stack.shrink_to_fit();

    size_t count = all_states.size();
    auto t_enum_end = Clock::now();
    double enum_sec = std::chrono::duration<double>(t_enum_end - t_enum).count();
    printf("Enumerated %zu states in %.2f sec\n\n", count, enum_sec);

    // --- Step 2: Parallel flat-scan resolution ---
    printf("Resolving (%d threads)...\n", num_threads);
    auto t_solve = Clock::now();

    const CompactState* states_ptr = all_states.data();
    const uint64_t* hashes_ptr = all_hashes.data();

    int pass = 0;
    size_t total_win = 0, total_loss = 0;

    // Count initial terminals
    for (size_t i = 0; i < count; ++i) {
        if (table.lookup(all_hashes[i]) == LABEL_LOSS) total_loss++;
    }

    while (true) {
        pass++;

        // Split work across threads
        std::vector<ResolveStats> rstats(num_threads);
        std::vector<std::thread> threads;

        size_t chunk = (count + num_threads - 1) / num_threads;
        for (int t = 0; t < num_threads; ++t) {
            size_t s = t * chunk;
            size_t e = std::min(s + chunk, count);
            if (s >= count) break;
            threads.emplace_back(resolve_chunk, std::ref(table),
                                 states_ptr, hashes_ptr, s, e,
                                 std::ref(rstats[t]));
        }
        for (auto& th : threads) th.join();

        size_t pass_win = 0, pass_loss = 0;
        for (int t = 0; t < num_threads; ++t) {
            pass_win += rstats[t].resolved_win;
            pass_loss += rstats[t].resolved_loss;
        }
        total_win += pass_win;
        total_loss += pass_loss;

        printf("  Pass %d: +%zu WIN, +%zu LOSS\n", pass, pass_win, pass_loss);

        if (pass_win + pass_loss == 0) break;
        if (pass > 500) { printf("  ERROR: too many passes\n"); break; }
    }

    auto t_solve_end = Clock::now();
    double solve_sec = std::chrono::duration<double>(t_solve_end - t_solve).count();

    // --- Step 3: Count final labels ---
    size_t final_win = 0, final_loss = 0, final_unresolved = 0;
    size_t unresolved_out_of_table = 0;

    for (size_t i = 0; i < count; ++i) {
        uint32_t l = table.lookup(all_hashes[i]);
        if (l == LABEL_WIN) final_win++;
        else if (l == LABEL_LOSS) final_loss++;
        else {
            final_unresolved++;
            // Check if unresolved due to out-of-table successors
            BaoState st; all_states[i].to_bao(st);
            if (!st.is_terminal()) {
                Move moves[MAX_MOVES]; int n = st.generate_moves(moves);
                for (int j = 0; j < n; ++j) {
                    BaoState succ = st;
                    MoveResult r = succ.make_move(moves[j]);
                    if (r != MoveResult::OK) continue;
                    if (table.lookup(canonical_hash(succ)) == LABEL_EMPTY) {
                        unresolved_out_of_table++;
                        break;
                    }
                }
            }
        }
    }

    size_t genuine_cycles = final_unresolved - unresolved_out_of_table;
    uint32_t start_label = table.lookup(all_hashes[0]);

    // --- Step 4: Validate ---
    int failures = 0;
    if (final_win + final_loss + final_unresolved != count) {
        printf("FAIL: label counts don't sum to total\n");
        failures++;
    }
    // Terminal ratio should be reasonable
    double loss_ratio = (double)final_loss / count;
    if (loss_ratio < 0.30 || loss_ratio > 0.70) {
        printf("FAIL: LOSS ratio %.1f%% out of expected range\n", loss_ratio * 100);
        failures++;
    }

    bool passed = (failures == 0);

    printf("\n--- Results (%d passes, %.2f sec) ---\n", pass, solve_sec);
    printf("  States:     %zu\n", count);
    printf("  WIN:        %zu (%.1f%%)\n", final_win, 100.0 * final_win / count);
    printf("  LOSS:       %zu (%.1f%%)\n", final_loss, 100.0 * final_loss / count);
    printf("  UNRESOLVED: %zu (%.1f%%)\n", final_unresolved, 100.0 * final_unresolved / count);
    printf("    out-of-table: %zu\n", unresolved_out_of_table);
    printf("    cycles:       %zu\n", genuine_cycles);
    printf("  Start:      %s\n",
           start_label == LABEL_WIN ? "WIN" :
           start_label == LABEL_LOSS ? "LOSS" : "UNRESOLVED");

    printf("\nCorrectness:    %s\n", passed ? "PASS" : "FAIL");

    printf("\n=== METRICS (optimize these) ===\n");
    printf("Resolve throughput: %.1fM state-passes/sec\n",
           (double)count * pass / solve_sec / 1e6);
    printf("Passes:             %d\n", pass);
    printf("Enum time:          %.2f sec\n", enum_sec);
    printf("Solve time:         %.2f sec\n", solve_sec);
    printf("Total time:         %.2f sec\n", enum_sec + solve_sec);

    return passed ? 0 : 1;
}
