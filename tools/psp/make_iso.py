#!/usr/bin/env python3
"""Master a PSP UMD-style ISO9660/XA image, byte-matched to a known-good disc.

Why not mkisofs?  Field-by-field diffing release/tetralumen.iso (mkisofs -xa
-iso-level 1) against 3rdparty_sample/glitchy-alpha-0_10.iso - a homebrew ISO
that boots on David's real ARK-4 PSP - showed mkisofs output differs from the
reference (and from a genuine UMD) in ways we cannot switch off:

  * every file name carries an ISO9660 version suffix (EBOOT.BIN;1); real
    discs and the reference have none.  ARK's VSH reader strips ';1' but the
    firmware isofs that loads EBOOT.BIN in game mode is not known to.
  * zero-length files (BOOT.BIN, OPNSSMP.BIN) get an extent of 0xFFFFFFF0,
    way outside the volume; the reference points them at the next file's LBA.
  * the PVD application-use area lacks the disc-id string
    "XXXX-NNNNN|<16 hex>|0001" at 0x373 that every UMD (and the reference)
    carries alongside the CD-XA001 signature at 0x400.
  * no optional path tables, file-structure version 1 vs 2, publisher /
    preparer / dates differ, system-area sectors 14-15 differ.

This writer reproduces the reference layout exactly:

  sector  0-13  zero            sector 14-15  0x20 fill (as the reference)
  sector 16     PVD             sector 17     terminator
  sector 18/19  L path table + optional copy
  sector 20/21  M path table + optional copy
  sector 22..   one 2048-byte sector per directory (root, then BFS order)
  then          UMD_DATA.BIN first, then every other file in tree order

Directory records carry the 14-byte CD-ROM XA system-use entry
(attr 0x0d55 for files, 0x8d55 for directories, "XA" signature), no version
suffixes, names sorted bytewise as ISO9660 requires.

usage: make_iso.py --volid VOL --disc-id XXXX-NNNNN [--publisher P]
                   [--preparer P] -o out.iso <root-dir>
"""
import argparse
import os
import struct
import sys
import time

SECTOR = 2048
XA_FILE = 0x0D55
XA_DIR = 0x8D55


def both(fmt_le, fmt_be, v):
    return struct.pack(fmt_le, v) + struct.pack(fmt_be, v)


def both32(v):
    return both('<I', '>I', v)


def both16(v):
    return both('<H', '>H', v)


def pad(b, n, fill=b' '):
    if len(b) > n:
        sys.exit(f"field too long ({len(b)} > {n}): {b!r}")
    return b + fill * (n - len(b))


def dir_date(t):
    tm = time.gmtime(t)
    return bytes([tm.tm_year - 1900, tm.tm_mon, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, 0])


def vol_date(t):
    tm = time.gmtime(t)
    return time.strftime("%Y%m%d%H%M%S00", tm).encode() + b'\x00'


ZERO_VOL_DATE = b'0' * 16 + b'\x00'


class Node:
    def __init__(self, name, path, is_dir, parent):
        self.name = name            # bytes, as written to the disc
        self.path = path
        self.is_dir = is_dir
        self.parent = parent
        self.children = []
        self.lba = 0
        self.size = 0
        self.dir_no = 0             # path-table index (dirs only)


def scan(root_path):
    root = Node(b'\x00', root_path, True, None)

    def walk(node):
        names = sorted(os.listdir(node.path))
        for n in names:
            if n.startswith('.'):
                continue
            p = os.path.join(node.path, n)
            child = Node(n.encode('ascii'), p, os.path.isdir(p), node)
            if not child.is_dir:
                child.size = os.path.getsize(p)
            node.children.append(child)
            if child.is_dir:
                walk(child)
        node.children.sort(key=lambda c: c.name)
    walk(root)
    return root


def record(node, name, lba, size, flags, date, xa=True):
    su = struct.pack('>HHH', 0, 0, XA_DIR if flags & 2 else XA_FILE) + b'XA' + b'\x00' * 6
    body = (b'\x00' + both32(lba) + both32(size) + date + bytes([flags, 0, 0])
            + both16(1) + bytes([len(name)]) + name)
    if len(name) % 2 == 0:
        body += b'\x00'
    rec = body + su if xa else body
    rec = bytes([len(rec) + 1]) + rec
    return rec


def build_dir_sector(node, date):
    """All directories in the reference fit one sector; enforce that."""
    recs = [record(node, b'\x00', node.lba, node.size, 2, date),
            record(node, b'\x01', (node.parent or node).lba,
                   (node.parent or node).size, 2, date)]
    for c in node.children:
        recs.append(record(c, c.name, c.lba, c.size, 2 if c.is_dir else 0, date))
    data = b''.join(recs)
    if len(data) > SECTOR:
        sys.exit(f"directory {node.path} has too many entries for one sector "
                 f"({len(data)} bytes) - extend make_iso.py to multi-sector dirs")
    return pad(data, SECTOR, b'\x00')


