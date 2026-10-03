/*
 * nes.h - shared state for the Zelda II recompilation runtime.
 *
 * The runtime is a small NES "virtual machine": it owns the CPU registers,
 * RAM, the cartridge (MMC1 mapper), the PPU (graphics), APU (sound) and the
 * controllers. The game's code itself is *not* interpreted here - it lives in
 * generated/ as C functions produced by tools/recomp.py. The dispatcher in
 * cpu.c calls those functions; an interpreter is kept only as a fallback for
 * code the recompiler hasn't discovered yet.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ CPU */

typedef struct {
    uint8_t a, x, y, s;
    uint16_t pc;
    uint8_t c, z, n, v, i, d; /* status flags, each 0 or 1 */
    uint64_t cycles;          /* CPU cycles since power-on */
    /* Recompiled code yields back to the dispatcher once cycles reaches this,
       so interrupts and frame boundaries are handled promptly. */
    uint64_t event_cycle;
    bool nmi_pending;
} Cpu;

extern Cpu cpu;
extern uint8_t ram[0x800];

/* ------------------------------------------------------------ Cartridge */

typedef struct {
    uint8_t *prg; /* PRG-ROM (code + data) */
    uint32_t prg_size;
    uint8_t *chr; /* CHR-ROM (tile graphics) */
    uint32_t chr_size;
    uint8_t sram[0x2000]; /* battery-backed save RAM at $6000-$7FFF */
    bool sram_dirty;

    /* MMC1 registers */
    uint8_t shift, control, chr0, chr1, prg_reg;
    uint32_t prg_off[4]; /* byte offset into prg for $8000,$A000,$C000,$E000 */
    uint32_t chr_off[2]; /* byte offset into chr for PPU $0000,$1000 */
} Cart;

extern Cart cart;

bool cart_load(const char *path, char *err, size_t errlen);
uint32_t crc32_buf(uint32_t crc, const uint8_t *p, size_t n);
void cart_reset(void);
void cart_update_banks(void); /* recompute bank offsets from the MMC1 registers */
void mmc1_write(uint16_t addr, uint8_t v);
uint8_t cart_mirroring(void); /* 0=1scr-lo 1=1scr-hi 2=vertical 3=horizontal */

/* ------------------------------------------------------------------ PPU */

typedef struct {
    uint8_t ctrl, mask, status, oam_addr;
    uint16_t v, t; /* "loopy" VRAM address registers */
    uint8_t x, w;  /* fine X scroll, write toggle */
    uint8_t read_buffer, open_bus;
    uint8_t oam[256];
    uint8_t nt[0x800]; /* 2KB nametable RAM */
    uint8_t pal[32];

    int scanline, dot; /* last processed position */
    bool odd_frame;
    uint64_t clock; /* PPU dots processed since power-on */
    uint64_t frame;

    /* background pipeline */
    uint8_t nt_byte, at_byte, lo_byte, hi_byte;
    uint64_t tile_data;

    /* sprites for the current scanline */
    int sprite_count;
    uint32_t sprite_pat[8];
    uint8_t sprite_x[8], sprite_pri[8], sprite_idx[8];

    bool frame_ready; /* set when the picture for a frame is complete */
} Ppu;

extern Ppu ppu;
extern uint32_t ppu_framebuffer[256 * 240]; /* 0xFFRRGGBB */

void ppu_reset(void);
void ppu_run_to(uint64_t dot);
uint64_t ppu_next_vblank_cycle(void);
uint8_t ppu_read_reg(uint16_t addr);
void ppu_write_reg(uint16_t addr, uint8_t v);

/* ------------------------------------------------------------------ APU */

typedef struct {
    bool enabled;
    uint8_t duty, seq;
    uint16_t period, timer;
    uint8_t length;
    bool halt;
    bool env_start, env_const;
    uint8_t env_period, env_div, env_decay;
    bool sweep_en, sweep_neg, sweep_reload;
    uint8_t sweep_period, sweep_div, sweep_shift;
    uint8_t ones_comp;
} ApuPulse;

typedef struct {
    bool enabled, control, linear_reload;
    uint8_t linear, linear_period, seq, length;
    uint16_t period, timer;
} ApuTriangle;

typedef struct {
    bool enabled, halt, mode;
    bool env_start, env_const;
    uint8_t env_period, env_div, env_decay, length;
    uint16_t period, timer, lfsr;
} ApuNoise;

