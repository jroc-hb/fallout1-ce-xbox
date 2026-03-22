#include "plib/gnw/svga.h"

#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/winmain.h"

#ifdef NXDK
#include <hal/video.h>
#include <hal/debug.h>
#include <xboxkrnl/xboxkrnl.h>
#endif

namespace fallout {

static bool createRenderer(int width, int height);
static void destroyRenderer();

#ifdef NXDK
static bool palette_changed = false;
static void* texture_pixels = nullptr;
static int texture_pitch = 0;
static bool texture_locked = false;
static Uint32* palette_32bit = nullptr;
#endif

// screen rect
Rect scr_size;

// 0x6ACA18
ScreenBlitFunc* scr_blit = GNW95_ShowRect;

SDL_Window* gSdlWindow = NULL;
SDL_Surface* gSdlSurface = NULL;
SDL_Renderer* gSdlRenderer = NULL;
SDL_Texture* gSdlTexture = NULL;
SDL_Surface* gSdlTextureSurface = NULL;

// TODO: Remove once migration to update-render cycle is completed.
FpsLimiter sharedFpsLimiter;

#ifdef NXDK
static void rebuildPalette32()
{
    if (!palette_32bit) {
        palette_32bit = new Uint32[256];
    }
    SDL_Color* colors = gSdlSurface->format->palette->colors;
    for (int i = 0; i < 256; i++) {
        palette_32bit[i] = (colors[i].a << 24) | (colors[i].r << 16) | (colors[i].g << 8) | colors[i].b;
    }
}

// 8-to-32 bit palette conversion, unrolled 8 pixels per iteration.
static void convert_8to32_unrolled(const Uint8* src, Uint32* dst, int width, int height,
                                   int src_pitch, int dst_pitch)
{
    dst_pitch /= sizeof(Uint32);
    for (int y = 0; y < height; y++) {
        const Uint8* src_row = src + y * src_pitch;
        Uint32* dst_row = dst + y * dst_pitch;
        int x = 0;
        for (; x <= width - 8; x += 8) {
            dst_row[x]     = palette_32bit[src_row[x]];
            dst_row[x + 1] = palette_32bit[src_row[x + 1]];
            dst_row[x + 2] = palette_32bit[src_row[x + 2]];
            dst_row[x + 3] = palette_32bit[src_row[x + 3]];
            dst_row[x + 4] = palette_32bit[src_row[x + 4]];
            dst_row[x + 5] = palette_32bit[src_row[x + 5]];
            dst_row[x + 6] = palette_32bit[src_row[x + 6]];
            dst_row[x + 7] = palette_32bit[src_row[x + 7]];
        }
        for (; x < width; x++) {
            dst_row[x] = palette_32bit[src_row[x]];
        }
    }
}

static void convert_8to32(const Uint8* src, Uint32* dst, int width, int height,
                           int src_pitch, int dst_pitch)
{
    dst_pitch /= sizeof(Uint32);
    for (int y = 0; y < height; y++) {
        const Uint8* src_row = src + y * src_pitch;
        Uint32* dst_row = dst + y * dst_pitch;
        for (int x = 0; x < width; x++) {
            dst_row[x] = palette_32bit[src_row[x]];
        }
    }
}

// Converts and blits directly into the locked texture, bypassing SDL_BlitSurface
// Xbox has a unified memory architecture so we can avoid some redundant memory copies
static void GNW95_ShowRect_Xbox(unsigned char* src, unsigned int srcPitch,
                                unsigned int a3, unsigned int srcX, unsigned int srcY,
                                unsigned int srcWidth, unsigned int srcHeight,
                                unsigned int destX, unsigned int destY)
{
    if (texture_locked && texture_pixels) {
        Uint32* dst_pixels = static_cast<Uint32*>(texture_pixels);
        Uint32* dst_ptr = dst_pixels + destY * (texture_pitch / sizeof(Uint32)) + destX;
        const Uint8* src_ptr = src + srcY * srcPitch + srcX;

        // Use unrolled path when destination is 16-byte aligned and width is a multiple of 8.
        if (((uintptr_t)dst_ptr & 15) == 0 && (srcWidth % 8) == 0) {
            convert_8to32_unrolled(src_ptr, dst_ptr, srcWidth, srcHeight, srcPitch, texture_pitch);
        } else {
            convert_8to32(src_ptr, dst_ptr, srcWidth, srcHeight, srcPitch, texture_pitch);
        }

        // Needed for the fullscreen palette fade effects
        buf_to_buf(const_cast<unsigned char*>(src_ptr),
                   static_cast<int>(srcWidth),
                   static_cast<int>(srcHeight),
                   static_cast<int>(srcPitch),
                   static_cast<unsigned char*>(gSdlSurface->pixels) + gSdlSurface->pitch * destY + destX,
                   gSdlSurface->pitch);
        return;
    }
}
#endif

// 0x4CB310
void GNW95_SetPaletteEntries(unsigned char* palette, int start, int count)
{
    if (gSdlSurface != NULL && gSdlSurface->format->palette != NULL) {
        SDL_Color colors[256];

        if (count != 0) {
            for (int index = 0; index < count; index++) {
                colors[index].r = palette[index * 3] << 2;
                colors[index].g = palette[index * 3 + 1] << 2;
                colors[index].b = palette[index * 3 + 2] << 2;
                colors[index].a = 255;
            }
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, start, count);
#ifdef NXDK
        if (palette_32bit) {
            rebuildPalette32();
        }
        palette_changed = true;
#endif
    }
}

// 0x4CB568
void GNW95_SetPalette(unsigned char* palette)
{
    if (gSdlSurface != NULL && gSdlSurface->format->palette != NULL) {
        SDL_Color colors[256];

        for (int index = 0; index < 256; index++) {
            colors[index].r = palette[index * 3] << 2;
            colors[index].g = palette[index * 3 + 1] << 2;
            colors[index].b = palette[index * 3 + 2] << 2;
            colors[index].a = 255;
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);
#ifdef NXDK
        if (palette_32bit) {
            rebuildPalette32();
        }
        palette_changed = true;
#endif
    }
}

// 0x4CB850
void GNW95_ShowRect(unsigned char* src, unsigned int srcPitch, unsigned int a3,
                    unsigned int srcX, unsigned int srcY, unsigned int srcWidth,
                    unsigned int srcHeight, unsigned int destX, unsigned int destY)
{
#ifdef NXDK
    GNW95_ShowRect_Xbox(src, srcPitch, a3, srcX, srcY, srcWidth, srcHeight, destX, destY);
#else
    buf_to_buf(src + srcPitch * srcY + srcX, srcWidth, srcHeight, srcPitch,
               (unsigned char*)gSdlSurface->pixels + gSdlSurface->pitch * destY + destX,
               gSdlSurface->pitch);

    SDL_Rect srcRect;
    srcRect.x = destX;
    srcRect.y = destY;
    srcRect.w = srcWidth;
    srcRect.h = srcHeight;

    SDL_Rect destRect;
    destRect.x = destX;
    destRect.y = destY;
    SDL_BlitSurface(gSdlSurface, &srcRect, gSdlTextureSurface, &destRect);
#endif
}

bool svga_init(VideoOptions* video_options)
{
#ifdef NXDK
    Sleep(1000);
    // Based on LithiumX solution to detect Xbox resolution: 
    // https://github.com/Ryzee119/LithiumX/blob/f4471d287d44abc84803d3b901bd4aa7ed459689/src/platform/xbox/platform.c#L99
    // First try the user-specified resolution in f1_res.ini, then fall back to 480p
    // NOTE: 1080i is currently broken in the experimental SDL2 hardware renderer lib
    if (XVideoSetMode(video_options->width, video_options->height, 32, REFRESH_DEFAULT) == false) {
        if (XVideoSetMode(640, 480, 32, REFRESH_DEFAULT)) {
            video_options->width = 640;
            video_options->height = 480;
        }
    }
#else
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
#endif

    debugPrint("Initializing the video");

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        return false;
    }

#ifdef NXDK
    Uint32 windowFlags = SDL_WINDOW_FULLSCREEN;
#else
    Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;
    if (video_options->fullscreen) {
        windowFlags |= SDL_WINDOW_FULLSCREEN;
    }
#endif

