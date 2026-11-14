#!/usr/bin/env bash
# Cross-build RadeonNIGLDriver.bundle, the OpenGL driver bundle Firebrand's accelerator names:
# Mesa 26.2.4 (radeonsi for GCN cards, r600, softpipe) with the patches in radeonni/mesa/patches,
# behind Apple's GL driver interface (radeonni/gld), for Mac OS X 10.5 on PowerPC.
#
#   radeonni/build-gl.sh [ppc|ppc64|fat] [reconfigure]
#
#   ppc     the 32-bit slice: what the window server and 32-bit programs load (the default)
#   ppc64   the 64-bit slice: for 64-bit programs (FrostByte, for example)
#   fat     both slices, joined with lipo (what scripts/install.sh installs when it is there)
#   reconfigure   discard the meson build directory first
#
# Needs, on a Linux or macOS build machine:
#   - a powerpc-apple-darwin9 GCC 15.3 cross toolchain with the ppc and ppc64 multilibs, C, C++
#     and Objective-C, libatomic and cctools/ld64 -- FrostByte's toolchain is exactly that:
#     in a FrostByte checkout, `./build.sh sdk toolchain --sdk=<Xcode 3.1.4 .dmg>`;
#   - the Mac OS X 10.5 SDK (from Xcode 3.1.4; Apple's licence, so you supply it);
#   - python3 (with venv and pip), ninja, curl, xz, rsync, patch.
# Point at them with one of
#   LEOPARD_TOOLCHAIN=<dir with bin/powerpc-apple-darwin9-gcc> LEOPARD_SDK=<.../MacOSX10.5.sdk>
#   FROSTBYTE=<a FrostByte checkout>   (reads its build/toolchain.env)
#
# Downloads (checksum-verified): Mesa 26.2.4 from archive.mesa3d.org; meson and mako into a
# private Python environment in build/host/venv. Output: build/gl/RadeonNIGLDriver.bundle.
#
# Copyright (c) 2026 Jordan Bettcher
# SPDX-License-Identifier: MIT
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/radeonni"
B="${FIREBRAND_BUILD:-$ROOT/build}"
OUT="$B/gl"
MESA_V=26.2.4
MESA_SHA256=bce5f7fbebb934373b86c999a064d52fb5065878dc57f287f95346648ec832e9
MESA_URL="https://archive.mesa3d.org/mesa-$MESA_V.tar.xz"
PY_PINS="meson==1.8.2 mako==1.4.3 packaging==25.0 pyyaml==6.0.3"
JOBS="${JOBS:-$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 4)}"

die() { echo "build-gl: $*" >&2; exit 1; }
say() { echo "== $*"; }

SLICE_ARG="${1:-ppc}"
RECONF=0
for a in "$@"; do [ "$a" = reconfigure ] && RECONF=1; done

# ---- toolchain and SDK --------------------------------------------------------------------------
if [ -z "${LEOPARD_TOOLCHAIN:-}" ] && [ -n "${FROSTBYTE:-}" ]; then
	[ -f "$FROSTBYTE/build/toolchain.env" ] || die "no $FROSTBYTE/build/toolchain.env: run FrostByte's ./build.sh sdk toolchain first"
	LEOPARD_TOOLCHAIN="$(sed -n 's/^LEOPARD_TOOLCHAIN=//p' "$FROSTBYTE/build/toolchain.env")"
	LEOPARD_SDK="$(sed -n 's/^LEOPARD_SDK=//p' "$FROSTBYTE/build/toolchain.env")"
fi
[ -n "${LEOPARD_TOOLCHAIN:-}" ] || die "set LEOPARD_TOOLCHAIN (or FROSTBYTE); see the top of this script"
[ -n "${LEOPARD_SDK:-}" ] || die "set LEOPARD_SDK (or FROSTBYTE); see the top of this script"
T="$LEOPARD_TOOLCHAIN/bin/powerpc-apple-darwin9"
CC="$T-gcc"
[ -x "$CC" ] || die "no $CC"
[ -f "$LEOPARD_SDK/System/Library/Frameworks/OpenGL.framework/Headers/gliDispatch.h" ] ||
	die "$LEOPARD_SDK is not the Mac OS X 10.5 SDK (no OpenGL.framework/Headers/gliDispatch.h)"
