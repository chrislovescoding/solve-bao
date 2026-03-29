#include "../src/bao.h"
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cstring>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name()
#define RUN(name) do { \
    printf("  %-50s ", #name); \
    name(); \
    printf("[PASS]\n"); \
    ++tests_passed; \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("[FAIL] line %d: %s\n", __LINE__, #cond); \
        ++tests_failed; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a != _b) { \
        printf("[FAIL] line %d: %s == %lld, expected %lld\n", \
               __LINE__, #a, (long long)_a, (long long)_b); \
        ++tests_failed; \
        return; \
    } \
} while(0)

// Helper: create a state from raw pit values and rehash
static BaoState make_state(const uint8_t p[TOTAL_PITS]) {
    BaoState s;
    memcpy(s.pits, p, TOTAL_PITS);
    s.rehash();
    return s;
}

// ===================================================================
// Test: Starting position
// ===================================================================

TEST(test_start_position) {
    BaoState s;
    s.init_start();

    for (int i = 0; i < TOTAL_PITS; ++i)
        ASSERT_EQ(s.pits[i], 2);
    ASSERT_EQ(s.total_seeds(), 64);
    ASSERT(!s.inner_empty(0));
    ASSERT(!s.inner_empty(1));
}

// ===================================================================
// Test: Seed conservation after every move from start
// ===================================================================

TEST(test_seed_conservation_one_move) {
    BaoState s;
    s.init_start();

    Move moves[MAX_MOVES];
    int n = s.generate_moves(moves);
    ASSERT(n > 0);

    for (int i = 0; i < n; ++i) {
        BaoState copy = s;
        MoveResult r = copy.make_move(moves[i]);
        if (r == MoveResult::OK)
            ASSERT_EQ(copy.total_seeds(), 64);
    }
}

// ===================================================================
// Test: All moves from start are mtaji (captures exist everywhere)
// ===================================================================

TEST(test_start_all_mtaji) {
    BaoState s;
    s.init_start();

    Move moves[MAX_MOVES];
    int n = s.generate_moves(moves);
    ASSERT(n > 0);

    for (int i = 0; i < n; ++i)
        ASSERT(moves[i].is_mtaji);

    // Kichwa constraints
    for (int i = 0; i < n; ++i) {
        if (moves[i].pit == KICHWA_L) ASSERT_EQ(moves[i].dir, CW);
        if (moves[i].pit == KICHWA_R) ASSERT_EQ(moves[i].dir, ACW);
    }
}

// ===================================================================
// Test: First sow capture detection (O(1))
// ===================================================================

TEST(test_first_sow_captures) {
    BaoState s;
    s.init_start();

    // Pit 0 CW: landing = (0+1) & 15 = 1, inner, pits[1] >= 1, opp[1] > 0 -> capture
    ASSERT(s.first_sow_captures(0, CW));
    // Pit 0 ACW: landing = (0-1+16) & 15 = 15, outer -> no capture
    ASSERT(!s.first_sow_captures(0, ACW));
    // Pit 3 CW/ACW: both land on inner pits with seeds and opponent seeds
    ASSERT(s.first_sow_captures(3, CW));
    ASSERT(s.first_sow_captures(3, ACW));

    // State with empty opponent inner row -> no captures
    uint8_t p[TOTAL_PITS] = {};
    p[3] = 4;
    p[16 + 10] = 60;
    BaoState s2 = make_state(p);
    ASSERT(!s2.first_sow_captures(3, CW));
    ASSERT(!s2.first_sow_captures(3, ACW));
}

// ===================================================================
// Test: L-R reflection is its own inverse
// ===================================================================

TEST(test_reflect_involution) {
    BaoState s;
    s.init_start();
    s.pits[0] = 5;
    s.pits[7] = 10;
    s.rehash();

    BaoState original = s;
    s.reflect_lr();
    ASSERT_EQ(s.pits[0], 10);
    ASSERT_EQ(s.pits[7], 5);

    s.reflect_lr();
    ASSERT(s == original);
}

// ===================================================================
// Test: Starting position is already canonical
// ===================================================================

TEST(test_start_is_canonical) {
    BaoState s;
    s.init_start();
    BaoState before = s;
    ASSERT(!s.canonicalize());
    ASSERT(s == before);
}

// ===================================================================
// Test: Swap sides
// ===================================================================

TEST(test_swap_sides) {
    BaoState s;
    s.init_start();
    s.pits[0] = 5;
    s.pits[16] = 10;
    s.rehash();

    s.swap_sides();
    ASSERT_EQ(s.pits[0], 10);
    ASSERT_EQ(s.pits[16], 5);
}

// ===================================================================
// Test: Zobrist hash changes when state changes, reverts when restored
// ===================================================================

TEST(test_zobrist_changes) {
    BaoState s;
    s.init_start();
    uint64_t h1 = s.hash;

    s.pits[0] = 5;
    s.rehash();
    ASSERT(s.hash != h1);

    s.pits[0] = 2;
    s.rehash();
    ASSERT_EQ(s.hash, h1);
}

// ===================================================================
// Test: Hash after make_move matches fresh compute
// ===================================================================

TEST(test_hash_after_move) {
    BaoState s;
    s.init_start();

    Move moves[MAX_MOVES];
    int n = s.generate_moves(moves);

    for (int i = 0; i < n; ++i) {
        BaoState copy = s;
        MoveResult r = copy.make_move(moves[i]);
        if (r == MoveResult::OK)
            ASSERT_EQ(copy.hash, copy.compute_hash());
    }
}

// ===================================================================
// Test: inner_empty detects empty/non-empty correctly
// ===================================================================

TEST(test_inner_empty) {
    uint8_t p[TOTAL_PITS] = {};
    // Side 0: all seeds on outer
    for (int i = 8; i < 16; ++i) p[i] = 4;
    // Side 1: seeds on inner
    for (int i = 0; i < 8; ++i) p[16 + i] = 4;
    BaoState s = make_state(p);

    ASSERT(s.inner_empty(0));   // side 0 inner is empty
    ASSERT(!s.inner_empty(1));  // side 1 inner has seeds
}

// ===================================================================
// Test: Terminal — empty inner row
// ===================================================================

TEST(test_terminal_empty_inner) {
    uint8_t p[TOTAL_PITS] = {};
    for (int i = 8; i < 16; ++i) p[i] = 4;
    for (int i = 0; i < 8; ++i) p[16 + i] = 4;
    BaoState s = make_state(p);
    ASSERT(s.is_terminal());
}

// ===================================================================
// Test: Terminal — no legal moves (all singletons)
// ===================================================================

TEST(test_terminal_no_moves) {
    uint8_t p[TOTAL_PITS] = {};
    p[0] = 1;
    p[16] = 63;
    BaoState s = make_state(p);
    ASSERT(s.is_terminal());
}

// ===================================================================
// Test: Seed conservation over 20 plies
// ===================================================================

TEST(test_seed_conservation_multi) {
    BaoState s;
    s.init_start();

    for (int ply = 0; ply < 20; ++ply) {
        if (s.is_terminal()) break;
        Move moves[MAX_MOVES];
        int n = s.generate_moves(moves);
        if (n == 0) break;
        MoveResult r = s.make_move(moves[0]);
        if (r != MoveResult::OK) break;
        ASSERT_EQ(s.total_seeds(), 64);
    }
}

// ===================================================================
// Test: Relay chains
// ===================================================================

TEST(test_relay_chains) {
    uint8_t p[TOTAL_PITS] = {};
    p[2] = 3;       // inner pit 2
    p[3] = 2;       // inner pit 3 — relay target
    p[16 + 5] = 10; // opponent inner (keeps game alive)
    p[16 + 10] = 49;// opponent outer (fills to 64)
    BaoState s = make_state(p);
    ASSERT_EQ(s.total_seeds(), 64);

    Move moves[MAX_MOVES];
    int n = s.generate_moves(moves);
    ASSERT(n > 0);

    // All should be takasa (opponent inner empty at capture columns)
    for (int i = 0; i < n; ++i)
        ASSERT(!moves[i].is_mtaji);

    // Execute pit 3 ACW (triggers relay through pit 2)
    for (int i = 0; i < n; ++i) {
        if (moves[i].pit == 3 && moves[i].dir == ACW) {
            BaoState copy = s;
            MoveResult r = copy.make_move(moves[i]);
            ASSERT(r == MoveResult::OK);
            ASSERT_EQ(copy.total_seeds(), 64);
            break;
        }
    }
}

// ===================================================================
// Test: >= 16 seeds rule
// ===================================================================

TEST(test_ge16_no_mtaji) {
    uint8_t p[TOTAL_PITS] = {};
    p[3] = 20;
    p[16 + 3] = 5;
    p[16 + 10] = 39;
    BaoState s = make_state(p);
    ASSERT_EQ(s.total_seeds(), 64);

    Move moves[MAX_MOVES];
    int n = s.generate_moves(moves);
    for (int i = 0; i < n; ++i)
        ASSERT(!moves[i].is_mtaji);
}

// ===================================================================
// Test: Capture execution
// ===================================================================

TEST(test_capture_execution) {
    uint8_t p[TOTAL_PITS] = {};
    p[2] = 3;        // inner pit 2
    p[4] = 1;        // inner pit 4 (will have 2 after sow -> non-empty)
    p[16 + 4] = 4;   // opponent pit 4 (capture target)
    p[16 + 0] = 2;   // opponent pit 0 (keeps inner alive)
    p[16 + 10] = 54; // fill to 64
    BaoState s = make_state(p);
    ASSERT_EQ(s.total_seeds(), 64);
    ASSERT(s.first_sow_captures(2, CW));

    Move moves[MAX_MOVES];
    int n = s.generate_moves(moves);

    bool found = false;
    for (int i = 0; i < n; ++i) {
        if (moves[i].pit == 2 && moves[i].dir == CW && moves[i].is_mtaji) {
            BaoState copy = s;
            MoveResult r = copy.make_move(moves[i]);
            ASSERT(r == MoveResult::OK);
            ASSERT_EQ(copy.total_seeds(), 64);
            // After swap: pits[0..15] = old opponent, pits[4] should be 0
            ASSERT_EQ(copy.pits[4], 0);
            found = true;
            break;
        }
    }
    ASSERT(found);
}

// ===================================================================
// Test: Capture at kimbi forces kichwa direction
// ===================================================================

TEST(test_capture_direction_kimbi) {
    uint8_t p[TOTAL_PITS] = {};
    p[0] = 2;        // left kichwa
    p[1] = 1;        // will have 2 after sow
    p[16 + 1] = 3;   // opponent capture target
    p[16 + 5] = 8;   // keeps opponent inner alive after capture
    p[16 + 10] = 50; // fill to 64
    BaoState s = make_state(p);
    ASSERT_EQ(s.total_seeds(), 64);
    ASSERT(s.first_sow_captures(0, CW));

    Move m = {0, CW, true};
    MoveResult r = s.make_move(m);
    ASSERT(r == MoveResult::OK);
    ASSERT_EQ(s.total_seeds(), 64);
}

// ===================================================================
// Test: Play full game
// ===================================================================

TEST(test_play_full_game) {
    BaoState s;
    s.init_start();
    int ply = 0;

    while (ply < 2000) {
        if (s.is_terminal()) break;
        Move moves[MAX_MOVES];
        int n = s.generate_moves(moves);
        if (n == 0) break;
        MoveResult r = s.make_move(moves[0]);
        if (r != MoveResult::OK) break;
        ASSERT_EQ(s.total_seeds(), 64);
        ply++;
    }
    ASSERT(ply > 0);
    printf("(%d plies) ", ply);
}

// ===================================================================
// Test: Canonicalization reduces asymmetric positions
// ===================================================================

TEST(test_canonicalize_asymmetric) {
    BaoState s1;
    s1.init_start();
    s1.pits[0] = 5;
    s1.pits[7] = 10;
    s1.rehash();

    BaoState s2;
    s2.init_start();
    s2.pits[7] = 5;
    s2.pits[0] = 10;
    s2.rehash();

    s1.canonicalize();
    s2.canonicalize();
    ASSERT(s1 == s2);
}

// ===================================================================
// Stress test: 5000 random games, check invariants every move
// ===================================================================

TEST(test_stress_random_games) {
    uint32_t rng = 42;
    auto next = [&]() -> uint32_t {
        rng = rng * 1103515245u + 12345u;
        return (rng >> 16) & 0x7FFF;
    };

    int games = 0, plies = 0;

    for (int g = 0; g < 5000; ++g) {
        BaoState s;
        s.init_start();
        int ply = 0;

        while (ply < 500) {
            if (s.is_terminal()) break;
            Move moves[MAX_MOVES];
            int n = s.generate_moves(moves);
            if (n == 0) break;

            int choice = next() % n;
            BaoState backup = s;
            MoveResult r = s.make_move(moves[choice]);

            if (r == MoveResult::INFINITE) {
                s = backup;
                bool found = false;
                for (int j = 0; j < n; ++j) {
                    if (j == choice) continue;
                    s = backup;
                    r = s.make_move(moves[j]);
                    if (r == MoveResult::OK || r == MoveResult::INNER_ROW_EMPTY) {
                        found = true; break;
                    }
                }
                if (!found) break;
                if (r == MoveResult::INNER_ROW_EMPTY) break;
            }
            if (r == MoveResult::INNER_ROW_EMPTY) break;

            ASSERT_EQ(s.total_seeds(), 64);
            ASSERT_EQ(s.hash, s.compute_hash());
            ply++;
        }
        games++;
        plies += ply;
    }
    printf("(%d games, %d plies) ", games, plies);
}

// ===================================================================
// Test: Batch sow (count >= 16) works correctly
// ===================================================================

TEST(test_batch_sow) {
    // Create a state with a large pit that will use the batch sow path
    uint8_t p[TOTAL_PITS] = {};
    p[3] = 20;      // 20 seeds — will trigger batch sow (>= 16)
    p[16 + 5] = 10;
    p[16 + 10] = 34;
    BaoState s = make_state(p);
    ASSERT_EQ(s.total_seeds(), 64);

    // Manually sow 20 seeds from pit 3, CW:
    // q = 20/16 = 1, r = 20%16 = 4
    // All 16 pits get +1, then first 4 pits (3,4,5,6) get +1 more
    // Landing = (3 + 3) & 15 = 6
    BaoState test = s;
    test.pits[3] = 0; // pick up
    int landing = test.sow(3, 20, CW);
    ASSERT_EQ(landing, 6);
    ASSERT_EQ(test.total_seeds(), 64); // 20 sown + 44 remaining = 64

    // Verify distribution: each pit should have original + 1 (from q),
    // plus 1 more for pits 3,4,5,6 (from r)
    // Pit 3 was 0 (emptied), gets 1+1 = 2
    ASSERT_EQ(test.pits[3], 2);
    // Pit 4 was 0, gets 1+1 = 2
    ASSERT_EQ(test.pits[4], 2);
    // Pit 7 was 0, gets 1 only (not in first 4)
    ASSERT_EQ(test.pits[7], 1);
}

// ===================================================================
// Main
// ===================================================================

int main() {
    zobrist_init();

    printf("Running Bao engine tests (v2 — optimized):\n");
    printf("----------------------------------------------------------\n");

    RUN(test_start_position);
    RUN(test_seed_conservation_one_move);
    RUN(test_start_all_mtaji);
    RUN(test_first_sow_captures);
    RUN(test_reflect_involution);
    RUN(test_start_is_canonical);
    RUN(test_swap_sides);
    RUN(test_zobrist_changes);
    RUN(test_hash_after_move);
    RUN(test_inner_empty);
    RUN(test_terminal_empty_inner);
    RUN(test_terminal_no_moves);
    RUN(test_seed_conservation_multi);
    RUN(test_relay_chains);
    RUN(test_ge16_no_mtaji);
    RUN(test_capture_execution);
    RUN(test_capture_direction_kimbi);
    RUN(test_play_full_game);
    RUN(test_canonicalize_asymmetric);
    RUN(test_stress_random_games);
    RUN(test_batch_sow);

    printf("----------------------------------------------------------\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
