/*
 * enumerate_gpu.cu — GPU-accelerated production enumerator
 *
 * Phase 1: CPU-only parallel DFS (reuses enumerate_core.h)
 *          Runs until discovery rate drops below threshold.
 * Phase 2: GPU batch pipeline (double-buffered)
 *          GPU computes successors, CPU does hash table inserts.
 *
 * Usage: ./build/enumerate_gpu --mem-gb 280 --threads 48 [--gpu-batch 256000]
 */

#include "bao_gpu.cuh"
#include "enumerate_core.h"
#include <ctime>
#include <algorithm>

#define CUDA_CHECK(call) do { \
    cudaError_t err = (call); \
    if (err != cudaSuccess) { \
        fprintf(stderr, "CUDA error at %s:%d: %s\n", \
                __FILE__, __LINE__, cudaGetErrorString(err)); \
        exit(1); \
    } \
} while(0)

// ---------------------------------------------------------------------------
// GPU Pipeline — double-buffered CPU<->GPU batch processor
// ---------------------------------------------------------------------------

struct GpuBuffer {
    // Device
    uint8_t*      d_input;       // [batch_size][32]
    GpuSuccessor* d_output;      // [batch_size][GPU_MAX_MOVES]
    int*          d_valid;       // [batch_size]

    // Pinned host
    uint8_t*      h_input;
    GpuSuccessor* h_output;
    int*          h_valid;

    cudaStream_t  stream;
    int           count;         // actual states in this batch
};

static void alloc_buffer(GpuBuffer& buf, int batch_size) {
    size_t out_size = (size_t)batch_size * GPU_MAX_MOVES * sizeof(GpuSuccessor);

    CUDA_CHECK(cudaMalloc(&buf.d_input,  batch_size * 32));
    CUDA_CHECK(cudaMalloc(&buf.d_output, out_size));
    CUDA_CHECK(cudaMalloc(&buf.d_valid,  batch_size * sizeof(int)));

    CUDA_CHECK(cudaMallocHost(&buf.h_input,  batch_size * 32));
    CUDA_CHECK(cudaMallocHost(&buf.h_output, out_size));
    CUDA_CHECK(cudaMallocHost(&buf.h_valid,  batch_size * sizeof(int)));

    CUDA_CHECK(cudaStreamCreate(&buf.stream));
    buf.count = 0;
}

static void free_buffer(GpuBuffer& buf) {
    cudaFree(buf.d_input);
    cudaFree(buf.d_output);
    cudaFree(buf.d_valid);
    cudaFreeHost(buf.h_input);
    cudaFreeHost(buf.h_output);
    cudaFreeHost(buf.h_valid);
    cudaStreamDestroy(buf.stream);
}

// Fill batch from CPU stacks (round-robin across threads)
static int fill_batch(GpuBuffer& buf, int batch_size,
                      ThreadWork* work, int num_threads) {
    int filled = 0;
    uint8_t* dest = buf.h_input;

    bool any = true;
    while (filled < batch_size && any) {
        any = false;
        for (int t = 0; t < num_threads && filled < batch_size; t++) {
            if (work[t].stack.empty()) continue;
            any = true;
            CompactState cs = work[t].stack.back();
            work[t].stack.pop_back();
            memcpy(&dest[filled * 32], cs.pits, 32);
            filled++;
        }
    }

    buf.count = filled;
    return filled;
}

// Launch async: H2D → kernel → D2H on the buffer's stream
static void launch_async(GpuBuffer& buf) {
    if (buf.count == 0) return;
    int n = buf.count;

    cudaMemcpyAsync(buf.d_input, buf.h_input, n * 32,
                    cudaMemcpyHostToDevice, buf.stream);

    int block = 256;
    int grid = (n + block - 1) / block;
    expand_states_kernel<<<grid, block, 0, buf.stream>>>(
        buf.d_input, buf.d_output, buf.d_valid, n);

    cudaMemcpyAsync(buf.h_valid, buf.d_valid, n * sizeof(int),
                    cudaMemcpyDeviceToHost, buf.stream);
    cudaMemcpyAsync(buf.h_output, buf.d_output,
                    (size_t)n * GPU_MAX_MOVES * sizeof(GpuSuccessor),
                    cudaMemcpyDeviceToHost, buf.stream);
}

// Wait for buffer and insert results into hash table
struct BatchResult {
    size_t new_states;
    size_t terminals;
};

