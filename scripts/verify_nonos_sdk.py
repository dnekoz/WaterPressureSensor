"""Verify bundled NON-OS SDK 3.0.5 in linked firmware, independently of macros.

Usage: python scripts/verify_nonos_sdk.py [--environment d1_mini]
Requires a completed PlatformIO build with firmware.elf, firmware.bin and its
linker map. All checks use the Python standard library.
"""
from pathlib import Path
import argparse
import hashlib
import re
import struct
import sys

SDK_RUNTIME = b"3.0.5(b29dcd3)"
LIBRARIES = ("airkiss", "crypto", "espnow", "main", "net80211", "phy", "pp",
             "smartconfig", "wpa", "wpa2", "wps")


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def elf_symbols(data):
    """Read named ELF32 little-endian symbols and their section-backed bytes."""
    require(data[:7] == b"\x7fELF\x01\x01\x01", "Expected an ELF32 little-endian firmware")
    require(len(data) >= 52, "Truncated ELF header")
    header = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    require(header[1] == 94, "ELF is not an Xtensa executable")
    section_offset, section_size, section_count = header[5], header[10], header[11]
    require(section_size == 40 and section_count > 0, "Invalid ELF section table")
    require(section_offset + section_size * section_count <= len(data), "Truncated ELF sections")
    sections = [struct.unpack_from("<IIIIIIIIII", data, section_offset + i * section_size)
                for i in range(section_count)]
    symbols = {}
    for section in sections:
        if section[1] != 2:  # SHT_SYMTAB
            continue
        offset, size, string_index, entry_size = section[4], section[5], section[6], section[9]
        require(entry_size == 16 and size % entry_size == 0, "Invalid ELF symbol table")
        require(offset + size <= len(data) and string_index < section_count, "Truncated ELF symbols")
        strings_section = sections[string_index]
        strings = data[strings_section[4]:strings_section[4] + strings_section[5]]
        require(len(strings) == strings_section[5], "Truncated ELF string table")
        for entry in range(offset, offset + size, entry_size):
            name_offset, value, length, info, _, index = struct.unpack_from("<IIIBBH", data, entry)
            if not name_offset or index == 0:
                continue
            require(name_offset < len(strings), "Invalid ELF symbol name")
            end = strings.find(b"\0", name_offset)
            require(end != -1, "Unterminated ELF symbol name")
            name = strings[name_offset:end].decode("ascii", errors="replace")
            contents = b""
            if index < section_count:
                owner = sections[index]
                position = owner[4] + value - owner[3]
                if owner[1] != 8 and value >= owner[3] and value + length <= owner[3] + owner[5]:
                    contents = data[position:position + length]
            symbols[name] = {"address": value, "type": info & 15, "data": contents}
    return symbols


def verify_map(text):
    normalized = text.replace("\\", "/").lower()
    require("/nonos-sdk-3.0.6/" not in normalized,
            "Linker map still references the cancelled SDK 3.0.6 overlay")
    expected_names = {f"lib{name}.a" for name in LIBRARIES}
    loaded = set()
    sdk_directories = set()
    for line in normalized.splitlines():
        # Linker-script wildcard rules (for example *libmain.a:(.text)) are
        # section selectors, not archive provenance entries.
        if line.lstrip().startswith("*"):
            continue
        names = re.findall(r"\b(lib[a-z0-9_]+\.a)\b", line)
        for name in expected_names.intersection(names):
            require(f"/nonosdk305/{name}" in line,
                    f"Unexpected link origin for {name}: {line.strip()}")
            if line.startswith("load "):
                loaded.add(name)
                sdk_directories.add(line[5:].strip().removesuffix(name))
    require(loaded == expected_names, "Missing SDK archive LOAD entries: " + ", ".join(sorted(expected_names - loaded)))
    require(len(sdk_directories) == 1, "SDK archives were linked from different directories")
    require("/nonosdk305/libmain.a(user_interface.o)" in normalized,
            "Linker map does not contain the SDK user_interface.o implementation")
    require(any("/nonosdk305/libmain.a(app_main.o)" in line and ".sdk.version" in line
                for line in normalized.splitlines()),
            "SDK version section is not supplied by bundled SDK 3.0.5 libmain.a")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--environment", default="d1_mini", help="PlatformIO build environment")
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    require(re.fullmatch(r"[A-Za-z0-9_.-]+", args.environment) and args.environment not in (".", ".."),
            "Invalid PlatformIO environment name")
    build = args.project / ".pio" / "build" / args.environment
    elf, binary = build / "firmware.elf", build / "firmware.bin"
    symbols = elf_symbols(elf.read_bytes())
    require("SDK_VERSION" in symbols, "ELF has no SDK_VERSION symbol")
    require(symbols["SDK_VERSION"]["data"] == SDK_RUNTIME + b"\0", "ELF SDK_VERSION is not the real bundled 3.0.5 runtime version")
    require(symbols.get("system_get_sdk_version", {}).get("type") == 2, "ELF lacks system_get_sdk_version implementation")
    require(SDK_RUNTIME + b"\0" in binary.read_bytes(), "Firmware BIN lacks the 3.0.5 runtime version")
    verify_map((build / "firmware.map").read_text(encoding="utf-8", errors="replace"))
    print(f"SDK runtime: {SDK_RUNTIME.decode()}")
    print("SDK source: Arduino Core bundled NONOSDK305; all linker origins verified")
    print(f"Firmware SHA256: {sha256(binary)}")
    print(f"PASS: {args.environment}; {len(LIBRARIES)} bundled SDK archives, matching ELF/BIN and linker origins")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, struct.error) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
