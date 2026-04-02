#include "../src/enumerate_core.h"
#include <chrono>
#include <vector>
#include <string>
#include <algorithm>

using Clock = std::chrono::high_resolution_clock;

static uint64_t splitmix64(uint64_t& x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

enum class ProbeVariant {
    CurrentCAS,
    LoadThenCAS,
    LoadOnly
};

struct ThreadResult {
    uint64_t lookups = 0;
    uint64_t inserted = 0;
    uint64_t duplicates = 0;
    uint64_t probes = 0;
    double elapsed_sec = 0.0;
};

template<ProbeVariant VARIANT>
static bool probe_hash(AtomicHashSet& ht, uint64_t h, uint64_t& probes) {
    std::atomic<uint32_t>* table = ht.raw_table();
    uint32_t tag = AtomicHashSet::tag_for(h);
    size_t slot = ht.slot_for(h);

    while (true) {
        probes++;

        if constexpr (VARIANT == ProbeVariant::CurrentCAS) {
            uint32_t expected = 0;
            if (table[slot].compare_exchange_weak(
                    expected, tag,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return true;
            }
            if (expected == tag)
                return false;
        } else if constexpr (VARIANT == ProbeVariant::LoadThenCAS) {
            uint32_t cur = table[slot].load(std::memory_order_relaxed);
            if (cur == tag)
                return false;
            if (cur == 0) {
                uint32_t expected = 0;
                if (table[slot].compare_exchange_weak(
                        expected, tag,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    return true;
                }
                if (expected == tag)
                    return false;
            }
        } else {
            uint32_t cur = table[slot].load(std::memory_order_relaxed);
            if (cur == tag)
                return false;
            if (cur == 0)
                return true;
        }

        slot = ht.next_slot(slot);
    }
}

template<ProbeVariant VARIANT>
static void bench_worker(AtomicHashSet& ht,
                         const std::vector<uint64_t>& dup_pool,
                         uint64_t lookups,
                         int tid,
                         ThreadResult& out) {
#ifdef _WIN32
    SetThreadAffinityMask(GetCurrentThread(), 1ULL << (tid % 64));
#endif

    uint64_t rng = 0x123456789ABCDEF0ULL ^ (uint64_t)(tid + 1) * 0x9E3779B97F4A7C15ULL;
    auto t0 = Clock::now();

    for (uint64_t i = 0; i < lookups; ++i) {
        uint64_t h = dup_pool[(size_t)(splitmix64(rng) % dup_pool.size())];
        ht.prefetch(h);
        bool inserted = probe_hash<VARIANT>(ht, h, out.probes);
        out.lookups++;
        if (inserted) out.inserted++;
        else out.duplicates++;
    }

    out.elapsed_sec = std::chrono::duration<double>(Clock::now() - t0).count();
}

template<ProbeVariant VARIANT>
static ThreadResult run_variant(AtomicHashSet& ht,
                                const std::vector<uint64_t>& dup_pool,
                                uint64_t lookups_per_thread,
                                int num_threads) {
    std::vector<ThreadResult> per_thread(num_threads);
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back(bench_worker<VARIANT>,
                             std::ref(ht), std::cref(dup_pool),
                             lookups_per_thread, t, std::ref(per_thread[t]));
    }
    for (auto& th : threads)
        th.join();

    ThreadResult total;
    for (const auto& r : per_thread) {
        total.lookups += r.lookups;
        total.inserted += r.inserted;
        total.duplicates += r.duplicates;
        total.probes += r.probes;
        if (r.elapsed_sec > total.elapsed_sec)
            total.elapsed_sec = r.elapsed_sec;
    }
    return total;
}

static void print_result(const char* label, const ThreadResult& r) {
    double lookups_per_sec = r.elapsed_sec > 0 ? r.lookups / r.elapsed_sec : 0.0;
    double avg_probes = r.lookups > 0 ? (double)r.probes / r.lookups : 0.0;
    double ns_per_lookup = lookups_per_sec > 0 ? 1e9 / lookups_per_sec : 0.0;

    printf("%s\n", label);
    printf("  Lookups:        %llu\n", (unsigned long long)r.lookups);
    printf("  Throughput:     %.3fM lookups/sec\n", lookups_per_sec / 1e6);
    printf("  Latency:        %.1f ns/lookup\n", ns_per_lookup);
    printf("  Avg probes:     %.4f\n", avg_probes);
    printf("  Inserted:       %llu\n", (unsigned long long)r.inserted);
    printf("  Duplicates:     %llu\n", (unsigned long long)r.duplicates);
    printf("\n");
}

int main(int argc, char* argv[]) {
    size_t mem_gb = 1;
    int num_threads = (int)std::thread::hardware_concurrency();
    if (num_threads < 1) num_threads = 8;
    double load = 0.54;
    uint64_t lookups_per_thread = 2000000;
    size_t dup_pool_target = 1000000;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--mem-gb") && i + 1 < argc)
            mem_gb = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc)
            num_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--load") && i + 1 < argc)
            load = atof(argv[++i]);
        else if (!strcmp(argv[i], "--lookups") && i + 1 < argc)
            lookups_per_thread = (uint64_t)atoll(argv[++i]);
        else if (!strcmp(argv[i], "--dup-pool") && i + 1 < argc)
            dup_pool_target = (size_t)atoll(argv[++i]);
    }

    size_t table_bytes = mem_gb * (size_t)1024 * 1024 * 1024;
    size_t table_cap = table_bytes / sizeof(uint32_t);
    size_t target_fill = (size_t)(table_cap * load);

    printf("Bao Hash Probe Benchmark\n");
    printf("========================\n");
    printf("Table:          %zu GB (%zuM slots)\n", mem_gb, table_cap / 1000000ULL);
    printf("Threads:        %d\n", num_threads);
    printf("Target load:    %.2f\n", load);
    printf("Lookups/thread: %llu\n", (unsigned long long)lookups_per_thread);
    printf("Duplicate pool: %zu\n\n", dup_pool_target);

    AtomicHashSet visited(table_cap, false);
    std::vector<uint64_t> dup_pool;
    dup_pool.reserve(std::min(dup_pool_target, target_fill));

    printf("Prefill: target %zuM entries...\n", target_fill / 1000000ULL);
    uint64_t seed = 0xCAFEBABE12345678ULL;
    size_t inserted = 0;
    auto fill_t0 = Clock::now();
    while (inserted < target_fill) {
        uint64_t h = splitmix64(seed);
        if (visited.insert(h)) {
            if (dup_pool.size() < dup_pool_target)
                dup_pool.push_back(h);
            inserted++;
        }
    }
    double fill_sec = std::chrono::duration<double>(Clock::now() - fill_t0).count();
    printf("Prefill done:    %zu entries in %.1fs (%.1fM inserts/sec)\n\n",
           inserted, fill_sec, inserted / fill_sec / 1e6);

    ThreadResult current = run_variant<ProbeVariant::CurrentCAS>(
        visited, dup_pool, lookups_per_thread, num_threads);
    ThreadResult load_then_cas = run_variant<ProbeVariant::LoadThenCAS>(
        visited, dup_pool, lookups_per_thread, num_threads);
    ThreadResult load_only = run_variant<ProbeVariant::LoadOnly>(
        visited, dup_pool, lookups_per_thread, num_threads);

    print_result("Current insert() logic (CAS every probe)", current);
    print_result("Load-first probe, CAS only on empty slot", load_then_cas);
    print_result("Read-only upper bound (load only)", load_only);

    double current_lps = current.elapsed_sec > 0 ? current.lookups / current.elapsed_sec : 0.0;
    double loadcas_lps = load_then_cas.elapsed_sec > 0 ? load_then_cas.lookups / load_then_cas.elapsed_sec : 0.0;
    double loadonly_lps = load_only.elapsed_sec > 0 ? load_only.lookups / load_only.elapsed_sec : 0.0;

    printf("Speedup vs current: load-first %.2fx, load-only %.2fx\n",
           current_lps > 0 ? loadcas_lps / current_lps : 0.0,
           current_lps > 0 ? loadonly_lps / current_lps : 0.0);
    printf("\n");
    printf("This benchmark isolates the suspected production bottleneck: duplicate-heavy\n");
    printf("hash probes into a table large enough to miss cache. If the CAS-every-probe\n");
    printf("variant collapses while load-first stays much faster, the tail is memory/\n");
    printf("coherence bound in AtomicHashSet::insert rather than compute bound in make_move.\n");

    return 0;
}