# Ravager 2 — Makefile
# HCE + NNUE evaluation, Syzygy tablebases via Pyrrhic.

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
    # skip -march=native when cross-compiling the Windows binary
    ifeq (,$(findstring ravager.exe,$(MAKECMDGOALS)))
      CFLAGS += -march=native
    endif
  endif
endif

# --- NNUE -----------------------------------------------------------------
# Default build embeds the bundled Leorik-format net into the binary via
# incbin, making ./ravager fully self-contained (~5 MB). Load a different
# net with 'make EVALFILE=path/to/net.nnue', or build an HCE-only binary
# with 'make EVALFILE='.
EVALFILE ?= nets/640HL-S-5io8-6116M-FRCv1.nnue

ifneq ($(EVALFILE),)
  ifeq (,$(wildcard $(EVALFILE)))
    $(error NNUE net not found at '$(EVALFILE)' — run 'make net' to fetch it, or build HCE-only with 'make EVALFILE=')
  endif
  CFLAGS += -DEVALFILE=\"$(EVALFILE)\"
endif

SRC := src/bitboard.c src/board.c src/movegen.c src/see.c src/evaluate.c src/params.c \
       src/tt.c src/search.c src/nnue.c \
       src/tb/tbprobe.c src/tb_syzygy.c \
       src/uci.c
OBJ := $(SRC:.c=.o)

TARGET := ravager

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

%.o: %.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

# Windows cross-build
ravager.exe: $(SRC) $(wildcard src/*.h) $(wildcard src/tb/*.h)
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

# Fetch the default net (only needed if nets/ was not cloned with the repo)
NET_URL := https://raw.githubusercontent.com/lithander/Leorik/master/Leorik.Core/640HL-S-5io8-6116M-FRCv1.nnue
net:
	@mkdir -p nets
	curl -sL "$(NET_URL)" -o $(EVALFILE)
	@echo "Downloaded $(EVALFILE)"

clean:
	rm -f $(OBJ) $(TARGET) ravager.exe tuner

.PHONY: all clean test bench debug tuner net
