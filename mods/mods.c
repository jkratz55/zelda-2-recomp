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
 *   $0775 / $0776  experience (high / low byte)
 *   $0755 / $0756  experience still to be added (counts into $0775/$0776)
 *   $C2+X  HP of the enemy in slot X
 *
 * Run with --no-mods to disable everything here (tools/verify.sh does).
 */
#include "recomp.h"
#include "recomp_funcs.h"

/* Turn the example mods on (1) or off (0). */
#define ENABLE_HALF_DAMAGE    1
#define ENABLE_INFINITE_MAGIC 0
#define ENABLE_FULL_HEALTH_DOUBLE_DAMAGE 1
#define ENABLE_DOUBLE_EXPERIENCE 1

/* Example routine hook: Link takes half damage (rounded up). */
static void half_damage(void) {
    cpu.a = (uint8_t)((cpu.a + 1) / 2);
    LinkTakeDamage_Apply();
}

/* Routine hook: Link's sword does double damage while his life is full.
 *
 * At 07:E72F the game is about to do "enemy HP -= sword damage", where the
 * damage is a table entry at $E66C indexed by Link's attack level (cpu.y).
 * We subtract that amount once more beforehand, so the original code ends up
 * subtracting it twice. If that extra hit alone would finish the enemy, we
 * leave it at 0 HP and the original subtraction still sees it die.
 */
static void full_health_double_damage(void) {
    uint8_t full = (uint8_t)(ram[0x0784] * 0x20 - 1); /* full life for current containers */
    if (ram[0x0774] >= full) {
        uint8_t damage = rd((uint16_t)(0xE66C + cpu.y));
        uint8_t *hp = &ram[(uint8_t)(0xC2 + cpu.x)];
        *hp = *hp > damage ? (uint8_t)(*hp - damage) : 0;
    }
    EnemyTakeSwordDamage();
}

/* Data patch: every experience reward is doubled.
 *
 * All enemy and P-bag EXP comes from two ROM tables in the fixed bank, read
 * at 07:DDF2 (GiveEnemyExperience): low bytes at $DDC0 and high bytes at
 * $DDDC, 16 entries each. The game reads them from cart.prg while it runs,
 * so doubling the numbers once at startup is enough - no hook needed.
 * (The floating number shown when an enemy dies still shows the old value.)
 */
static void double_experience(void) {
    uint8_t *lo = &cart.prg[7 * 0x4000 + (0xDDC0 & 0x3FFF)];
    uint8_t *hi = &cart.prg[7 * 0x4000 + (0xDDDC & 0x3FFF)];
    for (int i = 0; i < 16; i++) {
        unsigned exp = (unsigned)(lo[i] | hi[i] << 8) * 2;
        lo[i] = (uint8_t)exp;
        hi[i] = (uint8_t)(exp >> 8);
    }
}

void mods_init(void) {
    if (ENABLE_HALF_DAMAGE) recomp_override(7, 0xE32F, half_damage);
    if (ENABLE_FULL_HEALTH_DOUBLE_DAMAGE) recomp_override(7, 0xE72F, full_health_double_damage);
    if (ENABLE_DOUBLE_EXPERIENCE) double_experience();
}

void mods_frame(void) {
    if (ENABLE_INFINITE_MAGIC) {
        uint8_t full = (uint8_t)(ram[0x0783] * 0x20 - 1); /* full meter for current containers */
        if (ram[0x0773] < full) ram[0x0773] = full;
    }
}
