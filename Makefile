CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic
CXXFLAGS_FAST = -std=c++17 -O3 -march=native -Wall -Wextra -flto -fomit-frame-pointer -fno-stack-protector

SRC = src/bao.cpp
HDR = src/bao.h
TEST_SRC = tests/test_engine.cpp
ENUM_SRC = src/enumerate.cpp
ENUM_PAR_SRC = src/enumerate_parallel.cpp
SOLVER_SRC = src/solver.cpp
BENCH_SRC = tests/benchmark.cpp
BENCH_PAR_SRC = tests/benchmark_parallel.cpp
BENCH_SOLVER_SRC = tests/benchmark_solver.cpp

.PHONY: all test enumerate enumerate_par solver bench bench_par bench_solver bench_move bench_hash profile_tail clean
.PHONY: enumerate_gpu bench_gpu

all: test enumerate enumerate_par solver

test: build/test_engine
	./build/test_engine

enumerate: build/enumerate
enumerate_par: build/enumerate_par
solver: build/solver

build/test_engine: $(TEST_SRC) $(SRC) $(HDR) | build
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_SRC) $(SRC)

build/enumerate: $(ENUM_SRC) $(SRC) $(HDR) | build
	$(CXX) $(CXXFLAGS_FAST) -o $@ $(ENUM_SRC) $(SRC)

build/enumerate_par: $(ENUM_PAR_SRC) $(SRC) $(HDR) src/enumerate_core.h | build
	$(CXX) $(CXXFLAGS_FAST) -funroll-loops -o $@ $(ENUM_PAR_SRC) $(SRC) -lpthread

build/solver: $(SOLVER_SRC) $(SRC) $(HDR) src/solver_core.h | build
	$(CXX) $(CXXFLAGS_FAST) -funroll-loops -o $@ $(SOLVER_SRC) $(SRC) -lpthread

bench: build/benchmark
	./build/benchmark

bench_par: build/benchmark_parallel
	./build/benchmark_parallel

bench_solver: build/benchmark_solver
	./build/benchmark_solver

bench_move: build/benchmark_make_move
	./build/benchmark_make_move

bench_hash: build/benchmark_hash_table
	./build/benchmark_hash_table

profile_tail: build/profile_tail
	./build/profile_tail

build/benchmark: $(BENCH_SRC) $(SRC) $(HDR) | build
	$(CXX) $(CXXFLAGS_FAST) -o $@ $(BENCH_SRC) $(SRC)

build/benchmark_parallel: $(BENCH_PAR_SRC) $(SRC) $(HDR) src/enumerate_core.h | build
	$(CXX) $(CXXFLAGS_FAST) -funroll-loops -o $@ $(BENCH_PAR_SRC) $(SRC) -lpthread

build/benchmark_solver: $(BENCH_SOLVER_SRC) $(SRC) $(HDR) src/solver_core.h | build
	$(CXX) $(CXXFLAGS_FAST) -funroll-loops -o $@ $(BENCH_SOLVER_SRC) $(SRC) -lpthread

build/benchmark_make_move: tests/benchmark_make_move.cpp $(SRC) $(HDR) | build
	$(CXX) $(CXXFLAGS_FAST) -funroll-loops -o $@ tests/benchmark_make_move.cpp $(SRC)

build/profile_tail: tests/profile_tail.cpp $(SRC) $(HDR) src/enumerate_core.h src/bao_profile.h | build
	$(CXX) $(CXXFLAGS_FAST) -DBAO_ENABLE_MOVE_PROFILE -funroll-loops -o $@ tests/profile_tail.cpp $(SRC) -lpthread

build/benchmark_hash_table: tests/benchmark_hash_table.cpp $(SRC) $(HDR) src/enumerate_core.h | build
	$(CXX) $(CXXFLAGS_FAST) -funroll-loops -o $@ tests/benchmark_hash_table.cpp $(SRC) -lpthread

# GPU targets (require nvcc + CUDA)
NVCC = nvcc
NVCC_FLAGS = -std=c++17 -O3 -Xcompiler "-O3 -Wall -funroll-loops" --use_fast_math

enumerate_gpu: build/enumerate_gpu
bench_gpu: build/benchmark_gpu
	./build/benchmark_gpu

build/enumerate_gpu: src/enumerate_gpu.cu src/bao_gpu.cuh src/enumerate_core.h $(SRC) $(HDR) | build
	$(NVCC) $(NVCC_FLAGS) -o $@ src/enumerate_gpu.cu $(SRC) -lpthread

build/benchmark_gpu: tests/benchmark_gpu.cu src/bao_gpu.cuh $(SRC) $(HDR) | build
	$(NVCC) $(NVCC_FLAGS) -o $@ tests/benchmark_gpu.cu $(SRC)

build:
	mkdir -p build

clean:
	rm -rf build

