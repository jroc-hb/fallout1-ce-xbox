#include "game/padkeys.h"

#ifdef NXDK

#include <string.h>

#include <SDL3/SDL.h>

#include "game/gsound.h"
#include "game/padglyph.h"
#include "plib/color/color.h"
#include "plib/gnw/gamepad.hpp"
#include "plib/gnw/gnw.h"
#include "plib/gnw/gnw_types.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/input.h"
#include "plib/gnw/kb.h"
#include "plib/gnw/padnav.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/text.h"

namespace fallout {

// Four rows of characters, then the special keys, each a run of cells on
// the bottom row.
#define PK_COLUMNS 10
#define PK_ROWS 5
#define PK_CELL_W 26
#define PK_CELL_H 20
#define PK_GAP 2
#define PK_MARGIN 8
#define PK_HINT_LINE 16
#define PK_HINT_LINES 2

#define PK_RGB15(r, g, b) (((r) << 10) | ((g) << 5) | (b))

static const char* const pk_rows_lower[4] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl-",
    "zxcvbnm.'?",
};
static const char* const pk_rows_upper[4] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL-",
    "ZXCVBNM.'?",
};

enum {
    PK_SHIFT,
    PK_SPACE,
    PK_DELETE,
    PK_DONE,
    PK_SPECIAL_COUNT,
};

static const char* const pk_special_labels[PK_SPECIAL_COUNT] = { "SHIFT", "SPACE", "DEL", "DONE" };
static const int pk_special_start[PK_SPECIAL_COUNT] = { 0, 2, 6, 8 };
static const int pk_special_span[PK_SPECIAL_COUNT] = { 2, 4, 2, 2 };

typedef struct PkHint {
    PadGlyph glyph;
    const char* text;
} PkHint;

static const PkHint pk_hints[PK_HINT_LINES][4] = {
    { { PAD_GLYPH_A, "Type" }, { PAD_GLYPH_B, "Delete" }, { PAD_GLYPH_X, "Space" }, { PAD_GLYPH_Y, "Shift" } },
    { { PAD_GLYPH_START, "Done" }, { PAD_GLYPH_BACK, "Cancel" }, { PAD_GLYPH_COUNT, NULL }, { PAD_GLYPH_COUNT, NULL } },
};

static int pk_win = -1;
static int pk_width;
static int pk_height;
static int pk_row = 1;
static int pk_column = 0;
static bool pk_shift = false; // caps lock (Y / SHIFT)
static bool pk_shift_once = false; // shift for the next letter only
static int pk_presses[SDL_GAMEPAD_BUTTON_COUNT];
static bool pk_a_was_held = false;
// A has been up since the keyboard opened (the press that opened it, from
// the screen behind, must not type).
static bool pk_a_released = false;
// The d-pad direction or B held down repeats.
static int pk_repeat_button = -1;
static unsigned int pk_repeat_time = 0;
static bool pk_repeating = false;

static bool pk_upper()
{
    return pk_shift != pk_shift_once;
}

static int pk_special_at(int column)
{
    for (int index = 0; index < PK_SPECIAL_COUNT; index++) {
        if (column >= pk_special_start[index] && column < pk_special_start[index] + pk_special_span[index]) {
            return index;
        }
    }
    return PK_DONE;
}

static void pk_cell_rect(int row, int column, int* x, int* y, int* w)
{
    *x = PK_MARGIN + column * PK_CELL_W;
    *y = PK_MARGIN + row * PK_CELL_H;
    *w = PK_CELL_W - PK_GAP;
    if (row == PK_ROWS - 1) {
        int special = pk_special_at(column);
        *x = PK_MARGIN + pk_special_start[special] * PK_CELL_W;
        *w = pk_special_span[special] * PK_CELL_W - PK_GAP;
    }
}

