/*
 * benchmark_gpu.cu — GPU correctness + performance benchmark
 *
 * Verifies that the CUDA game engine produces BIT-IDENTICAL results
 * to the CPU engine, then benchmarks GPU kernel throughput.
 *
 * Usage: ./build/benchmark_gpu [--corpus N] [--secs F]
 *
 * Correctness: for each (state, move) pair in the corpus:
 *   - CPU and GPU must produce the same MoveResult
 *   - If OK: successor pits must match, canonical hash must match,
 *            is_terminal must match
 *
 * Performance: GPU expand_states_kernel calls/sec
 */

#include "../src/bao.h"
#include "../src/bao_gpu.cuh"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>

using Clock = std::chrono::high_resolution_clock;

#define CUDA_CHECK(call) do { \
    cudaError_t err = (call); \
    if (err != cudaSuccess) { \
        fprintf(stderr, "CUDA error at %s:%d: %s\n", \
                __FILE__, __LINE__, cudaGetErrorString(err)); \
        exit(1); \
    } \
} while(0)

// ---------------------------------------------------------------------------
// Corpus generation (same logic as benchmark_make_move.cpp)
// ---------------------------------------------------------------------------

static uint32_t rng_state = 0xBAD5EEDU;
static uint32_t rng_next() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

struct CorpusState {
    uint8_t pits[32];
};

