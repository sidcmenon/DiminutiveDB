CC        ?= cc
CFLAGS    ?= -std=gnu11 -Wall -Wextra -Werror -O1 -g -MMD -MP
INCLUDES   = -Iinclude -Isrc

SRC_DIR    = src
OBJ_DIR    = build
TEST_DIR   = test
CRASH_DIR  = test/crash
EX_DIR     = examples
TOOL_DIR   = tools
BENCH_DIR  = bench

SRCS      := $(wildcard $(SRC_DIR)/*.c)
OBJS      := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))
LIB       := $(OBJ_DIR)/libkhabibdb.a

TEST_SRCS := $(wildcard $(TEST_DIR)/test_*.c)
TEST_BINS := $(patsubst $(TEST_DIR)/%.c,$(OBJ_DIR)/%,$(TEST_SRCS))

CRASH_BIN := $(OBJ_DIR)/crash_child
EX_BIN    := $(OBJ_DIR)/basic
TOOL_BIN  := $(OBJ_DIR)/khbcheck
BENCH_BINS := $(OBJ_DIR)/bench_ops $(OBJ_DIR)/bench_lock

DEPS      := $(OBJS:.o=.d) $(TEST_BINS:=.d) $(CRASH_BIN).d

.PHONY: all lib test crash check tools examples bench clean

all: lib

lib: $(LIB)

$(LIB): $(OBJS)
	ar rcs $@ $(OBJS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/%: $(TEST_DIR)/%.c $(LIB) | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) -o $@

$(CRASH_BIN): $(CRASH_DIR)/crash_child.c $(LIB) | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

test: $(TEST_BINS)
	@fail=0; \
	for t in $(TEST_BINS); do \
	  echo "=== $$t ==="; \
	  $$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "SOME TESTS FAILED"; exit 1; fi; \
	echo "ALL TESTS PASSED"

$(TOOL_BIN): $(TOOL_DIR)/khbcheck.c $(LIB) | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) -o $@

$(EX_BIN): $(EX_DIR)/basic.c $(LIB) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -Iinclude $< $(LIB) -o $@

$(OBJ_DIR)/bench_ops: $(BENCH_DIR)/bench_ops.c $(LIB) | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) -o $@

$(OBJ_DIR)/bench_lock: $(BENCH_DIR)/bench_lock.c $(LIB) | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) -o $@

bench: $(BENCH_BINS)
	@sh $(BENCH_DIR)/run_bench.sh

tools: $(TOOL_BIN)

examples: $(EX_BIN)

crash: $(CRASH_BIN)
	@KHB_NO_FULLFSYNC=1 sh $(CRASH_DIR)/run_crash.sh $(CRASH_BIN)

check: test crash tools examples

clean:
	rm -rf $(OBJ_DIR)

-include $(DEPS)
