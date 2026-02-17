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
// Xbox-specific optimization variables
static SDL_Rect dirty_rect = {0, 0, 0, 0};
static bool full_update = true;
static bool palette_changed = false;
static Uint8 current_palette[256 * 3] = {0};

// Direct texture access for unified memory
static void* texture_pixels = nullptr;
static int texture_pitch = 0;
static bool texture_locked = false;
static Uint32* palette_32bit = nullptr; // Pre-converted 32-bit palette
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
// Pre-convert 8-bit palette to 32-bit ARGB for faster conversion
static void update_palette_32bit() {
    if (!palette_32bit) {
        palette_32bit = new Uint32[256];
    }
    
    SDL_Color* sdl_palette = gSdlSurface->format->palette->colors;
    for (int i = 0; i < 256; i++) {
        palette_32bit[i] = (sdl_palette[i].a << 24) | 
                          (sdl_palette[i].r << 16) | 
                          (sdl_palette[i].g << 8) | 
                          sdl_palette[i].b;
    }
}

// SIMD-optimized 8-bit to 32-bit conversion using Xbox's SSE1
static void convert_8to32_simd(const Uint8* src, Uint32* dst, int width, int height, 
                              int src_pitch, int dst_pitch) {
    // Xbox only has SSE1, so we need to work with floats
    // But for integer operations, we'll use a manual unrolled loop
    
    // Ensure pitch is in pixels, not bytes
    dst_pitch /= sizeof(Uint32);
    
    for (int y = 0; y < height; y++) {
        const Uint8* src_row = src + y * src_pitch;
        Uint32* dst_row = dst + y * dst_pitch;
        
        int x = 0;
        // Process 8 pixels at a time (unrolled)
        for (; x <= width - 8; x += 8) {
            // Manually unroll for better performance
            dst_row[x] = palette_32bit[src_row[x]];
            dst_row[x + 1] = palette_32bit[src_row[x + 1]];
            dst_row[x + 2] = palette_32bit[src_row[x + 2]];
            dst_row[x + 3] = palette_32bit[src_row[x + 3]];
            dst_row[x + 4] = palette_32bit[src_row[x + 4]];
            dst_row[x + 5] = palette_32bit[src_row[x + 5]];
            dst_row[x + 6] = palette_32bit[src_row[x + 6]];
            dst_row[x + 7] = palette_32bit[src_row[x + 7]];
        }
        
        // Handle remaining pixels
        for (; x < width; x++) {
            dst_row[x] = palette_32bit[src_row[x]];
        }
    }
}

// Non-SIMD fallback
static void convert_8to32_normal(const Uint8* src, Uint32* dst, int width, int height,
                                int src_pitch, int dst_pitch) {
    dst_pitch /= sizeof(Uint32);
    
    for (int y = 0; y < height; y++) {
        const Uint8* src_row = src + y * src_pitch;
        Uint32* dst_row = dst + y * dst_pitch;
        
        for (int x = 0; x < width; x++) {
            dst_row[x] = palette_32bit[src_row[x]];
        }
    }
}

