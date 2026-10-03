/*
 * ppu.c - the NES picture processing unit (2C02), emulated per dot.
 *
 * The PPU runs "lazily": it only catches up to the CPU's clock when the CPU
 * touches a PPU register, a mapper register, or at frame boundaries. Because
 * it is dot-accurate, mid-frame tricks (status-bar splits via sprite 0 hit,
 * CHR bank switches) render correctly.
 *
 * NTSC timing: 341 dots x 262 scanlines, 3 dots per CPU cycle.
 * Scanlines 0-239 are visible, 241 starts vblank (NMI), 261 is pre-render.
 */
#include <string.h>

#include "nes.h"

Ppu ppu;
uint32_t ppu_framebuffer[256 * 240];

static const uint32_t nes_palette[64] = {
    0x666666, 0x002A88, 0x1412A7, 0x3B00A4, 0x5C007E, 0x6E0040, 0x6C0600, 0x561D00,
    0x333500, 0x0B4800, 0x005200, 0x004F08, 0x00404D, 0x000000, 0x000000, 0x000000,
    0xADADAD, 0x155FD9, 0x4240FF, 0x7527FE, 0xA01ACC, 0xB71E7B, 0xB53120, 0x994E00,
    0x6B6D00, 0x388700, 0x0C9300, 0x008F32, 0x007C8D, 0x000000, 0x000000, 0x000000,
    0xFFFEFF, 0x64B0FF, 0x9290FF, 0xC676FF, 0xF36AFF, 0xFE6ECC, 0xFE8170, 0xEA9E22,
    0xBCBE00, 0x88D800, 0x5CE430, 0x45E082, 0x48CDDE, 0x4F4F4F, 0x000000, 0x000000,
    0xFFFEFF, 0xC0DFFF, 0xD3D2FF, 0xE8C8FF, 0xFBC2FF, 0xFEC4EA, 0xFECCC5, 0xF7D8A5,
    0xE4E594, 0xCFEF96, 0xBDF4AB, 0xB3F3CC, 0xB5EBF2, 0xB8B8B8, 0x000000, 0x000000,
};

void ppu_reset(void) {
    memset(&ppu, 0, sizeof ppu);
    ppu.scanline = 261; /* start in pre-render so frame 0 is a whole frame */
    ppu.dot = 0;
}

/* -------------------------------------------------------------- VRAM */

static inline uint16_t nt_index(uint16_t addr) {
    uint16_t table = (addr >> 10) & 3, off = addr & 0x3FF;
    switch (cart_mirroring()) {
    case 0: return off;
    case 1: return 0x400 | off;
    case 2: return (uint16_t)((table & 1) << 10 | off); /* vertical */
    default: return (uint16_t)((table >> 1) << 10 | off); /* horizontal */
    }
}

static inline uint8_t pal_index(uint16_t addr) {
    uint8_t i = addr & 0x1F;
    if (i >= 16 && (i & 3) == 0) i -= 16; /* $3F10/14/18/1C mirror $3F00/04/08/0C */
    return i;
}

static inline uint8_t chr_read(uint16_t addr) {
    return cart.chr[cart.chr_off[(addr >> 12) & 1] + (addr & 0xFFF)];
}

static uint8_t vram_read(uint16_t addr) {
    addr &= 0x3FFF;
    if (addr < 0x2000) return chr_read(addr);
    if (addr < 0x3F00) return ppu.nt[nt_index(addr)];
    return ppu.pal[pal_index(addr)];
}

static void vram_write(uint16_t addr, uint8_t v) {
    addr &= 0x3FFF;
    if (addr < 0x2000) return; /* CHR-ROM is read-only */
    if (addr < 0x3F00) { ppu.nt[nt_index(addr)] = v; return; }
    ppu.pal[pal_index(addr)] = v & 0x3F;
}

/* --------------------------------------------------------- Registers */

