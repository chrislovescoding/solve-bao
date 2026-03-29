#include "bao.h"
#include <cstdio>
#include <algorithm>

// ---------------------------------------------------------------------------
// Zobrist table
// ---------------------------------------------------------------------------

uint64_t ZOBRIST_TABLE[TOTAL_PITS][TOTAL_SEEDS + 1];

static uint64_t xorshift64(uint64_t& s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

void zobrist_init(uint64_t seed) {
    uint64_t s = seed;
    for (int p = 0; p < TOTAL_PITS; ++p)
        for (int c = 0; c <= TOTAL_SEEDS; ++c)
            ZOBRIST_TABLE[p][c] = xorshift64(s);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void BaoState::init_start() {
    for (int i = 0; i < TOTAL_PITS; ++i)
        pits[i] = 2;
    rehash();
}

int BaoState::total_seeds() const {
    int sum = 0;
    for (int i = 0; i < TOTAL_PITS; ++i)
        sum += pits[i];
    return sum;
}

// ---------------------------------------------------------------------------
// Hashing — computed once, not per-seed
// ---------------------------------------------------------------------------

uint64_t BaoState::compute_hash() const {
    uint64_t h = 0;
    for (int i = 0; i < TOTAL_PITS; ++i)
        h ^= ZOBRIST_TABLE[i][pits[i]];
    return h;
}

// ---------------------------------------------------------------------------
// Symmetry
// ---------------------------------------------------------------------------

void BaoState::swap_sides() {
    uint8_t tmp[PITS_PER_SIDE];
    memcpy(tmp,                   &pits[0],             PITS_PER_SIDE);
    memcpy(&pits[0],              &pits[PITS_PER_SIDE], PITS_PER_SIDE);
    memcpy(&pits[PITS_PER_SIDE],  tmp,                  PITS_PER_SIDE);
    rehash();
}

void BaoState::reflect_lr() {
    for (int s = 0; s < 2; ++s) {
        uint8_t* p = side(s);
        uint8_t tmp[PITS_PER_SIDE];
        for (int i = 0; i < PITS_PER_SIDE; ++i)
            tmp[REFLECT[i]] = p[i];
        memcpy(p, tmp, PITS_PER_SIDE);
    }
    rehash();
}

bool BaoState::canonicalize() {
    BaoState reflected = *this;
    reflected.reflect_lr();
    if (reflected < *this) {
        *this = reflected;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Sowing — bare metal, no hash/bitmask overhead
// ---------------------------------------------------------------------------

int BaoState::sow(int start_idx, int count, int dir) {
    uint8_t* p = &pits[0]; // side 0

    if (count < PITS_PER_SIDE) {
        // Common path: small count, simple loop
        int idx = start_idx;
        for (int i = 0; i < count; ++i) {
            p[idx]++;
            if (i < count - 1)
                idx = (idx + dir + PITS_PER_SIDE) & 15;
        }
        return idx;
    }

    // Large count: batch — add q to all pits, then 1 to first r pits
    int q = count >> 4;   // count / 16
    int r = count & 15;   // count % 16

    for (int i = 0; i < PITS_PER_SIDE; ++i)
        p[i] += (uint8_t)q;

    if (r == 0)
        return (start_idx + 15 * dir + PITS_PER_SIDE * 16) & 15;

    int idx = start_idx;
    for (int i = 0; i < r; ++i) {
        p[idx]++;
        if (i < r - 1)
            idx = (idx + dir + PITS_PER_SIDE) & 15;
    }
    return idx;
}

// ---------------------------------------------------------------------------
// First-sow capture check — O(1), no simulation
// ---------------------------------------------------------------------------

bool BaoState::first_sow_captures(int pit, int dir) const {
    int count = pits[pit];
    int landing = (pit + (count - 1) * dir + PITS_PER_SIDE * 16) & 15;

    if (landing >= INNER_PITS) return false;
    if (pits[landing] < 1) return false;
    return pits[PITS_PER_SIDE + landing] > 0;
}

// ---------------------------------------------------------------------------
// Move generation — bitmasks computed on demand
// ---------------------------------------------------------------------------

int BaoState::generate_moves(Move out[MAX_MOVES]) const {
    const uint8_t* p = &pits[0];
    int n_mtaji = 0;

    // Phase 1: find all mtaji (capturing) moves
    for (int pit = 0; pit < PITS_PER_SIDE; ++pit) {
        uint8_t seeds = p[pit];
        if (seeds < 2 || seeds >= 16) continue;

        for (int d : {CW, ACW}) {
            if (pit == KICHWA_L && d != CW) continue;
            if (pit == KICHWA_R && d != ACW) continue;

            if (first_sow_captures(pit, d)) {
                out[n_mtaji++] = {(uint8_t)pit, (int8_t)d, true};
            }
        }
    }

    if (n_mtaji > 0)
        return n_mtaji;

    // Phase 2: takasa moves
    // Compute inner_ge2 on demand
    uint8_t inner_ge2 = 0;
    for (int i = 0; i < INNER_PITS; ++i)
        if (p[i] >= 2) inner_ge2 |= (1u << i);

    int n_takasa = 0;
    bool use_inner = (inner_ge2 != 0);

    if (use_inner) {
        // Generate from inner row pits with >= 2 seeds
        int only_one = (__builtin_popcount(inner_ge2) == 1);
        uint8_t bits = inner_ge2;
        while (bits) {
            int pit = __builtin_ctz(bits);
            bits &= bits - 1;

            for (int d : {CW, ACW}) {
                if (pit == KICHWA_L && d != CW) continue;
                if (pit == KICHWA_R && d != ACW) continue;

                // Rule 3.5b: only inner pit is a kichwa -> can't go toward outer
                if (only_one) {
                    if (pit == KICHWA_L && d == ACW) continue;
                    if (pit == KICHWA_R && d == CW) continue;
                }

                out[n_takasa++] = {(uint8_t)pit, (int8_t)d, false};
            }
        }
    } else {
        // No inner pit with >= 2; use outer row
        for (int pit = INNER_PITS; pit < PITS_PER_SIDE; ++pit) {
            if (p[pit] < 2) continue;
            for (int d : {CW, ACW}) {
                out[n_takasa++] = {(uint8_t)pit, (int8_t)d, false};
            }
        }
    }

    return n_takasa;
}

// ---------------------------------------------------------------------------
// Move execution — hash computed once at end
// ---------------------------------------------------------------------------

MoveResult BaoState::make_move(const Move& m) {
    int current_dir = m.dir;
    int sow_start = m.pit;
    int total_sown = 0;
    bool is_capturing_move = m.is_mtaji;

    // Pick up seeds
    int seeds = pits[sow_start];
    pits[sow_start] = 0;

    for (;;) {
        int landing = sow(sow_start, seeds, current_dir);
        total_sown += seeds;

        if (total_sown > MAX_SOW_THRESHOLD)
            return MoveResult::INFINITE;

        if (inner_empty(1)) return MoveResult::INNER_ROW_EMPTY;
        if (inner_empty(0)) return MoveResult::INNER_ROW_EMPTY;

        uint8_t landing_count = pits[landing];

        // Empty landing (was empty before our seed) — move ends
        if (landing_count == 1)
            break;

        // Non-empty landing — check capture then relay
        if (is_capturing_move && landing < INNER_PITS) {
            int opp_idx = PITS_PER_SIDE + landing;
            if (pits[opp_idx] > 0) {
                // CAPTURE
                seeds = pits[opp_idx];
                pits[opp_idx] = 0;

                if (inner_empty(1))
                    return MoveResult::INNER_ROW_EMPTY;

                // Determine kichwa and direction for re-sow
                if (landing <= 1) {
                    sow_start = KICHWA_L;
                    current_dir = CW;
                } else if (landing >= 6) {
                    sow_start = KICHWA_R;
                    current_dir = ACW;
                } else {
                    sow_start = (current_dir == CW) ? KICHWA_L : KICHWA_R;
                }
                continue;
            }
        }

        // Relay (endelea): pick up from landing, sow from NEXT pit
        seeds = pits[landing];
        pits[landing] = 0;
        sow_start = (landing + current_dir + PITS_PER_SIDE) & 15;
    }

    // Move done. Switch sides, recompute hash once.
    swap_sides(); // swap_sides() calls rehash()
    return MoveResult::OK;
}

// ---------------------------------------------------------------------------
// Terminal detection
// ---------------------------------------------------------------------------

bool BaoState::is_terminal() const {
    if (inner_empty(0)) return true;

    // Check if any pit on side 0 has >= 2 seeds
    const uint8_t* p = &pits[0];
    for (int i = 0; i < PITS_PER_SIDE; ++i)
        if (p[i] >= 2) return false;

    return true; // no legal moves
}

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------

void BaoState::print() const {
    const uint8_t* opp = side(1);
    const uint8_t* me  = side(0);

    printf("  Opp outer: ");
    for (int i = 15; i >= 8; --i) printf("%3d", opp[i]);
    printf("\n  Opp inner: ");
    for (int i = 0; i < 8; ++i) printf("%3d", opp[i]);
    printf("\n  Me  inner: ");
    for (int i = 0; i < 8; ++i) printf("%3d", me[i]);
    printf("\n  Me  outer: ");
    for (int i = 15; i >= 8; --i) printf("%3d", me[i]);
    printf("\n  Seeds: %d  Hash: %016llx\n", total_seeds(), (unsigned long long)hash);
}
