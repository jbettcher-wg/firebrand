#!/bin/bash
# Build Firebrand Monitor.app: 32-bit PowerPC, Mac OS X 10.5 and later.
#
#   tools/FirebrandMonitor/build.sh
#       on Mac OS X 10.5 (a G5, a G4, a T4240): build here, into
#       build/Firebrand Monitor.app. Needs Apple's gcc (Xcode 3.x) and the
#       system's frameworks, nothing else.
#
# scripts/build-mac.sh runs it with the driver's other pieces.
# The icon is FirebrandMonitor.icns (mk-icon.py makes it from icon.svg).
#
# Copyright (c) 2026 Jordan Bettcher
# SPDX-License-Identifier: MIT
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
FRB="$(cd "$HERE/../.." && pwd)"
APPNAME="Firebrand Monitor.app"

if [ "$(uname -s)" != Darwin ]; then
	echo "build.sh: run this on Mac OS X 10.5 (./build.sh mac builds it with the rest)" >&2
	exit 1
fi

cd "$HERE"
CC="${CC:-gcc}"
APP="build/$APPNAME"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
$CC -arch ppc -mmacosx-version-min=10.5 -std=gnu99 -O2 -Wall \
	-I.. -I../../kext/Firebrand \
	-o "$APP/Contents/MacOS/FirebrandMonitor" \
	main.m FMController.m FMCardView.m FMGraphView.m ../frb_status.c \
	-framework Cocoa -framework IOKit -framework CoreFoundation
cp Info.plist "$APP/Contents/Info.plist"
printf 'APPLFbMn' > "$APP/Contents/PkgInfo"
cp FirebrandMonitor.icns "$APP/Contents/Resources/FirebrandMonitor.icns"
echo "$HERE/$APP: $(wc -c < "$APP/Contents/MacOS/FirebrandMonitor" | tr -d ' ') bytes ($(lipo -info "$APP/Contents/MacOS/FirebrandMonitor" 2>/dev/null | sed 's/.*: //'))"
