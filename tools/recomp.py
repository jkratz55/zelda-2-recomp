#!/usr/bin/env python3
"""recomp.py - statically recompile the NES game's 6502 code into C.

Pipeline
  1. Discovery: starting from the reset/NMI vectors plus every entry point
     listed in config/entries.txt and config/discovered.txt, walk the code
     (recursive descent) and collect every subroutine (JSR target).
  2. Partitioning: split the code into C functions. Each function starts at
     an entry point and includes everything reachable from it by branches,
     jumps and fall-through, stopping at other entry points. Code shared by
     several functions gets promoted to its own entry point.
  3. Emission: write one C file per PRG bank plus a dispatch table that maps
     (bank, address) -> C function.

Every generated function is a faithful translation of the instructions at
its addresses, so control can always be handed back to the dispatcher in
runtime/cpu.c (which also handles interrupts and falls back to an
interpreter for any address that has no recompiled code yet).

Usage: recomp.py --rom game.nes --config config --out generated
"""
import argparse
import collections
import os
import re
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from opcodes import BRANCHES, MODE_SIZE, OPCODES, PAGE_CROSS_READS, format_operand  # noqa: E402


# --------------------------------------------------------------------- ROM

class Rom:
    def __init__(self, path):
        data = open(path, "rb").read()
        if data[:4] != b"NES\x1a":
            sys.exit("%s: not an iNES ROM" % path)
        off = 16 + (512 if data[6] & 4 else 0)
        prg_len = data[4] * 0x4000
        chr_len = data[5] * 0x2000
        self.prg = data[off:off + prg_len]
        self.chr = data[off + prg_len:off + prg_len + chr_len]
        self.crc = zlib.crc32(self.prg + self.chr) & 0xFFFFFFFF
        self.nbanks = data[4]
        self.fixed = self.nbanks - 1  # MMC1 mode 3: last bank fixed at $C000

    def base(self, bank):
        return 0xC000 if bank == self.fixed else 0x8000

    def in_bank(self, bank, addr):
        b = self.base(bank)
        return b <= addr < b + 0x4000

    def byte(self, bank, addr):
        return self.prg[bank * 0x4000 + addr - self.base(bank)]

    def offset(self, bank, addr):
        return bank * 0x4000 + addr - self.base(bank)

    def resolve(self, bank, target):
        """Which bank does `target` live in when jumped to from `bank`?
        None if unknown at compile time (e.g. fixed bank -> switchable
        window, or code in RAM)."""
        if self.in_bank(bank, target):
            return bank
        if target >= 0xC000:
            return self.fixed
        return None


class Insn:
    __slots__ = ("addr", "op", "mnem", "mode", "size", "operand", "cycles", "raw")

    def __init__(self, addr, op, mnem, mode, size, operand, cycles, raw):
        self.addr, self.op, self.mnem, self.mode = addr, op, mnem, mode
        self.size, self.operand, self.cycles, self.raw = size, operand, cycles, raw

    @property
    def next(self):
        return (self.addr + self.size) & 0xFFFF

    def text(self, names=None):
        if self.mnem is None:
            return ".db $%02X" % self.op
        s = "%s %s" % (self.mnem, format_operand(self.mode, self.operand))
        return s.strip()


_decode_cache = {}


def decode(rom, bank, addr):
    key = (bank, addr)
    if key in _decode_cache:
        return _decode_cache[key]
    op = rom.byte(bank, addr)
    ins = None
    if op in OPCODES:
        mnem, mode, cyc = OPCODES[op]
        size = MODE_SIZE[mode]
        if rom.in_bank(bank, addr + size - 1):
            raw = [rom.byte(bank, addr + i) for i in range(size)]
            operand = None
            if size == 2:
                operand = raw[1]
            elif size == 3:
                operand = raw[1] | raw[2] << 8
            if mode == "rel":
                operand = (addr + 2 + (operand - 256 if operand > 127 else operand)) & 0xFFFF
            ins = Insn(addr, op, mnem, mode, size, operand, cyc, raw)
    if ins is None:  # illegal opcode or crosses the end of the bank
        ins = Insn(addr, op, None, "imp", 1, None, 2, [op])
    _decode_cache[key] = ins
    return ins


