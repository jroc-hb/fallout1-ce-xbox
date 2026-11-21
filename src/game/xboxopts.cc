#include "game/xboxopts.h"

#ifdef NXDK

#include <stdio.h>
#include <string.h>

#include <algorithm>

#include <hal/xbox.h>

#include "game/art.h"
#include "game/gconfig.h"
#include "game/gsound.h"
#include "game/object_types.h"
#include "game/stickwalk.h"
#include "plib/color/color.h"
#include "plib/gnw/button.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gamepad.hpp"
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/input.h"
#include "plib/gnw/memory.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/text.h"
#include "plib/gnw/padnav.h"

#include <SDL3/SDL.h>

namespace fallout {

// The XBOX OPTS screen, built from pieces of the Preferences screen art
// (PREFSCRN.FRM) so it looks like the game's own menus:
//
//   +------------------+---------------------------+
//   | title            | POINTER deadzone          |
//   | [knob] DISPLAY   |         sensitivity       |
//   | [knob] PAN CURSOR+---------------------------+
//   | [knob] ASSIST    | CAMERA  deadzone          |
//   | [knob] EDGE SCRL |         sensitivity       |
//   | [knob] SWAP      +---------------------------+
//   |                  | SAFE AREA                 |
//   |                  | RUMBLE                    |
//   +------------------+---------------------------+
//   | DEFAULT  DONE  CANCEL               hints    |
//   +----------------------------------------------+
//
// The left column and the bottom button strip are the Preferences screen's
// own, in place (its five rotary knob panels); the right column is three
// copies of its two-slider panel, the last one 12 px shorter.

#define XO_WIDTH 470
#define XO_HEIGHT 480

// Source art is the 640x480 Preferences background.
#define PREF_WIDTH 640

// The right column is the Preferences slider panel moved left by this much.
#define XO_SLIDER_SHIFT 170
// The second and third slider panels sit this much lower than the first.
#define XO_SLIDER_GROUP2_DY 148
#define XO_SLIDER_GROUP3_DY 296
// The third panel leaves out these rows of empty texture under its first
// slider so it fits above the button strip; its second slider moves up by
// the cut.
#define XO_PANEL3_CUT_TOP 70
#define XO_PANEL3_CUT 12
#define XO_SLIDER_MIN_X (384 - XO_SLIDER_SHIFT)
#define XO_SLIDER_SPAN 219
#define XO_SLIDER_RIGHT (624 - XO_SLIDER_SHIFT)

#define XO_KNOB_X 76
#define XO_KNOB_WIDTH 46
#define XO_KNOB_HEIGHT 47
#define XO_SLIDER_KNOB_WIDTH 21
#define XO_SLIDER_KNOB_HEIGHT 12

typedef enum XboxOptsFrm {
    XO_FRM_BACKGROUND, // prefscrn.frm
    XO_FRM_SLIDER_OFF, // prfsldof.frm
    XO_FRM_KNOB, // prfbknbs.frm (4 positions, stacked)
    XO_FRM_SLIDER_ON, // prfsldon.frm
    XO_FRM_RED_UP, // lilredup.frm
    XO_FRM_RED_DOWN, // lilreddn.frm
    XO_FRM_COUNT,
} XboxOptsFrm;

static const int xo_frm_ids[XO_FRM_COUNT] = { 240, 241, 242, 247, 8, 9 };

// Sliders are named for what the stick does (with SWAP STICKS the pointer is
// on the right stick); they store the [control] left_stick_* (pointer) and
// right_stick_* (camera) keys.
typedef enum XboxOptsItem {
    XO_ITEM_DISPLAY,
    XO_ITEM_CENTER_CURSOR,
    XO_ITEM_POINTER_ASSIST,
    XO_ITEM_EDGE_SCROLL,
    XO_ITEM_SWAP_STICKS,
    XO_ITEM_LEFT_DEADZONE,
    XO_ITEM_LEFT_SENSITIVITY,
    XO_ITEM_RIGHT_DEADZONE,
    XO_ITEM_RIGHT_SENSITIVITY,
    XO_ITEM_SAFE_AREA,
    XO_ITEM_RUMBLE,
    XO_ITEM_COUNT,
} XboxOptsItem;

#define XO_FIRST_SLIDER XO_ITEM_LEFT_DEADZONE
#define XO_KNOB_COUNT XO_FIRST_SLIDER
#define XO_SLIDER_COUNT (XO_ITEM_COUNT - XO_FIRST_SLIDER)

// DISPLAY knob positions: f1_res.ini video mode plus [XBOX] UI_SCALE.
#define XO_DISPLAY_480P 0
#define XO_DISPLAY_720P 1 // UI_SCALE 0..2 -> 1..3

// Button event codes.
#define XO_EVENT_DONE 504
#define XO_EVENT_DEFAULT 527
#define XO_EVENT_CANCEL 528
#define XO_EVENT_DEV 529
#define XO_EVENT_ITEM 610 // + item index

typedef struct XboxOptsKnob {
    const char* label;
    int valuesCount;
    const char* values[4];
    int labelY;
    int knobY;
} XboxOptsKnob;

typedef struct XboxOptsSlider {
    const char* label;
    const char* format;
    int minValue;
    int maxValue;
    int labelY;
    int knobY;
} XboxOptsSlider;

// The Preferences screen's knob rows (options.cc row1Ytab / btndat).
static const XboxOptsKnob xo_knobs[XO_KNOB_COUNT] = {
    { "DISPLAY", 4, { "480P", "720P", "720P 1.33X", "720P 1.5X" }, 48, 71 },
    { "PAN CENTERS CURSOR", 2, { "OFF", "ON" }, 125, 149 },
    // +RUMBLE: assist plus a tick when the pointer reaches something.
    { "POINTER ASSIST", 3, { "OFF", "ON", "+RUMBLE" }, 203, 226 },
    { "EDGE SCROLL", 2, { "OFF", "ON" }, 286, 309 },
    { "SWAP STICKS", 2, { "OFF", "ON" }, 363, 387 },
};

// Value label offsets from the knob, per position (options.cc bglbx/bglby):
// left of it, above it, right of it, below right.
static const int xo_knob_label_x[4] = { 2, 25, 46, 46 };
static const int xo_knob_label_y[4] = { 10, -4, 10, 31 };

static const XboxOptsSlider xo_sliders[XO_SLIDER_COUNT] = {
    { "POINTER DEADZONE", "%d%%", 0, 95, 19, 50 },
    { "POINTER SENSITIVITY", "%d", 1, 100, 94, 125 },
    { "CAMERA DEADZONE", "%d%%", 0, 95, 19 + XO_SLIDER_GROUP2_DY, 50 + XO_SLIDER_GROUP2_DY },
    { "CAMERA SENSITIVITY", "%d", 1, 100, 94 + XO_SLIDER_GROUP2_DY, 125 + XO_SLIDER_GROUP2_DY },
    { "SAFE AREA", "%d%%", 0, 8, 19 + XO_SLIDER_GROUP3_DY, 50 + XO_SLIDER_GROUP3_DY },
    { "RUMBLE", "%d%%", 0, 100, 94 - XO_PANEL3_CUT + XO_SLIDER_GROUP3_DY, 125 - XO_PANEL3_CUT + XO_SLIDER_GROUP3_DY },
};

// Defaults written by gconfig_init (gconfig.cc) for a fresh config. DEFAULT
// leaves the display mode alone.
static const int xo_default_values[XO_ITEM_COUNT] = { 0, 1, 1, 1, 0, 24, 50, 24, 50, 0, 100 };

static const char* xo_res_path = "E:\\UDATA\\FALLOUT1\\f1_res.ini";

static unsigned char* xo_frm_data[XO_FRM_COUNT];
static CacheEntry* xo_frm_keys[XO_FRM_COUNT];
static Size xo_frm_size[XO_FRM_COUNT];

static unsigned char* xo_background;
static int xo_win = -1;
static unsigned char* xo_win_buf;
static int xo_values[XO_ITEM_COUNT];
static int xo_selected;
static int xo_dragging = -1;

static void xo_blit(XboxOptsFrm frm, int srcX, int srcY, int width, int height, int destX, int destY)
{
    int pitch = xo_frm_size[frm].width;
    buf_to_buf(xo_frm_data[frm] + pitch * srcY + srcX,
        width,
        height,
        pitch,
        xo_background + XO_WIDTH * destY + destX,
        XO_WIDTH);
}

static void xo_build_background()
{
    // Left column with its five knob panels, as on the Preferences screen.
    xo_blit(XO_FRM_BACKGROUND, 0, 0, 196, 440, 0, 0);
    // Our title is wider than the gap the Preferences title sits in, so use
    // the plain part of the title band (no grille) above the left column.
    xo_blit(XO_FRM_BACKGROUND, 60, 0, 196, 38, 0, 0);

    // Right column: two copies of the two-slider panel...
    xo_blit(XO_FRM_BACKGROUND, 366, 0, 274, 152, 196, 0);
    xo_blit(XO_FRM_BACKGROUND, 366, 2, 274, 150, 196, XO_SLIDER_GROUP2_DY + 2);

    // ...and a third, without some empty rows under its first slider.
    xo_blit(XO_FRM_BACKGROUND, 366, 2, 274, XO_PANEL3_CUT_TOP - 2, 196, XO_SLIDER_GROUP3_DY + 2);
    int cutBottom = XO_PANEL3_CUT_TOP + XO_PANEL3_CUT;
    xo_blit(XO_FRM_BACKGROUND, 366, cutBottom, 274, 152 - cutBottom, 196, XO_SLIDER_GROUP3_DY + XO_PANEL3_CUT_TOP);

    // Plain texture (the title band between the grilles) below it.
    int fillTop = XO_SLIDER_GROUP3_DY + 152 - XO_PANEL3_CUT;
    if (fillTop < 440) {
        xo_blit(XO_FRM_BACKGROUND, 60, 0, 250, 440 - fillTop, 196, fillTop);
        xo_blit(XO_FRM_BACKGROUND, 60, 0, 24, 440 - fillTop, 446, fillTop);
    }

    // Bottom: the button strip with its three plates, where the Preferences
    // screen has it, plus plain texture to the right of it.
    xo_blit(XO_FRM_BACKGROUND, 0, 440, 366, 40, 0, 440);
    xo_blit(XO_FRM_BACKGROUND, 60, 0, 104, 40, 366, 440);
}

static void xo_text(const char* text, int x, int y, int color)
{
    text_to_buf(xo_win_buf + XO_WIDTH * y + x, text, XO_WIDTH - x, XO_WIDTH, color);
}

static int xo_slider_knob_x(int sliderIndex)
{
    const XboxOptsSlider* slider = &(xo_sliders[sliderIndex]);
    int value = xo_values[XO_FIRST_SLIDER + sliderIndex];
    return XO_SLIDER_MIN_X + (value - slider->minValue) * XO_SLIDER_SPAN / (slider->maxValue - slider->minValue);
}

static void xo_draw()
{
    int normal = colorTable[18979];
    int highlight = colorTable[32747];

    memcpy(xo_win_buf, xo_background, XO_WIDTH * XO_HEIGHT);

    // Title, where the Preferences screen puts its own.
    text_font(104);
    const char* title = "XBOX OPTIONS";
    xo_text(title, std::max(0, (196 - text_width(title)) / 2), 10, normal);

    // Knob panels.
    for (int index = 0; index < XO_KNOB_COUNT; index++) {
        const XboxOptsKnob* knob = &(xo_knobs[index]);
        int item = XO_ITEM_DISPLAY + index;
        int color = item == xo_selected ? highlight : normal;

        text_font(103);
        xo_text(knob->label, 99 - text_width(knob->label) / 2, knob->labelY, color);

        // Same value label placement as a Preferences knob: left of it,
        // above it, right of it, below right.
        text_font(101);
        for (int value = 0; value < knob->valuesCount; value++) {
            const char* text = knob->values[value];
            int x = XO_KNOB_X + xo_knob_label_x[value];
            if (value == 0) {
                x -= text_width(text);
            } else if (value == 1) {
                x -= text_width(text) / 2;
            }
            xo_text(text, x, knob->knobY + xo_knob_label_y[value], xo_values[item] == value ? color : normal);
        }

        trans_buf_to_buf(xo_frm_data[XO_FRM_KNOB] + XO_KNOB_WIDTH * XO_KNOB_HEIGHT * xo_values[item],
            XO_KNOB_WIDTH,
            XO_KNOB_HEIGHT,
            XO_KNOB_WIDTH,
            xo_win_buf + XO_WIDTH * knob->knobY + XO_KNOB_X,
            XO_WIDTH);
    }

    // Sliders.
    for (int index = 0; index < XO_SLIDER_COUNT; index++) {
        const XboxOptsSlider* slider = &(xo_sliders[index]);
        int item = XO_FIRST_SLIDER + index;
        int color = item == xo_selected ? highlight : normal;
        char text[32];

        text_font(103);
        xo_text(slider->label, XO_SLIDER_MIN_X, slider->labelY, color);
        snprintf(text, sizeof(text), slider->format, xo_values[item]);
        xo_text(text, XO_SLIDER_RIGHT - text_width(text), slider->labelY, color);

        text_font(101);
        snprintf(text, sizeof(text), slider->format, slider->minValue);
        xo_text(text, XO_SLIDER_MIN_X, slider->knobY - 12, normal);
        snprintf(text, sizeof(text), slider->format, slider->maxValue);
        xo_text(text, XO_SLIDER_RIGHT - text_width(text), slider->knobY - 12, normal);

        XboxOptsFrm knobFrm = item == xo_dragging ? XO_FRM_SLIDER_ON : XO_FRM_SLIDER_OFF;
        trans_buf_to_buf(xo_frm_data[knobFrm],
            XO_SLIDER_KNOB_WIDTH,
            XO_SLIDER_KNOB_HEIGHT,
            XO_SLIDER_KNOB_WIDTH,
            xo_win_buf + XO_WIDTH * slider->knobY + xo_slider_knob_x(index),
            XO_WIDTH);
    }

    // Button plates, at the Preferences screen's text positions.
    text_font(103);
    xo_text("DEFAULT", 43, 449, normal);
    xo_text("DONE", 169, 449, normal);
    xo_text("CANCEL", 283, 449, normal);

    // Controller hints in the empty corner, or a restart warning while a
    // display setting is selected.
    text_font(101);
    const char* hints[2] = { "Y: DEV MENU", "BACK: CANCEL" };
    if (xo_selected == XO_ITEM_DISPLAY || xo_selected == XO_ITEM_SAFE_AREA) {
        hints[0] = "RESTARTS GAME";
        hints[1] = "Y: DEV MENU";
    }
    for (int index = 0; index < 2; index++) {
        xo_text(hints[index], 460 - text_width(hints[index]), 448 + index * (text_height() + 2), normal);
    }

    win_draw(xo_win);
}

static void xo_set_value(int item, int value)
{
    if (item < XO_FIRST_SLIDER) {
        value = std::clamp(value, 0, xo_knobs[item].valuesCount - 1);
    } else {
        const XboxOptsSlider* slider = &(xo_sliders[item - XO_FIRST_SLIDER]);
        value = std::clamp(value, slider->minValue, slider->maxValue);
    }

    if (xo_values[item] != value) {
        xo_values[item] = value;
        if (item < XO_FIRST_SLIDER) {
            // The Preferences knob click.
            gsound_play_sfx_file("ib2p1xx1");
        }

        // Let the player feel what they picked.
        if (item == XO_ITEM_RUMBLE) {
            GamepadRumbleAtStrength(0.6f, 0.3f, 150, value);
        } else if (item == XO_ITEM_POINTER_ASSIST && value == 2) {
            GamepadPointerHapticTick(xo_values[XO_ITEM_RUMBLE]);
        }
    }
}

// Drag a slider knob with the mouse until the button is released, like the
// Preferences screen does.
static void xo_drag_slider(int item)
{
    const XboxOptsSlider* slider = &(xo_sliders[item - XO_FIRST_SLIDER]);

    gsound_play_sfx_file("ib1p1xx1");
    xo_selected = item;
    xo_dragging = item;

    while (true) {
        sharedFpsLimiter.mark();

        get_input();

        int x;
        int y;
        mouseGetPositionInWindow(xo_win, &x, &y);

        int knobX = std::clamp(x - XO_SLIDER_KNOB_WIDTH / 2, XO_SLIDER_MIN_X, XO_SLIDER_MIN_X + XO_SLIDER_SPAN);
        int range = slider->maxValue - slider->minValue;
        int value = slider->minValue + ((knobX - XO_SLIDER_MIN_X) * range + XO_SLIDER_SPAN / 2) / XO_SLIDER_SPAN;
        xo_set_value(item, value);

        if (mouse_get_buttons() & MOUSE_EVENT_LEFT_BUTTON_UP) {
            break;
        }

        xo_draw();
        renderPresent();
        sharedFpsLimiter.throttle();
    }

    gsound_play_sfx_file("ib1lu1x1");
    xo_dragging = -1;
    xo_draw();
}

static void xo_load_values()
{
    xo_values[XO_ITEM_DISPLAY] = XO_DISPLAY_480P;
    xo_values[XO_ITEM_SAFE_AREA] = 0;

    Config resolutionConfig;
    if (config_init(&resolutionConfig)) {
        if (config_load(&resolutionConfig, xo_res_path, false)) {
            int width;
            int height;
            if (config_get_value(&resolutionConfig, "MAIN", "SCR_WIDTH", &width)
                && config_get_value(&resolutionConfig, "MAIN", "SCR_HEIGHT", &height)
                && width == 1280 && height == 720) {
                int uiScale = 0;
                config_get_value(&resolutionConfig, "XBOX", "UI_SCALE", &uiScale);
                xo_values[XO_ITEM_DISPLAY] = XO_DISPLAY_720P + std::clamp(uiScale, 0, 2);
            }
            config_get_value(&resolutionConfig, "XBOX", "SAFE_AREA", &(xo_values[XO_ITEM_SAFE_AREA]));
        }
        config_exit(&resolutionConfig);
    }

    xo_values[XO_ITEM_CENTER_CURSOR] = xo_default_values[XO_ITEM_CENTER_CURSOR];
    xo_values[XO_ITEM_POINTER_ASSIST] = xo_default_values[XO_ITEM_POINTER_ASSIST];
    xo_values[XO_ITEM_EDGE_SCROLL] = xo_default_values[XO_ITEM_EDGE_SCROLL];
    xo_values[XO_ITEM_SWAP_STICKS] = xo_default_values[XO_ITEM_SWAP_STICKS];
    xo_values[XO_ITEM_LEFT_DEADZONE] = 20;
    xo_values[XO_ITEM_RIGHT_DEADZONE] = 20;
    xo_values[XO_ITEM_LEFT_SENSITIVITY] = 50;
    xo_values[XO_ITEM_RIGHT_SENSITIVITY] = 50;
    xo_values[XO_ITEM_RUMBLE] = xo_default_values[XO_ITEM_RUMBLE];
    int pointerHaptics = 0;

    config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_RIGHT_STICK_CENTER_CURSOR_KEY, &(xo_values[XO_ITEM_CENTER_CURSOR]));
    config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_POINTER_ASSIST_KEY, &(xo_values[XO_ITEM_POINTER_ASSIST]));
    config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_EDGE_SCROLL_KEY, &(xo_values[XO_ITEM_EDGE_SCROLL]));
    config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_SWAP_STICKS_KEY, &(xo_values[XO_ITEM_SWAP_STICKS]));
    config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_LEFT_STICK_DEADZONE_KEY, &(xo_values[XO_ITEM_LEFT_DEADZONE]));
    config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_RIGHT_STICK_DEADZONE_KEY, &(xo_values[XO_ITEM_RIGHT_DEADZONE]));
    config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_LEFT_STICK_SENSITIVITY_KEY, &(xo_values[XO_ITEM_LEFT_SENSITIVITY]));
    config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_RIGHT_STICK_SENSITIVITY_KEY, &(xo_values[XO_ITEM_RIGHT_SENSITIVITY]));
    config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_RUMBLE_KEY, &(xo_values[XO_ITEM_RUMBLE]));
    config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_POINTER_HAPTICS_KEY, &pointerHaptics);

    // Clamp whatever the config held into the controls' ranges.
    for (int item = XO_ITEM_CENTER_CURSOR; item < XO_KNOB_COUNT; item++) {
        xo_values[item] = xo_values[item] != 0 ? 1 : 0;
    }
    // POINTER ASSIST: OFF, ON, ON with the haptic tick.
    if (xo_values[XO_ITEM_POINTER_ASSIST] != 0 && pointerHaptics != 0) {
        xo_values[XO_ITEM_POINTER_ASSIST] = 2;
    }
    for (int index = 0; index < XO_SLIDER_COUNT; index++) {
        const XboxOptsSlider* slider = &(xo_sliders[index]);
        int* value = &(xo_values[XO_FIRST_SLIDER + index]);
        *value = std::clamp(*value, slider->minValue, slider->maxValue);
    }
}

