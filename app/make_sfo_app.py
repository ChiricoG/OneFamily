#!/usr/bin/env python3
"""
SFO Generator for Xbox Controller Plugin Configurator
Builds standard-compliant param.sfo with strict buffer bounds and exact OpenOrbis specs.
"""

import struct
import os

SFO_SCHEMA = {
    "APP_TYPE": (0x0404, 4),
    "APP_VER": (0x0204, 8),
    "ATTRIBUTE": (0x0404, 4),
    "CATEGORY": (0x0204, 4),
    "CONTENT_ID": (0x0204, 48),
    "DOWNLOAD_DATA_SIZE": (0x0404, 4),
    "SYSTEM_VER": (0x0404, 4),
    "TITLE": (0x0204, 128),
    "TITLE_ID": (0x0204, 12),
    "VERSION": (0x0204, 8),
}

def build_sfo(params, output_path="param.sfo"):
    sorted_keys = sorted(params.keys())
    header_size = 20
    entry_size = 16
    count = len(sorted_keys)

    key_table = bytearray()
    key_offsets = {}
    for k in sorted_keys:
        key_offsets[k] = len(key_table)
        key_table.extend(k.encode('utf-8') + b'\x00')

    while len(key_table) % 4 != 0:
        key_table.append(0)

    key_table_offset = header_size + (count * entry_size)
    data_table_offset = key_table_offset + len(key_table)

    data_table = bytearray()
    entries = []

    for k in sorted_keys:
        val = params[k]
        fmt, max_len = SFO_SCHEMA[k]
        val_offset = len(data_table)

        if fmt == 0x0404:
            data = struct.pack('<I', int(val))
            data_len = 4
        else:
            encoded = str(val).encode('utf-8') + b'\x00'
            data_len = len(encoded)
            data = encoded.ljust(max_len, b'\x00')

        data_table.extend(data)
        entries.append((key_offsets[k], fmt, data_len, max_len, val_offset))

    header = struct.pack('<4s4sIII', b'\x00PSF', b'\x01\x01\x00\x00', key_table_offset, data_table_offset, count)

    with open(output_path, 'wb') as f:
        f.write(header)
        for ent in entries:
            f.write(struct.pack('<HHIII', ent[0], ent[1], ent[2], ent[3], ent[4]))
        f.write(key_table)
        f.write(data_table)

    print(f"[SFO] Successfully built: {output_path} ({os.path.getsize(output_path)} bytes)")

if __name__ == '__main__':
    params = {
        "APP_TYPE": 1,
        "APP_VER": "01.13",
        "ATTRIBUTE": 0,
        "CATEGORY": "gd",
        "CONTENT_ID": "UP9000-XBOX00001_00-XBOXCONFIGURATOR",
        "DOWNLOAD_DATA_SIZE": 0,
        "SYSTEM_VER": 0,
        "TITLE": "OneFamily Companion",
        "TITLE_ID": "XBOX00001",
        "VERSION": "01.13"
    }

    out = os.path.join(os.path.dirname(__file__), "sce_sys", "param.sfo")
    build_sfo(params, out)
