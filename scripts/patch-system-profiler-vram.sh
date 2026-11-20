#!/bin/bash
# patch-system-profiler-vram.sh -- System Profiler shows a graphics card's real
# video memory above 2047 MB on Mac OS X 10.5.8 (PowerPC).
#
#   sudo ./patch-system-profiler-vram.sh            patch
#   sudo ./patch-system-profiler-vram.sh --restore  put Apple's file back
#        ./patch-system-profiler-vram.sh --status   say which one is installed
#
# WHY THE DRIVER CANNOT DO IT. 10.5.8's System Profiler cannot print more than
# 2047 MB of video memory from anything a driver publishes. Its displays
# reporter (SPDisplaysReporter) takes the card's VRAM,totalsize, sends it
# -intValue -- which SPSupport.framework's NSData category answers with the
# FIRST FOUR BYTES as a signed int -- and prints that divided by 2^20. So
# 0x7fffffff is the ceiling, and an 8-byte value prints its high word (8 GB
# reads 0 MB). Its other sources (IOFBMemorySize, the *,memsize keys) are
# 32-bit too, and this one runs last and wins. Firebrand therefore publishes
# VRAM,totalsize capped at 2047 MB, and the whole size in MB as VRAM,totalMB.
#
# THE FIX. The 32 instructions of that statement (ppc slice 0x4a4c-0x4acb)
# are rewritten to ask for VRAM,totalMB first and print it as it is; without
# it, Apple's VRAM,totalsize arithmetic runs exactly as before, so every other
# card reads as it did (no Apple driver publishes VRAM,totalMB). The string it
# needs is Apple's "gMux-version" with its 12 characters changed to
# "VRAM,totalMB": that key is only looked up on the graphics switch of Intel
# MacBook Pros, which no PowerPC Mac has. Only the ppc slice changes. The new
# block (r31 = 0x373c is the method's PIC base; the other registers are
# Apple's own; 0xfffeff00 is objc_msgSend in the commpage):
#
#   lwz r2,0x764(r1); addi r22,r2,0x5c98      key: spdisplays_vram
#   lwz r3,4(r16); lwz r4,0x68f0(r25)         [props objectForKey:
#   addi r5,r31,0x5e38; bla 0xfffeff00            @"VRAM,totalMB"]
#   cmpwi cr7,r3,0; beq cr7,bytes
#   lwz r4,0x6930(r31); bla 0xfffeff00        -intValue: the MB count
#   mr r6,r3; b fmt
#   bytes: lwz r3,4(r16); lwz r4,0x68f0(r25)  Apple's path:
#   addi r5,r31,0x5e58; bla 0xfffeff00        [props objectForKey:@"VRAM,totalsize"]
#   cmpwi cr7,r3,0; beq cr7,0x4acc            neither key: set nothing
#   lwz r4,0x6930(r31); bla 0xfffeff00        -intValue
#   srawi r6,r3,20; addze r6,r6               bytes / 2^20
#   fmt: lwz r3,0x699c(r31); lwz r4,0x6918(r31)
#   addi r5,r31,0x5d58; bla 0xfffeff00        [NSString stringWithFormat:@"%ld MB", r6]
#   lwz r4,0x68e8(r19); mr r5,r3; mr r3,r30   [out setObject:that forKey:r22]
#   mr r6,r22; bla 0xfffeff00; nop
#
# It REFUSES unless the file is byte for byte Apple's 10.5.8 reporter (md5
# below) with the original bytes in both places, and the result must match
# PATCHED_MD5 with exactly NDIFF bytes changed. Apple's file is kept as
#     /var/root/SPDisplaysReporter.apple-38b09de9
# and --restore copies it back. System Profiler loads the reporter on every
# run, so either direction shows the next time it is opened.
set -u
MODE="${1:-patch}"

F=/System/Library/SystemProfiler/SPDisplaysReporter.spreporter/Contents/MacOS/SPDisplaysReporter
BAK=/var/root/SPDisplaysReporter.apple-38b09de9
ORIG_MD5=38b09de9130b4702857a78a75a88b0fd      # Apple's 10.5.8 SPDisplaysReporter
PATCHED_MD5=3779759a979b8053f219978525e1371b
NDIFF=125                                      # bytes that differ from Apple's file