// Returns true if the game must relaunch for new display settings.
static bool xo_save_values(int initialDisplay, int initialSafeArea)
{
    debug_printf("Saving Xbox options: rumble=%d\n", xo_values[XO_ITEM_RUMBLE]);
    debug_printf("Saving Xbox options: display=%d safe_area=%d center_cursor=%d pointer_assist=%d edge_scroll=%d swap_sticks=%d left_deadzone=%d left_sensitivity=%d right_deadzone=%d right_sensitivity=%d\n",
        xo_values[XO_ITEM_DISPLAY],
        xo_values[XO_ITEM_SAFE_AREA],
        xo_values[XO_ITEM_CENTER_CURSOR],
        xo_values[XO_ITEM_POINTER_ASSIST],
        xo_values[XO_ITEM_EDGE_SCROLL],
        xo_values[XO_ITEM_SWAP_STICKS],
        xo_values[XO_ITEM_LEFT_DEADZONE],
        xo_values[XO_ITEM_LEFT_SENSITIVITY],
        xo_values[XO_ITEM_RIGHT_DEADZONE],
        xo_values[XO_ITEM_RIGHT_SENSITIVITY]);

    config_set_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_LEFT_STICK_DEADZONE_KEY, xo_values[XO_ITEM_LEFT_DEADZONE]);
    config_set_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_RIGHT_STICK_DEADZONE_KEY, xo_values[XO_ITEM_RIGHT_DEADZONE]);
    config_set_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_LEFT_STICK_SENSITIVITY_KEY, xo_values[XO_ITEM_LEFT_SENSITIVITY]);
    config_set_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_RIGHT_STICK_SENSITIVITY_KEY, xo_values[XO_ITEM_RIGHT_SENSITIVITY]);
    config_set_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_RIGHT_STICK_CENTER_CURSOR_KEY, xo_values[XO_ITEM_CENTER_CURSOR]);
    config_set_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_POINTER_ASSIST_KEY, xo_values[XO_ITEM_POINTER_ASSIST] != 0 ? 1 : 0);
    config_set_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_POINTER_HAPTICS_KEY, xo_values[XO_ITEM_POINTER_ASSIST] == 2 ? 1 : 0);
    config_set_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_RUMBLE_KEY, xo_values[XO_ITEM_RUMBLE]);
    config_set_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_EDGE_SCROLL_KEY, xo_values[XO_ITEM_EDGE_SCROLL]);
    config_set_value(&game_config, GAME_CONFIG_CONTROL_KEY, GAME_CONFIG_SWAP_STICKS_KEY, xo_values[XO_ITEM_SWAP_STICKS]);

    gconfig_save();
    GamepadInit();

    Config resolutionConfig;
    if (config_init(&resolutionConfig)) {
        config_load(&resolutionConfig, xo_res_path, false);
        bool hd = xo_values[XO_ITEM_DISPLAY] >= XO_DISPLAY_720P;
        config_set_value(&resolutionConfig, "MAIN", "SCR_WIDTH", hd ? 1280 : 640);
        config_set_value(&resolutionConfig, "MAIN", "SCR_HEIGHT", hd ? 720 : 480);
        config_set_value(&resolutionConfig, "XBOX", "UI_SCALE", hd ? xo_values[XO_ITEM_DISPLAY] - XO_DISPLAY_720P : 0);
        config_set_value(&resolutionConfig, "XBOX", "SAFE_AREA", xo_values[XO_ITEM_SAFE_AREA]);
        config_save(&resolutionConfig, xo_res_path, false);
        config_exit(&resolutionConfig);
    }

    return xo_values[XO_ITEM_DISPLAY] != initialDisplay || xo_values[XO_ITEM_SAFE_AREA] != initialSafeArea;
}

