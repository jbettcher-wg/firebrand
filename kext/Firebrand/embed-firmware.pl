#!/usr/bin/perl
# Embed the files firmware.list names into the built kext's personality as a
# "Firmware" dictionary (file name -> <data>), and copy LICENSE.amdgpu into
# Resources. After RadeonNI's embed-firmware.pl; text-level on purpose (the
# system perl of 10.5 has no plist module, and the insertion point is fixed by
# Info.plist).
#
#   perl embed-firmware.pl <kext bundle> <firmware.list> <firmware dir>
#
# Copyright (c) 2026 Jordan Bettcher
# SPDX-License-Identifier: MIT
use strict;
use MIME::Base64;

my ($bundle, $list, $dir) = @ARGV;
die "usage: embed-firmware.pl <kext> <firmware.list> <firmware dir>\n" unless $dir;
my $plist = "$bundle/Contents/Info.plist";

sub slurp {
	my ($f) = @_;
	local $/;
	open(my $h, '<', $f) or die "embed-firmware: $f: $!\n" .
		"run scripts/fetch-firmware.sh first; the kext carries its firmware\n";
	binmode $h;
	my $d = <$h>;
	close $h;
	return $d;
}

my @names;
open(my $lh, '<', $list) or die "embed-firmware: $list: $!\n";
while (<$lh>) {
	s/#.*//;
	my @f = split;
	next unless @f;
	die "embed-firmware: $list:$.: one file name per line\n" unless @f == 1 && $f[0] =~ /^[a-z0-9_]+\.bin$/;
	push @names, $f[0];
}
close $lh;

my ($xml, $bytes) = ("\t\t\t<key>Firmware</key>\n\t\t\t<dict>\n", 0);
for my $n (@names) {
	my $blob = slurp("$dir/$n");
	die "embed-firmware: $n is empty\n" unless length $blob;
	my $b64 = encode_base64($blob, '');
	$b64 =~ s/(.{1,76})/\t\t\t\t$1\n/g;
	$xml .= "\t\t\t\t<key>$n</key>\n\t\t\t\t<data>\n$b64\t\t\t\t</data>\n";
	$bytes += length $blob;
}
$xml .= "\t\t\t</dict>\n";

# into every personality (Firebrand and FirebrandFramebuffer): each instance
# reads the microcode from its own personality
my $p = slurp($plist);
my $n = ($p =~ s/(\n\t\t<key>Firebrand\w*<\/key>\n\t\t<dict>\n)/$1$xml/g);
die "embed-firmware: no Firebrand personality in $plist\n" unless $n;
open(my $oh, '>', $plist) or die "embed-firmware: $plist: $!\n";
print $oh $p;
close $oh;

mkdir "$bundle/Contents/Resources";
my $lic = slurp("$dir/LICENSE.amdgpu");
open($oh, '>', "$bundle/Contents/Resources/LICENSE.amdgpu") or die "embed-firmware: LICENSE.amdgpu: $!\n";
print $oh $lic;
close $oh;

printf "embed-firmware: %d files (%d bytes) into %d personalities; Info.plist %d bytes\n", scalar @names, $bytes, $n, length $p;
