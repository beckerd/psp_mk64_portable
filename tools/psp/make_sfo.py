#!/usr/bin/env python3
"""Write a disc PARAM.SFO with the same key set and layout as a real UMD.

mksfoex emits its -s overrides first, so the key table comes out unsorted
(CATEGORY, DISC_ID, DISC_VERSION, BOOTABLE, ...).  The SFO format requires
keys sorted bytewise, and the known-good reference disc carries two keys
mksfoex never writes (DISC_NUMBER, DISC_TOTAL).  This writes exactly the
reference's table:

  BOOTABLE CATEGORY DISC_ID DISC_NUMBER DISC_TOTAL DISC_VERSION
  PARENTAL_LEVEL PSP_SYSTEM_VER REGION TITLE

usage: make_sfo.py --title T --disc-id LUME00001 [--category UG]
                   [--version 1.00] [--parental N] <out.sfo>
"""
import argparse
import struct

FMT_UTF8 = 0x0204
FMT_INT = 0x0404


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--title', required=True)
    ap.add_argument('--disc-id', required=True, help='no dash, e.g. LUME00001')
    ap.add_argument('--category', default='UG')
    ap.add_argument('--version', default='1.00')
    ap.add_argument('--parental', type=int, default=1)
    ap.add_argument('out')
    a = ap.parse_args()

    # (key, fmt, value, max_len) - sorted bytewise by key, as the format requires
    entries = [
        ('BOOTABLE', FMT_INT, 1, 4),
        ('CATEGORY', FMT_UTF8, a.category, 4),
        ('DISC_ID', FMT_UTF8, a.disc_id, 16),
        ('DISC_NUMBER', FMT_INT, 1, 4),
        ('DISC_TOTAL', FMT_INT, 1, 4),
        ('DISC_VERSION', FMT_UTF8, a.version, 8),
        ('PARENTAL_LEVEL', FMT_INT, a.parental, 4),
        ('PSP_SYSTEM_VER', FMT_UTF8, '1.00', 8),
        ('REGION', FMT_INT, 0x8000, 4),
        ('TITLE', FMT_UTF8, a.title, 128),
    ]
    assert [e[0] for e in entries] == sorted(e[0] for e in entries)

    keys = b''
    data = b''
    index = b''
    for key, fmt, val, maxlen in entries:
        if fmt == FMT_INT:
            blob = struct.pack('<I', val)
            length = 4
        else:
            blob = val.encode('utf-8') + b'\x00'
            length = len(blob)
            assert length <= maxlen, key
            blob = blob + b'\x00' * (maxlen - length)
        index += struct.pack('<HHIII', len(keys), fmt, length, maxlen, len(data))
        keys += key.encode('ascii') + b'\x00'
        data += blob
    keys += b'\x00' * (-len(keys) % 4)
    key_off = 20 + len(index)
    data_off = key_off + len(keys)
    hdr = struct.pack('<4sIIII', b'\x00PSF', 0x101, key_off, data_off, len(entries))
    with open(a.out, 'wb') as f:
        f.write(hdr + index + keys + data)


if __name__ == '__main__':
    main()
