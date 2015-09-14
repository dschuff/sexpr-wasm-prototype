.SUFFIXES:

ALL = sexpr-wasm sexpr-cpp
CC = gcc
CXX = g++
CFLAGS = -Wall -Werror -g -O0

PARSER_OBJS = out/wasm-parse.o
WASMGEN_OBJS = out/sexpr-wasm.o out/wasm-gen.o
HEADERS = src/wasm.h src/wasm-parse.h src/hash.h

LLVM_PATH ?= /s/llvm-upstream/release_37/install
LLVM_CONFIG = $(LLVM_PATH)/bin/llvm-config

LLVM_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags)
LLVM_LIBS := $(shell $(LLVM_CONFIG) --libs)
LLVM_LIBDIR := $(shell $(LLVM_CONFIG) --libdir)
LLVM_SYSTEMLIBS := $(shell  $(LLVM_CONFIG) --system-libs)
LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags) -Wl,-rpath=$(LLVM_LIBDIR)



.PHONY: all
all: $(addprefix out/,$(ALL))

out/:
	mkdir $@

out/%.o: src/%.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<
out/%.o: src/%.cc $(HEADERS) src/wasm-cpp.h
	$(CXX) $(LLVM_CXXFLAGS) -Wno-format $(CFLAGS) -c -o $@ $<

out/sexpr-wasm: $(PARSER_OBJS) $(WASMGEN_OBJS) $(HEADERS)  | out
	$(CC) -o $@ $(PARSER_OBJS) $(WASMGEN_OBJS)

out/sexpr-llvm: $(PARSER_OBJS) $(HEADERS) out/sexpr-llvm.o
	$(CXX) -o $@ $(PARSER_OBJS) out/sexpr-llvm.o $(LLVM_LDFLAGS) $(LLVM_LIBS)

out/sexpr-cpp: $(PARSER_OBJS) $(HEADERS) out/sexpr-cpp.o out/wasm-cpp.o
	$(CXX) -o $@ $(PARSER_OBJS) out/sexpr-cpp.o out/wasm-cpp.o $(LLVM_LDFLAGS) $(LLVM_LIBS)

src/hash.h: src/hash.txt
	gperf --compare-strncmp --readonly-tables --struct-type $< --output-file $@

#### TESTS ####
TEST_EXES=$(shell python test/run-tests.py --list-exes)

.PHONY: test
test: $(TEST_EXES)
	@python test/run-tests.py

#### CLEAN ####
.PHONY: clean
clean:
	rm -rf out
