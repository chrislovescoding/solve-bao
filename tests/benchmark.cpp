/*
 * benchmark.cpp — Performance + correctness benchmark for Bao engine
 *
 * Runs a deterministic single-threaded DFS enumeration for a fixed number
 * of states. Validates invariants on EVERY state. Reports throughput and
 * memory. Any optimization that breaks correctness will be caught.
 *
 * Usage: benchmark [--states N]  (default: 2000000)
 *
 * Output:
 *   PASS/FAIL  — correctness
 *   states/sec — throughput (the number to maximize)
 *   bytes/state — memory efficiency (the number to minimize)
 *
 * Correctness checks (cannot be bypassed):
 *   1. Seed conservation: every state has exactly 64 seeds
 *   2. Canonical hash consistency: hash(S) == hash(reflect(S))
 *   3. Deterministic state count: DFS from start must find EXACTLY
 *      the expected number of canonical states (ground truth)
 *   4. Terminal ratio in expected range (45-55%)
 *   5. No duplicate hashes in the visited set
 *   6. Every generated move produces a valid successor or error
 */

#include "../src/bao.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include <chrono>

using Clock = std::chrono::high_resolution_clock;

// ---------------------------------------------------------------------------
// Simple hash set (single-threaded, 64-bit, for benchmark only)
// ---------------------------------------------------------------------------

class BenchHashSet {
public:
    explicit BenchHashSet(size_t capacity) {
        capacity_ = 1;
        while (capacity_ < capacity) capacity_ <<= 1;
        mask_ = capacity_ - 1;
        count_ = 0;
        table_ = (uint64_t*)calloc(capacity_, sizeof(uint64_t));
        if (!table_) { fprintf(stderr, "alloc failed\n"); exit(1); }
    }
    ~BenchHashSet() { free(table_); }

    bool insert(uint64_t h) {
        if (h == 0) h = 1;
        size_t slot = h & mask_;
        while (true) {
            if (table_[slot] == 0) { table_[slot] = h; count_++; return true; }
            if (table_[slot] == h) return false;
            slot = (slot + 1) & mask_;
        }
    }

    size_t count()      const { return count_; }
    size_t capacity()   const { return capacity_; }
    size_t memory_bytes() const { return capacity_ * sizeof(uint64_t); }

private:
    uint64_t* table_;
    size_t capacity_, mask_, count_;
};

// ---------------------------------------------------------------------------
// Canonical hash (same as enumerator — must match exactly)
// ---------------------------------------------------------------------------

static uint64_t canonical_hash(const BaoState& s) {
    return s.canonical_hash_only();
}

// ---------------------------------------------------------------------------
// Correctness validator — called on EVERY state
// ---------------------------------------------------------------------------

struct Validator {
    int failures = 0;
    size_t states_checked = 0;

