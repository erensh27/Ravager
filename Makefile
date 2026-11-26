# Ravager 3 — C11 UCI chess engine with an embedded network.
#
#   make                 native build for this machine (./ravager)
#   make release         portable Linux build, x86-64-v3 (AVX2/BMI2/FMA)
#   make windows         portable Windows build (MinGW-w64 cross compiler)
#   make android         Android ARM64 build (Android NDK, NEON with dot-product dispatch)
#   make EVALFILE=...    embed a different network (run make clean first)

CC       ?= gcc
CFLAGS   ?= -O3 -std=c11
override CFLAGS += -Wall -Wextra -Wno-unused-parameter -Wno-missing-braces -pthread
CPPFLAGS += -Isrc -Innue
LDLIBS   += -lm

ARCH_FLAGS     ?= -march=native
PORTABLE_FLAGS := -march=x86-64-v3 -mtune=generic
override CFLAGS += $(ARCH_FLAGS)

EVALFILE ?= nets/Ravager_NET.nnue
override CPPFLAGS += -DEVALFILE='"$(EVALFILE)"'

SRC := src/bench.c src/bitboard.c src/board.c src/evaluate.c src/movegen.c src/params.c \
       src/search.c src/see.c src/tb_syzygy.c src/tt.c src/uci.c src/tb/tbprobe.c \
       nnue/inference.c nnue/nnue.c
OBJ := $(SRC:.c=.o)
DEP := $(OBJ:.o=.d)

TARGET  := ravager
RELEASE := ravager-linux-x64-avx2
WINDOWS := ravager-windows-x64-avx2.exe
ANDROID := ravager-android-arm64

# Android NDK cross build: point ANDROID_NDK at an NDK install (or set
# ANDROID_NDK_HOME) and override ANDROID_API for a different minimum level.
ANDROID_NDK ?= $(ANDROID_NDK_HOME)
ANDROID_API ?= 24
ANDROID_CC  := $(ANDROID_NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$(ANDROID_API)-clang

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

nnue/nnue.o: $(EVALFILE)

release:
	$(MAKE) clean
	$(MAKE) ARCH_FLAGS="$(PORTABLE_FLAGS)" $(TARGET)
	cp $(TARGET) $(RELEASE)
	strip $(RELEASE)

windows: $(SRC) $(wildcard src/*.h src/tb/*.h nnue/*.h) $(EVALFILE)
	x86_64-w64-mingw32-gcc $(CPPFLAGS) -O3 -std=c11 -pthread $(PORTABLE_FLAGS) \
		-o $(WINDOWS) $(SRC) $(LDLIBS) -static -s

android: $(SRC) $(wildcard src/*.h src/tb/*.h nnue/*.h) $(EVALFILE)
	$(ANDROID_CC) $(CPPFLAGS) -O3 -std=c11 -pthread -flto -fno-emulated-tls \
		-o $(ANDROID) $(SRC) $(LDLIBS) -static -s

bench: $(TARGET)
	./$(TARGET) bench

ifeq ($(OS),Windows_NT)
TEST_EXT := .exe
endif
TEST_BINS := tests/search-test$(TEST_EXT) tests/nnue-test$(TEST_EXT) tests/tablebase-test$(TEST_EXT)

tests/search-test$(TEST_EXT): tests/search.c src/search.c $(filter-out src/uci.o src/search.o,$(OBJ))
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG -o $@ tests/search.c $(filter-out src/uci.o src/search.o,$(OBJ)) $(LDFLAGS) $(LDLIBS)

tests/nnue-test$(TEST_EXT): tests/nnue.c nnue/inference.c $(filter-out src/uci.o nnue/inference.o,$(OBJ))
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG -o $@ tests/nnue.c $(filter-out src/uci.o nnue/inference.o,$(OBJ)) $(LDFLAGS) $(LDLIBS)

tests/tablebase-test$(TEST_EXT): tests/tablebase.c src/tb_syzygy.c src/board.o src/bitboard.o src/movegen.o src/params.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG -o $@ tests/tablebase.c src/board.o src/bitboard.o src/movegen.o src/params.o $(LDFLAGS) $(LDLIBS)

test: $(TEST_BINS)
	./tests/search-test$(TEST_EXT)
	./tests/nnue-test$(TEST_EXT)
	./tests/tablebase-test$(TEST_EXT)

clean:
	rm -f $(OBJ) $(DEP) $(TARGET) $(RELEASE) $(WINDOWS) $(ANDROID) $(TEST_BINS)

-include $(DEP)
.PHONY: all release windows android bench test clean
