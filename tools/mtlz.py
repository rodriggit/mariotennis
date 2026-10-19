"""Decompressor for Mario Tennis' boot LZ format.

Transcribed from the boot routine at 0x80300080 (ROM 0x1080).
Layout: 4-byte header (ignored by the boot code), then groups of
  flag byte == 0x00 -> 8 literal bytes follow
  flag byte != 0x00 -> bits MSB-first: 0 = literal byte, 1 = back-reference;
                       a sentinel bit after the flags ends the group.
Back-reference (2 or 3 bytes): b0 = HHHHLLLL, b1 = distance low byte
  distance = (H << 8) | b1, distance 0 ends the stream
  L == 0 -> length = b2 + 0x11 (3-byte form)
  L == 1 -> length = 2
  else   -> length = L + 1
"""
import sys

M = 0xFFFFFFFF


def decompress(src, off):
    """Return (data, end_offset) for the stream starting at src[off]."""
    out = bytearray()
    t0 = off + 4
    while True:
        # 0x803000D8: fetch flag byte
        flag = src[t0]
        t0 += 1
        if flag == 0:
            out += src[t0:t0 + 8]
            t0 += 8
            continue
        a3 = (flag << 24) | 0x00800000
        new_group = False
        while not new_group:
            # 0x80300108: literals until a set bit
            bit = a3 >> 31
            a3 = (a3 << 1) & M
            if not bit:
                for k in range(7):
                    out.append(src[t0])
                    t0 += 1
                    bit = a3 >> 31
                    a3 = (a3 << 1) & M  # 0x80300180 drops the tested bit
                    if bit or k == 6:
                        break
            # 0x80300184: no bits left -> the set bit was the sentinel
            if a3 == 0:
                new_group = True
                continue
            # 0x8030018C: back-reference
            b0, b1 = src[t0], src[t0 + 1]
            dist = b1 + ((b0 & 0xF0) << 4)
            if dist == 0:
                return bytes(out), t0
            n = b0 & 0x0F
            if n == 0:
                length = src[t0 + 2] + 0x11
                t0 += 3
            else:
                length = 2 if n == 1 else n + 1
                t0 += 2
            start = len(out) - dist
            for i in range(length):
                out.append(out[start + i])


if __name__ == "__main__":
    rom = open(sys.argv[1], "rb").read()
    off = int(sys.argv[2], 16)
    data, end = decompress(rom, off)
    print("in 0x%X..0x%X (0x%X bytes) -> out 0x%X bytes" % (off, end, end - off, len(data)))
    if len(sys.argv) > 3:
        open(sys.argv[3], "wb").write(data)