    gSdlWindow = SDL_CreateWindow(GNW95_title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        video_options->width * video_options->scale,
        video_options->height * video_options->scale,
        windowFlags);
    if (gSdlWindow == NULL) {
        return false;
    }

    if (!createRenderer(video_options->width, video_options->height)) {
        destroyRenderer();
        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;
        return false;
    }

    gSdlSurface = SDL_CreateRGBSurface(0, video_options->width, video_options->height, 8, 0, 0, 0, 0);
    if (gSdlSurface == NULL) {
        destroyRenderer();
        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;
        return false;
    }

    SDL_Color colors[256];
    for (int index = 0; index < 256; index++) {
        colors[index].r = index;
        colors[index].g = index;
        colors[index].b = index;
        colors[index].a = 255;
    }

    SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);

    scr_size.ulx = 0;
    scr_size.uly = 0;
    scr_size.lrx = video_options->width - 1;
    scr_size.lry = video_options->height - 1;

    mouse_blit_trans = NULL;
    scr_blit = GNW95_ShowRect;
    mouse_blit = GNW95_ShowRect;

#ifdef NXDK
    rebuildPalette32();
#endif

    return true;
}

void svga_exit()
{
#ifdef NXDK
    if (palette_32bit) {
        delete[] palette_32bit;
        palette_32bit = nullptr;
    }
#endif

    destroyRenderer();

    if (gSdlWindow != NULL) {
        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

int screenGetWidth()
{
    // TODO: Make it on par with _xres;
    return rectGetWidth(&scr_size);
}

int screenGetHeight()
{
    // TODO: Make it on par with _yres.
    return rectGetHeight(&scr_size);
}

static bool createRenderer(int width, int height)
{
#ifdef NXDK
    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!gSdlRenderer) {
        gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, SDL_RENDERER_SOFTWARE);
    }
#else
    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, 0);
