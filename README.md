# Zelda II: The Adventure of Link — static recompilation

A native macOS port of *Zelda II* (NES, USA), made by **statically
recompiling** the original 6502 machine code into C. The game's own logic
runs as compiled native code. Around it sits a small runtime that stands in
for the NES hardware: graphics, sound, the cartridge mapper and the
controllers.

This is a personal learning project. The ROM is **not** included; you need
your own dump. Neither the ROM nor anything generated from it should be
committed (see `.gitignore`).

## Quick start (macOS)

Requirements: Xcode command line tools, CMake, Python 3, and SDL3:

```sh
brew install cmake sdl3
```

Build and run (the ROM is picked up from the project directory automatically):

```sh
cmake -S . -B build
cmake --build build -j8
./build/zelda2
```

| NES button | Keyboard | Xbox controller | PlayStation controller |
|---|---|---|---|
| D-pad | Arrow keys | D-pad or left stick | D-pad or left stick |
| A (jump) | X | B or Y | Circle or Triangle |
| B (attack) | Z | A or X | Cross or Square |
| Start | Enter | Menu | Options |
| Select | Right Shift | View | Create / Share |

| Emulator function | Key |
|---|---|
| Save / load state | F5 / F9 |
| Fast forward (hold) | Tab |
| Pause, fullscreen, reset, quit | P, F11, F12, Esc |