uint8_t ppu_read_reg(uint16_t addr) {
    uint8_t r = ppu.open_bus;
    switch (addr & 7) {
    case 2:
        r = (uint8_t)((ppu.status & 0xE0) | (ppu.open_bus & 0x1F));
        ppu.status &= 0x7F;
        ppu.w = 0;
        break;
    case 4: r = ppu.oam[ppu.oam_addr]; break;
    case 7:
        if ((ppu.v & 0x3FFF) < 0x3F00) {
            r = ppu.read_buffer;
            ppu.read_buffer = vram_read(ppu.v);
        } else {
            r = vram_read(ppu.v);
            ppu.read_buffer = vram_read((uint16_t)(ppu.v - 0x1000));
        }
        ppu.v = (uint16_t)(ppu.v + ((ppu.ctrl & 0x04) ? 32 : 1));
        break;
    }
    ppu.open_bus = r;
    return r;
}

void ppu_write_reg(uint16_t addr, uint8_t v) {
    ppu.open_bus = v;
    switch (addr & 7) {
    case 0: {
        bool was_enabled = ppu.ctrl & 0x80;
        ppu.ctrl = v;
        ppu.t = (uint16_t)((ppu.t & 0xF3FF) | ((v & 3) << 10));
        /* Enabling NMI while already in vblank fires one immediately. */
        if (!was_enabled && (v & 0x80) && (ppu.status & 0x80)) {
            cpu.nmi_pending = true;
            cpu.event_cycle = 0;
        }
        break;
    }
    case 1: ppu.mask = v; break;
    case 3: ppu.oam_addr = v; break;
    case 4: ppu.oam[ppu.oam_addr++] = v; break;
    case 5:
        if (!ppu.w) {
            ppu.t = (uint16_t)((ppu.t & 0xFFE0) | (v >> 3));
            ppu.x = v & 7;
        } else {
            ppu.t = (uint16_t)((ppu.t & 0x8FFF) | ((v & 7) << 12));
            ppu.t = (uint16_t)((ppu.t & 0xFC1F) | ((v & 0xF8) << 2));
        }
        ppu.w ^= 1;
        break;
    case 6:
        if (!ppu.w) {
            ppu.t = (uint16_t)((ppu.t & 0x80FF) | ((v & 0x3F) << 8));
        } else {
            ppu.t = (uint16_t)((ppu.t & 0xFF00) | v);
            ppu.v = ppu.t;
        }
        ppu.w ^= 1;
        break;
    case 7:
        vram_write(ppu.v, v);
        ppu.v = (uint16_t)(ppu.v + ((ppu.ctrl & 0x04) ? 32 : 1));
        break;
    }
}

/* ------------------------------------------------- Scroll increments */

static inline void increment_x(void) {
    if ((ppu.v & 0x001F) == 31) {
        ppu.v &= (uint16_t)~0x001F;
        ppu.v ^= 0x0400;
    } else {
        ppu.v++;
    }
}

static inline void increment_y(void) {
    if ((ppu.v & 0x7000) != 0x7000) {
        ppu.v += 0x1000;
        return;
    }
    ppu.v &= (uint16_t)~0x7000;
    int y = (ppu.v & 0x03E0) >> 5;
    if (y == 29) { y = 0; ppu.v ^= 0x0800; }
    else if (y == 31) y = 0;
    else y++;
    ppu.v = (uint16_t)((ppu.v & ~0x03E0) | (y << 5));
}

static inline void copy_x(void) { ppu.v = (uint16_t)((ppu.v & 0xFBE0) | (ppu.t & 0x041F)); }
static inline void copy_y(void) { ppu.v = (uint16_t)((ppu.v & 0x841F) | (ppu.t & 0x7BE0)); }

/* ---------------------------------------------------- Background fetch */

static inline void fetch_nt(void) { ppu.nt_byte = vram_read((uint16_t)(0x2000 | (ppu.v & 0x0FFF))); }

