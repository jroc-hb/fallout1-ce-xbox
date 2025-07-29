#include "game/palette.h"

#include <string.h>

#include "game/cycle.h"
#include "game/gsound.h"
#include "plib/color/color.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/input.h"

namespace fallout {

// 0x661F20
static unsigned char current_palette[256 * 3];

// 0x662220
unsigned char white_palette[256 * 3];

// 0x662520
unsigned char black_palette[256 * 3];

// 0x662820
static int fade_steps;

// 0x485090
void palette_init()
{
    memset(black_palette, 0, 256 * 3);
    memset(white_palette, 63, 256 * 3);
    memcpy(current_palette, cmap, 256 * 3);

    unsigned int tick = get_time();
    if (gsound_background_is_enabled() || gsound_speech_is_enabled()) {
        colorSetFadeBkFunc(soundUpdate);
    }

    fadeSystemPalette(current_palette, current_palette, 60);

    colorSetFadeBkFunc(NULL);

    // Actual fade duration will never be 0 since |fadeSystemPalette| uses
    // frame rate throttling.
    unsigned int actualFadeDuration = elapsed_time(tick);

    // Calculate fade steps needed to perform fading in about 700 ms.
    fade_steps = 60 * 700 / actualFadeDuration;

    debug_printf("\nFade time is %u\nFade steps are %d\n", actualFadeDuration, fade_steps);
}

// 0x485160
void palette_reset()
{
}

// 0x485160
void palette_exit()
{
}

// 0x485164
void palette_fade_to(unsigned char* palette)
{
    bool colorCycleWasEnabled = cycle_is_enabled();
    cycle_disable();

    if (gsound_background_is_enabled() || gsound_speech_is_enabled()) {
        colorSetFadeBkFunc(soundUpdate);
    }

    fadeSystemPalette(current_palette, palette, fade_steps);
    colorSetFadeBkFunc(NULL);

    memcpy(current_palette, palette, 768);

    if (colorCycleWasEnabled) {
        cycle_enable();
    }
}

// 0x4851D8
void palette_set_to(unsigned char* palette)
{
    memcpy(current_palette, palette, sizeof(current_palette));
    setSystemPalette(palette);
}

// 0x485208
void palette_set_entries(unsigned char* palette, int start, int end)
{
    memcpy(current_palette + 3 * start, palette, 3 * (end - start + 1));
    setSystemPaletteEntries(palette, start, end);
}

#ifdef NXDK
// Takes the raw screenshot thumbnail from a save, converts it to 8-bit BMP using the current palette, then saves the file as
// a cropped 64x64 pixel xbx so that it appears on the Xbox dashboard with the corresponding save file
void save_thumbnail_bmp(const char* path, const uint8_t* pixels, int width, int height) {
    int cropSize = (width < height) ? width : height;
    int xOffset = (width - cropSize) / 2;
    int yOffset = (height - cropSize) / 2;

    // Allocate space for final 64x64 output
    uint8_t* output = (uint8_t*)malloc(64 * 64);
    if (!output) {
        debug_printf("Failed to allocate memory for thumbnail.\n");
        return;
    }

    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            int srcX = xOffset + (x * cropSize) / 64;
            int srcY = yOffset + (y * cropSize) / 64;
            output[y * 64 + x] = pixels[srcY * width + srcX];
        }
    }

    FILE* fp = fopen(path, "wb");
    if (!fp) {
        debug_printf("Failed to open BMP file: %s\n", path);
        free(output);
        return;
    }

    uint8_t bmp_header[14] = {
        'B', 'M',
        0, 0, 0, 0,       // File size (filled below)
        0, 0,
        0, 0,
        0, 0, 0, 0        // Pixel data offset
    };

    uint8_t dib_header[40] = {0};
    *(uint32_t*)&dib_header[0]  = 40;
    *(int32_t*)&dib_header[4]   = 64;
    *(int32_t*)&dib_header[8]   = -64; // Top-down BMP
    *(uint16_t*)&dib_header[12] = 1;
    *(uint16_t*)&dib_header[14] = 8;
    *(uint32_t*)&dib_header[20] = 64 * 64;
    *(uint32_t*)&dib_header[24] = 2835;
    *(uint32_t*)&dib_header[28] = 2835;
    *(uint32_t*)&dib_header[32] = 256;
    *(uint32_t*)&dib_header[36] = 256;

    uint32_t pixel_data_offset = 14 + 40 + 1024;
    uint32_t file_size = pixel_data_offset + 64 * 64;
    *(uint32_t*)&bmp_header[2] = file_size;
    *(uint32_t*)&bmp_header[10] = pixel_data_offset;

    fwrite(bmp_header, 1, 14, fp);
    fwrite(dib_header, 1, 40, fp);

    // Write palette: B G R 0, scaling from 0–63 to 0–255
    for (int i = 0; i < 256; i++) {
        fputc(current_palette[i * 3 + 2] * 4, fp); // Blue
        fputc(current_palette[i * 3 + 1] * 4, fp); // Green
        fputc(current_palette[i * 3 + 0] * 4, fp); // Red
        fputc(0, fp);
    }

    // Write 64x64 8-bit image
    fwrite(output, 1, 64 * 64, fp);
    fclose(fp);
    free(output);

    debug_printf("Saved cropped + scaled BMP thumbnail to: %s\n", path);
}
#endif

} // namespace fallout
