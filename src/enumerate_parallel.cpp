/*
 * enumerate_parallel.cpp — Production parallel enumerator with checkpointing
 *
 * Supports spot/preemptible VMs: saves hash table + stacks to disk
 * periodically. On restart, loads the checkpoint and continues.
 *
 * Usage: ./build/enumerate_par --mem-gb 280 --threads 48 [--checkpoint /mnt/disk/ckpt]
 */

#include "enumerate_core.h"
#include <ctime>
#include <string>

// ---------------------------------------------------------------------------
// Checkpoint: save and load hash table + stacks
// ---------------------------------------------------------------------------

static bool save_checkpoint(const char* dir, AtomicHashSet& visited,
                            ThreadWork* work, int num_threads,
                            size_t warmup_states, size_t g_states_snapshot) {
    std::string table_path = std::string(dir) + "/table.bin";
    std::string stack_path = std::string(dir) + "/stacks.bin";

    fprintf(stderr, "\n  Saving checkpoint to %s ...\n", dir);
    auto t0 = time(nullptr);

    // Save hash table
    if (!visited.save(table_path.c_str())) return false;

    // Save stacks + counters
    FILE* f = fopen(stack_path.c_str(), "wb");
    if (!f) { perror("save stacks"); return false; }
    fwrite(&warmup_states, sizeof(size_t), 1, f);
    // Save current total state count so we can restore g.states on load
    size_t total_so_far = g_states_snapshot;
    fwrite(&total_so_far, sizeof(size_t), 1, f);
    fwrite(&num_threads, sizeof(int), 1, f);
    for (int t = 0; t < num_threads; ++t) {
        size_t sz = work[t].stack.size();
        fwrite(&sz, sizeof(size_t), 1, f);
        if (sz > 0)
            fwrite(work[t].stack.data(), sizeof(CompactState), sz, f);
    }
    fclose(f);

    double secs = difftime(time(nullptr), t0);
    fprintf(stderr, "  Checkpoint saved (%.0fs)\n", secs);
    return true;
}

