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
    PAD_GLYPH_HOLD, // dark pill, "HOLD": goes under a button glyph whose
                    // action needs the button held (pad_glyph_draw_held)
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

// A button glyph with the HOLD pill under it, centred on each other: for a
// button that does something else when tapped. Its size:
int pad_glyph_held_width(PadGlyph glyph);
int pad_glyph_held_height();
void pad_glyph_draw_held(unsigned char* buf, int pitch, int width, int height, int x, int y, PadGlyph glyph);

// Hold-LT glyph overlays outside the HUD (intface.cc has the HUD's): spots
// on a window, glyph centres in its coordinates. padModeOnly: only while the
// pointer is hidden (the button means something else in pointer mode).
typedef struct PadGlyphSpot {
    int x;
    int y;
    PadGlyph glyph;
    bool padModeOnly;
} PadGlyphSpot;

// Registers (replaces) a window's spots; clear when the window goes (a
// deleted window is also dropped on its own).
void pad_glyph_spots_set(int win, const PadGlyphSpot* spots, int count);
void pad_glyph_spots_clear(int win);
// Every frame with LT's value (gamepad.cc, next to intface_show_pad_glyphs).
void pad_glyph_spots_update(float trigger);

} // namespace fallout

#endif /* FALLOUT_GAME_PADGLYPH_H_ */
