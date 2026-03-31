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
 *   - Hot functions defined inline for guaranteed inlining.
 */

#include <cstdint>
#include <cstring>
#include <immintrin.h>

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
// Sow lookup tables — precomputed SIMD masks for small-count sowing
// ---------------------------------------------------------------------------

// Precomputed sow data: for each (dir, start, count) store 16-byte increment mask + landing
// dir_idx 0 = CW (step=1), dir_idx 1 = ACW (step=15)
struct alignas(32) SowEntry {
    alignas(16) uint8_t mask[16];  // increment mask (1 at each pit receiving a seed)
    uint8_t landing;               // landing pit index
    uint8_t pad[15];               // pad to 32 bytes for alignment
};

// SOW_TABLE[dir_idx][start][count]
extern SowEntry SOW_TABLE[2][16][16];
void sow_table_init();

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
    inline void swap_sides() {
        uint64_t *a = (uint64_t*)&pits[0];
        uint64_t *b = (uint64_t*)&pits[PITS_PER_SIDE];
        uint64_t t0 = a[0]; a[0] = b[0]; b[0] = t0;
        uint64_t t1 = a[1]; a[1] = b[1]; b[1] = t1;
    }
    void reflect_lr();
    bool canonicalize();

    // ---- Comparison ----
    bool operator==(const BaoState& o) const { return memcmp(pits, o.pits, TOTAL_PITS) == 0; }
    bool operator<(const BaoState& o) const  { return memcmp(pits, o.pits, TOTAL_PITS) < 0; }

    // ---- Hashing ----
    uint64_t compute_hash() const;
    void rehash() { hash = compute_hash(); }

    // Fused canonicalize + hash: computes both original and reflected hashes
    // in a single pass. Applies reflection only if needed.
    uint64_t canonicalize_and_hash();

    // Compute canonical hash WITHOUT modifying state. For enumeration only —
    // the state stays in its original (possibly non-canonical) orientation.
    // Both orientations generate the same set of canonical successor hashes,
    // so this is safe for counting reachable states.
    __attribute__((hot)) inline uint64_t canonical_hash_only() const {
        const uint8_t* __restrict__ p = pits;
        // Interleaved computation: access same table row for orig+refl
        // to maximize cache line reuse
        uint64_t ho = 0, hr = 0;

        // Side 0: inner row (positions 0-7, reflected 7-0)
        ho ^= ZOBRIST_TABLE[ 0][p[ 0]]; hr ^= ZOBRIST_TABLE[ 0][p[ 7]];
        ho ^= ZOBRIST_TABLE[ 1][p[ 1]]; hr ^= ZOBRIST_TABLE[ 1][p[ 6]];
        ho ^= ZOBRIST_TABLE[ 2][p[ 2]]; hr ^= ZOBRIST_TABLE[ 2][p[ 5]];
        ho ^= ZOBRIST_TABLE[ 3][p[ 3]]; hr ^= ZOBRIST_TABLE[ 3][p[ 4]];
        ho ^= ZOBRIST_TABLE[ 4][p[ 4]]; hr ^= ZOBRIST_TABLE[ 4][p[ 3]];
        ho ^= ZOBRIST_TABLE[ 5][p[ 5]]; hr ^= ZOBRIST_TABLE[ 5][p[ 2]];
        ho ^= ZOBRIST_TABLE[ 6][p[ 6]]; hr ^= ZOBRIST_TABLE[ 6][p[ 1]];
        ho ^= ZOBRIST_TABLE[ 7][p[ 7]]; hr ^= ZOBRIST_TABLE[ 7][p[ 0]];
        // Side 0: outer row (positions 8-15, reflected 15-8)
        ho ^= ZOBRIST_TABLE[ 8][p[ 8]]; hr ^= ZOBRIST_TABLE[ 8][p[15]];
        ho ^= ZOBRIST_TABLE[ 9][p[ 9]]; hr ^= ZOBRIST_TABLE[ 9][p[14]];
        ho ^= ZOBRIST_TABLE[10][p[10]]; hr ^= ZOBRIST_TABLE[10][p[13]];
        ho ^= ZOBRIST_TABLE[11][p[11]]; hr ^= ZOBRIST_TABLE[11][p[12]];
        ho ^= ZOBRIST_TABLE[12][p[12]]; hr ^= ZOBRIST_TABLE[12][p[11]];
        ho ^= ZOBRIST_TABLE[13][p[13]]; hr ^= ZOBRIST_TABLE[13][p[10]];
        ho ^= ZOBRIST_TABLE[14][p[14]]; hr ^= ZOBRIST_TABLE[14][p[ 9]];
        ho ^= ZOBRIST_TABLE[15][p[15]]; hr ^= ZOBRIST_TABLE[15][p[ 8]];
        // Side 1: inner row (positions 16-23, reflected 23-16)
        ho ^= ZOBRIST_TABLE[16][p[16]]; hr ^= ZOBRIST_TABLE[16][p[23]];
        ho ^= ZOBRIST_TABLE[17][p[17]]; hr ^= ZOBRIST_TABLE[17][p[22]];
        ho ^= ZOBRIST_TABLE[18][p[18]]; hr ^= ZOBRIST_TABLE[18][p[21]];
        ho ^= ZOBRIST_TABLE[19][p[19]]; hr ^= ZOBRIST_TABLE[19][p[20]];
        ho ^= ZOBRIST_TABLE[20][p[20]]; hr ^= ZOBRIST_TABLE[20][p[19]];
        ho ^= ZOBRIST_TABLE[21][p[21]]; hr ^= ZOBRIST_TABLE[21][p[18]];
        ho ^= ZOBRIST_TABLE[22][p[22]]; hr ^= ZOBRIST_TABLE[22][p[17]];
        ho ^= ZOBRIST_TABLE[23][p[23]]; hr ^= ZOBRIST_TABLE[23][p[16]];
        // Side 1: outer row (positions 24-31, reflected 31-24)
        ho ^= ZOBRIST_TABLE[24][p[24]]; hr ^= ZOBRIST_TABLE[24][p[31]];
        ho ^= ZOBRIST_TABLE[25][p[25]]; hr ^= ZOBRIST_TABLE[25][p[30]];
        ho ^= ZOBRIST_TABLE[26][p[26]]; hr ^= ZOBRIST_TABLE[26][p[29]];
        ho ^= ZOBRIST_TABLE[27][p[27]]; hr ^= ZOBRIST_TABLE[27][p[28]];
        ho ^= ZOBRIST_TABLE[28][p[28]]; hr ^= ZOBRIST_TABLE[28][p[27]];
        ho ^= ZOBRIST_TABLE[29][p[29]]; hr ^= ZOBRIST_TABLE[29][p[26]];
        ho ^= ZOBRIST_TABLE[30][p[30]]; hr ^= ZOBRIST_TABLE[30][p[25]];
        ho ^= ZOBRIST_TABLE[31][p[31]]; hr ^= ZOBRIST_TABLE[31][p[24]];

        return ho <= hr ? ho : hr;
    }

    // ---- Move generation (inline) ----
    __attribute__((hot)) inline int generate_moves(Move out[MAX_MOVES]) const {
        const uint8_t* p = &pits[0];
        int n_mtaji = 0;

        // Phase 1: find all mtaji (capturing) moves
        if (p[0] >= 2 && p[0] < 16 && first_sow_captures(0, CW))
            out[n_mtaji++] = {0, CW, true};
        for (int pit = 1; pit < 7; ++pit) {
            uint8_t seeds = p[pit];
            if (seeds < 2 || seeds >= 16) continue;
            if (first_sow_captures(pit, CW))
                out[n_mtaji++] = {(uint8_t)pit, CW, true};
            if (first_sow_captures(pit, ACW))
                out[n_mtaji++] = {(uint8_t)pit, ACW, true};
        }
        if (p[7] >= 2 && p[7] < 16 && first_sow_captures(7, ACW))
            out[n_mtaji++] = {7, ACW, true};
        for (int pit = 8; pit < 16; ++pit) {
            uint8_t seeds = p[pit];
            if (seeds < 2 || seeds >= 16) continue;
            if (first_sow_captures(pit, CW))
                out[n_mtaji++] = {(uint8_t)pit, CW, true};
            if (first_sow_captures(pit, ACW))
                out[n_mtaji++] = {(uint8_t)pit, ACW, true};
        }
        if (n_mtaji > 0) return n_mtaji;

        // Phase 2: takasa moves
        uint8_t inner_ge2 = 0;
        for (int i = 0; i < INNER_PITS; ++i)
            if (p[i] >= 2) inner_ge2 |= (1u << i);
        int n_takasa = 0;
        if (inner_ge2 != 0) {
            uint8_t bits = inner_ge2;
            while (bits) {
                int pit = __builtin_ctz(bits);
                bits &= bits - 1;
                if (pit == KICHWA_L) {
                    out[n_takasa++] = {(uint8_t)pit, CW, false};
                } else if (pit == KICHWA_R) {
                    out[n_takasa++] = {(uint8_t)pit, ACW, false};
                } else {
                    out[n_takasa++] = {(uint8_t)pit, CW, false};
                    out[n_takasa++] = {(uint8_t)pit, ACW, false};
                }
            }
        } else {
            for (int pit = INNER_PITS; pit < PITS_PER_SIDE; ++pit) {
                if (p[pit] < 2) continue;
                out[n_takasa++] = {(uint8_t)pit, CW, false};
                out[n_takasa++] = {(uint8_t)pit, ACW, false};
            }
        }
        return n_takasa;
    }

    // ---- Sow (public API, backward compat) ----
    __attribute__((hot)) inline int sow(int start_idx, int count, int dir) {
        int dir_idx = (dir == CW) ? 0 : 1;
        return sow_fast(start_idx, count, dir_idx);
    }

    // ---- Sow fast path (inline) — dir_idx precomputed by caller ----
    // dir_idx: 0=CW, 1=ACW
    __attribute__((hot)) inline int sow_fast(int start_idx, int count, int dir_idx) {
        uint8_t* __restrict__ p = &pits[0];

        if (count < PITS_PER_SIDE) {
            const SowEntry& e = SOW_TABLE[dir_idx][start_idx][count];
            __m128i v    = _mm_loadu_si128((__m128i*)p);
            __m128i mask = _mm_load_si128((__m128i*)e.mask);
            _mm_storeu_si128((__m128i*)p, _mm_add_epi8(v, mask));
            return e.landing;
        }
        int q = count >> 4;
        int r = count & 15;
        __m128i v  = _mm_loadu_si128((__m128i*)p);
        __m128i vq = _mm_set1_epi8((char)q);
        if (r == 0) {
            _mm_storeu_si128((__m128i*)p, _mm_add_epi8(v, vq));
            return (start_idx + (dir_idx == 0 ? 15 : 1)) & 15;
        }
        const SowEntry& e = SOW_TABLE[dir_idx][start_idx][r];
        __m128i mask = _mm_load_si128((__m128i*)e.mask);
        _mm_storeu_si128((__m128i*)p, _mm_add_epi8(_mm_add_epi8(v, vq), mask));
        return e.landing;
    }

    // ---- Move execution (inline, templated on capturing) ----
    template<bool IS_CAPTURING>
    __attribute__((hot)) inline MoveResult make_move_impl(const Move& m) {
        int dir_idx = (m.dir == CW) ? 0 : 1;
        int step = dir_idx == 0 ? 1 : 15; // precomputed for sow_start update
        int sow_start = m.pit;
        int total_sown = 0;

        int seeds = pits[sow_start];
        pits[sow_start] = 0;

        for (;;) {
            int landing = sow_fast(sow_start, seeds, dir_idx);
            total_sown += seeds;

            if (__builtin_expect(total_sown > MAX_SOW_THRESHOLD, 0))
                return MoveResult::INFINITE;

            uint8_t landing_count = pits[landing];

            if (__builtin_expect(landing_count == 1, 0)) {
                if (__builtin_expect(inner_empty(0), 0))
                    return MoveResult::INNER_ROW_EMPTY;
                break;
            }

            if (__builtin_expect(inner_empty(0), 0))
                return MoveResult::INNER_ROW_EMPTY;

            if constexpr (IS_CAPTURING) {
                if (landing < INNER_PITS) {
                    int opp_idx = PITS_PER_SIDE + landing;
                    if (pits[opp_idx] > 0) {
                        seeds = pits[opp_idx];
                        pits[opp_idx] = 0;
                        if (inner_empty(1))
                            return MoveResult::INNER_ROW_EMPTY;
                        if (landing <= 1) {
                            sow_start = KICHWA_L;
                            step = 1;
                            dir_idx = 0;
                        } else if (landing >= 6) {
                            sow_start = KICHWA_R;
                            step = 15;
                            dir_idx = 1;
                        } else {
                            sow_start = (dir_idx == 0) ? KICHWA_L : KICHWA_R;
                        }
                        continue;
                    }
                }
            }

            seeds = landing_count;
            pits[landing] = 0;
            sow_start = (landing + step) & 15;
        }

        swap_sides();
        return MoveResult::OK;
    }

    __attribute__((hot)) inline MoveResult make_move(const Move& m) {
        if (m.is_mtaji)
            return make_move_impl<true>(m);
        else
            return make_move_impl<false>(m);
    }

    // ---- Terminal detection (inline) ----
    inline bool is_terminal() const {
        if (inner_empty(0)) return true;
        // Check if any of the 16 pits of side 0 has value >= 2.
        // A byte is >= 2 iff any bit other than bit 0 is set, i.e. (byte & 0xFE) != 0.
        uint64_t v0, v1;
        memcpy(&v0, &pits[0], 8);
        memcpy(&v1, &pits[8], 8);
        return ((v0 | v1) & 0xFEFEFEFEFEFEFEFEULL) == 0;
    }

    // ---- Debug ----
    void print() const;

    // ---- O(1) capture check (inline) ----
    inline bool first_sow_captures(int pit, int dir) const {
        int count = pits[pit];
        int landing = (pit + (count - 1) * dir + PITS_PER_SIDE * 16) & 15;
        if (landing >= INNER_PITS) return false;
        if (pits[landing] < 1) return false;
        return pits[PITS_PER_SIDE + landing] > 0;
    }
};
