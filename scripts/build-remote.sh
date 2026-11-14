#!/usr/bin/env bash
# Build the Mac-side pieces on a Mac over ssh from this (Linux or macOS) machine, and bring
# build/dist/Firebrand back here.
#
#   scripts/build-remote.sh [user@]mac
#
# 1. fetches the AMD microcode into firmware/ (scripts/fetch-firmware.sh), here;
# 2. copies the tree -- with firmware/ and, if built, build/gl/RadeonNIGLDriver.bundle -- to
#    $FRB_RDIR (default /tmp/firebrand-build) on the Mac;
# 3. runs scripts/build-mac.sh there (Mac OS X 10.5.8, Xcode 3.1);
# 4. copies the Mac's build/dist/Firebrand to build/dist/Firebrand here.
#
# It installs nothing on the Mac. The copy goes through tar over ssh (Leopard's OpenSSH 4.5 has no
# SFTP for a current scp). A current OpenSSH client needs the old algorithms enabled for 10.5's
# sshd, for example in ~/.ssh/config:
#     Host mymac
#         HostKeyAlgorithms +ssh-rsa
#         PubkeyAcceptedAlgorithms +ssh-rsa
#         KexAlgorithms +diffie-hellman-group14-sha1
#         Ciphers +aes128-cbc
#         MACs +hmac-sha1
#
# Copyright (c) 2026 Jordan Bettcher
# SPDX-License-Identifier: MIT
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MAC="${1:?usage: scripts/build-remote.sh [user@]mac}"
RDIR="${FRB_RDIR:-/tmp/firebrand-build}"
SSH="${SSH:-ssh}"

"$ROOT/scripts/fetch-firmware.sh" >/dev/null

echo "== copy the tree to $MAC:$RDIR"
(cd "$ROOT" && COPYFILE_DISABLE=1 tar czf - --exclude=.git --exclude=build --exclude=local .) |
	$SSH "$MAC" "rm -rf '$RDIR' && mkdir -p '$RDIR' && cd '$RDIR' && tar xzf -"
if [ -d "$ROOT/build/gl/RadeonNIGLDriver.bundle" ]; then
	echo "== and the OpenGL bundle (build/gl)"
	(cd "$ROOT/build" && COPYFILE_DISABLE=1 tar czf - gl/RadeonNIGLDriver.bundle) |
		$SSH "$MAC" "mkdir -p '$RDIR/build' && cd '$RDIR/build' && tar xzf -"
fi

echo "== build on $MAC"
$SSH "$MAC" "cd '$RDIR' && /bin/bash scripts/build-mac.sh"

echo "== bring build/dist/Firebrand back"
mkdir -p "$ROOT/build/dist"
rm -rf "$ROOT/build/dist/Firebrand"
$SSH "$MAC" "cd '$RDIR/build/dist' && tar czf - Firebrand" | tar xzf - -C "$ROOT/build/dist"
echo "== $ROOT/build/dist/Firebrand (also in $MAC:$RDIR/build/dist/Firebrand)"
