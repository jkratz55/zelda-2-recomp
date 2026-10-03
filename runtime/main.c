/*
 * main.c - SDL3 frontend: window, audio, keyboard/gamepad input, battery
 * saves, save states, plus a headless mode used for testing.
 *
 *   zelda2 [options] [rom.nes]
 *
 *   --interpret          run the game in the 6502 interpreter only
 *   --headless N         run N frames without a window and exit
 *   --input FILE         scripted input for headless runs ("start end A+RIGHT")
 *   --shot FRAME:FILE    save a PNG screenshot at FRAME (headless, repeatable)
 *   --hash-log FILE      write per-frame RAM/picture checksums (headless)
 *   --no-discover        don't record newly discovered code addresses
 *   --no-mods            disable everything in mods/mods.c
 *   --scale N            initial window scale (default 3)
 *
 * Keys: arrows = D-pad, X = A, Z = B, Enter = Start, Right Shift = Select,
 *       F5 = save state, F9 = load state, F11 = fullscreen, P = pause,
 *       Tab (hold) = fast forward, F12 = reset, Esc = quit.
 * Xbox / PlayStation / other controllers work too, plugged in or unplugged
 * at any time - see input.c for the button mapping.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "input.h"
#include "nes.h"

bool png_write(const char *path, const uint32_t *argb, int w, int h);

#ifndef ZELDA2_PROJECT_DIR
#define ZELDA2_PROJECT_DIR "."
#endif

static char sram_path[1024], state_path[1024];
bool mods_enabled = true;

/* ------------------------------------------------------------ Helpers */

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static const char *find_rom(void) {
#ifdef ZELDA2_DEFAULT_ROM
    if (file_exists(ZELDA2_DEFAULT_ROM)) return ZELDA2_DEFAULT_ROM;
#endif
    static char found[1024];
    const char *dirs[] = {".", "rom", ZELDA2_PROJECT_DIR, ZELDA2_PROJECT_DIR "/rom"};
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        int n = 0;
        char **list = SDL_GlobDirectory(dirs[d], "*.nes", 0, &n);
        if (list && n > 0) {
            snprintf(found, sizeof found, "%s/%s", dirs[d], list[0]);
            SDL_free(list);
            return found;
        }
        SDL_free(list);
    }
    return NULL;
}

static void load_sram(void) {
    FILE *f = fopen(sram_path, "rb");
    if (!f) return;
    if (fread(cart.sram, 1, sizeof cart.sram, f) != sizeof cart.sram)
        fprintf(stderr, "warning: save file %s is short\n", sram_path);
    fclose(f);
}

static void save_sram(void) {
    FILE *f = fopen(sram_path, "wb");
    if (!f) { fprintf(stderr, "warning: cannot write %s\n", sram_path); return; }
    fwrite(cart.sram, 1, sizeof cart.sram, f);
    fclose(f);
    cart.sram_dirty = false;
}

static uint8_t parse_buttons(const char *s) {
    static const struct { const char *name; uint8_t bit; } map[] = {
        {"A", BTN_A}, {"B", BTN_B}, {"SELECT", BTN_SELECT}, {"START", BTN_START},
        {"UP", BTN_UP}, {"DOWN", BTN_DOWN}, {"LEFT", BTN_LEFT}, {"RIGHT", BTN_RIGHT},
    };
    uint8_t r = 0;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+"))
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (!SDL_strcasecmp(tok, map[i].name)) r |= map[i].bit;
    return r;
}

/* Scripted input: lines of "first_frame last_frame BUTTONS". */
typedef struct { long first, last; uint8_t buttons; } InputSpan;
static InputSpan *spans;
static size_t span_count;

static bool load_input_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        long a, b;
        char btn[128];
        if (line[0] == '#') continue;
        if (sscanf(line, "%ld %ld %127s", &a, &b, btn) == 3) {
            spans = realloc(spans, (span_count + 1) * sizeof *spans);
            spans[span_count++] = (InputSpan){a, b, parse_buttons(btn)};
        }
    }
    fclose(f);
    return true;
}