static void pk_draw()
{
    if (pk_win == -1) {
        return;
    }

    unsigned char* buf = win_get_buf(pk_win);
    int panel = colorTable[PK_RGB15(3, 4, 3)];
    int keyFill = colorTable[PK_RGB15(5, 7, 5)];
    int border = colorTable[PK_RGB15(8, 14, 8)];
    int green = colorTable[PK_RGB15(0, 31, 0)];
    int selectedFill = colorTable[PK_RGB15(6, 24, 6)];
    int selectedText = colorTable[PK_RGB15(1, 3, 1)];
    int dim = colorTable[PK_RGB15(0, 18, 0)];

    buf_fill(buf, pk_width, pk_height, pk_width, panel);
    draw_box(buf, pk_width, 0, 0, pk_width - 1, pk_height - 1, border);
    draw_box(buf, pk_width, 1, 1, pk_width - 2, pk_height - 2, colorTable[PK_RGB15(1, 2, 1)]);

    int oldFont = text_curr();
    text_font(101);
    int lineHeight = text_height();

    const char* const* rows = pk_upper() ? pk_rows_upper : pk_rows_lower;
    for (int row = 0; row < PK_ROWS; row++) {
        int columns = row == PK_ROWS - 1 ? PK_SPECIAL_COUNT : PK_COLUMNS;
        for (int index = 0; index < columns; index++) {
            int column = row == PK_ROWS - 1 ? pk_special_start[index] : index;
            int x;
            int y;
            int w;
            pk_cell_rect(row, column, &x, &y, &w);
            int h = PK_CELL_H - PK_GAP;

            bool selected = row == pk_row
                && (row == PK_ROWS - 1 ? pk_special_at(pk_column) == index : pk_column == index);
            buf_fill(buf + pk_width * y + x, w, h, pk_width, selected ? selectedFill : keyFill);
            draw_box(buf, pk_width, x, y, x + w - 1, y + h - 1, selected ? green : border);

            char label[8];
            int color = selected ? selectedText : green;
            if (row == PK_ROWS - 1) {
                strcpy(label, pk_special_labels[index]);
                // SHIFT reads lit while it's on.
                if (index == PK_SHIFT && pk_upper() && !selected) {
                    color = colorTable[PK_RGB15(31, 31, 10)];
                }
            } else {
                label[0] = rows[row][index];
                label[1] = '\0';
            }
            int textX = x + (w - text_width(label)) / 2;
            int textY = y + (h - lineHeight) / 2;
            text_to_buf(buf + pk_width * textY + textX, label, w, pk_width, color);
        }
    }

    // The hints, centred, one line each.
    int glyphSize = pad_glyph_size();
    int top = PK_MARGIN + PK_ROWS * PK_CELL_H + 4;
    for (int line = 0; line < PK_HINT_LINES; line++) {
        int lineWidth = 0;
        int count = 0;
        for (int index = 0; index < 4 && pk_hints[line][index].text != NULL; index++) {
            lineWidth += pad_glyph_width(pk_hints[line][index].glyph) + 3 + text_width(pk_hints[line][index].text) + 10;
            count++;
        }
        lineWidth -= 10;
        int x = (pk_width - lineWidth) / 2;
        int y = top + line * PK_HINT_LINE;
        for (int index = 0; index < count; index++) {
            const PkHint* hint = &(pk_hints[line][index]);
            pad_glyph_draw(buf, pk_width, pk_width, pk_height, x, y, hint->glyph);
            x += pad_glyph_width(hint->glyph) + 3;
            text_to_buf(buf + pk_width * (y + (glyphSize - lineHeight) / 2) + x, hint->text, pk_width - x, pk_width, dim);
            x += text_width(hint->text) + 10;
        }
    }

    text_font(oldFont);
    win_draw(pk_win);
}

static bool pk_pressed(int button)
{
    int count = GamepadButtonPresses(button);
    bool pressed = count != pk_presses[button];
    pk_presses[button] = count;
    return pressed;
}

// A press, or a repeat while it's held (400 ms, then every 90 ms).
static bool pk_pressed_or_repeat(int button)
{
    if (pk_pressed(button)) {
        pk_repeat_button = button;
        pk_repeat_time = get_time();
        pk_repeating = false;
        return true;
    }
    if (pk_repeat_button == button) {
        if (!GamepadButtonHeld(button)) {
            pk_repeat_button = -1;
            return false;
        }
        if (elapsed_time(pk_repeat_time) >= (pk_repeating ? 90u : 400u)) {
            pk_repeat_time = get_time();
            pk_repeating = true;
            return true;
        }
    }
    return false;
}

static void pk_move(int dx, int dy)
{
    if (dy != 0) {
        int fromRow = pk_row;
        pk_row = (pk_row + dy + PK_ROWS) % PK_ROWS;
        // Onto the bottom row: the special key under the column. Off it:
        // the middle of the special key.
        if (fromRow == PK_ROWS - 1 && pk_row != PK_ROWS - 1) {
            int special = pk_special_at(pk_column);
            pk_column = pk_special_start[special] + (pk_special_span[special] - 1) / 2;
        }
    }
    if (dx != 0) {
        if (pk_row == PK_ROWS - 1) {
            int special = (pk_special_at(pk_column) + dx + PK_SPECIAL_COUNT) % PK_SPECIAL_COUNT;
            pk_column = pk_special_start[special];
        } else {
            pk_column = (pk_column + dx + PK_COLUMNS) % PK_COLUMNS;
        }
    }
    gsound_play_sfx_file("ib1p1xx1");
    pk_draw();
}

