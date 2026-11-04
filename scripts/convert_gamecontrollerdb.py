#!/usr/bin/env python3
"""
convert_gamecontrollerdb.py - Convert SDL_GameControllerDB into OneFamily Generic HID INI Profiles

Downloads the latest gamecontrollerdb.txt from mdqinc/SDL_GameControllerDB, parses
third-party HID and DirectInput controllers, and converts them to OneFamily's
<VID>_<PID>.ini format.

Outputs:
  1. Individual .ini files in --out-dir (e.g. profiles/<VID>_<PID>.ini)
  2. C++ header --header-out (app/src/factory_profiles.h) for the Companion App installer.
"""

import argparse
import collections
import os
import sys
import urllib.request
import re

DEFAULT_DB_URL = "https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/master/gamecontrollerdb.txt"

# VIDs handled by native dedicated drivers in OneFamily
EXCLUDED_VIDS = {
    0x054C,  # Sony DualShock / DualSense
    0x045E,  # Microsoft Xbox (360, One, Series)
    0x057E,  # Nintendo Switch Pro / Joy-Con
    0x18D1,  # Google Stadia
}

SDL_BUTTON_TO_ONEFAMILY = {
    'a': 'cross',
    'b': 'circle',
    'x': 'square',
    'y': 'triangle',
    'leftshoulder': 'l1',
    'rightshoulder': 'r1',
    'lefttrigger': 'l2',
    'righttrigger': 'r2',
    'back': 'create',
    'start': 'options',
    'guide': 'ps',
    'leftstick': 'l3',
    'rightstick': 'r3',
    'touchpad': 'touchpad',
    'dpup': 'up',
    'dpdown': 'down',
    'dpleft': 'left',
    'dpright': 'right',
}

PRIORITY_BRANDS = [
    '8bitdo', 'logitech', 'mayflash', 'dragonrise', 'gamesir', 'shanwan',
    'hori', 'retro-bit', 'betop', 'mad catz', 'razer', 'steelseries',
    'speedlink', 'thrustmaster', 'qanba', 'powera', 'pdp', 'afterglow',
    'flydigi', 'ipega', 'hyperkin', 'nacon', 'elecom', 'sanwa', 'saitek',
    'interact', 'raphnet', 'machenike', 'gulikit', 'dinput', 'retropad'
]

