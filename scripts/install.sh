#!/bin/sh
# Install Firebrand on Mac OS X 10.5.8 (PowerPC). Run it from the built package directory
# (build/dist/Firebrand, which scripts/build-mac.sh makes), on the Mac itself, as root:
#
#   sudo ./install.sh [--no-gl] [--no-audio]
#
# Installs into /System/Library/Extensions:
#   Firebrand.kext            the driver, set up as the framebuffer with 2D, OpenGL, video,
#                             power management and DisplayPort audio
#   RadeonNIGLDriver.bundle   the OpenGL driver (if the package has it; --no-gl leaves it out)
#   RadeonNIGA.plugin         the window server's 2D plug-in
#   HDAudio.kext              DisplayPort audio (--no-audio leaves it out)
# and frbctl into /usr/local/bin.
#
# Install it while the old graphics card is still in the Mac, then shut down and swap the cards
# (README.md, "Installing"). An existing RadeonNIGLDriver.bundle or RadeonNIGA.plugin from another
# driver (RadeonNI) is saved in /Library/Application Support/Firebrand/saved and put back by
# uninstall.sh. Nothing is written to the card. To undo: sudo ./uninstall.sh
#
# Copyright (c) 2026 Jordan Bettcher
# SPDX-License-Identifier: MIT

SLE=/System/Library/Extensions
SUPPORT="/Library/Application Support/Firebrand"
SAVED="$SUPPORT/saved"
here=$(cd "$(dirname "$0")" && pwd)

fail() { echo "install.sh: $*" >&2; exit 1; }

gl=1
audio=1
for arg in "$@"; do
	case "$arg" in
	--no-gl) gl=0 ;;
	--no-audio) audio=0 ;;
	*) fail "unknown option $arg (options: --no-gl, --no-audio)" ;;
	esac
done

[ "$(id -u)" = 0 ] || fail "run as root: sudo ./install.sh"
case "$(sw_vers -productVersion 2>/dev/null)" in
10.5*) ;;
*) fail "Firebrand is for Mac OS X 10.5 (this is $(sw_vers -productVersion 2>/dev/null))" ;;
esac
[ "$(uname -p)" = powerpc ] || fail "Firebrand is for PowerPC"
[ -d "$here/Firebrand.kext" ] || fail "Firebrand.kext is not next to this script (run it from build/dist/Firebrand)"
[ -d "$here/RadeonNIGA.plugin" ] || fail "RadeonNIGA.plugin is not next to this script"
if [ "$gl" = 1 ] && [ ! -d "$here/RadeonNIGLDriver.bundle" ]; then
	echo "Note: this package has no RadeonNIGLDriver.bundle; OpenGL acceleration stays off."
	gl=0
fi
[ "$audio" = 0 ] || [ -d "$here/HDAudio.kext" ] || fail "HDAudio.kext is not next to this script (or use --no-audio)"

# Is a supported card in the Mac now? Fine if not: install with the old card in, then swap.
if ioreg -r -c IOPCIDevice -l -w0 2>/dev/null | grep -q '"device-id" = <9f690000>'; then
	echo "Found an AMD Polaris 12 card (1002:699f)."
else
	echo "No Polaris 12 card (1002:699f) in the Mac right now: the driver is installed and will"
	echo "start when the card is in."
fi

set -e
mkdir -p "$SAVED"
: > "$SUPPORT/installed.txt.new"

# Copy a bundle beside the old one and rename it into place, so a running window server keeps
# the copy it has loaded.
put() {
	b="$1"
	rm -rf "$SLE/$b.new" "$SLE/$b.old"
	cp -R "$here/$b" "$SLE/$b.new"
	chown -R root:wheel "$SLE/$b.new"
	chmod -R go-w "$SLE/$b.new"
	[ ! -d "$SLE/$b" ] || mv "$SLE/$b" "$SLE/$b.old"
	mv "$SLE/$b.new" "$SLE/$b"
	rm -rf "$SLE/$b.old"
	echo "$b" >> "$SUPPORT/installed.txt.new"
	echo "Installed $SLE/$b."
}

# Keep another driver's userland bundles (once: a second run does not overwrite the saved copy).
for b in RadeonNIGLDriver.bundle RadeonNIGA.plugin; do
	if [ -d "$SLE/$b" ] && [ ! -d "$SAVED/$b" ] && ! grep -qx "$b" "$SUPPORT/installed.txt" 2>/dev/null; then
		cp -R "$SLE/$b" "$SAVED/$b"
		echo "Saved the existing $b in $SAVED."
	fi
done

put Firebrand.kext
# The settings of an installed driver: the framebuffer personality at the last bring-up stage
# (7, video), with the accelerator; OpenGL announced only with the GL bundle installed.
glval=false
[ "$gl" = 1 ] && glval=true
perl -0777 -pi -e "
	s{(<key>FirebrandStage</key>\s*<integer>)\d+}{\${1}7}g;
	s{(<key>FirebrandFramebuffer</key>\s*)<(true|false)/>}{\${1}<true/>}g;
	s{(<key>FirebrandAccel</key>\s*)<(true|false)/>}{\${1}<true/>}g;
	s{(<key>FirebrandAccelGL</key>\s*)<(true|false)/>}{\${1}<$glval/>}g;
	s{(<key>FirebrandBringUp</key>\s*)<(true|false)/>}{\${1}<false/>}g;
	s{(<key>FirebrandAllowRegWrite</key>\s*)<(true|false)/>}{\${1}<false/>}g;
" "$SLE/Firebrand.kext/Contents/Info.plist"
plutil -lint "$SLE/Firebrand.kext/Contents/Info.plist" >/dev/null || fail "the edited Info.plist does not parse"
put RadeonNIGA.plugin
[ "$gl" = 0 ] || put RadeonNIGLDriver.bundle
[ "$audio" = 0 ] || put HDAudio.kext

mkdir -p /usr/local/bin
cp "$here/frbctl" /usr/local/bin/frbctl
chown root:wheel /usr/local/bin/frbctl
chmod 755 /usr/local/bin/frbctl
echo "/usr/local/bin/frbctl" >> "$SUPPORT/installed.txt.new"
mv "$SUPPORT/installed.txt.new" "$SUPPORT/installed.txt"

# The extension caches are rebuilt at the next start.
rm -f /System/Library/Extensions.mkext /System/Library/Extensions.kextcache
touch "$SLE"
sync

cat <<'MSG'

Firebrand is installed. Next:
  1. Make sure Remote Login (ssh) is on (System Preferences > Sharing) and note this
     Mac's address: if the screen stays dark you can still log in and undo this.
  2. Shut down, put the AMD card in (if it is not in yet) and connect a DisplayPort monitor.
  3. Start the Mac. There is no picture until the driver starts (the card has no Open
     Firmware driver); then the Firebrand boot screen appears, then the login window.
  4. Check: frbctl info, and System Profiler > Graphics/Displays.

If the Mac does not come up:
  - over ssh: sudo ./uninstall.sh (in this folder) and restart; or
  - over ssh: sudo nvram boot-args="frb_enable=0" and restart (the driver stays installed
    but does not start; sudo nvram -d boot-args undoes it); or
  - put the old card back, start in Safe Mode (hold Shift: no third-party drivers load),
    and run sudo ./uninstall.sh.
MSG
