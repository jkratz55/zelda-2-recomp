/*
 * cpu.c - the dispatcher that drives recompiled code, interrupt delivery,
 * the fallback 6502 interpreter, and save states.
 *
 * How execution works:
 *   Each recompiled function (generated/bank*.c) runs a stretch of the
 *   original game code, then sets cpu.pc to wherever control goes next
 *   (a JSR target, an RTS return address, ...) and returns here.
 *   nes_run_frame() looks up the function for cpu.pc in the current PRG bank
 *   and calls it. If there isn't one (code the recompiler hasn't seen yet),
 *   the interpreter executes it instead, and the address is logged so that
 *   the next build can recompile it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nes.h"

RunStats run_stats;

/* ------------------------------------------------------- Opcode table */

enum {
    ADC, AND, ASL, BCC, BCS, BEQ, BIT, BMI, BNE, BPL, BRK, BVC, BVS, CLC, CLD, CLI,
    CLV, CMP, CPX, CPY, DEC, DEX, DEY, EOR, INC, INX, INY, JMP, JSR, LDA, LDX, LDY,
    LSR, NOP, ORA, PHA, PHP, PLA, PLP, ROL, ROR, RTI, RTS, SBC, SEC, SED, SEI, STA,
    STX, STY, TAX, TAY, TSX, TXA, TXS, TYA, ILLEGAL
};
static const char *mnemonics =
    "ADC AND ASL BCC BCS BEQ BIT BMI BNE BPL BRK BVC BVS CLC CLD CLI "
    "CLV CMP CPX CPY DEC DEX DEY EOR INC INX INY JMP JSR LDA LDX LDY "
    "LSR NOP ORA PHA PHP PLA PLP ROL ROR RTI RTS SBC SEC SED SEI STA "
    "STX STY TAX TAY TSX TXA TXS TYA ";

enum { M_IMP, M_ACC, M_IMM, M_ZP, M_ZPX, M_ZPY, M_ABS, M_ABX, M_ABY, M_IND, M_IZX, M_IZY, M_REL };
static const char *mode_names[] = {"imp", "acc", "imm", "zp", "zpx", "zpy", "abs", "abx", "aby", "ind", "izx", "izy", "rel"};
static const uint8_t mode_len[] = {1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 2, 2, 2};

