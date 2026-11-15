#!/usr/bin/env bash
# Builds IntelHybridCores.kext with the Command Line Tools. Needs:
#   MacKernelSDK  https://github.com/acidanthera/MacKernelSDK   (DEPS/MacKernelSDK)
#   Lilu (debug or release build, for its Headers and Library)   (DEPS/Lilu.kext)
# Usage: DEPS=/path/to/deps bash build.sh [output dir, default ./build]
set -euo pipefail
S=$(cd "$(dirname "$0")" && pwd); OUT=${1:-$S/build}
DEPS=${DEPS:?set DEPS to the folder with MacKernelSDK and Lilu.kext}
SDK=$DEPS/MacKernelSDK; LILU=$DEPS/Lilu.kext/Contents/Resources
[ -d "$SDK" ] && [ -d "$LILU" ] || { echo "missing $SDK or $LILU"; exit 1; }
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT; cd "$W"
NAME=IntelHybridCores; BID=io.github.rodgod-rr.IntelHybridCores; VER=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$S/Info.plist")
FLAGS=(-arch x86_64 -mkernel -nostdinc -fno-builtin -fno-common -fno-stack-protector -mmacosx-version-min=13.0 -mmmx -msse -msse2
       -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DPRODUCT_NAME=$NAME -DMODULE_VERSION=$VER
       -isystem $SDK/Headers -I$LILU -O2 -g)
cat > info.c <<EOC
#include <mach/mach_types.h>
#include <mach/kmod.h>
extern kern_return_t ${NAME}_kern_start(kmod_info_t *ki, void *data);
extern kern_return_t ${NAME}_kern_stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL($BID, "$VER", ${NAME}_kern_start, ${NAME}_kern_stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
EOC
clang "${FLAGS[@]}" -c info.c -o info.o
CXX=(-std=gnu++17 -nostdinc++ -fno-exceptions -fno-rtti -fapple-kext -Wall -Wno-deprecated-declarations -Wno-unused-parameter)
clang++ "${FLAGS[@]}" "${CXX[@]}" -c "$S/src/IntelHybridCores.cpp" -o hc.o
clang++ "${FLAGS[@]}" "${CXX[@]}" -c $LILU/Library/plugin_start.cpp -o plugin_start.o
K=$NAME.kext; mkdir -p $K/Contents/MacOS; cp "$S/Info.plist" $K/Contents/
clang++ -arch x86_64 -mkernel -nostdlib -static -Xlinker -kext -Xlinker -export_dynamic -mmacosx-version-min=13.0 \
  info.o hc.o plugin_start.o $SDK/Library/x86_64/libkmod.a -lcc_kext -o $K/Contents/MacOS/$NAME
codesign -f -s - $K >/dev/null 2>&1 || true
plutil -lint $K/Contents/Info.plist >/dev/null
mkdir -p "$OUT"; rm -rf "$OUT/$K"; cp -R $K "$OUT/"; echo "$OUT/$K"; md5 -q "$OUT/$K/Contents/MacOS/$NAME"
