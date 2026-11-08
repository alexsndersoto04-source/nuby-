# CORTEX-LM — compilacion del motor LIF puro y herramientas corticales
# Objetivo: binarios C++ puros sin dependencias externas.

CXX       ?= $(shell command -v g++ >/dev/null 2>&1 && echo g++ || echo clang++)
CXXFLAGS  ?= -O3 -march=native -std=c++17 -fno-math-errno -funroll-loops
WARN      ?= -Wall -Wextra -Wno-unused-parameter
INC       := -Iengine
CORE      := engine/cortex_lm.cpp
HDR       := engine/cortex_lm.hpp

BIN_MAIN  := bin/cortex_lm
BIN_BRAIN := bin/cerebro
BIN_BENCH := bin/bench_cortical

.PHONY: all portable clean smoke

all: $(BIN_MAIN) $(BIN_BRAIN) $(BIN_BENCH)

$(BIN_MAIN): $(CORE) engine/cortex_lm_main.cpp $(HDR)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(WARN) $(INC) $(CORE) engine/cortex_lm_main.cpp -o $(BIN_MAIN)

$(BIN_BRAIN): $(CORE) tools/cerebro.cpp $(HDR)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(WARN) $(INC) $(CORE) tools/cerebro.cpp -o $(BIN_BRAIN)

$(BIN_BENCH): $(CORE) tools/bench_cortical.cpp $(HDR)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(WARN) $(INC) $(CORE) tools/bench_cortical.cpp -o $(BIN_BENCH)

# Version sin -march=native, optimizada (-O3) para Termux ARM64, Kaggle o cualquier CPU
portable:
	@mkdir -p bin
	$(CXX) -O3 -std=c++17 -fno-math-errno -funroll-loops $(WARN) $(INC) $(CORE) engine/cortex_lm_main.cpp -o $(BIN_MAIN)
	$(CXX) -O3 -std=c++17 -fno-math-errno -funroll-loops $(WARN) $(INC) $(CORE) tools/cerebro.cpp -o $(BIN_BRAIN)
	$(CXX) -O3 -std=c++17 -fno-math-errno -funroll-loops $(WARN) $(INC) $(CORE) tools/bench_cortical.cpp -o $(BIN_BENCH)

smoke: $(BIN_MAIN)
	./$(BIN_MAIN) smoke /tmp/cortex_smoke.bin

clean:
	rm -rf bin