/* Same table as tools/opcodes.py */
static const char *op_table =
    "00 BRK imp 7|01 ORA izx 6|05 ORA zp 3|06 ASL zp 5|08 PHP imp 3|09 ORA imm 2|"
    "0A ASL acc 2|0D ORA abs 4|0E ASL abs 6|10 BPL rel 2|11 ORA izy 5|15 ORA zpx 4|"
    "16 ASL zpx 6|18 CLC imp 2|19 ORA aby 4|1D ORA abx 4|1E ASL abx 7|20 JSR abs 6|"
    "21 AND izx 6|24 BIT zp 3|25 AND zp 3|26 ROL zp 5|28 PLP imp 4|29 AND imm 2|"
    "2A ROL acc 2|2C BIT abs 4|2D AND abs 4|2E ROL abs 6|30 BMI rel 2|31 AND izy 5|"
    "35 AND zpx 4|36 ROL zpx 6|38 SEC imp 2|39 AND aby 4|3D AND abx 4|3E ROL abx 7|"
    "40 RTI imp 6|41 EOR izx 6|45 EOR zp 3|46 LSR zp 5|48 PHA imp 3|49 EOR imm 2|"
    "4A LSR acc 2|4C JMP abs 3|4D EOR abs 4|4E LSR abs 6|50 BVC rel 2|51 EOR izy 5|"
    "55 EOR zpx 4|56 LSR zpx 6|58 CLI imp 2|59 EOR aby 4|5D EOR abx 4|5E LSR abx 7|"
    "60 RTS imp 6|61 ADC izx 6|65 ADC zp 3|66 ROR zp 5|68 PLA imp 4|69 ADC imm 2|"
    "6A ROR acc 2|6C JMP ind 5|6D ADC abs 4|6E ROR abs 6|70 BVS rel 2|71 ADC izy 5|"
    "75 ADC zpx 4|76 ROR zpx 6|78 SEI imp 2|79 ADC aby 4|7D ADC abx 4|7E ROR abx 7|"
    "81 STA izx 6|84 STY zp 3|85 STA zp 3|86 STX zp 3|88 DEY imp 2|8A TXA imp 2|"
    "8C STY abs 4|8D STA abs 4|8E STX abs 4|90 BCC rel 2|91 STA izy 6|94 STY zpx 4|"
    "95 STA zpx 4|96 STX zpy 4|98 TYA imp 2|99 STA aby 5|9A TXS imp 2|9D STA abx 5|"
    "A0 LDY imm 2|A1 LDA izx 6|A2 LDX imm 2|A4 LDY zp 3|A5 LDA zp 3|A6 LDX zp 3|"
    "A8 TAY imp 2|A9 LDA imm 2|AA TAX imp 2|AC LDY abs 4|AD LDA abs 4|AE LDX abs 4|"
    "B0 BCS rel 2|B1 LDA izy 5|B4 LDY zpx 4|B5 LDA zpx 4|B6 LDX zpy 4|B8 CLV imp 2|"
    "B9 LDA aby 4|BA TSX imp 2|BC LDY abx 4|BD LDA abx 4|BE LDX aby 4|C0 CPY imm 2|"
    "C1 CMP izx 6|C4 CPY zp 3|C5 CMP zp 3|C6 DEC zp 5|C8 INY imp 2|C9 CMP imm 2|"
    "CA DEX imp 2|CC CPY abs 4|CD CMP abs 4|CE DEC abs 6|D0 BNE rel 2|D1 CMP izy 5|"
    "D5 CMP zpx 4|D6 DEC zpx 6|D8 CLD imp 2|D9 CMP aby 4|DD CMP abx 4|DE DEC abx 7|"
    "E0 CPX imm 2|E1 SBC izx 6|E4 CPX zp 3|E5 SBC zp 3|E6 INC zp 5|E8 INX imp 2|"
    "E9 SBC imm 2|EA NOP imp 2|EC CPX abs 4|ED SBC abs 4|EE INC abs 6|F0 BEQ rel 2|"
    "F1 SBC izy 5|F5 SBC zpx 4|F6 INC zpx 6|F8 SED imp 2|F9 SBC aby 4|FD SBC abx 4|"
    "FE INC abx 7|";

static uint8_t op_mn[256], op_mode[256], op_cyc[256];

static void build_op_table(void) {
    for (int i = 0; i < 256; i++) { op_mn[i] = ILLEGAL; op_mode[i] = M_IMP; op_cyc[i] = 2; }
    const char *p = op_table;
    while (*p) {
        unsigned code, cyc;
        char mn[4], mode[4];
        if (sscanf(p, "%x %3s %3s %u", &code, mn, mode, &cyc) != 4) break;
        op_mn[code] = (uint8_t)((strstr(mnemonics, mn) - mnemonics) / 4);
        for (int m = 0; m <= M_REL; m++)
            if (!strcmp(mode_names[m], mode)) op_mode[code] = (uint8_t)m;
        op_cyc[code] = (uint8_t)cyc;
        p = strchr(p, '|') + 1;
    }
}

/* ---------------------------------------------------------- Interrupts */

void cpu_power_on(void) {
    memset(&cpu, 0, sizeof cpu);
    cpu.s = 0xFD;
    cpu.i = 1;
    cpu.pc = (uint16_t)(rd(0xFFFC) | rd(0xFFFD) << 8);
    cpu.cycles = 7;
}

void cpu_nmi(void) {
    push16(cpu.pc);
    push(get_p()); /* B flag clear */
    cpu.i = 1;
    cpu.pc = (uint16_t)(rd(0xFFFA) | rd(0xFFFB) << 8);
    cpu.cycles += 7;
}