// Direct-to-texture blitting - ELIMINATES intermediate copies!
void GNW95_ShowRect_Xbox(unsigned char* src, unsigned int srcPitch,
                        unsigned int a3, unsigned int srcX, unsigned int srcY,
                        unsigned int srcWidth, unsigned int srcHeight,
                        unsigned int destX, unsigned int destY) {
    
    // Update dirty rectangle
    if (!full_update) {
        if (dirty_rect.w == 0) {
            dirty_rect.x = static_cast<int>(destX);
            dirty_rect.y = static_cast<int>(destY);
            dirty_rect.w = static_cast<int>(srcWidth);
            dirty_rect.h = static_cast<int>(srcHeight);
        } else {
            int x1 = (static_cast<int>(destX) < dirty_rect.x) ? static_cast<int>(destX) : dirty_rect.x;
            int y1 = (static_cast<int>(destY) < dirty_rect.y) ? static_cast<int>(destY) : dirty_rect.y;
            int x2 = (static_cast<int>(destX) + static_cast<int>(srcWidth) > dirty_rect.x + dirty_rect.w) ?
                     static_cast<int>(destX) + static_cast<int>(srcWidth) : dirty_rect.x + dirty_rect.w;
            int y2 = (static_cast<int>(destY) + static_cast<int>(srcHeight) > dirty_rect.y + dirty_rect.h) ?
                     static_cast<int>(destY) + static_cast<int>(srcHeight) : dirty_rect.y + dirty_rect.h;
            
            dirty_rect.x = x1;
            dirty_rect.y = y1;
            dirty_rect.w = x2 - x1;
            dirty_rect.h = y2 - y1;
        }
    }
    
    // OPTION 1: Direct texture access (FASTEST - if texture is locked)
    if (texture_locked && texture_pixels) {
        Uint32* dst_pixels = static_cast<Uint32*>(texture_pixels);
        int dst_pitch_pixels = texture_pitch / sizeof(Uint32);
        Uint32* dst_ptr = dst_pixels + destY * dst_pitch_pixels + destX;
        const Uint8* src_ptr = src + srcY * srcPitch + srcX;
        
        // Direct 8-bit to 32-bit conversion into texture memory
        // Use SIMD version for Xbox (aligned access helps)
        if (((uintptr_t)dst_ptr & 15) == 0 && (srcWidth % 8) == 0) {
            // Aligned for better performance
            convert_8to32_simd(src_ptr, dst_ptr, srcWidth, srcHeight, srcPitch, texture_pitch);
        } else {
            // Unaligned fallback
            convert_8to32_normal(src_ptr, dst_ptr, srcWidth, srcHeight, srcPitch, texture_pitch);
        }
        
        // Also update the 8-bit surface (for compatibility)
        // Need to cast away const for buf_to_buf
        buf_to_buf(const_cast<unsigned char*>(src_ptr), 
                   static_cast<int>(srcWidth), 
                   static_cast<int>(srcHeight), 
                   static_cast<int>(srcPitch),
                   static_cast<unsigned char*>(gSdlSurface->pixels) + 
                   gSdlSurface->pitch * destY + destX,
                   gSdlSurface->pitch);
        return;
    }
    
    // OPTION 2: Update through SDL texture surface (slower but works)
    // Copy to 8-bit surface (need to cast away const)
    buf_to_buf(src + srcY * srcPitch + srcX, 
               static_cast<int>(srcWidth), 
               static_cast<int>(srcHeight), 
               static_cast<int>(srcPitch),
               static_cast<unsigned char*>(gSdlSurface->pixels) + 
               gSdlSurface->pitch * destY + destX,
               gSdlSurface->pitch);
    
    // Convert 8-bit to 32-bit directly into texture surface
    Uint32* dst_pixels = static_cast<Uint32*>(gSdlTextureSurface->pixels);
    int dst_pitch_pixels = gSdlTextureSurface->pitch / sizeof(Uint32);
    Uint32* dst_ptr = dst_pixels + destY * dst_pitch_pixels + destX;
    const Uint8* src_ptr = src + srcY * srcPitch + srcX;
    
    // Direct conversion avoiding SDL_BlitSurface overhead
    convert_8to32_normal(src_ptr, dst_ptr, 
                        static_cast<int>(srcWidth), 
                        static_cast<int>(srcHeight), 
                        static_cast<int>(srcPitch), 
                        gSdlTextureSurface->pitch);
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
                
                // Store the palette for fade effects
                #ifdef NXDK
                if (start + index < 256) {
                    current_palette[(start + index) * 3] = palette[index * 3];
                    current_palette[(start + index) * 3 + 1] = palette[index * 3 + 1];
                    current_palette[(start + index) * 3 + 2] = palette[index * 3 + 2];
                }
                #endif
            }
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, start, count);
        
