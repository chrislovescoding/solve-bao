#pragma once
/*
 * bao_gpu.cuh — CUDA port of Bao la Kujifunza game engine
 *
 * Scalar replacement of all SSE intrinsics from bao.h.
 * Every __device__ function here must produce bit-identical results
 * to its CPU counterpart — verified by benchmark_gpu.cu.
 */

#include <cstdint>
#include <cstring>
#include <cuda_runtime.h>

// ---------------------------------------------------------------------------
// Constants (duplicated for device code — same values as bao.h)
// ---------------------------------------------------------------------------

#define GPU_PITS_PER_SIDE  16
#define GPU_INNER_PITS     8
#define GPU_TOTAL_PITS     32
#define GPU_TOTAL_SEEDS    64
#define GPU_MAX_SOW        800
#define GPU_MAX_MOVES      32

#define GPU_CW   1
#define GPU_ACW -1
#define GPU_KICHWA_L 0
#define GPU_KICHWA_R 7

// ---------------------------------------------------------------------------
// GPU SowEntry — same layout as CPU, 32 bytes
// ---------------------------------------------------------------------------

struct GpuSowEntry {
    uint8_t mask[16];
    uint8_t landing;
    uint8_t pad[15];
};

// ---------------------------------------------------------------------------
// GPU Move — POD, no bool (use uint8_t for CUDA compatibility)
// ---------------------------------------------------------------------------

struct GpuMove {
    uint8_t pit;
    int8_t  dir;
    uint8_t is_mtaji;
};

// MoveResult values
#define GPU_MOVE_OK              0
#define GPU_MOVE_INFINITE        1
#define GPU_MOVE_INNER_ROW_EMPTY 2

// ---------------------------------------------------------------------------
// Constant memory: tables copied from CPU at startup
// ---------------------------------------------------------------------------

__constant__ GpuSowEntry d_SOW_TABLE[2][16][16];          // 16 KB
__constant__ uint64_t    d_ZOBRIST_TABLE[GPU_TOTAL_PITS][GPU_TOTAL_SEEDS + 1];  // ~16.25 KB

// ---------------------------------------------------------------------------
// Host-side init: copy tables from CPU globals to GPU constant memory
// ---------------------------------------------------------------------------

// Forward-declare CPU globals (defined in bao.cpp)
struct SowEntry;
extern SowEntry SOW_TABLE[2][16][16];
extern uint64_t ZOBRIST_TABLE[32][65];

inline void gpu_tables_init() {
    // SOW_TABLE and GpuSowEntry have identical memory layout (both 32-byte aligned)
    static_assert(sizeof(GpuSowEntry) == 32, "GpuSowEntry must be 32 bytes");
    cudaMemcpyToSymbol(d_SOW_TABLE, SOW_TABLE, sizeof(d_SOW_TABLE));
    cudaMemcpyToSymbol(d_ZOBRIST_TABLE, ZOBRIST_TABLE, sizeof(d_ZOBRIST_TABLE));
}

// ---------------------------------------------------------------------------
// Device functions
// ---------------------------------------------------------------------------

__device__ __forceinline__ bool gpu_inner_empty(const uint8_t* pits, int side) {
    uint64_t v;
    memcpy(&v, &pits[side * 16], 8);
    return v == 0;
}

__device__ __forceinline__ bool gpu_is_terminal(const uint8_t* pits) {
    if (gpu_inner_empty(pits, 0)) return true;
    uint64_t v0, v1;
    memcpy(&v0, &pits[0], 8);
    memcpy(&v1, &pits[8], 8);
    return ((v0 | v1) & 0xFEFEFEFEFEFEFEFEULL) == 0;
}

__device__ __forceinline__ void gpu_swap_sides(uint8_t* pits) {
    uint64_t* a = (uint64_t*)&pits[0];
    uint64_t* b = (uint64_t*)&pits[16];
    uint64_t t0 = a[0]; a[0] = b[0]; b[0] = t0;
    uint64_t t1 = a[1]; a[1] = b[1]; b[1] = t1;
}

__device__ __forceinline__ bool gpu_first_sow_captures(const uint8_t* pits, int pit, int dir) {
    int count = pits[pit];
    int landing = (pit + (count - 1) * dir + GPU_PITS_PER_SIDE * 16) & 15;
    if (landing >= GPU_INNER_PITS) return false;
    if (pits[landing] < 1) return false;
    return pits[GPU_PITS_PER_SIDE + landing] > 0;
}

// ---------------------------------------------------------------------------
// Sow — scalar replacement of SSE _mm_add_epi8
// ---------------------------------------------------------------------------

