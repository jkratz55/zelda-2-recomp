/*
 * apu.c - the NES audio processing unit: two pulse channels, a triangle,
 * a noise channel and the DMC sample channel. Stepped once per CPU cycle
 * (lazily, like the PPU) and resampled to APU_SAMPLE_RATE.
 */
#include <math.h>
#include <string.h>

#include "nes.h"

Apu apu;

#define CPU_HZ 1789773.0

static const uint8_t length_table[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};
static const uint8_t duty_table[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0}, {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0}, {1, 0, 0, 1, 1, 1, 1, 1},
};
static const uint8_t tri_table[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};
static const uint16_t noise_table[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068,
};
static const uint16_t dmc_table[16] = {
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54,
};

static float pulse_mix[31], tnd_mix[203];

#define RING_SIZE 16384
static int16_t ring[RING_SIZE];
static size_t ring_head, ring_tail; /* not part of save states */

void apu_reset(void) {
    memset(&apu, 0, sizeof apu);
    apu.p1.ones_comp = 1;
    apu.noise.lfsr = 1;
    apu.noise.period = noise_table[0];
    apu.dmc.rate = dmc_table[0];
    apu.dmc.buffer_empty = true;
    apu.dmc.bits = 8;
    for (int i = 0; i < 31; i++) pulse_mix[i] = i ? 95.52f / (8128.0f / i + 100.0f) : 0;
    for (int i = 0; i < 203; i++) tnd_mix[i] = i ? 163.67f / (24329.0f / i + 100.0f) : 0;
}

/* --------------------------------------------------------- Units */

static void env_clock(bool *start, uint8_t *div, uint8_t *decay, uint8_t period, bool loop) {
    if (*start) { *start = false; *decay = 15; *div = period; return; }
    if (*div) { (*div)--; return; }
    *div = period;
    if (*decay) (*decay)--;
    else if (loop) *decay = 15;
}

static uint16_t sweep_target(const ApuPulse *p) {
    uint16_t change = p->period >> p->sweep_shift;
    if (p->sweep_neg) {
        int t = p->period - change - p->ones_comp;
        return (uint16_t)(t < 0 ? 0 : t);
    }
    return (uint16_t)(p->period + change);
}

static bool pulse_muted(const ApuPulse *p) { return p->period < 8 || sweep_target(p) > 0x7FF; }

static void sweep_clock(ApuPulse *p) {
    if (p->sweep_div == 0 && p->sweep_en && p->sweep_shift && !pulse_muted(p))
        p->period = sweep_target(p);
    if (p->sweep_div == 0 || p->sweep_reload) { p->sweep_div = p->sweep_period; p->sweep_reload = false; }
    else p->sweep_div--;
}

static void quarter_frame(void) {
    env_clock(&apu.p1.env_start, &apu.p1.env_div, &apu.p1.env_decay, apu.p1.env_period, apu.p1.halt);
    env_clock(&apu.p2.env_start, &apu.p2.env_div, &apu.p2.env_decay, apu.p2.env_period, apu.p2.halt);
    env_clock(&apu.noise.env_start, &apu.noise.env_div, &apu.noise.env_decay, apu.noise.env_period, apu.noise.halt);
    ApuTriangle *t = &apu.tri;
    if (t->linear_reload) t->linear = t->linear_period;
    else if (t->linear) t->linear--;
    if (!t->control) t->linear_reload = false;
}

static void half_frame(void) {
    if (!apu.p1.halt && apu.p1.length) apu.p1.length--;
    if (!apu.p2.halt && apu.p2.length) apu.p2.length--;
    if (!apu.tri.control && apu.tri.length) apu.tri.length--;
    if (!apu.noise.halt && apu.noise.length) apu.noise.length--;
    sweep_clock(&apu.p1);
    sweep_clock(&apu.p2);
}

static void frame_counter_step(void) {
    apu.frame_cycle++;
    uint32_t c = apu.frame_cycle;
    if (apu.frame_mode == 0) {
        if (c == 7457 || c == 22371) quarter_frame();
        else if (c == 14913) { quarter_frame(); half_frame(); }
        else if (c == 29829) {
            quarter_frame(); half_frame();
            if (!apu.irq_inhibit) apu.frame_irq = true;
        } else if (c >= 29830) apu.frame_cycle = 0;
    } else {
        if (c == 7457 || c == 22371) quarter_frame();
        else if (c == 14913 || c == 37281) { quarter_frame(); half_frame(); }
        else if (c >= 37282) apu.frame_cycle = 0;
    }
}