static void pk_size()
{
    pk_width = PK_MARGIN * 2 + PK_COLUMNS * PK_CELL_W - PK_GAP;
    pk_height = PK_MARGIN + PK_ROWS * PK_CELL_H + 4 + PK_HINT_LINES * PK_HINT_LINE + 2;
}

void padkeys_begin(int fieldX, int fieldY, bool firstCapital)
{
    (void)fieldX;
    pk_size();

    // Below the field if it fits, else above it.
    int y = fieldY + 24;
    if (y + pk_height > screenGetHeight()) {
        y = fieldY - 8 - pk_height;
    }
    padkeys_begin_at(y, firstCapital);
}

void padkeys_begin_at(int y, bool firstCapital)
{
    padkeys_end();
    pk_size();

    if (y + pk_height > screenGetHeight()) {
        y = screenGetHeight() - pk_height;
    }
    if (y < 0) {
        y = 0;
    }
    int x = (screenGetWidth() - pk_width) / 2;

    // Self-navigating: padnav hides the pointer and leaves the d-pad alone.
    pk_win = win_add(x, y, pk_width, pk_height, colorTable[0], WINDOW_MOVE_ON_TOP | WINDOW_PAD_SELF_NAV);
    if (pk_win == -1) {
        return;
    }

    pk_row = 1;
    pk_column = 0;
    pk_shift = false;
    pk_shift_once = firstCapital;
    pk_repeat_button = -1;
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; button++) {
        pk_presses[button] = GamepadButtonPresses(button);
    }
    pk_a_was_held = true;
    pk_a_released = false;

    GamepadSetTextCapture(true);
    pk_draw();
}

void padkeys_end()
{
    if (pk_win != -1) {
        win_delete(pk_win);
        pk_win = -1;
        GamepadSetTextCapture(false);
    }
}

int padkeys_frame(int keyCode)
{
    if (pk_win == -1 || keyCode != -1) {
        return keyCode;
    }

    if (pk_pressed_or_repeat(SDL_GAMEPAD_BUTTON_DPAD_UP)) {
        pk_move(0, -1);
    } else if (pk_pressed_or_repeat(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) {
        pk_move(0, 1);
    } else if (pk_pressed_or_repeat(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) {
        pk_move(-1, 0);
    } else if (pk_pressed_or_repeat(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) {
        pk_move(1, 0);
    }

    if (pk_pressed(SDL_GAMEPAD_BUTTON_START)) {
        return KEY_RETURN;
    }
    if (pk_pressed(SDL_GAMEPAD_BUTTON_BACK)) {
        return KEY_ESCAPE;
    }
    if (pk_pressed_or_repeat(SDL_GAMEPAD_BUTTON_EAST)) {
        return KEY_BACKSPACE;
    }
    if (pk_pressed(SDL_GAMEPAD_BUTTON_WEST)) {
        return ' ';
    }
    if (pk_pressed(SDL_GAMEPAD_BUTTON_NORTH)) {
        pk_shift = !pk_shift;
        pk_shift_once = false;
        pk_draw();
        return -1;
    }

    // A (or RT), on press, once A has been seen up.
    bool held = GamepadClickHeld() || GamepadButtonHeld(SDL_GAMEPAD_BUTTON_SOUTH);
    bool counted = pk_pressed(SDL_GAMEPAD_BUTTON_SOUTH);
    bool aPressed = pk_a_released && (counted || (held && !pk_a_was_held));
    pk_a_was_held = held;
    if (!held) {
        pk_a_released = true;
    }
    if (!aPressed) {
        return -1;
    }

    if (pk_row == PK_ROWS - 1) {
        switch (pk_special_at(pk_column)) {
        case PK_SHIFT:
            pk_shift = !pk_shift;
            pk_shift_once = false;
            pk_draw();
            return -1;
        case PK_SPACE:
            return ' ';
        case PK_DELETE:
            return KEY_BACKSPACE;
        default:
            return KEY_RETURN;
        }
    }

    const char* const* rows = pk_upper() ? pk_rows_upper : pk_rows_lower;
    int ch = (unsigned char)rows[pk_row][pk_column];
    if (pk_shift_once) {
        pk_shift_once = false;
        pk_draw();
    }
    return ch;
}

} // namespace fallout

#else

namespace fallout {

void padkeys_begin(int fieldX, int fieldY, bool firstCapital)
{
}

void padkeys_begin_at(int top, bool firstCapital)
{
}

int padkeys_frame(int keyCode)
{
    return keyCode;
}

void padkeys_end()
{
}

} // namespace fallout

#endif