__device__ __forceinline__ int gpu_sow(uint8_t* pits, int start_idx, int count, int dir_idx) {
    if (count < GPU_PITS_PER_SIDE) {
        const GpuSowEntry& e = d_SOW_TABLE[dir_idx][start_idx][count];
        #pragma unroll
        for (int j = 0; j < 16; j++)
            pits[j] += e.mask[j];
        return e.landing;
    }

    int q = count >> 4;
    int r = count & 15;

    if (r == 0) {
        #pragma unroll
        for (int j = 0; j < 16; j++)
            pits[j] += (uint8_t)q;
        return (start_idx + (dir_idx == 0 ? 15 : 1)) & 15;
    }

    const GpuSowEntry& e = d_SOW_TABLE[dir_idx][start_idx][r];
    #pragma unroll
    for (int j = 0; j < 16; j++)
        pits[j] += (uint8_t)q + e.mask[j];
    return e.landing;
}

// ---------------------------------------------------------------------------
// Move generation
// ---------------------------------------------------------------------------

__device__ int gpu_generate_moves(const uint8_t* pits, GpuMove* out) {
    int n_mtaji = 0;

    // Phase 1: mtaji (capturing) moves
    if (pits[0] >= 2 && pits[0] < 16 && gpu_first_sow_captures(pits, 0, GPU_CW))
        out[n_mtaji++] = {0, GPU_CW, 1};
    for (int pit = 1; pit < 7; ++pit) {
        uint8_t seeds = pits[pit];
        if (seeds < 2 || seeds >= 16) continue;
        if (gpu_first_sow_captures(pits, pit, GPU_CW))
            out[n_mtaji++] = {(uint8_t)pit, GPU_CW, 1};
        if (gpu_first_sow_captures(pits, pit, GPU_ACW))
            out[n_mtaji++] = {(uint8_t)pit, GPU_ACW, 1};
    }
    if (pits[7] >= 2 && pits[7] < 16 && gpu_first_sow_captures(pits, 7, GPU_ACW))
        out[n_mtaji++] = {7, GPU_ACW, 1};
    for (int pit = 8; pit < 16; ++pit) {
        uint8_t seeds = pits[pit];
        if (seeds < 2 || seeds >= 16) continue;
        if (gpu_first_sow_captures(pits, pit, GPU_CW))
            out[n_mtaji++] = {(uint8_t)pit, GPU_CW, 1};
        if (gpu_first_sow_captures(pits, pit, GPU_ACW))
            out[n_mtaji++] = {(uint8_t)pit, GPU_ACW, 1};
    }
    if (n_mtaji > 0) return n_mtaji;

    // Phase 2: takasa moves
    uint8_t inner_ge2 = 0;
    for (int i = 0; i < GPU_INNER_PITS; ++i)
        if (pits[i] >= 2) inner_ge2 |= (1u << i);

    int n_takasa = 0;
    if (inner_ge2 != 0) {
        uint8_t bits = inner_ge2;
        while (bits) {
            int pit = __ffs(bits) - 1;  // CUDA equivalent of __builtin_ctz
            bits &= bits - 1;
            if (pit == GPU_KICHWA_L) {
                out[n_takasa++] = {(uint8_t)pit, GPU_CW, 0};
            } else if (pit == GPU_KICHWA_R) {
                out[n_takasa++] = {(uint8_t)pit, GPU_ACW, 0};
            } else {
                out[n_takasa++] = {(uint8_t)pit, GPU_CW, 0};
                out[n_takasa++] = {(uint8_t)pit, GPU_ACW, 0};
            }
        }
    } else {
        for (int pit = GPU_INNER_PITS; pit < GPU_PITS_PER_SIDE; ++pit) {
            if (pits[pit] < 2) continue;
            out[n_takasa++] = {(uint8_t)pit, GPU_CW, 0};
            out[n_takasa++] = {(uint8_t)pit, GPU_ACW, 0};
        }
    }
    return n_takasa;
}

// ---------------------------------------------------------------------------
// Make move — templated on IS_CAPTURING (same as CPU)
// ---------------------------------------------------------------------------

