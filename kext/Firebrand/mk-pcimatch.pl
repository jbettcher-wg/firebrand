#!/usr/bin/perl
# IOPCIMatch from the family table, so the two cannot drift: every device id
# in amdgpu/frb_drv.c's pciidlist (amdgpu_drv.c's SI, CIK, Polaris and Vega
# rows), as "0xDDDD1002". With a plist, the IOPCIMatch string of every
# Firebrand personality in it is replaced; without one, the list is printed.
#
#   perl mk-pcimatch.pl <frb_drv.c> [<Info.plist>]
#
# Copyright (c) 2026 Jordan Bettcher
# SPDX-License-Identifier: MIT
use strict;

my ($drv, $plist) = @ARGV;
die "usage: mk-pcimatch.pl <frb_drv.c> [<Info.plist>]\n" unless $drv;
open(my $h, '<', $drv) or die "mk-pcimatch: $drv: $!\n";
my $src = do { local $/; <$h> };
close $h;
$src =~ /\bpciidlist\[\]\s*=\s*\{(.*?)\n\};/s or die "mk-pcimatch: no pciidlist in $drv\n";
my $table = $1;
my (%seen, @ids);
while ($table =~ /\{\s*0x([0-9A-Fa-f]{4}),\s*CHIP_\w+\s*\}/g) {
	my $id = lc($1);
	push @ids, "0x${id}1002" unless $seen{$id}++;
}
die "mk-pcimatch: no ids in pciidlist\n" unless @ids;
my $list = join(' ', @ids);
unless ($plist) {
	print "$list\n";
	exit 0;
}
open($h, '<', $plist) or die "mk-pcimatch: $plist: $!\n";
my $p = do { local $/; <$h> };
close $h;
my $n = ($p =~ s{(<key>IOPCIMatch</key>\s*<string>)[^<]*(</string>)}{$1$list$2}g);
die "mk-pcimatch: no IOPCIMatch in $plist\n" unless $n;
open($h, '>', $plist) or die "mk-pcimatch: $plist: $!\n";
print $h $p;
close $h;
printf "mk-pcimatch: %d device ids in %d personalities\n", scalar(@ids), $n;
