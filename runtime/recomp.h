/*
 * recomp.h - helpers used by the generated C code in generated/bank*.c.
 *
 * A note on reading generated code: each line is one original 6502
 * instruction. The comment shows the original assembly, e.g.
 *
 *     L_C010:
 *         / * C010: LDA $0736 * / CYC(4); cpu.a = ram[0x736]; set_nz(cpu.a);
 *
 * cpu.a/x/y are the 6502 registers, cpu.c/z/n/v the flags, ram[] the 2KB
 * work RAM, rd()/wr() access everything else (PPU, APU, save RAM, ROM).
 */
#pragma once
#include "nes.h"

/* Account for the time an instruction takes (keeps video/audio in sync). */
#define CYC(n) (cpu.cycles += (n))

/* At loop back-edges: hand control back to the dispatcher if an interrupt
   (vblank NMI) or the end of a frame is due. */
#define POLL(addr) \
    if (cpu.cycles >= cpu.event_cycle) { cpu.pc = (addr); return; }

/* "JMP *": the CPU spins until an interrupt arrives - skip ahead to it. */
#define IDLE(addr) \
    do { if (cpu.cycles < cpu.event_cycle) cpu.cycles = cpu.event_cycle; cpu.pc = (addr); return; } while (0)

/* After a write that may have switched PRG banks: if the bank this code
   lives in is no longer mapped, continue via the dispatcher. */
#define BANK_CHECK(next, bank) \
    if (!bank_is((next), (bank))) { cpu.pc = (next); return; }

/* After CLI / PLP: take a pending IRQ. */
#define IRQ_CHECK(next) \
    if (!cpu.i && apu_irq_line()) { cpu.event_cycle = 0; cpu.pc = (next); return; }
#define IRQ_RECHECK() \
    if (!cpu.i && apu_irq_line()) cpu.event_cycle = 0
