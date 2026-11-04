#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Integration test without root: the "disk" is a plain file.
set -euo pipefail
cd "$(dirname "$0")/.."
T=${TMPDIR:-/tmp}/copydisk-test.$$
mkdir -p "$T/root"; [ -n "${KEEP:-}" ] || trap 'rm -rf "$T"' EXIT
D=$T/disk.img; O=$T/mini.img
R=$T/root

# --- populate the rootfs with different object types ---
mkdir -p $R/etc $R/usr/bin $R/var/lib $R/empty $R/lost+found
for i in $(seq 1 300); do echo "file $i" > $R/etc/f$i; done
head -c 5000000 /dev/urandom > $R/usr/bin/big
truncate -s 300M $R/sparse; printf 'X' | dd of=$R/sparse bs=1 seek=150000000 conv=notrunc 2>/dev/null
ln $R/usr/bin/big $R/usr/bin/big.hard
ln -s big $R/usr/bin/big.sym
ln -s "$(printf 'a%.0s' $(seq 1 200))" $R/longlink
mkfifo $R/var/fifo
chmod 4755 $R/usr/bin/big; touch -d '2020-01-02 03:04:05' $R/etc/f1
setfattr -n user.test -v hello $R/etc/f2 2>/dev/null || echo "(xattr not supported by this fs - skipped)"
# regression: big directories must be expanded during the copy (many symlinks and subdirs)
mkdir -p $R/alternatives
for i in $(seq 1 3000); do ln -s /usr/bin/some-long-target-name-$i $R/alternatives/symlink-with-a-fairly-long-name-$i; done
for i in $(seq 1 1500); do mkdir $R/alternatives/subdirectory-with-a-fairly-long-name-$i; done
mkdir -p $R/deep; p=$R/deep; for i in $(seq 1 30); do p=$p/d$i; mkdir $p; done; echo leaf > $p/leaf

# --- 1 GiB disk: ESP 64M + root 800M ---
truncate -s 1G $D
sfdisk -q $D <<S
label: gpt
label-id: 11111111-2222-3333-4444-555555555555
unit: sectors
first-lba: 2048
${D}1 : start=2048, size=131072, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, uuid=AAAAAAAA-0000-4000-8000-000000000001, name="EFI system"
${D}2 : start=133120, size=1638400, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=BBBBBBBB-0000-4000-8000-000000000002, name="rootfs", attrs="LegacyBIOSBootable"
S
mkfs.fat -F32 -i DEADBEEF -n ESP --offset 2048 $D 32768 >/dev/null
mke2fs -q -t ext4 -d $R -U 0a1b2c3d-1111-2222-3333-444455556666 -L rootlabel -E offset=$((133120*512)) $D $((1638400/2))K

echo "=== running copydisk ==="
./copydisk -y -m 32 $D $O > $T/log.txt 2>&1 || { cat $T/log.txt; exit 1; }
grep -vE "^        \.|\.\.\. [0-9]+%" $T/log.txt | tail -n 45

echo "=== checks ==="
OFF=$(awk '$1==2{print $5*512}' $O.layout)
e2fsck -fn "$O?offset=$OFF" && echo "e2fsck OK"
sfdisk -d $O | sed 's|'$O'|X|' > $T/new.dump; sfdisk -d $D | sed 's|'$D'|X|' > $T/old.dump
diff <(grep -E 'uuid|label-id' $T/old.dump | sed 's/size=[ ]*[0-9]*,//; s/start=[ ]*[0-9]*,//') \
     <(grep -E 'uuid|label-id' $T/new.dump | sed 's/size=[ ]*[0-9]*,//; s/start=[ ]*[0-9]*,//') && echo "GPT GUID/PARTUUID/names/attributes match"
echo "UUID/LABEL (original | image):"
paste -d'|' <(blkid -p -O $((133120*512)) -o export $D | grep -E '^(UUID|LABEL|TYPE)=') \
            <(blkid -p -O $OFF -o export $O | grep -E '^(UUID|LABEL|TYPE)=')
[ "$(blkid -p -O $((133120*512)) -o export $D | grep -E '^(UUID|LABEL)=')" = "$(blkid -p -O $OFF -o export $O | grep -E '^(UUID|LABEL)=')" ] && echo "rootfs UUID/LABEL match"
cmp <(dd if=$D bs=512 skip=2048 count=131072 2>/dev/null) <(dd if=$O bs=512 skip=2048 count=131072 2>/dev/null) && echo "ESP is byte-identical (FAT serial preserved)"
mkdir $T/a $T/b
debugfs -R "rdump / $T/a" "$D?offset=$((133120*512))" 2>/dev/null
debugfs -R "rdump / $T/b" "$O?offset=$OFF" 2>/dev/null
diff -r $T/a $T/b || true; diff -rq $T/a $T/b && echo "file tree is identical (rdump)"
for f in usr/bin/big.hard usr/bin/big; do debugfs -R "stat /$f" "$O?offset=$OFF" 2>/dev/null | grep -E "Links|Mode"; done
echo "sparse: on disk $(du -k $T/b/sparse | cut -f1) KiB, size $(stat -c%s $T/b/sparse)"
echo "IMAGE: apparent $(stat -c%s $O), on disk $(du -k $O | cut -f1) KiB"
./copydisk -y -m 32 $D $T/again.img >/dev/null 2>&1
cmp $O $T/again.img && echo "output is reproducible (two runs are byte-identical)"
echo "ALL OK"