static bool xo_open()
{
    for (int index = 0; index < XO_FRM_COUNT; index++) {
        int fid = art_id(OBJ_TYPE_INTERFACE, xo_frm_ids[index], 0, 0, 0);
        xo_frm_data[index] = art_lock(fid, &(xo_frm_keys[index]), &(xo_frm_size[index].width), &(xo_frm_size[index].height));
        if (xo_frm_data[index] == NULL) {
            while (--index >= 0) {
                art_ptr_unlock(xo_frm_keys[index]);
            }
            return false;
        }
    }

    xo_background = (unsigned char*)mem_malloc(XO_WIDTH * XO_HEIGHT);
    if (xo_background == NULL) {
        for (int index = 0; index < XO_FRM_COUNT; index++) {
            art_ptr_unlock(xo_frm_keys[index]);
        }
        return false;
    }
    xo_build_background();

    xo_win = win_add((screenGetWidth() - XO_WIDTH) / 2,
        (screenGetHeight() - XO_HEIGHT) / 2,
        XO_WIDTH,
        XO_HEIGHT,
        256,
        WINDOW_MODAL | WINDOW_DONT_MOVE_TOP);
#ifdef NXDK
    // It navigates with the d-pad itself; padnav only hides the pointer.
    padnav_set_window_flags(xo_win, WINDOW_PAD_SELF_NAV);
    // B cancels (as Back does); Escape would apply.
    padnav_set_back_key(xo_win, SDL_SCANCODE_F11);
#endif
    if (xo_win == -1) {
        mem_free(xo_background);
        xo_background = NULL;
        for (int index = 0; index < XO_FRM_COUNT; index++) {
            art_ptr_unlock(xo_frm_keys[index]);
        }
        return false;
    }
    xo_win_buf = win_get_buf(xo_win);

    // Clicking a knob panel flips it; pressing on a slider drags it.
    for (int index = 0; index < XO_KNOB_COUNT; index++) {
        win_register_button(xo_win, 8, xo_knobs[index].labelY - 4, 182, 72, -1, -1, -1, XO_EVENT_ITEM + XO_ITEM_DISPLAY + index, NULL, NULL, NULL, 0);
    }
    for (int index = 0; index < XO_SLIDER_COUNT; index++) {
        win_register_button(xo_win, XO_SLIDER_MIN_X, xo_sliders[index].knobY - 12, 240, 23, -1, -1, XO_EVENT_ITEM + XO_FIRST_SLIDER + index, -1, NULL, NULL, NULL, 0);
    }

    // DEFAULT, DONE and CANCEL red buttons on the plates.
    static const int buttonX[3] = { 23, 148, 263 };
    static const int buttonEvent[3] = { XO_EVENT_DEFAULT, XO_EVENT_DONE, XO_EVENT_CANCEL };
    int doneButton = -1;
    for (int index = 0; index < 3; index++) {
        int btn = win_register_button(xo_win,
            buttonX[index],
            450,
            xo_frm_size[XO_FRM_RED_UP].width,
            xo_frm_size[XO_FRM_RED_UP].height,
            -1,
            -1,
            -1,
            buttonEvent[index],
            xo_frm_data[XO_FRM_RED_UP],
            xo_frm_data[XO_FRM_RED_DOWN],
            NULL,
            BUTTON_FLAG_TRANSPARENT);
        if (btn != -1) {
            win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
        }
        if (buttonEvent[index] == XO_EVENT_DONE) {
            doneButton = btn;
        }
    }

    // The hint corner opens the dev menu (Y does too).
    win_register_button(xo_win, 366, 440, XO_WIDTH - 366, 40, -1, -1, -1, XO_EVENT_DEV, NULL, NULL, NULL, 0);

    if (doneButton != -1) {
        warp_mouse_to_button(doneButton);
    }

    return true;
}