def successors(ins, noreturn):
    """Control-flow edges out of an instruction: list of (kind, target)."""
    m = ins.mnem
    if m is None or m in ("RTS", "RTI", "BRK"):
        return []
    if m == "JMP":
        return [("jmp", ins.operand)] if ins.mode == "abs" else []
    if m == "JSR":
        edges = [("jsr", ins.operand)]
        if ins.operand not in noreturn:
            edges.append(("ret", ins.next))
        return edges
    if m in BRANCHES:
        return [("branch", ins.operand), ("fall", ins.next)]
    return [("fall", ins.next)]


# ------------------------------------------------------------------ Config

ENTRY_RE = re.compile(r"^\s*([0-9A-Fa-f]{1,2})\s*:\s*([0-9A-Fa-f]{4})\b(.*)$")


def read_entries(path):
    out = []
    if not os.path.exists(path):
        return out
    for line in open(path):
        line = line.split("#", 1)[0]
        m = ENTRY_RE.match(line)
        if m:
            out.append((int(m.group(1), 16), int(m.group(2), 16)))
    return out


def read_labels(path):
    """labels.txt lines: `BB:AAAA  Name  [noreturn]`"""
    names, flags = {}, collections.defaultdict(set)
    if not os.path.exists(path):
        return names, flags
    for line in open(path):
        line = line.split("#", 1)[0]
        m = ENTRY_RE.match(line)
        if not m:
            continue
        key = (int(m.group(1), 16), int(m.group(2), 16))
        rest = m.group(3).split()
        if rest:
            if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", rest[0]):
                sys.exit("bad label name %r in %s" % (rest[0], path))
            names[key] = rest[0]
            flags[key].update(rest[1:])
    return names, flags


# --------------------------------------------------------------- Analysis

class Function:
    def __init__(self, bank, start):
        self.bank, self.start = bank, start
        self.insns = {}

    @property
    def key(self):
        return (self.bank, self.start)


class Recompiler:
    def __init__(self, rom, seeds, names, flags):
        self.rom = rom
        self.names = names
        # noreturn is keyed by CPU address for successors(); collect per bank.
        self.noreturn = {b: {a for (bb, a), f in flags.items() if "noreturn" in f and
                             (bb == b or bb == rom.fixed)} for b in range(rom.nbanks)}
        self.entries = set()
        for bank, addr in seeds:
            if 0 <= bank < rom.nbanks and rom.in_bank(bank, addr):
                self.entries.add((bank, addr))
            else:
                print("warning: ignoring entry %02X:%04X (not in bank)" % (bank, addr), file=sys.stderr)

    # Phase 1: follow calls to find every reachable subroutine.
    def discover(self):
        work = list(self.entries)
        explored = set()
        while work:
            bank, start = work.pop()
            if (bank, start) in explored:
                continue
            explored.add((bank, start))
            seen = set()
            stack = [start]
            while stack:
                a = stack.pop()
                if a in seen:
                    continue
                seen.add(a)
                ins = decode(self.rom, bank, a)
                for kind, t in successors(ins, self.noreturn[bank]):
                    tb = self.rom.resolve(bank, t)
                    if tb is None:
                        continue
                    if kind == "jsr" or tb != bank:
                        if (tb, t) not in self.entries:
                            self.entries.add((tb, t))
                            work.append((tb, t))
                    else:
                        stack.append(t)

    def build(self, bank, start):
        f = Function(bank, start)
        stack = [start]
        while stack:
            a = stack.pop()
            if a in f.insns:
                continue
            ins = decode(self.rom, bank, a)
            f.insns[a] = ins
            for kind, t in successors(ins, self.noreturn[bank]):
                if kind == "jsr":
                    continue
                if self.inside(f, t):
                    stack.append(t)
        return f

    def inside(self, f, t):
        """Is target t compiled as part of f (goto) rather than an exit?"""
        if self.rom.resolve(f.bank, t) != f.bank:
            return False
        return t == f.start or (f.bank, t) not in self.entries

    # Phase 2: build functions; promote shared code to entry points.
    def partition(self):
        for _ in range(50):
            funcs = {e: self.build(*e) for e in self.entries}
            count = collections.Counter()
            for f in funcs.values():
                for a in f.insns:
                    count[(f.bank, a)] += 1
            promote = set()
            for f in funcs.values():
                for a, ins in f.insns.items():
                    for kind, t in successors(ins, self.noreturn[f.bank]):
                        if kind != "jsr" and t in f.insns and t != f.start:
                            if count[(f.bank, t)] > count[(f.bank, a)]:
                                promote.add((f.bank, t))
            if not promote:
                return funcs
            self.entries |= promote
        sys.exit("partitioning did not converge")