static uint8_t scripted_buttons(long frame) {
    uint8_t r = 0;
    for (size_t i = 0; i < span_count; i++)
        if (frame >= spans[i].first && frame <= spans[i].last) r |= spans[i].buttons;
    return r;
}

/* --------------------------------------------------------- Headless */

typedef struct { long frame; const char *path; } Shot;

static int run_headless(long frames, Shot *shots, int nshots, const char *hash_log) {
    FILE *hl = hash_log ? fopen(hash_log, "w") : NULL;
    uint64_t t0 = SDL_GetTicksNS();
    for (long fr = 0; fr < frames; fr++) {
        pad_state[0] = scripted_buttons(fr);
        if (mods_enabled) mods_frame();
        nes_run_frame();
        int16_t drain[4096];
        while (apu_take_samples(drain, 4096)) {}
        if (hl) {
            fprintf(hl, "%ld %08X %08X", fr, crc32_buf(0, ram, sizeof ram),
                    crc32_buf(0, (const uint8_t *)ppu_framebuffer, sizeof ppu_framebuffer));
            if (getenv("ZELDA2_RAM_LOG"))
                for (int i = 0; i < 0x800; i++) fprintf(hl, "%s%02X", i % 64 ? "" : " ", ram[i]);
            fputc('\n', hl);
        }
        for (int i = 0; i < nshots; i++)
            if (shots[i].frame == fr && !png_write(shots[i].path, ppu_framebuffer, 256, 240))
                fprintf(stderr, "failed to write %s\n", shots[i].path);
    }
    if (hl) fclose(hl);
    double secs = (double)(SDL_GetTicksNS() - t0) / 1e9;
    printf("ran %ld frames in %.2fs (%.0f fps); recompiled calls: %llu, interpreted instructions: %llu\n",
           frames, secs, frames / secs, (unsigned long long)run_stats.recomp_calls,
           (unsigned long long)run_stats.interp_instructions);
    if (cart.sram_dirty) save_sram();
    return 0;
}

/* ----------------------------------------------------------- Windowed */

