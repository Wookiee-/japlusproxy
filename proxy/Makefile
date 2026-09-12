# japlusproxy — Linux i386 wrapper build.
# Run on a 32-bit-capable Linux host (NOT Windows):
#   sudo apt install gcc-multilib libc6-dev-i386
#   make            # -> japlusproxy.so
# Deploy next to jampded (two options):
#   A) stock jampded (loads "jampgamei386.so"):
#        mv jampgamei386.so japlus_real_i386.so   # original JA+ lib
#        cp proxy/japlusproxy.so jampgamei386.so  # wrapper takes the name
#   B) engine loading "japlusproxy.so": drop proxy/japlusproxy.so next to the
#      untouched jampgamei386.so (self-skip resolution finds the real lib).
#   Either way, JAPLUS_REAL=/path/to/real.so overrides resolution.
CC      ?= gcc
CFLAGS  ?= -m32 -fPIC -O2 -Wall -Wextra
LDFLAGS ?= -m32 -shared
LDLIBS  ?= -ldl
OUT = japlusproxy.so
SRC = proxy.c patch.c

all: $(OUT)

$(OUT): $(SRC) patch.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(OUT)
