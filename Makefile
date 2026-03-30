CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic
CXXFLAGS_FAST = -std=c++17 -O3 -march=native -Wall -Wextra -flto

SRC = src/bao.cpp
HDR = src/bao.h
TEST_SRC = tests/test_engine.cpp
ENUM_SRC = src/enumerate.cpp
ENUM_PAR_SRC = src/enumerate_parallel.cpp

BENCH_SRC = tests/benchmark.cpp
BENCH_PAR_SRC = tests/benchmark_parallel.cpp

.PHONY: all test enumerate enumerate_par bench bench_par clean pgo_train

all: test enumerate enumerate_par

test: build/test_engine
	./build/test_engine

enumerate: build/enumerate

enumerate_par: build/enumerate_par

build/test_engine: $(TEST_SRC) $(SRC) $(HDR) | build
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_SRC) $(SRC)

build/enumerate: $(ENUM_SRC) $(SRC) $(HDR) | build
	$(CXX) $(CXXFLAGS_FAST) -o $@ $(ENUM_SRC) $(SRC)

build/enumerate_par: $(ENUM_PAR_SRC) $(SRC) $(HDR) | build
	$(CXX) $(CXXFLAGS_FAST) -o $@ $(ENUM_PAR_SRC) $(SRC) -lpthread

bench: build/benchmark
	./build/benchmark

bench_par: build/benchmark_parallel
	./build/benchmark_parallel

build/benchmark: $(BENCH_SRC) $(SRC) $(HDR) | build
	$(CXX) $(CXXFLAGS_FAST) -o $@ $(BENCH_SRC) $(SRC)

build/benchmark_parallel: $(BENCH_PAR_SRC) $(SRC) $(HDR) | build
	$(CXX) $(CXXFLAGS_FAST) -fprofile-use=build/pgo -fprofile-correction -funroll-loops -o $@ $(BENCH_PAR_SRC) $(SRC) -lpthread

# PGO training: use same output name so gcda files match
pgo_train: | build
	rm -rf build/pgo
	$(CXX) $(CXXFLAGS_FAST) -fprofile-generate=build/pgo -o build/benchmark_parallel $(BENCH_PAR_SRC) $(SRC) -lpthread
	-./build/benchmark_parallel --states 5000000

build:
	mkdir -p build

clean:
	rm -rf build
