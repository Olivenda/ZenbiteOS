#!/usr/bin/env bash
# Build the i686-elf cross toolchain into ~/opt/i686-elf. Used by CI and
# documented for contributors in tools/TOOLCHAIN.md.
set -euo pipefail

PREFIX="${PREFIX:-$HOME/opt/i686-elf}"
TARGET=i686-elf
BINUTILS=binutils-2.42
GCC=gcc-14.1.0

mkdir -p "$PREFIX/src"
cd "$PREFIX/src"

[[ -f $BINUTILS.tar.xz ]] || curl -fLO https://ftp.gnu.org/gnu/binutils/$BINUTILS.tar.xz
[[ -f $GCC.tar.xz      ]] || curl -fLO https://ftp.gnu.org/gnu/gcc/$GCC/$GCC.tar.xz
[[ -d $BINUTILS        ]] || tar xf $BINUTILS.tar.xz
[[ -d $GCC             ]] || tar xf $GCC.tar.xz

mkdir -p build-binutils && cd build-binutils
"../$BINUTILS/configure" --target=$TARGET --prefix="$PREFIX" \
    --with-sysroot --disable-nls --disable-werror
make -j"$(nproc)"
make install
cd ..

cd "$GCC"
contrib/download_prerequisites
cd ..

mkdir -p build-gcc && cd build-gcc
"../$GCC/configure" --target=$TARGET --prefix="$PREFIX" \
    --disable-nls --enable-languages=c --without-headers
make -j"$(nproc)" all-gcc all-target-libgcc
make install-gcc install-target-libgcc

echo "toolchain installed under $PREFIX"
