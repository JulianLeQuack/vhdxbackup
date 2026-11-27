#!/bin/sh
# Cross-Compile unter Linux mit MinGW-w64 (Ubuntu: apt install g++-mingw-w64-x86-64-posix).
# Ergebnis: build/vhdxbackup.exe (64 Bit, statisch gelinkt, keine DLLs außer Windows-Bordmitteln).
set -eu
cd "$(dirname "$0")"
mkdir -p build
CXX=${CXX:-x86_64-w64-mingw32-g++-posix}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
$WINDRES -I src src/vhdxbackup.rc -O coff -o build/vhdxbackup.res.o
$CXX -std=c++17 -O2 -municode -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
  -Wall -Wextra -Werror -Wno-unknown-pragmas -Wno-missing-field-initializers -Wno-cast-function-type \
  src/*.cpp build/vhdxbackup.res.o \
  -o build/vhdxbackup.exe \
  -static -static-libgcc -static-libstdc++ -s \
  -lvssapi -lvirtdisk -lbcrypt -lole32 -loleaut32 -lwbemuuid -lodbc32 -luuid -ladvapi32
echo "OK: build/vhdxbackup.exe"