# Baseline factory profiles to always preserve
PRESERVED_PROFILES = {
    "default.ini": (
        "# OneFamily Generic Controller Profile - Default Template\n"
        "[Info]\n"
        "name = Generic Controller (Default)\n"
        "\n"
        "[Buttons]\n"
        "button1 = cross\n"
        "button2 = circle\n"
        "button3 = square\n"
        "button4 = triangle\n"
        "button5 = l1\n"
        "button6 = r1\n"
        "button7 = l2\n"
        "button8 = r2\n"
        "button9 = create\n"
        "button10 = options\n"
        "button11 = l3\n"
        "button12 = r3\n"
        "button13 = none\n"
        "button14 = none\n"
        "\n"
        "[Axes]\n"
        "lx = x\n"
        "ly = y\n"
        "rx = z\n"
        "ry = rz\n"
        "l2 = rx\n"
        "r2 = ry\n"
        "\n"
        "[Invert]\n"
        "invert_ly = 0\n"
        "invert_ry = 0\n"
        "\n"
        "[Rumble]\n"
        "# Presets: auto, logitech, dragonrise, greenasia, betop, shanwan, 8bitdo, dinput05, none (or hex e.g. 05 L R 00 00)\n"
        "rumble = auto\n"
    ),
    "2563_0119.ini": (
        "# OneFamily Generic Controller Profile\n"
        "[Info]\n"
        "name = ShanWan 2.4G Wireless Dongle / BSP-G6\n"
        "vendor_id = 0x2563\n"
        "product_id = 0x0119\n"
        "\n"
        "[Buttons]\n"
        "button1 = square\n"
        "button2 = cross\n"
        "button3 = circle\n"
        "button4 = triangle\n"
        "button5 = l1\n"
        "button6 = r1\n"
        "button7 = l2\n"
        "button8 = r2\n"
        "button9 = create\n"
        "button10 = options\n"
        "button11 = l3\n"
        "button12 = r3\n"
        "button13 = none\n"
        "button14 = none\n"
        "\n"
        "[Axes]\n"
        "lx = x\n"
        "ly = y\n"
        "rx = z\n"
        "ry = rz\n"
        "\n"
        "[Invert]\n"
        "invert_ly = 0\n"
        "invert_ry = 0\n"
        "\n"
        "[Rumble]\n"
        "rumble = none\n"
    ),
    "2DC8_310B.ini": (
        "# OneFamily Generic Controller Profile\n"
        "[Info]\n"
        "name = 8BitDo Ultimate 2 Wireless\n"
        "vendor_id = 0x2DC8\n"
        "product_id = 0x310B\n"
        "\n"
        "[Buttons]\n"
        "button1 = cross\n"
        "button2 = circle\n"
        "button3 = none\n"
        "button4 = square\n"
        "button5 = triangle\n"
        "button6 = none\n"
        "button7 = l1\n"
        "button8 = r1\n"
        "button9 = l2\n"
        "button10 = r2\n"
        "button11 = create\n"
        "button12 = options\n"
        "button13 = ps\n"
        "button14 = l3\n"
        "button15 = r3\n"
        "\n"
        "[Axes]\n"
        "lx = x\n"
        "ly = y\n"
        "rx = z\n"
        "ry = rz\n"
        "l2 = rx\n"
        "r2 = ry\n"
        "\n"
        "[Invert]\n"
        "invert_ly = 0\n"
        "invert_ry = 0\n"
        "\n"
        "[Rumble]\n"
        "rumble = auto\n"
    ),
    "2DC8_3012.ini": (
        "# OneFamily Generic Controller Profile\n"
        "[Info]\n"
        "name = 8BitDo Ultimate 2.4G\n"
        "vendor_id = 0x2DC8\n"
        "product_id = 0x3012\n"
        "\n"
        "[Buttons]\n"
        "button1 = cross\n"
        "button2 = circle\n"
        "button3 = none\n"
        "button4 = square\n"
        "button5 = triangle\n"
        "button6 = none\n"
        "button7 = l1\n"
        "button8 = r1\n"
        "button9 = l2\n"
        "button10 = r2\n"
        "button11 = create\n"
        "button12 = options\n"
        "button13 = ps\n"
        "button14 = l3\n"
        "button15 = r3\n"
        "\n"
        "[Axes]\n"
        "lx = x\n"
        "ly = y\n"
        "rx = z\n"
        "ry = rz\n"
        "l2 = rx\n"
        "r2 = ry\n"
        "\n"
        "[Invert]\n"
        "invert_ly = 0\n"
        "invert_ry = 0\n"
        "\n"
        "[Rumble]\n"
        "rumble = auto\n"
    ),
    "2DC8_6012.ini": (
        "# OneFamily Generic Controller Profile\n"
        "[Info]\n"
        "name = 8BitDo Ultimate 2C Wireless\n"
        "vendor_id = 0x2DC8\n"
        "product_id = 0x6012\n"
        "\n"
        "[Buttons]\n"
        "button1 = cross\n"
        "button2 = circle\n"
        "button3 = none\n"
        "button4 = square\n"
        "button5 = triangle\n"
        "button6 = none\n"
        "button7 = l1\n"
        "button8 = r1\n"
        "button9 = l2\n"
        "button10 = r2\n"
        "button11 = create\n"
        "button12 = options\n"
        "button13 = ps\n"
        "button14 = l3\n"
        "button15 = r3\n"
        "\n"
        "[Axes]\n"
        "lx = x\n"
        "ly = y\n"
        "rx = z\n"
        "ry = rz\n"
        "l2 = rx\n"
        "r2 = ry\n"
        "\n"
        "[Invert]\n"
        "invert_ly = 0\n"
        "invert_ry = 0\n"
        "\n"
        "[Rumble]\n"
        "rumble = auto\n"
    ),
    "2DC8_2100.ini": (
        "# OneFamily Generic Controller Profile\n"
        "[Info]\n"
        "name = 8BitDo SN30 Pro\n"
        "vendor_id = 0x2DC8\n"
        "product_id = 0x2100\n"
        "\n"
        "[Buttons]\n"
        "button1 = cross\n"
        "button2 = circle\n"
        "button3 = none\n"
        "button4 = square\n"
        "button5 = triangle\n"
        "button6 = none\n"
        "button7 = l1\n"
        "button8 = r1\n"
        "button9 = l2\n"
        "button10 = r2\n"
        "button11 = create\n"
        "button12 = options\n"
        "button13 = ps\n"
        "button14 = l3\n"
        "button15 = r3\n"
        "\n"
        "[Axes]\n"
        "lx = x\n"
        "ly = y\n"
        "rx = z\n"
        "ry = rz\n"
        "l2 = rx\n"
        "r2 = ry\n"
        "\n"
        "[Invert]\n"
        "invert_ly = 0\n"
        "invert_ry = 0\n"
        "\n"
        "[Rumble]\n"
        "rumble = auto\n"
    ),
    "3537_1022.ini": (
        "# OneFamily Generic Controller Profile\n"
        "[Info]\n"
        "name = GameSir G7\n"
        "vendor_id = 0x3537\n"
        "product_id = 0x1022\n"
        "\n"
        "[Buttons]\n"
        "button1 = cross\n"
        "button2 = circle\n"
        "button3 = none\n"
        "button4 = square\n"
        "button5 = triangle\n"
        "button6 = none\n"
        "button7 = l1\n"
        "button8 = r1\n"
        "button9 = l2\n"
        "button10 = r2\n"
        "button11 = create\n"
        "button12 = options\n"
        "button13 = ps\n"
        "button14 = l3\n"
        "button15 = r3\n"
        "\n"
        "[Axes]\n"
        "lx = x\n"
        "ly = y\n"
        "rx = z\n"
        "ry = rz\n"
        "l2 = rx\n"
        "r2 = ry\n"
        "\n"
        "[Invert]\n"
        "invert_ly = 0\n"
        "invert_ry = 0\n"
        "\n"
        "[Rumble]\n"
        "rumble = auto\n"
    ),
    "FFFF_046E.ini": (
        "# OneFamily Generic Controller Profile\n"
        "[Info]\n"
        "name = GameSir G3s\n"
        "vendor_id = 0xFFFF\n"
        "product_id = 0x046E\n"
        "\n"
        "[Buttons]\n"
        "button1 = cross\n"
        "button2 = circle\n"
        "button3 = none\n"
        "button4 = square\n"
        "button5 = triangle\n"
        "button6 = none\n"
        "button7 = l1\n"
        "button8 = r1\n"
        "button9 = l2\n"
        "button10 = r2\n"
        "button11 = create\n"
        "button12 = options\n"
        "button13 = ps\n"
        "button14 = l3\n"
        "button15 = r3\n"
        "\n"
        "[Axes]\n"
        "lx = x\n"
        "ly = y\n"
        "rx = z\n"
        "ry = rz\n"
        "l2 = rx\n"
        "r2 = ry\n"
        "\n"
        "[Invert]\n"
        "invert_ly = 0\n"
        "invert_ry = 0\n"
        "\n"
        "[Rumble]\n"
        "rumble = auto\n"
    ),
}

