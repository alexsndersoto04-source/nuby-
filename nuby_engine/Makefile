CXX ?= $(shell which clang++ 2>/dev/null || which g++ 2>/dev/null || echo c++)
CXXFLAGS = -std=c++20 -O3 -Wall -Wextra -pthread -I./include
SRCS = src/core/types.cpp \
       src/css/css_value.cpp \
       src/css/css_rule.cpp \
       src/server/web_server.cpp \
       src/main.cpp

# Los headers SÍ son dependencias (antes no lo eran y make no recompilaba
# tras editarlos — builds falsamente "verdes")
HEADERS = $(shell find include -name '*.hpp')

TARGET = bin/nuby_engine

all: $(TARGET)

$(TARGET): $(SRCS) $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET)
	@echo "\033[1;32m[BUILD SUCCESS] Nuby binary generated at $(TARGET)\033[0m"

test: tests/test_engine.cpp src/core/types.cpp src/css/css_value.cpp src/css/css_rule.cpp src/server/web_server.cpp
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) tests/test_engine.cpp src/core/types.cpp src/css/css_value.cpp src/css/css_rule.cpp src/server/web_server.cpp -o bin/test_runner
	@./bin/test_runner

clean:
	rm -rf bin/nuby_engine bin/test_runner