# --------------------------------------------------------------- Emission

def ram_or_bus(addr):
    return addr < 0x2000


class Emitter:
    def __init__(self, rc, strict=False):
        self.rc = rc
        # strict: yield to the dispatcher before every instruction, so
        # interrupts land exactly where the interpreter would deliver them.
        # Used to verify the generated code against the interpreter.
        self.strict = strict
        self.rom = rc.rom
        self.fn_names = {}

    def name_of(self, key):
        if key in self.rc.names:
            return self.rc.names[key]
        return "f%X_%04X" % key

    def target_comment(self, bank, t):
        tb = self.rom.resolve(bank, t)
        if tb is not None and (tb, t) in self.rc.names:
            return " -> " + self.rc.names[(tb, t)]
        return ""

    # --- operand helpers -------------------------------------------------
    def read_expr(self, ins):
        m, v = ins.mode, ins.operand
        pen = ins.mnem in PAGE_CROSS_READS
        if m == "imm":
            return "0x%02X" % v
        if m == "zp":
            return "ram[0x%02X]" % v
        if m == "zpx":
            return "ram[(uint8_t)(0x%02X + cpu.x)]" % v
        if m == "zpy":
            return "ram[(uint8_t)(0x%02X + cpu.y)]" % v
        if m == "abs":
            return ("ram[0x%03X]" % (v & 0x7FF)) if ram_or_bus(v) else "rd(0x%04X)" % v
        if m == "abx":
            return ("rd(ea_abx_r(0x%04X))" if pen else "rd((uint16_t)(0x%04X + cpu.x))") % v
        if m == "aby":
            return ("rd(ea_aby_r(0x%04X))" if pen else "rd((uint16_t)(0x%04X + cpu.y))") % v
        if m == "izx":
            return "rd(ea_izx(0x%02X))" % v
        if m == "izy":
            return ("rd(ea_izy_r(0x%02X))" if pen else "rd(ea_izy(0x%02X))") % v
        raise ValueError(m)

    def lvalue(self, ins):
        """C lvalue for zero-page / RAM operands, else None."""
        m, v = ins.mode, ins.operand
        if m == "zp":
            return "ram[0x%02X]" % v
        if m == "zpx":
            return "ram[(uint8_t)(0x%02X + cpu.x)]" % v
        if m == "zpy":
            return "ram[(uint8_t)(0x%02X + cpu.y)]" % v
        if m == "abs" and ram_or_bus(v):
            return "ram[0x%03X]" % (v & 0x7FF)
        return None

    def ea_expr(self, ins):
        m, v = ins.mode, ins.operand
        if m == "abs":
            return "0x%04X" % v
        if m == "abx":
            return "(uint16_t)(0x%04X + cpu.x)" % v
        if m == "aby":
            return "(uint16_t)(0x%04X + cpu.y)" % v
        if m == "izx":
            return "ea_izx(0x%02X)" % v
        if m == "izy":
            return "ea_izy(0x%02X)" % v
        raise ValueError(m)

    @staticmethod
    def may_hit_mapper(ins):
        m, v = ins.mode, ins.operand
        if m in ("zp", "zpx", "zpy"):
            return False
        if m == "abs":
            return v >= 0x8000
        if m in ("abx", "aby"):
            return v + 0xFF >= 0x8000
        return True  # indirect: unknown

    def store(self, ins, val):
        lv = self.lvalue(ins)
        if lv:
            return "%s = %s;" % (lv, val)
        return "wr(%s, %s);" % (self.ea_expr(ins), val)

    def rmw(self, ins, fn):
        if ins.mode == "acc":
            return "cpu.a = %s(cpu.a);" % fn
        lv = self.lvalue(ins)
        if lv:
            return "%s = %s(%s);" % (lv, fn, lv)
        if ins.mode == "abs":
            return "wr(0x%04X, %s(rd(0x%04X)));" % (ins.operand, fn, ins.operand)
        return "{ uint16_t ea = %s; wr(ea, %s(rd(ea))); }" % (self.ea_expr(ins), fn)

    # --- one function ----------------------------------------------------
    def emit_function(self, f, out):
        rom, rc = self.rom, self.rc
        bank = f.bank
        addrs = sorted(f.insns)
        noret = rc.noreturn[bank]

        # Resume points: where the dispatcher may enter this function.
        resume = {f.start}
        labels = {f.start}
        for a in addrs:
            ins = f.insns[a]
            for kind, t in successors(ins, noret):
                if kind == "ret" and t in f.insns and rc.inside(f, t):
                    resume.add(t)
                    labels.add(t)
                elif kind in ("branch", "jmp") and rc.inside(f, t):
                    labels.add(t)
                    if t <= a:
                        resume.add(t)  # backward edge: may yield here
            if ins.mnem in ("CLI", "PLP") and ins.next in f.insns and rc.inside(f, ins.next):
                resume.add(ins.next)
                labels.add(ins.next)
        # fall-through edges that are not adjacent in emission order need labels
        for i, a in enumerate(addrs):
            ins = f.insns[a]
            for kind, t in successors(ins, noret):
                if kind == "fall" and t in f.insns and rc.inside(f, t):
                    if i + 1 >= len(addrs) or addrs[i + 1] != t:
                        labels.add(t)
        if self.strict:
            resume.update(addrs)
            labels.update(addrs)
        f.resume = resume

        name = self.name_of(f.key)
        out.append("/* ---- %02X:%04X %s */" % (bank, f.start, "-" * 50))
        out.append("void %s(void) {" % name)
        others = sorted(resume - {f.start})
        if others:
            out.append("    switch (cpu.pc) {")
            for r in others:
                out.append("    case 0x%04X: goto L_%04X;" % (r, r))
            out.append("    }")
        if addrs[0] != f.start:
            out.append("    goto L_%04X;" % f.start)

        for i, a in enumerate(addrs):
            ins = f.insns[a]
            if a in labels:
                out.append("L_%04X:" % a)
            if self.strict:
                out.append("    POLL(0x%04X);" % a)
            code, falls = self.emit_insn(f, ins)
            comment = "/* %04X: %-14s */" % (a, ins.text())
            out.append("    %s %s" % (comment, code))
            if falls:
                t = ins.next
                if t in f.insns and rc.inside(f, t):
                    if i + 1 >= len(addrs) or addrs[i + 1] != t:
                        out.append("    goto L_%04X;" % t)
                else:
                    out.append("    cpu.pc = 0x%04X; return;%s" % (t, self.target_comment(bank, t)
                                                                   and " /*%s */" % self.target_comment(bank, t)))
        out.append("}")
        out.append("")

    def transfer(self, f, ins, t, kind):
        """Code for a jump/branch to t (without the cycle accounting)."""
        if self.rc.inside(f, t):
            poll = "POLL(0x%04X); " % t if t <= ins.addr else ""
            if t == ins.addr and ins.mnem == "JMP" and not self.strict:
                return "IDLE(0x%04X);" % t  # JMP * : wait for the next interrupt
            return "%sgoto L_%04X;" % (poll, t)
        tc = self.target_comment(f.bank, t)
        return "cpu.pc = 0x%04X; return;%s" % (t, (" /*%s */" % tc) if tc else "")

    def emit_insn(self, f, ins):
        """Returns (C code, falls_through)."""
        m = ins.mnem
        cyc = "CYC(%d); " % ins.cycles
        if m is None:
            return ("cpu.pc = 0x%04X; cpu_interp_step(); return; /* not valid code */" % ins.addr, False)

        def rd_():
            return self.read_expr(ins)

        if m in ("LDA", "LDX", "LDY"):
            r = "cpu." + m[2].lower()
            return (cyc + "%s = %s; set_nz(%s);" % (r, rd_(), r), True)
        if m in ("STA", "STX", "STY"):
            code = cyc + self.store(ins, "cpu." + m[2].lower())
            if self.may_hit_mapper(ins):
                code += " BANK_CHECK(0x%04X, %d);" % (ins.next, f.bank)
            return (code, True)
        if m in ("AND", "ORA", "EOR"):
            op = {"AND": "&=", "ORA": "|=", "EOR": "^="}[m]
            return (cyc + "cpu.a %s %s; set_nz(cpu.a);" % (op, rd_()), True)
        if m in ("ADC", "SBC"):
            return (cyc + "op_%s(%s);" % (m.lower(), rd_()), True)
        if m in ("CMP", "CPX", "CPY"):
            r = {"CMP": "a", "CPX": "x", "CPY": "y"}[m]
            return (cyc + "op_cmp(cpu.%s, %s);" % (r, rd_()), True)
        if m == "BIT":
            return (cyc + "op_bit(%s);" % rd_(), True)
        if m in ("ASL", "LSR", "ROL", "ROR", "INC", "DEC"):
            code = cyc + self.rmw(ins, "op_" + m.lower())
            if ins.mode != "acc" and self.may_hit_mapper(ins):
                code += " BANK_CHECK(0x%04X, %d);" % (ins.next, f.bank)
            return (code, True)
        simple = {
            "TAX": "cpu.x = cpu.a; set_nz(cpu.x);", "TAY": "cpu.y = cpu.a; set_nz(cpu.y);",
            "TXA": "cpu.a = cpu.x; set_nz(cpu.a);", "TYA": "cpu.a = cpu.y; set_nz(cpu.a);",
            "TSX": "cpu.x = cpu.s; set_nz(cpu.x);", "TXS": "cpu.s = cpu.x;",
            "INX": "cpu.x++; set_nz(cpu.x);", "INY": "cpu.y++; set_nz(cpu.y);",
            "DEX": "cpu.x--; set_nz(cpu.x);", "DEY": "cpu.y--; set_nz(cpu.y);",
            "CLC": "cpu.c = 0;", "SEC": "cpu.c = 1;", "CLV": "cpu.v = 0;",
            "CLD": "cpu.d = 0;", "SED": "cpu.d = 1;", "SEI": "cpu.i = 1;",
            "PHA": "push(cpu.a);", "PHP": "push(get_p() | 0x10);",
            "PLA": "cpu.a = pull(); set_nz(cpu.a);", "NOP": "",
        }
        if m in simple:
            return (cyc + simple[m], True)
        if m in ("CLI", "PLP"):
            code = cyc + ("cpu.i = 0;" if m == "CLI" else "set_p(pull());")
            return (code + " IRQ_CHECK(0x%04X);" % ins.next, True)
        if m in BRANCHES:
            cond = {"BPL": "!cpu.n", "BMI": "cpu.n", "BVC": "!cpu.v", "BVS": "cpu.v",
                    "BCC": "!cpu.c", "BCS": "cpu.c", "BNE": "!cpu.z", "BEQ": "cpu.z"}[m]
            extra = 1 + (1 if (ins.next ^ ins.operand) & 0xFF00 else 0)
            return (cyc + "if (%s) { CYC(%d); %s }" % (cond, extra, self.transfer(f, ins, ins.operand, "branch")),
                    True)
        if m == "JMP":
            if ins.mode == "ind":
                return (cyc + "cpu.pc = rd16_jmp_ind(0x%04X); return;" % ins.operand, False)
            return (cyc + self.transfer(f, ins, ins.operand, "jmp"), False)
        if m == "JSR":
            tc = self.target_comment(f.bank, ins.operand)
            return (cyc + "push16(0x%04X); cpu.pc = 0x%04X; return;%s"
                    % ((ins.addr + 2) & 0xFFFF, ins.operand, (" /*%s */" % tc) if tc else ""), False)
        if m == "RTS":
            return (cyc + "cpu.pc = (uint16_t)(pull16() + 1); return;", False)
        if m == "RTI":
            return (cyc + "set_p(pull()); cpu.pc = pull16(); IRQ_RECHECK(); return;", False)
        if m == "BRK":
            return (cyc + "push16(0x%04X); push(get_p() | 0x10); cpu.i = 1; "
                          "cpu.pc = (uint16_t)(rd(0xFFFE) | rd(0xFFFF) << 8); return;" % ((ins.addr + 2) & 0xFFFF),
                    False)
        raise ValueError(m)


