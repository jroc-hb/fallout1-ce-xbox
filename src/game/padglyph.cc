#include "game/padglyph.h"

#include <string.h>

#include "plib/gnw/gnw.h"
#include "plib/gnw/gnw_types.h"
#include "plib/gnw/input.h"
#include "plib/gnw/memory.h"
#include "plib/gnw/padnav.h"

#include "plib/color/color.h"

namespace fallout {

// Disc diameter. Odd, so it has a centre pixel.
#define PAD_GLYPH_SIZE 13

// 15-bit RGB (5 bits each) for colorTable.
#define RGB15(r, g, b) (((r) << 10) | ((g) << 5) | (b))

typedef struct PadGlyphStyle {
    int fill; // RGB15
    int rim; // RGB15
    int text; // RGB15
    const char* label;
} PadGlyphStyle;

// The Duke / Controller S colours.
static const PadGlyphStyle pad_glyph_styles[PAD_GLYPH_COUNT] = {
    { RGB15(4, 22, 4), RGB15(1, 10, 1), RGB15(0, 0, 0), "A" },
    { RGB15(26, 3, 3), RGB15(12, 1, 1), RGB15(0, 0, 0), "B" },
    { RGB15(5, 10, 28), RGB15(2, 4, 13), RGB15(0, 0, 0), "X" },
    { RGB15(30, 26, 3), RGB15(14, 12, 1), RGB15(0, 0, 0), "Y" },
    { RGB15(2, 2, 2), RGB15(18, 18, 18), RGB15(0, 0, 0), "" },
    { RGB15(30, 30, 30), RGB15(14, 14, 14), RGB15(0, 0, 0), "" },
    { RGB15(20, 20, 20), RGB15(8, 8, 8), RGB15(0, 0, 0), "RS" },
    { RGB15(20, 20, 20), RGB15(8, 8, 8), RGB15(0, 0, 0), "LS" },
    { RGB15(4, 4, 4), RGB15(18, 18, 18), RGB15(29, 29, 29), "START" },
    { RGB15(4, 4, 4), RGB15(18, 18, 18), RGB15(29, 29, 29), "BACK" },
    { RGB15(4, 4, 4), RGB15(18, 18, 18), RGB15(29, 29, 29), "HOLD" },
};

// The glyph letters: a 5x7 pixel font of their own (the game's small font
// isn't legible this size; its R reads as an A). One row per byte, bit 4 is
// the leftmost pixel.
#define PAD_GLYPH_LETTER_WIDTH 5
#define PAD_GLYPH_LETTER_HEIGHT 7

static const unsigned char* pad_glyph_letter(char letter)
{
    static const unsigned char a[] = { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 };
    static const unsigned char b[] = { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E };
    static const unsigned char x[] = { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 };
    static const unsigned char y[] = { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 };
    static const unsigned char r[] = { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 };
    static const unsigned char l[] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F };
    static const unsigned char s[] = { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E };
    static const unsigned char t[] = { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 };
    static const unsigned char c[] = { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E };
    static const unsigned char k[] = { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 };
    static const unsigned char h[] = { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 };
    static const unsigned char o[] = { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E };
    static const unsigned char d[] = { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E };
    switch (letter) {
    case 'A':
        return a;
    case 'B':
        return b;
    case 'X':
        return x;
    case 'Y':
        return y;
    case 'R':
        return r;
    case 'L':
        return l;
    case 'S':
        return s;
    case 'T':
        return t;
    case 'C':
        return c;
    case 'K':
        return k;
    case 'H':
        return h;
    case 'O':
        return o;
    case 'D':
        return d;
    }
    return NULL;
}

// Width in pixels of `label` in that font (one pixel between letters).
static int pad_glyph_label_width(const char* label)
{
    int count = (int)strlen(label);
    return count > 0 ? count * (PAD_GLYPH_LETTER_WIDTH + 1) - 1 : 0;
}