void cpu_irq(void) {
    push16(cpu.pc);
    push(get_p());
    cpu.i = 1;
    cpu.pc = (uint16_t)(rd(0xFFFE) | rd(0xFFFF) << 8);
    cpu.cycles += 7;
}

/* --------------------------------------------------------- Interpreter */

void cpu_interp_step(void) {
    uint16_t pc = cpu.pc;
    uint8_t op = rd(pc);
    uint8_t mn = op_mn[op], mode = op_mode[op];
    uint8_t b1 = rd((uint16_t)(pc + 1));
    uint16_t w = (uint16_t)(b1 | rd((uint16_t)(pc + 2)) << 8);
    bool page_penalty = mn == LDA || mn == LDX || mn == LDY || mn == EOR || mn == AND ||
                        mn == ORA || mn == ADC || mn == SBC || mn == CMP;
    uint16_t ea = 0;

    run_stats.interp_instructions++;
    cpu.cycles += op_cyc[op];
    cpu.pc = (uint16_t)(pc + mode_len[mode]);

    switch (mode) {
    case M_IMM: ea = (uint16_t)(pc + 1); break;
    case M_ZP: ea = b1; break;
    case M_ZPX: ea = (uint8_t)(b1 + cpu.x); break;
    case M_ZPY: ea = (uint8_t)(b1 + cpu.y); break;
    case M_ABS: ea = w; break;
    case M_ABX: ea = page_penalty ? ea_abx_r(w) : (uint16_t)(w + cpu.x); break;
    case M_ABY: ea = page_penalty ? ea_aby_r(w) : (uint16_t)(w + cpu.y); break;
    case M_IND: ea = rd16_jmp_ind(w); break;
    case M_IZX: ea = ea_izx(b1); break;
    case M_IZY: ea = page_penalty ? ea_izy_r(b1) : ea_izy(b1); break;
    case M_REL: ea = (uint16_t)(cpu.pc + (int8_t)b1); break;
    }

#define BRANCH(cond)                                                    \
    if (cond) {                                                         \
        cpu.cycles += 1 + (((cpu.pc ^ ea) & 0xFF00) != 0);              \
        cpu.pc = ea;                                                    \
    }                                                                   \
    break
#define RMW(fn)                                                         \
    if (mode == M_ACC) cpu.a = fn(cpu.a);                               \
    else wr(ea, fn(rd(ea)));                                            \
    break

    switch (mn) {
    case ADC: op_adc(rd(ea)); break;
    case SBC: op_sbc(rd(ea)); break;
    case AND: cpu.a &= rd(ea); set_nz(cpu.a); break;
    case ORA: cpu.a |= rd(ea); set_nz(cpu.a); break;
    case EOR: cpu.a ^= rd(ea); set_nz(cpu.a); break;
    case ASL: RMW(op_asl);
    case LSR: RMW(op_lsr);
    case ROL: RMW(op_rol);
    case ROR: RMW(op_ror);
    case INC: wr(ea, op_inc(rd(ea))); break;
    case DEC: wr(ea, op_dec(rd(ea))); break;
    case BIT: op_bit(rd(ea)); break;
    case CMP: op_cmp(cpu.a, rd(ea)); break;
    case CPX: op_cmp(cpu.x, rd(ea)); break;
    case CPY: op_cmp(cpu.y, rd(ea)); break;
    case LDA: cpu.a = rd(ea); set_nz(cpu.a); break;
    case LDX: cpu.x = rd(ea); set_nz(cpu.x); break;
    case LDY: cpu.y = rd(ea); set_nz(cpu.y); break;
    case STA: wr(ea, cpu.a); break;
    case STX: wr(ea, cpu.x); break;
    case STY: wr(ea, cpu.y); break;
    case BCC: BRANCH(!cpu.c);
    case BCS: BRANCH(cpu.c);
    case BNE: BRANCH(!cpu.z);
    case BEQ: BRANCH(cpu.z);
    case BPL: BRANCH(!cpu.n);
    case BMI: BRANCH(cpu.n);
    case BVC: BRANCH(!cpu.v);
    case BVS: BRANCH(cpu.v);
    case JMP: cpu.pc = ea; break;
    case JSR: push16((uint16_t)(pc + 2)); cpu.pc = ea; break;
    case RTS: cpu.pc = (uint16_t)(pull16() + 1); break;
    case RTI: set_p(pull()); cpu.pc = pull16(); break;
    case BRK:
        push16((uint16_t)(pc + 2));
        push(get_p() | 0x10);
        cpu.i = 1;
        cpu.pc = (uint16_t)(rd(0xFFFE) | rd(0xFFFF) << 8);
        break;
    case PHA: push(cpu.a); break;
    case PHP: push(get_p() | 0x10); break;
    case PLA: cpu.a = pull(); set_nz(cpu.a); break;
    case PLP: set_p(pull()); break;
    case CLC: cpu.c = 0; break;
    case SEC: cpu.c = 1; break;
    case CLI: cpu.i = 0; break;
    case SEI: cpu.i = 1; break;
    case CLV: cpu.v = 0; break;
    case CLD: cpu.d = 0; break;
    case SED: cpu.d = 1; break;
    case TAX: cpu.x = cpu.a; set_nz(cpu.x); break;
    case TAY: cpu.y = cpu.a; set_nz(cpu.y); break;
    case TXA: cpu.a = cpu.x; set_nz(cpu.a); break;
    case TYA: cpu.a = cpu.y; set_nz(cpu.a); break;
    case TSX: cpu.x = cpu.s; set_nz(cpu.x); break;
    case TXS: cpu.s = cpu.x; break;
    case INX: cpu.x++; set_nz(cpu.x); break;
    case INY: cpu.y++; set_nz(cpu.y); break;
    case DEX: cpu.x--; set_nz(cpu.x); break;
    case DEY: cpu.y--; set_nz(cpu.y); break;
    case NOP: break;
    default: {
        static int warned;
        if (warned++ < 8)
            fprintf(stderr, "warning: illegal opcode $%02X at $%04X (treated as NOP)\n", op, pc);
        break;
    }
    }
#undef BRANCH
#undef RMW
}

