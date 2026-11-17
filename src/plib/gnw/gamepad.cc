#include <algorithm>
#include <cmath>

#include <SDL3/SDL.h>

#include "plib/gnw/button.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gamepad.hpp"
#include "plib/gnw/mouse.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/dxinput.h"
#include "plib/gnw/input.h"
#include "game/gconfig.h"
#include "game/gmouse.h"
#include "game/map.h"

// Based on glebm's initial mouse support PR https://github.com/alexbatalov/fallout1-ce/pull/118

namespace fallout {

// Gamepad settings read from config on init.
// Deadzone: whole-number percentage (0-100), converted to [0.0..1.0] at use.
// Sensitivity: 1-100, 50 is the recommended speed. Every 25 steps doubles
//   (or halves) the speed, so 1 is about 1/4 and 100 is 4x the speed at 50,
//   and equal steps feel like equal changes. See SensitivityScale.
//
// Left stick: at 50 the pointer moves at up to kLeftStickPixelsPerSecond at
//   640x480 (scaled up for bigger screens, see ProcessLeftStick), through a
//   response curve so small deflections give fine control. 500 px/s crosses
//   the 480p screen in about 1.3 s at full deflection.
//
// Right stick: at 50 the map pans one 32x24 scroll step per
//   kRightStickStepMs at full deflection (20 steps/sec, the engine's old
//   speed), smoothly at stickValue * (32 px per divisor ms) across and
//   (24 px per divisor ms) down.
static constexpr float kLeftStickPixelsPerSecond = 500.0f;
static constexpr float kRightStickStepMs = 50.0f;
static int gLeftStickDeadzonePercent  = 20;
static int gRightStickDeadzonePercent = 20;
static int gLeftStickSensitivity      = 50;
static int gRightStickSensitivity     = 50;
// When set, panning with the right stick locks the pointer to the middle of
// the map view (XBOX OPTS menu, [control] right_stick_center_cursor).
static bool gRightStickCenterCursor   = true;
// Slow the pointer over clickable things and bend it toward them
// (XBOX OPTS, [control] pointer_assist).
static bool gPointerAssist            = true;
// Pointer on the right stick, camera on the left (XBOX OPTS "SWAP STICKS",
// [control] swap_sticks). The stick clicks swap with them.
static bool gSwapSticks               = false;

// Derived from the 1-100 sensitivity values in GamepadInit.
// Pointer speed at full deflection, in 640x480 pixels per second.
static float gLeftStickPixelsPerSecond  = kLeftStickPixelsPerSecond;
// Milliseconds per scroll step at full deflection.
static float gRightStickInternalDivisor = kRightStickStepMs;

// Speed multiplier for a 1-100 sensitivity: 1.0 at 50, x2 per 25 steps.
static float SensitivityScale(int sensitivity)
{
    // exp, not exp2: nxdk's exp2/exp2f are assert(0) stubs.
    return static_cast<float>(std::exp((sensitivity - 50) / 25.0 * 0.69314718055994531));
}

// Simulated mouse state using gamepad inputs
int gGamepadLeftClick = 0;
int gGamepadRightClick = 0;
int gLeftStickDeltaX = 0;
int gLeftStickDeltaY = 0;

// Takes gamepad input and simulates mouse state
bool GetGamepadMouseState(MouseData* mouseState)
{
    if (!mouseState) {
        return false;
    }

    mouseState->x = gLeftStickDeltaX;
    mouseState->y = gLeftStickDeltaY;

    mouseState->buttons[0] = gGamepadLeftClick;
    mouseState->buttons[1] = gGamepadRightClick;

    mouseState->wheelX = 0;
    mouseState->wheelY = 0;

    // Reset deltas after they've been consumed
    gLeftStickDeltaX = 0;
    gLeftStickDeltaY = 0;

    return true;
}

namespace {

// Analog trigger values in [0.0..1.0]
static float gLeftTriggerValue  = 0.0f;
static float gRightTriggerValue = 0.0f;

// Right-trigger click state
static bool gRightTriggerShiftHeld = false;

// Tracks whether the A button is currently held (set/cleared in button handlers).
static bool gAButtonHeld = false;

// D-pad held state, used to detect diagonals for the left-trigger chord mapping.
static bool gDpadUp    = false;
static bool gDpadDown  = false;
static bool gDpadLeft  = false;
static bool gDpadRight = false;

// The scancode currently held via the left-trigger + d-pad chord
// (SDL_SCANCODE_UNKNOWN if none).
static SDL_Scancode gLeftTriggerChordKey = SDL_SCANCODE_UNKNOWN;

// Minimum left-trigger value to activate the d-pad chord mapping.
static constexpr float kChordTriggerThreshold = 0.5f;

// Maps the current d-pad held state to a digit scancode (1-8, clockwise from Up),
// or SDL_SCANCODE_UNKNOWN if no recognised direction is active.
SDL_Scancode GetDpadChordScancode()
{
    if ( gDpadUp   && !gDpadDown && !gDpadLeft && !gDpadRight) return SDL_SCANCODE_1;
    if ( gDpadUp   && !gDpadDown && !gDpadLeft &&  gDpadRight) return SDL_SCANCODE_2;
    if (!gDpadUp   && !gDpadDown && !gDpadLeft &&  gDpadRight) return SDL_SCANCODE_3;
    if (!gDpadUp   &&  gDpadDown && !gDpadLeft &&  gDpadRight) return SDL_SCANCODE_4;
    if (!gDpadUp   &&  gDpadDown && !gDpadLeft && !gDpadRight) return SDL_SCANCODE_5;
    if (!gDpadUp   &&  gDpadDown &&  gDpadLeft && !gDpadRight) return SDL_SCANCODE_6;
    if (!gDpadUp   && !gDpadDown &&  gDpadLeft && !gDpadRight) return SDL_SCANCODE_7;
    if ( gDpadUp   && !gDpadDown &&  gDpadLeft && !gDpadRight) return SDL_SCANCODE_8;
    return SDL_SCANCODE_UNKNOWN;
}

// Releases the currently held chord key, if any.
void ReleaseChordKey()
{
    if (gLeftTriggerChordKey != SDL_SCANCODE_UNKNOWN) {
        KeyboardData key = {gLeftTriggerChordKey, 0};
        GNW95_process_key(&key);
        gLeftTriggerChordKey = SDL_SCANCODE_UNKNOWN;
    }
}

