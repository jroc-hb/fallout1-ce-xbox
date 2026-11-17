#include "plib/gnw/svga.h"

#include "plib/gnw/bcdebug.h" // TEMP-HANG-DEBUG
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/winmain.h"

#include <algorithm>
#include <cmath>

#ifdef NXDK
#include <hal/video.h>
#include <hal/debug.h>
#include <xboxkrnl/xboxkrnl.h>
#endif

namespace fallout {

static bool createRenderer(int width, int height);
static void destroyRenderer();

#ifdef NXDK
// Where the game's image goes in the video mode: centered, inside the safe
// area, scaled up for the larger interface settings.
static SDL_FRect gPresentRect;

// Works out the size the game draws at (the screen size every game window and
// the mouse use) for a video mode, from the XBOX OPTS settings:
// - the safe area shrinks the usable part of the mode on every edge;
// - the interface scale draws the game smaller and lets the GPU scale it up,
//   which is what makes the fixed-size interface windows bigger;
// - the game can't draw below 640x480, so at 480p a safe area is reached by
//   scaling the image down instead.
// Fills gPresentRect and returns the scale from game pixels to screen pixels.
static float svgaComputeLayout(int modeWidth, int modeHeight, int uiScale, int safeArea, int* width, int* height)
{
    safeArea = std::clamp(safeArea, 0, 8);
    uiScale = std::clamp(uiScale, 0, 2);
    if (modeWidth < 1280) {
        uiScale = 0;
    }

    static const float scales[3] = { 1.0f, 4.0f / 3.0f, 1.5f };
    float areaWidth = (float)modeWidth * (100 - 2 * safeArea) / 100.0f;
    float areaHeight = (float)modeHeight * (100 - 2 * safeArea) / 100.0f;

    // Game size: the area at the chosen scale, kept to the area's shape but
    // never below 640x480, and a multiple of 8 wide (the dirty-rect uploads
    // work in 8x4 blocks).
    float gameWidth = areaWidth / scales[uiScale];
    float gameHeight = areaHeight / scales[uiScale];
    if (gameHeight < 480.0f) {
        gameWidth = gameWidth * 480.0f / gameHeight;
        gameHeight = 480.0f;
    }
    if (gameWidth < 640.0f) {
        gameHeight = gameHeight * 640.0f / gameWidth;
        gameWidth = 640.0f;
    }
    *width = std::max(640, ((int)gameWidth) & ~7);
    *height = std::max(480, ((int)gameHeight) & ~1);

    float scale = std::min(areaWidth / *width, areaHeight / *height);
    // Snap to exactly 1:1 when it's that close, so it stays pixel-sharp.
    if (std::fabs(scale - 1.0f) < 0.01f) {
        scale = 1.0f;
    }

    gPresentRect.w = (float)(int)(*width * scale + 0.5f);
    gPresentRect.h = (float)(int)(*height * scale + 0.5f);
    gPresentRect.x = (float)(int)((modeWidth - gPresentRect.w) / 2);
    gPresentRect.y = (float)(int)((modeHeight - gPresentRect.h) / 2);
    return scale;
}

// Bounding box of gSdlSurface changed since the last upload to gSdlTexture.
static SDL_Rect dirty_rect;
static bool dirty = false;
#endif

// TEMP-HANG-DEBUG
volatile const char* g_bc_main = "init";
volatile const char* g_bc_audio = "init";
volatile unsigned g_bc_loops = 0;
#ifdef NXDK
#include <pbkit/pbkit.h>
extern "C" volatile int xgu_dbg_stage;
static DWORD WINAPI bc_watchdog(LPVOID)
{
    unsigned last = 0;
    for (;;) {
        Sleep(1000);
        unsigned loops = g_bc_loops;
        DbgPrint("HANGDBG t=%u loops=%u%s main=%s audio=%s xgu=%d vbl=%lu pcrtc=%08lx bb=%08lx\n", (unsigned)SDL_GetTicks(), loops - last,
            loops == last ? " STALLED" : "", (const char*)g_bc_main, (const char*)g_bc_audio, xgu_dbg_stage,
            (unsigned long)pb_get_vbl_counter(), (unsigned long)(VIDEOREG(0x00600800) & 0x03FFFFFF), (unsigned long)((uint32_t)pb_back_buffer() & 0x03FFFFFF));
        last = loops;
    }
    return 0;
}
void bc_start_watchdog() { CreateThread(NULL, 16384, bc_watchdog, NULL, 0, NULL); }
#else
void bc_start_watchdog() { }
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
void svgaMarkDirty(const SDL_Rect* rect)
{
    if (rect->w <= 0 || rect->h <= 0) {
        return;
    }

    if (dirty) {
        SDL_GetRectUnion(&dirty_rect, rect, &dirty_rect);
    } else {
        dirty_rect = *rect;
        dirty = true;
    }
}

// Uploads the changed part of the 8-bit screen surface to the paletted
// texture. The GPU does the palette lookup, so palette changes (color cycling,
// fades) cost nothing here.
static void uploadDirtyRect()
{
    if (!dirty) {
        return;
    }
    dirty = false;

    // The driver swizzles 8x4 pixel blocks fastest, so grow the rect to that
    // alignment. The surface is the source, so the extra pixels are valid.
    int x0 = dirty_rect.x & ~7;
    int y0 = dirty_rect.y & ~3;
    int x1 = std::min((dirty_rect.x + dirty_rect.w + 7) & ~7, gSdlSurface->w);
    int y1 = std::min((dirty_rect.y + dirty_rect.h + 3) & ~3, gSdlSurface->h);

    SDL_Rect rect = { x0, y0, x1 - x0, y1 - y0 };
    SDL_UpdateTexture(gSdlTexture, &rect,
        static_cast<unsigned char*>(gSdlSurface->pixels) + gSdlSurface->pitch * y0 + x0,
        gSdlSurface->pitch);
}
#endif

// 0x4CB310
void GNW95_SetPaletteEntries(unsigned char* palette, int start, int count)
{
    SDL_Palette* surfacePalette = gSdlSurface != NULL ? SDL_GetSurfacePalette(gSdlSurface) : NULL;
    if (surfacePalette != NULL) {
        SDL_Color colors[256];

        if (count != 0) {
            for (int index = 0; index < count; index++) {
                colors[index].r = palette[index * 3] << 2;
                colors[index].g = palette[index * 3 + 1] << 2;
                colors[index].b = palette[index * 3 + 2] << 2;
                colors[index].a = 255;
            }
        }

        SDL_SetPaletteColors(surfacePalette, colors, start, count);
    }
}

// 0x4CB568
void GNW95_SetPalette(unsigned char* palette)
{
    SDL_Palette* surfacePalette = gSdlSurface != NULL ? SDL_GetSurfacePalette(gSdlSurface) : NULL;
    if (surfacePalette != NULL) {
        SDL_Color colors[256];

        for (int index = 0; index < 256; index++) {
            colors[index].r = palette[index * 3] << 2;
            colors[index].g = palette[index * 3 + 1] << 2;
            colors[index].b = palette[index * 3 + 2] << 2;
            colors[index].a = 255;
        }

        SDL_SetPaletteColors(surfacePalette, colors, 0, 256);
    }
}

// 0x4CB850
void GNW95_ShowRect(unsigned char* src, unsigned int srcPitch, unsigned int a3,
                    unsigned int srcX, unsigned int srcY, unsigned int srcWidth,
                    unsigned int srcHeight, unsigned int destX, unsigned int destY)
{
    buf_to_buf(src + srcPitch * srcY + srcX, srcWidth, srcHeight, srcPitch,
               (unsigned char*)gSdlSurface->pixels + gSdlSurface->pitch * destY + destX,
               gSdlSurface->pitch);

#ifdef NXDK
    SDL_Rect rect = { static_cast<int>(destX), static_cast<int>(destY),
        static_cast<int>(srcWidth), static_cast<int>(srcHeight) };
    svgaMarkDirty(&rect);
#else
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
#else
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
#endif

    debugPrint("Initializing the video");

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        return false;
    }

#ifdef NXDK
    // Deliberately not SDL_WINDOW_FULLSCREEN: SDL3 replaces a fullscreen
    // window's requested size with the display bounds, and the nxdk driver's
    // primary display is 640x480, so the f1_res.ini resolution would never
    // reach it. The driver makes the window fullscreen on its own and sets the
    // video mode from the size requested here (640x480 -> 480p, 1280x720 or
    // larger -> 720p).
    SDL_WindowFlags windowFlags = 0;
#else
    SDL_WindowFlags windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (video_options->fullscreen) {
        windowFlags |= SDL_WINDOW_FULLSCREEN;
    }
#endif

