#!/usr/bin/env python3
"""Convert the host renderer's P6 snapshots to PNG without extra dependencies."""
import struct
import sys
import zlib
from pathlib import Path


def chunk(tag, data):
    return (struct.pack('>I', len(data)) + tag + data +
            struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))


for path in Path(sys.argv[1]).glob('*.ppm'):
    magic, dimensions, maximum, pixels = path.read_bytes().split(b'\n', 3)
    width, height = map(int, dimensions.split())
    assert magic == b'P6' and maximum == b'255'
    assert len(pixels) == width * height * 3
    stride = width * 3
    rows = b''.join(b'\0' + pixels[y*stride:(y+1)*stride] for y in range(height))
    path.with_suffix('.png').write_bytes(
        b'\x89PNG\r\n\x1a\n' +
        chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
        chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
