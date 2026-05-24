---
name: disk-debug
description: Dump and inspect the virtual disk contents — superblock, inodes, data blocks, FAT table. Use when debugging filesystem issues.
---

Debug the PseudoFS virtual disk image. Arguments: `$ARGUMENTS` can be:
- `super` — dump the superblock (free counts, stacks)
- `inode <n>` — dump inode number n
- `block <n>` — hex dump data block n
- `free` — show the free block chain (group-linked list)
- `fat` — dump the FAT table (FAT16 mode)
- `dir <path>` — dump directory entries at the given path
- `all` — overview of all regions
- (empty) — same as `all`

Steps:

1. Check if a virtual disk image exists (`pfs.img` or `*.pfs` in the project root or build directory). If not, tell the user to run the program with `--format` first.

2. Read the disk layout constants from `include/core/` or `src/core/` headers to determine offsets:
   - UNIX FS: boot block (block 0), superblock (block 1), inode area (blocks 2-33), data area (blocks 34+)
   - FAT16: boot sector (block 0), FAT1 (blocks 1-4), FAT2 (blocks 5-8), root dir (block 9), data (blocks 10+)

3. Use `xxd` or a small C++ helper to read and parse the binary data:
   - `xxd -s <offset> -l 512 <disk_image>` for raw hex dump of a block
   - For structured dumps (superblock, inode), interpret the bytes according to the struct layout

4. Format the output clearly:
   - Superblock: show free inode count, free block count, stack contents
   - INode: show mode, uid, size, timestamps, addr[] array
   - Directory: show each entry as (name, inode_no) pairs
   - FAT: show cluster chain with status markers (free/used/EOF/bad)
   - Free chain: walk the group-linked list and show each group

5. Highlight anomalies: zero-size files with allocated blocks, orphan inodes, broken chains, etc.
