#ifndef FALLOUT_PLIB_GNW_DXINPUT_H_
#define FALLOUT_PLIB_GNW_DXINPUT_H_

#include <SDL.h>

namespace fallout {

#ifdef NXDK
typedef struct ControllerState {
    float analogX;
    float analogY;
    bool buttonA;
    bool buttonB;
} ControllerState;

typedef struct ControllerKeyMapping {
    SDL_GameControllerButton button;
    SDL_Scancode scancode;
} ControllerKeyMapping;

// Default controller button to keyboard mappings
extern const ControllerKeyMapping CONTROLLER_KEY_MAPPINGS[];
extern const int CONTROLLER_KEY_MAPPING_COUNT;
#endif

typedef struct MouseData {
    int x;
    int y;
    unsigned char buttons[2];
    int wheelX;
    int wheelY;
} MouseData;

typedef struct KeyboardData {
    int key;
    unsigned char down;
} KeyboardData;

bool dxinput_init();
void dxinput_exit();
bool dxinput_acquire_mouse();
bool dxinput_unacquire_mouse();
bool dxinput_get_mouse_state(MouseData* mouseData);
bool dxinput_acquire_keyboard();
bool dxinput_unacquire_keyboard();
bool dxinput_flush_keyboard_buffer();
bool dxinput_read_keyboard_buffer(KeyboardData* keyboardData);

void handleMouseEvent(SDL_Event* event);

#ifdef NXDK
bool dxinput_get_controller_state(ControllerState* state);
#endif

} // namespace fallout

#endif /* FALLOUT_PLIB_GNW_DXINPUT_H_ */