static inline void fetch_at(void) {
    uint16_t v = ppu.v;
    uint16_t a = (uint16_t)(0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 0x07));
    int shift = ((v >> 4) & 4) | (v & 2);
    ppu.at_byte = (uint8_t)(((vram_read(a) >> shift) & 3) << 2);
}

static inline uint16_t bg_pattern_addr(void) {
    return (uint16_t)(((ppu.ctrl & 0x10) ? 0x1000 : 0) + ppu.nt_byte * 16 + ((ppu.v >> 12) & 7));
}

static inline void store_tile(void) {
    uint32_t data = 0;
    uint8_t lo = ppu.lo_byte, hi = ppu.hi_byte;
    for (int i = 0; i < 8; i++) {
        data <<= 4;
        data |= (uint32_t)(ppu.at_byte | ((lo & 0x80) >> 7) | ((hi & 0x80) >> 6));
        lo <<= 1; hi <<= 1;
    }
    ppu.tile_data |= data;
}

/* ------------------------------------------------------------ Sprites */

static uint32_t sprite_pattern(int i, int row) {
    uint8_t tile = ppu.oam[i * 4 + 1], attr = ppu.oam[i * 4 + 2];
    uint16_t addr;
    if (!(ppu.ctrl & 0x20)) { /* 8x8 */
        if (attr & 0x80) row = 7 - row;
        addr = (uint16_t)(((ppu.ctrl & 0x08) ? 0x1000 : 0) + tile * 16 + row);
    } else { /* 8x16 */
        if (attr & 0x80) row = 15 - row;
        uint16_t table = (tile & 1) ? 0x1000 : 0;
        tile &= 0xFE;
        if (row > 7) { tile++; row -= 8; }
        addr = (uint16_t)(table + tile * 16 + row);
    }
    uint8_t lo = vram_read(addr), hi = vram_read((uint16_t)(addr + 8));
    uint8_t a = (uint8_t)((attr & 3) << 2);
    uint32_t data = 0;
    for (int k = 0; k < 8; k++) {
        uint8_t p1, p2;
        if (attr & 0x40) { p1 = lo & 1; p2 = (uint8_t)((hi & 1) << 1); lo >>= 1; hi >>= 1; }
        else { p1 = (lo & 0x80) >> 7; p2 = (hi & 0x80) >> 6; lo <<= 1; hi <<= 1; }
        data <<= 4;
        data |= (uint32_t)(a | p1 | p2);
    }
    return data;
}

static void evaluate_sprites(void) {
    int h = (ppu.ctrl & 0x20) ? 16 : 8;
    int count = 0;
    for (int i = 0; i < 64; i++) {
        int row = ppu.scanline - ppu.oam[i * 4];
        if (row < 0 || row >= h) continue;
        if (count < 8) {
            ppu.sprite_pat[count] = sprite_pattern(i, row);
            ppu.sprite_x[count] = ppu.oam[i * 4 + 3];
            ppu.sprite_pri[count] = (ppu.oam[i * 4 + 2] >> 5) & 1;
            ppu.sprite_idx[count] = (uint8_t)i;
        }
        count++;
    }
    if (count > 8) { count = 8; ppu.status |= 0x20; }
    ppu.sprite_count = count;
}

/* ------------------------------------------------------------- Output */

static inline void render_pixel(void) {
    int x = ppu.dot - 1, y = ppu.scanline;
    uint8_t bg = 0;
    if ((ppu.mask & 0x08) && (x >= 8 || (ppu.mask & 0x02)))
        bg = (uint8_t)((ppu.tile_data >> 32 >> ((7 - ppu.x) * 4)) & 0x0F);

    uint8_t spr = 0;
    int si = 0;
    if ((ppu.mask & 0x10) && (x >= 8 || (ppu.mask & 0x04))) {
        for (int i = 0; i < ppu.sprite_count; i++) {
            int off = x - ppu.sprite_x[i];
            if (off < 0 || off > 7) continue;
            uint8_t c = (uint8_t)((ppu.sprite_pat[i] >> ((7 - off) * 4)) & 0x0F);
            if ((c & 3) == 0) continue;
            spr = c; si = i;
            break;
        }
    }

    uint8_t color;
    bool b = bg & 3, s = spr & 3;
    if (!b && !s) color = 0;
    else if (!b) color = 0x10 | spr;
    else if (!s) color = bg;
    else {
        if (ppu.sprite_idx[si] == 0 && x < 255) ppu.status |= 0x40; /* sprite 0 hit */
        color = ppu.sprite_pri[si] ? bg : (uint8_t)(0x10 | spr);
    }
    uint8_t idx = ppu.pal[pal_index(color)];
    if (ppu.mask & 0x01) idx &= 0x30; /* greyscale */
    ppu_framebuffer[y * 256 + x] = 0xFF000000u | nes_palette[idx & 0x3F];
}

