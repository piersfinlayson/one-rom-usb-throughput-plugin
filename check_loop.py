#!/usr/bin/env python3
# Copyright (C) 2026 Piers Finlayson <piers@piers.rocks>
#
# MIT License

"""Fails if the plugin refers to a flash code address outside its entry code.

check_loop.py <elf>

plugin_main runs the streaming loop through the uncached XIP alias, and the
loop stays there only while every call it makes into flash is PC-relative.
Code reached from a flash code address runs through the XIP cache.

Reads the relocations the linker keeps with --emit-relocs.  An absolute
relocation is allowed in the plugin header and in the entry code, which runs
before the loop.  Elsewhere one whose target is a function in .text fails, and
so does any absolute MOVW or MOVT relocation, as its target isn't checked.
"""

import struct
import sys

ENTRY = {"plugin_main", "drive_main"}
SHT_SYMTAB = 2
SHT_REL = 9
STT_FUNC = 2
R_ARM_ABS32 = 2
R_ARM_THM_MOVW_ABS_NC = 47
R_ARM_THM_MOVT_ABS = 48


def main():
    data = open(sys.argv[1], "rb").read()
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    secs = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize)
            for i in range(shnum)]
    names_off = secs[shstrndx][4]

    def cstr(off):
        return data[off:data.index(b"\0", off)].decode()

    names = [cstr(names_off + s[0]) for s in secs]
    text = names.index(".text")

    def word_at(addr):
        for s in secs:
            if s[1] != 8 and s[3] <= addr < s[3] + s[5]:
                return struct.unpack_from("<I", data, s[4] + addr - s[3])[0]
        return None

    symtab = next(s for s in secs if s[1] == SHT_SYMTAB)
    strtab = secs[symtab[6]][4]
    funcs = []
    for off in range(symtab[4], symtab[4] + symtab[5], 16):
        name, value, size, info, _, shndx = struct.unpack_from("<IIIBBH", data, off)
        if info & 0xF == STT_FUNC and size:
            funcs.append((value & ~1, (value & ~1) + size, cstr(strtab + name), shndx))

    def func_at(addr):
        return next((f for f in funcs if f[0] <= addr < f[1]), None)

    bad = []
    for s in secs:
        if s[1] != SHT_REL or names[s[7]] == ".plugin_header":
            continue
        for off in range(s[4], s[4] + s[5], 8):
            where, info = struct.unpack_from("<II", data, off)
            rtype = info & 0xFF
            source = func_at(where)
            if source and source[2] in ENTRY:
                continue
            label = source[2] if source else f"{names[s[7]]}+{where:#x}"
            if rtype in (R_ARM_THM_MOVW_ABS_NC, R_ARM_THM_MOVT_ABS):
                bad.append(f"{label}: absolute MOVW/MOVT at {where:#x}")
            elif rtype == R_ARM_ABS32:
                target = func_at((word_at(where) or 0) & ~1)
                if target and target[3] == text:
                    bad.append(f"{label}: refers to {target[2]} at {where:#x}")
    for b in bad:
        print(f"check_loop: {b}", file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