// --- Dev menu -------------------------------------------------------------
//
// Settings for trying things out, in a small window over XBOX OPTS built from
// the same Preferences art: the title band, four copies of a two-position
// knob panel, and the DONE plate. D-pad up/down picks, left/right (or A, or a
// click) turns a knob, START or DONE applies, B / BACK cancels.

#define XO_DEV_COUNT 4
#define XO_DEV_PANEL_TOP 117 // the PAN CENTERS CURSOR panel in the background
#define XO_DEV_PANEL_HEIGHT 78
#define XO_DEV_WIDTH 196
#define XO_DEV_HEIGHT (38 + XO_DEV_COUNT * XO_DEV_PANEL_HEIGHT + 40)
#define XO_DEV_EVENT_ITEM 640
#define XO_DEV_EVENT_DONE 650

static const XboxOptsKnob xo_dev_knobs[XO_DEV_COUNT] = {
    { "WALK STYLE", 2, { "HEX", "FREE" }, 0, 0 },
    { "FREE WALK PACE", 2, { "GLIDE", "STRIDE" }, 0, 0 },
    { "FREE WALK STOP", 2, { "SLIDE", "STEP" }, 0, 0 },
    // [debug] disable_fmv, inverted (gmovie_play reads it before each movie).
    { "MOVIES", 2, { "OFF", "ON" }, 0, 0 },
};