    // [-32767.0..+32767.0] -> [-1.0..1.0]
    void ScaleJoystickAxes(float* x, float* y, float deadzone)
    {
        if (deadzone >= 1.0) {
            *x = 0;
            *y = 0;
            return;
        }

        const float maximum = 32767.0;
        float analogX = *x;
        float analogY = *y;
        float deadZone = deadzone * maximum;

        float magnitude = std::sqrt(analogX * analogX + analogY * analogY);
        // Rescale [deadzone..max] to [0..1], keeping the direction.
        if (magnitude > deadZone && magnitude > 0.0f) {
            float scalingFactor = 1.F / magnitude * (magnitude - deadZone) / (maximum - deadZone);
            analogX = (analogX * scalingFactor);
            analogY = (analogY * scalingFactor);

            float clampingFactor = 1.F;
            float absAnalogX = std::fabs(analogX);
            float absAnalogY = std::fabs(analogY);
            if (absAnalogX > 1.0 || absAnalogY > 1.0) {
                if (absAnalogX > absAnalogY) {
                    clampingFactor = 1.F / absAnalogX;
                } else {
                    clampingFactor = 1.F / absAnalogY;
                }
            }
            *x = (clampingFactor * analogX);
            *y = (clampingFactor * analogY);
        } else {
            *x = 0;
            *y = 0;
        }
    }

    float leftStickXUnscaled, leftStickYUnscaled, rightStickXUnscaled, rightStickYUnscaled;

    void ScaleLeftJoystick()
    {
        const float leftDeadzone = gLeftStickDeadzonePercent / 100.0f;
        leftStickX = leftStickXUnscaled;
        leftStickY = leftStickYUnscaled;
        ScaleJoystickAxes(&leftStickX, &leftStickY, leftDeadzone);
    }

    void ScaleRightJoystick()
    {
        const float rightDeadzone = gRightStickDeadzonePercent / 100.0f;
        rightStickX = rightStickXUnscaled;
        rightStickY = rightStickYUnscaled;
        ScaleJoystickAxes(&rightStickX, &rightStickY, rightDeadzone);
    }


} // namespace

float leftStickX, leftStickY, rightStickX, rightStickY;

// With the sticks swapped, the stick clicks follow their stick: the camera
// stick's click still centers the camera.
static int SwapStickButton(int button)
{
    if (gSwapSticks) {
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
        }
        if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            return SDL_GAMEPAD_BUTTON_LEFT_STICK;
        }
    }
    return button;
}

