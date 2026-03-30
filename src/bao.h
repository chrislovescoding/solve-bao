#pragma once
/*
 * bao.h — Bao la Kujifunza game engine (v2 — optimized)
 *
 * State representation:
 *   pits[0..15]  = side to move (loop order: inner i1-i8 then outer o8-o1)
 *   pits[16..31] = opponent     (same loop order from their perspective)
 *
 * Loop index layout per side (16 pits):
 *   idx  0  1  2  3  4  5  6  7  |  8  9 10 11 12 13 14 15
 *   pit i1 i2 i3 i4 i5 i6 i7 i8 | o8 o7 o6 o5 o4 o3 o2 o1
 *        ---- inner row -------     ------ outer row -------
 *
 * Clockwise  = index + 1 (mod 16)   [rightward along inner row]
 * Anti-clock = index - 1 (mod 16)   [leftward along inner row]
 *
 * Kichwa: idx 0 (left, CW start) and idx 7 (right, ACW start)
 * Kimbi:  idx 0,1 (left) and idx 6,7 (right)
 *
 * Design principles:
 *   - sow() is bare metal: just pits[idx]++. No hash, no bitmask.
 *   - Hash computed once after move completes (32 lookups, always).
 *   - Bitmasks computed on demand in generate_moves().
 *   - inner_empty() uses uint64 load — one compare, no bitmask.
 */

#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int PITS_PER_SIDE  = 16;
static constexpr int INNER_PITS     = 8;
static constexpr int TOTAL_PITS     = 32;
static constexpr int TOTAL_SEEDS    = 64;
static constexpr int MAX_SOW_THRESHOLD = 800;

static constexpr int CW  =  1;
static constexpr int ACW = -1;

static constexpr int KICHWA_L = 0;
static constexpr int KICHWA_R = 7;

static constexpr int REFLECT[16] = {
    7, 6, 5, 4, 3, 2, 1, 0,
    15, 14, 13, 12, 11, 10, 9, 8
};

// ---------------------------------------------------------------------------
// Zobrist hashing
// ---------------------------------------------------------------------------

extern uint64_t ZOBRIST_TABLE[TOTAL_PITS][TOTAL_SEEDS + 1];
void zobrist_init(uint64_t seed = 0x12345678ABCDEF01ULL);

// ---------------------------------------------------------------------------
// Move
// ---------------------------------------------------------------------------

struct Move {
    uint8_t pit;      // 0..15
    int8_t  dir;      // +1 (CW) or -1 (ACW)
    bool    is_mtaji;
};

static constexpr int MAX_MOVES = 32;

enum class MoveResult : uint8_t {
    OK,
    INFINITE,
    INNER_ROW_EMPTY
};

// ---------------------------------------------------------------------------
// BaoState
// ---------------------------------------------------------------------------

struct BaoState {
    // ---- Core state (identity) ----
    uint8_t pits[TOTAL_PITS]; // [0..15] to-move, [16..31] opponent

    // ---- Cached hash (recomputed after moves, not maintained per-seed) ----
    uint64_t hash;

    // ---- Construction ----
    void init_start();

    // ---- Pit access ----
    uint8_t*       side(int s)       { return &pits[s * PITS_PER_SIDE]; }
    const uint8_t* side(int s) const { return &pits[s * PITS_PER_SIDE]; }

    // ---- Inner row empty check (uint64 trick — one load, one compare) ----
    inline bool inner_empty(int s) const {
        uint64_t v;
        memcpy(&v, &pits[s * PITS_PER_SIDE], 8);
        return v == 0;
    }

    // ---- Seed invariant ----
    int total_seeds() const;

    // ---- Symmetry ----
    void swap_sides();
    void reflect_lr();
    bool canonicalize();

    // ---- Comparison ----
    bool operator==(const BaoState& o) const { return memcmp(pits, o.pits, TOTAL_PITS) == 0; }
    bool operator<(const BaoState& o) const  { return memcmp(pits, o.pits, TOTAL_PITS) < 0; }

    // ---- Hashing ----
    uint64_t compute_hash() const;
    void rehash() { hash = compute_hash(); }

    // Fused canonicalize + hash: computes both original and reflected hashes
    // in a single pass (32 Zobrist lookups, not 64). Applies reflection only
    // if needed. Returns the canonical hash. ~2x faster than separate
    // canonicalize + rehash.
    uint64_t canonicalize_and_hash();

    // ---- Move generation ----
    int generate_moves(Move out[MAX_MOVES]) const;

    // ---- Move execution ----
    MoveResult make_move(const Move& m);

    // ---- Terminal detection ----
    bool is_terminal() const;

    // ---- Debug ----
    void print() const;

    // ---- Engine internals (public for testing) ----

    // Sow count seeds from start_idx in direction dir on side 0.
    // Pure pit manipulation — no hash/bitmask updates.
    // Returns landing pit index.
    int sow(int start_idx, int count, int dir);

    // O(1) check: does first sow from (pit, dir) produce a capture?
    bool first_sow_captures(int pit, int dir) const;
};