# the block: the ppc slice is at fat offset 73728; + 0x4a4c, 128 bytes
VRAM_OFF=92748
VRAM_LEN=128
VRAM_ORIG_MD5=61e923660b883a22950b747990b50783
VRAM_NEW=804107643ac25c9880700004809968f038bf5e384bfeff032f830000419e0014809f69304bfeff037c661b784800002c80700004809968f038bf5e584bfeff032f830000419e003c809f69304bfeff037c66a6707cc60194807f699c809f691838bf5d584bfeff03809368e87c651b787fc3f3787ec6b3784bfeff0360000000
# the string: 73728 + 0x7d24, "gMux-version" -> "VRAM,totalMB"
STR_OFF=105764
STR_ORIG=674d75782d76657273696f6e
STR_NEW=5652414d2c746f74616c4d42

die() { echo "$*" >&2; exit 2; }
cur=$(md5 -q "$F") || die "cannot read $F"
case $MODE in
--status)
    case $cur in
    $ORIG_MD5) echo "Apple's SPDisplaysReporter (VRAM shows at most 2047 MB)" ;;
    $PATCHED_MD5) echo "patched SPDisplaysReporter (VRAM from VRAM,totalMB when the driver publishes it)" ;;
    *) echo "unknown SPDisplaysReporter, md5 $cur" ;;
    esac
    exit 0 ;;
--restore|patch) [ "$(id -u)" = 0 ] || die "run it with sudo" ;;
*) die "usage: $0 [--restore|--status]" ;;
esac

if [ "$MODE" = --restore ]; then
    [ "$cur" = $ORIG_MD5 ] && { echo "already Apple's file"; exit 0; }
    [ "$(md5 -q "$BAK" 2>/dev/null)" = $ORIG_MD5 ] || die "REFUSE: $BAK missing or not Apple's file"
    cp -p "$BAK" "$F.new" && mv -f "$F.new" "$F"
    echo "restored: $(md5 -q "$F")"
    exit 0
fi

[ "$cur" = $PATCHED_MD5 ] && { echo "already patched"; exit 0; }
[ "$cur" = $ORIG_MD5 ] || die "REFUSE: md5 $cur is not Apple's 10.5.8 SPDisplaysReporter ($ORIG_MD5)"
[ -f "$BAK" ] || cp -p "$F" "$BAK"
[ "$(md5 -q "$BAK")" = $ORIG_MD5 ] || die "REFUSE: backup $BAK is not Apple's file"
blk=$(dd if="$BAK" bs=1 skip=$VRAM_OFF count=$VRAM_LEN 2>/dev/null | md5 -q)
[ "$blk" = $VRAM_ORIG_MD5 ] || die "REFUSE: $VRAM_LEN bytes at $VRAM_OFF have md5 $blk, expected $VRAM_ORIG_MD5"
str=$(dd if="$BAK" bs=1 skip=$STR_OFF count=12 2>/dev/null | xxd -p)
[ "$str" = $STR_ORIG ] || die "REFUSE: bytes at $STR_OFF are $str, expected $STR_ORIG"
cp -p "$BAK" "$F.new"
echo $VRAM_NEW | xxd -r -p | dd of="$F.new" bs=1 seek=$VRAM_OFF conv=notrunc 2>/dev/null
echo $STR_NEW | xxd -r -p | dd of="$F.new" bs=1 seek=$STR_OFF conv=notrunc 2>/dev/null
new=$(md5 -q "$F.new")
[ "$new" = $PATCHED_MD5 ] || { rm -f "$F.new"; die "REFUSE: result md5 $new is not the expected patched file"; }
n=$(cmp -l "$BAK" "$F.new" | wc -l | tr -d ' ')
[ "$n" = $NDIFF ] || { rm -f "$F.new"; die "REFUSE: $n bytes differ from Apple's file, expected $NDIFF"; }
mv -f "$F.new" "$F"
echo "patched: $new (Apple's file kept as $BAK)"
