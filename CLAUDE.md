# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A static recompilation of *Zelda II* (NES, USA, MMC1, ROM CRC32 `BA322865`) into native C for macOS (Windows planned).
`tools/recomp.py` (Python 3.9-compatible, no third-party deps) translates the ROM's 6502 code into C. `runtime/` is a
small NES hardware runtime (dispatcher, interpreter fallback, PPU, APU, MMC1, SDL3 frontend). It's a personal
learning project, and the user is not a C/C++ developer, so explain C and hardware concepts plainly.

The ROM (`*.nes` in the project root) and everything derived from it (`generated/`, `saves/`) must never be committed.

## Commands

```sh
cmake -S . -B build && cmake --build build -j8      # runs recomp.py, then compiles; needs SDL3 (brew install sdl3)
./build/zelda2                                     # play (ROM found automatically)
./build/zelda2 --headless 3000 --input tests/new_game.txt --shot 2999:/tmp/s.png   # no window; PNG viewable with Read
./build/zelda2 --interpret                         # reference mode: everything through the 6502 interpreter
tools/verify.sh 20000                              # main correctness test (all tests/*.txt; ~2.5 min at 20000 frames)
cmake --build build --target test_input && ./build/test_input   # controller test (SDL virtual gamepads)
python3 tools/random_input.py SEED FRAMES > tests/random_SEED.txt   # new random-play test script
```

To run verification for a single input script, run what `tools/verify.sh` does by hand:

```sh
cmake --build build --target zelda2 zelda2_verify
./build/zelda2 --no-mods --interpret --headless N --input tests/X.txt --hash-log /tmp/a
./build/zelda2_verify --no-mods --no-discover --headless N --input tests/X.txt --hash-log /tmp/b
cmp /tmp/a /tmp/b
```

`ZELDA2_RAM_LOG=1` makes `--hash-log` also dump all 2 KB of RAM every frame. This is useful for locating game
variables, or for finding which bytes diverge.

Gotchas:
- A normal (non-`--interpret`, non-`--no-discover`) run **appends** to `config/discovered.txt`, which is tracked.
  Pass `--no-discover` for throwaway experiments.
- Headless runs start from blank save RAM and write to `saves/headless.sav`, never to the real `saves/zelda2.sav`.
- `generated/` lives in the source tree and is shared by every build directory (`build/` and CLion's
  `cmake-build-debug/`).

## Architecture

**Recompiler → dispatcher model.** Generated functions never call each other in C. A JSR pushes the return
address onto the *emulated* 6502 stack (`ram[0x100..]`), sets `cpu.pc` and `return`s. `nes_run_frame()` in
`runtime/cpu.c` then looks up the next function by **physical PRG offset** of `cpu.pc` in the currently mapped
bank. This is deliberate, because the game manipulates its own stack: the JumpEngine at `07:D385`/`07:D382` pops
its return address and reads a jump table placed after the JSR. Don't "optimize" JSRs into direct C calls.

**Resume points.** A function has a `switch (cpu.pc)` at the top listing every address the dispatcher may
re-enter at: the function start, JSR return points, backward-branch targets, and the instruction after CLI/PLP.
`generated/dispatch.c` registers all of them. Any recompiled function is a faithful translation of the
instructions at its addresses, so overlapping or duplicated decodes are harmless. Leaving to the dispatcher is
always safe.

**Interrupt/timing model.** Every instruction does `CYC(n)`. PPU and APU are lazy: they catch up to
`cpu.cycles` (`ppu_run_to(cycles*3)`, `apu_run_to`) whenever a register (PPU, APU or mapper) is accessed, and at
frame boundaries. `cpu.event_cycle` is the next vblank/IRQ time. Recompiled code checks it only at backward
branches (`POLL`) and function exits, so NMIs arrive a few instructions later than on hardware. That's why the
normal build's RAM differs slightly (mostly stack bytes `$01FD`/`$01FE`) from `--interpret`. `nes_run_frame()`
returns at vblank start.

**Verification.** `recomp.py --strict` (target `zelda2_verify`, output in `build/generated_strict/`) polls before
*every* instruction and disables the `IDLE` fast-forward, so it must be bit-identical to `--interpret`. This
checks control flow and addressing translation. It does **not** catch bugs in the op helpers (`op_adc`, `set_nz`,
`ea_*`, `push`, ...) in `runtime/nes.h`, because the interpreter and the generated code share them.

**Code discovery.** Static descent only reaches code in the fixed bank and code it can see calls to. Calls from
the fixed bank into `$8000–$BFFF` have an unknown bank at compile time. Unknown addresses fall back to
`cpu_interp_step()`, and the start of each interpreted run is logged as `BB:AAAA` in `config/discovered.txt`.
Rebuilding recompiles those addresses. Seeds come from: reset/NMI vectors, `config/entries.txt`,
`config/discovered.txt`, and every address in `config/labels.txt`.

**Config files** (format `BB:AAAA` with hex bank and CPU address):
- `labels.txt`: `BB:AAAA Name [noreturn]`. The name becomes the C function name. Mark `noreturn` on any routine
  that consumes inline data after its JSR, or the recompiler decodes that data as code. Look for
  `/* not valid code */` in `generated/` to find such cases.
- `entries.txt`: extra entry points. Adding an address here (or in `labels.txt`) is how to create a **hook
  point** at an arbitrary instruction.

**Mods** (`mods/mods.c`). `recomp_override(bank, addr, fn)` replaces the dispatcher entry for that address, and
`mods_frame()` runs once per frame. A hook can run code *before* the original (then call the generated function),
or replace it entirely (it must leave `cpu.pc` at the next address). It can't run code "after the original
returns", because one 6502 routine spans many dispatcher calls. `--no-mods` disables mods; `verify.sh` uses it.

## Invariants to keep in sync

- The 6502 opcode table exists twice: `tools/opcodes.py` and the `op_table` string in `runtime/cpu.c`.
- Bank layout assumption: MMC1 PRG mode 3. The last bank (7) is fixed at `$C000`, and banks 0–6 run at `$8000`
  (`Rom.base()` in recomp.py, `log_miss()` in cpu.c). `BANK_CHECK` after stores that may hit `$8000+` exits if the
  code's own bank got switched out.
- The runtime refuses to run if the ROM's CRC differs from `recomp_rom_crc32` in the generated code.
- Save states (`nes_save_state`) are raw dumps of the `Cpu`/`Ppu`/`Apu` structs. Changing those structs breaks
  existing `.state` files, so bump `STATE_MAGIC`.
- Controller button mapping lives in `pad_map`/`key_map` in `runtime/input.c`. The default is positional,
  NES-style (bottom face button = B/attack, right = A/jump).

## Known game RAM

`$0773` magic, `$0774` life (`0x7F` = 4 full containers, `0x20` each), `$0777`/`$0778`/`$0779` attack/magic/life
level, `$0783`/`$0784` magic/heart containers, `$0518` post-hit invincibility timer, `$0100` NMI mode flags (bit 7
clear = sound-only NMI). Link's damage routine is `07:E30D` (`LinkTakeDamage`). At `07:E32F` register A holds the
damage amount.