def write_if_changed(path, text):
    if os.path.exists(path) and open(path).read() == text:
        return
    with open(path, "w") as fh:
        fh.write(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--config", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--strict", action="store_true",
                    help="poll for interrupts before every instruction (verification builds)")
    args = ap.parse_args()

    rom = Rom(args.rom)
    names, flags = read_labels(os.path.join(args.config, "labels.txt"))
    seeds = [(rom.fixed, rom.byte(rom.fixed, 0xFFFC) | rom.byte(rom.fixed, 0xFFFD) << 8),
             (rom.fixed, rom.byte(rom.fixed, 0xFFFA) | rom.byte(rom.fixed, 0xFFFB) << 8)]
    seeds += read_entries(os.path.join(args.config, "entries.txt"))
    seeds += read_entries(os.path.join(args.config, "discovered.txt"))
    seeds += list(names.keys())

    rc = Recompiler(rom, seeds, names, flags)
    rc.discover()
    funcs = rc.partition()
    em = Emitter(rc, strict=args.strict)

    os.makedirs(args.out, exist_ok=True)
    header = ("/* AUTO-GENERATED by tools/recomp.py from ROM CRC32 %08X - do not edit.\n"
              " * To change behaviour, override functions in mods/ instead. */\n" % rom.crc)

    by_bank = collections.defaultdict(list)
    for key in sorted(funcs):
        by_bank[key[0]].append(funcs[key])

    protos = [header, "#pragma once", ""]
    total_insns = 0
    for bank in range(rom.nbanks):
        out = [header, '#include "recomp.h"', '#include "recomp_funcs.h"', ""]
        for f in by_bank.get(bank, []):
            em.emit_function(f, out)
            protos.append("void %s(void);" % em.name_of(f.key))
            total_insns += len(f.insns)
        write_if_changed(os.path.join(args.out, "bank%d.c" % bank), "\n".join(out) + "\n")
    write_if_changed(os.path.join(args.out, "recomp_funcs.h"), "\n".join(protos) + "\n")

    # Dispatch table: every resume point of every function.
    rows = {}
    for key in sorted(funcs):
        f = funcs[key]
        for r in sorted(f.resume):
            k = (f.bank, r)
            if k not in rows or r == f.start:
                rows[k] = (f, r)
    disp = [header, '#include "nes.h"', '#include "recomp_funcs.h"', "",
            "const uint32_t recomp_rom_crc32 = 0x%08Xu;" % rom.crc, "",
            "const RecompEntry recomp_entries[] = {"]
    for (bank, r), (f, _) in sorted(rows.items()):
        disp.append('    {0x%05X, 0x%04X, %s, "%s"},' % (rom.offset(bank, r), r, em.name_of(f.key),
                                                     em.name_of(f.key)))
    disp.append("};")
    disp.append("const size_t recomp_entry_count = sizeof recomp_entries / sizeof recomp_entries[0];")
    write_if_changed(os.path.join(args.out, "dispatch.c"), "\n".join(disp) + "\n")

    covered = set()
    for f in funcs.values():
        for a, ins in f.insns.items():
            for i in range(ins.size):
                covered.add(rom.offset(f.bank, a + i))
    print("recomp: %d functions, %d instructions, %d dispatch entries, %d/%d PRG bytes as code (%.1f%%)"
          % (len(funcs), total_insns, len(rows), len(covered), len(rom.prg), 100.0 * len(covered) / len(rom.prg)))
    for bank in range(rom.nbanks):
        n = sum(1 for o in covered if o // 0x4000 == bank)
        print("  bank %d: %3d functions, %5d code bytes" % (bank, len(by_bank.get(bank, [])), n))


if __name__ == "__main__":
    main()