// The palette index for an RGB15 colour, never 0 (0 is transparent when a
// buffer is blitted as a sprite; the palette has other blacks).
static int pad_glyph_color(int rgb)
{
    int index = colorTable[rgb];
    for (int level = 1; index == 0 && level < 8; level++) {
        index = colorTable[RGB15(level, level, level)];
    }
    return index;
}

int pad_glyph_size()
{
    return PAD_GLYPH_SIZE;
}

// Discs are square; the stick clicks are pills wide enough for "RS"/"LS".
#define PAD_GLYPH_MAX_WIDTH 40

int pad_glyph_width(PadGlyph glyph)
{
    if (glyph != PAD_GLYPH_RS && glyph != PAD_GLYPH_LS && glyph != PAD_GLYPH_START && glyph != PAD_GLYPH_BACK
        && glyph != PAD_GLYPH_HOLD) {
        return PAD_GLYPH_SIZE;
    }

    int width = pad_glyph_label_width(pad_glyph_styles[glyph].label) + 8;
    if (width < PAD_GLYPH_SIZE) {
        width = PAD_GLYPH_SIZE;
    }
    if (width > PAD_GLYPH_MAX_WIDTH) {
        width = PAD_GLYPH_MAX_WIDTH;
    }
    return width;
}

void pad_glyph_draw(unsigned char* buf, int pitch, int width, int height, int x, int y, PadGlyph glyph)
{
    if (glyph < 0 || glyph >= PAD_GLYPH_COUNT) {
        return;
    }

    const PadGlyphStyle* style = &(pad_glyph_styles[glyph]);
    int fill = pad_glyph_color(style->fill);
    int rim = pad_glyph_color(style->rim);
    int text = pad_glyph_color(style->text);

    // Draw on a scratch buffer first so the text can be clipped to the
    // shape, then copy the non-zero pixels.
    int glyphWidth = pad_glyph_width(glyph);
    unsigned char scratch[PAD_GLYPH_MAX_WIDTH * PAD_GLYPH_SIZE];
    memset(scratch, 0, sizeof(scratch));

    // A disc (or a pill: a disc stretched sideways) with a one pixel rim.
    // Distances are in half pixels so the odd-sized shape is round.
    int c = PAD_GLYPH_SIZE - 1; // centre of the left end cap, doubled
    int stretch = (glyphWidth - PAD_GLYPH_SIZE) * 2; // doubled
    int outer = PAD_GLYPH_SIZE * PAD_GLYPH_SIZE; // (diameter)^2
    int inner = (PAD_GLYPH_SIZE - 2) * (PAD_GLYPH_SIZE - 2);
    for (int row = 0; row < PAD_GLYPH_SIZE; row++) {
        for (int column = 0; column < glyphWidth; column++) {
            int px = column * 2;
            int dx = px < c ? px - c : (px > c + stretch ? px - c - stretch : 0);
            int dy = row * 2 - c;
            int d = dx * dx + dy * dy;
            if (d <= inner) {
                scratch[row * PAD_GLYPH_MAX_WIDTH + column] = (unsigned char)fill;
            } else if (d <= outer) {
                scratch[row * PAD_GLYPH_MAX_WIDTH + column] = (unsigned char)rim;
            }
        }
    }

    // A little shine on the upper left, as on the pad's domed buttons.
    if (glyph != PAD_GLYPH_WHITE && glyph != PAD_GLYPH_START && glyph != PAD_GLYPH_BACK && glyph != PAD_GLYPH_HOLD) {
        int shine = pad_glyph_color(glyph == PAD_GLYPH_BLACK ? RGB15(10, 10, 10) : RGB15(31, 31, 31));
        scratch[3 * PAD_GLYPH_MAX_WIDTH + 4] = (unsigned char)shine;
        scratch[3 * PAD_GLYPH_MAX_WIDTH + 5] = (unsigned char)shine;
        scratch[4 * PAD_GLYPH_MAX_WIDTH + 3] = (unsigned char)shine;
    }

    // The letters, centred.
    int labelWidth = pad_glyph_label_width(style->label);
    int labelX = (glyphWidth - labelWidth) / 2;
    int labelY = (PAD_GLYPH_SIZE - PAD_GLYPH_LETTER_HEIGHT) / 2;
    for (int index = 0; style->label[index] != '\0'; index++) {
        const unsigned char* bits = pad_glyph_letter(style->label[index]);
        if (bits == NULL) {
            continue;
        }
        int letterX = labelX + index * (PAD_GLYPH_LETTER_WIDTH + 1);
        for (int row = 0; row < PAD_GLYPH_LETTER_HEIGHT; row++) {
            for (int column = 0; column < PAD_GLYPH_LETTER_WIDTH; column++) {
                if ((bits[row] & (0x10 >> column)) == 0) {
                    continue;
                }
                int destRow = labelY + row;
                int destColumn = letterX + column;
                if (destColumn >= 0 && destColumn < glyphWidth) {
                    scratch[destRow * PAD_GLYPH_MAX_WIDTH + destColumn] = (unsigned char)text;
                }
            }
        }
    }

    for (int row = 0; row < PAD_GLYPH_SIZE; row++) {
        int destY = y + row;
        if (destY < 0 || destY >= height) {
            continue;
        }
        for (int column = 0; column < glyphWidth; column++) {
            int destX = x + column;
            unsigned char pixel = scratch[row * PAD_GLYPH_MAX_WIDTH + column];
            if (pixel != 0 && destX >= 0 && destX < width) {
                buf[destY * pitch + destX] = pixel;
            }
        }
    }
}

