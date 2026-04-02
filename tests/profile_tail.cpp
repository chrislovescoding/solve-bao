#define BAO_ENABLE_MOVE_PROFILE

#include "../src/enumerate_core.h"
#include <chrono>
#include <vector>
#include <algorithm>

using Clock = std::chrono::high_resolution_clock;

thread_local BaoMoveProfile* g_bao_move_profile = nullptr;

static uint32_t rng_state = 0xBAD5EEDU;

static uint32_t rng_next() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

struct Entry {
    BaoState state;
};

static std::vector<Entry> build_corpus(int min_depth, int min_seeds, size_t target) {
    std::vector<Entry> corpus;
    corpus.reserve(target);

    while (corpus.size() < target) {
        BaoState s;
        s.init_start();
        int depth = 0;

        for (int step = 0; step < 600; ++step) {
            if (s.is_terminal()) break;

            Move moves[MAX_MOVES];
            int n = s.generate_moves(moves);
            if (n == 0) break;

            if (depth >= min_depth && corpus.size() < target) {
                int max_pit = 0;
                for (int i = 0; i < PITS_PER_SIDE; ++i)
                    max_pit = std::max(max_pit, (int)s.pits[i]);
                if (max_pit >= min_seeds) {
                    Entry e;
                    e.state = s;
                    corpus.push_back(e);
                }
            }

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

static inline uint64_t elapsed_ns(Clock::time_point a, Clock::time_point b) {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}

struct PhaseStats {
    size_t states_processed = 0;
    size_t moves_generated  = 0;
    size_t ok_moves         = 0;
    size_t infinite_moves   = 0;
    size_t inner_moves      = 0;
    size_t insert_true      = 0;
    size_t insert_false     = 0;
    size_t terminal_new     = 0;
    uint64_t generate_ns    = 0;
    uint64_t move_ns        = 0;
    uint64_t hash_ns        = 0;
    uint64_t insert_ns      = 0;
    uint64_t terminal_ns    = 0;
    double elapsed_sec      = 0.0;
    BaoMoveProfile move_profile;
};

static PhaseStats run_phase(const std::vector<Entry>& corpus,
                            AtomicHashSet& visited,
                            size_t passes) {
    PhaseStats stats;
    g_bao_move_profile = &stats.move_profile;

    auto phase_t0 = Clock::now();

    for (size_t pass = 0; pass < passes; ++pass) {
        for (const Entry& e : corpus) {
            stats.states_processed++;

            Move moves[MAX_MOVES];
            auto t0 = Clock::now();
            int n = e.state.generate_moves(moves);
            auto t1 = Clock::now();
            stats.generate_ns += elapsed_ns(t0, t1);
            stats.moves_generated += n;

            for (int i = 0; i < n; ++i) {
                BaoState succ = e.state;

                auto tm0 = Clock::now();
                MoveResult r = succ.make_move(moves[i]);
                auto tm1 = Clock::now();
                stats.move_ns += elapsed_ns(tm0, tm1);

                if (r == MoveResult::INFINITE) {
                    stats.infinite_moves++;
                    continue;
                }
                if (r == MoveResult::INNER_ROW_EMPTY) {
                    stats.inner_moves++;
                    continue;
                }

                stats.ok_moves++;

                auto th0 = Clock::now();
                uint64_t h = canonical_hash(succ);
                auto th1 = Clock::now();
                stats.hash_ns += elapsed_ns(th0, th1);

                auto ti0 = Clock::now();
                bool is_new = visited.insert(h);
                auto ti1 = Clock::now();
                stats.insert_ns += elapsed_ns(ti0, ti1);

                if (is_new) {
                    stats.insert_true++;
                    auto tt0 = Clock::now();
                    if (succ.is_terminal())
                        stats.terminal_new++;
                    auto tt1 = Clock::now();
                    stats.terminal_ns += elapsed_ns(tt0, tt1);
                } else {
                    stats.insert_false++;
                }
            }
        }
    }

    auto phase_t1 = Clock::now();
    g_bao_move_profile = nullptr;
    stats.elapsed_sec = std::chrono::duration<double>(phase_t1 - phase_t0).count();
    return stats;
}

static void print_phase(const char* label, const PhaseStats& s) {
    double total_ns = (double)(s.generate_ns + s.move_ns + s.hash_ns +
                               s.insert_ns + s.terminal_ns);
    auto pct = [total_ns](uint64_t ns) {
        return total_ns > 0.0 ? (100.0 * ns / total_ns) : 0.0;
    };

    double states_per_sec = s.elapsed_sec > 0 ? s.states_processed / s.elapsed_sec : 0.0;
    double inserts_per_sec = s.elapsed_sec > 0 ? s.insert_true / s.elapsed_sec : 0.0;
    double dup_per_sec = s.elapsed_sec > 0 ? s.insert_false / s.elapsed_sec : 0.0;
    double avg_moves = s.states_processed > 0 ? (double)s.moves_generated / s.states_processed : 0.0;
    double avg_sows = s.move_profile.move_calls > 0
        ? (double)s.move_profile.sow_calls / s.move_profile.move_calls : 0.0;
    double avg_total_sown = s.move_profile.move_calls > 0
        ? (double)s.move_profile.total_sown / s.move_profile.move_calls : 0.0;
    double dup_ratio = (s.insert_true + s.insert_false) > 0
        ? (100.0 * s.insert_false / (s.insert_true + s.insert_false)) : 0.0;

    printf("%s\n", label);
    printf("%.*s\n", (int)strlen(label), "----------------------------------------");
    printf("States processed:   %zu (%.1fK states/sec)\n",
           s.states_processed, states_per_sec / 1e3);
    printf("Moves generated:    %zu (avg %.2f/state)\n",
           s.moves_generated, avg_moves);
    printf("Move results:       OK=%zu  INFINITE=%zu  INNER_ROW_EMPTY=%zu\n",
           s.ok_moves, s.infinite_moves, s.inner_moves);
    printf("Hash inserts:       new=%zu (%.1fK/sec)  dup=%zu (%.1fK/sec, %.1f%% dup)\n",
           s.insert_true, inserts_per_sec / 1e3,
           s.insert_false, dup_per_sec / 1e3, dup_ratio);
    printf("Time split:         generate=%.1f%%  make_move=%.1f%%  hash=%.1f%%  insert=%.1f%%  terminal=%.1f%%\n",
           pct(s.generate_ns), pct(s.move_ns), pct(s.hash_ns),
           pct(s.insert_ns), pct(s.terminal_ns));
    printf("Relay profile:      avg sow calls/move=%.2f  avg total_sown/move=%.2f  large_sows=%zu\n",
           avg_sows, avg_total_sown, (size_t)s.move_profile.large_sow_calls);
    printf("Resows:             capture=%zu  relay=%zu  max total_sown=%zu  max sow calls=%zu\n",
           (size_t)s.move_profile.capture_resows,
           (size_t)s.move_profile.relay_resows,
           (size_t)s.move_profile.max_total_sown,
           (size_t)s.move_profile.max_sow_calls_in_move);
    printf("\n");
}

int main(int argc, char* argv[]) {
    int min_depth = 80;
    int min_seeds = 24;
    size_t corpus_size = 5000;
    size_t duplicate_passes = 200;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--depth") && i + 1 < argc) min_depth = atoi(argv[++i]);
        if (!strcmp(argv[i], "--seeds") && i + 1 < argc) min_seeds = atoi(argv[++i]);
        if (!strcmp(argv[i], "--corpus") && i + 1 < argc) corpus_size = (size_t)atol(argv[++i]);
        if (!strcmp(argv[i], "--dup-passes") && i + 1 < argc) duplicate_passes = (size_t)atol(argv[++i]);
    }

    zobrist_init();

    printf("Bao Tail Profiler\n");
    printf("=================\n");
    printf("Corpus target: %zu states, depth >= %d, max_pit >= %d\n",
           corpus_size, min_depth, min_seeds);
    printf("Duplicate phase: %zu passes over the same corpus\n\n", duplicate_passes);

    auto setup_t0 = Clock::now();
    std::vector<Entry> corpus = build_corpus(min_depth, min_seeds, corpus_size);
    auto setup_t1 = Clock::now();
    double setup_sec = std::chrono::duration<double>(setup_t1 - setup_t0).count();

    printf("Corpus built:  %zu states in %.2fs\n\n", corpus.size(), setup_sec);

    size_t table_cap = std::max((size_t)65536, corpus.size() * 16);
    AtomicHashSet visited(table_cap, false);

    PhaseStats discovery = run_phase(corpus, visited, 1);
    PhaseStats duplicate = run_phase(corpus, visited, duplicate_passes);

    print_phase("Discovery Pass", discovery);
    print_phase("Duplicate Tail Passes", duplicate);

    printf("Takeaway\n");
    printf("--------\n");
    printf("The duplicate phase is the tail analogue: states are still being processed,\n");
    printf("but almost every successor hash is already present. If processed states/sec\n");
    printf("stays high while new inserts/sec collapses, the apparent slowdown is a\n");
    printf("measurement artifact of tracking discovery rate instead of expansion rate.\n");

    return 0;
}