static bool load_checkpoint(const char* dir, AtomicHashSet& visited,
                            ThreadWork* work, int num_threads,
                            size_t& warmup_states, size_t& restored_states) {
    std::string table_path = std::string(dir) + "/table.bin";
    std::string stack_path = std::string(dir) + "/stacks.bin";

    FILE* f = fopen(stack_path.c_str(), "rb");
    if (!f) return false; // no checkpoint

    fprintf(stderr, "Loading checkpoint from %s ...\n", dir);
    auto t0 = time(nullptr);

    // Load hash table
    if (!visited.load_from(table_path.c_str())) { fclose(f); return false; }

    // Load counters
    fread(&warmup_states, sizeof(size_t), 1, f);
    fread(&restored_states, sizeof(size_t), 1, f);
    int saved_threads;
    fread(&saved_threads, sizeof(int), 1, f);

    // Distribute loaded stacks across current threads
    for (int t = 0; t < saved_threads; ++t) {
        size_t sz;
        fread(&sz, sizeof(size_t), 1, f);
        if (sz > 0) {
            int target = t % num_threads; // round-robin if thread count differs
            size_t old_sz = work[target].stack.size();
            work[target].stack.resize(old_sz + sz);
            fread(&work[target].stack[old_sz], sizeof(CompactState), sz, f);
        }
    }
    fclose(f);

    double secs = difftime(time(nullptr), t0);
    fprintf(stderr, "Checkpoint loaded (%.0fs)\n\n", secs);
    return true;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    size_t mem_gb = 100;
    int num_threads = (int)std::thread::hardware_concurrency();
    if (num_threads < 1) num_threads = 8;
    const char* ckpt_dir = nullptr;
    int ckpt_interval = 7200; // checkpoint every 2 hours (seconds)

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mem-gb") == 0 && i + 1 < argc)
            mem_gb = (size_t)atol(argv[++i]);
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            num_threads = atoi(argv[++i]);
        if (strcmp(argv[i], "--checkpoint") == 0 && i + 1 < argc)
            ckpt_dir = argv[++i];
        if (strcmp(argv[i], "--ckpt-interval") == 0 && i + 1 < argc)
            ckpt_interval = atoi(argv[++i]);
    }

    size_t table_bytes = mem_gb * (size_t)1024 * 1024 * 1024;
    size_t table_cap   = table_bytes / sizeof(uint32_t);
    size_t max_states  = (size_t)(table_cap * 0.70);

    printf("Bao la Kujifunza — Parallel State Enumerator\n");
    printf("=============================================\n");
    printf("Hash table:  %zu GB, 4 bytes/slot, %zuB slots, ~%zuB max states\n",
           mem_gb, table_cap / 1000000000, max_states / 1000000000);
    printf("Threads:     %d\n", num_threads);
    printf("Stack entry: %zu bytes\n", sizeof(CompactState));
    if (ckpt_dir)
        printf("Checkpoint:  %s (every %ds)\n", ckpt_dir, ckpt_interval);
    printf("\n");

    zobrist_init();

    fprintf(stderr, "Allocating hash table...\n");
    AtomicHashSet visited(table_cap, true);

    EnumGlobals g;
    g.num_threads = num_threads;
    g.max_states = 0; // unlimited

    size_t warmup_states = 0;
    bool resumed = false;

    // Allocate thread work BEFORE checkpoint load (enum_warmup normally does this)
    g.work = new ThreadWork[num_threads];

    // Try loading checkpoint
    size_t restored_states = 0;
    if (ckpt_dir && load_checkpoint(ckpt_dir, visited, g.work, num_threads,
                                     warmup_states, restored_states)) {
        resumed = true;
        g.states.store(restored_states, std::memory_order_relaxed);
        size_t loaded_stack = 0;
        for (int t = 0; t < num_threads; ++t)
            loaded_stack += g.work[t].stack.size();
        fprintf(stderr, "Resumed: %zu states, %zuM stack entries\n\n",
                restored_states, loaded_stack / 1000000);
    }

    if (!resumed) {
        size_t warmup_target = (size_t)num_threads * 10000;
        fprintf(stderr, "Warmup (target %zu stack entries)...\n", warmup_target);
        warmup_states = enum_warmup(visited, g, warmup_target);
        fprintf(stderr, "Warmup: %zu states\n\n", warmup_states);
    }

    time_t t0 = time(nullptr);
    time_t last_ckpt = t0;

    std::vector<ThreadStats> thread_stats(num_threads);
    std::vector<std::thread> threads;

    // Main work loop with inline reporting, convergence detection, and checkpointing
    // (Single loop avoids the reporter-thread-dies-after-checkpoint bug)
    bool converged = false;
    size_t prev_states = 0, prev_stack = 0;
    size_t prev_pops = 0, prev_pushes = 0;
    size_t prev_ins_true = 0, prev_ins_false = 0;
    int stall_count = 0;

    g.done.store(false, std::memory_order_relaxed);
    threads.clear();
    for (int t = 0; t < num_threads; ++t)
        threads.emplace_back(enum_worker, std::ref(visited),
                             std::ref(thread_stats[t]), t, std::ref(g));

    while (!converged) {
        std::this_thread::sleep_for(std::chrono::seconds(2));

        size_t s = g.states.load(std::memory_order_relaxed);
        double elapsed = difftime(time(nullptr), t0);
        double rate = elapsed > 0 ? s / elapsed : 0;
        size_t new_states = s - prev_states;

        size_t total_stack = 0;
        size_t total_pops = 0, total_pushes = 0;
        size_t total_ins_true = 0, total_ins_false = 0;
        int active_threads = 0;
        for (int i = 0; i < g.num_threads; ++i) {
            size_t sz = g.work[i].stack.size();
            total_stack += sz;
            total_pops += thread_stats[i].pops;
            total_pushes += thread_stats[i].pushes;
            total_ins_true += thread_stats[i].insert_true;
            total_ins_false += thread_stats[i].insert_false;
            if (sz > 0) active_threads++;
        }

        long long stack_delta = (long long)total_stack - (long long)prev_stack;
        size_t pop_delta = total_pops - prev_pops;
        size_t push_delta = total_pushes - prev_pushes;
        size_t ins_delta = total_ins_true - prev_ins_true;
        size_t dup_delta = total_ins_false - prev_ins_false;
        prev_stack = total_stack;
        prev_pops = total_pops;
        prev_pushes = total_pushes;

        if (new_states < 5000000) {
            fprintf(stderr,
                "\r  St:%zu +%zu | Stk:%zuM(%+lldK) pop:%.1fK push:%.1fK | "
                "ins:%.1fK dup:%.1fK | %dthr %.0fs     ",
                s, new_states, total_stack / 1000000,
                stack_delta / 1000, pop_delta / 1e3, push_delta / 1e3,
                ins_delta / 1e3, dup_delta / 1e3,
                active_threads, elapsed);
        } else {
            fprintf(stderr,
                "\r  St: %12zu | +%zu/2s | Stk: %zuM (%+lldM/2s, %d thr) | "
                "%.1fM/s | %.0fs     ",
                s, new_states, total_stack / 1000000,
                stack_delta / 1000000, active_threads,
                rate / 1e6, elapsed);
        }
        fflush(stderr);

        // Convergence detection
        if (ins_delta == 0 && s > 1000000000) {
            stall_count++;
            if (stall_count >= 15) {
                fprintf(stderr,
                    "\n\n  >>> Discovery converged (0 new inserts for 30s). "
                    "Stopping. <<<\n");
                converged = true;
            }
        } else {
            stall_count = 0;
        }

        // Table full
        if (g.table_full.load(std::memory_order_relaxed))
            converged = true;

        // Periodic checkpoint: stop workers, save, restart
        if (!converged && ckpt_dir &&
            difftime(time(nullptr), last_ckpt) >= ckpt_interval) {
            g.done.store(true, std::memory_order_relaxed);
            for (auto& th : threads) th.join();
            threads.clear();

            save_checkpoint(ckpt_dir, visited, g.work, num_threads, warmup_states,
                            g.states.load(std::memory_order_relaxed));
            last_ckpt = time(nullptr);
            fprintf(stderr, "  Resuming workers...\n\n");

            g.done.store(false, std::memory_order_relaxed);
            for (int t = 0; t < num_threads; ++t)
                threads.emplace_back(enum_worker, std::ref(visited),
                                     std::ref(thread_stats[t]), t, std::ref(g));
        }

        prev_ins_true = total_ins_true;
        prev_ins_false = total_ins_false;
        prev_states = s;
    }

    // Stop workers
    g.done.store(true, std::memory_order_relaxed);
    for (auto& th : threads) th.join();

    // Aggregate: restored states + new states found in this session
    size_t total_states = resumed ? restored_states : warmup_states;
    size_t total_terminal = 0, total_moves = 0;
    size_t total_infinite = 0, total_inner = 0;
    size_t peak_stack = 0, total_steals = 0;

    for (int t = 0; t < num_threads; ++t) {
        total_states   += thread_stats[t].states;
        total_terminal += thread_stats[t].terminal;
        total_moves    += thread_stats[t].moves;
        total_infinite += thread_stats[t].infinite;
        total_inner    += thread_stats[t].inner_row_end;
        total_steals   += thread_stats[t].steals;
        if (thread_stats[t].stack_peak > peak_stack)
            peak_stack = thread_stats[t].stack_peak;
    }

    // Count remaining unprocessed stack entries
    size_t remaining_stack = 0;
    for (int t = 0; t < num_threads; ++t)
        remaining_stack += g.work[t].stack.size();

    // Verify sample
    size_t verify_missed = 0;
    if (remaining_stack > 0) {
        size_t sample_limit = std::min((size_t)1000000, remaining_stack);
        size_t verified = 0;
        fprintf(stderr, "\nVerifying %zuK of %zuM unprocessed stack entries...\n",
                sample_limit / 1000, remaining_stack / 1000000);
        for (int t = 0; t < num_threads && verified < sample_limit; ++t) {
            for (size_t i = 0; i < g.work[t].stack.size() && verified < sample_limit; ++i) {
                BaoState state;
                g.work[t].stack[i].to_bao(state);
                Move moves[MAX_MOVES];
                int nm = state.generate_moves(moves);
                for (int j = 0; j < nm; ++j) {
                    BaoState succ = state;
                    if (succ.make_move(moves[j]) != MoveResult::OK) continue;
                    if (visited.insert(canonical_hash(succ))) {
                        verify_missed++;
                        total_states++;
                    }
                }
                verified++;
            }
        }
        if (verify_missed == 0)
            fprintf(stderr, "  Verified: all %zu sampled states' successors in table.\n", verified);
        else
            fprintf(stderr, "  WARNING: %zu new states found in verification!\n", verify_missed);
    }

    // Final checkpoint
    if (ckpt_dir)
        save_checkpoint(ckpt_dir, visited, g.work, num_threads, warmup_states,
                            g.states.load(std::memory_order_relaxed));

    bool complete = !g.table_full.load() && verify_missed == 0;
    double elapsed = difftime(time(nullptr), t0);

    fprintf(stderr, "\n\n%s\n", complete ? "COMPLETE." : "INCOMPLETE.");

    printf("\nResults\n-------\n");
    printf("Complete:          %s\n", complete ? "YES" : "NO");
    printf("Canonical states:  %zu\n", total_states);
    printf("Terminal states:   %zu\n", total_terminal);
    printf("Moves explored:    %zu\n", total_moves);
    printf("Infinite moves:    %zu\n", total_infinite);
    printf("Inner-row wins:    %zu\n", total_inner);
    printf("Work steals:       %zu\n", total_steals);
    printf("Unprocessed stack: %zuM\n", remaining_stack / 1000000);
    printf("Peak stack/thread: %zu (%zu MB)\n",
           peak_stack, peak_stack * sizeof(CompactState) / (1024*1024));
    printf("Hash load:         %.4f\n", (double)total_states / visited.capacity());
    printf("Elapsed:           %.1f sec\n", elapsed);
    if (elapsed > 0)
        printf("Rate:              %.1fM states/sec\n", total_states / elapsed / 1e6);

    delete[] g.work;
    return complete ? 0 : 1;
}