int pad_glyph_held_width(PadGlyph glyph)
{
    int glyphWidth = pad_glyph_width(glyph);
    int holdWidth = pad_glyph_width(PAD_GLYPH_HOLD);
    return glyphWidth > holdWidth ? glyphWidth : holdWidth;
}

int pad_glyph_held_height()
{
    return PAD_GLYPH_SIZE * 2;
}

void pad_glyph_draw_held(unsigned char* buf, int pitch, int width, int height, int x, int y, PadGlyph glyph)
{
    int total = pad_glyph_held_width(glyph);
    pad_glyph_draw(buf, pitch, width, height, x + (total - pad_glyph_width(glyph)) / 2, y, glyph);
    // The pill overlaps the disc's bottom row by one pixel: they read as a pair.
    pad_glyph_draw(buf, pitch, width, height, x + (total - pad_glyph_width(PAD_GLYPH_HOLD)) / 2, y + PAD_GLYPH_SIZE - 1, PAD_GLYPH_HOLD);
}

// ---------------------------------------------------------------------------
// Hold-LT glyph overlays for screens other than the HUD (intface.cc has its
// own): a screen registers glyph spots on one of its windows; while LT is
// held they fade in over those spots, each in a small window holding a copy
// of what's under it, so nothing the screen redraws can erase them.
// ---------------------------------------------------------------------------

#define PAD_GLYPH_OVERLAY_WINDOWS 2
#define PAD_GLYPH_OVERLAY_SPOTS 8
#define PAD_GLYPH_FADE_STEPS 8
#define PAD_GLYPH_FADE_MS 150.0f

typedef struct PadGlyphOverlay {
    int win; // -1: unused
    int count;
    PadGlyphSpot spots[PAD_GLYPH_OVERLAY_SPOTS];
    int glyphWins[PAD_GLYPH_OVERLAY_SPOTS];
    unsigned char* under[PAD_GLYPH_OVERLAY_SPOTS];
    unsigned char* art[PAD_GLYPH_OVERLAY_SPOTS];
    int widths[PAD_GLYPH_OVERLAY_SPOTS];
    bool shown;
} PadGlyphOverlay;

static PadGlyphOverlay pad_glyph_overlays[PAD_GLYPH_OVERLAY_WINDOWS] = {
    { -1 },
    { -1 },
};
static float pad_glyph_alpha = 0.0f;
static int pad_glyph_drawn_step = -1;
static unsigned int pad_glyph_last_time = 0;