static std::vector<CorpusState> build_corpus(int min_depth, int min_seeds, size_t target) {
    std::vector<CorpusState> corpus;
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

            if (depth >= min_depth && corpus.size() < target) {
                int max_pit = 0;
                for (int i = 0; i < PITS_PER_SIDE; ++i)
                    max_pit = max_pit > (int)s.pits[i] ? max_pit : (int)s.pits[i];
                if (max_pit >= min_seeds) {
                    CorpusState cs;
                    memcpy(cs.pits, s.pits, 32);
                    corpus.push_back(cs);
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

// ---------------------------------------------------------------------------
// CPU reference: compute all successors for each corpus state
// ---------------------------------------------------------------------------

struct CpuSuccResult {
    uint8_t  pits[32];
    uint64_t hash;
    uint8_t  result;       // 0=OK, 1=INFINITE, 2=INNER_ROW_EMPTY
    uint8_t  is_terminal;
};

struct CpuStateResult {
    CpuSuccResult succs[MAX_MOVES];
    int move_count;    // total moves generated
    int valid_count;   // moves that returned OK
};

static std::vector<CpuStateResult> cpu_reference(const std::vector<CorpusState>& corpus) {
    std::vector<CpuStateResult> results(corpus.size());

    for (size_t i = 0; i < corpus.size(); i++) {
        BaoState state;
        memcpy(state.pits, corpus[i].pits, 32);

        Move moves[MAX_MOVES];
        int n = state.generate_moves(moves);
        results[i].move_count = n;

        int valid = 0;
        for (int j = 0; j < n; j++) {
            BaoState succ;
            memcpy(succ.pits, state.pits, 32);
            MoveResult r = succ.make_move(moves[j]);

            results[i].succs[j].result = (uint8_t)r;
            if (r == MoveResult::OK) {
                memcpy(results[i].succs[j].pits, succ.pits, 32);
                results[i].succs[j].hash = succ.canonical_hash_only();
                results[i].succs[j].is_terminal = succ.is_terminal() ? 1 : 0;
                valid++;
            }
        }
        results[i].valid_count = valid;
    }

    return results;
}

// ---------------------------------------------------------------------------
// GPU processing: launch kernel and retrieve results
// ---------------------------------------------------------------------------

struct GpuResults {
    std::vector<GpuSuccessor> successors;  // [corpus_size * GPU_MAX_MOVES]
    std::vector<int> valid_counts;         // [corpus_size]
};

static GpuResults gpu_process(const std::vector<CorpusState>& corpus) {
    int count = (int)corpus.size();

    // Allocate device memory
    uint8_t* d_input;
    GpuSuccessor* d_output;
    int* d_valid;

    CUDA_CHECK(cudaMalloc(&d_input,  count * 32));
    CUDA_CHECK(cudaMalloc(&d_output, (size_t)count * GPU_MAX_MOVES * sizeof(GpuSuccessor)));
    CUDA_CHECK(cudaMalloc(&d_valid,  count * sizeof(int)));

    // Copy input to device
    CUDA_CHECK(cudaMemcpy(d_input, corpus.data(), count * 32, cudaMemcpyHostToDevice));

    // Launch kernel
    int block = 256;
    int grid = (count + block - 1) / block;
    expand_states_kernel<<<grid, block>>>(d_input, d_output, d_valid, count);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());

    // Copy results back
    GpuResults res;
    res.successors.resize((size_t)count * GPU_MAX_MOVES);
    res.valid_counts.resize(count);

    CUDA_CHECK(cudaMemcpy(res.valid_counts.data(), d_valid,
                           count * sizeof(int), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(res.successors.data(), d_output,
                           (size_t)count * GPU_MAX_MOVES * sizeof(GpuSuccessor),
                           cudaMemcpyDeviceToHost));

    cudaFree(d_input);
    cudaFree(d_output);
    cudaFree(d_valid);

    return res;
}

// ---------------------------------------------------------------------------
// Comparison: CPU vs GPU
// ---------------------------------------------------------------------------

static int compare_results(const std::vector<CorpusState>& corpus,
                           const std::vector<CpuStateResult>& cpu,
                           const GpuResults& gpu) {
    int failures = 0;
    int total_valid = 0;

    for (size_t i = 0; i < corpus.size(); i++) {
        int cpu_valid = cpu[i].valid_count;
        int gpu_valid = gpu.valid_counts[i];

        if (cpu_valid != gpu_valid) {
            if (failures < 10)
                fprintf(stderr, "FAIL state %zu: CPU valid=%d, GPU valid=%d\n",
                        i, cpu_valid, gpu_valid);
            failures++;
            continue;
        }

        total_valid += cpu_valid;

        // Compare each valid successor (CPU stores them in order of moves,
        // GPU stores them in order of valid moves — same order since both
        // skip non-OK moves sequentially)
        const GpuSuccessor* gpu_succs = &gpu.successors[i * GPU_MAX_MOVES];
        int vi = 0;
        for (int j = 0; j < cpu[i].move_count; j++) {
            if (cpu[i].succs[j].result != 0) continue; // skip non-OK

            // Compare pits
            if (memcmp(cpu[i].succs[j].pits, gpu_succs[vi].pits, 32) != 0) {
                if (failures < 10)
                    fprintf(stderr, "FAIL state %zu move %d: pits mismatch\n", i, j);
                failures++;
            }

            // Compare hash
            if (cpu[i].succs[j].hash != gpu_succs[vi].hash) {
                if (failures < 10)
                    fprintf(stderr, "FAIL state %zu move %d: hash mismatch "
                            "(CPU=%016llx GPU=%016llx)\n",
                            i, j,
                            (unsigned long long)cpu[i].succs[j].hash,
                            (unsigned long long)gpu_succs[vi].hash);
                failures++;
            }

            // Compare terminal
            if (cpu[i].succs[j].is_terminal != gpu_succs[vi].is_terminal) {
                if (failures < 10)
                    fprintf(stderr, "FAIL state %zu move %d: terminal mismatch\n", i, j);
                failures++;
            }

            vi++;
        }
    }

    printf("Compared %zu states, %d valid successors\n", corpus.size(), total_valid);
    return failures;
}

// ---------------------------------------------------------------------------
// Performance benchmark: time the kernel over multiple iterations
// ---------------------------------------------------------------------------

static double benchmark_kernel(const std::vector<CorpusState>& corpus, double bench_secs) {
    int count = (int)corpus.size();

    uint8_t* d_input;
    GpuSuccessor* d_output;
    int* d_valid;

    CUDA_CHECK(cudaMalloc(&d_input,  count * 32));
    CUDA_CHECK(cudaMalloc(&d_output, (size_t)count * GPU_MAX_MOVES * sizeof(GpuSuccessor)));
    CUDA_CHECK(cudaMalloc(&d_valid,  count * sizeof(int)));
    CUDA_CHECK(cudaMemcpy(d_input, corpus.data(), count * 32, cudaMemcpyHostToDevice));

    int block = 256;
    int grid = (count + block - 1) / block;

    // Warmup
    expand_states_kernel<<<grid, block>>>(d_input, d_output, d_valid, count);
    CUDA_CHECK(cudaDeviceSynchronize());

    // Timed loop
    size_t iterations = 0;
    auto t0 = Clock::now();
    auto deadline = t0 + std::chrono::duration<double>(bench_secs);

    while (Clock::now() < deadline) {
        expand_states_kernel<<<grid, block>>>(d_input, d_output, d_valid, count);
        CUDA_CHECK(cudaDeviceSynchronize());
        iterations++;
    }

    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();

    cudaFree(d_input);
    cudaFree(d_output);
    cudaFree(d_valid);

    size_t total_states = iterations * count;
    return total_states / elapsed;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    size_t corpus_size = 5000;
    double bench_secs = 5.0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--corpus") && i+1 < argc) corpus_size = (size_t)atol(argv[++i]);
        if (!strcmp(argv[i], "--secs")   && i+1 < argc) bench_secs = atof(argv[++i]);
    }

    // Init CPU tables
    zobrist_init();
    sow_table_init();

    // Init GPU tables
    gpu_tables_init();

    // Print GPU info
    cudaDeviceProp prop;
    CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    printf("Bao GPU Benchmark\n");
    printf("=================\n");
    printf("GPU: %s (%d SMs, %d cores/SM)\n",
           prop.name, prop.multiProcessorCount,
           prop.maxThreadsPerMultiProcessor / 32 * 32);
    printf("VRAM: %zu MB\n", prop.totalGlobalMem / (1024*1024));
    printf("Corpus: %zu states\n\n", corpus_size);

    // Build corpus
    auto t0 = Clock::now();
    auto corpus = build_corpus(20, 16, corpus_size);
    double setup = std::chrono::duration<double>(Clock::now() - t0).count();
    printf("Corpus built: %zu states (%.2fs)\n\n", corpus.size(), setup);

    // === CORRECTNESS ===
    printf("Correctness\n");
    printf("-----------\n");

    auto cpu_ref = cpu_reference(corpus);
    printf("CPU reference computed\n");

    auto gpu_res = gpu_process(corpus);
    printf("GPU results computed\n");

    int failures = compare_results(corpus, cpu_ref, gpu_res);

    if (failures > 0) {
        printf("\nCorrectness: FAIL (%d failures)\n", failures);
        return 1;
    }
    printf("\nCorrectness: PASS\n\n");

    // === PERFORMANCE ===
    printf("Performance (%.1f sec)\n", bench_secs);
    printf("---------------------\n");

    double states_per_sec = benchmark_kernel(corpus, bench_secs);

    printf("=== PRIMARY METRIC ===\n");
    printf("GPU expand states/sec: %.3fM\n", states_per_sec / 1e6);
    printf("\n");

    return 0;
}
