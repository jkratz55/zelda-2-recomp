/*
 * input.c - keyboard + game controller input.
 *
 * Any number of controllers (Xbox, PlayStation, Switch Pro, 8BitDo, ...) can
 * be connected or unplugged at any time. Every connected controller and the
 * keyboard all drive player 1 together, so the keyboard keeps working
 * whether or not a controller is present.
 *
 * SDL names controller buttons by position, so the same mapping works for
 * every brand:
 *
 *   NES        Xbox            PlayStation       keyboard
 *   A (jump)   B  or Y         Circle or Triangle   X
 *   B (attack) A  or X         Cross  or Square     Z
 *   Start      Menu            Options              Enter
 *   Select     View            Create / Share       Right Shift
 *   D-pad      D-pad or left stick                  Arrow keys
 *
 * The default (bottom = B, right = A) matches the layout of the original NES
 * pad, where B sits to the left of A. To change it, edit pad_map below.
 *
 * For unusual controllers SDL doesn't recognise, drop a gamecontrollerdb.txt
 * (https://github.com/mdqinc/SDL_GameControllerDB) into the project folder.
 */
#include <SDL3/SDL.h>
#include <stdio.h>

#include "input.h"
#include "nes.h"

#ifndef ZELDA2_PROJECT_DIR
#define ZELDA2_PROJECT_DIR "."
#endif

static const struct { SDL_GamepadButton button; uint8_t nes; } pad_map[] = {
    {SDL_GAMEPAD_BUTTON_EAST, BTN_A},       /* Xbox B / PS Circle   */
    {SDL_GAMEPAD_BUTTON_NORTH, BTN_A},      /* Xbox Y / PS Triangle */
    {SDL_GAMEPAD_BUTTON_SOUTH, BTN_B},      /* Xbox A / PS Cross    */
    {SDL_GAMEPAD_BUTTON_WEST, BTN_B},       /* Xbox X / PS Square   */
    {SDL_GAMEPAD_BUTTON_START, BTN_START},
    {SDL_GAMEPAD_BUTTON_BACK, BTN_SELECT},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, BTN_UP},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, BTN_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, BTN_LEFT},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, BTN_RIGHT},
};

static const struct { SDL_Scancode key; uint8_t nes; } key_map[] = {
    {SDL_SCANCODE_X, BTN_A},
    {SDL_SCANCODE_Z, BTN_B},
    {SDL_SCANCODE_RETURN, BTN_START},
    {SDL_SCANCODE_RSHIFT, BTN_SELECT},
    {SDL_SCANCODE_BACKSPACE, BTN_SELECT},
    {SDL_SCANCODE_UP, BTN_UP},
    {SDL_SCANCODE_DOWN, BTN_DOWN},
    {SDL_SCANCODE_LEFT, BTN_LEFT},
    {SDL_SCANCODE_RIGHT, BTN_RIGHT},
};

#define STICK_DEADZONE 12000 /* of 32767: how far the stick must move to count */
#define MAX_PADS 8

static SDL_Gamepad *pads[MAX_PADS];

static void open_pad(SDL_JoystickID id) {
    if (!SDL_IsGamepad(id)) return;
    int free_slot = -1;
    for (int i = 0; i < MAX_PADS; i++) {
        if (pads[i] && SDL_GetGamepadID(pads[i]) == id) return; /* already open */
        if (!pads[i] && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return;
    SDL_Gamepad *pad = SDL_OpenGamepad(id);
    if (!pad) {
        fprintf(stderr, "could not open controller: %s\n", SDL_GetError());
        return;
    }
    pads[free_slot] = pad;
    const char *name = SDL_GetGamepadName(pad);
    printf("Controller connected: %s\n", name ? name : "(unknown)");
}

static void close_pad(SDL_JoystickID id) {
    for (int i = 0; i < MAX_PADS; i++) {
        if (pads[i] && SDL_GetGamepadID(pads[i]) == id) {
            const char *name = SDL_GetGamepadName(pads[i]);
            printf("Controller disconnected: %s\n", name ? name : "(unknown)");
            SDL_CloseGamepad(pads[i]);
            pads[i] = NULL;
        }
    }
}

void input_init(void) {
    char db[1024];
    snprintf(db, sizeof db, "%s/gamecontrollerdb.txt", ZELDA2_PROJECT_DIR);
    if (SDL_GetPathInfo(db, NULL)) {
        int n = SDL_AddGamepadMappingsFromFile(db);
        if (n >= 0) printf("Loaded %d extra controller mappings from %s\n", n, db);
    }
    /* Controllers already plugged in at startup. (SDL also sends "added"
       events for them; open_pad ignores duplicates.) */
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; i++) open_pad(ids[i]);
    SDL_free(ids);
}

void input_handle_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_GAMEPAD_ADDED) open_pad(e->gdevice.which);
    else if (e->type == SDL_EVENT_GAMEPAD_REMOVED) close_pad(e->gdevice.which);
}

int input_controller_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_PADS; i++) n += pads[i] != NULL;
    return n;
}

uint8_t input_read(void) {
    uint8_t b = 0;

    const bool *keys = SDL_GetKeyboardState(NULL);
    if (keys)
        for (size_t i = 0; i < SDL_arraysize(key_map); i++)
            if (keys[key_map[i].key]) b |= key_map[i].nes;

    for (int p = 0; p < MAX_PADS; p++) {
        SDL_Gamepad *pad = pads[p];
        if (!pad) continue;
        for (size_t i = 0; i < SDL_arraysize(pad_map); i++)
            if (SDL_GetGamepadButton(pad, pad_map[i].button)) b |= pad_map[i].nes;
        int lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        int ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (lx < -STICK_DEADZONE) b |= BTN_LEFT;
        if (lx > STICK_DEADZONE) b |= BTN_RIGHT;
        if (ly < -STICK_DEADZONE) b |= BTN_UP;
        if (ly > STICK_DEADZONE) b |= BTN_DOWN;
    }

    /* A real NES pad can't press opposite directions; the game misbehaves if
       it sees them (easy to do with keyboard + controller together). */
    if ((b & (BTN_LEFT | BTN_RIGHT)) == (BTN_LEFT | BTN_RIGHT)) b &= (uint8_t)~(BTN_LEFT | BTN_RIGHT);
    if ((b & (BTN_UP | BTN_DOWN)) == (BTN_UP | BTN_DOWN)) b &= (uint8_t)~(BTN_UP | BTN_DOWN);
    return b;
}

void input_shutdown(void) {
    for (int i = 0; i < MAX_PADS; i++) {
        if (pads[i]) SDL_CloseGamepad(pads[i]);
        pads[i] = NULL;
    }
}
