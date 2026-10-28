"""Independent little-endian protocol-v2 encoder; no platform or process APIs."""

import struct


def make_config(theme, mappings, target, log):
    data = bytearray(4752)
    if len(mappings) > 256:
        raise ValueError("Protocol v2 supports at most 256 mappings")
    identifier = theme.id if theme else "diagnostic"
    encoded_id = identifier.encode("ascii")
    if not 0 < len(encoded_id) < 64:
        raise ValueError("Invalid theme identifier length")
    struct.pack_into("<IIII", data, 0, len(data), 2, int(theme is not None), len(mappings) if theme else 0)
    data[16:16 + len(encoded_id)] = encoded_id
    if theme:
        struct.pack_into("<10I", data, 80, *theme.native_colors)
    for offset, capacity, path in [(120, 260, target), (640, 520, log)]:
        encoded = str(path).encode("utf-16le")
        if "\0" in str(path) or len(encoded) >= capacity * 2:
            raise ValueError("Invalid protocol path")
        data[offset:offset + len(encoded)] = encoded
    for index, mapping in enumerate(mappings if theme else []):
        struct.pack_into("<III", data, 1680 + index * 12, *mapping)
    return data