#ifdef NXDK
        // Update pre-converted 32-bit palette
        if (palette_32bit) {
            update_palette_32bit();
        }
        
        // Force full update when palette changes for fade effects
        full_update = true;
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
            
            // Store the palette for fade effects
            #ifdef NXDK
            current_palette[index * 3] = palette[index * 3];
            current_palette[index * 3 + 1] = palette[index * 3 + 1];
            current_palette[index * 3 + 2] = palette[index * 3 + 2];
            #endif
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);
        
#ifdef NXDK
        // Update pre-converted 32-bit palette
        if (palette_32bit) {
            update_palette_32bit();
        }
        
        // Force full update when palette changes for fade effects
        full_update = true;
        palette_changed = true;
#endif
    }
}

// 0x4CB850 - Main blit function
void GNW95_ShowRect(unsigned char* src, unsigned int srcPitch, unsigned int a3, 
                   unsigned int srcX, unsigned int srcY, unsigned int srcWidth, 
                   unsigned int srcHeight, unsigned int destX, unsigned int destY)
{
#ifdef NXDK
    GNW95_ShowRect_Xbox(src, srcPitch, a3, srcX, srcY, srcWidth, srcHeight, destX, destY);
#else
    // Original code for other platforms
    buf_to_buf(src + srcPitch * srcY + srcX, 
               static_cast<int>(srcWidth), 
               static_cast<int>(srcHeight), 
               static_cast<int>(srcPitch),
               (unsigned char*)gSdlSurface->pixels + gSdlSurface->pitch * destY + destX,
               gSdlSurface->pitch);

    SDL_Rect srcRect;
    srcRect.x = static_cast<int>(destX);
    srcRect.y = static_cast<int>(destY);
    srcRect.w = static_cast<int>(srcWidth);
    srcRect.h = static_cast<int>(srcHeight);

    SDL_Rect destRect;
    destRect.x = static_cast<int>(destX);
    destRect.y = static_cast<int>(destY);
    destRect.w = static_cast<int>(srcWidth);
    destRect.h = static_cast<int>(srcHeight);
    
    SDL_BlitSurface(gSdlSurface, &srcRect, gSdlTextureSurface, &destRect);
#endif
}

bool svga_init(VideoOptions* video_options)
{
#ifdef NXDK
    Sleep(1000);
    
    if (XVideoSetMode(video_options->width, video_options->height, 32, REFRESH_DEFAULT) == false)
    {
        if (XVideoSetMode(640, 480, 32, REFRESH_DEFAULT))
        {
            video_options->width = 640;
            video_options->height = 480;
        }
    }
#endif

    debugPrint("Initializing the video");

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        return false;
    }

    // NO FRAME TIMING INITIALIZATION - Removed

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

    gSdlSurface = SDL_CreateRGBSurface(0,
        video_options->width,
        video_options->height,
        8,
        0,
        0,
        0,
        0);
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
    
#ifdef NXDK
    scr_blit = GNW95_ShowRect_Xbox;
    mouse_blit = GNW95_ShowRect_Xbox;
    
    // Initialize 32-bit palette cache
    update_palette_32bit();
#else
    scr_blit = GNW95_ShowRect;
    mouse_blit = GNW95_ShowRect;
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
    return rectGetWidth(&scr_size);
}

int screenGetHeight()
{
    return rectGetHeight(&scr_size);
}

