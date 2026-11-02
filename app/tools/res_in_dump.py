#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Charles Vestal
"""Put a firmware's resource region into a full-flash dump, so M-Flasher can carry it.

Why this exists. build.py writes the samples region as a SEPARATE file
(build/NAME-res.bin) and the package (build/NAME.fwsc) does not contain it: the
package's flash image is FLASH_SIZE = 0x93000 bytes, and the resource region starts at
0x97000 (app.h APP_RES_BASE). The platform's own way to write it is a USB-MIDI SysEx
transfer (tools/fm1_resources.py: RES_REQ 0x44, the same read-request serve as a
backup restore), which needs mido and a cable.

Whoever flashes with M-Flasher flashes ONE image and nothing else, so the region never
gets written and the instrument reports NO SAMPLES. This script closes that gap offline:
it takes a full flash dump (the one M-Flasher makes before it writes anything -- keep
it, it is also the unbricking image), puts the resource bytes at APP_RES_BASE, and
writes a new dump to flash instead.

    python3 tools/res_in_dump.py DUMP.bin build/NAME-res.bin -o OUT.bin
    python3 tools/res_in_dump.py DUMP.bin build/NAME-res.bin -o OUT.bin --pad 0x100000

The dump is not modified in place: a bad dump plus a bad region is a brick, and the
one thing this script must never do is destroy the file that can unbrick the unit.

Checks, because the failure modes here are all silent on the device:
  * the region's own header is verified (RES1, its length, both CRCs) -- a truncated
    .bin would otherwise be written and then rejected by res_valid(), which looks
    exactly like "the samples did not pack in";
  * the write range is inside [APP_RES_BASE, APP_RES_BASE + sectors*4096);
  * it does not overlap the store objects, which live ABOVE the region (APP_STORE_MAP);
  * the first 32 bytes at APP_RES_BASE in the dump are erased (0xFF) or already this
    exact region -- anything else is somebody else's data and this will not overwrite it
    without --force. (An older build's resource header is the expected case: --force.)

The image M-Flasher reads is the flash as the chip sees it through XIP, which is NOT
plain: the boot header, the directory and the app area are encoded (SDK SFC/ENC,
tools/fm1pkg_make.py). So only the region's own bytes are written here, at their real
offset, and the rest of the dump is left byte for byte as it came. That is the whole
reason this works offline: nothing needs re-encoding, because nothing else changes.
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

HEAD = 32
MAGIC = 0x31534552                     # "RES1"
APP_RES_BASE = 0x97000                 # app/app.h
APP_RES_SECTORS = 69                   # app/app.h: 4 KiB sectors, 0x97000..0xDC000
STORE_OBJS = (0xFC000, 0xDC000, 0xDE000)   # app/app.h APP_STORE_MAP: above the region


def check_region(region):
    """the payload of a NAME-res.bin, header verified -> (length, crc)"""
    if len(region) < HEAD:
        raise SystemExit(f"region: {len(region)} B is too short for a resource file")
    magic, length, crc = struct.unpack_from("<3I", region)
    hcrc = struct.unpack_from("<I", region, 28)[0]
    if magic != MAGIC:
        raise SystemExit(f"region: magic {magic:#x} is not RES1 -- that is not a "
                         f"tools/build.py -res.bin")
    if hcrc != zlib.crc32(region[:28]):
        raise SystemExit("region: the header's CRC does not cover its own bytes")
    payload = region[HEAD:HEAD + length]
    if len(payload) != length:
        raise SystemExit(f"region: header says {length} B of payload, file holds "
                         f"{len(payload)} -- cut short")
    if zlib.crc32(payload) != crc:
        raise SystemExit("region: the payload's CRC does not match -- damaged")
    return length, crc


def main(argv=None):
    ap = argparse.ArgumentParser(description="put a resource region into a flash dump")
    ap.add_argument("dump", help="a full flash dump (M-Flasher's backup, or any 1 MiB read)")
    ap.add_argument("region", help="build/NAME-res.bin (tools/build.py)")
    ap.add_argument("-o", "--out", required=True, help="the dump to write back")
    ap.add_argument("--pad", metavar="SIZE",
                    help="extend a short dump with 0xFF to SIZE (0x100000), for a dump "
                         "that is not the whole chip")
    ap.add_argument("--force", action="store_true",
                    help="the range holds something other than erased bytes or this "
                         "region: overwrite it (an older build's resources)")
    ap.add_argument("--dry-run", action="store_true", help="check and report, write nothing")
    a = ap.parse_args(argv)

    dump = bytearray(Path(a.dump).read_bytes())
    region = Path(a.region).read_bytes()
    length, crc = check_region(region)

    if a.pad:
        size = int(a.pad, 0)
        if len(dump) > size:
            raise SystemExit(f"dump is {len(dump)} B, larger than --pad {size:#x}")
        if len(dump) < APP_RES_BASE + len(region):
            dump += b"\xFF" * (size - len(dump))
    if len(dump) < APP_RES_BASE + len(region):
        raise SystemExit(f"dump is {len(dump)} B; the region ends at "
                         f"{APP_RES_BASE + len(region):#x}. Read the whole chip "
                         f"(--pad 0x100000) or this cannot hold it")

    lo, hi = APP_RES_BASE, APP_RES_BASE + APP_RES_SECTORS * 4096
    if not (lo <= APP_RES_BASE and APP_RES_BASE + len(region) <= hi):
        raise SystemExit(f"region does not fit [{lo:#x},{hi:#x})")
    for obj in STORE_OBJS:
        if APP_RES_BASE + len(region) > obj and APP_RES_BASE < obj + 2 * 4096:
            raise SystemExit(f"region would overlap the store at {obj:#x}")

    old = dump[APP_RES_BASE:APP_RES_BASE + len(region)]
    was_erased = all(b == 0xFF for b in old)
    if not was_erased and old != region and not a.force:
        o = struct.unpack_from("<3I", old)
        hint = (f"it looks like a resource header (RES1, {o[1]} B, crc {o[2]:#x})"
                if o[0] == MAGIC else "it is not a resource header")
        raise SystemExit(f"{APP_RES_BASE:#x} already holds something: {hint}. "
                         f"Use --force to replace it.")

    print(f"dump    {a.dump}: {len(dump)} B")
    print(f"region  {a.region}: {len(region)} B (payload {length} B, crc {crc:#010x})")
    print(f"write   {APP_RES_BASE:#07x}..{APP_RES_BASE + len(region) - 1:#07x} "
          f"({len(region)} B)")
    print(f"was     {'erased (0xFF)' if was_erased else 'another region' if old != region else 'this region'}")
    print(f"store   {', '.join(f'{o:#07x}' for o in STORE_OBJS)} untouched "
          f"(region ends {APP_RES_BASE + len(region):#07x})")
    if a.dry_run:
        print("dry run: nothing written")
        return 0

    dump[APP_RES_BASE:APP_RES_BASE + len(region)] = region
    out = Path(a.out)
    out.write_bytes(bytes(dump))
    print(f"out     {out}: {out.stat().st_size} B")
    print()
    print("Flash this with M-Flasher. After it boots, SOURCE -> SMPL shows the slots")
    print("instead of NO SAMPLES.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
