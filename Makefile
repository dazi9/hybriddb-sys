CXX ?= g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Werror -pedantic -pthread -Iinclude

BUILD_DIR := build
LIB_SRCS := src/disk_manager.cpp src/buffer_pool.cpp src/wal.cpp src/bloom.cpp src/btree.cpp src/lsm.cpp
LIB_OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(LIB_SRCS))

TEST_BIN := $(BUILD_DIR)/test_runner
BENCH_BIN := $(BUILD_DIR)/bench_experiments
CLI_BIN := $(BUILD_DIR)/hybriddb_cli

.PHONY: all test bench clean

all: $(TEST_BIN) $(BENCH_BIN) $(CLI_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TEST_BIN): $(LIB_OBJS) tests/test_main.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(LIB_OBJS) tests/test_main.cpp -o $(TEST_BIN)

$(BENCH_BIN): $(LIB_OBJS) benchmarks/bench_experiments.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(LIB_OBJS) benchmarks/bench_experiments.cpp -o $(BENCH_BIN)

$(CLI_BIN): $(LIB_OBJS) src/cli_main.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(LIB_OBJS) src/cli_main.cpp -o $(CLI_BIN)

test: $(TEST_BIN)
	./$(TEST_BIN)

bench: $(BENCH_BIN)
	./$(BENCH_BIN) results

clean:
	rm -rf $(BUILD_DIR)
