#!/bin/sh
# Builds the compiler plugin shim and installs it as AppleIntelGraphicsShared.bundle's
# libMTLIGCCompilerPlugin.dylib. The 2020 plugin moves beside it as libMTLIGCCompilerPlugin.dylib.real,
# a name NootedGreen's plugin patches still match.
set -e
D=/Library/GPUBundles/AppleIntelGraphicsShared.bundle/Contents/MacOS
PLUGIN_SHA256=477083cb773da4dc814d97fa1a64611194c6ecf4bb7b98c3d4ea31094c1df667
OUT=$(mktemp -d)
clang -arch x86_64 -dynamiclib -O2 -Wall -Wextra -Wno-unused-parameter \
  -install_name "$D/libMTLIGCCompilerPlugin.dylib" \
  -o "$OUT/libMTLIGCCompilerPlugin.dylib" "$(dirname "$0")/ngreen_igc_shim.c"
codesign -f -s - "$OUT/libMTLIGCCompilerPlugin.dylib"
[ -f "$D/libMTLIGCCompilerPlugin.dylib.real" ] || cp -X "$D/libMTLIGCCompilerPlugin.dylib" "$D/libMTLIGCCompilerPlugin.dylib.real"
got=$(shasum -a 256 "$D/libMTLIGCCompilerPlugin.dylib.real" | cut -d' ' -f1)
[ "$got" = "$PLUGIN_SHA256" ] || { echo "libMTLIGCCompilerPlugin.dylib.real is not the 2020 plugin ($got)"; exit 1; }
cp -X "$OUT/libMTLIGCCompilerPlugin.dylib" "$D/libMTLIGCCompilerPlugin.dylib"