static int run_window(int scale) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("Zelda II (recompiled)", 256 * scale, 240 * scale, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    if (!ren) {
        fprintf(stderr, "could not create window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderLogicalPresentation(ren, 256, 240, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 240);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

    SDL_AudioSpec spec = {SDL_AUDIO_S16, 1, APU_SAMPLE_RATE};
    SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio) SDL_ResumeAudioStreamDevice(audio);
    else fprintf(stderr, "warning: no audio (%s)\n", SDL_GetError());

    input_init();
    bool running = true, paused = false, fullscreen = false;
    const uint64_t frame_ns = 16639267; /* 1 / 60.0988 Hz */
    uint64_t next_frame = SDL_GetTicksNS(), last_sram_check = 0, title_time = SDL_GetTicksNS();
    int frames_this_second = 0;
    uint64_t interp_at_title = run_stats.interp_instructions;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            input_handle_event(&e);
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                switch (e.key.scancode) {
                case SDL_SCANCODE_ESCAPE: running = false; break;
                case SDL_SCANCODE_P: paused = !paused; break;
                case SDL_SCANCODE_F11:
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(win, fullscreen);
                    break;
                case SDL_SCANCODE_F5:
                    printf(nes_save_state(state_path) ? "state saved to %s\n" : "could not save %s\n", state_path);
                    break;
                case SDL_SCANCODE_F9:
                    printf(nes_load_state(state_path) ? "state loaded from %s\n" : "could not load %s\n", state_path);
                    break;
                case SDL_SCANCODE_F12: nes_power_on(); break;
                default: break;
                }
            }
        }

        bool turbo = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_TAB];
        int frames_to_run = 0;
        uint64_t now = SDL_GetTicksNS();
        if (paused) {
            next_frame = now;
        } else if (turbo) {
            frames_to_run = 4;
            next_frame = now;
        } else if (audio) {
            /* audio is the master clock: keep ~50ms queued */
            int queued = SDL_GetAudioStreamQueued(audio) / 2;
            if (queued < APU_SAMPLE_RATE / 20) frames_to_run = queued < APU_SAMPLE_RATE / 100 ? 2 : 1;
        } else if (now >= next_frame) {
            frames_to_run = 1;
            next_frame += frame_ns;
            if (now > next_frame + frame_ns * 4) next_frame = now;
        }

        for (int i = 0; i < frames_to_run; i++) {
            pad_state[0] = input_read();
            if (mods_enabled) mods_frame();
            nes_run_frame();
            frames_this_second++;
            int16_t buf[4096];
            size_t n;
            while ((n = apu_take_samples(buf, 4096)) > 0)
                if (audio && !turbo) SDL_PutAudioStreamData(audio, buf, (int)(n * 2));
        }

        if (frames_to_run) {
            SDL_UpdateTexture(tex, NULL, ppu_framebuffer, 256 * 4);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            SDL_RenderTexture(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
        } else {
            SDL_DelayNS(1000000);
        }

        if (now - title_time >= 1000000000ull) {
            char title[160];
            snprintf(title, sizeof title, "Zelda II (recompiled) - %d fps - interpreted: %llu instr/s%s",
                     frames_this_second,
                     (unsigned long long)(run_stats.interp_instructions - interp_at_title),
                     paused ? " - PAUSED" : "");
            SDL_SetWindowTitle(win, title);
            frames_this_second = 0;
            interp_at_title = run_stats.interp_instructions;
            title_time = now;
        }
        if (cart.sram_dirty && now - last_sram_check > 2000000000ull) {
            save_sram();
            last_sram_check = now;
        }
    }

    if (cart.sram_dirty) save_sram();
    input_shutdown();
    if (audio) SDL_DestroyAudioStream(audio);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv) {
    const char *rom_path = NULL, *hash_log = NULL;
    long headless_frames = -1;
    int scale = 3;
    bool discover = true;
    Shot shots[64];
    int nshots = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if (!strcmp(a, "--interpret")) run_stats.interpret_only = true;
        else if (!strcmp(a, "--headless") && more) headless_frames = atol(argv[++i]);
        else if (!strcmp(a, "--input") && more) {
            if (!load_input_script(argv[++i])) { fprintf(stderr, "cannot read %s\n", argv[i]); return 1; }
        } else if (!strcmp(a, "--shot") && more && nshots < 64) {
            char *spec = argv[++i], *colon = strchr(spec, ':');
            if (!colon) { fprintf(stderr, "--shot expects FRAME:FILE\n"); return 1; }
            *colon = 0;
            shots[nshots++] = (Shot){atol(spec), colon + 1};
        } else if (!strcmp(a, "--hash-log") && more) hash_log = argv[++i];
        else if (!strcmp(a, "--no-discover")) discover = false;
        else if (!strcmp(a, "--no-mods")) mods_enabled = false;
        else if (!strcmp(a, "--scale") && more) scale = atoi(argv[++i]);
        else if (a[0] == '-') { fprintf(stderr, "unknown option %s (see runtime/main.c)\n", a); return 1; }
        else rom_path = a;
    }

    if (!rom_path) rom_path = find_rom();
    if (!rom_path) {
        fprintf(stderr, "No ROM found. Pass the path to your Zelda II .nes dump as an argument.\n");
        return 1;
    }
    char err[512];
    if (!cart_load(rom_path, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }

    char save_dir[900];
    snprintf(save_dir, sizeof save_dir, "%s/saves", ZELDA2_PROJECT_DIR);
    SDL_CreateDirectory(save_dir);
    snprintf(sram_path, sizeof sram_path, "%s/zelda2.sav", save_dir);
    snprintf(state_path, sizeof state_path, "%s/zelda2.state", save_dir);
    if (discover && !run_stats.interpret_only)
        run_stats.miss_log = ZELDA2_PROJECT_DIR "/config/discovered.txt";

    dispatch_init();
    if (mods_enabled) mods_init();
    nes_power_on();
    if (headless_frames >= 0) {
        /* headless runs start from blank save RAM so tests are reproducible */
        snprintf(sram_path, sizeof sram_path, "%s/headless.sav", save_dir);
        return run_headless(headless_frames, shots, nshots, hash_log);
    }
    load_sram();
    return run_window(scale);
}
