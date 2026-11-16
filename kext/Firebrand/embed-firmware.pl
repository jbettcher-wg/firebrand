#!/usr/bin/perl
# Embed the files firmware.list names into the built kext, ONCE: in the
# FirebrandFirmware personality, which matches nothing and only holds them
# (both driver personalities look it up in the IOCatalogue,
# FirebrandCore::findFirmwareStore). A "Firmware" dictionary (file name ->
# <data>) with each distinct image once, and "FirmwareAliases" (file name ->
# the name its identical bytes are under: linux-firmware's links, such as
# tahiti_ce.bin -> verde_ce.bin). Copies LICENSE.amdgpu into Resources.
# After RadeonNI's embed-firmware.pl; text-level on purpose (10.5's perl has
# no plist module, and the insertion point is fixed by Info.plist).
#
#   perl embed-firmware.pl <kext bundle> <firmware.list> <firmware dir>
#
# Copyright (c) 2026 Jordan Bettcher
# SPDX-License-Identifier: MIT
use strict;
use MIME::Base64;
use Digest::MD5 qw(md5_hex);

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

my ($xml, $bytes, $unique, %canon, @aliases) = ("\t\t\t<key>Firmware</key>\n\t\t\t<dict>\n", 0, 0);
my %seen;
for my $n (@names) {
	die "embed-firmware: $n is listed twice\n" if $seen{$n}++;
	my $blob = slurp("$dir/$n");
	die "embed-firmware: $n is empty\n" unless length $blob;
	$bytes += length $blob;
	my $key = md5_hex($blob) . ':' . length($blob);
	if (my $c = $canon{$key}) {
		die "embed-firmware: $n and $c differ\n" unless slurp("$dir/$c") eq $blob;
		push @aliases, [$n, $c];
		next;
	}
	$canon{$key} = $n;
	$unique += length $blob;
	my $b64 = encode_base64($blob, '');
	$b64 =~ s/(.{1,76})/\t\t\t\t$1\n/g;
	$xml .= "\t\t\t\t<key>$n</key>\n\t\t\t\t<data>\n$b64\t\t\t\t</data>\n";
}
$xml .= "\t\t\t</dict>\n";
$xml .= "\t\t\t<key>FirmwareAliases</key>\n\t\t\t<dict>\n";
$xml .= "\t\t\t\t<key>$_->[0]</key>\n\t\t\t\t<string>$_->[1]</string>\n" for @aliases;
$xml .= "\t\t\t</dict>\n";

# into the FirebrandFirmware personality only
my $p = slurp($plist);
my $n = ($p =~ s/(\n\t\t<key>FirebrandFirmware<\/key>\n\t\t<dict>\n)/$1$xml/g);
die "embed-firmware: no FirebrandFirmware personality in $plist\n" unless $n == 1;
open(my $oh, '>', $plist) or die "embed-firmware: $plist: $!\n";
print $oh $p;
close $oh;

mkdir "$bundle/Contents/Resources";
my $lic = slurp("$dir/LICENSE.amdgpu");
open($oh, '>', "$bundle/Contents/Resources/LICENSE.amdgpu") or die "embed-firmware: LICENSE.amdgpu: $!\n";
print $oh $lic;
close $oh;

printf "embed-firmware: %d files (%d bytes), %d distinct (%d bytes) and %d aliases, into the FirebrandFirmware personality; Info.plist %d bytes\n",
	scalar @names, $bytes, scalar(@names) - scalar(@aliases), $unique, scalar @aliases, length $p;