void HandleControllerAxisMotion(const SDL_Event& event)
{
    int axis = event.gaxis.axis;
    if (gSwapSticks) {
        switch (axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: axis = SDL_GAMEPAD_AXIS_RIGHTX; break;
        case SDL_GAMEPAD_AXIS_LEFTY: axis = SDL_GAMEPAD_AXIS_RIGHTY; break;
        case SDL_GAMEPAD_AXIS_RIGHTX: axis = SDL_GAMEPAD_AXIS_LEFTX; break;
        case SDL_GAMEPAD_AXIS_RIGHTY: axis = SDL_GAMEPAD_AXIS_LEFTY; break;
        }
    }

    switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
        leftStickXUnscaled = static_cast<float>(event.gaxis.value);
        ScaleLeftJoystick();
        break;

    case SDL_GAMEPAD_AXIS_LEFTY:
        leftStickYUnscaled = static_cast<float>(-event.gaxis.value);
        ScaleLeftJoystick();
        break;

    case SDL_GAMEPAD_AXIS_RIGHTX:
        rightStickXUnscaled = static_cast<float>(event.gaxis.value);
        ScaleRightJoystick();
        break;

    case SDL_GAMEPAD_AXIS_RIGHTY:
        rightStickYUnscaled = static_cast<float>(-event.gaxis.value);
        ScaleRightJoystick();
        break;

    // Left trigger: analog mouse speed modifier.
    // Full press -> 25% speed, half press -> 62.5% speed, etc.
    // Speed multiplier = 1.0 - triggerValue * 0.75 (applied in ProcessLeftStick).
    // Also gates the d-pad chord mapping when >= kChordTriggerThreshold.
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
        {
            const float prev = gLeftTriggerValue;
            gLeftTriggerValue = std::max(0.0f, std::min(1.0f,
                static_cast<float>(event.gaxis.value) / 32767.0f));

            // If the trigger drops below the chord threshold, release any held
            // chord key so it doesn't get stuck.
            if (prev >= kChordTriggerThreshold && gLeftTriggerValue < kChordTriggerThreshold) {
                ReleaseChordKey();
            }
        }
        break;

    // Right trigger: simple left mouse click (held while trigger is pressed).
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
        gRightTriggerValue = std::max(0.0f, std::min(1.0f,
            static_cast<float>(event.gaxis.value) / 32767.0f));
        break;
    }
}


