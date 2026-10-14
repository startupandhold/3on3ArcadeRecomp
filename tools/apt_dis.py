#!/usr/bin/env python3
"""Disassembles the ActionScript in an EA Apt movie (big-endian Xbox 360
"Apt Data:1:7:4" files, with their "Apt constant file").

Usage: apt_dis.py APT_FILE CONST_FILE [--strings]

Prints every action block (frame actions, init actions, clip events) with
function bodies inline. Pointers in the .apt are offsets from its start;
constant indices refer to the constant file's item table.
"""
import argparse
import struct
import sys

SIGNATURE = 0x09876543

CHARACTER_TYPES = {1: "shape", 2: "text", 3: "font", 4: "button", 5: "sprite", 6: "sound",
                   7: "image", 8: "morph", 9: "movie", 10: "static text", 11: "none",
                   12: "video"}

# Opcodes without operands.
SIMPLE = {
    0x00: "end", 0x04: "nextFrame", 0x05: "prevFrame", 0x06: "play", 0x07: "stop",
    0x08: "toggleQuality", 0x09: "stopSounds", 0x0A: "add", 0x0B: "subtract",
    0x0C: "multiply", 0x0D: "divide", 0x0E: "equals", 0x0F: "less", 0x10: "and",
    0x11: "or", 0x12: "not", 0x13: "stringEquals", 0x14: "stringLength", 0x15: "subString",
    0x17: "pop", 0x18: "toInteger", 0x1C: "getVariable", 0x1D: "setVariable",
    0x20: "setTarget2", 0x21: "stringAdd", 0x22: "getProperty", 0x23: "setProperty",
    0x24: "cloneSprite", 0x25: "removeSprite", 0x26: "trace", 0x27: "startDrag",
    0x28: "endDrag", 0x29: "stringLess", 0x2A: "throw", 0x2B: "castOp",
    0x2C: "implementsOp", 0x30: "random", 0x31: "mbStringLength", 0x32: "charToAscii",
    0x33: "asciiToChar", 0x34: "getTime", 0x35: "mbSubString", 0x36: "mbCharToAscii",
    0x37: "mbAsciiToChar", 0x3A: "delete", 0x3B: "delete2", 0x3C: "defineLocal",
    0x3D: "callFunction", 0x3E: "return", 0x3F: "modulo", 0x40: "newObject",
    0x41: "defineLocal2", 0x42: "initArray", 0x43: "initObject", 0x44: "typeOf",
    0x45: "targetPath", 0x46: "enumerate", 0x47: "add2", 0x48: "less2", 0x49: "equals2",
    0x4A: "toNumber", 0x4B: "toString", 0x4C: "pushDuplicate", 0x4D: "stackSwap",
    0x4E: "getMember", 0x4F: "setMember", 0x50: "increment", 0x51: "decrement",
    0x52: "callMethod", 0x53: "newMethod", 0x54: "instanceOf", 0x55: "enumerate2",
    0x56: "pushThis", 0x57: "pushGlobal", 0x58: "zeroVar", 0x59: "pushZero",
    0x5A: "pushOne", 0x5B: "callFunctionPop", 0x5C: "callFunctionKeep",
    0x5D: "callMethodPop", 0x5E: "callMethodKeep",
    0x60: "bitAnd", 0x61: "bitOr", 0x62: "bitXor", 0x63: "shiftLeft", 0x64: "shiftRight",
    0x65: "shiftRight2", 0x66: "strictEquals", 0x67: "greater", 0x68: "stringGreater",
    0x69: "extends", 0x70: "pushThisVar", 0x71: "pushGlobalVar", 0x72: "zeroVar2",
    0x73: "pushTrue", 0x74: "pushFalse", 0x75: "pushNull", 0x76: "pushUndefined",
}

BYTE_CONST = {0xA2: "pushConst", 0xAE: "pushValueOfVar", 0xAF: "getNamedMember",
              0xB0: "callNamedFuncPop", 0xB1: "callNamedFuncSetVar", 0xB2: "callNamedMethodPop",
              0xB3: "callNamedMethodSetVar"}


