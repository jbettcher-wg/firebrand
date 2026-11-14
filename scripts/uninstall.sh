#!/bin/sh
# Remove what install.sh installed, and put back the OpenGL bundle and 2D plug-in it saved from
# another driver. Takes effect at the next restart.
#
#   sudo ./uninstall.sh
#
# Copyright (c) 2026 Jordan Bettcher
# SPDX-License-Identifier: MIT

SLE=/System/Library/Extensions
SUPPORT="/Library/Application Support/Firebrand"
SAVED="$SUPPORT/saved"

[ "$(id -u)" = 0 ] || { echo "run as root: sudo ./uninstall.sh" >&2; exit 1; }

removed=0
remove() {
	if [ -e "$1" ]; then
		rm -rf "$1"
		echo "Removed $1."
		removed=1
	fi
}

remove "$SLE/Firebrand.kext"
# HDAudio, the GL bundle and the 2D plug-in only if install.sh put them there.
for b in HDAudio.kext RadeonNIGLDriver.bundle RadeonNIGA.plugin; do
	if grep -qx "$b" "$SUPPORT/installed.txt" 2>/dev/null; then
		remove "$SLE/$b"
	fi
done
for b in RadeonNIGLDriver.bundle RadeonNIGA.plugin; do
	if [ -d "$SAVED/$b" ] && [ ! -e "$SLE/$b" ]; then
		cp -R "$SAVED/$b" "$SLE/$b"
		chown -R root:wheel "$SLE/$b"
		chmod -R go-w "$SLE/$b"
		echo "Put back the saved $b."
	fi
done
if grep -qx /usr/local/bin/frbctl "$SUPPORT/installed.txt" 2>/dev/null; then
	remove /usr/local/bin/frbctl
fi
rm -rf "$SUPPORT"

if [ "$removed" = 1 ]; then
	rm -f /System/Library/Extensions.mkext /System/Library/Extensions.kextcache
	touch "$SLE"
	sync
	echo "Done. Restart the Mac to stop the running driver."
else
	echo "Firebrand is not installed."
fi