static void pulse_timer(ApuPulse *p) {
    if (p->timer == 0) { p->timer = p->period; p->seq = (p->seq + 1) & 7; }
    else p->timer--;
}

static void dmc_step(void) {
    ApuDmc *d = &apu.dmc;
    if (d->buffer_empty && d->remaining) {
        d->buffer = rd(d->cur_addr);
        d->buffer_empty = false;
        d->cur_addr = d->cur_addr == 0xFFFF ? 0x8000 : (uint16_t)(d->cur_addr + 1);
        if (--d->remaining == 0) {
            if (d->loop) { d->cur_addr = d->addr; d->remaining = d->len; }
            else if (d->irq_en) d->irq = true;
        }
    }
    if (d->timer) { d->timer--; return; }
    d->timer = (uint16_t)(d->rate - 1);
    if (!d->silence) {
        if (d->shift & 1) { if (d->output <= 125) d->output += 2; }
        else if (d->output >= 2) d->output -= 2;
    }
    d->shift >>= 1;
    if (--d->bits == 0) {
        d->bits = 8;
        if (d->buffer_empty) d->silence = true;
        else { d->silence = false; d->shift = d->buffer; d->buffer_empty = true; }
    }
}

static inline uint8_t pulse_out(const ApuPulse *p) {
    if (!p->length || pulse_muted(p) || !duty_table[p->duty][p->seq]) return 0;
    return p->env_const ? p->env_period : p->env_decay;
}

static void apu_cycle(void) {
    frame_counter_step();

    /* triangle and DMC/noise timers run at CPU rate, pulses at half rate */
    ApuTriangle *t = &apu.tri;
    if (t->timer == 0) {
        t->timer = t->period;
        if (t->length && t->linear) t->seq = (t->seq + 1) & 31;
    } else t->timer--;

    ApuNoise *n = &apu.noise;
    if (n->timer == 0) {
        n->timer = (uint16_t)(n->period - 1);
        uint16_t fb = (n->lfsr & 1) ^ ((n->lfsr >> (n->mode ? 6 : 1)) & 1);
        n->lfsr = (uint16_t)((n->lfsr >> 1) | (fb << 14));
    } else n->timer--;

    if (apu.cycle & 1) { pulse_timer(&apu.p1); pulse_timer(&apu.p2); }
    dmc_step();

    /* mix */
    uint8_t p = (uint8_t)(pulse_out(&apu.p1) + pulse_out(&apu.p2));
    uint8_t tv = t->period >= 2 ? tri_table[t->seq] : 7; /* ultrasonic -> mid level */
    uint8_t nv = (n->length && !(n->lfsr & 1)) ? (n->env_const ? n->env_period : n->env_decay) : 0;
    float s = pulse_mix[p] + tnd_mix[3 * tv + 2 * nv + apu.dmc.output];
    apu.sample_sum += s;
    apu.sample_n++;

    apu.sample_acc += APU_SAMPLE_RATE;
    if (apu.sample_acc >= CPU_HZ) {
        apu.sample_acc -= CPU_HZ;
        float x = (float)(apu.sample_sum / apu.sample_n);
        apu.sample_sum = 0;
        apu.sample_n = 0;
        /* ~90Hz high-pass (removes DC) then ~14kHz low-pass, like the console */
        float hp = 0.9883f * (apu.hp_prev_out + x - apu.hp_prev_in);
        apu.hp_prev_in = x;
        apu.hp_prev_out = hp;
        apu.lp_prev += (hp - apu.lp_prev) * 0.815f;
        float out = apu.lp_prev * 1.6f;
        if (out > 1) out = 1;
        if (out < -1) out = -1;
        size_t next = (ring_head + 1) % RING_SIZE;
        if (next != ring_tail) { ring[ring_head] = (int16_t)(out * 32000); ring_head = next; }
    }
}

void apu_run_to(uint64_t cycle) {
    while (apu.cycle < cycle) {
        apu.cycle++;
        apu_cycle();
    }
}

size_t apu_take_samples(int16_t *out, size_t max) {
    size_t n = 0;
    while (n < max && ring_tail != ring_head) {
        out[n++] = ring[ring_tail];
        ring_tail = (ring_tail + 1) % RING_SIZE;
    }
    return n;
}

/* ---------------------------------------------------------- Registers */