/* ------------------------------------------------------------ Dispatch */

static RecompFn *fn_table;   /* indexed by physical PRG offset */
static uint16_t *fn_addr;    /* CPU address each entry was compiled for */
static uint8_t *miss_logged; /* bitmap of PRG offsets already logged */
static FILE *miss_file;

static void register_entry(uint32_t off, uint16_t addr, RecompFn fn) {
    if (off >= cart.prg_size) return;
    fn_table[off] = fn;
    fn_addr[off] = addr;
}

void dispatch_init(void) {
    build_op_table();
    fn_table = calloc(cart.prg_size, sizeof *fn_table);
    fn_addr = calloc(cart.prg_size, sizeof *fn_addr);
    miss_logged = calloc(cart.prg_size / 8 + 1, 1);
    for (size_t i = 0; i < recomp_entry_count; i++) {
        const RecompEntry *e = &recomp_entries[i];
        register_entry(e->prg_offset, e->addr, e->fn);
        miss_logged[e->prg_offset >> 3] |= (uint8_t)(1 << (e->prg_offset & 7));
    }
}

void recomp_override(int bank, uint16_t addr, RecompFn fn) {
    register_entry((uint32_t)bank * 0x4000 + (addr & 0x3FFF), addr, fn);
}

RecompFn dispatch_lookup(uint16_t pc) {
    if (pc < 0x8000) return NULL; /* code running from RAM: interpreter */
    uint32_t off = cart.prg_off[(pc >> 13) & 3] + (pc & 0x1FFF);
    RecompFn fn = fn_table[off];
    return (fn && fn_addr[off] == pc) ? fn : NULL;
}