[ -n "$("$CC" -print-prog-name=cc1obj 2>/dev/null)" ] && [ -x "$("$CC" -print-prog-name=cc1obj)" ] ||
	die "$CC has no Objective-C compiler (Mesa's meson.build needs one on darwin)"
for t in g++ ar ranlib strip nm otool lipo; do [ -x "$T-$t" ] || die "no $T-$t"; done
for t in ninja curl xz rsync patch python3; do command -v "$t" >/dev/null || die "$t is not installed"; done

# ---- host Python: meson and mako -------------------------------------------------------------
VENV="$B/host/venv"
if [ ! -x "$VENV/bin/meson" ] || [ "$(cat "$VENV/.pins" 2>/dev/null)" != "$PY_PINS" ]; then
	say "Python environment for meson and mako ($PY_PINS)"
	rm -rf "$VENV"
	python3 -m venv "$VENV"
	"$VENV/bin/python" -m pip install -q --disable-pip-version-check $PY_PINS
	echo "$PY_PINS" > "$VENV/.pins"
fi
# Mesa's generators run under the python3 meson finds on PATH: the environment's, with mako.
export PATH="$VENV/bin:$PATH"
MESON="$VENV/bin/meson"

# ---- Mesa source, patched ---------------------------------------------------------------------
mkdir -p "$B/dl" "$B/gl-src"
TARBALL="$B/dl/mesa-$MESA_V.tar.xz"
sha_of() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
if [ ! -f "$TARBALL" ] || [ "$(sha_of "$TARBALL")" != "$MESA_SHA256" ]; then
	say "download $MESA_URL"
	curl -fL --retry 3 -o "$TARBALL.part" "$MESA_URL"
	[ "$(sha_of "$TARBALL.part")" = "$MESA_SHA256" ] || { rm -f "$TARBALL.part"; die "mesa-$MESA_V.tar.xz: wrong checksum"; }
	mv "$TARBALL.part" "$TARBALL"
fi
MESA="$B/gl-src/mesa-$MESA_V"
# A fresh, patched tree whenever the patch series changes (its digest is kept beside the tree).
PATCHES_DIGEST="$(cat "$SRC"/mesa/patches/*.patch | (sha256sum 2>/dev/null || shasum -a 256) | cut -d' ' -f1)"
if [ "$(cat "$MESA.patched" 2>/dev/null)" != "$PATCHES_DIGEST" ]; then
	say "unpack and patch Mesa $MESA_V ($(ls "$SRC"/mesa/patches/*.patch | wc -l | tr -d ' ') patches)"
	rm -rf "$MESA" "$MESA.patched" "$B/gl-build"
	tar -C "$B/gl-src" -xJf "$TARBALL"
	for p in "$SRC"/mesa/patches/*.patch; do
		patch -d "$MESA" -p1 --batch --forward -s < "$p" || die "${p##*/} did not apply"
	done
	echo "$PATCHES_DIGEST" > "$MESA.patched"
fi

# ---- the rdn gallium target into the Mesa tree ------------------------------------------------
# Directories Mesa does not have upstream: the winsys over the kext's user client, the frontend,
# the target with the GL bundle (gld), the video layer and the hardware library's headers.
say "stage the rdn driver into Mesa"
TGT="$MESA/src/gallium/targets/rdn"
rsync -ac --delete "$SRC/mesa/winsys/" "$MESA/src/gallium/winsys/rdn/"
rsync -ac --delete "$SRC/mesa/frontend/" "$MESA/src/gallium/frontends/rdn/"
rsync -ac --delete --exclude hw --exclude gld --exclude video "$SRC/mesa/target/" "$TGT/"
rsync -ac --delete "$SRC/video/" "$TGT/video/"
rsync -ac --delete "$SRC/hw/" "$TGT/hw/"
rsync -ac --delete "$SRC/gld/" "$TGT/gld/"

