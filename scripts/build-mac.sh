#!/bin/bash
# Build Firebrand's Mac-side pieces ON Mac OS X 10.5.8 (PowerPC) with Xcode 3.1 (Apple gcc 4.0.1),
# and gather everything an installation needs in build/dist/Firebrand.
#
#   scripts/build-mac.sh
#
# Builds:
#   Firebrand.kext       the driver (kext/Firebrand), with the AMD microcode embedded
#   HDAudio.kext         DisplayPort audio (kexts/HDAudio)
#   RadeonNIGA.plugin    the window server's 2D plug-in (radeonni/ga)
#   frbctl               the driver's control and status tool (tools/frbctl.c)
#   Firebrand Monitor    the status app: clocks, temperature, load (tools/FirebrandMonitor)
# and copies in RadeonNIGLDriver.bundle, the OpenGL driver, from build/gl if it is there (it is
# cross-built on a Linux or macOS machine by radeonni/build-gl.sh; scripts/build-remote.sh brings
# it along).
#
# Needs firmware/ filled by scripts/fetch-firmware.sh. That script needs a current curl and
# sha256sum, so run it on a modern machine and copy firmware/ over (scripts/build-remote.sh does).
#
# Kexts are compiled with Apple's gcc 4.0.1 because the kernel's C++ ABI is that compiler's
# (GCC 15's vtables do not match it). Two checks on each kext's executable:
#   - relocations: no external PPC_RELOC_BR24 and some JBSR (a kext loads ~2.3 GB from the
#     kernel's text; a 24-bit branch would wrap);
#   - linkage: no library function referenced with C++ name mangling.
#
# IT INSTALLS NOTHING. To install: sudo build/dist/Firebrand/install.sh (README.md).
#
# FRB_JOBS (default 2) sets make's parallelism for the driver. FRB_BRINGUP (default vega20)
# is the kext Makefile's BRINGUP: vega20 embeds the Vega 20 microcode
# (kext/Firebrand/firmware-vega20.list); FRB_BRINGUP= (empty) leaves it out, and a Vega 20
# card then stops before its display comes up. The driver matches the same cards either way
# (every SI, CIK, Polaris and Vega id; kext/Firebrand/mk-pcimatch.pl).
#
# Copyright (c) 2026 Jordan Bettcher
# SPDX-License-Identifier: MIT
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
JOBS="${FRB_JOBS:-2}"
BRINGUP="${FRB_BRINGUP-vega20}"
SDK="${SDK:-/Developer/SDKs/MacOSX10.5.sdk}"
DIST="$ROOT/build/dist/Firebrand"

die() { echo "build-mac: $*" >&2; exit 1; }
say() { echo "== $*"; }

[ "$(uname -s)" = Darwin ] || die "run this on Mac OS X 10.5 (scripts/build-remote.sh runs it over ssh)"
[ "$(uname -p)" = powerpc ] || die "this is not a PowerPC Mac"
case "$(sw_vers -productVersion)" in 10.5*) ;; *) die "Mac OS X 10.5 is needed (this is $(sw_vers -productVersion))" ;; esac
command -v gcc-4.0 >/dev/null || die "no gcc-4.0: install Xcode 3.1 (Xcode 3.1.4 is the last for PowerPC)"
[ -d /System/Library/Frameworks/Kernel.framework/Headers/IOKit ] || die "no Kernel.framework headers: install Xcode 3.1"
if [ ! -d "$SDK" ]; then
	# An Xcode install without the SDK package still puts the 10.5 headers and libraries in the
	# running system; build against that root instead.
	[ -f /usr/include/stdio.h ] && [ -d /System/Library/Frameworks/IOKit.framework/Headers ] ||
		die "no $SDK and no system headers: install Xcode 3.1 with the 10.5 SDK"
	echo "build-mac: no $SDK; building against the running system's headers (/)" >&2
	SDK=/
fi

missing=
lists=kext/Firebrand/firmware.list
[ "$BRINGUP" != vega20 ] || lists="$lists kext/Firebrand/firmware-vega20.list"
for f in $(sed 's/#.*//' $lists) LICENSE.amdgpu; do
	[ -s "firmware/$f" ] || missing="$missing $f"
done
[ -z "$missing" ] || die "firmware/ lacks$missing. Run scripts/fetch-firmware.sh on a current machine and copy firmware/ here."

