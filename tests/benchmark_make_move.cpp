/*
 * benchmark_make_move.cpp — Targeted make_move performance benchmark
 *
 * The production enumerator's tail phase is bottlenecked by make_move() on
 * deep game states where relay chains sow hundreds of seeds (concentrated
 * seeds in a few pits). The bottleneck breakdown in the tail is roughly:
 *
 *   ~95% make_move (relay sowing loops)
 *   ~5%  canonical_hash, stack ops, hash table insert
 *
 * This benchmark exercises EXACTLY those expensive states.
 *
 * PRIMARY METRIC: make_move calls/sec on deep states (MAXIMIZE THIS)
 * SECONDARY:      INFINITE rate (wasted work hitting the 800-seed threshold)
 *
 * Correctness checks (ALL must pass before reporting throughput):
 *   1. Seed conservation: every OK successor has exactly 64 seeds
 *   2. Determinism: running corpus twice gives identical successor hashes
 *   3. `make bench` must still pass (DFS ground truth unchanged)
 *
 * Usage: ./build/benchmark_make_move [--depth N] [--corpus N] [--secs F]
 */

#include "../src/bao.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <chrono>

using Clock = std::chrono::high_resolution_clock;

// ---------------------------------------------------------------------------
// Fixed-seed xorshift32 for reproducible corpus (same states every run)
// ---------------------------------------------------------------------------
static uint32_t rng_state = 0xBAD5EEDU;
static uint32_t rng_next() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

// ---------------------------------------------------------------------------
// Corpus entry: one deep game state + all its pre-generated moves
// ---------------------------------------------------------------------------
struct Entry {
    BaoState state;
    Move     moves[MAX_MOVES];
    int      nmoves;
};

// ---------------------------------------------------------------------------
// Corpus generation
//
// Plays random games, collecting states at depth >= min_depth.
// Filters for states where at least one pit has >= min_seeds seeds —
// these produce expensive relay chains that reflect the real bottleneck.
// ---------------------------------------------------------------------------
static std::vector<Entry> build_corpus(int min_depth, int min_seeds, size_t target) {
    std::vector<Entry> corpus;
    corpus.reserve(target);

    while (corpus.size() < target) {
        BaoState s;
        s.init_start();
        int depth = 0;

        for (int step = 0; step < 400; ++step) {
            if (s.is_terminal()) break;

            Move moves[MAX_MOVES];
            int n = s.generate_moves(moves);
            if (n == 0) break;

            // Collect states at the right depth with concentrated seeds
            if (depth >= min_depth && corpus.size() < target) {
                int max_pit = 0;
                for (int i = 0; i < PITS_PER_SIDE; ++i)
                    max_pit = std::max(max_pit, (int)s.pits[i]);
                if (max_pit >= min_seeds) {
                    Entry e;
                    e.state  = s;
                    e.nmoves = n;
                    memcpy(e.moves, moves, n * sizeof(Move));
                    corpus.push_back(e);
                }
            }

            // Pick a random valid move, try all if needed
            bool moved = false;
            int start = rng_next() % n;
            for (int attempt = 0; attempt < n; ++attempt) {
                int idx = (start + attempt) % n;
                BaoState tmp = s;
                if (tmp.make_move(moves[idx]) == MoveResult::OK) {
                    s = tmp;
                    depth++;
                    moved = true;
                    break;
                }
            }
            if (!moved) break;
        }
    }

    return corpus;
}

// ---------------------------------------------------------------------------
// Run the corpus once, collecting outcomes for correctness checking
// ---------------------------------------------------------------------------
struct RunStats {
    std::vector<uint64_t> fingerprints; // successor hash or sentinel per call
    size_t total_calls   = 0;
    size_t ok_count      = 0;
    size_t infinite_count= 0;
    size_t inner_count   = 0;
    size_t seed_failures = 0;
};