static bool createRenderer(int width, int height)
{
#ifdef NXDK
    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, 
                                  SDL_RENDERER_ACCELERATED | 
                                  SDL_RENDERER_PRESENTVSYNC);
    
    if (!gSdlRenderer) {
        // Fallback to software
        gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, 
                                          SDL_RENDERER_SOFTWARE);
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

    // Set render scale quality to nearest (fastest for pixel art)
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    
#ifdef NXDK
    // CRITICAL: Use SDL_TEXTUREACCESS_STREAMING for Xbox unified memory
    // This tells SDL the texture will be updated frequently
    gSdlTexture = SDL_CreateTexture(gSdlRenderer, 
                                    SDL_PIXELFORMAT_ARGB8888, 
                                    SDL_TEXTUREACCESS_STREAMING, 
                                    width, height);
    
    // Try to lock texture immediately for direct access
    if (gSdlTexture) {
        texture_locked = (SDL_LockTexture(gSdlTexture, NULL, &texture_pixels, &texture_pitch) == 0);
        if (texture_locked) {
            debugPrint("Direct texture access enabled (pitch: %d)\n", texture_pitch);
        }
    }
#else
    gSdlTexture = SDL_CreateTexture(gSdlRenderer, 
                                    SDL_PIXELFORMAT_RGB888, 
                                    SDL_TEXTUREACCESS_STREAMING, 
                                    width, height);
#endif
    
    if (gSdlTexture == NULL) {
        return false;
    }

    Uint32 format;
    if (SDL_QueryTexture(gSdlTexture, &format, NULL, NULL, NULL) != 0) {
        return false;
    }

    gSdlTextureSurface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 
                                                        SDL_BITSPERPIXEL(format), format);
    if (gSdlTextureSurface == NULL) {
        return false;
    }

    return true;
}

static void destroyRenderer()
{
#ifdef NXDK
    // Unlock texture if locked
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
        // If palette changed, reconvert entire screen before presenting
        if (palette_changed && texture_pixels) {
            Uint32* dst_pixels = static_cast<Uint32*>(texture_pixels);
            const Uint8* src_pixels = static_cast<const Uint8*>(gSdlSurface->pixels);
            convert_8to32_simd(src_pixels, dst_pixels, 
                             gSdlSurface->w, gSdlSurface->h,
                             gSdlSurface->pitch, texture_pitch);
            palette_changed = false;
        }
        
        // Texture is already locked, just unlock it and let SDL know it changed
        SDL_UnlockTexture(gSdlTexture);
        texture_locked = false;
        
        // Re-lock for next frame
        texture_locked = (SDL_LockTexture(gSdlTexture, NULL, &texture_pixels, &texture_pitch) == 0);
    } else {
        // Normal SDL update path
        if (full_update || palette_changed) {
            SDL_UpdateTexture(gSdlTexture, NULL, gSdlTextureSurface->pixels, 
                             gSdlTextureSurface->pitch);
            full_update = false;
            palette_changed = false;
            dirty_rect.w = 0;
            dirty_rect.h = 0;
        } else if (dirty_rect.w > 0 && dirty_rect.h > 0) {
            SDL_UpdateTexture(gSdlTexture, &dirty_rect,
                             (Uint8*)gSdlTextureSurface->pixels + 
                             dirty_rect.y * gSdlTextureSurface->pitch +
                             dirty_rect.x * SDL_BYTESPERPIXEL(gSdlTextureSurface->format->format),
                             gSdlTextureSurface->pitch);
            dirty_rect.w = 0;
            dirty_rect.h = 0;
        }
    }
    
    SDL_RenderClear(gSdlRenderer);
    SDL_RenderCopy(gSdlRenderer, gSdlTexture, NULL, NULL);
    
    SDL_RenderPresent(gSdlRenderer);    
#else
    // Standard SDL rendering
    SDL_UpdateTexture(gSdlTexture, NULL, gSdlTextureSurface->pixels, 
                     gSdlTextureSurface->pitch);
    SDL_RenderClear(gSdlRenderer);
    SDL_RenderCopy(gSdlRenderer, gSdlTexture, NULL, NULL);
    SDL_RenderPresent(gSdlRenderer);
#endif
}

#ifdef NXDK
// Movie playback support - temporarily unlock texture so SDL_BlitSurface can work
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
    // Force full update for next render since movie bypasses normal blit path
    full_update = true;
}
#endif

} // namespace fallout