    gSdlWindow = SDL_CreateWindow(GNW95_title,
        video_options->width * video_options->scale,
        video_options->height * video_options->scale,
        windowFlags);
    if (gSdlWindow == NULL) {
        return false;
    }

#ifdef NXDK
    // The nxdk video driver picks the closest mode it can actually set (480p or
    // 720p) and resizes the window accordingly, so take the resolution it gave
    // us rather than the one requested in f1_res.ini.
    int actualWidth;
    int actualHeight;
    SDL_GetWindowSize(gSdlWindow, &actualWidth, &actualHeight);
    if (actualWidth > 0 && actualHeight > 0) {
        if (actualWidth != video_options->width || actualHeight != video_options->height) {
            debugPrint("Requested %dx%d, using %dx%d\n",
                video_options->width, video_options->height, actualWidth, actualHeight);
        }
        video_options->width = actualWidth;
        video_options->height = actualHeight;
    }

    int modeWidth = video_options->width;
    int modeHeight = video_options->height;
    float presentScale = svgaComputeLayout(modeWidth, modeHeight, video_options->uiScale, video_options->safeArea,
        &(video_options->width), &(video_options->height));
    DbgPrint("svga_init: mode %dx%d, game %dx%d, scale %d/100, at %d,%d\n", modeWidth, modeHeight,
        video_options->width, video_options->height, (int)(presentScale * 100.0f), (int)gPresentRect.x, (int)gPresentRect.y);
#endif

