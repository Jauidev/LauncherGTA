"""Section layout of a dumped XEX image (xexinfo's <out.pe>), so the scan tools work on any title.
The headers are the original little-endian PE's; sections as (virtual address, virtual size)."""
import struct

BASE = 0x82000000


def sections(pe):
    nt = struct.unpack_from("<I", pe, 0x3C)[0]
    count = struct.unpack_from("<H", pe, nt + 6)[0]
    table = nt + 24 + struct.unpack_from("<H", pe, nt + 20)[0]
    result = {}
    for i in range(count):
        entry = table + 40 * i
        name = pe[entry:entry + 8].rstrip(b"\0").decode("latin-1")
        size, address = struct.unpack_from("<II", pe, entry + 8)
        result[name] = (BASE + address, size)
    return result
