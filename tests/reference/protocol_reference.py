"""Independent little-endian protocol-v3 encoder; no platform or process APIs."""

import struct


def make_config(theme, mappings, target, log, icon_mode=0, identifier=None):
    data = bytearray(4760)
    if len(mappings) > 256:
        raise ValueError("Protocol v3 supports at most 256 mappings")
    identifier = identifier or (theme.id if theme else "diagnostic")
    encoded_id = identifier.encode("ascii")
    if not 0 < len(encoded_id) < 64:
        raise ValueError("Invalid theme identifier length")
    struct.pack_into("<IIII", data, 0, len(data), 3, int(theme is not None), len(mappings) if theme else 0)
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
    struct.pack_into("<II", data, 4752, icon_mode, int(icon_mode != 0))
    return data
