CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2
BUILD    := build
LIB_SRC  := $(addprefix src/,profile.cpp impairment.cpp sim.cpp netem.cpp proxy.cpp probe.cpp)
LIB_OBJ  := $(LIB_SRC:src/%.cpp=$(BUILD)/%.o)
BIN      := chaos
# Records the compiler flags so that changing CXXFLAGS (e.g. adding sanitizers) rebuilds everything
# instead of silently re-using stale object files.
STAMP    := $(BUILD)/.flags

.PHONY: all test e2e cli check clean FORCE
all: $(BIN)

FORCE:
$(STAMP): FORCE
	@mkdir -p $(BUILD)
	@echo '$(CXX) $(CXXFLAGS)' | cmp -s - $@ || echo '$(CXX) $(CXXFLAGS)' > $@

$(BUILD)/%.o: src/%.cpp $(wildcard src/*.h) $(STAMP)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BIN): $(LIB_OBJ) $(BUILD)/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/unit_tests: tests/test_main.cpp $(LIB_OBJ) $(STAMP)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc tests/test_main.cpp $(LIB_OBJ) -o $@

test: $(BUILD)/unit_tests
	./$(BUILD)/unit_tests

e2e: $(BIN)
	./tests/e2e_proxy.sh

cli: $(BIN)
	./tests/cli_regress.sh

check: test e2e cli

clean:
	rm -rf $(BUILD) $(BIN)
