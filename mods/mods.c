/*
 * mods.c - your gameplay changes live here.
 *
 * There are two kinds of hooks:
 *
 * 1. Routine hooks - run your C code whenever the game reaches an address.
 *
 *        static void my_hook(void) {
 *            ...                     // look at / change cpu.a, ram[], ...
 *            LinkTakeDamage_Apply(); // then run the original code
 *        }
 *        recomp_override(7, 0xE32F, my_hook);   // in mods_init()
 *
 *    The address must be an entry point the recompiler knows about. To hook
 *    any instruction, add it to config/entries.txt (or give it a name in
 *    config/labels.txt, which also turns it into a C function name) and
 *    rebuild. Your hook either calls the original function, or does the
 *    whole job itself and sets cpu.pc to the next address to run (an RTS is
 *    `cpu.pc = (uint16_t)(pull16() + 1);`).
 *
 * 2. Frame hooks - mods_frame() runs once per frame, before the game's
 *    frame. Handy for "cheat"-style RAM tweaks.
 *
 * Known RAM (see also config/labels.txt):
 *   $0773 magic (0x7F = 4 full containers, 0x20 per container)
 *   $0774 life  (same scale)
 *   $0777 / $0778 / $0779  attack / magic / life level
 *   $0783 / $0784  magic / heart containers
 *   $0518 Link's invincibility timer after being hit
 *
 * Run with --no-mods to disable everything here (tools/verify.sh does).
 */
#include "recomp.h"
#include "recomp_funcs.h"

/* Turn the example mods on (1) or off (0). */
#define ENABLE_HALF_DAMAGE    0
#define ENABLE_INFINITE_MAGIC 0

/* Example routine hook: Link takes half damage (rounded up). */
static void half_damage(void) {
    cpu.a = (uint8_t)((cpu.a + 1) / 2);
    LinkTakeDamage_Apply();
}

void mods_init(void) {
    if (ENABLE_HALF_DAMAGE) recomp_override(7, 0xE32F, half_damage);
}

void mods_frame(void) {
    if (ENABLE_INFINITE_MAGIC) {
        uint8_t full = (uint8_t)(ram[0x0783] * 0x20 - 1); /* full meter for current containers */
        if (ram[0x0773] < full) ram[0x0773] = full;
    }
}
