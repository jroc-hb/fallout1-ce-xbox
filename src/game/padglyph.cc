#include "game/padglyph.h"

#include <string.h>

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
    if (glyph != PAD_GLYPH_RS && glyph != PAD_GLYPH_LS && glyph != PAD_GLYPH_START && glyph != PAD_GLYPH_BACK) {
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
    if (glyph != PAD_GLYPH_WHITE && glyph != PAD_GLYPH_START && glyph != PAD_GLYPH_BACK) {
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

} // namespace fallout