build_slice() {
	local slice="$1" archflags cpu_family cpu m
	case "$slice" in
	# -mcpu=7400: runs on the G4, the G5 and the e6500 alike.
	ppc)	archflags="'-mcpu=7400'"; cpu_family=ppc; cpu=ppc7400; m="" ;;
	# -mcpu=powerpc64, not 970: the 970 target uses fsqrt, which the e6500 does not implement.
	ppc64)	archflags="'-m64', '-mcpu=powerpc64'"; cpu_family=ppc64; cpu=ppc64; m="-m64" ;;
	*)	die "slice must be ppc, ppc64 or fat" ;;
	esac
	local W="$B/gl-build/$slice" COMPAT="$B/gl-build/$slice/compat" BUILD="$B/gl-build/$slice/mesa"
	mkdir -p "$COMPAT/linkshim"

	# libtigercompat.a: clock_gettime, posix_memalign, strnlen, getline, ... which 10.5 lacks.
	# tiger_emutls.o is linked as an object: Mesa's TLS patch (0025) calls its rdn_tls_* helpers.
	say "$slice: compatibility library"
	rsync -ac --delete "$SRC/mesa/darwin8/" "$COMPAT/src/"
	for u in tiger_compat tiger_emutls; do
		"$CC" -isysroot "$LEOPARD_SDK" -O2 -Wall $(echo "$archflags" | tr -d "',") \
			-mmacosx-version-min=10.5 -c -o "$COMPAT/$u.o" "$COMPAT/src/$u.c"
	done
	rm -f "$COMPAT/libtigercompat.a"
	"$T-ar" rcs "$COMPAT/libtigercompat.a" "$COMPAT/tiger_compat.o"
	# -latomic must find the static archive, never the toolchain's dylib (whose install name is a
	# path on the build machine, which dyld on the Mac cannot load).
	cp -f "$("$CC" $m -print-file-name=libatomic.a)" "$COMPAT/linkshim/libatomic.a"

	# -mone-byte-bool: Mesa assumes a one-byte bool; the bundle shares no bool-bearing structure
	# with the system or the kext (rdn_user.h is fixed-width).
	local CROSS="$W/cross.ini"
	local common="'-isysroot', '$LEOPARD_SDK', '-mmacosx-version-min=10.5', '-mlongcall', $archflags, '-mone-byte-bool', '-fno-strict-aliasing', '-isystem', '$COMPAT/src/include'"
	local link="'-isysroot', '$LEOPARD_SDK', '-mmacosx-version-min=10.5', $archflags, '-L$COMPAT/linkshim', '-Wl,-search_paths_first', '-Wl,-dead_strip_dylibs', '-static-libgcc'"
	cat > "$CROSS" <<INI
[binaries]
c = '$T-gcc'
cpp = '$T-g++'
objc = '$T-gcc'
ar = '$T-ar'
strip = '$T-strip'
ranlib = '$T-ranlib'
pkg-config = 'false'

[properties]
rdn_sdk = '$LEOPARD_SDK'

[built-in options]
c_args = [$common]
cpp_args = [$common, '-include', '$COMPAT/src/tiger_compat.h']
objc_args = [$common]
c_link_args = [$link, '-Wl,-dead_strip', '$COMPAT/tiger_emutls.o', '$COMPAT/libtigercompat.a']
cpp_link_args = [$link, '-static-libstdc++', '-Wl,-dead_strip', '$COMPAT/tiger_emutls.o', '$COMPAT/libtigercompat.a']

[host_machine]
system = 'darwin'
cpu_family = '$cpu_family'
cpu = '$cpu'
endian = 'big'
INI

	[ "$RECONF" = 1 ] && rm -rf "$BUILD"
	if [ ! -f "$BUILD/build.ninja" ]; then
		say "$slice: meson setup ($("$MESON" --version))"
		(cd "$MESA" && "$MESON" setup "$BUILD" \
			-Drdn=true -Dgallium-drivers=r600,radeonsi,softpipe -Dvulkan-drivers= \
			-Damd-use-llvm=false \
			-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled \
			-Dplatforms= -Dgles1=disabled -Dgles2=disabled -Dzstd=disabled \
			-Dvalgrind=disabled -Dlibunwind=disabled -Dvideo-codecs= \
			-Dgallium-va=disabled -Dbuildtype=release -Doptimization=2 \
			-Dxmlconfig=disabled -Dshader-cache=enabled -Db_ndebug=true \
			--cross-file "$CROSS" --wrap-mode=nodownload)
	fi
	# meson drops cross-file argument lists on a reconfigure; put them back
	python3 "$SRC/scripts/meson-darwin9-args.py" "$CROSS" "$BUILD" || true

	say "$slice: ninja (-j$JOBS)"
	ninja -C "$BUILD" -j"$JOBS" src/gallium/targets/rdn/RadeonNIGLDriver.dylib
	local DYLIB="$BUILD/src/gallium/targets/rdn/RadeonNIGLDriver.dylib"
	gates "$DYLIB" "$slice"
	cp -f "$DYLIB" "$W/RadeonNIGLDriver"
}