#endif

    if (gSdlRenderer == NULL) {
        return false;
    }

    if (SDL_RenderSetLogicalSize(gSdlRenderer, width, height) != 0) {
        return false;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

#ifdef NXDK
    gSdlTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, width, height);
    if (gSdlTexture) {
        texture_locked = (SDL_LockTexture(gSdlTexture, NULL, &texture_pixels, &texture_pitch) == 0);
        if (texture_locked) {
            debugPrint("Direct texture access enabled (pitch: %d)\n", texture_pitch);
        }
    }
#else
    gSdlTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_RGB888,
                                    SDL_TEXTUREACCESS_STREAMING, width, height);
#endif

    if (gSdlTexture == NULL) {
        return false;
    }

    Uint32 format;
    if (SDL_QueryTexture(gSdlTexture, &format, NULL, NULL, NULL) != 0) {
        return false;
    }

#ifndef NXDK
    gSdlTextureSurface = SDL_CreateRGBSurfaceWithFormat(0, width, height, SDL_BITSPERPIXEL(format), format);
    if (gSdlTextureSurface == NULL) {
        return false;
    }
#endif

    return true;
}

static void destroyRenderer()
{
#ifdef NXDK
    if (texture_locked && gSdlTexture) {
        SDL_UnlockTexture(gSdlTexture);
        texture_locked = false;
        texture_pixels = nullptr;
    }
#endif

    if (gSdlTextureSurface != NULL) {
        SDL_FreeSurface(gSdlTextureSurface);
        gSdlTextureSurface = NULL;
    }

    if (gSdlTexture != NULL) {
        SDL_DestroyTexture(gSdlTexture);
        gSdlTexture = NULL;
    }

    if (gSdlRenderer != NULL) {
        SDL_DestroyRenderer(gSdlRenderer);
        gSdlRenderer = NULL;
    }
}

void handleWindowSizeChanged()
{
    destroyRenderer();
    createRenderer(screenGetWidth(), screenGetHeight());
}

void renderPresent()
{
#ifdef NXDK
    if (texture_locked) {
        if (palette_changed && texture_pixels) {
            // Palette changed mid-frame; reconvert the entire 8-bit surface.
            // Needed for fullscreen palette fade effects like the main menu
            convert_8to32_unrolled(static_cast<const Uint8*>(gSdlSurface->pixels),
                                   static_cast<Uint32*>(texture_pixels),
                                   gSdlSurface->w, gSdlSurface->h,
                                   gSdlSurface->pitch, texture_pitch);
            palette_changed = false;
        }
        SDL_UnlockTexture(gSdlTexture);
        texture_locked = false;
        texture_locked = (SDL_LockTexture(gSdlTexture, NULL, &texture_pixels, &texture_pitch) == 0);
    }
#else
    SDL_UpdateTexture(gSdlTexture, NULL, gSdlTextureSurface->pixels, gSdlTextureSurface->pitch);
#endif

    SDL_RenderClear(gSdlRenderer);
    SDL_RenderCopy(gSdlRenderer, gSdlTexture, NULL, NULL);
    SDL_RenderPresent(gSdlRenderer);
}

#ifdef NXDK
// Temporarily unlock the texture before movie frame blitting via SDL_BlitSurface.
void movieFrameStart()
{
    if (texture_locked && gSdlTexture) {
        SDL_UnlockTexture(gSdlTexture);
        texture_locked = false;
    }
}

void movieFrameEnd()
{
    if (!texture_locked && gSdlTexture) {
        texture_locked = (SDL_LockTexture(gSdlTexture, NULL, &texture_pixels, &texture_pitch) == 0);
    }
}

// Converts gSdlSurface (8-bit) into the locked texture.
// Called by movie playback after blitting a decoded frame into gSdlSurface,
void svgaConvertSurfaceToTexture()
{
    if (texture_pixels && gSdlSurface) {
        convert_8to32_unrolled(
            static_cast<const Uint8*>(gSdlSurface->pixels),
            static_cast<Uint32*>(texture_pixels),
            gSdlSurface->w, gSdlSurface->h,
            gSdlSurface->pitch, texture_pitch);
    }
}
#endif

} // namespace fallout