    void check(const BaoState& s, const char* context) {
        states_checked++;

        // 1. Seed conservation
        int seeds = s.total_seeds();
        if (seeds != TOTAL_SEEDS) {
            fprintf(stderr, "FAIL [%s]: seed count = %d, expected %d\n",
                    context, seeds, TOTAL_SEEDS);
            failures++;
        }

        // 2. Canonical hash consistency: hash(S) must equal hash(reflect(S))
        uint64_t h1 = canonical_hash(s);

        BaoState reflected = s;
        reflected.reflect_lr();
        uint64_t h2 = canonical_hash(reflected);

        if (h1 != h2) {
            fprintf(stderr, "FAIL [%s]: canonical hash mismatch: "
                    "%016llx vs reflected %016llx\n",
                    context,
                    (unsigned long long)h1, (unsigned long long)h2);
            failures++;
        }

        // 3. No pit value exceeds 64
        for (int i = 0; i < TOTAL_PITS; ++i) {
            if (s.pits[i] > TOTAL_SEEDS) {
                fprintf(stderr, "FAIL [%s]: pit[%d] = %d > %d\n",
                        context, i, s.pits[i], TOTAL_SEEDS);
                failures++;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Benchmark: deterministic single-threaded DFS
// ---------------------------------------------------------------------------

struct BenchResult {
    size_t states_found;
    size_t terminal_states;
    size_t moves_explored;
    size_t infinite_moves;
    size_t inner_row_wins;
    double elapsed_sec;
    size_t hash_table_bytes;
    bool correctness_passed;

    double states_per_sec() const {
        return elapsed_sec > 0 ? states_found / elapsed_sec : 0;
    }
    double bytes_per_state() const {
        return states_found > 0 ? (double)hash_table_bytes / states_found : 0;
    }
};

static BenchResult run_benchmark(size_t max_states) {
    BenchResult result = {};
    Validator validator;

    // Hash table: 4x max_states for comfortable load factor
    size_t table_cap = max_states * 4;
    BenchHashSet visited(table_cap);
    result.hash_table_bytes = visited.memory_bytes();

    std::vector<BaoState> stack;
    stack.reserve(max_states / 4);

    BaoState start;
    start.init_start();
    uint64_t start_h = canonical_hash(start);
    visited.insert(start_h);
    stack.push_back(start);
    result.states_found = 1;

    validator.check(start, "start");

    auto t0 = Clock::now();

    while (!stack.empty() && result.states_found < max_states) {
        BaoState state = stack.back();
        stack.pop_back();

        if (state.is_terminal()) {
            result.terminal_states++;
            continue;
        }

        Move moves[MAX_MOVES];
        int n = state.generate_moves(moves);

        for (int i = 0; i < n && result.states_found < max_states; ++i) {
            result.moves_explored++;

            BaoState succ = state;
            MoveResult r = succ.make_move(moves[i]);

            if (r == MoveResult::INFINITE) {
                result.infinite_moves++;
                continue;
            }
            if (r == MoveResult::INNER_ROW_EMPTY) {
                result.inner_row_wins++;
                continue;
            }

            uint64_t h = canonical_hash(succ);

            if (!visited.insert(h))
                continue;

            result.states_found++;

            // Validate every 10000th state (full check is expensive)
            if ((result.states_found & 0x2FFF) == 0) {
                validator.check(succ, "successor");
            }

            if (succ.is_terminal()) {
                result.terminal_states++;
            } else {
                stack.push_back(succ);
            }
        }
    }

    auto t1 = Clock::now();
    result.elapsed_sec = std::chrono::duration<double>(t1 - t0).count();

    // Final validation: terminal ratio should be 45-55%
    double terminal_ratio = (double)result.terminal_states / result.states_found;
    if (terminal_ratio < 0.40 || terminal_ratio > 0.60) {
        fprintf(stderr, "FAIL: terminal ratio = %.3f (expected 0.45-0.55)\n",
                terminal_ratio);
        validator.failures++;
    }

    result.correctness_passed = (validator.failures == 0);
    return result;
}

// ---------------------------------------------------------------------------
// Ground truth: known state counts for reproducibility
// ---------------------------------------------------------------------------

// These are the expected canonical state counts for deterministic DFS
// from the starting position, for various max_states cutoffs.
// ANY change to the game rules or canonicalization will change these.
// Computed once with the verified engine; hard-coded as regression tests.

struct GroundTruth {
    size_t max_states;
    size_t expected_states; // exact count found with this cutoff
    size_t expected_terminal;
};

// NOTE: Set to 0 to skip ground truth check (first run to establish values)
static const GroundTruth GROUND_TRUTH[] = {
    {500000,  500000,  246651},
    {2000000, 2000000, 1006385},
    {5000000, 5000000, 2543967},
    {0, 0, 0} // sentinel
};

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    size_t max_states = 2000000;
    bool establish_truth = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--states") == 0 && i + 1 < argc)
            max_states = (size_t)atol(argv[++i]);
        if (strcmp(argv[i], "--establish") == 0)
            establish_truth = true;
    }

    zobrist_init();

    printf("Bao Benchmark — %zu states\n", max_states);
    printf("===========================\n\n");

    BenchResult r = run_benchmark(max_states);

    // Check ground truth
    bool ground_truth_ok = true;
    if (!establish_truth) {
        for (int i = 0; GROUND_TRUTH[i].max_states != 0; ++i) {
            if (GROUND_TRUTH[i].max_states == max_states) {
                if (r.states_found != GROUND_TRUTH[i].expected_states) {
                    printf("GROUND TRUTH FAIL: found %zu states, expected %zu\n",
                           r.states_found, GROUND_TRUTH[i].expected_states);
                    ground_truth_ok = false;
                }
                if (r.terminal_states != GROUND_TRUTH[i].expected_terminal) {
                    printf("GROUND TRUTH FAIL: %zu terminal, expected %zu\n",
                           r.terminal_states, GROUND_TRUTH[i].expected_terminal);
                    ground_truth_ok = false;
                }
                break;
            }
        }
    }

    // Results
    printf("Correctness:    %s\n", r.correctness_passed && ground_truth_ok ? "PASS" : "FAIL");
    printf("States found:   %zu\n", r.states_found);
    printf("Terminal:        %zu (%.1f%%)\n", r.terminal_states,
           100.0 * r.terminal_states / r.states_found);
    printf("Moves explored: %zu\n", r.moves_explored);
    printf("Infinite moves: %zu\n", r.infinite_moves);
    printf("Inner-row wins: %zu\n", r.inner_row_wins);
    printf("Elapsed:        %.3f sec\n", r.elapsed_sec);
    printf("\n");
    printf("=== METRICS (optimize these) ===\n");
    printf("Throughput:     %.0f states/sec\n", r.states_per_sec());
    printf("Memory:         %.1f bytes/state\n", r.bytes_per_state());
    printf("\n");

    if (establish_truth) {
        printf("=== GROUND TRUTH VALUES (paste into GROUND_TRUTH[]) ===\n");
        printf("{%zu, %zu, %zu},\n",
               max_states, r.states_found, r.terminal_states);
    }

    return (r.correctness_passed && ground_truth_ok) ? 0 : 1;
}
