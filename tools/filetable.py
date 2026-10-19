"""Dump Mario Tennis' ROM file table (ROM 0x116C0: pairs of start/end offsets).

Usage: filetable.py ROM [--code]
  --code  only list compressed files that look like MIPS code
"""
import struct
import sys

from mtlz import decompress

TABLE = 0x116C0
TABLE_END = 0x156C0


def read_table(rom):
    files = []
    for off in range(TABLE, TABLE_END, 8):
        start, end = struct.unpack_from(">II", rom, off)
        if start == 0 and end == 0:
            break
        files.append((start, end))
    return files


def is_compressed(rom, start, end):
    size = int.from_bytes(rom[start + 1:start + 4], "big")
    if rom[start] != 0x01 or not 0 < size < 0x800000:
        return None
    try:
        data, _ = decompress(rom, start)
    except IndexError:
        return None
    return data if len(data) == size else None


def jr_ra_count(data):
    return sum(1 for i in range(0, len(data) - 3, 4)
               if struct.unpack_from(">I", data, i)[0] == 0x03E00008)


if __name__ == "__main__":
    rom = open(sys.argv[1], "rb").read()
    code_only = "--code" in sys.argv
    files = read_table(rom)
    print("%d files" % len(files))
    for i, (s, e) in enumerate(files):
        data = is_compressed(rom, s, e)
        jr = jr_ra_count(data) if data else 0
        if code_only and jr < 3:
            continue
        kind = "lz 0x%06X" % len(data) if data else "raw"
        print("%4d  %07X-%07X  %-13s %s" % (i, s, e, kind, "jr_ra %d" % jr if jr else ""))
