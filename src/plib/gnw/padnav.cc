#include "plib/gnw/padnav.h"

#include <stdlib.h>

#include <SDL3/SDL.h>

#include "plib/color/color.h"
#include "plib/gnw/button.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/gnw_types.h"
#include "plib/gnw/input.h"
#include "plib/gnw/kb.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/svga.h"

namespace fallout {

// Buttons d-pad navigation considers at once.
#define PADNAV_MAX_CANDIDATES 160
// The focus frame: this thick, this far outside the button.
#define PADNAV_FRAME_WIDTH 2
#define PADNAV_FRAME_GAP 1

typedef struct PadNavCandidate {
    int win;
    int btn;
    Rect rect; // on screen
} PadNavCandidate;

static bool padnav_pad_mode = true;
static bool padnav_pad_by_default = true;

static PadNavCandidate padnav_candidates[PADNAV_MAX_CANDIDATES];
static int padnav_candidate_count = 0;
// The screen hides the pointer itself but navigates on its own.
static bool padnav_self_nav = false;
// The self-navigating window, if padnav_self_nav.
static int padnav_self_win = -1;

// B in pad mode: the key a window wants instead of Escape (0: none).
#define PADNAV_MAX_BACK_KEYS 8
static int padnav_back_win[PADNAV_MAX_BACK_KEYS];
static int padnav_back_scancode[PADNAV_MAX_BACK_KEYS];
static int padnav_back_count = 0;
// The topmost window of the current candidates (a new one = a new menu).
static int padnav_top_win = -1;

static int padnav_focus = -1; // index into padnav_candidates, or -1
static int padnav_focus_btn = -1;
// Where the focus last was (screen centre), for when its button goes.
static int padnav_focus_x = -1;
static int padnav_focus_y = -1;
static int padnav_hint_btn = -1;
// A focus asked for (warp_mouse_to_button) that may not exist yet: taken as
// soon as it appears (dialogue asks for its first option before making it).
static int padnav_pending_btn = -1;
static unsigned int padnav_pending_time = 0;

// The brightened copy's window (index 0; see padnav_frame_show), and where.
static int padnav_frame_wins[4] = { -1, -1, -1, -1 };
static Rect padnav_frame_rect;
static bool padnav_frame_shown = false;
// The window the highlight copies.
static int padnav_frame_source = -1;

static bool padnav_hid_mouse = false;
// The button A is holding down (padnav_activate / padnav_release).
static int padnav_pressed_btn = -1;

// padnav_show_area: a highlight a screen positions itself.
static bool padnav_area_on = false;
static int padnav_area_win = -1;
static Rect padnav_area_rect; // screen coordinates

// Largest area the brightened highlight covers (pixels).
#define PADNAV_MAX_HIGHLIGHT_PIXELS (400 * 200)

// padnav_set_highlight_rect's areas.
#define PADNAV_MAX_HIGHLIGHT_RECTS 24
typedef struct PadNavHighlightRect {
    int btn;
    int win;
    Rect rect; // window coordinates
} PadNavHighlightRect;
static PadNavHighlightRect padnav_highlight_rects[PADNAV_MAX_HIGHLIGHT_RECTS];
static int padnav_highlight_rect_count = 0;

static const PadNavHighlightRect* padnav_find_highlight_rect(int btn)
{
    for (int index = 0; index < padnav_highlight_rect_count; index++) {
        if (padnav_highlight_rects[index].btn == btn) {
            return &(padnav_highlight_rects[index]);
        }
    }
    return NULL;
}

void padnav_set_highlight_rect(int btn, int x, int y, int width, int height)
{
    Window* w;
    if (GNW_find_button(btn, &w) == NULL) {
        return;
    }

    PadNavHighlightRect* entry = (PadNavHighlightRect*)padnav_find_highlight_rect(btn);
    if (entry == NULL) {
        if (padnav_highlight_rect_count >= PADNAV_MAX_HIGHLIGHT_RECTS) {
            return;
        }
        entry = &(padnav_highlight_rects[padnav_highlight_rect_count++]);
    }
    entry->btn = btn;
    entry->win = w->id;
    entry->rect.ulx = x;
    entry->rect.uly = y;
    entry->rect.lrx = x + width - 1;
    entry->rect.lry = y + height - 1;
}

// padnav_set_adjust's buttons.
#define PADNAV_MAX_ADJUST 32
static int padnav_adjust_btn[PADNAV_MAX_ADJUST];
static PadNavAdjustProc* padnav_adjust_proc[PADNAV_MAX_ADJUST];
static bool padnav_adjust_pressable[PADNAV_MAX_ADJUST];
static int padnav_adjust_count = 0;

static int padnav_find_adjust_index(int btn)
{
    for (int index = 0; index < padnav_adjust_count; index++) {
        if (padnav_adjust_btn[index] == btn) {
            return index;
        }
    }
    return -1;
}

static PadNavAdjustProc* padnav_find_adjust(int btn)
{
    int index = padnav_find_adjust_index(btn);
    return index != -1 ? padnav_adjust_proc[index] : NULL;
}

void padnav_set_adjust(int btn, PadNavAdjustProc* proc, bool pressable)
{
    int index = padnav_find_adjust_index(btn);
    if (index != -1) {
        if (proc != NULL) {
            padnav_adjust_proc[index] = proc;
            padnav_adjust_pressable[index] = pressable;
        } else {
            padnav_adjust_count--;
            padnav_adjust_btn[index] = padnav_adjust_btn[padnav_adjust_count];
            padnav_adjust_proc[index] = padnav_adjust_proc[padnav_adjust_count];
            padnav_adjust_pressable[index] = padnav_adjust_pressable[padnav_adjust_count];
        }
        return;
    }

    if (proc != NULL && padnav_adjust_count < PADNAV_MAX_ADJUST) {
        padnav_adjust_btn[padnav_adjust_count] = btn;
        padnav_adjust_proc[padnav_adjust_count] = proc;
        padnav_adjust_pressable[padnav_adjust_count] = pressable;
        padnav_adjust_count++;
    }
}

// Whether a button can be pressed with A: enabled, does something on a left
// click, and isn't a window-sized hotspot.
// padnav_set_filter: one window's say on which of its buttons count.
static int padnav_filter_win = -1;
static PadNavFilterProc* padnav_filter_proc = NULL;

static bool padnav_button_eligible(Window* w, Button* button)
{
    if ((button->flags & BUTTON_FLAG_DISABLED) != 0) {
        return false;
    }

    if (padnav_filter_proc != NULL && w->id == padnav_filter_win && !padnav_filter_proc(button->id)) {
        return false;
    }

    if (button->leftMouseUpEventCode == -1 && button->lefMouseDownEventCode == -1
        && button->leftMouseUpProc == NULL && button->leftMouseDownProc == NULL) {
        return false;
    }

    // Scroll areas that just send an arrow key (dialogue reply halves, list
    // arrows): the d-pad sends those itself when there's no button that way.
    int code = button->leftMouseUpEventCode != -1 ? button->leftMouseUpEventCode : button->lefMouseDownEventCode;
    if (code == KEY_ARROW_UP || code == KEY_ARROW_DOWN || code == KEY_ARROW_LEFT || code == KEY_ARROW_RIGHT
        || code == KEY_PAGE_UP || code == KEY_PAGE_DOWN) {
        return false;
    }

    int width = button->rect.lrx - button->rect.ulx + 1;
    int height = button->rect.lry - button->rect.uly + 1;
    if (width < 3 || height < 3) {
        return false;
    }

    // A hotspot covering most of a big window (e.g. "click anywhere").
    if (w->width * w->height > 300 * 200 && width * height * 2 > w->width * w->height) {
        return false;
    }

    // An area that works out what was clicked from where (the save slot
    // list, a map): pressing it with a hidden pointer would pick something
    // arbitrary, so the screen's own d-pad handling (the arrow keys) does
    // it. Real buttons are smaller; a lone dialogue option can be ~380x107.
    if (height > 150 && width * height > 30000) {
        return false;
    }

    return true;
}

// Rebuilds the candidates: the eligible buttons of the windows a click could
// reach, top down to the first modal one, stopping at one that opts out.
static void padnav_collect()
{
    padnav_candidate_count = 0;
    padnav_self_nav = false;
    int topWin = -1;

    for (int index = GNW_window_count() - 1; index >= 1; index--) {
        Window* w = GNW_window_at(index);
        if (w == NULL || (w->flags & (WINDOW_PAD_OVERLAY | WINDOW_HIDDEN)) != 0) {
            continue;
        }

        if ((w->flags & WINDOW_NO_PAD_NAV) != 0) {
            break;
        }

        if ((w->flags & WINDOW_PAD_SELF_NAV) != 0) {
            if (padnav_candidate_count == 0) {
                padnav_self_nav = true;
                padnav_self_win = w->id;
            }
            break;
        }

        for (Button* button = w->buttonListHead; button != NULL; button = button->next) {
            if (padnav_candidate_count >= PADNAV_MAX_CANDIDATES) {
                break;
            }
            if (!padnav_button_eligible(w, button)) {
                continue;
            }

            PadNavCandidate* candidate = &(padnav_candidates[padnav_candidate_count++]);
            candidate->win = w->id;
            candidate->btn = button->id;
            candidate->rect.ulx = w->rect.ulx + button->rect.ulx;
            candidate->rect.uly = w->rect.uly + button->rect.uly;
            candidate->rect.lrx = w->rect.ulx + button->rect.lrx;
            candidate->rect.lry = w->rect.uly + button->rect.lry;
            if (topWin == -1) {
                topWin = w->id;
            }
        }

        if ((w->flags & WINDOW_MODAL) != 0) {
            break;
        }
    }

    // A different menu came up: start it in pad mode if that's the default,
    // and don't carry the old focus position over.
    if (topWin != padnav_top_win) {
        if (topWin != -1 && padnav_top_win != -1) {
            padnav_focus_x = -1;
        }
        padnav_top_win = topWin;
        if (topWin != -1 && padnav_pad_by_default) {
            padnav_pad_mode = true;
        }
    }

    // A requested focus, as soon as its button is there (for a second).
    if (padnav_pending_btn != -1) {
        for (int index = 0; index < padnav_candidate_count; index++) {
            if (padnav_candidates[index].btn == padnav_pending_btn) {
                padnav_focus_btn = padnav_pending_btn;
                padnav_pending_btn = -1;
                break;
            }
        }
        if (padnav_pending_btn != -1 && elapsed_time(padnav_pending_time) > 1000) {
            padnav_pending_btn = -1;
        }
    }

    // Keep the focus on the same button if it's still there.
    padnav_focus = -1;
    for (int index = 0; index < padnav_candidate_count; index++) {
        if (padnav_candidates[index].btn == padnav_focus_btn) {
            padnav_focus = index;
            break;
        }
    }

    // Otherwise the screen's choice (its warp_mouse_to_button), else the
    // top left button.
    if (padnav_focus == -1 && padnav_candidate_count > 0) {
        for (int index = 0; index < padnav_candidate_count; index++) {
            if (padnav_candidates[index].btn == padnav_hint_btn) {
                padnav_focus = index;
                break;
            }
        }
        // The focused button went (e.g. dialogue options replaced): the one
        // nearest where it was.
        if (padnav_focus == -1 && padnav_focus_x != -1) {
            int bestDistance = 0;
            for (int index = 0; index < padnav_candidate_count; index++) {
                const Rect* rect = &(padnav_candidates[index].rect);
                int dx = (rect->ulx + rect->lrx) / 2 - padnav_focus_x;
                int dy = (rect->uly + rect->lry) / 2 - padnav_focus_y;
                int distance = dx * dx + dy * dy;
                if (padnav_focus == -1 || distance < bestDistance) {
                    padnav_focus = index;
                    bestDistance = distance;
                }
            }
        }
        if (padnav_focus == -1) {
            int best = 0;
            for (int index = 1; index < padnav_candidate_count; index++) {
                const Rect* a = &(padnav_candidates[index].rect);
                const Rect* b = &(padnav_candidates[best].rect);
                if (a->uly < b->uly - 4 || (abs(a->uly - b->uly) <= 4 && a->ulx < b->ulx)) {
                    best = index;
                }
            }
            padnav_focus = best;
        }
        padnav_focus_btn = padnav_candidates[padnav_focus].btn;
    }
}

// The highlight. Most buttons show their own hover look (a hover image, or a
// proc such as dialogue's option recolouring) when the pointer is over them,
// whether it's visible or not, so in pad mode the hidden pointer is simply
// parked on the focused button and Fallout draws its own highlight. Buttons
// with no hover look get a brightened copy of themselves in a small window
// on top, refreshed each frame.
static bool padnav_brighten_ready = false;
static unsigned char padnav_brighten_map[256];

static void padnav_build_brighten()
{
    for (int index = 0; index < 256; index++) {
        int r = cmap[index * 3] >> 1; // 6 bit -> 5 bit
        int g = cmap[index * 3 + 1] >> 1;
        int b = cmap[index * 3 + 2] >> 1;
        r = r * 7 / 5 + 1;
        g = g * 7 / 5 + 1;
        b = b * 7 / 5 + 1;
        r = r > 31 ? 31 : r;
        g = g > 31 ? 31 : g;
        b = b > 31 ? 31 : b;
        padnav_brighten_map[index] = index == 0 ? 0 : colorTable[(r << 10) | (g << 5) | b];
    }
    padnav_brighten_ready = true;
}

static bool padnav_has_hover_look(int btn)
{
    Window* w;
    Button* button = GNW_find_button(btn, &w);
    // A hover image, a proc, or an enter event the screen handles (dialogue
    // options recolour on 1200+n). One shared enter/exit code (Preferences'
    // sliders, 526) changes nothing visible.
    return button == NULL || button->hoverImage != NULL || button->mouseEnterProc != NULL
        || (button->mouseEnterEventCode != -1 && button->mouseEnterEventCode != button->mouseExitEventCode);
}

static void padnav_frame_hide()
{
    if (padnav_frame_wins[0] != -1) {
        win_delete(padnav_frame_wins[0]);
        padnav_frame_wins[0] = -1;
    }
    padnav_frame_shown = false;
}

static void padnav_overlay_draw(Window* w, const Rect* rect);

static void padnav_frame_show(const Rect* rect)
{
    const PadNavCandidate* candidate = &(padnav_candidates[padnav_focus]);

    // Park the hidden pointer on it: its own hover highlight shows.
    int centerX = (rect->ulx + rect->lrx) / 2;
    int centerY = (rect->uly + rect->lry) / 2;
    int mouseX;
    int mouseY;
    mouse_get_position(&mouseX, &mouseY);
    if (mouseX != centerX || mouseY != centerY) {
        if (!mouse_hidden()) {
            mouse_hide();
            padnav_hid_mouse = true;
        }
        mouse_set_position(centerX, centerY);
    }

    // A screen-given area (the plate a small button sits on), in screen
    // coordinates; always brightened.
    Rect area = *rect;
    const PadNavHighlightRect* custom = padnav_find_highlight_rect(candidate->btn);
    Window* w = GNW_find(candidate->win);
    if (custom != NULL && w != NULL && custom->win == w->id) {
        area.ulx = w->rect.ulx + custom->rect.ulx;
        area.uly = w->rect.uly + custom->rect.uly;
        area.lrx = w->rect.ulx + custom->rect.lrx;
        area.lry = w->rect.uly + custom->rect.lry;
        rect = &area;
    } else if (padnav_has_hover_look(candidate->btn)) {
        if (padnav_frame_shown) {
            padnav_frame_hide();
        }
        return;
    }

    // No hover look: a brightened copy on top.
    padnav_overlay_draw(w, rect);
}

// A brightened copy of `rect` (screen coordinates) of window `w`, in a small
// window on top of it.
static void padnav_overlay_draw(Window* w, const Rect* rect)
{
    if (w == NULL) {
        return;
    }
    int width = rect->lrx - rect->ulx + 1;
    int height = rect->lry - rect->uly + 1;
    if (!padnav_frame_shown || rect->ulx != padnav_frame_rect.ulx || rect->uly != padnav_frame_rect.uly
        || rect->lrx != padnav_frame_rect.lrx || rect->lry != padnav_frame_rect.lry) {
        padnav_frame_hide();
        // Stacked like the menu under it, so any window opened later lands
        // above it: deleting it instead would repaint the spot from a new
        // window that isn't drawn yet (black while a screen fades out).
        padnav_frame_wins[0] = win_add(rect->ulx, rect->uly, width, height, 0, (w->flags & WINDOW_MOVE_ON_TOP) | WINDOW_PAD_OVERLAY);
        if (padnav_frame_wins[0] == -1) {
            return;
        }
        padnav_frame_rect = *rect;
        padnav_frame_shown = true;
        padnav_frame_source = w->id;
        padnav_brighten_ready = false; // the palette may have changed
    }

    if (!padnav_brighten_ready) {
        padnav_build_brighten();
    }

    // Refreshed every frame from the window under it: its pixels, then its
    // buttons' current images on top. GNW keeps those out of the window
    // buffer and layers them when drawing (a button pressed by A shows only
    // there).
    static unsigned char composed[PADNAV_MAX_HIGHLIGHT_PIXELS];
    if (width * height > PADNAV_MAX_HIGHLIGHT_PIXELS) {
        return;
    }

    unsigned char* dest = win_get_buf(padnav_frame_wins[0]);
    int srcX = rect->ulx - w->rect.ulx;
    int srcY = rect->uly - w->rect.uly;
    for (int row = 0; row < height; row++) {
        int y = srcY + row;
        for (int column = 0; column < width; column++) {
            int x = srcX + column;
            composed[row * width + column] = x >= 0 && y >= 0 && x < w->width && y < w->height
                ? w->buffer[y * w->width + x]
                : 0;
        }
    }

    for (Button* button = w->buttonListHead; button != NULL; button = button->next) {
        unsigned char* image = button->currentImage;
        if (image == NULL) {
            continue;
        }
        int buttonWidth = button->rect.lrx - button->rect.ulx + 1;
        bool transparent = (button->flags & BUTTON_FLAG_TRANSPARENT) != 0;
        for (int y = button->rect.uly; y <= button->rect.lry; y++) {
            int row = y - srcY;
            if (row < 0 || row >= height) {
                continue;
            }
            for (int x = button->rect.ulx; x <= button->rect.lrx; x++) {
                int column = x - srcX;
                if (column < 0 || column >= width) {
                    continue;
                }
                unsigned char pixel = image[(y - button->rect.uly) * buttonWidth + (x - button->rect.ulx)];
                if (!transparent || pixel != 0) {
                    composed[row * width + column] = pixel;
                }
            }
        }
    }

    bool changed = false;
    for (int row = 0; row < height; row++) {
        for (int column = 0; column < width; column++) {
            unsigned char pixel = padnav_brighten_map[composed[row * width + column]];
            if (dest[row * width + column] != pixel) {
                dest[row * width + column] = pixel;
                changed = true;
            }
        }
    }
    if (changed) {
        win_draw(padnav_frame_wins[0]);
    }
}

void padnav_update()
{
    padnav_collect();

    bool active = padnav_pad_mode && padnav_focus != -1;
    if (padnav_focus != -1) {
        const Rect* rect = &(padnav_candidates[padnav_focus].rect);
        padnav_focus_x = (rect->ulx + rect->lrx) / 2;
        padnav_focus_y = (rect->uly + rect->lry) / 2;
    }
    if (active) {
        padnav_frame_show(&(padnav_candidates[padnav_focus].rect));
    } else if (padnav_area_on && padnav_pad_mode && padnav_candidate_count == 0) {
        padnav_overlay_draw(GNW_find(padnav_area_win), &padnav_area_rect);
    } else if (padnav_frame_shown) {
        padnav_frame_hide();
    }

    // In pad mode the pointer stays hidden over menus (screens show it when
    // they open).
    bool hide = padnav_pad_mode && (padnav_candidate_count > 0 || padnav_self_nav);
    if (hide) {
        if (!mouse_hidden()) {
            mouse_hide();
            padnav_hid_mouse = true;
        }
    } else if (padnav_hid_mouse) {
        padnav_hid_mouse = false;
        mouse_show();
    }
}

bool padnav_active()
{
    return padnav_pad_mode && padnav_focus != -1;
}

bool padnav_dpad(int dx, int dy)
{
    padnav_collect();
    if (padnav_candidate_count == 0) {
        // The map, the inventory, or a screen with its own d-pad use.
        if (padnav_self_nav) {
            padnav_pad_mode = true;
        }
        return false;
    }

    // Back from pointer mode: just bring the focus back.
    if (!padnav_pad_mode) {
        padnav_pad_mode = true;
        padnav_update();
        return true;
    }

    // A slider: left/right change it.
    if (dx != 0) {
        PadNavAdjustProc* adjust = padnav_find_adjust(padnav_candidates[padnav_focus].btn);
        if (adjust != NULL) {
            adjust(padnav_candidates[padnav_focus].btn, dx);
            return true;
        }
    }

    const Rect* from = &(padnav_candidates[padnav_focus].rect);
    int fromX = (from->ulx + from->lrx) / 2;
    int fromY = (from->uly + from->lry) / 2;

    int best = -1;
    int bestScore = 0;
    for (int index = 0; index < padnav_candidate_count; index++) {
        if (index == padnav_focus) {
            continue;
        }

        const Rect* to = &(padnav_candidates[index].rect);
        int toX = (to->ulx + to->lrx) / 2;
        int toY = (to->uly + to->lry) / 2;

        // Distance along the pressed direction (must be ahead) and across
        // it (zero while the two overlap on that axis).
        int along;
        int across;
        if (dx != 0) {
            along = (toX - fromX) * dx;
            if (to->lry < from->uly) {
                across = from->uly - to->lry;
            } else if (to->uly > from->lry) {
                across = to->uly - from->lry;
            } else {
                across = 0;
            }
        } else {
            along = (toY - fromY) * dy;
            if (to->lrx < from->ulx) {
                across = from->ulx - to->lrx;
            } else if (to->ulx > from->lrx) {
                across = to->ulx - from->lrx;
            } else {
                across = 0;
            }
        }

        // Within 45 degrees of the direction: farther off, the press is
        // more likely meant for the screen (e.g. up/down in a list next to
        // the buttons).
        if (along <= 0 || across > along) {
            continue;
        }

        int score = along + across * 3;
        if (best == -1 || score < bestScore) {
            best = index;
            bestScore = score;
        }
    }

    if (best == -1) {
        // Nothing that way: the screen gets the arrow (lists, ...).
        return false;
    }

    padnav_focus = best;
    padnav_focus_btn = padnav_candidates[best].btn;
    padnav_frame_show(&(padnav_candidates[best].rect));
    return true;
}

bool padnav_activate()
{
    padnav_collect();
    if (!padnav_active()) {
        return false;
    }

    int btn = padnav_candidates[padnav_focus].btn;
    int adjust = padnav_find_adjust_index(btn);
    if (adjust != -1 && !padnav_adjust_pressable[adjust]) {
        return true; // sliders take left/right, not A
    }

    // Some screens work out what was clicked from the pointer's position
    // (e.g. a knob turns when clicked on its middle), so put the hidden
    // pointer on the button's centre first.
    const Rect* rect = &(padnav_candidates[padnav_focus].rect);
    mouse_set_position((rect->ulx + rect->lrx) / 2, (rect->uly + rect->lry) / 2);

    // Pressed while A is held, released (the click) when it's let go.
    win_button_press(btn);
    padnav_pressed_btn = btn;
    return true;
}

void padnav_release()
{
    if (padnav_pressed_btn == -1) {
        return;
    }

    int btn = padnav_pressed_btn;
    padnav_pressed_btn = -1;
    win_button_release(btn);
}

void padnav_pointer_moved()
{
    if (!padnav_pad_mode) {
        return;
    }

    padnav_pad_mode = false;

    // The pointer comes back on the focused button.
    padnav_collect();
    if (padnav_focus != -1) {
        const Rect* rect = &(padnav_candidates[padnav_focus].rect);
        // Hidden while it moves (a visible pointer would leave its image).
        mouse_hide();
        padnav_hid_mouse = true;
        mouse_set_position((rect->ulx + rect->lrx) / 2, (rect->uly + rect->lry) / 2);
    }
    if (padnav_frame_shown) {
        padnav_frame_hide();
    }
    if (padnav_hid_mouse) {
        padnav_hid_mouse = false;
        mouse_show();
    }
}

void padnav_set_filter(int win, PadNavFilterProc* proc)
{
    padnav_filter_win = proc != NULL ? win : -1;
    padnav_filter_proc = proc;
}

void padnav_window_changed(int win)
{
    if (win != -1 && win == padnav_filter_win) {
        padnav_set_filter(-1, NULL);
    }

    if (padnav_area_on && padnav_area_win == win) {
        padnav_area_on = false;
    }

    if (padnav_frame_shown && padnav_frame_source == win) {
        padnav_frame_hide();
    }

    if (win != -1) {
        for (int index = padnav_back_count - 1; index >= 0; index--) {
            if (padnav_back_win[index] == win) {
                padnav_back_count--;
                padnav_back_win[index] = padnav_back_win[padnav_back_count];
                padnav_back_scancode[index] = padnav_back_scancode[padnav_back_count];
            }
        }
        for (int index = padnav_highlight_rect_count - 1; index >= 0; index--) {
            if (padnav_highlight_rects[index].win == win) {
                padnav_highlight_rects[index] = padnav_highlight_rects[--padnav_highlight_rect_count];
            }
        }
    }
}

void padnav_set_focus(int btn)
{
    padnav_hint_btn = btn;
    padnav_focus_btn = btn;
    padnav_pending_btn = btn;
    padnav_pending_time = get_time();
}

void padnav_set_pad_by_default(bool padByDefault)
{
    padnav_pad_by_default = padByDefault;
    // Walk mode off is the PC-like layout: the pointer until the d-pad is
    // used.
    padnav_pad_mode = padByDefault;
}

void padnav_set_window_flags(int win, int flags)
{
    Window* w = GNW_find(win);
    if (w != NULL) {
        w->flags |= flags & (WINDOW_NO_PAD_NAV | WINDOW_PAD_SELF_NAV);
    }
}

void padnav_set_back_key(int win, int scancode)
{
    for (int index = 0; index < padnav_back_count; index++) {
        if (padnav_back_win[index] == win) {
            padnav_back_scancode[index] = scancode;
            return;
        }
    }
    if (padnav_back_count < PADNAV_MAX_BACK_KEYS) {
        padnav_back_win[padnav_back_count] = win;
        padnav_back_scancode[padnav_back_count] = scancode;
        padnav_back_count++;
    }
}

int padnav_back_key()
{
    padnav_collect();

    int win;
    if (padnav_pad_mode && padnav_self_nav) {
        win = padnav_self_win;
    } else if (padnav_active()) {
        win = padnav_candidates[padnav_focus].win;
    } else {
        return -1;
    }

    for (int index = 0; index < padnav_back_count; index++) {
        if (padnav_back_win[index] == win) {
            return padnav_back_scancode[index];
        }
    }
    return SDL_SCANCODE_ESCAPE;
}

void padnav_show_area(int win, int x, int y, int width, int height)
{
    Window* w = GNW_find(win);
    if (w == NULL) {
        padnav_hide_area();
        return;
    }
    padnav_area_on = true;
    padnav_area_win = win;
    padnav_area_rect.ulx = w->rect.ulx + x;
    padnav_area_rect.uly = w->rect.uly + y;
    padnav_area_rect.lrx = padnav_area_rect.ulx + width - 1;
    padnav_area_rect.lry = padnav_area_rect.uly + height - 1;
    if (padnav_pad_mode && padnav_focus == -1) {
        padnav_overlay_draw(w, &padnav_area_rect);
    }
}

void padnav_hide_area()
{
    padnav_area_on = false;
    if (padnav_frame_shown && padnav_focus == -1) {
        padnav_frame_hide();
    }
}

void padnav_set_pad_mode(bool on)
{
    padnav_pad_mode = on;
}

void padnav_menu_opened()
{
    if (padnav_pad_by_default) {
        padnav_pad_mode = true;
    }
}

bool padnav_pad_mode_on()
{
    return padnav_pad_mode;
}

} // namespace fallout