static void pad_glyph_overlay_close(PadGlyphOverlay* overlay)
{
    for (int index = 0; index < overlay->count; index++) {
        if (overlay->shown && overlay->glyphWins[index] != -1) {
            win_delete(overlay->glyphWins[index]);
        }
        overlay->glyphWins[index] = -1;
        mem_free(overlay->under[index]);
        overlay->under[index] = NULL;
        mem_free(overlay->art[index]);
        overlay->art[index] = NULL;
    }
    overlay->shown = false;
}

void pad_glyph_spots_set(int win, const PadGlyphSpot* spots, int count)
{
    pad_glyph_spots_clear(win);
    for (int slot = 0; slot < PAD_GLYPH_OVERLAY_WINDOWS; slot++) {
        PadGlyphOverlay* overlay = &(pad_glyph_overlays[slot]);
        if (overlay->win != -1) {
            continue;
        }
        overlay->win = win;
        overlay->count = count < PAD_GLYPH_OVERLAY_SPOTS ? count : PAD_GLYPH_OVERLAY_SPOTS;
        for (int index = 0; index < overlay->count; index++) {
            overlay->spots[index] = spots[index];
            overlay->glyphWins[index] = -1;
            overlay->under[index] = NULL;
            overlay->art[index] = NULL;
        }
        overlay->shown = false;
        return;
    }
}

void pad_glyph_spots_clear(int win)
{
    for (int slot = 0; slot < PAD_GLYPH_OVERLAY_WINDOWS; slot++) {
        PadGlyphOverlay* overlay = &(pad_glyph_overlays[slot]);
        if (overlay->win == win) {
            pad_glyph_overlay_close(overlay);
            overlay->win = -1;
        }
    }
}

