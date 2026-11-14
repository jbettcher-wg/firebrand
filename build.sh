#!/usr/bin/env bash
# Build Firebrand.
#
#   ./build.sh                      on a Linux or macOS machine: the firmware and the OpenGL
#                                   bundle (both slices); on Mac OS X 10.5 (PowerPC): the kexts,
#                                   the 2D plug-in and frbctl
#   ./build.sh firmware             fetch the AMD microcode into firmware/
#   ./build.sh gl [ppc|ppc64|fat]   cross-build RadeonNIGLDriver.bundle (radeonni/build-gl.sh)
#   ./build.sh mac [user@]host      build the Mac-side pieces on a Mac over ssh and bring
#                                   build/dist/Firebrand back (scripts/build-remote.sh)
#   ./build.sh native               the Mac-side pieces, on this Mac (scripts/build-mac.sh)
#
# The kexts must be built on Mac OS X 10.5 with Xcode 3.1 (Apple gcc 4.0.1); the OpenGL bundle
# needs GCC 15 (Mesa), so it is cross-built. README.md, "Building", has the whole sequence.
#
# Copyright (c) 2026 Jordan Bettcher
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"

native_mac() { [ "$(uname -s)" = Darwin ] && [ "$(uname -p)" = powerpc ]; }

case "${1:-}" in
firmware)	exec "$ROOT/scripts/fetch-firmware.sh" ;;
gl)		shift; exec "$ROOT/radeonni/build-gl.sh" "${@:-fat}" ;;
mac)		shift; exec "$ROOT/scripts/build-remote.sh" "$@" ;;
native)		exec "$ROOT/scripts/build-mac.sh" ;;
"")
	if native_mac; then
		exec "$ROOT/scripts/build-mac.sh"
	fi
	"$ROOT/scripts/fetch-firmware.sh"
	"$ROOT/radeonni/build-gl.sh" fat
	echo
	echo "Next: the kexts, the 2D plug-in and frbctl are built on the Mac:"
	echo "    ./build.sh mac [user@]your-mac"
	;;
*)		sed -n '2,16p' "$0"; exit 2 ;;
esac
