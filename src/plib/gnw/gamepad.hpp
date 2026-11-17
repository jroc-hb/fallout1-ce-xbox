#ifndef FALLOUT_PLIB_GNW_GAMEPAD_H_
#define FALLOUT_PLIB_GNW_GAMEPAD_H_

#include <SDL3/SDL.h>

#include "plib/gnw/rect.h"
#include "plib/gnw/dxinput.h"

namespace fallout {

extern float leftStickX, leftStickY, rightStickX, rightStickY;

bool GetGamepadMouseState(MouseData* mouseState);
void GamepadInit();
void GamepadRegisterDefaultMapping();
void HandleJoystickDeviceAdded(const SDL_Event& event);
void HandleJoystickDeviceRemoved(const SDL_Event& event);
void HandleControllerDeviceAdded(const SDL_Event& event);
void HandleControllerDeviceRemoved(const SDL_Event& event);
void HandleControllerAxisMotion(const SDL_Event& event);
void HandleControllerButtonUp(const SDL_Event& event);
void HandleControllerButtonDown(const SDL_Event& event);
void ProcessLeftStick();
void ProcessRightStick();
void ProcessTriggers();

// Rumble every pad: `low`/`high` are the big (low frequency) and small
// (high frequency) motors, 0..1, scaled by the RUMBLE setting. Overlapping
// calls keep the stronger values.
void GamepadRumble(float low, float high, int durationMs);
// The same at an explicit strength 0-100 (XBOX OPTS preview).
void GamepadRumbleAtStrength(float low, float high, int durationMs, int strength);
// The pointer haptic tick at strength 0-100.
void GamepadPointerHapticTick(int strength);
// Stops a rumble whose time is up; call every frame.
void GamepadRumbleUpdate();

} // namespace fallout

#endif /* FALLOUT_PLIB_GNW_GAMEPAD_H_ */