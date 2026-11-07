# CORTEX-LM — compilacion del motor
# Objetivo: un solo binario sin dependencias externas.

CXX      ?= g++
CXXFLAGS ?= -O3 -march=native -std=c++17 -fno-math-errno -funroll-loops
WARN     ?= -Wall -Wextra -Wno-unused-parameter
INC      := -Iengine
SRC      := engine/cortex_lm.cpp engine/cortex_lm_main.cpp
BIN      := bin/cortex_lm

.PHONY: all clean smoke

all: $(BIN)

$(BIN): $(SRC) engine/cortex_lm.hpp
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(WARN) $(INC) $(SRC) -o $(BIN)

# version sin -march=native, para que corra en cualquier maquina (Kaggle, Termux)
portable:
	@mkdir -p bin
	$(CXX) -O2 -std=c++17 $(WARN) $(INC) $(SRC) -o $(BIN)

smoke: $(BIN)
	./$(BIN) smoke /tmp/cortex_smoke.bin

clean:
	rm -rf bin
