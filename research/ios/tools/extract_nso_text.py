#!/usr/bin/env python3
"""Extract and decompress the executable text segment from an NSO0 image."""
import argparse
from pathlib import Path
import struct


def decompress_lz4_block(data: bytes, expected_size: int) -> bytes:
    output = bytearray()
    cursor = 0
    while cursor < len(data):
        token = data[cursor]
        cursor += 1
        literal_length = token >> 4
        if literal_length == 15:
            while True:
                value = data[cursor]
                cursor += 1
                literal_length += value
                if value != 255:
                    break
        output.extend(data[cursor:cursor + literal_length])
        cursor += literal_length
        if cursor == len(data):
            break
        if cursor + 2 > len(data):
            raise ValueError("truncated LZ4 match offset")
        distance = int.from_bytes(data[cursor:cursor + 2], "little")
        cursor += 2
        if distance == 0 or distance > len(output):
            raise ValueError("invalid LZ4 match distance")
        match_length = token & 15
        if match_length == 15:
            while True:
                value = data[cursor]
                cursor += 1
                match_length += value
                if value != 255:
                    break
        match_length += 4
        for _ in range(match_length):
            output.append(output[-distance])
    if len(output) != expected_size:
        raise ValueError(f"expected {expected_size} bytes, decoded {len(output)}")
    return bytes(output)


def extract_text(source: Path) -> bytes:
    image = source.read_bytes()
    if image[:4] != b"NSO0" or len(image) < 0x6C:
        raise ValueError("not an NSO0 image")
    flags, file_offset, _, memory_size = struct.unpack_from("<IIII", image, 0x0C)
    compressed_size = struct.unpack_from("<I", image, 0x60)[0]
    stored_size = compressed_size if flags & 1 else memory_size
    payload = image[file_offset:file_offset + stored_size]
    if len(payload) != stored_size:
        raise ValueError("truncated NSO text segment")
    return decompress_lz4_block(payload, memory_size) if flags & 1 else payload


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_bytes(extract_text(args.source))
    print(f"wrote {args.output} ({args.output.stat().st_size} bytes)")
