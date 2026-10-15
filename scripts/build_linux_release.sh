#!/usr/bin/env bash
# Configure (if needed) and build the optimized Linux Release binaries
# (-O3, ThinLTO, x86-64-v3) plus the settings launcher.
#
# LLVM: clang-20 / clang++-20 and ld.lld from PATH, or set LLVM_DIR to an LLVM
# install (for example ~/tools/LLVM-20.1.8-Linux-X64). Ninja: from PATH, or
# set NINJA to its path.
set -euo pipefail
cd "$(dirname "$0")/.."

build=out/build/linux-amd64-release
if [[ ! -f $build/build.ninja ]]; then
  args=(-DCMAKE_PREFIX_PATH="$PWD/sdk/linux-amd64/linux-amd64"
        -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld
        -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld)
  if [[ -n ${LLVM_DIR:-} ]]; then
    args+=(-DCMAKE_C_COMPILER="$LLVM_DIR/bin/clang"
           -DCMAKE_CXX_COMPILER="$LLVM_DIR/bin/clang++"
           -DCMAKE_AR="$LLVM_DIR/bin/llvm-ar"
           -DCMAKE_RANLIB="$LLVM_DIR/bin/llvm-ranlib")
    export PATH="$LLVM_DIR/bin:$PATH"
  fi
  if [[ -n ${NINJA:-} ]]; then
    args+=(-DCMAKE_MAKE_PROGRAM="$NINJA")
  fi
  cmake --preset linux-amd64-release "${args[@]}"
fi
cmake --build "$build"