Controllers work over USB or Bluetooth and can be plugged in or unplugged at
any time; the terminal prints `Controller connected: …` when one is
detected. The keyboard always works too, alongside any controllers. The
default mapping copies the original NES pad, where B (attack) sits to the
left of A (jump). To change it, edit `pad_map` at the top of
`runtime/input.c`. Other brands (Switch Pro, 8BitDo, …) work the same way.
For anything SDL doesn't recognise, put a
[`gamecontrollerdb.txt`](https://github.com/mdqinc/SDL_GameControllerDB) in
the project folder.

Battery saves (your three save files) go to `saves/zelda2.sav`.

## How it works

```
 ROM (6502 code) ──► tools/recomp.py ──► generated/bank0..7.c  (one C function per routine)
                                                   │
 runtime/  (the "hardware")                        ▼
   cpu.c   dispatcher + interrupts ◄──── calls the generated functions
   ppu.c   graphics chip (per-dot accurate)
   apu.c   sound chip
   bus.c   memory map + MMC1 bank switching + controllers
   main.c  SDL3 window, audio, input
```

1. **Recompiler** (`tools/recomp.py`, Python). Starting from the reset and
   NMI (vblank interrupt) vectors, it follows the code, splits it into
   routines and writes each one as a C function. Every line of output is
   one original instruction, with the assembly kept as a comment:

   ```c
   /* E331: LDA $0774      */ CYC(4); cpu.a = ram[0x774]; set_nz(cpu.a);
   /* E334: SEC            */ CYC(2); cpu.c = 1;
   /* E335: SBC $0C        */ CYC(3); op_sbc(ram[0x0C]);
   /* E337: STA $0774      */ CYC(4); ram[0x774] = cpu.a;
   ```

2. **Dispatcher** (`runtime/cpu.c`). A generated function runs until control
   leaves it (JSR, RTS, a jump elsewhere), sets `cpu.pc` and returns. The
   dispatcher then looks up the next function for that address in whichever
   ROM bank is currently mapped. Keeping the original 6502 stack in emulated
   RAM means the game's stack tricks just work: its jump-table routine pops
   its own return address, for example. The dispatcher is also where the
   vblank NMI gets delivered.

3. **Timing.** Each instruction adds its cycle count. The PPU and APU run
   lazily and catch up to the CPU whenever the game touches their registers.
   That keeps sprite-0 status-bar splits, music and sound effects in time.

### Code discovery

Zelda II switches 16 KB ROM banks at runtime, so the recompiler can't find
every routine just by reading the code. When execution reaches code that
hasn't been recompiled yet, the runtime falls back to a built-in 6502
interpreter, so the game keeps working. It also appends the address to
`config/discovered.txt`. The next `cmake --build build` recompiles those
routines too.

So **just play the game, then rebuild now and then.** The window title shows
how many instructions per second still go through the interpreter; that
number drops toward 0 as coverage grows. Title screen, file select, name
entry, the overworld and side-view battles are already fully native.

### Verifying correctness

```sh
tools/verify.sh            # default 1500 frames; e.g. tools/verify.sh 20000
```

This replays every input script in `tests/` twice: once through the
reference interpreter, and once through a "strict" build of the recompiled
code (`zelda2_verify`) that delivers interrupts at exactly the same
instruction boundaries. The RAM and the picture must match on every frame.
Currently four scripts × 20,000 frames match bit for bit.

To make a new test, generate random play or write a script by hand:

```sh
python3 tools/random_input.py 4 20000 > tests/random_4.txt
```

Script format, one line per button press: `first_frame last_frame BUTTONS`,
e.g. `1100 1400 LEFT` or `500 505 A+RIGHT`.

Handy headless options for experiments:

```sh
./build/zelda2 --headless 3000 --input tests/new_game.txt --shot 2999:shot.png
./build/zelda2 --interpret     # run purely in the interpreter (reference mode)
```

## Making changes to the game

Everything goes in **`mods/mods.c`**. The generated code is rebuilt from the
ROM on every build, so never edit it by hand. There are two kinds of hook:

* **Routine hooks** run your C code when the game reaches an address. The
  included example, *half damage*, hooks `$E32F` inside Link's damage
  routine. At that point register A holds the damage about to be subtracted,
  so the hook halves it and then runs the original code. To hook any other
  instruction, add its address to `config/entries.txt` or name it in
  `config/labels.txt`, then rebuild.
* **Frame hooks** (`mods_frame()`) run once per frame, which suits RAM tweaks
  like the included *infinite magic* example.

Both examples are off by default. Flip the `ENABLE_…` switches at the top of
`mods/mods.c` and rebuild. Run with `--no-mods` to disable all mods.

**Naming things:** `config/labels.txt` gives routines readable names, and
those become the C function names. As you work out what code does, naming it
is the main way this "recompilation" turns into a real "decompilation".
Common next steps:

1. Name routines and RAM variables as you figure them out (fan-made RAM maps
   and disassemblies of Zelda II are a great reference).
2. Rewrite interesting routines as clean, idiomatic C in `mods/`, using
   `recomp_override()` to swap them in. Then run `tools/verify.sh` with the
   override enabled to check your rewrite behaves identically.
3. Build enhancements on top: rebalancing, quality-of-life features,
   widescreen, and so on.

## Project layout

| Path | What |
|---|---|
| `tools/recomp.py` | 6502 → C recompiler |
| `tools/opcodes.py` | 6502 instruction table |
| `tools/verify.sh` | recompiled-vs-interpreter equivalence test |
| `tools/random_input.py` | random-play input generator |
| `runtime/input.c` | keyboard + controller handling (button mapping lives here) |
| `tests/test_input.c` | controller test using virtual controllers (`cmake --build build --target test_input && ./build/test_input`) |
| `runtime/` | NES hardware runtime + SDL3 frontend |
| `mods/mods.c` | your changes |
| `config/labels.txt` | names (and flags) for routines |
| `config/entries.txt` | extra code entry points / hook points |
| `config/discovered.txt` | entry points found while playing (auto-appended) |
| `tests/` | input scripts for verification |
| `generated/` | recompiler output (git-ignored, rebuilt automatically) |

## Windows (later)

All the code is portable C11 plus SDL3 and Python. The plan is to install
SDL3 (e.g. via vcpkg), then run the same CMake commands with MSVC or
clang-cl. This hasn't been tested yet.