template<bool IS_CAPTURING>
__device__ uint8_t gpu_make_move_impl(uint8_t* pits, const GpuMove& m) {
    int dir_idx = (m.dir == GPU_CW) ? 0 : 1;
    int step = dir_idx == 0 ? 1 : 15;
    int sow_start = m.pit;
    int total_sown = 0;

    int seeds = pits[sow_start];
    pits[sow_start] = 0;

    for (;;) {
        int landing = gpu_sow(pits, sow_start, seeds, dir_idx);
        total_sown += seeds;

        if (total_sown > GPU_MAX_SOW)
            return GPU_MOVE_INFINITE;

        uint8_t landing_count = pits[landing];

        if (landing_count == 1) {
            if (gpu_inner_empty(pits, 0))
                return GPU_MOVE_INNER_ROW_EMPTY;
            break;
        }

        if (gpu_inner_empty(pits, 0))
            return GPU_MOVE_INNER_ROW_EMPTY;

        if (IS_CAPTURING) {
            if (landing < GPU_INNER_PITS) {
                int opp_idx = GPU_PITS_PER_SIDE + landing;
                if (pits[opp_idx] > 0) {
                    seeds = pits[opp_idx];
                    pits[opp_idx] = 0;
                    if (gpu_inner_empty(pits, 1))
                        return GPU_MOVE_INNER_ROW_EMPTY;
                    if (landing <= 1) {
                        sow_start = GPU_KICHWA_L;
                        step = 1;
                        dir_idx = 0;
                    } else if (landing >= 6) {
                        sow_start = GPU_KICHWA_R;
                        step = 15;
                        dir_idx = 1;
                    } else {
                        sow_start = (dir_idx == 0) ? GPU_KICHWA_L : GPU_KICHWA_R;
                    }
                    continue;
                }
            }
        }

        seeds = landing_count;
        pits[landing] = 0;
        sow_start = (landing + step) & 15;
    }

    gpu_swap_sides(pits);
    return GPU_MOVE_OK;
}

__device__ __forceinline__ uint8_t gpu_make_move(uint8_t* pits, const GpuMove& m) {
    if (m.is_mtaji)
        return gpu_make_move_impl<true>(pits, m);
    else
        return gpu_make_move_impl<false>(pits, m);
}

// ---------------------------------------------------------------------------
// Canonical hash — scalar port of Zobrist XOR (no SSE)
// ---------------------------------------------------------------------------

