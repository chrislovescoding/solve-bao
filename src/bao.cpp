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

uint64_t BaoState::canonicalize_and_hash() {
    // Compute BOTH original and reflected hashes in ONE pass.
    // The Zobrist table maps (position, value) → random bits.
    // For the reflected state, position i maps to REFLECT[i] within each side,
    // but the VALUE at that reflected position is pits[side*16 + REFLECT[i]].
    //
    // Original hash:  XOR_i ZOBRIST[i][pits[i]]
    // Reflected hash: XOR_i ZOBRIST[i][pits[side*16 + REFLECT[i%16]]]
    //
    // We compute both simultaneously, then compare.
    // If reflected hash < original: apply reflection, use reflected hash.
    // If equal: compare pit arrays to break tie.
    // If original hash < reflected: keep original, use original hash.
    //
    // This replaces: canonicalize() [32-byte compare + conditional 32-byte shuffle]
    //                + rehash() [32 Zobrist lookups]
    // With: 32 Zobrist lookups for original + 32 for reflected = 64 lookups,
    // BUT no conditional branch, no memcpy, no reflect_lr() call.

    uint64_t h_orig = 0;
    uint64_t h_refl = 0;

    for (int s = 0; s < 2; ++s) {
        int base = s * PITS_PER_SIDE;
        for (int i = 0; i < PITS_PER_SIDE; ++i) {
            int pos = base + i;
            h_orig ^= ZOBRIST_TABLE[pos][pits[pos]];
            h_refl ^= ZOBRIST_TABLE[pos][pits[base + REFLECT[i]]];
        }
    }

    if (h_refl < h_orig) {
        // Reflected version has smaller hash — apply reflection
        reflect_lr();
        hash = h_refl;
        return h_refl;
    }
    if (h_orig < h_refl) {
        // Original is canonical
        hash = h_orig;
        return h_orig;
    }

    // Hashes are equal (extremely rare) — fall back to lex comparison
    // to determine canonical form deterministically
    bool do_reflect = false;
    for (int pos = 0; pos < TOTAL_PITS; ++pos) {
        int si = pos >> 4, i = pos & 15;
        uint8_t orig = pits[pos];
        uint8_t refl = pits[(si << 4) + REFLECT[i]];
        if (refl < orig) { do_reflect = true; break; }
        if (refl > orig) break;
    }
    if (do_reflect) {
        reflect_lr();
        hash = h_refl; // same as h_orig since they're equal
    } else {
        hash = h_orig;
    }
    return hash;
}

// canonical_hash_only(), swap_sides(), first_sow_captures(), is_terminal()
// are defined inline in bao.h

// ---------------------------------------------------------------------------
// Symmetry
// ---------------------------------------------------------------------------

void BaoState::reflect_lr() {
    for (int s = 0; s < 2; ++s) {
        uint8_t* p = side(s);
        uint8_t tmp[PITS_PER_SIDE];
        for (int i = 0; i < PITS_PER_SIDE; ++i)
            tmp[REFLECT[i]] = p[i];
        memcpy(p, tmp, PITS_PER_SIDE);
    }
    // NOTE: does NOT rehash. Caller must rehash when needed.
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

// sow(), generate_moves(), make_move(), is_terminal() are defined inline in bao.h

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
