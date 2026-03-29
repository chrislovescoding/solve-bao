CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic
CXXFLAGS_FAST = -std=c++17 -O3 -march=native -Wall -Wextra

SRC = src/bao.cpp
HDR = src/bao.h
TEST_SRC = tests/test_engine.cpp
ENUM_SRC = src/enumerate.cpp

.PHONY: all test enumerate clean

all: test enumerate

test: build/test_engine
	./build/test_engine

enumerate: build/enumerate

build/test_engine: $(TEST_SRC) $(SRC) $(HDR) | build
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_SRC) $(SRC)

build/enumerate: $(ENUM_SRC) $(SRC) $(HDR) | build
	$(CXX) $(CXXFLAGS_FAST) -o $@ $(ENUM_SRC) $(SRC)

build:
	mkdir -p build

clean:
	rm -rf build
