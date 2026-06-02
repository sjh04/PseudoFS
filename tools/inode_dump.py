#!/usr/bin/env python3
"""Dump a UNIX-engine inode's address array from a PseudoFS disk image.

Useful for *seeing* the mixed-indexing (direct / single-indirect / double-
indirect) actually in use — e.g. after writing a large file.

Usage:
    python3 tools/inode_dump.py <image.img> [inode_no]

If inode_no is omitted, the largest regular file's inode is shown (handy right
after building a big test file). The inode-area offsets are fixed regardless of
DATA_BLK_NUM, so this works for both the default 512-block disk and an enlarged
one.
"""
import struct
import sys

BLOCK = 512
INODE_START_BLK = 2          # boot=0, super=1, inodes start at 2
INODE_BLK_NUM = 32
INODE_SIZE = 64
INODES_PER_BLK = BLOCK // INODE_SIZE          # 8
INODE_TOTAL = INODE_BLK_NUM * INODES_PER_BLK  # 256
DATA_START_BLK = INODE_START_BLK + INODE_BLK_NUM  # 34
PTRS_PER_BLK = BLOCK // 2                      # 256 uint16 pointers per block

MODE_FILE, MODE_DIR, MODE_SYMLINK = 0x0400, 0x0800, 0x1000


def inode_offset(ino):
    return (INODE_START_BLK + ino // INODES_PER_BLK) * BLOCK + (ino % INODES_PER_BLK) * INODE_SIZE


def read_inode(img, ino):
    o = inode_offset(ino)
    mode, nlink, uid, gid = struct.unpack_from("<HHHH", img, o)
    size = struct.unpack_from("<I", img, o + 8)[0]
    addr = struct.unpack_from("<10H", img, o + 24)
    return dict(ino=ino, mode=mode, nlink=nlink, uid=uid, gid=gid, size=size, addr=addr)


def ftype(mode):
    if mode & MODE_DIR:
        return "dir"
    if mode & MODE_SYMLINK:
        return "symlink"
    if mode & MODE_FILE:
        return "file"
    return "free/unused"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    img = open(sys.argv[1], "rb").read()
    if len(img) < DATA_START_BLK * BLOCK:
        print("image too small / not a UNIX-engine image")
        return 1

    if len(sys.argv) >= 3:
        ino = int(sys.argv[2])
    else:
        best = max((read_inode(img, i) for i in range(INODE_TOTAL)),
                   key=lambda d: d["size"] if (d["mode"] & MODE_FILE) else -1)
        ino = best["ino"]
        print(f"(auto-selected largest regular file: inode #{ino})")

    d = read_inode(img, ino)
    print(f"inode #{d['ino']}  type={ftype(d['mode'])}  mode=0o{d['mode'] & 0o777:o}"
          f"  nlink={d['nlink']}  uid={d['uid']}  size={d['size']} bytes")
    labels = [f"direct[{i}]" for i in range(6)] + \
             ["single-indirect[6]", "single-indirect[7]", "DOUBLE-indirect[8]", "triple(reserved)[9]"]
    print("  di_addr[10] (0 = unused / hole):")
    for i, (lb, a) in enumerate(zip(labels, d["addr"])):
        print(f"    addr[{i}]  {lb:22s} = {a:6d}  {'<used>' if a else '<unused>'}")

    dbl = d["addr"][8]
    if dbl:
        l1 = struct.unpack_from("<256H", img, (DATA_START_BLK + dbl) * BLOCK)
        singles = [x for x in l1 if x]
        data_blocks = 0
        for s in singles:
            l2 = struct.unpack_from("<256H", img, (DATA_START_BLK + s) * BLOCK)
            data_blocks += sum(1 for x in l2 if x)
        print(f"  double-indirect addr[8]=#{dbl}: {len(singles)} single-indirect block(s) "
              f"-> {data_blocks} data block(s) ({data_blocks * BLOCK} bytes via double indirection)")

    a = d["addr"]
    levels = []
    if any(a[:6]):
        levels.append("direct")
    if a[6] or a[7]:
        levels.append("single-indirect")
    if a[8]:
        levels.append("double-indirect")
    print("  index levels in use:", " + ".join(levels) if levels else "(none)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