# A link that succeeds proves little here: a bundle the GL engine refuses is dropped in silence and
# the system falls back to software rendering. So:
gates() {
	local DYLIB="$1" slice="$2" n own bad unresolved exports
	# 1. Leopard's GL engine needs all 66 gld* entry points.
	n=$("$T-nm" -g "$DYLIB" | grep -c ' T _gld' || true)
	[ "$n" -ge 66 ] || die "$slice: only $n of the 66 gld* entry points are exported"
	# 2. none of the bundle's own symbols may be left undefined
	own=$("$T-nm" -u "$DYLIB" | grep -iE '_rdn|_RDN' || true)
	[ -z "$own" ] || die "$slice: own symbols left undefined: $own"
	# 3. every library it loads must exist on the Mac: /usr/lib and /System/Library only
	bad=$("$T-otool" -L "$DYLIB" | tail -n +2 | awk '{print $1}' | grep -vE '^(/usr/lib/|/System/Library/)' || true)
	[ -z "$bad" ] || die "$slice: depends on paths that do not exist on the Mac: $bad"
	# 4. every undefined symbol must be exported by one of those libraries (or what they re-export)
	exports=$(mktemp)
	{
		"$T-otool" -L "$DYLIB" | tail -n +2 | awk '{print $1}'
		"$T-otool" -L "$DYLIB" | tail -n +2 | awk '{print $1}' | while read -r lib; do
			[ -f "$LEOPARD_SDK$lib" ] && "$T-otool" -L "$LEOPARD_SDK$lib" | tail -n +2 | awk '{print $1}'
		done
	} | sort -u | while read -r lib; do
		[ -f "$LEOPARD_SDK$lib" ] && "$T-nm" -g -arch "$slice" "$LEOPARD_SDK$lib" 2>/dev/null | awk '$2 ~ /^[TDSBAI]$/ {print $3}'
	done | sort -u > "$exports"
	unresolved=$("$T-nm" -u "$DYLIB" | sed 's/^ *//' | sort -u | comm -23 - "$exports" || true)
	rm -f "$exports"
	[ -z "$unresolved" ] || die "$slice: undefined symbols no system library exports: $unresolved"
	echo "   gates: $n gld* entries, no own undefined symbols, system paths only, every symbol resolved"
}

case "$SLICE_ARG" in
ppc|ppc64)	build_slice "$SLICE_ARG"; BIN="$B/gl-build/$SLICE_ARG/RadeonNIGLDriver" ;;
fat)		build_slice ppc; build_slice ppc64
		BIN="$B/gl-build/RadeonNIGLDriver.fat"
		"$T-lipo" -create "$B/gl-build/ppc/RadeonNIGLDriver" "$B/gl-build/ppc64/RadeonNIGLDriver" -output "$BIN" ;;
reconfigure)	build_slice ppc; BIN="$B/gl-build/ppc/RadeonNIGLDriver" ;;
*)		sed -n '2,12p' "$0"; exit 2 ;;
esac

# ---- the bundle ---------------------------------------------------------------------------------
# OpenGL loads it by the name the accelerator publishes (IOGLBundleName = RadeonNIGLDriver), from
# /System/Library/Extensions.
rm -rf "$OUT/RadeonNIGLDriver.bundle"
mkdir -p "$OUT/RadeonNIGLDriver.bundle/Contents/MacOS"
cp "$SRC/gld/Info.plist" "$OUT/RadeonNIGLDriver.bundle/Contents/Info.plist"
cp "$BIN" "$OUT/RadeonNIGLDriver.bundle/Contents/MacOS/RadeonNIGLDriver"
chmod 755 "$OUT/RadeonNIGLDriver.bundle/Contents/MacOS/RadeonNIGLDriver"
say "built $OUT/RadeonNIGLDriver.bundle ($("$T-lipo" -info "$BIN" | sed 's/.*: //'))"
