#ifndef FALLOUT_GAME_PADGLYPH_H_
#define FALLOUT_GAME_PADGLYPH_H_

namespace fallout {

// Original Xbox controller button glyphs, drawn from scratch into 8-bit
// paletted buffers (the game's art has none): a coloured disc with the
// button's letter, like the pad's own buttons.
typedef enum PadGlyph {
    PAD_GLYPH_A, // green, black "A"
    PAD_GLYPH_B, // red, black "B"
    PAD_GLYPH_X, // blue, black "X"
    PAD_GLYPH_Y, // yellow, black "Y"
    PAD_GLYPH_BLACK, // the Black button: a black disc
    PAD_GLYPH_WHITE, // the White button: a white disc
    PAD_GLYPH_RS, // the right stick click: grey disc, "RS"
    PAD_GLYPH_LS, // the left stick click: grey disc, "LS"
    PAD_GLYPH_START, // dark pill, "START"
    PAD_GLYPH_BACK, // dark pill, "BACK"
    PAD_GLYPH_COUNT,
} PadGlyph;


// The glyph's height in pixels, and the width of a disc.
int pad_glyph_size();
// The glyph's width: pad_glyph_size() for the buttons, wider for the stick
// clicks (a pill reading "RS"/"LS").
int pad_glyph_width(PadGlyph glyph);

// Draws `glyph` with its upper left corner at buf + y * pitch + x, clipped to
// width x height. Index 0 (transparent in sprites) is never written.
void pad_glyph_draw(unsigned char* buf, int pitch, int width, int height, int x, int y, PadGlyph glyph);


} // namespace fallout

#endif /* FALLOUT_GAME_PADGLYPH_H_ */