typedef struct {
    bool enabled, irq_en, loop, irq, silence, buffer_empty;
    uint8_t output, shift, bits, buffer;
    uint16_t rate, timer, addr, cur_addr, len, remaining;
} ApuDmc;

typedef struct {
    ApuPulse p1, p2;
    ApuTriangle tri;
    ApuNoise noise;
    ApuDmc dmc;
    uint8_t frame_mode;
    bool irq_inhibit, frame_irq;
    uint32_t frame_cycle;
    uint64_t cycle; /* CPU cycles processed */

    /* resampling */
    double sample_acc, sample_sum;
    int sample_n;
    float hp_prev_in, hp_prev_out, lp_prev;
} Apu;

extern Apu apu;

void apu_reset(void);
void apu_run_to(uint64_t cycle);
uint8_t apu_read_status(void);
void apu_write(uint16_t addr, uint8_t v);
bool apu_irq_line(void);
uint64_t apu_next_irq_cycle(void);
/* Pull up to max generated audio samples (mono, int16, APU_SAMPLE_RATE). */
size_t apu_take_samples(int16_t *out, size_t max);
#define APU_SAMPLE_RATE 48000

/* ---------------------------------------------------------- Controllers */

enum {
    BTN_A = 0x01, BTN_B = 0x02, BTN_SELECT = 0x04, BTN_START = 0x08,
    BTN_UP = 0x10, BTN_DOWN = 0x20, BTN_LEFT = 0x40, BTN_RIGHT = 0x80,
};
extern uint8_t pad_state[2]; /* set by the frontend each frame */

/* ------------------------------------------------------------------ Bus */

uint8_t bus_read_io(uint16_t a);
void bus_write_io(uint16_t a, uint8_t v);

static inline uint8_t rd(uint16_t a) {
    if (a < 0x2000) return ram[a & 0x7FF];
    if (a >= 0x8000) return cart.prg[cart.prg_off[(a >> 13) & 3] + (a & 0x1FFF)];
    return bus_read_io(a);
}

static inline void wr(uint16_t a, uint8_t v) {
    if (a < 0x2000) { ram[a & 0x7FF] = v; return; }
    bus_write_io(a, v);
}

/* Bring PPU and APU up to the CPU's current time. */
static inline void nes_sync(void) {
    ppu_run_to(cpu.cycles * 3);
    apu_run_to(cpu.cycles);
}

/* ------------------------------------------------- CPU operation helpers
 * Shared by the interpreter and the recompiled code so both have identical
 * semantics. */

static inline void set_nz(uint8_t v) { cpu.z = (v == 0); cpu.n = v >> 7; }

static inline void op_adc(uint8_t m) {
    unsigned sum = cpu.a + m + cpu.c;
    cpu.v = ((~(cpu.a ^ m) & (cpu.a ^ sum)) >> 7) & 1;
    cpu.c = sum > 0xFF;
    cpu.a = (uint8_t)sum;
    set_nz(cpu.a);
}
static inline void op_sbc(uint8_t m) { op_adc(m ^ 0xFF); } /* 2A03: no decimal mode */
static inline void op_cmp(uint8_t r, uint8_t m) { cpu.c = r >= m; set_nz((uint8_t)(r - m)); }
static inline void op_bit(uint8_t m) { cpu.z = (cpu.a & m) == 0; cpu.n = m >> 7; cpu.v = (m >> 6) & 1; }
static inline uint8_t op_asl(uint8_t v) { cpu.c = v >> 7; v = (uint8_t)(v << 1); set_nz(v); return v; }
static inline uint8_t op_lsr(uint8_t v) { cpu.c = v & 1; v >>= 1; set_nz(v); return v; }
static inline uint8_t op_rol(uint8_t v) { uint8_t c = cpu.c; cpu.c = v >> 7; v = (uint8_t)(v << 1) | c; set_nz(v); return v; }
static inline uint8_t op_ror(uint8_t v) { uint8_t c = cpu.c; cpu.c = v & 1; v = (uint8_t)((v >> 1) | (c << 7)); set_nz(v); return v; }
static inline uint8_t op_inc(uint8_t v) { v++; set_nz(v); return v; }
static inline uint8_t op_dec(uint8_t v) { v--; set_nz(v); return v; }

