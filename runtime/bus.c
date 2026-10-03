/*
 * bus.c - CPU memory map, the MMC1 cartridge mapper and the controllers.
 *
 * CPU address space:
 *   $0000-$1FFF  2KB internal RAM (mirrored)
 *   $2000-$3FFF  PPU registers (mirrored every 8 bytes)
 *   $4000-$4017  APU + I/O registers
 *   $6000-$7FFF  8KB battery-backed save RAM
 *   $8000-$BFFF  switchable 16KB PRG-ROM bank
 *   $C000-$FFFF  fixed 16KB PRG-ROM bank (the last one)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nes.h"

Cpu cpu;
uint8_t ram[0x800];
Cart cart;
uint8_t pad_state[2];

static uint8_t pad_shift[2];
static bool pad_strobe;

/* -------------------------------------------------------------- CRC32 */

uint32_t crc32_buf(uint32_t crc, const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
            table[i] = c;
        }
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---------------------------------------------------------- Cartridge */

bool cart_load(const char *path, char *err, size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "cannot open ROM '%s'", path); return false; }
    uint8_t hdr[16];
    if (fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "NES\x1A", 4) != 0) {
        fclose(f);
        snprintf(err, errlen, "'%s' is not an iNES ROM", path);
        return false;
    }
    int mapper = (hdr[6] >> 4) | (hdr[7] & 0xF0);
    if (mapper != 1) {
        fclose(f);
        snprintf(err, errlen, "expected an MMC1 (mapper 1) ROM, got mapper %d", mapper);
        return false;
    }
    if (hdr[6] & 0x04) fseek(f, 512, SEEK_CUR); /* skip trainer */
    cart.prg_size = hdr[4] * 0x4000u;
    cart.chr_size = hdr[5] * 0x2000u;
    cart.prg = malloc(cart.prg_size);
    cart.chr = malloc(cart.chr_size ? cart.chr_size : 0x2000);
    if (fread(cart.prg, 1, cart.prg_size, f) != cart.prg_size ||
        fread(cart.chr, 1, cart.chr_size, f) != cart.chr_size) {
        fclose(f);
        snprintf(err, errlen, "ROM file is truncated");
        return false;
    }
    fclose(f);

    uint32_t crc = crc32_buf(0, cart.prg, cart.prg_size);
    crc = crc32_buf(crc, cart.chr, cart.chr_size);
    if (crc != recomp_rom_crc32) {
        snprintf(err, errlen,
                 "ROM CRC32 is %08X but the recompiled code was generated from %08X.\n"
                 "Re-run the build against this ROM (or use the matching dump).",
                 crc, recomp_rom_crc32);
        return false;
    }
    return true;
}

void cart_update_banks(void) {
    uint32_t banks16 = cart.prg_size / 0x4000;
    uint32_t bank = cart.prg_reg & 0x0F;
    uint32_t lo, hi;
    switch ((cart.control >> 2) & 3) {
    case 0: case 1: lo = bank & ~1u; hi = lo + 1; break; /* 32KB mode */
    case 2: lo = 0; hi = bank; break;                     /* fix first bank */
    default: lo = bank; hi = banks16 - 1; break;          /* fix last bank */
    }
    lo %= banks16; hi %= banks16;
    cart.prg_off[0] = lo * 0x4000; cart.prg_off[1] = lo * 0x4000 + 0x2000;
    cart.prg_off[2] = hi * 0x4000; cart.prg_off[3] = hi * 0x4000 + 0x2000;

    uint32_t chr_banks4 = (cart.chr_size ? cart.chr_size : 0x2000) / 0x1000;
    if (cart.control & 0x10) { /* two 4KB banks */
        cart.chr_off[0] = (cart.chr0 % chr_banks4) * 0x1000;
        cart.chr_off[1] = (cart.chr1 % chr_banks4) * 0x1000;
    } else { /* one 8KB bank */
        cart.chr_off[0] = ((cart.chr0 & 0x1E) % chr_banks4) * 0x1000;
        cart.chr_off[1] = cart.chr_off[0] + 0x1000;
    }
}

void cart_reset(void) {
    cart.shift = 0x10;
    cart.control = 0x0C; /* power on with the last bank fixed at $C000 */
    cart.chr0 = cart.chr1 = cart.prg_reg = 0;
    cart_update_banks();
}

uint8_t cart_mirroring(void) { return cart.control & 3; }

void mmc1_write(uint16_t addr, uint8_t v) {
    /* Bank changes affect what the PPU fetches, so let it catch up first. */
    ppu_run_to(cpu.cycles * 3);
    if (v & 0x80) {
        cart.shift = 0x10;
        cart.control |= 0x0C;
        cart_update_banks();
        return;
    }
    bool full = cart.shift & 1;
    cart.shift = (uint8_t)((cart.shift >> 1) | ((v & 1) << 4));
    if (!full) return;
    uint8_t value = cart.shift;
    cart.shift = 0x10;
    switch ((addr >> 13) & 3) {
    case 0: cart.control = value; break;
    case 1: cart.chr0 = value; break;
    case 2: cart.chr1 = value; break;
    case 3: cart.prg_reg = value; break;
    }
    cart_update_banks();
}

/* ---------------------------------------------------------------- I/O */

uint8_t bus_read_io(uint16_t a) {
    if (a < 0x4000) {
        ppu_run_to(cpu.cycles * 3);
        return ppu_read_reg(a);
    }
    if (a < 0x4020) {
        switch (a) {
        case 0x4015: apu_run_to(cpu.cycles); return apu_read_status();
        case 0x4016: case 0x4017: {
            int p = a & 1;
            if (pad_strobe) pad_shift[p] = pad_state[p];
            uint8_t bit = pad_shift[p] & 1;
            pad_shift[p] = (uint8_t)((pad_shift[p] >> 1) | 0x80); /* 1s after 8 reads */
            return 0x40 | bit;
        }
        default: return 0x40; /* open bus-ish */
        }
    }
    if (a >= 0x6000) return cart.sram[a - 0x6000];
    return 0;
}

void bus_write_io(uint16_t a, uint8_t v) {
    if (a < 0x4000) {
        ppu_run_to(cpu.cycles * 3);
        ppu_write_reg(a, v);
        return;
    }
    if (a < 0x4020) {
        if (a == 0x4014) { /* OAM DMA: copy a 256-byte page into sprite memory */
            ppu_run_to(cpu.cycles * 3);
            uint16_t page = (uint16_t)(v << 8);
            for (int i = 0; i < 256; i++)
                ppu.oam[(uint8_t)(ppu.oam_addr + i)] = rd((uint16_t)(page | i));
            cpu.cycles += 513 + (cpu.cycles & 1);
            return;
        }
        if (a == 0x4016) {
            pad_strobe = v & 1;
            if (pad_strobe) { pad_shift[0] = pad_state[0]; pad_shift[1] = pad_state[1]; }
            return;
        }
        apu_run_to(cpu.cycles);
        apu_write(a, v);
        return;
    }
    if (a >= 0x8000) { mmc1_write(a, v); return; }
    if (a >= 0x6000) {
        if (cart.sram[a - 0x6000] != v) { cart.sram[a - 0x6000] = v; cart.sram_dirty = true; }
    }
}
