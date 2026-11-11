#!/bin/sh
# Copies the Tiger Lake user-space drivers into /Library/GPUBundles, the one directory from which the
# WindowServer and MTLCompilerService sandboxes map executable code. Writing there needs
# CSR_ALLOW_UNRESTRICTED_FS; without it every copy fails and is logged.
#
# The bundles are GPUDriversIntel 16.0.32 (June 2020, Big Sur SDK) as shipped unchanged in this
# repository's sle_Internal/sle/, installed in /Library/Extensions:
#   AppleIntelTGLGraphicsMTLDriver.bundle  Metal driver
#   AppleIntelTGLGraphicsGLDriver.bundle   OpenGL driver
#   AppleIntelTGLGraphicsVADriver.bundle   video decoder
#   AppleIntelTGLGraphicsVAME.bundle       video encoder
#   AppleIntelGraphicsShared.bundle        libMTLIGCCompilerPlugin.dylib, the compiler plugin with the Tiger Lake backend
# /Library/GPUBundles/libLLVM.dylib (Big Sur 11.7.10's libLLVM, rebuilt to load on macOS 15) and the
# compiler plugin shim (tools/igc-shim) are installed separately.
#
# Install as /usr/local/bin/ngreen-gpubundles.sh, with com.ngreen.gpubundles.plist in /Library/LaunchDaemons.

BUNDLES="AppleIntelTGLGraphicsMTLDriver AppleIntelTGLGraphicsGLDriver AppleIntelTGLGraphicsVADriver AppleIntelTGLGraphicsVAME AppleIntelGraphicsShared"

mkdir -p /Library/GPUBundles || exit 1
for b in $BUNDLES; do
	src="/Library/Extensions/$b.bundle"
	dst="/Library/GPUBundles/$b.bundle"
	if [ ! -d "$src" ]; then
		echo "missing $src"
	elif [ -d "$dst" ]; then
		echo "present $dst"
	elif ditto "$src" "$dst"; then
		echo "copied $dst"
	else
		echo "cannot write $dst"
	fi
done
