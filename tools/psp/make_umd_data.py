#!/usr/bin/env python3
"""Write a UMD_DATA.BIN in the exact on-disc layout a real UMD uses.

Format (48 bytes, matching a genuine disc and ARK-4's own loader):

    <DISC-ID>|<16 hex key>|<version>|<region>  then NUL padding, then '|'

The disc id carries its dash here (LUME-00001) even though PARAM.SFO stores
it without (LUME00001).  The key is all-zero for homebrew.  No trailing
newline - an earlier version wrote one with printf and it corrupted the
field the disc-boot loader reads.

usage: make_umd_data.py <DISC-ID> <output>
"""
import sys

if len(sys.argv) != 3:
    sys.exit(__doc__.strip())

disc_id, out = sys.argv[1], sys.argv[2]
body = f"{disc_id}|0000000000000000|0001|G".encode("ascii")
data = body + b"\x00" * (0x2F - len(body)) + b"|"
assert len(data) == 0x30, len(data)
open(out, "wb").write(data)