/* ------------------------------------------------------------- Stepping */

static void ppu_step(void) {
    bool rendering = ppu.mask & 0x18;

    /* advance position (odd frames skip the last pre-render dot) */
    if (rendering && ppu.odd_frame && ppu.scanline == 261 && ppu.dot == 339) {
        ppu.dot = 0; ppu.scanline = 0; ppu.odd_frame = !ppu.odd_frame; ppu.frame++;
    } else if (++ppu.dot > 340) {
        ppu.dot = 0;
        if (++ppu.scanline > 261) { ppu.scanline = 0; ppu.odd_frame = !ppu.odd_frame; ppu.frame++; }
    }

    int line = ppu.scanline, dot = ppu.dot;
    bool visible = line < 240, pre = line == 261;

    if (rendering) {
        if (visible && dot >= 1 && dot <= 256) render_pixel();
        if ((visible || pre) && ((dot >= 1 && dot <= 256) || (dot >= 321 && dot <= 336))) {
            ppu.tile_data <<= 4;
            switch (dot & 7) {
            case 1: fetch_nt(); break;
            case 3: fetch_at(); break;
            case 5: ppu.lo_byte = vram_read(bg_pattern_addr()); break;
            case 7: ppu.hi_byte = vram_read((uint16_t)(bg_pattern_addr() + 8)); break;
            case 0: store_tile(); increment_x(); break;
            }
        }
        if (visible || pre) {
            if (dot == 256) increment_y();
            if (dot == 257) copy_x();
            if (pre && dot >= 280 && dot <= 304) copy_y();
        }
        if (dot == 257) {
            if (visible) evaluate_sprites();
            else ppu.sprite_count = 0;
        }
    } else if (visible && dot >= 1 && dot <= 256) {
        /* rendering disabled: show the backdrop colour */
        uint8_t idx = ppu.pal[0];
        if ((ppu.v & 0x3F00) == 0x3F00) idx = ppu.pal[pal_index(ppu.v)];
        ppu_framebuffer[line * 256 + dot - 1] = 0xFF000000u | nes_palette[idx & 0x3F];
    }

    if (line == 241 && dot == 1) {
        ppu.status |= 0x80;
        ppu.frame_ready = true;
        if (ppu.ctrl & 0x80) { cpu.nmi_pending = true; cpu.event_cycle = 0; }
    }
    if (pre && dot == 1) ppu.status &= 0x1F; /* clear vblank, sprite 0, overflow */
}

void ppu_run_to(uint64_t dot) {
    while (ppu.clock < dot) {
        ppu_step();
        ppu.clock++;
    }
}

/* CPU cycle at which the next vblank begins (may be off by a dot; the
   dispatcher simply re-checks). */
uint64_t ppu_next_vblank_cycle(void) {
    long dots;
    int line = ppu.scanline, dot = ppu.dot;
    if (line < 241 || (line == 241 && dot < 1))
        dots = (long)(241 - line) * 341 + (1 - dot);
    else
        dots = (long)(261 - line) * 341 + (340 - dot) + 241L * 341 + 2;
    uint64_t target = ppu.clock + (uint64_t)dots;
    return (target + 2) / 3;
}
