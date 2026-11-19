#include "game/skillwheel.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "game/actions.h"
#include "game/art.h"
#include "game/cycle.h"
#include "game/game.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/intface.h"
#include "game/map.h"
#include "game/message.h"
#include "game/object.h"
#include "game/padglyph.h"
#include "game/skill.h"
#include "game/skilldex.h"
#include "game/stickwalk.h"
#include "platform_compat.h"
#include "plib/color/color.h"
#include "plib/gnw/gamepad.hpp"
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/input.h"
#include "plib/gnw/kb.h"
#include "plib/gnw/memory.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/text.h"

namespace fallout {

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Clockwise from the top, in Skilldex order.
#define WHEEL_SLOTS 8

// Skilldex art (skilldex.cc grphfid): the button, up and pressed.
#define WHEEL_FRM_BUTTON_UP 120
#define WHEEL_FRM_BUTTON_DOWN 119

// Stick deflection that picks a slot.
#define WHEEL_STICK_PICK 0.5f

static const int wheel_skills[WHEEL_SLOTS] = {
    SKILL_SNEAK,
    SKILL_LOCKPICK,
    SKILL_STEAL,
    SKILL_TRAPS,
    SKILL_FIRST_AID,
    SKILL_DOCTOR,
    SKILL_SCIENCE,
    SKILL_REPAIR,
};

// SKILLDEX_RC_* for each slot (the same order).
static const int wheel_rcs[WHEEL_SLOTS] = {
    SKILLDEX_RC_SNEAK,
    SKILLDEX_RC_LOCKPICK,
    SKILLDEX_RC_STEAL,
    SKILLDEX_RC_TRAPS,
    SKILLDEX_RC_FIRST_AID,
    SKILLDEX_RC_DOCTOR,
    SKILLDEX_RC_SCIENCE,
    SKILLDEX_RC_REPAIR,
};

// The game mouse mode that picks a target for each slot (-1: Sneak needs
// none).
static const int wheel_modes[WHEEL_SLOTS] = {
    -1,
    GAME_MOUSE_MODE_USE_LOCKPICK,
    GAME_MOUSE_MODE_USE_STEAL,
    GAME_MOUSE_MODE_USE_TRAPS,
    GAME_MOUSE_MODE_USE_FIRST_AID,
    GAME_MOUSE_MODE_USE_DOCTOR,
    GAME_MOUSE_MODE_USE_SCIENCE,
    GAME_MOUSE_MODE_USE_REPAIR,
};

typedef struct Wheel Wheel;

// Writes the center line (what A will do) for the current selection.
typedef void WheelHintProc(Wheel* wheel, char* line, int size);

struct Wheel {
    int win;
    int width;
    int height;
    unsigned char* buf;
    // The map under the window, dimmed (GNW only does transparent windows
    // when back-buffering, which isn't used, so the wheel draws over a copy).
    unsigned char* background;

    unsigned char* up;
    unsigned char* down;
    CacheEntry* upKey;
    CacheEntry* downKey;
    int buttonWidth;
    int buttonHeight;

    // Slot button positions (upper left) in the window.
    int x[WHEEL_SLOTS];
    int y[WHEEL_SLOTS];