static RunStats run_corpus_once(const std::vector<Entry>& corpus) {
    RunStats rs;
    for (const Entry& e : corpus) {
        for (int i = 0; i < e.nmoves; ++i) {
            rs.total_calls++;
            BaoState succ = e.state;
            MoveResult mr = succ.make_move(e.moves[i]);

            if (mr == MoveResult::OK) {
                rs.ok_count++;
                int seeds = succ.total_seeds();
                if (seeds != TOTAL_SEEDS) {
                    rs.seed_failures++;
                    rs.fingerprints.push_back(0xBAD0BAD0BAD0BAD0ULL);
                } else {
                    rs.fingerprints.push_back(succ.canonical_hash_only());
                }
            } else if (mr == MoveResult::INFINITE) {
                rs.infinite_count++;
                rs.fingerprints.push_back(0xFFFF000000000001ULL);
            } else { // INNER_ROW_EMPTY
                rs.inner_count++;
                rs.fingerprints.push_back(0xFFFF000000000002ULL);
            }
        }
    }
    return rs;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    int    min_depth   = 20;   // collect states from depth >= this
    int    min_seeds   = 16;   // at least one pit with >= this many seeds (hits batch sow path)
    size_t corpus_size = 5000; // number of deep states in corpus
    double bench_secs  = 5.0;  // timing window

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--depth")  && i+1<argc) min_depth   = atoi(argv[++i]);
        if (!strcmp(argv[i], "--seeds")  && i+1<argc) min_seeds   = atoi(argv[++i]);
        if (!strcmp(argv[i], "--corpus") && i+1<argc) corpus_size = (size_t)atol(argv[++i]);
        if (!strcmp(argv[i], "--secs")   && i+1<argc) bench_secs  = atof(argv[++i]);
    }

    zobrist_init();

    printf("Bao make_move Benchmark\n");
    printf("=======================\n");
    printf("Corpus: %zu states, depth >= %d, max_pit >= %d seeds\n\n",
           corpus_size, min_depth, min_seeds);

    // ---- Build corpus ----
    auto t_setup = Clock::now();
    std::vector<Entry> corpus = build_corpus(min_depth, min_seeds, corpus_size);
    double setup_sec = std::chrono::duration<double>(Clock::now() - t_setup).count();

    size_t total_moves = 0;
    for (const auto& e : corpus) total_moves += e.nmoves;
    printf("Corpus built:  %zu states, %zu moves total (avg %.1f/state), %.2fs setup\n\n",
           corpus.size(), total_moves, (double)total_moves / corpus_size, setup_sec);

    // ---- Correctness: two passes, compare ----
    printf("Correctness\n");
    printf("-----------\n");

    RunStats r1 = run_corpus_once(corpus);
    RunStats r2 = run_corpus_once(corpus);

    bool seed_ok  = (r1.seed_failures == 0);
    bool determ_ok = (r1.fingerprints == r2.fingerprints);
    bool correct  = seed_ok && determ_ok;

    printf("Seed conservation: %s", seed_ok   ? "PASS" : "FAIL");
    if (!seed_ok)   printf("  (%zu violations!)", r1.seed_failures);
    printf("\n");
    printf("Determinism:       %s", determ_ok ? "PASS" : "FAIL");
    if (!determ_ok) printf("  (non-deterministic output detected!)");
    printf("\n");

    if (!correct) {
        printf("\nCorrectness: FAIL — aborting\n");
        return 1;
    }
    printf("\nCorrectness: PASS\n\n");

    // ---- Move statistics ----
    double inf_pct   = 100.0 * r1.infinite_count / r1.total_calls;
    double inner_pct = 100.0 * r1.inner_count    / r1.total_calls;
    double ok_pct    = 100.0 * r1.ok_count       / r1.total_calls;

    printf("Move profile (deep states)\n");
    printf("--------------------------\n");
    printf("Total calls:     %zu\n",    r1.total_calls);
    printf("OK:              %5.1f%%\n", ok_pct);
    printf("INFINITE:        %5.1f%%  <- wasted work (each sows ~800 seeds)\n", inf_pct);
    printf("INNER_ROW_EMPTY: %5.1f%%\n", inner_pct);
    printf("\n");

    // ---- Timing: run corpus repeatedly for bench_secs ----
    // Use volatile sink to prevent dead-code elimination
    volatile uint8_t sink = 0;
    size_t timing_calls = 0;

    auto t0       = Clock::now();
    auto deadline = t0 + std::chrono::duration<double>(bench_secs);

    while (Clock::now() < deadline) {
        for (const Entry& e : corpus) {
            for (int i = 0; i < e.nmoves; ++i) {
                BaoState succ = e.state;
                succ.make_move(e.moves[i]);
                sink ^= succ.pits[0];
                timing_calls++;
            }
        }
    }

    double elapsed     = std::chrono::duration<double>(Clock::now() - t0).count();
    double calls_per_s = timing_calls / elapsed;
    double us_per_call = 1e6 / calls_per_s;

    printf("Performance (%.1f sec)\n", elapsed);
    printf("---------------------\n");
    printf("Total calls:  %zu\n",  timing_calls);
    printf("Throughput:   %.3fM calls/sec\n", calls_per_s / 1e6);
    printf("Latency:      %.2f us/call\n",    us_per_call);
    printf("\n");

    printf("=== PRIMARY METRIC ===\n");
    printf("make_move calls/sec: %.3fM  (maximize this)\n", calls_per_s / 1e6);
    printf("\n");
    printf("=== SECONDARY METRICS ===\n");
    printf("INFINITE overhead:   ~%.1f%% of time wasted on failed moves\n",
           inf_pct > 0 ? std::min(inf_pct * 2.0, 99.0) : 0.0);
    printf("(Reducing INFINITE rate or detection cost gives proportional speedup)\n");
    printf("\n");

    (void)sink; // suppress unused warning
    return 0;
}