static int xo_dev_win = -1;
static unsigned char* xo_dev_buf;
static unsigned char* xo_dev_background;
static int xo_dev_values[XO_DEV_COUNT];
static int xo_dev_selected = 0;

static int xo_dev_panel_y(int index)
{
    return 38 + index * XO_DEV_PANEL_HEIGHT;
}

static void xo_dev_text(const char* text, int x, int y, int color)
{
    text_to_buf(xo_dev_buf + XO_DEV_WIDTH * y + x, text, XO_DEV_WIDTH - x, XO_DEV_WIDTH, color);
}

static void xo_dev_draw()
{
    int normal = colorTable[18979];
    int highlight = colorTable[32747];

    memcpy(xo_dev_buf, xo_dev_background, XO_DEV_WIDTH * XO_DEV_HEIGHT);

    text_font(104);
    const char* title = "DEV MENU";
    xo_dev_text(title, (XO_DEV_WIDTH - text_width(title)) / 2, 10, normal);

    for (int index = 0; index < XO_DEV_COUNT; index++) {
        const XboxOptsKnob* knob = &(xo_dev_knobs[index]);
        int top = xo_dev_panel_y(index);
        int color = index == xo_dev_selected ? highlight : normal;
        int knobY = top + 32;

        text_font(103);
        xo_dev_text(knob->label, 99 - text_width(knob->label) / 2, top + 8, color);

        text_font(101);
        for (int value = 0; value < knob->valuesCount; value++) {
            const char* text = knob->values[value];
            int x = XO_KNOB_X + xo_knob_label_x[value];
            if (value == 0) {
                x -= text_width(text);
            } else if (value == 1) {
                x -= text_width(text) / 2;
            }
            xo_dev_text(text, x, knobY + xo_knob_label_y[value], xo_dev_values[index] == value ? color : normal);
        }

        trans_buf_to_buf(xo_frm_data[XO_FRM_KNOB] + XO_KNOB_WIDTH * XO_KNOB_HEIGHT * xo_dev_values[index],
            XO_KNOB_WIDTH,
            XO_KNOB_HEIGHT,
            XO_KNOB_WIDTH,
            xo_dev_buf + XO_DEV_WIDTH * knobY + XO_KNOB_X,
            XO_DEV_WIDTH);
    }

    text_font(103);
    xo_dev_text("DONE", 49 + 32, XO_DEV_HEIGHT - 31, normal);

    win_draw(xo_dev_win);
}

