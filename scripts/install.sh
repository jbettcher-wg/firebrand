#!/bin/sh
# Install Firebrand on Mac OS X 10.5.8 (PowerPC). Run it from the built package directory
# (build/dist/Firebrand, which scripts/build-mac.sh makes), on the Mac itself, as root:
#
#   sudo ./install.sh [--no-gl] [--no-audio] [--unlock]
#
# Installs into /System/Library/Extensions:
#   Firebrand.kext            the driver, set up as the framebuffer with 2D, OpenGL, video,
#                             power management and DisplayPort audio
#   RadeonNIGLDriver.bundle   the OpenGL driver (if the package has it; --no-gl leaves it out)
#   RadeonNIGA.plugin         the window server's 2D plug-in
#   HDAudio.kext              DisplayPort audio (--no-audio leaves it out)
# and frbctl into /usr/local/bin.
#
# The driver matches every AMD card of the Southern Islands, Sea Islands, Polaris and Vega
# families (the IOPCIMatch list in Firebrand.kext's Info.plist). A card Firebrand has been
# verified on runs everything; any other card of those families starts display-only, and
# --unlock (or `sudo frbctl unlock` later) lets it try the rest. README.md, "Your card isn't
# listed as verified?".
#
# Install it while the old graphics card is still in the Mac, then shut down and swap the cards
# (README.md, "Installing"). An existing RadeonNIGLDriver.bundle or RadeonNIGA.plugin from another
# driver (RadeonNI) is saved in /Library/Application Support/Firebrand/saved and put back by
# uninstall.sh. Every kext is checked with kextload -nt (it loads nothing) before anything in
# /System/Library/Extensions changes; a Firebrand.kext this replaces is kept in
# /Library/Application Support/Firebrand/previous. Nothing is written to the card.
# To undo: sudo ./uninstall.sh
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
unlock=false
for arg in "$@"; do
	case "$arg" in
	--no-gl) gl=0 ;;
	--no-audio) audio=0 ;;
	--unlock) unlock=true ;;
	*) fail "unknown option $arg (options: --no-gl, --no-audio, --unlock)" ;;
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

# Is a card the driver matches in the Mac now? Fine if not: install with the old card in, then
# swap. The ids are the kext's own IOPCIMatch list. The registry shows an id as four bytes,
# big-endian on PowerPC (1002:699f: <00001002>, <0000699f>); a little-endian <9f690000> is
# read too.
ids=$(perl -0777 -ne 'if (/<key>IOPCIMatch<\/key>\s*<string>([^<]*)</) { for (split " ", $1) { print "$1\n" if /^0x([0-9a-fA-F]{4})1002$/ } }' \
	"$here/Firebrand.kext/Contents/Info.plist" | tr 'A-F' 'a-f' | sort -u)
