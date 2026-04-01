# GPU Optimization Agent — Bao la Kujifunza State Enumerator

## Objective

Maximize `GPU expand states/sec` from `make bench_gpu` while maintaining **perfect correctness** (bit-identical to CPU reference).

## Running Benchmarks

This code must be compiled and run on a GPU VM. Use SSH:

```bash
# SSH to GPU VM and run benchmark:
gcloud compute ssh bao-gpu --zone=us-central1-a --command \
  "cd solve-bao && git pull && make bench_gpu 2>&1"
```

## Correctness Checks

`make bench_gpu` verifies every GPU successor against CPU reference:
- Successor pits must match exactly
- Canonical hash must match exactly
- is_terminal flag must match

If ANY mismatch: `Correctness: FAIL`. Revert immediately.

## Files You CAN Modify

- `src/bao_gpu.cuh` — CUDA game engine (all device functions)
- `Makefile` — CUDA compiler flags only

## Files You CANNOT Modify

- `tests/benchmark_gpu.cu` — GPU benchmark (correctness oracle)
- `src/bao.h` — CPU game engine (the reference)
- `src/bao.cpp` — CPU init functions
- `tests/test_engine.cpp`, `tests/benchmark.cpp`, etc.

## Optimization Ideas

### HIGH IMPACT

1. **Move ZOBRIST_TABLE to shared memory**: Constant memory serializes divergent accesses within a warp. Loading the 16KB table into `__shared__` memory at kernel start avoids this.

2. **Sort input batch by max pit count**: Groups similar-cost states in the same warp, reducing divergence in the relay chain loop.

3. **Persistent threads with work queue**: Instead of one-thread-per-state, use an atomic counter. Threads pull next state when done, keeping all warp lanes busy.

4. **Reduce output transfer size**: Only transfer (hash, is_terminal) per successor. For new states, re-run make_move on CPU to get the pits. Trades CPU compute for 5x less PCIe bandwidth.

5. **Use `__ldg()` for read-only data**: Mark input_pits loads with `__ldg()` to use the texture cache path.

### MEDIUM IMPACT

6. **Tune block size**: Try 128, 256, 512 threads per block.
7. **Use cooperative groups for hash probing**: Not applicable here (hash table is on CPU), but relevant if we move dedup to GPU.
8. **Register pressure reduction**: Reduce local arrays if possible to increase occupancy.

## Build Commands

```bash
make bench_gpu    # Correctness + performance (PRIMARY)
make bench        # CPU ground truth (must still pass)
make test         # Engine unit tests (must still pass)
```

## Workflow

```
1. Record baseline: make bench_gpu → GPU expand states/sec
2. Apply change to src/bao_gpu.cuh
3. make bench_gpu → must print "Correctness: PASS"
4. Record new GPU expand states/sec
5. If improved → KEEP. If not → REVERT.
```
