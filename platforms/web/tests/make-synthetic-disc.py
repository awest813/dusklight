#!/usr/bin/env python3
"""Writes a small synthetic GameCube disc image (no game data) for testing the web build's disc path:

    platforms/web/tests/make-synthetic-disc.py out.iso

It has the Twilight Princess (USA) game ID and the GameCube magic, a boot header, an apploader
header, a minimal DOL and a file system table with a few placeholder files, so it passes the disc
picker's checks and opens through nod_iso and aurora's DVD layer. The game itself stops early when
the files it needs aren't there.
"""
import struct
import sys

FILES = {
    "opening.bnr": b"banner placeholder",
    "res/Placeholder/readme.txt": b"This synthetic image contains no game data.\n",
}


def build() -> bytes:
    image = bytearray(0x10000)
    image[0:6] = b"GZ2E01"
    struct.pack_into(">I", image, 0x1C, 0xC2339F3D)  # GameCube magic
    image[0x20:0x20 + 14] = b"Synthetic Disc"

    # Apploader header at 0x2440: 0x40 bytes of code, no trailer.
    struct.pack_into(">II", image, 0x2440 + 0x14, 0x40, 0)

    # DOL at 0x3000 with one 0x100-byte text section.
    dol = 0x3000
    struct.pack_into(">I", image, 0x420, dol)
    struct.pack_into(">I", image, dol + 0x00, 0x100)       # text0 offset
    struct.pack_into(">I", image, dol + 0x48, 0x80003100)  # text0 address
    struct.pack_into(">I", image, dol + 0x90, 0x100)       # text0 size
    struct.pack_into(">I", image, dol + 0xE0, 0x80003100)  # entry point

    # File system table: root, directories, files.
    entries = []
    names = bytearray()
    data_offset = 0x8000

    def name_offset(name: str) -> int:
        offset = len(names)
        names.extend(name.encode() + b"\0")
        return offset

    def add_dir(name, parent):
        entries.append([1, name_offset(name) if name else 0, parent, 0])
        return len(entries) - 1

    def add_file(name, payload):
        nonlocal data_offset
        entries.append([0, name_offset(name), data_offset, len(payload)])
        image[data_offset:data_offset + len(payload)] = payload
        data_offset += (len(payload) + 31) & ~31

    # Build the tree in FST order (a directory's children follow it; its "next" is the end).
    tree = {}
    for path, payload in FILES.items():
        node = tree
        parts = path.split("/")
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        node[parts[-1]] = payload

    def emit(node, parent):
        for name in sorted(node, key=str.lower):
            value = node[name]
            if isinstance(value, dict):
                index = add_dir(name, parent)
                emit(value, index)
                entries[index][3] = len(entries)
            else:
                add_file(name, value)

    root = add_dir("", 0)
    emit(tree, root)
    entries[root][3] = len(entries)

    fst = bytearray()
    for kind, name, offset, length in entries:
        fst += struct.pack(">II", (kind << 24) | name, offset) + struct.pack(">I", length)
    fst += names
    fst_offset = 0x4000
    image[fst_offset:fst_offset + len(fst)] = fst
    struct.pack_into(">III", image, 0x424, fst_offset, len(fst), len(fst))
    return bytes(image)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    with open(sys.argv[1], "wb") as out:
        out.write(build())