void HandleControllerButtonUp(const SDL_Event& event)
{
    KeyboardData simulatedKeyboardKey = {0, 0};

    switch (SwapStickButton(event.gbutton.button)) {
    case SDL_GAMEPAD_BUTTON_SOUTH: // Left Mouse Click
        gAButtonHeld = false;
        break;

    case SDL_GAMEPAD_BUTTON_EAST: // Right MouseClick
        gGamepadRightClick = 0;
        break;

    case SDL_GAMEPAD_BUTTON_WEST: // Inventory (I)
        simulatedKeyboardKey = {SDL_SCANCODE_I, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_GAMEPAD_BUTTON_NORTH: // Pip-Boy (P)
        simulatedKeyboardKey = {SDL_SCANCODE_P, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_GAMEPAD_BUTTON_BACK: // Toggle Active Items (B)
        simulatedKeyboardKey = {SDL_SCANCODE_B, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_GAMEPAD_BUTTON_START: // Options Menu (Esc)
        simulatedKeyboardKey = {SDL_SCANCODE_ESCAPE, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_GAMEPAD_BUTTON_LEFT_STICK: // Skilldex (S)
        simulatedKeyboardKey = {SDL_SCANCODE_S, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: // Center Camera on Player (Home)
        simulatedKeyboardKey = {SDL_SCANCODE_HOME, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: // [Black Button] Exit Combat (Enter)
        simulatedKeyboardKey = {SDL_SCANCODE_KP_ENTER, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: // [White Button] End Turn (Space)
        simulatedKeyboardKey = {SDL_SCANCODE_SPACE, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    // D-pad: update held state, then either release the chord key (trigger held)
    // or send the normal arrow key release.
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        gDpadUp = false;
        if (gLeftTriggerValue >= kChordTriggerThreshold) {
            ReleaseChordKey();
        } else {
            simulatedKeyboardKey = {SDL_SCANCODE_UP, 0};
            GNW95_process_key(&simulatedKeyboardKey);
        }
        break;

    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        gDpadDown = false;
        if (gLeftTriggerValue >= kChordTriggerThreshold) {
            ReleaseChordKey();
        } else {
            simulatedKeyboardKey = {SDL_SCANCODE_DOWN, 0};
            GNW95_process_key(&simulatedKeyboardKey);
        }
        break;

    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        gDpadLeft = false;
        if (gLeftTriggerValue >= kChordTriggerThreshold) {
            ReleaseChordKey();
        } else {
            simulatedKeyboardKey = {SDL_SCANCODE_LEFT, 0};
            GNW95_process_key(&simulatedKeyboardKey);
        }
        break;

    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        gDpadRight = false;
        if (gLeftTriggerValue >= kChordTriggerThreshold) {
            ReleaseChordKey();
        } else {
            simulatedKeyboardKey = {SDL_SCANCODE_RIGHT, 0};
            GNW95_process_key(&simulatedKeyboardKey);
        }
        break;

    default:
        break;
    }
}


void HandleControllerButtonDown(const SDL_Event& event)
{
    KeyboardData simulatedKeyboardKey = {0, 0};
    switch (SwapStickButton(event.gbutton.button)) {
    case SDL_GAMEPAD_BUTTON_SOUTH: // Left Click
        gAButtonHeld = true;
        break;

    case SDL_GAMEPAD_BUTTON_EAST: // Right Click
        gGamepadRightClick = 1;
        break;

    case SDL_GAMEPAD_BUTTON_WEST: // Inventory (I)
        simulatedKeyboardKey = {SDL_SCANCODE_I, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_GAMEPAD_BUTTON_NORTH: // Pip-Boy (P)
        simulatedKeyboardKey = {SDL_SCANCODE_P, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_GAMEPAD_BUTTON_BACK: // Toggle Active Items (B)
        simulatedKeyboardKey = {SDL_SCANCODE_B, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_GAMEPAD_BUTTON_START: // Options Menu (Esc)
        simulatedKeyboardKey = {SDL_SCANCODE_ESCAPE, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_GAMEPAD_BUTTON_LEFT_STICK: // Skilldex (S)
        simulatedKeyboardKey = {SDL_SCANCODE_S, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: // Center Camera on Player (Home)
        simulatedKeyboardKey = {SDL_SCANCODE_HOME, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: // [Black Button] Exit Combat (Enter)
        simulatedKeyboardKey = {SDL_SCANCODE_KP_ENTER, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: // [White Button] End Turn (Space)
        simulatedKeyboardKey = {SDL_SCANCODE_SPACE, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    // D-pad: update held state, then either fire the chord digit key (trigger held)
    // or send the normal arrow key.
    //
    // Chord mapping (left trigger + d-pad, clockwise from Up):
    //   Up=1  Up+Right=2  Right=3  Down+Right=4
    //   Down=5  Down+Left=6  Left=7  Up+Left=8
    //
    // For diagonals the second button pressed resolves the final chord key.
    // The previous chord key is always released before the new one fires so
    // the game never sees two digit keys down at once.
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        gDpadUp = true;
        if (gLeftTriggerValue >= kChordTriggerThreshold) {
            ReleaseChordKey();
            gLeftTriggerChordKey = GetDpadChordScancode();
            if (gLeftTriggerChordKey != SDL_SCANCODE_UNKNOWN) {
                simulatedKeyboardKey = {gLeftTriggerChordKey, 1};
                GNW95_process_key(&simulatedKeyboardKey);
            }
        } else {
            simulatedKeyboardKey = {SDL_SCANCODE_UP, 1};
            GNW95_process_key(&simulatedKeyboardKey);
        }
        break;

    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        gDpadDown = true;
        if (gLeftTriggerValue >= kChordTriggerThreshold) {
            ReleaseChordKey();
            gLeftTriggerChordKey = GetDpadChordScancode();
            if (gLeftTriggerChordKey != SDL_SCANCODE_UNKNOWN) {
                simulatedKeyboardKey = {gLeftTriggerChordKey, 1};
                GNW95_process_key(&simulatedKeyboardKey);
            }
        } else {
            simulatedKeyboardKey = {SDL_SCANCODE_DOWN, 1};
            GNW95_process_key(&simulatedKeyboardKey);
        }
        break;

    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        gDpadLeft = true;
        if (gLeftTriggerValue >= kChordTriggerThreshold) {
            ReleaseChordKey();
            gLeftTriggerChordKey = GetDpadChordScancode();
            if (gLeftTriggerChordKey != SDL_SCANCODE_UNKNOWN) {
                simulatedKeyboardKey = {gLeftTriggerChordKey, 1};
                GNW95_process_key(&simulatedKeyboardKey);
            }
        } else {
            simulatedKeyboardKey = {SDL_SCANCODE_LEFT, 1};
            GNW95_process_key(&simulatedKeyboardKey);
        }
        break;

    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        gDpadRight = true;
        if (gLeftTriggerValue >= kChordTriggerThreshold) {
            ReleaseChordKey();
            gLeftTriggerChordKey = GetDpadChordScancode();
            if (gLeftTriggerChordKey != SDL_SCANCODE_UNKNOWN) {
                simulatedKeyboardKey = {gLeftTriggerChordKey, 1};
                GNW95_process_key(&simulatedKeyboardKey);
            }
        } else {
            simulatedKeyboardKey = {SDL_SCANCODE_RIGHT, 1};
            GNW95_process_key(&simulatedKeyboardKey);
        }
        break;

    default:
        break;
    }
}

void GamepadInit()
{
    // Read gamepad settings from the [control] section of the game config.
    // If a key is absent the variable retains its default value.
    config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_LEFT_STICK_DEADZONE_KEY,  &gLeftStickDeadzonePercent);
    config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_RIGHT_STICK_DEADZONE_KEY, &gRightStickDeadzonePercent);
    config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_LEFT_STICK_SENSITIVITY_KEY,  &gLeftStickSensitivity);
    config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_RIGHT_STICK_SENSITIVITY_KEY, &gRightStickSensitivity);
    int centerCursor = gRightStickCenterCursor ? 1 : 0;
    config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_RIGHT_STICK_CENTER_CURSOR_KEY, &centerCursor);
    gRightStickCenterCursor = centerCursor != 0;
    int pointerAssist = gPointerAssist ? 1 : 0;
    config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_POINTER_ASSIST_KEY, &pointerAssist);
    gPointerAssist = pointerAssist != 0;

    int edgeScroll = 1;
    config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_EDGE_SCROLL_KEY, &edgeScroll);
    gmouse_set_edge_scroll(edgeScroll != 0);

    int swapSticks = gSwapSticks ? 1 : 0;
    config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_SWAP_STICKS_KEY, &swapSticks);
    if ((swapSticks != 0) != gSwapSticks) {
        gSwapSticks = swapSticks != 0;
        // Drop whatever each stick was holding; new events will come in on
        // the other one.
        leftStickXUnscaled = leftStickYUnscaled = rightStickXUnscaled = rightStickYUnscaled = 0.0f;
        ScaleLeftJoystick();
        ScaleRightJoystick();
    }

    // Clamp deadzone to a sane range so a bad config value can't break input.
    gLeftStickDeadzonePercent  = std::max(0, std::min(95, gLeftStickDeadzonePercent));
    gRightStickDeadzonePercent = std::max(0, std::min(95, gRightStickDeadzonePercent));
    // Clamp sensitivity to the slider's 1-100.
    gLeftStickSensitivity  = std::max(1, std::min(100, gLeftStickSensitivity));
    gRightStickSensitivity = std::max(1, std::min(100, gRightStickSensitivity));

    gLeftStickPixelsPerSecond  = kLeftStickPixelsPerSecond * SensitivityScale(gLeftStickSensitivity);
    gRightStickInternalDivisor = kRightStickStepMs / SensitivityScale(gRightStickSensitivity);

    debug_printf("Gamepad config: left_deadzone=%d%% right_deadzone=%d%% left_sensitivity=%d (px/s=%.2f) right_sensitivity=%d (divisor=%.2f)\n",
        gLeftStickDeadzonePercent, gRightStickDeadzonePercent,
        gLeftStickSensitivity, gLeftStickPixelsPerSecond,
        gRightStickSensitivity, gRightStickInternalDivisor);
    debug_printf("Gamepad config: center_cursor=%d pointer_assist=%d edge_scroll=%d swap_sticks=%d\n", gRightStickCenterCursor ? 1 : 0, gPointerAssist ? 1 : 0, edgeScroll != 0 ? 1 : 0, gSwapSticks ? 1 : 0);
}

void ProcessLeftStick()
{
    // Fractional pixels carried over to the next frame.
    static float remainderX = 0.0f;
    static float remainderY = 0.0f;
    static Uint64 lastTicks = 0;

    const Uint64 now = SDL_GetTicksNS();
    // Cap the step so a stall (loading, a dialog) doesn't fling the pointer.
    const float dt = std::min(static_cast<float>(now - lastTicks) / 1000000000.0f, 0.05f);
    lastTicks = now;

    // Deflection past the deadzone, 0..1.
    const float length = std::sqrt(leftStickX * leftStickX + leftStickY * leftStickY);
    const float magnitude = std::min(1.0f, length);
    if (magnitude <= 0.0f) {
        remainderX = 0.0f;
        remainderY = 0.0f;
        return;
    }

    // Response curve: mostly quadratic, so half deflection moves at about a
    // third of full speed for fine aiming, while full deflection is
    // unchanged. The small linear part keeps a light push from feeling dead.
    const float curved = 0.2f * magnitude + 0.8f * magnitude * magnitude;

    // Keep the same feel at any resolution: speed scales with the screen's
    // linear size relative to 640x480 (about 1.73x at 1280x720).
    const float screenScale = std::sqrt(static_cast<float>(screenGetWidth()) * static_cast<float>(screenGetHeight()) / (640.0f * 480.0f));

    // Left trigger acts as an analog speed brake.
    // At 0% trigger: full speed (multiplier 1.0).
    // At 100% trigger: quarter speed (multiplier 0.25).
    const float speedMult = 1.0f - gLeftTriggerValue * 0.75f;

    const float pixels = curved * gLeftStickPixelsPerSecond * screenScale * speedMult * dt;
    float moveX = leftStickX / length * pixels;
    float moveY = -leftStickY / length * pixels;

    if (gPointerAssist && !win_pointer_assist(&moveX, &moveY)) {
        gmouse_pointer_assist(&moveX, &moveY);
    }

    remainderX += moveX;
    remainderY += moveY;

    const int dx = static_cast<int>(remainderX);
    const int dy = static_cast<int>(remainderY);
    remainderX -= static_cast<float>(dx);
    remainderY -= static_cast<float>(dy);

    gLeftStickDeltaX += dx;
    gLeftStickDeltaY += dy;
}

void ProcessRightStick()
{
    // Fractional pixels carried over to the next frame.
    static float remainderX = 0.0f;
    static float remainderY = 0.0f;
    static Uint64 lastTicks = 0;

    const Uint64 now = SDL_GetTicksNS();
    // Cap the step so a stall (loading, a dialog) doesn't make the map jump.
    const float dt = std::min(static_cast<float>(now - lastTicks) / 1000000.0f, 50.0f);
    lastTicks = now;

    if (rightStickX == 0.0f && rightStickY == 0.0f) {
        remainderX = 0.0f;
        remainderY = 0.0f;
        return;
    }

    // Pan around a pointer locked to the middle of the screen, so the hex
    // cursor lands on the center hex when the camera stops (optional).
    if (gRightStickCenterCursor) {
        map_center_mouse();
    }

    // Same speed as the stepped scrolling this replaced: one 32x24 px scroll
    // step per divisor ms at full deflection. Positive stick Y scrolls up, as
    // before.
    remainderX += rightStickX * dt * 32.0f / gRightStickInternalDivisor;
    remainderY -= rightStickY * dt * 24.0f / gRightStickInternalDivisor;

    const int dx = static_cast<int>(remainderX);
    const int dy = static_cast<int>(remainderY);
    if (dx != 0 || dy != 0) {
        map_scroll_pixels(dx, dy);
        remainderX -= dx;
        remainderY -= dy;
    }
}

void ProcessTriggers()
{
    // Minimum analog value to consider the right trigger "engaged".
    const float kDeadzone = 0.05f;

    // Right trigger: simple left mouse click held for as long as the trigger is pressed.
    // Release any held shift from old logic defensively.
    if (gRightTriggerShiftHeld) {
        KeyboardData shiftUp = {SDL_SCANCODE_LSHIFT, 0};
        GNW95_process_key(&shiftUp);
        gRightTriggerShiftHeld = false;
    }
    gGamepadLeftClick = (gRightTriggerValue >= kDeadzone || gAButtonHeld) ? 1 : 0;
}


void GamepadRegisterDefaultMapping()
{
    // SDL3's mapping database has no entry for the Original Xbox controller's
    // USB VID/PID, and the nxdk joystick driver does not supply one either, so
    // without this every pad would be seen as a plain joystick and no
    // SDL_EVENT_GAMEPAD_* events would ever arrive.
    //
    // The driver already reports axes and buttons in SDL_GAMEPAD_AXIS_* /
    // SDL_GAMEPAD_BUTTON_* order and the d-pad as hat 0, so the mapping below is
    // just the identity. Registering it under the "default" GUID makes SDL apply
    // it to any pad it doesn't otherwise recognise, matching what the nxdk SDL2
    // library hardcoded internally.
    //
    // It is supplied through the hint rather than SDL_AddGamepadMapping() so
    // that it can be set before SDL_INIT_JOYSTICK, which loads the hint ahead of
    // detecting any devices. See dxinput_init().
    static const char kDefaultMapping[] =
        "default,Original Xbox Controller,"
        "a:b0,b:b1,x:b2,y:b3,"
        "back:b4,guide:b5,start:b6,"
        "leftstick:b7,rightstick:b8,"
        "leftshoulder:b9,rightshoulder:b10,"
        "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"
        "leftx:a0,lefty:a1,rightx:a2,righty:a3,"
        "lefttrigger:a4,righttrigger:a5,";

    if (!SDL_SetHint(SDL_HINT_GAMECONTROLLERCONFIG, kDefaultMapping)) {
        debug_printf("Failed to register default gamepad mapping: %s\n", SDL_GetError());
        SDL_ClearError();
    }
}

void HandleJoystickDeviceAdded(const SDL_Event& event)
{
    const SDL_JoystickID instanceId = event.jdevice.which;
    debug_printf("Adding joystick %u: %s\n", (unsigned int)instanceId,
        SDL_GetJoystickNameForID(instanceId));
    SDL_Joystick* const joystick = SDL_OpenJoystick(instanceId);
    if (joystick == nullptr) {
        debug_printf("%s", SDL_GetError());
        SDL_ClearError();
        return;
    }
    SDL_GUID guid = SDL_GetJoystickGUID(joystick);
    char guidBuf[33];
    SDL_GUIDToString(guid, guidBuf, sizeof(guidBuf));
    debug_printf("Added joystick %u {%s}\n", (unsigned int)instanceId, guidBuf);
}

void HandleJoystickDeviceRemoved(const SDL_Event& event)
{
    debug_printf("Removed joystick %u\n", (unsigned int)event.jdevice.which);
}

void HandleControllerDeviceAdded(const SDL_Event& event)
{
    const SDL_JoystickID instanceId = event.gdevice.which;

    // Input is handled per event, so every attached pad is opened and drives the
    // game. A pad attached at startup is announced twice (once when the joystick
    // is detected and again when the gamepad subsystem comes up); opening it a
    // second time would bump its refcount and the single close on removal would
    // leak it.
    if (SDL_GetGamepadFromID(instanceId) != nullptr) {
        return;
    }

    debug_printf("Opening game controller for joystick %u\n", (unsigned int)instanceId);

    if (!SDL_IsGamepad(instanceId)) {
        debug_printf("Device %u is not a compatible game controller.\n", (unsigned int)instanceId);
        return;
    }

    SDL_Gamepad* controller = SDL_OpenGamepad(instanceId);
    if (controller == nullptr) {
        debug_printf("Failed to open game controller: %s\n", SDL_GetError());
        SDL_ClearError();
        return;
    }

    char* mapping = SDL_GetGamepadMapping(controller);
    if (mapping != nullptr) {
        debug_printf("Opened game controller with mapping:\n%s\n", mapping);
        SDL_free(mapping);
    }
}

void HandleControllerDeviceRemoved(const SDL_Event& event)
{
    SDL_Gamepad* controller = SDL_GetGamepadFromID(event.gdevice.which);
    if (controller != nullptr) {
        debug_printf("Removed game controller for joystick %u\n", (unsigned int)event.gdevice.which);
        SDL_CloseGamepad(controller);
    }
}

} // namespace fallout