class Apt:
    def __init__(self, apt, const):
        self.d = apt
        self.c = const
        if not apt.startswith(b"Apt Data:"):
            raise ValueError("not an Apt data file")
        if not const.startswith(b"Apt constant file"):
            raise ValueError("not an Apt constant file")
        self.movie_offset, count, items = struct.unpack(">III", const[20:32])
        self.consts = []
        for i in range(count):
            kind, value = struct.unpack(">II", const[items + i * 8:items + i * 8 + 8])
            if kind == 1:
                self.consts.append(repr(self.cstr(const, value)))
            elif kind == 4:
                self.consts.append(f"reg{value}")
            elif kind == 2:
                self.consts.append(str(struct.unpack(">f", struct.pack(">I", value))[0]))
            elif kind == 5:
                self.consts.append("true" if value else "false")
            else:
                self.consts.append(f"<const {kind}:{value:#x}>")
        self.seen = set()

    @staticmethod
    def cstr(buf, off):
        end = buf.index(b"\0", off)
        return buf[off:end].decode("latin1")

    def u32(self, off):
        return struct.unpack(">I", self.d[off:off + 4])[0]

    def i32(self, off):
        return struct.unpack(">i", self.d[off:off + 4])[0]

    def s(self, off):
        return repr(self.cstr(self.d, off)) if off else "null"

    def const(self, i):
        return self.consts[i] if i < len(self.consts) else f"<const #{i}>"

    def disasm(self, off, end=None, indent="  ", pool=None, out=None):
        pool = pool or []
        d = self.d
        while end is None or off < end:
            op = d[off]
            at = off
            off += 1
            text = None
            if op in SIMPLE:
                text = SIMPLE[op]
            elif op in BYTE_CONST:
                i = d[off]
                off += 1
                name = pool[i] if i < len(pool) else f"pool#{i}"
                text = f"{BYTE_CONST[op]} {name}"
            elif op == 0xA3:
                i = struct.unpack(">H", d[off:off + 2])[0]
                off += 2
                text = f"pushConst {pool[i] if i < len(pool) else f'pool#{i}'}"
            elif op == 0xB5:
                text = f"pushByte {struct.unpack('>b', d[off:off + 1])[0]}"
                off += 1
            elif op == 0xB6:
                text = f"pushShort {struct.unpack('>h', d[off:off + 2])[0]}"
                off += 2
            elif op == 0xB9:
                text = f"pushRegister {d[off]}"
                off += 1
            elif op == 0xB7:
                text = f"pushLong {struct.unpack('>i', d[off:off + 4])[0]}"
                off += 4
            else:
                off = (off + 3) & ~3
                if op == 0x88:
                    n, p = self.u32(off), self.u32(off + 4)
                    off += 8
                    pool = [self.const(self.u32(p + 4 * k)) for k in range(n)]
                    text = f"constantPool [{len(pool)}]"
                elif op == 0x96:
                    n, p = self.u32(off), self.u32(off + 4)
                    off += 8
                    text = "push " + ", ".join(self.const(self.u32(p + 4 * k)) for k in range(n))
                elif op in (0x99, 0x9D, 0xB8):
                    rel = self.i32(off)
                    off += 4
                    name = {0x99: "jump", 0x9D: "branchIfTrue", 0xB8: "branchIfFalse"}[op]
                    text = f"{name} {off + rel:#x}"
                elif op == 0x81:
                    text = f"gotoFrame {self.u32(off)}"
                    off += 4
                elif op == 0x87:
                    text = f"storeRegister {self.u32(off)}"
                    off += 4
                elif op in (0x8B, 0x8C, 0xA1, 0xA4, 0xA5, 0xA6, 0xA7):
                    name = {0x8B: "setTarget", 0x8C: "gotoLabel", 0xA1: "pushString",
                            0xA4: "getStringVar", 0xA5: "getStringMember",
                            0xA6: "setStringVar", 0xA7: "setStringMember"}[op]
                    text = f"{name} {self.s(self.u32(off))}"
                    off += 4
                elif op == 0x83:
                    text = f"getURL {self.s(self.u32(off))} {self.s(self.u32(off + 4))}"
                    off += 8
                elif op == 0xB4:
                    text = f"pushFloat {struct.unpack('>f', d[off:off + 4])[0]}"
                    off += 4
                elif op == 0xB7:
                    text = f"pushLong {self.i32(off)}"
                    off += 4
                elif op == 0x9A:
                    text = f"getURL2 {d[off]}"
                    off += 4
                elif op == 0x9F:
                    text = f"gotoFrame2 {d[off]}"
                    off += 4
                elif op == 0x94:
                    text = f"with size {self.u32(off)}"
                    off += 4
                elif op == 0x8E:
                    name, nparams = self.u32(off), self.u32(off + 4)
                    regs, flags = d[off + 8], int.from_bytes(d[off + 9:off + 12], "big")
                    params, size = self.u32(off + 12), self.u32(off + 16)
                    off += 28
                    args = []
                    for k in range(nparams):
                        reg, pname = self.u32(params + 8 * k), self.u32(params + 8 * k + 4)
                        args.append(f"{self.cstr(d, pname)}@r{reg}" if reg else self.cstr(d, pname))
                    print(f"{indent}{at:06x}: function2 {self.s(name)}({', '.join(args)}) "
                          f"regs {regs} flags {flags:#x} size {size}", file=out)
                    self.disasm(off, off + size, indent + "    ", pool, out)
                    off += size
                    continue
                elif op == 0x9B:
                    name, nparams, params, size = (self.u32(off), self.u32(off + 4),
                                                   self.u32(off + 8), self.u32(off + 12))
                    off += 24
                    args = [self.cstr(d, self.u32(params + 4 * k)) for k in range(nparams)]
                    print(f"{indent}{at:06x}: function {self.s(name)}({', '.join(args)}) "
                          f"size {size}", file=out)
                    self.disasm(off, off + size, indent + "    ", pool, out)
                    off += size
                    continue
                elif op == 0x8F:
                    text = f"try <{d[off:off + 16].hex()}>"
                    off += 16
                else:
                    text = f"<op {op:#04x}>"
            print(f"{indent}{at:06x}: {text}", file=out)
            if op == 0x00 and end is None:
                return

    def frames(self, count, ptr, label, out):
        for f in range(count):
            items_count, items_ptr = self.u32(ptr + 8 * f), self.u32(ptr + 8 * f + 4)
            for k in range(items_count):
                item = self.u32(items_ptr + 4 * k)
                kind = self.u32(item)
                if kind == 1:
                    code = self.u32(item + 4)
                    print(f"\n== {label} frame {f} action @{code:#x}", file=out)
                    self.disasm(code, out=out)
                elif kind == 2:
                    print(f"\n== {label} frame {f} label {self.s(self.u32(item + 4))}", file=out)
                elif kind == 8:
                    sprite, code = self.u32(item + 4), self.u32(item + 8)
                    print(f"\n== {label} frame {f} init action for character {sprite} "
                          f"@{code:#x}", file=out)
                    self.disasm(code, out=out)
                elif kind == 3:
                    self.place_object(item, f"{label} frame {f}", out)

    def place_object(self, item, label, out):
        # PlaceObject: type, flags, depth, character, matrix(6 floats), color
        # transform, unknown, ratio, name, clip depth, clip actions.
        name = self.u32(item + 52)
        actions = self.u32(item + 60)
        if not actions:
            return
        n, events = self.u32(actions), self.u32(actions + 4)
        for e in range(n):
            ev = events + 16 * e
            flags, key, code = self.u32(ev), self.u32(ev + 4), self.u32(ev + 8)
            print(f"\n== {label} clip {self.s(name)} event flags {flags:#x} key {key} "
                  f"@{code:#x}", file=out)
            self.disasm(code, out=out)

    def run(self, out):
        m = self.movie_offset
        frame_count, frame_ptr = self.u32(m + 16), self.u32(m + 20)
        char_count, char_ptr = self.u32(m + 28), self.u32(m + 32)
        exports = self.u32(m + 56), self.u32(m + 60)
        for k in range(exports[0]):
            e = exports[1] + 8 * k
            print(f"export {self.s(self.u32(e))} -> character {self.u32(e + 4)}", file=out)
        self.frames(frame_count, frame_ptr, "movie", out)
        for c in range(char_count):
            ch = self.u32(char_ptr + 4 * c)
            if not ch or self.u32(ch + 4) != SIGNATURE:
                continue
            kind = self.u32(ch)
            if kind in (5, 9) and ch != m:
                self.frames(self.u32(ch + 16), self.u32(ch + 20), f"character {c} sprite", out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("apt")
    ap.add_argument("const")
    ap.add_argument("--strings", action="store_true", help="list the constant table")
    args = ap.parse_args()
    apt = Apt(open(args.apt, "rb").read(), open(args.const, "rb").read())
    if args.strings:
        for i, c in enumerate(apt.consts):
            print(i, c)
        return
    apt.run(sys.stdout)


if __name__ == "__main__":
    main()