static inline void push(uint8_t v) { ram[0x100 | cpu.s] = v; cpu.s--; }
static inline uint8_t pull(void) { cpu.s++; return ram[0x100 | cpu.s]; }
static inline void push16(uint16_t v) { push(v >> 8); push(v & 0xFF); }
static inline uint16_t pull16(void) { uint8_t lo = pull(); return (uint16_t)(lo | (pull() << 8)); }

static inline uint8_t get_p(void) {
    return (uint8_t)(cpu.c | cpu.z << 1 | cpu.i << 2 | cpu.d << 3 | 0x20 | cpu.v << 6 | cpu.n << 7);
}
static inline void set_p(uint8_t p) {
    cpu.c = p & 1; cpu.z = (p >> 1) & 1; cpu.i = (p >> 2) & 1;
    cpu.d = (p >> 3) & 1; cpu.v = (p >> 6) & 1; cpu.n = p >> 7;
}

/* Effective-address helpers. The "_r" variants add the page-cross cycle that
   indexed reads incur. */
static inline uint16_t ea_abx_r(uint16_t base) {
    uint16_t a = (uint16_t)(base + cpu.x);
    cpu.cycles += ((a ^ base) & 0xFF00) != 0;
    return a;
}
static inline uint16_t ea_aby_r(uint16_t base) {
    uint16_t a = (uint16_t)(base + cpu.y);
    cpu.cycles += ((a ^ base) & 0xFF00) != 0;
    return a;
}
static inline uint16_t ea_izx(uint8_t zp) {
    uint8_t p = (uint8_t)(zp + cpu.x);
    return (uint16_t)(ram[p] | ram[(uint8_t)(p + 1)] << 8);
}
static inline uint16_t ea_izy(uint8_t zp) {
    uint16_t base = (uint16_t)(ram[zp] | ram[(uint8_t)(zp + 1)] << 8);
    return (uint16_t)(base + cpu.y);
}
static inline uint16_t ea_izy_r(uint8_t zp) {
    uint16_t base = (uint16_t)(ram[zp] | ram[(uint8_t)(zp + 1)] << 8);
    uint16_t a = (uint16_t)(base + cpu.y);
    cpu.cycles += ((a ^ base) & 0xFF00) != 0;
    return a;
}
/* JMP ($xxFF) famously wraps within the page on the 6502. */
static inline uint16_t rd16_jmp_ind(uint16_t a) {
    uint8_t lo = rd(a);
    uint8_t hi = rd((uint16_t)((a & 0xFF00) | ((a + 1) & 0xFF)));
    return (uint16_t)(lo | hi << 8);
}

/* ----------------------------------------------------- Dispatch / driver */

typedef void (*RecompFn)(void);

typedef struct {
    uint32_t prg_offset; /* physical offset into PRG-ROM */
    uint16_t addr;       /* CPU address the code runs at */
    RecompFn fn;
    const char *name;
} RecompEntry;

/* Provided by generated/dispatch.c */
extern const RecompEntry recomp_entries[];
extern const size_t recomp_entry_count;
extern const uint32_t recomp_rom_crc32; /* CRC32 of the ROM the code was generated from */

void cpu_power_on(void);
void cpu_nmi(void);
void cpu_irq(void);
void cpu_interp_step(void); /* run exactly one instruction in the interpreter */
void dispatch_init(void);
RecompFn dispatch_lookup(uint16_t pc);
/* Replace the routine at (16KB PRG bank, CPU address) with hand-written C. */
void recomp_override(int bank, uint16_t addr, RecompFn fn);
/* True if the PRG bank mapped at addr is still `bank` (used after mapper writes). */
static inline bool bank_is(uint16_t addr, int bank) {
    return (cart.prg_off[(addr >> 13) & 3] >> 14) == (uint32_t)bank;
}

typedef struct {
    bool interpret_only;      /* ignore recompiled code (reference mode) */
    const char *miss_log;     /* where to append newly discovered entry points */
    uint64_t interp_instructions, recomp_calls;
} RunStats;
extern RunStats run_stats;

void nes_power_on(void);
void nes_run_frame(void);
void nes_update_event_cycle(void);

/* Save states */
bool nes_save_state(const char *path);
bool nes_load_state(const char *path);

/* mods/mods.c */
void mods_init(void);
void mods_frame(void); /* called once per frame */
extern bool mods_enabled;
