# Ravager 2.0 — Makefile
# Pure HCE chess engine. No NNUE, no books, no tablebases.

CC       ?= gcc
CFLAGS   ?= -O3 -std=c11
CFLAGS   += -Wall -Wextra -Wno-unused-parameter -Wno-missing-braces -pthread
LDFLAGS  ?=
LDLIBS   += -lm

# Native build for maximum local performance (BMI2/POPCNT included).
# For a portable release binary: make CFLAGS="-O3 -std=c11 -mbmi2 -mpopcnt"
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  ARCH := $(shell uname -m)
  ifeq ($(ARCH),x86_64)
    CFLAGS += -march=native
  endif
endif

SRC := src/bitboard.c src/board.c src/movegen.c src/see.c src/evaluate.c src/params.c \
       src/tt.c src/search.c src/uci.c
OBJ := $(SRC:.c=.o)

TARGET := ravager

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

%.o: %.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

# Windows cross-build
ravager.exe: $(SRC) $(wildcard src/*.h)
	x86_64-w64-mingw32-gcc $(CFLAGS) -o $@ $(SRC) $(LDLIBS) -static

debug: CFLAGS += -O0 -g -fsanitize=address,undefined
debug: clean $(TARGET)

TUN_SRC := src/tuner.c src/params.c src/board.c src/bitboard.c src/evaluate.c

tuner: $(TUN_SRC) $(wildcard src/*.h)
	$(CC) $(CFLAGS) -o $@ $(TUN_SRC) $(LDLIBS)

test: $(TARGET)
	./$(TARGET) perft 5

bench: $(TARGET)
	./$(TARGET) bench

clean:
	rm -f $(OBJ) $(TARGET) ravager.exe tuner

.PHONY: all clean test bench debug