def path_table(dirs, little):
    out = b''
    for d in dirs:
        name = d.name
        ext = struct.pack('<I' if little else '>I', d.lba)
        par = struct.pack('<H' if little else '>H', d.parent.dir_no if d.parent else 1)
        ent = bytes([len(name), 0]) + ext + par + name
        if len(name) % 2:
            ent += b'\x00'
        out += ent
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--volid', required=True)
    ap.add_argument('--disc-id', required=True, help='e.g. LUME-00001 (with dash)')
    ap.add_argument('--disc-hash', default='0' * 16)
    ap.add_argument('--publisher', default='')
    ap.add_argument('--preparer', default='')
    ap.add_argument('-o', '--out', required=True)
    ap.add_argument('root')
    a = ap.parse_args()

    now = int(os.environ.get('SOURCE_DATE_EPOCH', time.time()))
    ddate = dir_date(now)

    root = scan(a.root)

    # directories in path-table order: root, then breadth first
    dirs = [root]
    i = 0
    while i < len(dirs):
        dirs += [c for c in dirs[i].children if c.is_dir]
        i += 1
    for n, d in enumerate(dirs, 1):
        d.dir_no = n
        d.size = SECTOR

    ptab_l = path_table(dirs, True)  # size known before LBAs (names only affect len)
    lba = 22
    for d in dirs:
        d.lba = lba
        lba += 1

    # files: UMD_DATA.BIN first (as on the reference), then tree order.
    files = []

    def collect(n):
        for c in n.children:
            if c.is_dir:
                collect(c)
            else:
                files.append(c)
    collect(root)
    umd = [f for f in files if f.parent is root and f.name == b'UMD_DATA.BIN']
    if not umd:
        sys.exit("root must contain UMD_DATA.BIN")
    files = umd + [f for f in files if f not in umd]
    for f in files:
        f.lba = lba                      # empty files point at the next file
        lba += (f.size + SECTOR - 1) // SECTOR
    total = lba

    ptab_l = path_table(dirs, True)
    ptab_m = path_table(dirs, False)
    root_rec = record(root, b'\x00', root.lba, root.size, 2, ddate, xa=False)
    assert len(root_rec) == 34

    pvd = b''.join([
        b'\x01', b'CD001', b'\x01', b'\x00',
        pad(b'PSP GAME', 32), pad(a.volid.encode(), 32),
        b'\x00' * 8, both32(total), b'\x00' * 32,
        both16(1), both16(1), both16(SECTOR), both32(len(ptab_l)),
        struct.pack('<I', 18), struct.pack('<I', 19),
        struct.pack('>I', 20), struct.pack('>I', 21),
        root_rec,
        pad(b'', 128), pad(a.publisher.encode(), 128), pad(a.preparer.encode(), 128),
        pad(b'PSP GAME', 128), pad(b'', 37), pad(b'', 37), pad(b'', 37),
        vol_date(now), ZERO_VOL_DATE, ZERO_VOL_DATE, ZERO_VOL_DATE,
        b'\x02', b'\x00',
    ])
    assert len(pvd) == 883, len(pvd)
    app = bytearray(b' ' * 512)
    ident = f"{a.disc_id}|{a.disc_hash}|0001".encode()
    app[0:len(ident)] = ident
    app[0x400 - 883:0x400 - 883 + 8] = b'CD-XA001'
    pvd += bytes(app)
    pvd = pad(pvd, SECTOR, b'\x00')

    term = pad(b'\xff' + b'CD001' + b'\x01', SECTOR, b'\x00')

    with open(a.out, 'wb') as out:
        out.write(b'\x00' * (14 * SECTOR))
        out.write(b' ' * (2 * SECTOR))
        out.write(pvd)
        out.write(term)
        out.write(pad(ptab_l, SECTOR, b'\x00'))
        out.write(pad(ptab_l, SECTOR, b'\x00'))
        out.write(pad(ptab_m, SECTOR, b'\x00'))
        out.write(pad(ptab_m, SECTOR, b'\x00'))
        for d in dirs:
            out.write(build_dir_sector(d, ddate))
        for f in files:
            assert out.tell() == f.lba * SECTOR, (f.path, out.tell(), f.lba)
            with open(f.path, 'rb') as src:
                while True:
                    chunk = src.read(1 << 20)
                    if not chunk:
                        break
                    out.write(chunk)
            out.write(b'\x00' * (-f.size % SECTOR))
        assert out.tell() == total * SECTOR
    print(f"{a.out}: {total} sectors, {len(files)} files, {len(dirs)} dirs")


if __name__ == '__main__':
    main()
