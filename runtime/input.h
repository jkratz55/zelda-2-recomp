/* input.h - keyboard + game controller input (see input.c). */
#pragma once
#include <SDL3/SDL.h>
#include <stdint.h>

void input_init(void);                       /* call after SDL_Init(SDL_INIT_GAMEPAD) */
void input_handle_event(const SDL_Event *e); /* pass every SDL event through here */
uint8_t input_read(void);                    /* NES button bits (BTN_*) for player 1 */
int input_controller_count(void);
void input_shutdown(void);
