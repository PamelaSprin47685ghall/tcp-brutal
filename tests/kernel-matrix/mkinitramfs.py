#!/usr/bin/env python3
"""Write the minimal initramfs used by the kernel-matrix load check.

The image holds exactly what PID 1 (tests/kernel-matrix/init.c) needs:

    /init           the static checker
    /tbtun.ko       the module under test
    /proc           mount point for the modules list
    /dev/console    the console device

The archive is newc cpio, gzipped.  It is written by hand so that the device
node needs no root and the image is reproducible (uid/gid/mtime all zero).

    mkinitramfs.py --init init_x86_64 --ko tbtun.ko --out initramfs.cpio.gz
"""
import argparse
import gzip
import os
import sys

CPIO_MAGIC = b"070701"
TRAILER = "TRAILER!!!"


def _header(name, ino, mode, size, rdevmajor=0, rdevminor=0):
    # newc order: magic, ino, mode, uid, gid, nlink, mtime, filesize,
    # devmajor, devminor, rdevmajor, rdevminor, namesize, check.
    fields = (
        ino,
        mode,
        0,  # uid
        0,  # gid
        2 if mode & 0o040000 else 1,  # nlink: directories have two
        0,  # mtime
        size,
        0,  # devmajor
        0,  # devminor
        rdevmajor,
        rdevminor,
        len(name) + 1,
        0,  # check
    )
    out = bytearray(CPIO_MAGIC)
    for value in fields:
        out += b"%08x" % value
    out += name.encode("ascii") + b"\0"
    while len(out) % 4:
        out += b"\0"
    return bytes(out)


def _pad(data):
    out = bytearray(data)
    while len(out) % 4:
        out += b"\0"
    return bytes(out)


def add(entries, name, mode, data=b"", rdevmajor=0, rdevminor=0):
    entries.append((name, mode, data, rdevmajor, rdevminor))


def build(paths):
    entries = []
    add(entries, ".", 0o040755)
    add(entries, "proc", 0o040755)
    add(entries, "dev", 0o040755)
    add(entries, "dev/console", 0o020600, rdevmajor=5, rdevminor=1)
    for name, path, mode in paths:
        with open(path, "rb") as f:
            add(entries, name, mode | 0o100000, f.read())

    out = bytearray()
    for ino, (name, mode, data, rmaj, rmin) in enumerate(entries, start=1):
        out += _header(name, ino, mode, len(data), rmaj, rmin)
        out += _pad(data)
    out += _header(TRAILER, len(entries) + 1, 0, 0)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--init", required=True)
    ap.add_argument("--ko", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    image = build(
        [
            ("init", args.init, 0o755),
            ("tbtun.ko", args.ko, 0o644),
        ]
    )
    with open(args.out, "wb") as f:
        with gzip.GzipFile(filename="", mode="wb", fileobj=f, mtime=0, compresslevel=6) as gz:
            gz.write(image)
    print(
        "%s: %d bytes cpio, %d bytes gzip"
        % (args.out, len(image), os.path.getsize(args.out)),
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
