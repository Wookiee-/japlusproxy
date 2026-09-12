# japlusproxy — CMake alternative to the plain Makefile.
# Linux i386 build (NOT Windows):
#   cmake -S . -B build-i686 -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchains/linux-i686.cmake
#   cmake --build build-i686   # -> japlusproxy.so (plus build-i686/japlusproxy.so)
# Or simply:  make cmake-build   (same two steps, see Makefile).
# The toolchain file is vendored from OpenJK (OpenJK/ is gitignored).
cmake_minimum_required(VERSION 3.13)
project(japlusproxy C)

add_library(japlusproxy SHARED proxy.c patch.c)
set_target_properties(japlusproxy PROPERTIES
  PREFIX ""
  OUTPUT_NAME "japlusproxy"
  SUFFIX ".so")
target_compile_options(japlusproxy PRIVATE -m32 -fPIC -O2 -Wall -Wextra)
target_link_options(japlusproxy PRIVATE -m32 -shared)
target_link_libraries(japlusproxy PRIVATE dl)
