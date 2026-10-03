#!/usr/bin/env python3
"""
Build release artifacts for SHS-Z2M-Presence after `idf.py build`.

Produces in build/release/:
  SHS_Z2M_Presence_vX.Y.Z.ota         Zigbee OTA image for Zigbee2MQTT
  SHS_Z2M_Presence_vX.Y.Z_merged.bin  Full image for USB / ESPHome Web flashing
and updates ota/index.json (the Z2M OTA override index) to point at the new image.

Run with the ESP-IDF Python env active (esptool is needed for the merged bin):
  python tools/make_release.py [--notes "Release notes"]
"""

import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "main" / "shs01.h"
BUILD = ROOT / "build"
OUT = BUILD / "release"
INDEX = ROOT / "ota" / "index.json"

APP_BIN = BUILD / "SHS_Z2M_Presence.bin"
REPO_URL = "https://github.com/notownblues/SHS-Z2M-Presence"
MODEL_ID = "SHS-Z2M-Presence"
MANUFACTURER_NAME = "SmartHomeScene"

OTA_MAGIC = 0x0BEEF11E
OTA_HEADER_VERSION = 0x0100
OTA_HEADER_LEN = 56
OTA_STACK_VERSION = 0x0002          # Zigbee PRO
OTA_TAG_UPGRADE_IMAGE = 0x0000


def read_define(text, name):
    m = re.search(r"^#define\s+" + name + r"\s+(\S+)", text, re.MULTILINE)
    if not m:
        sys.exit(f"error: {name} not found in {HEADER}")
    return m.group(1).strip('"')


def read_versions():
    text = HEADER.read_text()
    sw = read_define(text, "SHS_BASIC_SW_BUILD_ID")
    m = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)", sw)
    if not m:
        sys.exit(f"error: SHS_BASIC_SW_BUILD_ID '{sw}' is not vX.Y.Z")
    major, minor, patch = (int(x) for x in m.groups())
    expected = (major << 24) | (minor << 16) | (patch << 8)
    file_version = int(read_define(text, "SHS_OTA_FILE_VERSION"), 0)
    if file_version != expected:
        sys.exit(f"error: SHS_OTA_FILE_VERSION 0x{file_version:08X} does not match {sw} "
                 f"(expected 0x{expected:08X})")
    return {
        "sw": sw,
        "file_version": file_version,
        "manufacturer_code": int(read_define(text, "SHS_OTA_MANUFACTURER_CODE"), 0),
        "image_type": int(read_define(text, "SHS_OTA_IMAGE_TYPE"), 0),
    }


def build_ota(v, app):
    header_string = f"{MODEL_ID} {v['sw']}".encode()[:32].ljust(32, b"\0")
    element = struct.pack("<HI", OTA_TAG_UPGRADE_IMAGE, len(app)) + app
    total = OTA_HEADER_LEN + len(element)
    header = struct.pack("<IHHHHHIH32sI", OTA_MAGIC, OTA_HEADER_VERSION, OTA_HEADER_LEN, 0,
                         v["manufacturer_code"], v["image_type"], v["file_version"],
                         OTA_STACK_VERSION, header_string, total)
    assert len(header) == OTA_HEADER_LEN
    return header + element, header_string.rstrip(b"\0").decode()


def build_merged(out_path):
    """Merge bootloader, partition table and app using build/flash_args.

    otadata is deliberately left out: merge_bin pads gaps with 0xFF, so including
    it (at 0x195000) would wipe zb_storage and lose the Zigbee pairing. A blank
    otadata boots ota_0, which is where the app is written.
    """
    lines = (BUILD / "flash_args").read_text().split("\n")
    flash_opts = lines[0].split()
    segments = []
    for line in lines[1:]:
        parts = line.split()
        if len(parts) == 2 and "ota_data" not in parts[1]:
            segments += [parts[0], str(BUILD / parts[1])]
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32c6", "merge_bin",
           "-o", str(out_path)] + flash_opts + segments
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def update_index(v, ota_name, ota_data, header_string, notes):
    entries = json.loads(INDEX.read_text()) if INDEX.exists() else []
    # Keep only entries for other devices; this device always gets just the latest image
    entries = [e for e in entries
               if not (e.get("imageType") == v["image_type"]
                       and e.get("manufacturerCode") == v["manufacturer_code"])]
    entry = {
        "fileName": ota_name,
        "url": f"{REPO_URL}/releases/download/{v['sw']}/{ota_name}",
        "fileVersion": v["file_version"],
        "fileSize": len(ota_data),
        "manufacturerCode": v["manufacturer_code"],
        "imageType": v["image_type"],
        "sha512": hashlib.sha512(ota_data).hexdigest(),
        "otaHeaderString": header_string,
        "modelId": MODEL_ID,
        "manufacturerName": [MANUFACTURER_NAME],
    }
    if notes:
        entry["releaseNotes"] = notes
    entries.append(entry)
    INDEX.parent.mkdir(exist_ok=True)
    INDEX.write_text(json.dumps(entries, indent=2) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--notes", help="release notes shown in the Z2M OTA tab")
    ap.add_argument("--no-index", action="store_true", help="don't update ota/index.json")
    args = ap.parse_args()

    if not APP_BIN.exists():
        sys.exit(f"error: {APP_BIN} not found - run idf.py build first")
    v = read_versions()
    app = APP_BIN.read_bytes()
    OUT.mkdir(parents=True, exist_ok=True)

    ota_name = f"SHS_Z2M_Presence_{v['sw']}.ota"
    ota_data, header_string = build_ota(v, app)
    (OUT / ota_name).write_bytes(ota_data)

    merged_name = f"SHS_Z2M_Presence_{v['sw']}_merged.bin"
    build_merged(OUT / merged_name)

    if not args.no_index:
        update_index(v, ota_name, ota_data, header_string, args.notes)

    print(f"Version:      {v['sw']} (file version 0x{v['file_version']:08X})")
    print(f"OTA image:    {OUT / ota_name} ({len(ota_data)} bytes)")
    print(f"Merged bin:   {OUT / merged_name}")
    print(f"Index:        {'unchanged' if args.no_index else INDEX.relative_to(ROOT)}")
    print(f"Next: attach both files to the GitHub release '{v['sw']}', then commit ota/index.json")


if __name__ == "__main__":
    main()