def clean_controller_name(raw_name):
    # Remove quotes, excessive spaces, and trailing symbols
    name = raw_name.replace('"', '').strip()
    return name if name else "Generic Controller"

def extract_vid_pid_from_guid(guid):
    if len(guid) != 32:
        return None, None
    try:
        b = bytes.fromhex(guid)
        # SDL GUID format: bytes 4-5 are VID (LE), bytes 8-9 are PID (LE)
        vid = b[4] | (b[5] << 8)
        pid = b[8] | (b[9] << 8)
        if vid == 0 and pid == 0:
            return None, None
        return vid, pid
    except ValueError:
        return None, None

def parse_sdl_mapping_line(line):
    line = line.strip()
    if not line or line.startswith('#'):
        return None
    parts = line.split(',')
    if len(parts) < 3:
        return None
    guid = parts[0].strip()
    name = clean_controller_name(parts[1])
    vid, pid = extract_vid_pid_from_guid(guid)
    if vid is None or pid is None:
        return None
    if vid in EXCLUDED_VIDS:
        return None

    platform = "unknown"
    mappings = {}
    for token in parts[2:]:
        token = token.strip()
        if not token:
            continue
        if ':' in token:
            k, v = token.split(':', 1)
            k = k.strip().lower()
            v = v.strip().lower()
            if k == 'platform':
                platform = v
            else:
                mappings[k] = v

    return {
        'guid': guid,
        'vid': vid,
        'pid': pid,
        'name': name,
        'platform': platform,
        'mappings': mappings
    }