// The hidden pointer on the selected knob, so A (a click there) turns it.
static void xo_dev_park_pointer()
{
    Rect rect;
    if (win_get_rect(xo_dev_win, &rect) == 0) {
        mouse_set_position(rect.ulx + 99, rect.uly + xo_dev_panel_y(xo_dev_selected) + 40);
    }
}

static void xo_dev_menu()
{
    xo_dev_background = (unsigned char*)mem_malloc(XO_DEV_WIDTH * XO_DEV_HEIGHT);
    if (xo_dev_background == NULL) {
        return;
    }

    // Title band, three knob panels, and a strip of plain texture with the
    // DONE plate in the middle.
    buf_to_buf(xo_background, XO_DEV_WIDTH, 38, XO_WIDTH, xo_dev_background, XO_DEV_WIDTH);
    for (int index = 0; index < XO_DEV_COUNT; index++) {
        buf_to_buf(xo_background + XO_WIDTH * XO_DEV_PANEL_TOP,
            XO_DEV_WIDTH,
            XO_DEV_PANEL_HEIGHT,
            XO_WIDTH,
            xo_dev_background + XO_DEV_WIDTH * xo_dev_panel_y(index),
            XO_DEV_WIDTH);
    }
    unsigned char* strip = xo_dev_background + XO_DEV_WIDTH * (XO_DEV_HEIGHT - 40);
    buf_to_buf(xo_background + XO_WIDTH * 440 + 366, 104, 40, XO_WIDTH, strip, XO_DEV_WIDTH);
    buf_to_buf(xo_background + XO_WIDTH * 440 + 366, XO_DEV_WIDTH - 104, 40, XO_WIDTH, strip + 104, XO_DEV_WIDTH);
    buf_to_buf(xo_background + XO_WIDTH * 440 + 137, 98, 40, XO_WIDTH, strip + 49, XO_DEV_WIDTH);

    xo_dev_win = win_add((screenGetWidth() - XO_DEV_WIDTH) / 2,
        (screenGetHeight() - XO_DEV_HEIGHT) / 2,
        XO_DEV_WIDTH,
        XO_DEV_HEIGHT,
        256,
        WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (xo_dev_win == -1) {
        mem_free(xo_dev_background);
        xo_dev_background = NULL;
        return;
    }
#ifdef NXDK
    padnav_set_window_flags(xo_dev_win, WINDOW_PAD_SELF_NAV);
    padnav_set_back_key(xo_dev_win, SDL_SCANCODE_F11);
#endif
    xo_dev_buf = win_get_buf(xo_dev_win);

    for (int index = 0; index < XO_DEV_COUNT; index++) {
        win_register_button(xo_dev_win, 8, xo_dev_panel_y(index) + 4, 182, 72, -1, -1, -1, XO_DEV_EVENT_ITEM + index, NULL, NULL, NULL, 0);
    }
    int doneButton = win_register_button(xo_dev_win,
        49 + 11,
        XO_DEV_HEIGHT - 30,
        xo_frm_size[XO_FRM_RED_UP].width,
        xo_frm_size[XO_FRM_RED_UP].height,
        -1,
        -1,
        -1,
        XO_DEV_EVENT_DONE,
        xo_frm_data[XO_FRM_RED_UP],
        xo_frm_data[XO_FRM_RED_DOWN],
        NULL,
        BUTTON_FLAG_TRANSPARENT);
    if (doneButton != -1) {
        win_register_button_sound_func(doneButton, gsound_red_butt_press, gsound_red_butt_release);
    }

    xo_dev_values[0] = stick_walk_free_style() ? 1 : 0;
    xo_dev_values[1] = stick_walk_dev_pace();
    xo_dev_values[2] = stick_walk_dev_stop();
    int disableFmv = 0;
    config_get_value(&game_config, GAME_CONFIG_DEBUG_KEY, GAME_CONFIG_DISABLE_FMV_KEY, &disableFmv);
    xo_dev_values[3] = disableFmv != 0 ? 0 : 1;
    xo_dev_selected = 0;
    xo_dev_draw();
    xo_dev_park_pointer();

#ifdef NXDK
    // Without the pointer (pad mode) A and B are read here: walk mode would
    // take a hidden-pointer A as "use the highlighted object".
    GamepadSetScreenCapture(true);
    int aPresses = GamepadButtonPresses(SDL_GAMEPAD_BUTTON_SOUTH);
    int bPresses = GamepadButtonPresses(SDL_GAMEPAD_BUTTON_EAST);
#endif

    bool done = false;
    bool save = false;
    while (!done) {
        sharedFpsLimiter.mark();

        int keyCode = get_input();
        bool redraw = true;
#ifdef NXDK
        if (GamepadButtonPresses(SDL_GAMEPAD_BUTTON_SOUTH) != aPresses) {
            aPresses = GamepadButtonPresses(SDL_GAMEPAD_BUTTON_SOUTH);
            if (GamepadScreenCaptureActive()) {
                keyCode = XO_DEV_EVENT_ITEM + xo_dev_selected;
            }
        }
        if (GamepadButtonPresses(SDL_GAMEPAD_BUTTON_EAST) != bPresses) {
            bPresses = GamepadButtonPresses(SDL_GAMEPAD_BUTTON_EAST);
            if (GamepadScreenCaptureActive()) {
                keyCode = KEY_F11;
            }
        }
#endif
        switch (keyCode) {
        case KEY_ARROW_UP:
            xo_dev_selected = (xo_dev_selected + XO_DEV_COUNT - 1) % XO_DEV_COUNT;
            xo_dev_park_pointer();
            break;
        case KEY_ARROW_DOWN:
            xo_dev_selected = (xo_dev_selected + 1) % XO_DEV_COUNT;
            xo_dev_park_pointer();
            break;
        case KEY_ARROW_LEFT:
        case KEY_ARROW_RIGHT:
            xo_dev_values[xo_dev_selected] = keyCode == KEY_ARROW_RIGHT ? 1 : 0;
            gsound_play_sfx_file("ib1p1xx1");
            break;
        case KEY_ESCAPE: // START
            gsound_play_sfx_file("ib1p1xx1");
            // FALLTHROUGH
        case XO_DEV_EVENT_DONE:
            save = true;
            done = true;
            break;
        case KEY_F11: // BACK, B
            done = true;
            break;
        default:
            if (keyCode >= XO_DEV_EVENT_ITEM && keyCode < XO_DEV_EVENT_ITEM + XO_DEV_COUNT) {
                int index = keyCode - XO_DEV_EVENT_ITEM;
                xo_dev_selected = index;
                xo_dev_values[index] = 1 - xo_dev_values[index];
                gsound_play_sfx_file("ib1p1xx1");
            } else {
                redraw = false;
            }
            break;
        }

        if (redraw && !done) {
            xo_dev_draw();
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

#ifdef NXDK
    GamepadSetScreenCapture(false);
#endif

    if (save) {
        stick_walk_set_free_style_nosave(xo_dev_values[0] != 0);
        stick_walk_set_dev(xo_dev_values[1], xo_dev_values[2]);
        config_set_value(&game_config, GAME_CONFIG_DEBUG_KEY, GAME_CONFIG_DISABLE_FMV_KEY, xo_dev_values[3] != 0 ? 0 : 1);
        gconfig_save();
    }

    win_delete(xo_dev_win);
    xo_dev_win = -1;
    xo_dev_buf = NULL;
    mem_free(xo_dev_background);
    xo_dev_background = NULL;
}

static void xo_close()
{
    win_delete(xo_win);
    xo_win = -1;
    xo_win_buf = NULL;

    mem_free(xo_background);
    xo_background = NULL;

    for (int index = 0; index < XO_FRM_COUNT; index++) {
        art_ptr_unlock(xo_frm_keys[index]);
    }
}

void xbox_options_menu()
{
    debug_printf("xbox_options_menu\n");

    int oldFont = text_curr();

    xo_load_values();
    int initialDisplay = xo_values[XO_ITEM_DISPLAY];
    int initialSafeArea = xo_values[XO_ITEM_SAFE_AREA];

    if (!xo_open()) {
        debug_printf("xbox_options_menu: can't load the Preferences art\n");
        text_font(oldFont);
        return;
    }

    xo_selected = XO_ITEM_DISPLAY;
    xo_dragging = -1;
    xo_draw();

    bool done = false;
    bool save = false;

    while (!done) {
        sharedFpsLimiter.mark();

        int keyCode = get_input();
        bool redraw = true;

        switch (keyCode) {
        case KEY_ARROW_UP:
            xo_selected = (xo_selected - 1 + XO_ITEM_COUNT) % XO_ITEM_COUNT;
            break;
        case KEY_ARROW_DOWN:
            xo_selected = (xo_selected + 1) % XO_ITEM_COUNT;
            break;
        case KEY_ARROW_LEFT:
            xo_set_value(xo_selected, xo_values[xo_selected] - 1);
            break;
        case KEY_ARROW_RIGHT:
            xo_set_value(xo_selected, xo_values[xo_selected] + 1);
            break;
        case XO_EVENT_DEFAULT:
            for (int item = XO_ITEM_CENTER_CURSOR; item < XO_ITEM_COUNT; item++) {
                xo_values[item] = xo_default_values[item];
            }
            break;
        // START sends Escape, which applies here as it always has.
        case KEY_ESCAPE:
            gsound_play_sfx_file("ib1p1xx1");
            // FALLTHROUGH
        case XO_EVENT_DONE:
            save = true;
            done = true;
            break;
        // BACK sends F11 (the menus wheel's key in the game).
        case KEY_F11:
        case XO_EVENT_CANCEL:
            done = true;
            break;
        // Y sends B.
        case KEY_LOWERCASE_B:
        case KEY_UPPERCASE_B:
        case XO_EVENT_DEV:
            xo_dev_menu();
            break;
        default:
            if (keyCode >= XO_EVENT_ITEM && keyCode < XO_EVENT_ITEM + XO_ITEM_COUNT) {
                int item = keyCode - XO_EVENT_ITEM;
                if (item < XO_FIRST_SLIDER) {
                    // Clicking a knob turns it to the next position.
                    xo_selected = item;
                    xo_set_value(item, (xo_values[item] + 1) % xo_knobs[item].valuesCount);
                } else {
                    xo_drag_slider(item);
                }
            } else {
                redraw = false;
            }
            break;
        }

        if (redraw && !done) {
            xo_draw();
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    bool relaunch = false;
    if (save) {
        relaunch = xo_save_values(initialDisplay, initialSafeArea);
    }

    xo_close();
    text_font(oldFont);

    if (relaunch) {
        debug_printf("Display settings changed, relaunching default.xbe\n");
        XLaunchXBE(".\\default.xbe");
    }
}

} // namespace fallout

#endif