[ -n "$ids" ] || fail "Firebrand.kext's Info.plist names no AMD device ids"
card=
# each PCI device's vendor and device id, as "vvvv dddd" (one device per "+-o" entry)
present=$(ioreg -r -c IOPCIDevice -l -w0 2>/dev/null | awk '
	function id16(s) {
		sub(/.*-id" = </, "", s)
		return substr(s, 1, 4) == "0000" ? substr(s, 5, 4) : substr(s, 3, 2) substr(s, 1, 2)
	}
	/\+-o / { if (v != "") print v, d; v = ""; d = "" }
	/"vendor-id" = </ { v = id16($0) }
	/"device-id" = </ { d = id16($0) }
	END { if (v != "") print v, d }')
for id in $(echo "$present" | awk '$1 == "1002" { print $2 }'); do
	if echo "$ids" | grep -qx "$id"; then
		card="1002:$id"
		break
	fi
done
if [ -n "$card" ]; then
	echo "Found an AMD card Firebrand matches ($card). If it is not one of the verified cards in"
	echo "README.md it starts display-only; see \"Your card isn't listed as verified?\" there."
else
	echo "No card Firebrand matches is in the Mac right now: the driver is installed and will start"
	echo "when the card is in."
fi

set -e
mkdir -p "$SAVED"
: > "$SUPPORT/installed.txt.new"

# Everything is prepared and checked in a staging folder first; nothing in
# /System/Library/Extensions changes unless every kext passes.
STAGE=/var/tmp/firebrand-install
rm -rf "$STAGE"
mkdir -p "$STAGE"
for b in Firebrand.kext RadeonNIGA.plugin; do cp -R "$here/$b" "$STAGE/"; done
[ "$gl" = 0 ] || cp -R "$here/RadeonNIGLDriver.bundle" "$STAGE/"
[ "$audio" = 0 ] || cp -R "$here/HDAudio.kext" "$STAGE/"

# The settings of an installed driver: the framebuffer personality at the last bring-up stage
# (7, video), with the accelerator; OpenGL announced only with the GL bundle installed. An
# untested card stays display-only unless --unlock (FirebrandUnlock; frbctl unlock / lock).
glval=false
[ "$gl" = 1 ] && glval=true
perl -0777 -pi -e "
	s{(<key>FirebrandStage</key>\s*<integer>)\d+}{\${1}7}g;
	s{(<key>FirebrandFramebuffer</key>\s*)<(true|false)/>}{\${1}<true/>}g;
	s{(<key>FirebrandAccel</key>\s*)<(true|false)/>}{\${1}<true/>}g;
	s{(<key>FirebrandAccelGL</key>\s*)<(true|false)/>}{\${1}<$glval/>}g;
	s{(<key>FirebrandBringUp</key>\s*)<(true|false)/>}{\${1}<false/>}g;
	s{(<key>FirebrandUnlock</key>\s*)<(true|false)/>}{\${1}<$unlock/>}g;
	s{(<key>FirebrandAllowRegWrite</key>\s*)<(true|false)/>}{\${1}<false/>}g;
" "$STAGE/Firebrand.kext/Contents/Info.plist"
plutil -lint "$STAGE/Firebrand.kext/Contents/Info.plist" >/dev/null || fail "the edited Info.plist does not parse"

# Owned by root:wheel and writable by root only, as the kext loader requires.
for b in "$STAGE"/*; do
	chown -R root:wheel "$b"
	find "$b" -type d -exec chmod 755 {} +
	find "$b" -type f -exec chmod go-w {} +
done
# Ask the system whether it would load each kext (dependencies, symbols); load nothing.
for k in "$STAGE"/*.kext; do
	if ! kextload -nt "$k" > "$STAGE/check.txt" 2>&1; then
		cat "$STAGE/check.txt" >&2
		rm -rf "$STAGE"
		fail "the system rejects ${k##*/}; nothing was installed"
	fi
done
rm -f "$STAGE/check.txt"

# Keep another driver's userland bundles (once: a second run does not overwrite the saved copy).
for b in RadeonNIGLDriver.bundle RadeonNIGA.plugin; do
	if [ -d "$SLE/$b" ] && [ ! -d "$SAVED/$b" ] && ! grep -qx "$b" "$SUPPORT/installed.txt" 2>/dev/null; then
		cp -R "$SLE/$b" "$SAVED/$b"
		echo "Saved the existing $b in $SAVED."
	fi
done
# The Firebrand.kext this replaces, if any, is kept in "$SUPPORT/previous".
if [ -d "$SLE/Firebrand.kext" ]; then
	rm -rf "$SUPPORT/previous"
	mkdir -p "$SUPPORT/previous"
	cp -R "$SLE/Firebrand.kext" "$SUPPORT/previous/"
fi

# Move each item beside the old one and rename it into place, so a running window server keeps
# the copy it has loaded.
put() {
	b="$1"
	rm -rf "$SLE/$b.new" "$SLE/$b.old"
	mv "$STAGE/$b" "$SLE/$b.new"
	[ ! -d "$SLE/$b" ] || mv "$SLE/$b" "$SLE/$b.old"
	mv "$SLE/$b.new" "$SLE/$b"
	rm -rf "$SLE/$b.old"
	echo "$b" >> "$SUPPORT/installed.txt.new"
	echo "Installed $SLE/$b."
}
put Firebrand.kext
put RadeonNIGA.plugin
[ "$gl" = 0 ] || put RadeonNIGLDriver.bundle
[ "$audio" = 0 ] || put HDAudio.kext
rm -rf "$STAGE"

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
  4. Check: frbctl report, and System Profiler > Graphics/Displays. A card that is not
     one of the verified ones runs display-only: `sudo frbctl unlock` and a restart try
     the rest. Please send `frbctl report --md` (or Firebrand Monitor's Copy Report) as a
     GitHub issue either way: https://github.com/jbettcher-wg/firebrand/issues

If the Mac does not come up:
  - over ssh: sudo ./uninstall.sh (in this folder) and restart; or
  - over ssh: sudo nvram boot-args="frb_enable=0" and restart (the driver stays installed
    but does not start; sudo nvram -d boot-args undoes it); or
  - put the old card back, start in Safe Mode (hold Shift: no third-party drivers load),
    and run sudo ./uninstall.sh; or start in single-user mode (hold Command-S: Firebrand
    does not load there), then: mount -uw / ; rm -rf /System/Library/Extensions/Firebrand.kext ;
    touch /System/Library/Extensions ; reboot
MSG