// The window's pixels under a rectangle, its buttons' current images on top
// (GNW keeps those out of the window buffer).
static void pad_glyph_copy_under(Window* w, int x, int y, int width, int height, unsigned char* dest)
{
    for (int row = 0; row < height; row++) {
        for (int column = 0; column < width; column++) {
            int sx = x + column;
            int sy = y + row;
            dest[row * width + column] = sx >= 0 && sy >= 0 && sx < w->width && sy < w->height
                ? w->buffer[sy * w->width + sx]
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
        for (int by = button->rect.uly; by <= button->rect.lry; by++) {
            int row = by - y;
            if (row < 0 || row >= height) {
                continue;
            }
            for (int bx = button->rect.ulx; bx <= button->rect.lrx; bx++) {
                int column = bx - x;
                if (column < 0 || column >= width) {
                    continue;
                }
                unsigned char pixel = image[(by - button->rect.uly) * buttonWidth + (bx - button->rect.ulx)];
                if (!transparent || pixel != 0) {
                    dest[row * width + column] = pixel;
                }
            }
        }
    }
}

static void pad_glyph_overlay_open(PadGlyphOverlay* overlay, Window* w)
{
    int height = pad_glyph_size();
    for (int index = 0; index < overlay->count; index++) {
        const PadGlyphSpot* spot = &(overlay->spots[index]);
        overlay->glyphWins[index] = -1;
        if (spot->padModeOnly && !padnav_pad_mode_on()) {
            continue;
        }

        int width = pad_glyph_width(spot->glyph);
        int x = spot->x - width / 2;
        int y = spot->y - height / 2;
        unsigned char* under = (unsigned char*)mem_malloc(width * height);
        unsigned char* art = (unsigned char*)mem_malloc(width * height);
        int win = under != NULL && art != NULL
            ? win_add(w->rect.ulx + x, w->rect.uly + y, width, height, 0, WINDOW_MOVE_ON_TOP | WINDOW_PAD_OVERLAY)
            : -1;
        if (win == -1) {
            mem_free(under);
            mem_free(art);
            continue;
        }

        pad_glyph_copy_under(w, x, y, width, height, under);
        memset(art, 0, width * height);
        pad_glyph_draw(art, width, width, height, 0, 0, spot->glyph);

        overlay->glyphWins[index] = win;
        overlay->under[index] = under;
        overlay->art[index] = art;
        overlay->widths[index] = width;
    }
    overlay->shown = true;
}

// Each glyph pixel blended with the pixel under it at step/STEPS strength.
static void pad_glyph_overlay_draw(PadGlyphOverlay* overlay, int step)
{
    int height = pad_glyph_size();
    int amount = step * 256 / PAD_GLYPH_FADE_STEPS;
    for (int index = 0; index < overlay->count; index++) {
        int win = overlay->glyphWins[index];
        if (win == -1) {
            continue;
        }
        unsigned char* buf = win_get_buf(win);
        int size = overlay->widths[index] * height;
        for (int pixel = 0; pixel < size; pixel++) {
            int under = overlay->under[index][pixel];
            int glyph = overlay->art[index][pixel];
            if (glyph == 0 || amount <= 0) {
                buf[pixel] = (unsigned char)under;
            } else if (amount >= 256) {
                buf[pixel] = (unsigned char)glyph;
            } else {
                int r = (cmap[under * 3] * (256 - amount) + cmap[glyph * 3] * amount) >> 8;
                int g = (cmap[under * 3 + 1] * (256 - amount) + cmap[glyph * 3 + 1] * amount) >> 8;
                int b = (cmap[under * 3 + 2] * (256 - amount) + cmap[glyph * 3 + 2] * amount) >> 8;
                buf[pixel] = colorTable[((r >> 1) << 10) | ((g >> 1) << 5) | (b >> 1)];
            }
        }
        win_draw(win);
    }
}

// Whether a registered window is showing, on top where its first glyph goes.
static bool pad_glyph_overlay_visible(PadGlyphOverlay* overlay, Window** out)
{
    Window* w = GNW_find(overlay->win);
    if (w == NULL || (w->flags & WINDOW_HIDDEN) != 0 || overlay->count == 0) {
        return false;
    }
    *out = w;
    int top = win_get_top_win(w->rect.ulx + overlay->spots[0].x, w->rect.uly + overlay->spots[0].y);
    if (top == overlay->win) {
        return true;
    }
    // Our own glyph window is there.
    for (int index = 0; index < overlay->count; index++) {
        if (overlay->shown && overlay->glyphWins[index] != -1 && overlay->glyphWins[index] == top) {
            return true;
        }
    }
    return false;
}

void pad_glyph_spots_update(float trigger)
{
    unsigned int now = get_time();
    float dt = (float)(now - pad_glyph_last_time);
    pad_glyph_last_time = now;
    if (dt > 100.0f) {
        dt = 100.0f;
    }

    float target = (trigger - 0.1f) / 0.75f;
    target = target < 0.0f ? 0.0f : target > 1.0f ? 1.0f : target;
    float maxStep = dt / PAD_GLYPH_FADE_MS;
    if (pad_glyph_alpha < target) {
        pad_glyph_alpha = pad_glyph_alpha + maxStep < target ? pad_glyph_alpha + maxStep : target;
    } else if (pad_glyph_alpha > target) {
        pad_glyph_alpha = pad_glyph_alpha - maxStep > target ? pad_glyph_alpha - maxStep : target;
    }
    int step = (int)(pad_glyph_alpha * PAD_GLYPH_FADE_STEPS + 0.5f);

    bool redraw = step != pad_glyph_drawn_step;
    pad_glyph_drawn_step = step;

    for (int slot = 0; slot < PAD_GLYPH_OVERLAY_WINDOWS; slot++) {
        PadGlyphOverlay* overlay = &(pad_glyph_overlays[slot]);
        if (overlay->win == -1) {
            continue;
        }
        Window* w;
        if (step == 0 || !pad_glyph_overlay_visible(overlay, &w)) {
            if (overlay->shown) {
                pad_glyph_overlay_close(overlay);
            }
            if (GNW_find(overlay->win) == NULL) {
                overlay->win = -1; // the window went
            }
            continue;
        }
        if (!overlay->shown) {
            pad_glyph_overlay_open(overlay, w);
            pad_glyph_overlay_draw(overlay, step);
        } else if (redraw) {
            pad_glyph_overlay_draw(overlay, step);
        }
    }
}

} // namespace fallout
