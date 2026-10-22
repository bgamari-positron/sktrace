CXX ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra
CXXFLAGS += -std=c++20
HOSTCXX ?= g++

all: example tracedump

example: example.o sktrace.o
	$(CXX) $(CXXFLAGS) -pthread -o $@ $^

%.o: %.cpp sktrace.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Runs on the host; the dump format is little-endian like AArch64 Linux.
tracedump: tracedump.cpp
	$(HOSTCXX) -O2 -std=c++20 -Wall -o $@ $<

clean:
	rm -f *.o example tracedump trace.bin

.PHONY: all clean
