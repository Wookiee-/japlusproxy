# japlusproxy — Linux i386 wrapper build.
# Run on a 32-bit-capable Linux host (NOT Windows):
#   sudo apt install gcc-multilib libc6-dev-i386 cmake
#   make            # -> japlusproxy.so (plain gcc, same -m32 flags)
#   make cmake-build  # -> build-i686/japlusproxy.so (CMake + vendored
#                     #    cmake/Toolchains/linux-i686.cmake toolchain)
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
SRC = proxy.c patch.c hde32.c

all: $(OUT)

$(OUT): $(SRC) patch.h hde32.h table32.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

cmake-build:
	cmake -S . -B build-i686 -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchains/linux-i686.cmake
	cmake --build build-i686

clean:
	rm -f $(OUT)
	rm -rf build-i686
