#include <algorithm>
#include <cmath>

#include <SDL.h>

#include "plib/gnw/debug.h"
#include "plib/gnw/gamepad.hpp"
#include "plib/gnw/mouse.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/dxinput.h"
#include "plib/gnw/input.h"
#include "game/map.h"

// Based on glebm's initial mouse support PR https://github.com/alexbatalov/fallout1-ce/pull/118

namespace fallout {

static SDL_GameController* gController = nullptr;

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
        const float leftDeadzone = 0.24f;
        leftStickX = leftStickXUnscaled;
        leftStickY = leftStickYUnscaled;
        ScaleJoystickAxes(&leftStickX, &leftStickY, leftDeadzone);
    }

    void ScaleRightJoystick()
    {
        const float rightDeadzone = 0.24f;
        rightStickX = rightStickXUnscaled;
        rightStickY = rightStickYUnscaled;
        ScaleJoystickAxes(&rightStickX, &rightStickY, rightDeadzone);
    }

    struct RightStickAccumulator {
        RightStickAccumulator()
        {
            lastTc = SDL_GetTicks();
            hiresDX = 0;
            hiresDY = 0;
        }

        bool GetScrollDelta(int* outX, int* outY, int slowdown)
        {
            const Uint32 tc = SDL_GetTicks();
            const int dtc = tc - lastTc;
            lastTc = tc;

            hiresDX += rightStickX * dtc;
            hiresDY += rightStickY * dtc;

            int dx = static_cast<int>(hiresDX / slowdown);
            int dy = static_cast<int>(hiresDY / slowdown);

            // If both stick axes are being pushed, enforce a diagonal
            const float diagThreshold = 0.3f;  // analog stick strength

            bool diagonalIntent =
                std::abs(rightStickX) > diagThreshold &&
                std::abs(rightStickY) > diagThreshold;

            if (diagonalIntent) {
                if (std::abs(hiresDX) >= slowdown || std::abs(hiresDY) >= slowdown) {
                    *outX = (rightStickX > 0) ? 1 : -1;
                    *outY = (rightStickY > 0) ? -1 : 1;

                    hiresDX = 0;
                    hiresDY = 0;
                    return true;
                }
            }
            else {
                if (std::abs(hiresDX) >= slowdown) {
                    *outX = (hiresDX > 0) ? 1 : -1;
                    *outY = 0;
                    hiresDX = 0;
                    return true;
                }
                if (std::abs(hiresDY) >= slowdown) {
                    *outX = 0;
                    *outY = (hiresDY > 0) ? -1 : 1;
                    hiresDY = 0;
                    return true;
                }
            }

            return false;
        }

        void Clear()
        {
            lastTc = SDL_GetTicks();
        }

        uint32_t lastTc;
        float hiresDX;
        float hiresDY;
    };

    struct LeftStickAccumulator {

        LeftStickAccumulator()
        {
            lastTc = SDL_GetTicks();
            hiresDX = 0;
            hiresDY = 0;
        }

        void Pool(int* x, int* y, int slowdown)
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
            // keep track of remainder for sub-pixel motion
            hiresDX -= dx * slowdown;
            hiresDY -= dy * slowdown;
        }

        void Clear()
        {
            lastTc = SDL_GetTicks();
        }

        uint32_t lastTc;
        float hiresDX;
        float hiresDY;
    };

} // namespace

float leftStickX, leftStickY, rightStickX, rightStickY;

void HandleControllerAxisMotion(const SDL_Event& event)
{
    static bool leftTriggerPressed = false;
    static bool rightTriggerPressed = false;
    const int triggerThreshold = 30000;

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

    // Trigger inputs are handled here since they are technically analog axes
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
        if (event.caxis.value > triggerThreshold) {
            if (!leftTriggerPressed) {
                leftTriggerPressed = true;
                KeyboardData key = {SDL_SCANCODE_N, 1};
                GNW95_process_key(&key);
            }
        } else if (leftTriggerPressed) {
            leftTriggerPressed = false;
            KeyboardData key = {SDL_SCANCODE_N, 0};
            GNW95_process_key(&key);
        }
        break;

    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
        if (event.caxis.value > triggerThreshold) {
            if (!rightTriggerPressed) {
                rightTriggerPressed = true;
                KeyboardData key = {SDL_SCANCODE_B, 1};
                GNW95_process_key(&key);
            }
        } else if (rightTriggerPressed) {
            rightTriggerPressed = false;
            KeyboardData key = {SDL_SCANCODE_B, 0};
            GNW95_process_key(&key);
        }
        break;
    }
}