__device__ uint64_t gpu_canonical_hash_only(const uint8_t* pits) {
    uint64_t ho = 0, hr = 0;

    // Side 0: inner row (positions 0-7, reflected 7-0)
    ho ^= d_ZOBRIST_TABLE[ 0][pits[ 0]]; hr ^= d_ZOBRIST_TABLE[ 0][pits[ 7]];
    ho ^= d_ZOBRIST_TABLE[ 1][pits[ 1]]; hr ^= d_ZOBRIST_TABLE[ 1][pits[ 6]];
    ho ^= d_ZOBRIST_TABLE[ 2][pits[ 2]]; hr ^= d_ZOBRIST_TABLE[ 2][pits[ 5]];
    ho ^= d_ZOBRIST_TABLE[ 3][pits[ 3]]; hr ^= d_ZOBRIST_TABLE[ 3][pits[ 4]];
    ho ^= d_ZOBRIST_TABLE[ 4][pits[ 4]]; hr ^= d_ZOBRIST_TABLE[ 4][pits[ 3]];
    ho ^= d_ZOBRIST_TABLE[ 5][pits[ 5]]; hr ^= d_ZOBRIST_TABLE[ 5][pits[ 2]];
    ho ^= d_ZOBRIST_TABLE[ 6][pits[ 6]]; hr ^= d_ZOBRIST_TABLE[ 6][pits[ 1]];
    ho ^= d_ZOBRIST_TABLE[ 7][pits[ 7]]; hr ^= d_ZOBRIST_TABLE[ 7][pits[ 0]];
    // Side 0: outer row (positions 8-15, reflected 15-8)
    ho ^= d_ZOBRIST_TABLE[ 8][pits[ 8]]; hr ^= d_ZOBRIST_TABLE[ 8][pits[15]];
    ho ^= d_ZOBRIST_TABLE[ 9][pits[ 9]]; hr ^= d_ZOBRIST_TABLE[ 9][pits[14]];
    ho ^= d_ZOBRIST_TABLE[10][pits[10]]; hr ^= d_ZOBRIST_TABLE[10][pits[13]];
    ho ^= d_ZOBRIST_TABLE[11][pits[11]]; hr ^= d_ZOBRIST_TABLE[11][pits[12]];
    ho ^= d_ZOBRIST_TABLE[12][pits[12]]; hr ^= d_ZOBRIST_TABLE[12][pits[11]];
    ho ^= d_ZOBRIST_TABLE[13][pits[13]]; hr ^= d_ZOBRIST_TABLE[13][pits[10]];
    ho ^= d_ZOBRIST_TABLE[14][pits[14]]; hr ^= d_ZOBRIST_TABLE[14][pits[ 9]];
    ho ^= d_ZOBRIST_TABLE[15][pits[15]]; hr ^= d_ZOBRIST_TABLE[15][pits[ 8]];
    // Side 1: inner row (positions 16-23, reflected 23-16)
    ho ^= d_ZOBRIST_TABLE[16][pits[16]]; hr ^= d_ZOBRIST_TABLE[16][pits[23]];
    ho ^= d_ZOBRIST_TABLE[17][pits[17]]; hr ^= d_ZOBRIST_TABLE[17][pits[22]];
    ho ^= d_ZOBRIST_TABLE[18][pits[18]]; hr ^= d_ZOBRIST_TABLE[18][pits[21]];
    ho ^= d_ZOBRIST_TABLE[19][pits[19]]; hr ^= d_ZOBRIST_TABLE[19][pits[20]];
    ho ^= d_ZOBRIST_TABLE[20][pits[20]]; hr ^= d_ZOBRIST_TABLE[20][pits[19]];
    ho ^= d_ZOBRIST_TABLE[21][pits[21]]; hr ^= d_ZOBRIST_TABLE[21][pits[18]];
    ho ^= d_ZOBRIST_TABLE[22][pits[22]]; hr ^= d_ZOBRIST_TABLE[22][pits[17]];
    ho ^= d_ZOBRIST_TABLE[23][pits[23]]; hr ^= d_ZOBRIST_TABLE[23][pits[16]];
    // Side 1: outer row (positions 24-31, reflected 31-24)
    ho ^= d_ZOBRIST_TABLE[24][pits[24]]; hr ^= d_ZOBRIST_TABLE[24][pits[31]];
    ho ^= d_ZOBRIST_TABLE[25][pits[25]]; hr ^= d_ZOBRIST_TABLE[25][pits[30]];
    ho ^= d_ZOBRIST_TABLE[26][pits[26]]; hr ^= d_ZOBRIST_TABLE[26][pits[29]];
    ho ^= d_ZOBRIST_TABLE[27][pits[27]]; hr ^= d_ZOBRIST_TABLE[27][pits[28]];
    ho ^= d_ZOBRIST_TABLE[28][pits[28]]; hr ^= d_ZOBRIST_TABLE[28][pits[27]];
    ho ^= d_ZOBRIST_TABLE[29][pits[29]]; hr ^= d_ZOBRIST_TABLE[29][pits[26]];
    ho ^= d_ZOBRIST_TABLE[30][pits[30]]; hr ^= d_ZOBRIST_TABLE[30][pits[25]];
    ho ^= d_ZOBRIST_TABLE[31][pits[31]]; hr ^= d_ZOBRIST_TABLE[31][pits[24]];

    return ho <= hr ? ho : hr;
}

// ---------------------------------------------------------------------------
// GPU successor output struct
// ---------------------------------------------------------------------------

struct GpuSuccessor {
    uint8_t  pits[32];     // successor state (after make_move + swap_sides)
    uint64_t hash;         // canonical hash
    uint8_t  is_terminal;  // 1 if terminal
    uint8_t  pad[7];       // pad to 48 bytes
};

// ---------------------------------------------------------------------------
// Main expansion kernel: one thread per input state
// ---------------------------------------------------------------------------

__global__ void expand_states_kernel(
    const uint8_t* __restrict__ input_pits,   // [count][32]
    GpuSuccessor*  __restrict__ output,        // [count][GPU_MAX_MOVES]
    int*           __restrict__ valid_counts,  // [count]
    int count)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= count) return;

    // Load state into registers
    uint8_t pits[32];
    memcpy(pits, &input_pits[idx * 32], 32);

    // Generate moves
    GpuMove moves[GPU_MAX_MOVES];
    int nmoves = gpu_generate_moves(pits, moves);

    // Execute each move
    int valid = 0;
    GpuSuccessor* my_out = &output[idx * GPU_MAX_MOVES];

    for (int i = 0; i < nmoves; i++) {
        uint8_t succ_pits[32];
        memcpy(succ_pits, pits, 32);

        uint8_t result = gpu_make_move(succ_pits, moves[i]);
        if (result != GPU_MOVE_OK) continue;

        memcpy(my_out[valid].pits, succ_pits, 32);
        my_out[valid].hash = gpu_canonical_hash_only(succ_pits);
        my_out[valid].is_terminal = gpu_is_terminal(succ_pits) ? 1 : 0;
        valid++;
    }

    valid_counts[idx] = valid;
}
