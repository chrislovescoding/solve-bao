/*
 * enumerate.cpp — Bao la Kujifunza state space enumerator (v4)
 *
 * DFS with push-all-successors. The game graph has explosive BFS frontiers
 * but manageable DFS stacks (~5% of total states).
 *
 * Memory model:
 *   Hash table: --mem-gb allocation (8 bytes per entry)
 *   DFS stack:  grows dynamically (~40 bytes per entry, peaks at ~5% of N)
 *
 * Usage: enumerate [--mem-gb N]  (default: 6, for the hash table only)
 */

#include "bao.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

// ---------------------------------------------------------------------------
// Open-addressing hash set (64-bit keys, linear probing)
// ---------------------------------------------------------------------------

class HashSet64 {
public:
    explicit HashSet64(size_t capacity) {
        capacity_ = 1;
        while (capacity_ < capacity) capacity_ <<= 1;
        mask_ = capacity_ - 1;
        count_ = 0;
        table_ = (uint64_t*)calloc(capacity_, sizeof(uint64_t));
        if (!table_) {
            fprintf(stderr, "FATAL: alloc failed (%zu MB)\n",
                    capacity_ * 8 / (1024*1024));
            exit(1);
        }
    }
    ~HashSet64() { free(table_); }

    bool insert(uint64_t h) {
        if (h == 0) h = 1;
        size_t slot = h & mask_;
        while (true) {
            uint64_t e = table_[slot];
            if (e == 0) { table_[slot] = h; count_++; return true; }
            if (e == h) return false;
            slot = (slot + 1) & mask_;
        }
    }

    size_t count()    const { return count_; }
    size_t capacity() const { return capacity_; }
    double load()     const { return (double)count_ / capacity_; }

private:
    uint64_t* table_;
    size_t capacity_, mask_, count_;
};

// ---------------------------------------------------------------------------
// Canonicalize: compare on-the-fly
// ---------------------------------------------------------------------------

static void canonicalize_state(BaoState& s) {
    bool do_reflect = false;
    for (int pos = 0; pos < TOTAL_PITS; ++pos) {
        int si = pos >> 4, i = pos & 15;
        uint8_t orig = s.pits[pos];
        uint8_t refl = s.pits[(si << 4) + REFLECT[i]];
        if (refl < orig) { do_reflect = true; break; }
        if (refl > orig) break;
    }
    if (do_reflect) s.reflect_lr();
    s.rehash();
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

struct Stats {
    size_t states        = 0;
    size_t terminal      = 0;
    size_t moves         = 0;
    size_t infinite      = 0;
    size_t inner_row_end = 0;
    size_t stack_peak    = 0;
    time_t t0            = 0;
};

static Stats st;

static void report() {
    double elapsed = difftime(time(nullptr), st.t0);
    double rate = elapsed > 0 ? st.states / elapsed : 0;
    fprintf(stderr,
        "\r  States: %12zu | Terminal: %9zu | Stack: %9zu | "
        "%.0f st/s | %.0fs   ",
        st.states, st.terminal, st.stack_peak, rate, elapsed);
    fflush(stderr);
}

// ---------------------------------------------------------------------------
// DFS Enumerator
// ---------------------------------------------------------------------------

static bool enumerate(HashSet64& visited) {
    std::vector<BaoState> stack;
    stack.reserve(1 << 20); // pre-alloc 1M entries

    BaoState start;
    start.init_start();
    canonicalize_state(start);
    visited.insert(start.hash);
    stack.push_back(start);
    st.states = 1;

    size_t next_report = 500000;

    while (!stack.empty()) {
        BaoState state = stack.back();
        stack.pop_back();

        if (stack.size() > st.stack_peak)
            st.stack_peak = stack.size();

        if (state.is_terminal()) {
            st.terminal++;
            continue;
        }

        Move moves[MAX_MOVES];
        int n = state.generate_moves(moves);

        for (int i = 0; i < n; ++i) {
            st.moves++;

            BaoState succ = state;
            MoveResult r = succ.make_move(moves[i]);

            if (r == MoveResult::INFINITE)        { st.infinite++; continue; }
            if (r == MoveResult::INNER_ROW_EMPTY) { st.inner_row_end++; continue; }

            canonicalize_state(succ);

            if (!visited.insert(succ.hash))
                continue;

            st.states++;

            if (st.states >= next_report) {
                report();
                next_report += 500000;
            }

            if (visited.load() > 0.75) {
                fprintf(stderr,
                    "\n\nTABLE FULL at %zu states (load %.2f, cap %zu)\n",
                    visited.count(), visited.load(), visited.capacity());
                return false;
            }

            stack.push_back(succ);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    size_t mem_gb = 6;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mem-gb") == 0 && i + 1 < argc)
            mem_gb = (size_t)atol(argv[++i]);
    }

    size_t table_bytes = mem_gb * (size_t)1024 * 1024 * 1024;
    size_t table_cap   = table_bytes / sizeof(uint64_t);
    size_t max_states  = (size_t)(table_cap * 0.70);

    printf("Bao la Kujifunza — State Space Enumerator v4 (DFS)\n");
    printf("===================================================\n");
    printf("Hash table:   %zu GB (%zu M entries, ~%zu M states max)\n",
           mem_gb, table_cap / 1000000, max_states / 1000000);
    printf("Stack:        dynamic (40 bytes/entry)\n");
    printf("\n");

    zobrist_init();
    HashSet64 visited(table_cap);

    st.t0 = time(nullptr);
    fprintf(stderr, "Running...\n");

    bool complete = enumerate(visited);

    report();
    fprintf(stderr, "\n\n%s\n", complete ? "COMPLETE." : "INCOMPLETE.");

    double elapsed = difftime(time(nullptr), st.t0);
    printf("\nResults\n-------\n");
    printf("Complete:          %s\n", complete ? "YES" : "NO");
    printf("Canonical states:  %zu\n", st.states);
    printf("Terminal states:   %zu\n", st.terminal);
    printf("Moves explored:    %zu\n", st.moves);
    printf("Infinite moves:    %zu\n", st.infinite);
    printf("Inner-row wins:    %zu\n", st.inner_row_end);
    printf("Peak stack:        %zu (%zu MB)\n",
           st.stack_peak, st.stack_peak * sizeof(BaoState) / (1024*1024));
    printf("Hash load:         %.4f\n", visited.load());
    printf("Elapsed:           %.1f sec\n", elapsed);
    if (elapsed > 0)
        printf("Rate:              %.0f states/sec\n", st.states / elapsed);

    return complete ? 0 : 1;
}