    if (!createRenderer(video_options->width, video_options->height)) {
        destroyRenderer();
        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;
        return false;
    }

    gSdlSurface = SDL_CreateSurface(video_options->width, video_options->height, SDL_PIXELFORMAT_INDEX8);
    if (gSdlSurface == NULL) {
        destroyRenderer();
        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;
        return false;
    }

    // SDL3 no longer attaches a palette to indexed surfaces automatically.
    SDL_Palette* surfacePalette = SDL_CreateSurfacePalette(gSdlSurface);
    if (surfacePalette == NULL) {
        SDL_DestroySurface(gSdlSurface);
        gSdlSurface = NULL;
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

    SDL_SetPaletteColors(surfacePalette, colors, 0, 256);

#ifdef NXDK
    // Share the surface's palette, so SDL re-uploads it to the GPU whenever
    // the game changes it.
    if (!SDL_SetTexturePalette(gSdlTexture, surfacePalette)) {
        SDL_DestroySurface(gSdlSurface);
        gSdlSurface = NULL;
        destroyRenderer();
        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;
        return false;
    }

    SDL_Rect screenRect = { 0, 0, gSdlSurface->w, gSdlSurface->h };
    svgaMarkDirty(&screenRect);
#endif

    scr_size.ulx = 0;
    scr_size.uly = 0;
    scr_size.lrx = video_options->width - 1;
    scr_size.lry = video_options->height - 1;

    mouse_blit_trans = NULL;
    scr_blit = GNW95_ShowRect;
    mouse_blit = GNW95_ShowRect;

    bc_start_watchdog(); // TEMP-HANG-DEBUG

    return true;
}

void svga_exit()
{
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
    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, NULL);
    if (gSdlRenderer == NULL) {
        return false;
    }

    // NXDK: pbkit always flips on vblank, so there is no vsync to request -
    // the SDL2 SDL_RENDERER_PRESENTVSYNC flag has no SDL3 equivalent here.

#ifndef NXDK
    if (!SDL_SetRenderLogicalPresentation(gSdlRenderer, width, height, SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        return false;
    }
#endif

#ifdef NXDK
    // An 8-bit paletted texture: the GPU does the palette lookup. The palette
    // is attached in svga_init once the screen surface exists.
    gSdlTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_INDEX8,
                                    SDL_TEXTUREACCESS_STREAMING, width, height);
    if (gSdlTexture != NULL) {
        SDL_SetTextureBlendMode(gSdlTexture, SDL_BLENDMODE_NONE);
    }
#else
    gSdlTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_XRGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, width, height);
#endif

    if (gSdlTexture == NULL) {
        return false;
    }

    // Replaces the SDL2 SDL_HINT_RENDER_SCALE_QUALITY="0" hint.
#ifdef NXDK
    // Sharp pixels at 1:1; smooth them when the image is scaled by a
    // non-whole amount, where nearest would make uneven pixels.
    SDL_SetTextureScaleMode(gSdlTexture,
        gPresentRect.w == (float)width ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
#else
    SDL_SetTextureScaleMode(gSdlTexture, SDL_SCALEMODE_NEAREST);
#endif

#ifndef NXDK
    gSdlTextureSurface = SDL_CreateSurface(width, height, gSdlTexture->format);
    if (gSdlTextureSurface == NULL) {
        return false;
    }
#endif

    return true;
}

static void destroyRenderer()
{
    if (gSdlTextureSurface != NULL) {
        SDL_DestroySurface(gSdlTextureSurface);
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
#ifdef NXDK
    // The window is the whole screen and the video mode is only chosen at
    // launch, so its size never really changes. SDL still sends a
    // PIXEL_SIZE_CHANGED (1280x720 -> 1280x720) shortly after startup when it
    // syncs the window with the driver. Tearing down and recreating the
    // renderer in response left the screen black at 720p, so don't.
    return;
#endif

    destroyRenderer();
    createRenderer(screenGetWidth(), screenGetHeight());
}

void renderPresent()
{
#ifdef NXDK
    BC_MAIN("rp:upload");
    uploadDirtyRect();
#else
    SDL_UpdateTexture(gSdlTexture, NULL, gSdlTextureSurface->pixels, gSdlTextureSurface->pitch);
#endif

    BC_MAIN("rp:clear");
    SDL_RenderClear(gSdlRenderer);
    BC_MAIN("rp:texture");
#ifdef NXDK
    SDL_RenderTexture(gSdlRenderer, gSdlTexture, NULL, &gPresentRect);
#else
    SDL_RenderTexture(gSdlRenderer, gSdlTexture, NULL, NULL);
#endif
    BC_MAIN("rp:present");
    SDL_RenderPresent(gSdlRenderer);
    BC_MAIN("rp:done");
}

} // namespace fallout