void HandleControllerButtonUp(const SDL_Event& event)
{
    KeyboardData simulatedKeyboardKey = {0, 0};

    switch (event.cbutton.button) {
    case SDL_CONTROLLER_BUTTON_A: // Left Mouse Click
        gGamepadLeftClick = 0;
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

    case SDL_CONTROLLER_BUTTON_BACK: // Character Sheet (C)
        simulatedKeyboardKey = {SDL_SCANCODE_C, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_START: // Options Menu (Esc)
        simulatedKeyboardKey = {SDL_SCANCODE_ESCAPE, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_LEFTSTICK: // Center Camera on Player (Home)
        simulatedKeyboardKey = {SDL_SCANCODE_HOME, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: // Enter Combat Mode (A)
        simulatedKeyboardKey = {SDL_SCANCODE_A, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: // [Black Button] Exit Combat (Enter)
        simulatedKeyboardKey = {SDL_SCANCODE_KP_ENTER, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: // [White Button] End Turn (Space)
        simulatedKeyboardKey = {SDL_SCANCODE_E, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_DPAD_UP: // Menu/Map Uo (Up Arrow)
        simulatedKeyboardKey = {SDL_SCANCODE_UP, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: // Menu/Map Down (Down Arrow)
        simulatedKeyboardKey = {SDL_SCANCODE_DOWN, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: // Menu/Map Left (Left Arrow)
        simulatedKeyboardKey = {SDL_SCANCODE_LEFT, 0};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: // Menu/Map Right (Right Arrow)
        simulatedKeyboardKey = {SDL_SCANCODE_RIGHT, 0};
        GNW95_process_key(&simulatedKeyboardKey);
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
        gGamepadLeftClick = 1;
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

    case SDL_CONTROLLER_BUTTON_BACK: // Character Sheet (C)
        simulatedKeyboardKey = {SDL_SCANCODE_C, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_START: // Options Menu (Esc)
        simulatedKeyboardKey = {SDL_SCANCODE_ESCAPE, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_LEFTSTICK: // Center Camera on Player (Home)
        simulatedKeyboardKey = {SDL_SCANCODE_HOME, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: // Enter Combat Mode (A)
        simulatedKeyboardKey = {SDL_SCANCODE_A, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: // [Black Button] Exit Combat (Enter)
        simulatedKeyboardKey = {SDL_SCANCODE_KP_ENTER, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: // [White Button] End Turn (Space)
        simulatedKeyboardKey = {SDL_SCANCODE_E, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_DPAD_UP: // Menu/Map Up (Up Arrow)
        simulatedKeyboardKey = {SDL_SCANCODE_UP, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: // Menu/Map Down (Down Arrow)
        simulatedKeyboardKey = {SDL_SCANCODE_DOWN, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: // Menu/Map Right (Right Arrow)
        simulatedKeyboardKey = {SDL_SCANCODE_LEFT, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: // Menu/Map Right (Right Arrow)
        simulatedKeyboardKey = {SDL_SCANCODE_RIGHT, 1};
        GNW95_process_key(&simulatedKeyboardKey);
        break;

    default:
        break;
    }
}

void ProcessLeftStick()
{
    static LeftStickAccumulator acc;
    // deadzone is handled in ScaleJoystickAxes() already
    if (leftStickX == 0 && leftStickY == 0) {
        acc.Clear();
        return;
    }

    int x, y;
    SDL_GetRelativeMouseState(&x, &y);
    int newX = x;
    int newY = y;
    acc.Pool(&newX, &newY, 2);

    if (newX != x || newY != y) {
        gLeftStickDeltaX += (newX - x);
        gLeftStickDeltaY += (newY - y);
    }
}

void ProcessRightStick()
{
    static RightStickAccumulator acc;
    int dx = 0, dy = 0;

    // Skip when stick is neutral
    if (rightStickX == 0 && rightStickY == 0) {
        acc.Clear();
        return;
    }

    if (acc.GetScrollDelta(&dx, &dy, 50)) {
        map_scroll(dx, dy);
    }
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