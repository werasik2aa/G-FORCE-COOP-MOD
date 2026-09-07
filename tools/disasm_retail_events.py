"""Read-only x86 disassembly aid for observed G-Force event routes.

The retail executable has no symbols.  This script maps fixed virtual addresses
through its PE sections, prints a bounded Capstone disassembly, and dumps selected
vtables.  It never opens the EXE for writing or starts the game.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs


DEFAULT_EXE = Path(r"E:\G-Force\GForce.exe")
FUNCTIONS = (
    (0x0041E890, 0x180, "object relay"),
    (0x0044BF70, 0x500, "observed door proximity caller"),
    (0x0046D6F0, 0x100, "object forwarder"),
)
VTABLES = (
    (0x006F48CC, 19, "XTrigger_CO_Door"),
    (0x006F2FF4, 21, "XTrigger_MO_Blender"),
    (0x006F3104, 22, "XTrigger_MO_Mouse"),
)


class PeImage:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        if self.data[:2] != b"MZ":
            raise ValueError("not an MZ executable")
        pe_offset = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise ValueError("missing PE signature")
        coff_offset = pe_offset + 4
        section_count = struct.unpack_from("<H", self.data, coff_offset + 2)[0]
        optional_size = struct.unpack_from("<H", self.data, coff_offset + 16)[0]
        optional_offset = coff_offset + 20
        magic = struct.unpack_from("<H", self.data, optional_offset)[0]
        if magic != 0x10B:
            raise ValueError("expected PE32 image")
        self.image_base = struct.unpack_from("<I", self.data, optional_offset + 28)[0]
        section_offset = optional_offset + optional_size
        self.sections: list[tuple[str, int, int, int, int]] = []
        for index in range(section_count):
            offset = section_offset + index * 40
            name = self.data[offset:offset + 8].split(b"\0", 1)[0].decode("ascii", "replace")
            virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
                "<IIII", self.data, offset + 8)
            self.sections.append((name, virtual_address, virtual_size, raw_offset, raw_size))

    def file_offset(self, va: int) -> int:
        rva = va - self.image_base
        for _name, section_rva, virtual_size, raw_offset, raw_size in self.sections:
            end = section_rva + max(virtual_size, raw_size)
            if section_rva <= rva < end:
                offset = raw_offset + rva - section_rva
                if offset >= len(self.data):
                    break
                return offset
        raise ValueError("VA %08X is outside mapped sections" % va)

    def bytes_at(self, va: int, size: int) -> bytes:
        offset = self.file_offset(va)
        return self.data[offset:offset + size]

    def u32_at(self, va: int) -> int:
        return struct.unpack("<I", self.bytes_at(va, 4))[0]


def dump_function(image: PeImage, disassembler: Cs, va: int, size: int, label: str) -> None:
    print("[function] %s at %08X size=%X" % (label, va, size))
    for instruction in disassembler.disasm(image.bytes_at(va, size), va):
        print("  %08X  %-8s %s" % (instruction.address, instruction.mnemonic, instruction.op_str))


def dump_vtable(image: PeImage, va: int, count: int, label: str) -> None:
    print("[vtable] %s at %08X entries=%d" % (label, va, count))
    for slot in range(count):
        target = image.u32_at(va + slot * 4)
        print("  slot=%02d offset=%02X target=%08X" % (slot, slot * 4, target))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    args = parser.parse_args()
    image = PeImage(args.exe)
    print("[image] path=%s image_base=%08X" % (image.path, image.image_base))
    disassembler = Cs(CS_ARCH_X86, CS_MODE_32)
    for va, size, label in FUNCTIONS:
        dump_function(image, disassembler, va, size, label)
    for va, count, label in VTABLES:
        dump_vtable(image, va, count, label)


if __name__ == "__main__":
    main()