def convert_to_ini(entry):
    vid = entry['vid']
    pid = entry['pid']
    name = entry['name']
    mappings = entry['mappings']

    btn_alloc = {}
    for sdl_key, sdl_val in mappings.items():
        if sdl_val.startswith('b'):
            try:
                b_idx = int(sdl_val[1:]) + 1
                if sdl_key in SDL_BUTTON_TO_ONEFAMILY and 1 <= b_idx <= 16:
                    btn_alloc[b_idx] = SDL_BUTTON_TO_ONEFAMILY[sdl_key]
            except ValueError:
                pass

    lines = []
    lines.append("# OneFamily Generic Controller Profile")
    lines.append("[Info]")
    lines.append(f"name = {name}")
    lines.append(f"vendor_id = 0x{vid:04X}")
    lines.append(f"product_id = 0x{pid:04X}")
    lines.append("")
    lines.append("[Buttons]")

    max_btn = max(btn_alloc.keys()) if btn_alloc else 12
    max_btn = max(max_btn, 12)
    for i in range(1, max_btn + 1):
        target = btn_alloc.get(i, 'none')
        lines.append(f"button{i} = {target}")

    lines.append("")
    lines.append("[Axes]")
    lines.append("lx = x")
    lines.append("ly = y")
    lines.append("rx = z")
    lines.append("ry = rz")

    lines.append("")
    lines.append("[Invert]")
    inv_lx = 1 if ('leftx' in mappings and '~' in mappings['leftx']) else 0
    inv_ly = 1 if ('lefty' in mappings and '~' in mappings['lefty']) else 0
    inv_rx = 1 if ('rightx' in mappings and '~' in mappings['rightx']) else 0
    inv_ry = 1 if ('righty' in mappings and '~' in mappings['righty']) else 0
    if inv_lx:
        lines.append(f"invert_lx = {inv_lx}")
    lines.append(f"invert_ly = {inv_ly}")
    if inv_rx:
        lines.append(f"invert_rx = {inv_rx}")
    lines.append(f"invert_ry = {inv_ry}")
    lines.append("")
    lines.append("[Rumble]")
    name_lower = name.lower()
    if 'shanwan' in name_lower:
        lines.append("rumble = shanwan")
    elif 'dragonrise' in name_lower:
        lines.append("rumble = dragonrise")
    elif 'logitech' in name_lower:
        lines.append("rumble = logitech")
    elif '8bitdo' in name_lower:
        lines.append("rumble = 8bitdo")
    elif 'betop' in name_lower:
        lines.append("rumble = betop")
    else:
        lines.append("rumble = auto")

    lines.append("")
    return '\n'.join(lines)

def score_entry(entry):
    score = 0
    name_lower = entry['name'].lower()
    for idx, brand in enumerate(PRIORITY_BRANDS):
        if brand in name_lower:
            score += (len(PRIORITY_BRANDS) - idx) * 10
            break
    # Platform preference
    if entry['platform'] == 'linux':
        score += 5
    elif entry['platform'] == 'windows':
        score += 4
    # Number of mapped buttons
    score += len(entry['mappings'])
    return score

def select_balanced_profiles(entries_by_vid_pid, top_count):
    # Group by detected brand
    brand_buckets = collections.defaultdict(list)
    for (vid, pid), (sc, entry) in entries_by_vid_pid.items():
        fname = f"{vid:04X}_{pid:04X}.ini"
        if fname in PRESERVED_PROFILES:
            continue
        name_lower = entry['name'].lower()
        matched_brand = "other"
        for b in PRIORITY_BRANDS:
            if b in name_lower:
                matched_brand = b
                break
        brand_buckets[matched_brand].append(((vid, pid), sc, entry))

    # Sort each bucket by quality score descending
    for b in brand_buckets:
        brand_buckets[b].sort(key=lambda item: item[1], reverse=True)

    selected = []
    # Always include baseline preserved profiles first
    for fname, text in PRESERVED_PROFILES.items():
        selected.append((fname, text))

    # Round-robin pick from priority brands
    picked_keys = set()
    while len(selected) < top_count and any(brand_buckets[b] for b in PRIORITY_BRANDS):
        progress = False
        for b in PRIORITY_BRANDS:
            if brand_buckets[b]:
                (vid, pid), sc, entry = brand_buckets[b].pop(0)
                fname = f"{vid:04X}_{pid:04X}.ini"
                if fname not in picked_keys:
                    picked_keys.add(fname)
                    selected.append((fname, convert_to_ini(entry)))
                    progress = True
                    if len(selected) >= top_count:
                        break
        if not progress:
            break

    # If still below top_count, pick from 'other'
    while len(selected) < top_count and brand_buckets['other']:
        (vid, pid), sc, entry = brand_buckets['other'].pop(0)
        fname = f"{vid:04X}_{pid:04X}.ini"
        if fname not in picked_keys:
            picked_keys.add(fname)
            selected.append((fname, convert_to_ini(entry)))

    return selected