static BatchResult process_results(GpuBuffer& buf, AtomicHashSet& visited,
                                   ThreadWork* work, int num_threads) {
    cudaStreamSynchronize(buf.stream);

    BatchResult br = {0, 0};
    int push_target = 0;

    for (int i = 0; i < buf.count; i++) {
        int valid = buf.h_valid[i];
        GpuSuccessor* succs = &buf.h_output[i * GPU_MAX_MOVES];

        // Prefetch hash table slots
        for (int j = 0; j < valid; j++)
            visited.prefetch(succs[j].hash);

        // Insert
        for (int j = 0; j < valid; j++) {
            if (!visited.insert(succs[j].hash))
                continue;

            br.new_states++;

            if (succs[j].is_terminal) {
                br.terminals++;
                continue;
            }

            CompactState cs;
            memcpy(cs.pits, succs[j].pits, 32);
            work[push_target].stack.push_back(cs);
            push_target = (push_target + 1) % num_threads;
        }
    }

    return br;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    size_t mem_gb = 100;
    int num_threads = (int)std::thread::hardware_concurrency();
    if (num_threads < 1) num_threads = 8;
    int gpu_batch = 256000;
    size_t gpu_transition = 1000000; // switch to GPU when <1M new/2s

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mem-gb") == 0 && i+1 < argc)
            mem_gb = (size_t)atol(argv[++i]);
        if (strcmp(argv[i], "--threads") == 0 && i+1 < argc)
            num_threads = atoi(argv[++i]);
        if (strcmp(argv[i], "--gpu-batch") == 0 && i+1 < argc)
            gpu_batch = atoi(argv[++i]);
        if (strcmp(argv[i], "--gpu-transition") == 0 && i+1 < argc)
            gpu_transition = (size_t)atol(argv[++i]);
    }

    size_t table_bytes = mem_gb * (size_t)1024 * 1024 * 1024;
    size_t table_cap   = table_bytes / sizeof(uint32_t);

    printf("Bao la Kujifunza — GPU-Accelerated Enumerator\n");
    printf("==============================================\n");

    // Print GPU info
    cudaDeviceProp prop;
    CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    printf("GPU:         %s (%d SMs)\n", prop.name, prop.multiProcessorCount);
    printf("Hash table:  %zu GB, %zuB slots\n", mem_gb, table_cap / 1000000000);
    printf("CPU threads: %d (Phase 1)\n", num_threads);
    printf("GPU batch:   %d states\n", gpu_batch);
    printf("GPU switch:  <%zuK new/2s\n\n", gpu_transition / 1000);

    zobrist_init();
    sow_table_init();
    gpu_tables_init();

    fprintf(stderr, "Allocating hash table...\n");
    AtomicHashSet visited(table_cap, true);

    EnumGlobals g;
    g.num_threads = num_threads;
    g.max_states = 0;

    size_t warmup_target = (size_t)num_threads * 10000;
    fprintf(stderr, "Warmup...\n");
    size_t warmup_states = enum_warmup(visited, g, warmup_target);
    fprintf(stderr, "Warmup: %zu states\n\n", warmup_states);

    time_t t0 = time(nullptr);
    std::vector<ThreadStats> thread_stats(num_threads);
    std::vector<std::thread> threads;

    // ===== PHASE 1: CPU DFS =====
    fprintf(stderr, "=== PHASE 1: CPU DFS (%d threads) ===\n", num_threads);

    std::atomic<bool> switch_to_gpu{false};

    std::thread reporter([&]() {
        size_t prev_states = 0;
        size_t prev_ins_true = 0;
        size_t prev_ins_false = 0;
        size_t prev_stack = 0;
        while (!g.done.load(std::memory_order_relaxed) &&
               !g.table_full.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            size_t s = g.states.load(std::memory_order_relaxed);
            double elapsed = difftime(time(nullptr), t0);
            double rate = elapsed > 0 ? s / elapsed : 0;
            size_t new_states = s - prev_states;

            size_t total_stack = 0;
            size_t total_ins_true = 0, total_ins_false = 0;
            for (int i = 0; i < g.num_threads; ++i) {
                total_stack += g.work[i].stack.size();
                total_ins_true += thread_stats[i].insert_true;
                total_ins_false += thread_stats[i].insert_false;
            }
            size_t ins_delta = total_ins_true - prev_ins_true;

            if (new_states < 5000000) {
                fprintf(stderr,
                    "\r  P1 | St:%zu +%zu | Stk:%zuM | ins:%.1fK | %dthr %.0fs     ",
                    s, new_states, total_stack / 1000000,
                    ins_delta / 1e3, num_threads, elapsed);
            } else {
                fprintf(stderr,
                    "\r  P1 | St: %12zu | +%zu/2s | Stk: %zuM | %.1fM/s | %.0fs     ",
                    s, new_states, total_stack / 1000000, rate / 1e6, elapsed);
            }
            fflush(stderr);

            // Transition to GPU when discovery rate drops
            if (new_states < gpu_transition && s > 1000000000) {
                switch_to_gpu.store(true, std::memory_order_relaxed);
                g.done.store(true, std::memory_order_relaxed);
                fprintf(stderr,
                    "\n\n>>> Discovery rate < %zuK/2s. Switching to GPU Phase 2. <<<\n\n",
                    gpu_transition / 1000);
            }

            prev_states = s;
            prev_ins_true = total_ins_true;
            prev_ins_false = total_ins_false;
            prev_stack = total_stack;
        }
    });

    for (int t = 0; t < num_threads; ++t)
        threads.emplace_back(enum_worker, std::ref(visited),
                             std::ref(thread_stats[t]), t, std::ref(g));

    for (auto& th : threads) th.join();
    g.done.store(true, std::memory_order_relaxed);
    reporter.join();

    // Aggregate Phase 1 stats
    size_t total_states = warmup_states;
    size_t total_terminal = 0;
    for (int t = 0; t < num_threads; ++t) {
        total_states += thread_stats[t].states;
        total_terminal += thread_stats[t].terminal;
    }

    size_t remaining_stack = 0;
    for (int t = 0; t < num_threads; ++t)
        remaining_stack += g.work[t].stack.size();

    fprintf(stderr, "Phase 1 complete: %zu states, %zuM stack remaining\n",
            total_states, remaining_stack / 1000000);

    // ===== PHASE 2: GPU PIPELINE =====
    if (switch_to_gpu.load() && remaining_stack > 0) {
        fprintf(stderr, "\n=== PHASE 2: GPU Pipeline (batch=%d) ===\n", gpu_batch);

        GpuBuffer bufs[2];
        alloc_buffer(bufs[0], gpu_batch);
        alloc_buffer(bufs[1], gpu_batch);

        size_t gpu_new = 0, gpu_term = 0, gpu_batches = 0;
        time_t t2 = time(nullptr);

        // Prime first buffer
        int filled = fill_batch(bufs[0], gpu_batch, g.work, num_threads);
        if (filled > 0) launch_async(bufs[0]);

        int cur = 0;
        bool done = false;

        while (!done) {
            int next = 1 - cur;

            // Fill next buffer while GPU processes current
            filled = fill_batch(bufs[next], gpu_batch, g.work, num_threads);
            if (filled > 0) launch_async(bufs[next]);

            // Process current buffer results
            if (bufs[cur].count > 0) {
                BatchResult br = process_results(bufs[cur], visited, g.work, num_threads);
                gpu_new += br.new_states;
                gpu_term += br.terminals;
                total_states += br.new_states;
                total_terminal += br.terminals;
                gpu_batches++;

                // Report every 10 batches
                if (gpu_batches % 10 == 0) {
                    double elapsed = difftime(time(nullptr), t2);
                    remaining_stack = 0;
                    for (int t = 0; t < num_threads; ++t)
                        remaining_stack += g.work[t].stack.size();
                    fprintf(stderr,
                        "\r  P2 | St:%zu +%zuK | Stk:%zuM | Batch %zu | %.1fM/s | %.0fs     ",
                        total_states, gpu_new / 1000,
                        remaining_stack / 1000000, gpu_batches,
                        gpu_new / (elapsed > 0 ? elapsed : 1) / 1e6, elapsed);
                    fflush(stderr);
                }
            }

            cur = next;

            // Check if all stacks are empty
            if (filled == 0) {
                // Process last pending buffer
                if (bufs[cur].count > 0) {
                    BatchResult br = process_results(bufs[cur], visited, g.work, num_threads);
                    gpu_new += br.new_states;
                    gpu_term += br.terminals;
                    total_states += br.new_states;
                    total_terminal += br.terminals;
                }

                remaining_stack = 0;
                for (int t = 0; t < num_threads; ++t)
                    remaining_stack += g.work[t].stack.size();
                if (remaining_stack == 0)
                    done = true;
                // else: new states were pushed, continue
            }
        }

        double gpu_elapsed = difftime(time(nullptr), t2);
        fprintf(stderr, "\n\nPhase 2 complete: +%zu states, %zu batches, %.1f sec\n",
                gpu_new, gpu_batches, gpu_elapsed);

        free_buffer(bufs[0]);
        free_buffer(bufs[1]);
    }

    // ===== FINAL REPORT =====
    double elapsed = difftime(time(nullptr), t0);
    bool complete = !g.table_full.load();

    fprintf(stderr, "\n\n%s\n", complete ? "COMPLETE." : "INCOMPLETE.");

    printf("\nResults\n-------\n");
    printf("Complete:          %s\n", complete ? "YES" : "NO");
    printf("Canonical states:  %zu\n", total_states);
    printf("Terminal states:   %zu\n", total_terminal);
    printf("Hash load:         %.4f\n", (double)total_states / visited.capacity());
    printf("Elapsed:           %.1f sec\n", elapsed);
    if (elapsed > 0)
        printf("Rate:              %.1fM states/sec\n", total_states / elapsed / 1e6);

    delete[] g.work;
    return complete ? 0 : 1;
}
