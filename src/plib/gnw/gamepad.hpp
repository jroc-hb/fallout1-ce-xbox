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
// Whether A or RT (the gamepad's left click) is down right now.
bool GamepadClickHeld();
// Whether B (the gamepad's right click) is down right now. Read directly:
// the mouse layer ignores buttons while the pointer is hidden.
bool GamepadBackHeld();
// Switches walk mode (left stick walks the player) on or off, saves it and
// says so in the message box. Bound to the left stick click.
void GamepadToggleStickWalk();
// Walk mode is on.
bool GamepadStickWalkEnabled();
// The physical left stick's deflection past its deadzone (x right, y up),
// whatever the stick swap.
void GamepadGetPhysicalLeftStick(float* x, float* y);
// While set, A/RT never click and the pointer stick doesn't move the
// pointer: a pad-driven menu (the skill wheel) reads them itself.
void GamepadSetUiCapture(bool capture);

// A screen that reads the face buttons itself while the pointer is hidden
// (the inventory screens). While on, in pad mode (padnav) and with no
// d-pad-navigated window above it, A/RT don't click and B, X, Y, Black and
// White send nothing; the screen polls them with GamepadButtonPresses /
// GamepadButtonHeld. The d-pad still sends the arrow keys and the sticks
// still move the pointer (which leaves pad mode, ending the capture).
void GamepadSetScreenCapture(bool capture);

// A screen that scrolls its own view (the world map): while set, the camera
// stick calls `proc` every frame it's deflected (x/y -1..1, positive y up;
// dtMs since the last frame) instead of walking or panning the map. NULL ends it.
typedef void GamepadPanProc(float x, float y, float dtMs);
void GamepadSetPanHandler(GamepadPanProc* proc);
// The on-screen keyboard (game/padkeys.cc) is up: every button but the
// stick clicks goes only to it (polled as above), A never clicks and the
// sticks do nothing.
void GamepadSetTextCapture(bool capture);
// Whether the capture is in effect right now.
bool GamepadScreenCaptureActive();
// Running count of presses of an SDL gamepad button (any pad).
int GamepadButtonPresses(int button);
// Whether an SDL gamepad button is down (any pad).
bool GamepadButtonHeld(int button);

} // namespace fallout

#endif /* FALLOUT_PLIB_GNW_GAMEPAD_H_ */