def escape_c_string(text):
    out = []
    for line in text.splitlines():
        escaped = line.replace('\\', '\\\\').replace('"', '\\"')
        out.append(f'        "{escaped}\\n"')
    return '\n'.join(out)

def main():
    parser = argparse.ArgumentParser(description="Convert SDL_GameControllerDB to OneFamily INI profiles.")
    parser.add_argument("--url", default=DEFAULT_DB_URL, help="URL to download gamecontrollerdb.txt")
    parser.add_argument("--input", default=None, help="Local path to gamecontrollerdb.txt")
    parser.add_argument("--out-dir", default="profiles", help="Output directory for .ini files")
    parser.add_argument("--header-out", default="app/src/factory_profiles.h", help="Path for C++ factory_profiles.h")
    parser.add_argument("--top-count", type=int, default=160, help="Number of priority profiles to bundle in header")
    args = parser.parse_args()

    content = ""
    if args.input and os.path.exists(args.input):
        print(f"Reading local database: {args.input}")
        with open(args.input, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
    else:
        print(f"Downloading database from: {args.url}")
        req = urllib.request.Request(args.url, headers={'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)'})
        try:
            with urllib.request.urlopen(req) as resp:
                content = resp.read().decode('utf-8', errors='ignore')
            print(f"Successfully downloaded {len(content)} bytes.")
        except Exception as e:
            print(f"Error downloading gamecontrollerdb.txt: {e}", file=sys.stderr)
            sys.exit(1)

    lines = content.splitlines()
    entries_by_vid_pid = {}

    for line in lines:
        parsed = parse_sdl_mapping_line(line)
        if not parsed:
            continue
        key = (parsed['vid'], parsed['pid'])
        sc = score_entry(parsed)
        if key not in entries_by_vid_pid or sc > entries_by_vid_pid[key][0]:
            entries_by_vid_pid[key] = (sc, parsed)

    print(f"Extracted {len(entries_by_vid_pid)} unique third-party controllers.")

    # Write all individual .ini files
    os.makedirs(args.out_dir, exist_ok=True)
    all_profiles = {}
    for (vid, pid), (sc, entry) in entries_by_vid_pid.items():
        fname = f"{vid:04X}_{pid:04X}.ini"
        if fname in PRESERVED_PROFILES:
            ini_data = PRESERVED_PROFILES[fname]
        else:
            ini_data = convert_to_ini(entry)
        all_profiles[fname] = ini_data
        with open(os.path.join(args.out_dir, fname), 'w', encoding='utf-8') as f:
            f.write(ini_data)

    print(f"Saved {len(all_profiles)} .ini files to '{args.out_dir}/'.")

    # Select balanced curated profiles for the C++ header
    selected_profiles = select_balanced_profiles(entries_by_vid_pid, args.top_count)
    print(f"Selected {len(selected_profiles)} factory profiles for {args.header_out}.")

    # Generate C++ header
    header_dir = os.path.dirname(args.header_out)
    if header_dir:
        os.makedirs(header_dir, exist_ok=True)

    with open(args.header_out, 'w', encoding='utf-8') as hf:
        hf.write("// Auto-generated by scripts/convert_gamecontrollerdb.py - DO NOT EDIT MANUALLY\n")
        hf.write("#ifndef FACTORY_PROFILES_H\n")
        hf.write("#define FACTORY_PROFILES_H\n\n")
        hf.write("struct FactoryProfileEntry {\n")
        hf.write("    const char* filename;\n")
        hf.write("    const char* content;\n")
        hf.write("};\n\n")
        hf.write(f"static const FactoryProfileEntry k_factory_profiles[{len(selected_profiles)}] = {{\n")

        for idx, (fname, text) in enumerate(selected_profiles):
            hf.write("    {\n")
            hf.write(f'        "{fname}",\n')
            hf.write(escape_c_string(text))
            hf.write("\n    }")
            if idx < len(selected_profiles) - 1:
                hf.write(",\n")
            else:
                hf.write("\n")

        hf.write("};\n\n")
        hf.write("#endif // FACTORY_PROFILES_H\n")

    print(f"Successfully generated '{args.header_out}'.")

if __name__ == "__main__":
    main()