    char names[WHEEL_SLOTS][64];
    // A line under each button (the skill level), or empty.
    char subs[WHEEL_SLOTS][32];
    char title[64];
    WheelHintProc* hint;
    // A key that closes it again (the one that opened it), besides B / Esc.
    int closeKey;
    int selected; // -1 for none yet
};

// What the slot's skill would be used on right now, for the center text.
static Object* wheel_target(int slot)
{
    if (wheel_modes[slot] == -1) {
        return NULL;
    }

    Object* target = gmouse_near_target_for_mode(wheel_modes[slot]);
    if (target == NULL && (wheel_skills[slot] == SKILL_FIRST_AID || wheel_skills[slot] == SKILL_DOCTOR)) {
        target = obj_dude;
    }
    return target;
}

// Text centered at (centerX, y) in the window, outlined in black so it reads
// over the map: drawn and outlined on a blank scratch buffer, then copied
// over (0 is transparent).
static void wheel_text(Wheel* wheel, const char* text, int centerX, int y, int color)
{
    int width = text_width(text) + 2;
    int height = text_height() + 2;
    int x = centerX - width / 2;
    if (x < 0 || x + width > wheel->width || y < 1 || y - 1 + height > wheel->height) {
        return;
    }

    static unsigned char scratch[320 * 40];
    if (width > 320 || height > 40) {
        return;
    }
    memset(scratch, 0, width * height);
    text_to_buf(scratch + width + 1, text, width - 1, width, color);
    buf_outline(scratch, width, height, width, colorTable[0]);
    trans_buf_to_buf(scratch, width, height, width, wheel->buf + wheel->width * (y - 1) + x, wheel->width);
}

// A button glyph followed by text, the pair centred on centerX; `y` is the
// glyph's top (the text is centred on it).
static void wheel_glyph_text(Wheel* wheel, PadGlyph glyph, const char* text, int centerX, int y, int color)
{
    int glyphSize = pad_glyph_size();
    int gap = 3;
    int textWidth = text_width(text) + 2;
    int left = centerX - (glyphSize + gap + textWidth) / 2;
    pad_glyph_draw(wheel->buf, wheel->width, wheel->width, wheel->height, left, y, glyph);
    wheel_text(wheel, text, left + glyphSize + gap + textWidth / 2, y + (glyphSize - text_height()) / 2, color);
}

static void wheel_draw(Wheel* wheel)
{
    memcpy(wheel->buf, wheel->background, wheel->width * wheel->height);

    for (int slot = 0; slot < WHEEL_SLOTS; slot++) {
        bool selected = slot == wheel->selected;
        unsigned char* art = selected ? wheel->down : wheel->up;
        unsigned char* dest = wheel->buf + wheel->width * wheel->y[slot] + wheel->x[slot];
        trans_buf_to_buf(art, wheel->buttonWidth, wheel->buttonHeight, wheel->buttonWidth, dest, wheel->width);

        // A gold frame (two pixels) around the selected one: the pressed art
        // alone is only a shade darker.
        if (selected) {
            int left = wheel->x[slot] - 2;
            int top = wheel->y[slot] - 2;
            int right = wheel->x[slot] + wheel->buttonWidth + 1;
            int bottom = wheel->y[slot] + wheel->buttonHeight + 1;
            if (left >= 0 && top >= 0 && right < wheel->width && bottom < wheel->height) {
                draw_box(wheel->buf, wheel->width, left, top, right, bottom, colorTable[32747]);
                draw_box(wheel->buf, wheel->width, left + 1, top + 1, right - 1, bottom - 1, colorTable[32747]);
            }
        }

        // Name on the button, as the Skilldex draws it (in the small font
        // when the big one doesn't fit, e.g. "INVENTORY").
        text_font(103);
        if (text_width(wheel->names[slot]) > wheel->buttonWidth - 6) {
            text_font(101);
        }
        int nameWidth = text_width(wheel->names[slot]);
        int nameX = (wheel->buttonWidth - nameWidth) / 2 + 1;
        int nameY = (wheel->buttonHeight - text_height()) / 2 + 1;
        if (nameX < 0) {
            nameX = 0;
        }
        text_to_buf(dest + wheel->width * nameY + nameX,
            wheel->names[slot],
            wheel->buttonWidth - nameX,
            wheel->width,
            colorTable[selected ? 32747 : 18979]);

        // The line under it (the skill level).
        if (wheel->subs[slot][0] != '\0') {
            text_font(101);
            wheel_text(wheel, wheel->subs[slot], wheel->x[slot] + wheel->buttonWidth / 2, wheel->y[slot] + wheel->buttonHeight + 2,
                colorTable[selected ? 32747 : 21140]);
        }
    }

    // Center: title, then what A will do.
    int centerX = wheel->width / 2;
    int centerY = wheel->height / 2;

    text_font(103);
    wheel_text(wheel, wheel->title, centerX, centerY - 20, colorTable[18979]);

    text_font(101);
    char line[128];
    wheel->hint(wheel, line, sizeof(line));
    // "A: ..." hints get the A button's glyph instead of the "A:".
    if (strncmp(line, "A: ", 3) == 0) {
        wheel_glyph_text(wheel, PAD_GLYPH_A, line + 3, centerX, centerY - 1, colorTable[32747]);
    } else {
        wheel_text(wheel, line, centerX, centerY, colorTable[32747]);
    }
    wheel_glyph_text(wheel, PAD_GLYPH_B, "Close", centerX, centerY + 14, colorTable[21140]);

    win_draw(wheel->win);
}

static void wheel_init(Wheel* wheel)
{
    memset(wheel, 0, sizeof(*wheel));
    wheel->win = -1;
    wheel->selected = -1;
    wheel->closeKey = -1;
}

// Opens the window around the player, once names, title and hint are set.
// False if anything is missing.
static bool wheel_open(Wheel* wheel)
{
    int upWidth;
    int upHeight;
    int downWidth;
    int downHeight;
    wheel->up = art_lock(art_id(OBJ_TYPE_INTERFACE, WHEEL_FRM_BUTTON_UP, 0, 0, 0), &(wheel->upKey), &upWidth, &upHeight);
    wheel->down = art_lock(art_id(OBJ_TYPE_INTERFACE, WHEEL_FRM_BUTTON_DOWN, 0, 0, 0), &(wheel->downKey), &downWidth, &downHeight);
    if (wheel->up == NULL || wheel->down == NULL || upWidth != downWidth || upHeight != downHeight) {
        if (wheel->up != NULL) {
            art_ptr_unlock(wheel->upKey);
        }
        if (wheel->down != NULL) {
            art_ptr_unlock(wheel->downKey);
        }
        return false;
    }
    wheel->buttonWidth = upWidth;
    wheel->buttonHeight = upHeight;

    // Slots on an ellipse wide enough that neighbours don't overlap: at 45
    // degrees apart, horizontal neighbours are 0.71 rx apart and vertical
    // ones 0.71 ry (plus the level line under each button).
    int slotHeight = wheel->buttonHeight + 14;
    int rx = (int)(wheel->buttonWidth * 1.5f);
    int ry = (int)(slotHeight * 1.5f);
    wheel->width = 2 * rx + wheel->buttonWidth + 8;
    wheel->height = 2 * ry + slotHeight + 8;
    for (int slot = 0; slot < WHEEL_SLOTS; slot++) {
        double angle = slot * (M_PI / 4.0);
        int centerX = wheel->width / 2 + (int)(rx * sin(angle));
        int centerY = wheel->height / 2 - (int)(ry * cos(angle));
        wheel->x[slot] = centerX - wheel->buttonWidth / 2;
        wheel->y[slot] = centerY - slotHeight / 2;
    }

    // Centered on the player (the middle of the sprite), kept on the map view.
    int mapWidth = screenGetWidth();
    int mapHeight = screenGetHeight() - INTERFACE_BAR_HEIGHT;
    int centerX = mapWidth / 2;
    int centerY = mapHeight / 2;
    if (obj_dude != NULL) {
        Rect bounds;
        obj_bound(obj_dude, &bounds);
        centerX = (bounds.ulx + bounds.lrx) / 2;
        centerY = (bounds.uly + bounds.lry) / 2;
    }
    int x = centerX - wheel->width / 2;
    int y = centerY - wheel->height / 2;
    if (x + wheel->width > mapWidth) {
        x = mapWidth - wheel->width;
    }
    if (y + wheel->height > mapHeight) {
        y = mapHeight - wheel->height;
    }
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }

    // Copy the map under it first, dimmed so the wheel stands out.
    wheel->background = (unsigned char*)mem_malloc(wheel->width * wheel->height);
    if (wheel->background == NULL) {
        art_ptr_unlock(wheel->upKey);
        art_ptr_unlock(wheel->downKey);
        return false;
    }
    memset(wheel->background, colorTable[0], wheel->width * wheel->height);
    Rect mapRect;
    unsigned char* mapBuf = win_get_buf(display_win);
    if (mapBuf != NULL && win_get_rect(display_win, &mapRect) == 0) {
        int mapPitch = mapRect.lrx - mapRect.ulx + 1;
        int mapRows = mapRect.lry - mapRect.uly + 1;
        for (int row = 0; row < wheel->height; row++) {
            int mapY = y + row - mapRect.uly;
            if (mapY < 0 || mapY >= mapRows) {
                continue;
            }
            for (int column = 0; column < wheel->width; column++) {
                int mapX = x + column - mapRect.ulx;
                if (mapX < 0 || mapX >= mapPitch) {
                    continue;
                }
                wheel->background[row * wheel->width + column] = intensityColorTable[mapBuf[mapY * mapPitch + mapX]][80];
            }
        }
    }

    wheel->win = win_add(x, y, wheel->width, wheel->height, 0, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (wheel->win == -1) {
        mem_free(wheel->background);
        art_ptr_unlock(wheel->upKey);
        art_ptr_unlock(wheel->downKey);
        return false;
    }
    wheel->buf = win_get_buf(wheel->win);
    return true;
}

static void wheel_close(Wheel* wheel)
{
    win_delete(wheel->win);
    mem_free(wheel->background);
    art_ptr_unlock(wheel->upKey);
    art_ptr_unlock(wheel->downKey);
}

// The slot the stick points at, or -1 if it's not pushed far enough.
static int wheel_stick_slot()
{
    // The wheels are steered with the physical left stick in every mode.
    float x;
    float y;
    GamepadGetPhysicalLeftStick(&x, &y);
    if (x * x + y * y < WHEEL_STICK_PICK * WHEEL_STICK_PICK) {
        return -1;
    }

    // Clockwise from up.
    double angle = atan2(x, y);
    if (angle < 0) {
        angle += 2.0 * M_PI;
    }
    return (int)floor(angle / (M_PI / 4.0) + 0.5) % WHEEL_SLOTS;
}

// Shows the wheel and runs it until something is picked (returns the slot)
// or it's closed (-1). -2 if it couldn't be shown.
static int wheel_run(Wheel* wheel)
{
    if (!wheel_open(wheel)) {
        return -2;
    }

    int oldFont = text_curr();
    bool bkEnabled = map_disable_bk_processes();
    cycle_disable();

    // The pad's A, stick and walking go to the wheel, not the hidden pointer
    // or the map.
    GamepadSetUiCapture(true);
    stick_walk_pause(true);

    wheel_draw(wheel);

    int picked = -1;
    // Ignore a press that was already down when the wheel opened.
    bool clickWasHeld = GamepadClickHeld();
    bool backWasHeld = GamepadBackHeld();
    bool done = false;
    while (!done) {
        sharedFpsLimiter.mark();

        int key = get_input();
        int selected = wheel->selected;

        if (key == KEY_ESCAPE || (key != -1 && key == wheel->closeKey)) {
            done = true;
        }

        switch (key) {
        case KEY_ARROW_RIGHT:
        case KEY_ARROW_DOWN:
            selected = selected == -1 ? 0 : (selected + 1) % WHEEL_SLOTS;
            break;
        case KEY_ARROW_LEFT:
        case KEY_ARROW_UP:
            selected = selected == -1 ? WHEEL_SLOTS - 1 : (selected + WHEEL_SLOTS - 1) % WHEEL_SLOTS;
            break;
        }

        // B closes (read from the pad: with the pointer hidden the mouse
        // layer reports no buttons).
        bool backHeld = GamepadBackHeld();
        if (backHeld && !backWasHeld) {
            done = true;
        }
        backWasHeld = backHeld;

        if (game_user_wants_to_quit != 0) {
            done = true;
        }

        int stickSlot = wheel_stick_slot();
        if (stickSlot != -1) {
            selected = stickSlot;
        }

        if (selected != wheel->selected) {
            wheel->selected = selected;
            gsound_play_sfx_file("ib1p1xx1");
            wheel_draw(wheel);
        }

        bool clickHeld = GamepadClickHeld();
        if (!done && clickHeld && !clickWasHeld && wheel->selected != -1) {
            gsound_lrg_butt_press(-1, -1);
            picked = wheel->selected;
            done = true;
        }
        clickWasHeld = clickHeld;

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    wheel_close(wheel);
    stick_walk_pause(false);
    GamepadSetUiCapture(false);
    text_font(oldFont);
    cycle_enable();
    if (bkEnabled) {
        map_enable_bk_processes();
    }

    return picked;
}

static void skill_wheel_hint(Wheel* wheel, char* line, int size)
{
    if (wheel->selected == -1) {
        snprintf(line, size, "Pick a skill");
    } else if (wheel_modes[wheel->selected] == -1) {
        snprintf(line, size, "A: Toggle");
    } else {
        Object* target = wheel_target(wheel->selected);
        if (target == obj_dude) {
            snprintf(line, size, "A: On yourself");
        } else if (target != NULL) {
            const char* name = object_name(target);
            snprintf(line, size, "A: On %s", name != NULL ? name : "it");
        } else {
            snprintf(line, size, "A: Pick a target");
        }
    }
}

int skill_wheel_select()
{
    Wheel wheel;
    wheel_init(&wheel);
    wheel.hint = skill_wheel_hint;
    wheel.closeKey = KEY_LOWERCASE_S;

    // Names from skilldex.msg (102.. in Skilldex order, 100 the title).
    MessageList messages;
    MessageListItem item;
    bool loaded = false;
    if (message_init(&messages)) {
        char path[COMPAT_MAX_PATH];
        snprintf(path, sizeof(path), "%s%s", msg_path, "skilldex.msg");
        loaded = message_load(&messages, path);
    }
    for (int slot = 0; slot < WHEEL_SLOTS; slot++) {
        const char* name = loaded ? getmsg(&messages, &item, 102 + slot) : skill_name(wheel_skills[slot]);
        snprintf(wheel.names[slot], sizeof(wheel.names[slot]), "%s", name != NULL ? name : "");

        int value = skill_level(obj_dude, wheel_skills[slot]);
        snprintf(wheel.subs[slot], sizeof(wheel.subs[slot]), "%d%%", value < 0 ? 0 : value);
    }
    snprintf(wheel.title, sizeof(wheel.title), "%s", loaded ? getmsg(&messages, &item, 100) : "SKILLDEX");
    message_exit(&messages);

    int slot = wheel_run(&wheel);
    if (slot == -2) {
        // Fall back to the Skilldex window.
        return skilldex_select();
    }
    return slot == -1 ? SKILLDEX_RC_CANCELED : wheel_rcs[slot];
}

// The menus wheel (BACK): the game's screens and save/load, each opened by
// sending its hotkey through game_handle_input.
static const char* menus_names[WHEEL_SLOTS] = {
    "INVENTORY",
    "PIP-BOY",
    "CHARACTER",
    "MAP",
    "SKILLDEX",
    "OPTIONS",
    "LOAD GAME",
    "SAVE GAME",
};

static const int menus_keys[WHEEL_SLOTS] = {
    KEY_LOWERCASE_I,
    KEY_LOWERCASE_P,
    KEY_LOWERCASE_C,
    KEY_TAB,
    KEY_LOWERCASE_S,
    KEY_ESCAPE,
    KEY_F5,
    KEY_F4,
};

static void menus_wheel_hint(Wheel* wheel, char* line, int size)
{
    snprintf(line, size, "%s", wheel->selected == -1 ? "Pick a menu" : "A: Open");
}

int menus_wheel_select(int openKey)
{
    Wheel wheel;
    wheel_init(&wheel);
    wheel.hint = menus_wheel_hint;
    wheel.closeKey = openKey;
    snprintf(wheel.title, sizeof(wheel.title), "%s", "MENUS");
    for (int slot = 0; slot < WHEEL_SLOTS; slot++) {
        snprintf(wheel.names[slot], sizeof(wheel.names[slot]), "%s", menus_names[slot]);
    }

    int slot = wheel_run(&wheel);
    return slot >= 0 ? menus_keys[slot] : -1;
}

void skill_wheel_use(int rc)
{
    int slot = -1;
    for (int index = 0; index < WHEEL_SLOTS; index++) {
        if (wheel_rcs[index] == rc) {
            slot = index;
        }
    }
    if (slot == -1) {
        return;
    }

    if (wheel_modes[slot] == -1) {
        action_skill_use(wheel_skills[slot]);
        return;
    }

    Object* target = wheel_target(slot);
    if (target != NULL) {
        action_use_skill_on(obj_dude, target, wheel_skills[slot]);
        return;
    }

    // Nothing suitable next to the player: the usual skill cursor.
    gmouse_walk_show_pointer(true);
    gmouse_set_cursor(MOUSE_CURSOR_USE_CROSSHAIR);
    gmouse_3d_set_mode(wheel_modes[slot]);
}

} // namespace fallout
