#!/bin/sh
# build_local.sh - 用用户级 mingw-w64 工具链交叉编译 HOW.exe（Windows XP 兼容）
# 用法: ./scripts/build_local.sh [输出路径]
set -e
ROOT=/home/z/my-project/toolchain-root
DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$DIR/HOW.exe}"

SRCS="$DIR/src/main.c $DIR/src/compat.c $DIR/src/ui_parse.c $DIR/src/ui_render.c \
$DIR/src/store.c $DIR/src/abc.c $DIR/src/json.c $DIR/src/selftest.c \
$DIR/third_party/miniz/miniz.c $DIR/third_party/miniz/miniz_tdef.c \
$DIR/third_party/miniz/miniz_tinfl.c $DIR/third_party/miniz/miniz_zip.c"

# XP 兼容（参考 Task 1 已验证参数）：PE 头 5.01 + msvcrt 静态链接
"$ROOT/usr/bin/i686-w64-mingw32-gcc-14-win32" $SRCS \
  -B"$ROOT/usr/lib/gcc/i686-w64-mingw32/14-win32/" \
  -B"$ROOT/usr/i686-w64-mingw32/bin/" -B"$ROOT/usr/i686-w64-mingw32/lib/" \
  -isystem "$ROOT/usr/i686-w64-mingw32/include" \
  -isystem "$ROOT/usr/share/mingw-w64/include" \
  -I"$DIR/src" -I"$DIR/third_party/miniz" \
  -O2 -s -static -static-libgcc -fno-stack-protector \
  -DUNICODE -D_UNICODE \
  -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 \
  -mwindows \
  -Wl,--major-os-version,5,--minor-os-version,1 \
  -Wl,--major-subsystem-version,5,--minor-subsystem-version,1 \
  -o "$OUT" 2>&1 | head -60
test -f "$OUT" && echo "BUILD OK: $OUT ($(wc -c < "$OUT") bytes)"
