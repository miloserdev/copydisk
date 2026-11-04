# copydisk - a "mini slice" of a GPT disk

Turns a big disk (e.g. a 256 GB NVMe: EFI + root, ~2 GB used) into a small sparse
image file that keeps **all identifiers**:

* disk GUID and the number of GPT slots;
* per partition: slot number, type GUID, **PARTUUID**, name (PARTLABEL), attributes;
* **UUID and LABEL** of the filesystems (the recreated ext filesystem gets the original
  values; all other partitions are copied byte-for-byte, so e.g. the FAT serial is unchanged).

Partitions (EFI, /boot, recovery, ...) are copied 1:1, while the largest ext2/3/4
partition (rootfs) is **recreated smaller** and filled with the content of the original.

Restoring is not part of the program - see "Restoring onto the original disk" below.

## Build

Requires `gcc`/`make`, `pkg-config` and the dev packages: `libfdisk`, `libblkid` (util-linux),
`libext2fs` + `com_err` (e2fsprogs).

```
# Arch/Manjaro: util-linux-libs e2fsprogs      Debian/Ubuntu: libfdisk-dev libblkid-dev libext2fs-dev comerr-dev
make
make test      # integration test on a file-backed "disk", no root needed
```

## Usage

```
copydisk [options] SOURCE IMAGE
  -r N   shrink partition number N (fdisk numbering); default: the largest ext2/3/4
  -n     shrink nothing
  -m MiB free-space margin in the new filesystem (default 256)
  -f     continue if the source journal was not replayed
  -y     overwrite IMAGE
  -p     print the plan only (writes nothing)
  -v     print every file
```

```
sudo ./copydisk -v /dev/nvme0n1 /mnt/backup/nvme.img
./copydisk -p /dev/nvme0n1 x        # see what would happen
```

SOURCE is a block device **or** a disk image file. It is only ever opened read-only.
Output files: `IMAGE`, `IMAGE.sfdisk` (the original GPT in sfdisk format),
`IMAGE.layout` ("before -> after" table).

**Important:** copy from an inactive filesystem (or one mounted read-only / a snapshot),
e.g. from a live USB. A live read-write filesystem changes while it is read, so the copy
could be inconsistent. If the journal was not replayed the program refuses to run
(override with `-f`).

## How it works (the steps in the log)

1. **GPT** is read via *libfdisk*: disk GUID, slots, partition GUID/name/attributes.
2. **libblkid** detects filesystem type, UUID and LABEL of every partition.
3. **Choice and estimate.** Through *libext2fs* all source inodes are scanned: really used
   blocks (`i_blocks`, holes are not counted), inode count and journal size are summed up.
   New size = (data + journal + inode tables + bitmaps/GDT copies) x 1.10 + `-m` margin,
   rounded up to 1 MiB.
4. **Layout.** Partitions before the shrunk one keep their original LBA; the ones after it
   follow back-to-back, 1 MiB aligned. File size = end of the last partition + 1 MiB for the
   backup GPT.
5. **Image** is created with `ftruncate` (sparse); *libfdisk* writes the GPT into it, then the
   GPT is re-read and compared with the plan.
6. **Contents.**
   * ordinary partition - `pread/pwrite` in 1 MiB chunks, zero 4K blocks are skipped (holes);
   * shrunk ext partition - the equivalent of `mke2fs` via *libext2fs* (`ext2fs_initialize`,
     `allocate_tables`, resize/bad-block/root inodes, journal of the same size) with the same
     features, block and inode size, UUID, LABEL, hash seed and checksum seed; then the
     equivalent of `rsync -aHAX`: a recursive walk copying files (sparse, extents), directories,
     symlinks, fifos/sockets/device nodes, hardlinks (tracked by inode), owners, modes, suid,
     timestamps (with nanoseconds and crtime), flags, xattr/POSIX ACL/capabilities.
     No mounts and no loop devices - both filesystems are opened as `file?offset=N`.
   * if space runs out, the file is removed and the run is repeated with a doubled margin
     (up to 3 times).
7. **Restore artifacts** and a final check (UUID/LABEL of the new filesystem).

## Restoring onto the original disk (manual)

```
dd if=nvme.img of=/dev/nvme0n1 bs=4M conv=sparse status=progress   # image -> start of the disk
sfdisk /dev/nvme0n1 < nvme.img.sfdisk                              # put back the ORIGINAL GPT (same GUIDs/sizes)
partprobe /dev/nvme0n1
e2fsck -f /dev/nvme0n1p2 && resize2fs /dev/nvme0n1p2               # grow rootfs to the whole partition
```

If the shrunk partition is the last one (the typical case), the starts of all partitions
match the original. If other partitions followed it, the program warns that they were shifted
and they have to be moved back separately (`.layout` holds both sets of coordinates).

## Limitations

* GPT only; a hybrid MBR is not carried over.
* Not supported: bigalloc, inline_data, MMP. LVM/LUKS/RAID inside partitions are copied as-is
  (not shrunk).
* Quotas (quota/project) and `orphan_file` are not enabled in the new filesystem (a warning is
  logged); quotas can be restored with `tune2fs -O quota`.
* Directories in the copy are linear (no htree) - this is valid and the kernel builds indexes
  as you write; run `e2fsck -fD` if you want them.
* Only ext2/3/4 can be shrunk; btrfs/xfs/f2fs etc. are copied 1:1.

## Layout of the sources

`src/main.c` - orchestration, `layout.c` - GPT/blkid, `extshrink.c` - mkfs + tree copy,
`rawcopy.c` - sparse copy, `log.c` - output, `tests/run.sh` - test, `TZ.md` - the specification.

## License

Licensed under **GPL-2.0-only**, the same as the Linux kernel. See [LICENSE](LICENSE).
Every source file carries an `SPDX-License-Identifier: GPL-2.0-only` line.
Dynamically linked libraries: libfdisk, libblkid (util-linux), libext2fs and com_err (e2fsprogs).
