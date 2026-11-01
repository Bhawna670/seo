CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall
SRC = src/main.cpp src/Trie.cpp

search_engine: $(SRC) src/Trie.h
	$(CXX) $(CXXFLAGS) $(SRC) -o $@

test: search_engine
	./search_engine --selftest

clean:
	rm -f search_engine

.PHONY: test clean