static void log_miss(uint16_t pc) {
    if (pc < 0x8000 || !run_stats.miss_log) return;
    uint32_t off = cart.prg_off[(pc >> 13) & 3] + (pc & 0x1FFF);
    if (miss_logged[off >> 3] & (1 << (off & 7))) return;
    miss_logged[off >> 3] |= (uint8_t)(1 << (off & 7));
    int bank = (int)(off >> 14);
    uint16_t base = bank == (int)(cart.prg_size / 0x4000) - 1 ? 0xC000 : 0x8000;
    if ((pc & 0xC000) != base) return; /* bank mapped at an unusual address */
    if (!miss_file) miss_file = fopen(run_stats.miss_log, "a");
    if (miss_file) {
        fprintf(miss_file, "%02X:%04X\n", bank, pc);
        fflush(miss_file);
    }
}

/* ---------------------------------------------------------- Frame loop */

void nes_update_event_cycle(void) {
    uint64_t e = ppu_next_vblank_cycle();
    uint64_t irq = apu_next_irq_cycle();
    if (irq < e) e = irq;
    if (cpu.nmi_pending || (!cpu.i && apu_irq_line())) e = 0;
    cpu.event_cycle = e;
}

void nes_power_on(void) {
    memset(ram, 0, sizeof ram);
    cart_reset();
    ppu_reset();
    apu_reset();
    cpu_power_on();
    nes_update_event_cycle();
}

void nes_run_frame(void) {
    bool interpreting = false;
    for (;;) {
        if (cpu.cycles >= cpu.event_cycle) {
            nes_sync();
            if (cpu.nmi_pending) {
                cpu.nmi_pending = false;
                cpu_nmi();
            } else if (!cpu.i && apu_irq_line()) {
                cpu_irq();
            }
            nes_update_event_cycle();
            if (ppu.frame_ready) {
                ppu.frame_ready = false;
                return;
            }
            continue;
        }
        RecompFn fn = run_stats.interpret_only ? NULL : dispatch_lookup(cpu.pc);
        if (fn) {
            interpreting = false;
            run_stats.recomp_calls++;
            fn();
        } else {
            if (!interpreting && !run_stats.interpret_only) log_miss(cpu.pc);
            interpreting = true;
            cpu_interp_step();
        }
    }
}

/* ---------------------------------------------------------- Save states */

#define STATE_MAGIC "Z2RSTAT1"

typedef struct {
    uint8_t sram[0x2000];
    uint8_t shift, control, chr0, chr1, prg_reg;
} CartState;

bool nes_save_state(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    CartState cs;
    memcpy(cs.sram, cart.sram, sizeof cs.sram);
    cs.shift = cart.shift; cs.control = cart.control;
    cs.chr0 = cart.chr0; cs.chr1 = cart.chr1; cs.prg_reg = cart.prg_reg;
    fwrite(STATE_MAGIC, 1, 8, f);
    fwrite(&cpu, sizeof cpu, 1, f);
    fwrite(ram, sizeof ram, 1, f);
    fwrite(&cs, sizeof cs, 1, f);
    fwrite(&ppu, sizeof ppu, 1, f);
    fwrite(&apu, sizeof apu, 1, f);
    fclose(f);
    return true;
}

bool nes_load_state(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char magic[8];
    CartState cs;
    Cpu c;
    Ppu p;
    Apu a;
    static uint8_t r[0x800];
    bool ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, STATE_MAGIC, 8) &&
              fread(&c, sizeof c, 1, f) == 1 && fread(r, sizeof r, 1, f) == 1 &&
              fread(&cs, sizeof cs, 1, f) == 1 && fread(&p, sizeof p, 1, f) == 1 &&
              fread(&a, sizeof a, 1, f) == 1;
    fclose(f);
    if (!ok) return false;
    cpu = c;
    memcpy(ram, r, sizeof ram);
    ppu = p;
    apu = a;
    memcpy(cart.sram, cs.sram, sizeof cs.sram);
    cart.shift = cs.shift;
    cart.control = cs.control; cart.chr0 = cs.chr0; cart.chr1 = cs.chr1; cart.prg_reg = cs.prg_reg;
    cart_update_banks();
    nes_update_event_cycle();
    return true;
}
