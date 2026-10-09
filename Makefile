# Ravager Fury: C11 engine with an embedded network.
CC ?= gcc
CFLAGS ?= -O3 -std=c11
override CFLAGS += -Wall -Wextra -Wno-unused-parameter -Wno-missing-braces -pthread
CPPFLAGS += -Isrc -Innue -Itests -Ilocal_zstd/usr/include
LDFLAGS ?= -Llocal_zstd/usr/lib/x86_64-linux-gnu
LDLIBS += -lm -lzstd
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  ifeq ($(shell uname -m),x86_64)
    ifeq (,$(findstring ravager.exe,$(MAKECMDGOALS)))
      ARCH_FLAGS ?= -march=native
      override CFLAGS += $(ARCH_FLAGS)
    endif
  endif
endif
# Changing EVALFILE requires make clean before rebuilding.
EVALFILE ?= nets/Ravager_NET.nnue.zst
ifneq ($(strip $(EVALFILE)),)
  override CPPFLAGS += -DEVALFILE='"$(EVALFILE)"'
  NET_DEP := $(EVALFILE)
endif
CORE_SRC := src/bitboard.c src/board.c src/movegen.c src/see.c src/evaluate.c src/params.c \
            src/tt.c src/search.c nnue/nnue.c nnue/inference.c src/tb/tbprobe.c src/tb_syzygy.c
SRC := $(CORE_SRC) tests/diagnostics.c src/uci.c
OBJ := $(SRC:.c=.o)
DEP := $(OBJ:.o=.d)
TARGET := ravager
all: $(TARGET)
$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)
%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<
nnue/nnue.o: $(NET_DEP)
ravager.exe: $(SRC) $(wildcard src/*.h nnue/*.h tests/*.h src/tb/*.h) $(NET_DEP)
	x86_64-w64-mingw32-gcc $(CPPFLAGS) $(CFLAGS) $(ARCH_FLAGS) -o $@ $(SRC) $(LDLIBS) -static
debug: CFLAGS += -O0 -g -fsanitize=address,undefined
debug: clean $(TARGET)
TUN_SRC := tests/tuner.c src/params.c src/board.c src/bitboard.c src/evaluate.c
tuner: $(TUN_SRC) $(wildcard src/*.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(TUN_SRC) $(LDLIBS)
test: $(TARGET)
	python3 tests/regression.py ./$(TARGET)
bench: $(TARGET)
	./$(TARGET) bench
orthodox-bench: $(CORE_SRC) tests/orthodox_bench.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)
clean:
	rm -f $(OBJ) $(DEP) src/nnue.o src/tuner.o $(TARGET) ravager.exe tuner orthodox-bench
-include $(DEP)
.PHONY: all clean test bench debug tuner
