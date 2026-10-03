/*
 * test_input.c - checks controller handling using SDL's virtual (software)
 * controllers: detection at startup, button mapping, stick deadzone,
 * hot-plugging, and unplugging while in use.
 *
 * Build and run:  cmake --build build --target test_input && ./build/test_input
 */
#include <SDL3/SDL.h>
#include <stdio.h>

#include "input.h"
#include "nes.h"

uint8_t pad_state[2]; /* normally defined in bus.c */

static int failures;
#define CHECK(cond, what)                                                       \
    do {                                                                        \
        if (cond) printf("  ok    %s\n", what);                                 \
        else { printf("  FAIL  %s  (line %d)\n", what, __LINE__); failures++; } \
    } while (0)

static SDL_JoystickID attach(const char *name, SDL_Joystick **handle) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.name = name;
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    *handle = SDL_OpenJoystick(id);
    return id;
}

static void pump(void) {
    SDL_UpdateJoysticks();
    SDL_Event e;
    while (SDL_PollEvent(&e)) input_handle_event(&e);
}

static uint8_t press(SDL_Joystick *j, SDL_GamepadButton b) {
    SDL_SetJoystickVirtualButton(j, b, true);
    pump();
    uint8_t r = input_read();
    SDL_SetJoystickVirtualButton(j, b, false);
    pump();
    return r;
}

static uint8_t stick(SDL_Joystick *j, SDL_GamepadAxis axis, int16_t value) {
    SDL_SetJoystickVirtualAxis(j, axis, value);
    pump();
    uint8_t r = input_read();
    SDL_SetJoystickVirtualAxis(j, axis, 0);
    pump();
    return r;
}

int main(void) {
    if (!SDL_Init(SDL_INIT_GAMEPAD)) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }

    printf("no controller:\n");
    input_init();
    pump();
    CHECK(input_controller_count() == 0, "nothing detected");
    CHECK(input_read() == 0, "no buttons pressed");
    input_shutdown();

    printf("controller plugged in before the game starts:\n");
    SDL_Joystick *j1, *j2;
    SDL_JoystickID id1 = attach("Virtual Xbox Controller", &j1);
    input_init();
    pump();
    CHECK(input_controller_count() == 1, "detected at startup (only once)");
    CHECK(press(j1, SDL_GAMEPAD_BUTTON_SOUTH) == BTN_B, "Xbox A / PS Cross -> NES B");
    CHECK(press(j1, SDL_GAMEPAD_BUTTON_EAST) == BTN_A, "Xbox B / PS Circle -> NES A");
    CHECK(press(j1, SDL_GAMEPAD_BUTTON_WEST) == BTN_B, "Xbox X / PS Square -> NES B");
    CHECK(press(j1, SDL_GAMEPAD_BUTTON_NORTH) == BTN_A, "Xbox Y / PS Triangle -> NES A");
    CHECK(press(j1, SDL_GAMEPAD_BUTTON_START) == BTN_START, "Menu / Options -> Start");
    CHECK(press(j1, SDL_GAMEPAD_BUTTON_BACK) == BTN_SELECT, "View / Share -> Select");
    CHECK(press(j1, SDL_GAMEPAD_BUTTON_DPAD_UP) == BTN_UP, "D-pad up");
    CHECK(press(j1, SDL_GAMEPAD_BUTTON_DPAD_LEFT) == BTN_LEFT, "D-pad left");
    CHECK(stick(j1, SDL_GAMEPAD_AXIS_LEFTX, 32767) == BTN_RIGHT, "left stick right");
    CHECK(stick(j1, SDL_GAMEPAD_AXIS_LEFTY, 32767) == BTN_DOWN, "left stick down");
    CHECK(stick(j1, SDL_GAMEPAD_AXIS_LEFTX, 6000) == 0, "small stick drift ignored (deadzone)");

    printf("second controller hot-plugged:\n");
    SDL_JoystickID id2 = attach("Virtual DualSense", &j2);
    pump();
    CHECK(input_controller_count() == 2, "detected while running");
    SDL_SetJoystickVirtualButton(j1, SDL_GAMEPAD_BUTTON_EAST, true);
    SDL_SetJoystickVirtualButton(j2, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    pump();
    CHECK(input_read() == (BTN_A | BTN_RIGHT), "both controllers drive player 1");
    SDL_SetJoystickVirtualButton(j2, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    pump();
    CHECK(!(input_read() & (BTN_LEFT | BTN_RIGHT)), "left+right at once is filtered out");
    SDL_SetJoystickVirtualButton(j1, SDL_GAMEPAD_BUTTON_EAST, false);
    SDL_SetJoystickVirtualButton(j2, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false);
    SDL_SetJoystickVirtualButton(j2, SDL_GAMEPAD_BUTTON_DPAD_LEFT, false);
    pump();

    printf("unplugging while playing:\n");
    SDL_CloseJoystick(j1);
    SDL_DetachVirtualJoystick(id1);
    pump();
    CHECK(input_controller_count() == 1, "first controller removed");
    CHECK(press(j2, SDL_GAMEPAD_BUTTON_SOUTH) == BTN_B, "remaining controller still works");
    SDL_CloseJoystick(j2);
    SDL_DetachVirtualJoystick(id2);
    pump();
    CHECK(input_controller_count() == 0, "all controllers removed");
    CHECK(input_read() == 0, "reading input after unplug is safe");

    input_shutdown();
    SDL_Quit();
    printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures != 0;
}
