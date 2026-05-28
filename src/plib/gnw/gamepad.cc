#include <algorithm>
#include <cmath>

#include <SDL.h>

#include "plib/gnw/debug.h"
#include "plib/gnw/gamepad.hpp"
#include "plib/gnw/mouse.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/dxinput.h"
#include "plib/gnw/input.h"
#include "game/gconfig.h"
#include "game/map.h"

// Based on glebm's initial mouse support PR https://github.com/alexbatalov/fallout1-ce/pull/118

namespace fallout {

static SDL_GameController* gController = nullptr;

// Gamepad settings read from config on init.
// Deadzone: whole-number percentage (0-100), converted to [0.0..1.0] at use.
// Sensitivity: user-facing 1-100 scale where 50 is the default and higher means faster.
//   Converted at init to an internal divisor — see GamepadInit.
//
// Left stick default (40): maps to internal divisor 2.0. Linear input — stick
//   deflection maps directly to cursor speed, uniform in all directions.
//
// Right stick default (40): maps to internal divisor 50.0.
//   Accumulates stickValue * dt each frame; a scroll step fires when the
//   accumulator crosses the divisor. At full deflection: ~20 tile-steps/sec.
static int gLeftStickDeadzonePercent  = 20;
static int gRightStickDeadzonePercent = 20;
static int gLeftStickSensitivity      = 40;
static int gRightStickSensitivity     = 40;

// Internal divisors derived from the 1-100 sensitivity values in GamepadInit.
static float gLeftStickInternalDivisor  = 2.0f;
static float gRightStickInternalDivisor = 50.0f;

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
        if (deadzone == 0) {
            return;
        }
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
        if (magnitude >= deadZone) {
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

    struct RightStickAccumulator {
        RightStickAccumulator()
            : lastTc(SDL_GetTicks()), hiresDX(0.0f), hiresDY(0.0f)
        {}

        bool GetScrollDelta(int* outX, int* outY, float divisor)
        {
            const Uint32 tc = SDL_GetTicks();
            const int dtc = static_cast<int>(tc - lastTc);
            lastTc = tc;

            hiresDX += rightStickX * dtc;
            hiresDY += rightStickY * dtc;

            static constexpr float kDiagThreshold = 0.3f;
            const bool diagonalIntent = std::abs(rightStickX) > kDiagThreshold
                                     && std::abs(rightStickY) > kDiagThreshold;

            if (diagonalIntent) {
                if (std::abs(hiresDX) >= divisor || std::abs(hiresDY) >= divisor) {
                    *outX = (rightStickX > 0.0f) ? 1 : -1;
                    *outY = (rightStickY > 0.0f) ? -1 : 1;
                    hiresDX = 0.0f;
                    hiresDY = 0.0f;
                    return true;
                }
            } else {
                if (std::abs(hiresDX) >= divisor) {
                    *outX = (hiresDX > 0.0f) ? 1 : -1;
                    *outY = 0;
                    hiresDX = 0.0f;
                    return true;
                }
                if (std::abs(hiresDY) >= divisor) {
                    *outX = 0;
                    *outY = (hiresDY > 0.0f) ? -1 : 1;
                    hiresDY = 0.0f;
                    return true;
                }
            }

            return false;
        }

        void Clear()
        {
            lastTc  = SDL_GetTicks();
            hiresDX = 0.0f;
            hiresDY = 0.0f;
        }

        Uint32 lastTc;
        float  hiresDX;
        float  hiresDY;
    };

    struct LeftStickAccumulator {

        LeftStickAccumulator()
        {
            lastTc = SDL_GetTicks();
            hiresDX = 0;
            hiresDY = 0;
        }

        void Pool(int* x, int* y, float slowdown)
        {
            const Uint32 tc = SDL_GetTicks();
            const int dtc = tc - lastTc;
            hiresDX += leftStickX * dtc;
            hiresDY += leftStickY * dtc;
            const int dx = static_cast<int>(hiresDX / slowdown);
            const int dy = static_cast<int>(hiresDY / slowdown);
            *x += dx;
            *y -= dy;
            lastTc = tc;
            hiresDX -= dx * slowdown;
            hiresDY -= dy * slowdown;
        }

        void Clear()
        {
            lastTc  = SDL_GetTicks();
            hiresDX = 0.0f;
            hiresDY = 0.0f;
        }

        uint32_t lastTc;
        float hiresDX;
        float hiresDY;
    };

} // namespace

float leftStickX, leftStickY, rightStickX, rightStickY;

void HandleControllerAxisMotion(const SDL_Event& event)
{
    switch (event.caxis.axis) {
    case SDL_CONTROLLER_AXIS_LEFTX:
        leftStickXUnscaled = static_cast<float>(event.caxis.value);
        ScaleLeftJoystick();
        break;

    case SDL_CONTROLLER_AXIS_LEFTY:
        leftStickYUnscaled = static_cast<float>(-event.caxis.value);
        ScaleLeftJoystick();
        break;

    case SDL_CONTROLLER_AXIS_RIGHTX:
        rightStickXUnscaled = static_cast<float>(event.caxis.value);
        ScaleRightJoystick();
        break;

    case SDL_CONTROLLER_AXIS_RIGHTY:
        rightStickYUnscaled = static_cast<float>(-event.caxis.value);
        ScaleRightJoystick();
        break;

    // Left trigger: analog mouse speed modifier.
    // Full press -> 25% speed, half press -> 62.5% speed, etc.
    // Speed multiplier = 1.0 - triggerValue * 0.75 (applied in ProcessLeftStick).
    // Also gates the d-pad chord mapping when >= kChordTriggerThreshold.
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
        {
            const float prev = gLeftTriggerValue;
            gLeftTriggerValue = std::max(0.0f, std::min(1.0f,
                static_cast<float>(event.caxis.value) / 32767.0f));

            // If the trigger drops below the chord threshold, release any held
            // chord key so it doesn't get stuck.
            if (prev >= kChordTriggerThreshold && gLeftTriggerValue < kChordTriggerThreshold) {
                ReleaseChordKey();
            }
        }
        break;

    // Right trigger: simple left mouse click (held while trigger is pressed).
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
        gRightTriggerValue = std::max(0.0f, std::min(1.0f,
            static_cast<float>(event.caxis.value) / 32767.0f));
        break;
    }
}


void HandleControllerButtonUp(const SDL_Event& event)
{
    KeyboardData simulatedKeyboardKey = {0, 0};

    switch (event.cbutton.button) {
    case SDL_CONTROLLER_BUTTON_A: // Left Mouse Click
        gAButtonHeld = false;
        break;

    case SDL_CONTROLLER_BUTTON_B: // Right MouseClick
        gGamepadRightClick = 0;
        break;

    case SDL_CONTROLLER_BUTTON_X: // Inventory (I)
        simulatedKeyboardKey = {SDL_SCANCODE_I, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_Y: // Pip-Boy (P)
        simulatedKeyboardKey = {SDL_SCANCODE_P, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_BACK: // Toggle Active Items (B)
        simulatedKeyboardKey = {SDL_SCANCODE_B, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_START: // Options Menu (Esc)
        simulatedKeyboardKey = {SDL_SCANCODE_ESCAPE, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_LEFTSTICK: // Skilldex (S)
        simulatedKeyboardKey = {SDL_SCANCODE_S, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: // Center Camera on Player (Home)
        simulatedKeyboardKey = {SDL_SCANCODE_HOME, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: // [Black Button] Exit Combat (Enter)
        simulatedKeyboardKey = {SDL_SCANCODE_KP_ENTER, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: // [White Button] End Turn (Space)
        simulatedKeyboardKey = {SDL_SCANCODE_SPACE, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    // D-pad: update held state, then either release the chord key (trigger held)
    // or send the normal arrow key release.
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        gDpadUp = false;
        if (gLeftTriggerValue >= kChordTriggerThreshold) {
            ReleaseChordKey();
        } else {
            simulatedKeyboardKey = {SDL_SCANCODE_UP, 0};
            GNW95_process_key(&simulatedKeyboardKey);
        }
        break;

    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        gDpadDown = false;
        if (gLeftTriggerValue >= kChordTriggerThreshold) {
            ReleaseChordKey();
        } else {
            simulatedKeyboardKey = {SDL_SCANCODE_DOWN, 0};
            GNW95_process_key(&simulatedKeyboardKey);
        }
        break;

    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        gDpadLeft = false;
        if (gLeftTriggerValue >= kChordTriggerThreshold) {
            ReleaseChordKey();
        } else {
            simulatedKeyboardKey = {SDL_SCANCODE_LEFT, 0};
            GNW95_process_key(&simulatedKeyboardKey);
        }
        break;

    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
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
    switch (event.cbutton.button) {
    case SDL_CONTROLLER_BUTTON_A: // Left Click
        gAButtonHeld = true;
        break;

    case SDL_CONTROLLER_BUTTON_B: // Right Click
        gGamepadRightClick = 1;
        break;

    case SDL_CONTROLLER_BUTTON_X: // Inventory (I)
        simulatedKeyboardKey = {SDL_SCANCODE_I, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_Y: // Pip-Boy (P)
        simulatedKeyboardKey = {SDL_SCANCODE_P, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_BACK: // Toggle Active Items (B)
        simulatedKeyboardKey = {SDL_SCANCODE_B, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_START: // Options Menu (Esc)
        simulatedKeyboardKey = {SDL_SCANCODE_ESCAPE, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_LEFTSTICK: // Skilldex (S)
        simulatedKeyboardKey = {SDL_SCANCODE_S, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: // Center Camera on Player (Home)
        simulatedKeyboardKey = {SDL_SCANCODE_HOME, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: // [Black Button] Exit Combat (Enter)
        simulatedKeyboardKey = {SDL_SCANCODE_KP_ENTER, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: // [White Button] End Turn (Space)
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
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
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

    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
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

    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
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

    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
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

    // Clamp deadzone to a sane range so a bad config value can't break input.
    gLeftStickDeadzonePercent  = std::max(0, std::min(95, gLeftStickDeadzonePercent));
    gRightStickDeadzonePercent = std::max(0, std::min(95, gRightStickDeadzonePercent));
    // Clamp sensitivity to 1-100; 0 would produce a division-by-zero in the conversion.
    gLeftStickSensitivity  = std::max(1, std::min(100, gLeftStickSensitivity));
    gRightStickSensitivity = std::max(1, std::min(100, gRightStickSensitivity));

    // Convert 1-100 user values to internal divisors. 50 == default behaviour.
    // Higher user value = larger divisor at 50, smaller above — i.e. faster.
    gLeftStickInternalDivisor  = (50.0f / static_cast<float>(gLeftStickSensitivity))  * 2.0f;
    gRightStickInternalDivisor = (50.0f / static_cast<float>(gRightStickSensitivity)) * 50.0f;

    debug_printf("Gamepad config: left_deadzone=%d%% right_deadzone=%d%% left_sensitivity=%d (divisor=%.2f) right_sensitivity=%d (divisor=%.2f)\n",
        gLeftStickDeadzonePercent, gRightStickDeadzonePercent,
        gLeftStickSensitivity, gLeftStickInternalDivisor,
        gRightStickSensitivity, gRightStickInternalDivisor);
}

void ProcessLeftStick()
{
    static LeftStickAccumulator acc;
    if (leftStickX == 0 && leftStickY == 0) {
        acc.Clear();
        return;
    }

    // Left trigger acts as an analog speed brake.
    // At 0% trigger: full speed (multiplier 1.0).
    // At 100% trigger: quarter speed (multiplier 0.25).
    const float speedMult = 1.0f - gLeftTriggerValue * 0.75f;

    const float savedX = leftStickX;
    const float savedY = leftStickY;
    leftStickX *= speedMult;
    leftStickY *= speedMult;

    int x, y;
    SDL_GetRelativeMouseState(&x, &y);
    int newX = x;
    int newY = y;
    acc.Pool(&newX, &newY, gLeftStickInternalDivisor);

    leftStickX = savedX;
    leftStickY = savedY;

    if (newX != x || newY != y) {
        gLeftStickDeltaX += (newX - x);
        gLeftStickDeltaY += (newY - y);
    }
}

void ProcessRightStick()
{
    static RightStickAccumulator acc;

    if (rightStickX == 0.0f && rightStickY == 0.0f) {
        acc.Clear();
        return;
    }

    int dx = 0, dy = 0;
    if (acc.GetScrollDelta(&dx, &dy, gRightStickInternalDivisor)) {
        map_scroll(dx, dy);
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


void HandleJoystickDeviceAdded(const SDL_Event& event)
{
    const int32_t deviceIndex = event.jdevice.which;
    if (SDL_NumJoysticks() <= deviceIndex)
        return;
    debug_printf("Adding joystick %d: %s\n", deviceIndex,
        SDL_JoystickNameForIndex(deviceIndex));
    SDL_Joystick* const joystick = SDL_JoystickOpen(deviceIndex);
    if (joystick == nullptr) {
        debug_printf("%s", SDL_GetError());
        SDL_ClearError();
        return;
    }
    SDL_JoystickGUID guid = SDL_JoystickGetGUID(joystick);
    char guidBuf[33];
    SDL_JoystickGetGUIDString(guid, guidBuf, sizeof(guidBuf));
    debug_printf("Added joystick %d {%s}\n", deviceIndex, guidBuf);
}

void HandleJoystickDeviceRemoved(const SDL_Event& event)
{
    const int32_t deviceIndex = event.jdevice.which;
    debug_printf("Removed joystick %d\n", deviceIndex);
}

void HandleControllerDeviceAdded(const SDL_Event& event)
{
    const int32_t joystickIndex = event.cdevice.which;
    debug_printf("Opening game controller for joystick %d\n", joystickIndex);

    if (!SDL_IsGameController(joystickIndex)) {
        debug_printf("Device at index %d is not a compatible game controller.\n", joystickIndex);
        return;
    }

    gController = SDL_GameControllerOpen(joystickIndex);
    if (gController == nullptr) {
        debug_printf("Failed to open game controller: %s\n", SDL_GetError());
        SDL_ClearError();
        return;
    }

    SDL_Joystick* sdlJoystick = SDL_GameControllerGetJoystick(gController);
    SDL_JoystickGUID guid = SDL_JoystickGetGUID(sdlJoystick);

    char* mapping = SDL_GameControllerMappingForGUID(guid);
    if (mapping != nullptr) {
        debug_printf("Opened game controller with mapping:\n%s\n", mapping);
        SDL_free(mapping);
    }
}

void HandleControllerDeviceRemoved(const SDL_Event& event)
{
    if (gController != nullptr) {
        SDL_GameController* controller = SDL_GameControllerFromInstanceID(event.cdevice.which);
        if (controller == gController) {
            debug_printf("Removed game controller for joystick %d\n", event.cdevice.which);
            SDL_GameControllerClose(gController);
            gController = nullptr;
        }
    }
}

} // namespace fallout