mkdir -p build

# The two checks, on a kext executable
gates() {
	local exe="$1" br24 jbsr
	br24=$(otool -r "$exe" | awk '/__TEXT,__text/{t=1;next} /^Relocation/{t=0} t&&NF==7&&$1!="address"&&$4==1&&$5==3{n++} END{print n+0}')
	jbsr=$(otool -r "$exe" | awk '/__TEXT,__text/{t=1;next} /^Relocation/{t=0} t&&NF==7&&$1!="address"&&$5==13{n++} END{print n+0}')
	[ "$br24" = 0 ] || die "$exe: $br24 external PPC_RELOC_BR24 relocations (the kext would branch to a wrapped address)"
	[ "$jbsr" != 0 ] || die "$exe: no JBSR relocations -- is -mlong-branch in effect?"
	if nm -u "$exe" | grep -E '^__Z[0-9]+(frb_|atom_|amdgpu_|gmc_|vi_|hda_)' >&2; then
		die "$exe: a library function is referenced with C++ linkage"
	fi
	echo "   checks: $jbsr JBSR, 0 external BR24, $(nm -u "$exe" | wc -l | tr -d ' ') undefined symbols, none mangled from the library"
}

say "Firebrand.kext (make -j$JOBS BRINGUP=$BRINGUP)"
make -C kext/Firebrand -j"$JOBS" BRINGUP="$BRINGUP" >build/build-firebrand.log 2>&1 || { tail -30 build/build-firebrand.log >&2; die "the driver did not build (build/build-firebrand.log)"; }
echo "   warnings: $(grep -c 'warning:' build/build-firebrand.log || true)"
gates kext/Firebrand/build/Firebrand.kext/Contents/MacOS/Firebrand

say "HDAudio.kext"
make -C kexts/HDAudio >build/build-hdaudio.log 2>&1 || { tail -30 build/build-hdaudio.log >&2; die "HDAudio did not build (build/build-hdaudio.log)"; }
gates kexts/HDAudio/build/HDAudio.kext/HDAudio

say "RadeonNIGA.plugin"
make -C radeonni/ga SDK="$SDK" >build/build-ga.log 2>&1 || { tail -30 build/build-ga.log >&2; die "the 2D plug-in did not build (build/build-ga.log)"; }

say "frbctl"
gcc -std=gnu99 -Wall -O1 -Ikext/Firebrand -Ishared -o build/frbctl tools/frbctl.c tools/frb_status.c shared/frb_edid.c shared/frb_dmt.c \
	-framework IOKit -framework CoreFoundation

say "Firebrand Monitor.app"
tools/FirebrandMonitor/build.sh >build/build-monitor.log 2>&1 || { tail -30 build/build-monitor.log >&2; die "Firebrand Monitor did not build (build/build-monitor.log)"; }

say "build/dist/Firebrand"
rm -rf "$DIST"
mkdir -p "$DIST"
cp -R kext/Firebrand/build/Firebrand.kext kexts/HDAudio/build/HDAudio.kext radeonni/ga/build/RadeonNIGA.plugin "$DIST/"
cp build/frbctl scripts/install.sh scripts/uninstall.sh "$DIST/"
cp -R "tools/FirebrandMonitor/build/Firebrand Monitor.app" "$DIST/"
cp README.md LICENSE NOTICE "$DIST/"
cp -R LICENSES "$DIST/"
if [ -d build/gl/RadeonNIGLDriver.bundle ]; then
	cp -R build/gl/RadeonNIGLDriver.bundle "$DIST/"
	echo "   with RadeonNIGLDriver.bundle ($(lipo -info build/gl/RadeonNIGLDriver.bundle/Contents/MacOS/RadeonNIGLDriver 2>/dev/null | sed 's/.*: //'))"
else
	echo "   WITHOUT RadeonNIGLDriver.bundle: build it with radeonni/build-gl.sh on a Linux or macOS"
	echo "   machine and put it in build/gl/ (install.sh then leaves OpenGL acceleration off)"
fi
chmod 755 "$DIST/install.sh" "$DIST/uninstall.sh" "$DIST/frbctl"
ls -l "$DIST"
echo "Built. Nothing is installed; see README.md, \"Installing\"."