static void pulse_write(ApuPulse *p, int reg, uint8_t v) {
    switch (reg) {
    case 0:
        p->duty = v >> 6; p->halt = v & 0x20; p->env_const = v & 0x10; p->env_period = v & 0x0F;
        break;
    case 1:
        p->sweep_en = v & 0x80; p->sweep_period = (v >> 4) & 7;
        p->sweep_neg = v & 0x08; p->sweep_shift = v & 7; p->sweep_reload = true;
        break;
    case 2: p->period = (uint16_t)((p->period & 0x700) | v); break;
    case 3:
        p->period = (uint16_t)((p->period & 0xFF) | ((v & 7) << 8));
        if (p->enabled) p->length = length_table[v >> 3];
        p->seq = 0;
        p->env_start = true;
        break;
    }
}

void apu_write(uint16_t addr, uint8_t v) {
    switch (addr) {
    case 0x4000: case 0x4001: case 0x4002: case 0x4003: pulse_write(&apu.p1, addr & 3, v); break;
    case 0x4004: case 0x4005: case 0x4006: case 0x4007: pulse_write(&apu.p2, addr & 3, v); break;
    case 0x4008: apu.tri.control = v & 0x80; apu.tri.linear_period = v & 0x7F; break;
    case 0x400A: apu.tri.period = (uint16_t)((apu.tri.period & 0x700) | v); break;
    case 0x400B:
        apu.tri.period = (uint16_t)((apu.tri.period & 0xFF) | ((v & 7) << 8));
        if (apu.tri.enabled) apu.tri.length = length_table[v >> 3];
        apu.tri.linear_reload = true;
        break;
    case 0x400C:
        apu.noise.halt = v & 0x20; apu.noise.env_const = v & 0x10; apu.noise.env_period = v & 0x0F;
        break;
    case 0x400E: apu.noise.mode = v & 0x80; apu.noise.period = noise_table[v & 0x0F]; break;
    case 0x400F:
        if (apu.noise.enabled) apu.noise.length = length_table[v >> 3];
        apu.noise.env_start = true;
        break;
    case 0x4010:
        apu.dmc.irq_en = v & 0x80; apu.dmc.loop = v & 0x40; apu.dmc.rate = dmc_table[v & 0x0F];
        if (!apu.dmc.irq_en) apu.dmc.irq = false;
        break;
    case 0x4011: apu.dmc.output = v & 0x7F; break;
    case 0x4012: apu.dmc.addr = (uint16_t)(0xC000 | (v << 6)); break;
    case 0x4013: apu.dmc.len = (uint16_t)((v << 4) | 1); break;
    case 0x4015:
        apu.p1.enabled = v & 1; if (!apu.p1.enabled) apu.p1.length = 0;
        apu.p2.enabled = v & 2; if (!apu.p2.enabled) apu.p2.length = 0;
        apu.tri.enabled = v & 4; if (!apu.tri.enabled) apu.tri.length = 0;
        apu.noise.enabled = v & 8; if (!apu.noise.enabled) apu.noise.length = 0;
        apu.dmc.irq = false;
        if (!(v & 0x10)) apu.dmc.remaining = 0;
        else if (!apu.dmc.remaining) { apu.dmc.cur_addr = apu.dmc.addr; apu.dmc.remaining = apu.dmc.len; }
        break;
    case 0x4017:
        apu.frame_mode = v >> 7;
        apu.irq_inhibit = v & 0x40;
        if (apu.irq_inhibit) apu.frame_irq = false;
        apu.frame_cycle = 0;
        if (apu.frame_mode) { quarter_frame(); half_frame(); }
        nes_update_event_cycle();
        break;
    }
}

uint8_t apu_read_status(void) {
    uint8_t r = 0;
    if (apu.p1.length) r |= 1;
    if (apu.p2.length) r |= 2;
    if (apu.tri.length) r |= 4;
    if (apu.noise.length) r |= 8;
    if (apu.dmc.remaining) r |= 0x10;
    if (apu.frame_irq) r |= 0x40;
    if (apu.dmc.irq) r |= 0x80;
    apu.frame_irq = false;
    return r;
}

bool apu_irq_line(void) { return apu.frame_irq || apu.dmc.irq; }

/* Next CPU cycle at which the frame counter could raise an IRQ (or ~0). */
uint64_t apu_next_irq_cycle(void) {
    if (apu.frame_mode != 0 || apu.irq_inhibit) return UINT64_MAX;
    uint32_t c = apu.frame_cycle;
    uint32_t until = c < 29829 ? 29829 - c : 29830 - c + 29829;
    return apu.cycle + until;
}
