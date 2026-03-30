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

.PHONY: all test enumerate enumerate_par solver bench bench_par bench_solver clean

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

build/benchmark: $(BENCH_SRC) $(SRC) $(HDR) | build
	$(CXX) $(CXXFLAGS_FAST) -o $@ $(BENCH_SRC) $(SRC)

build/benchmark_parallel: $(BENCH_PAR_SRC) $(SRC) $(HDR) src/enumerate_core.h | build
	$(CXX) $(CXXFLAGS_FAST) -funroll-loops -o $@ $(BENCH_PAR_SRC) $(SRC) -lpthread

build/benchmark_solver: $(BENCH_SOLVER_SRC) $(SRC) $(HDR) src/solver_core.h | build
	$(CXX) $(CXXFLAGS_FAST) -funroll-loops -o $@ $(BENCH_SOLVER_SRC) $(SRC) -lpthread

build:
	mkdir -p build

clean:
	rm -rf build
