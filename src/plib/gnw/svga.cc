#include "plib/gnw/svga.h"

#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/winmain.h"

#ifdef NXDK
#include <hal/video.h>
#include <hal/debug.h>
#include <xboxkrnl/xboxkrnl.h>
#include <SDL_ttf.h>
#include "game/gconfig.h"
#endif

namespace fallout {

static bool createRenderer(int width, int height);
static void destroyRenderer();

#ifdef NXDK
// --- Debug overlay state ---
static int performance_overlay = 0;
static TTF_Font *debugFont = NULL;
static Uint32 lastTime = 0;
static int frameCount = 0;
static int currentFPS = 0;
static MM_STATISTICS mem_stats = {0};
static DWORD last_updated = 0;

// Xbox-specific optimization variables
static SDL_Rect dirty_rect = {0, 0, 0, 0};
static bool full_update = true;
static bool palette_changed = false;
static Uint8 current_palette[256 * 3] = {0}; // Track current palette for fades
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
// ============================================
// Xbox-specific optimizations
// ============================================

static void debug_update_fps(void) {
    frameCount++;
    Uint32 now = SDL_GetTicks();
    if (now - lastTime >= 1000) {
        currentFPS = frameCount;
        frameCount = 0;
        lastTime = now;
    }
}

static void update_mem_stats_periodically(void) {
    DWORD now = GetTickCount();
    if ((last_updated == 0) || ((now - last_updated) > 1000)) {
        last_updated = now;
        memset(&mem_stats, 0, sizeof(mem_stats));
        mem_stats.Length = sizeof(mem_stats);
        MmQueryStatistics(&mem_stats);
    }
}

// Optimized debug overlay - cache textures
static SDL_Texture* debug_overlay_texture = NULL;
static char last_debug_text[128] = {0};

static void debug_draw_overlay(SDL_Renderer *renderer) {
    if (!debugFont) return;

    update_mem_stats_periodically();

    char buf[128];
    snprintf(buf, sizeof(buf), "FPS: %d  RAM: %d/%d MB",
        currentFPS,
        mem_stats.AvailablePages >> 8,
        mem_stats.TotalPhysicalPages >> 8);

    // Only recreate texture if text changed
    if (strcmp(last_debug_text, buf) != 0 || !debug_overlay_texture) {
        strcpy(last_debug_text, buf);
        
        if (debug_overlay_texture) {
            SDL_DestroyTexture(debug_overlay_texture);
            debug_overlay_texture = NULL;
        }
        
        SDL_Color neonPink = {255, 20, 147, 255};
        SDL_Surface *textSurface = TTF_RenderText_Blended(debugFont, buf, neonPink);
        if (textSurface) {
            debug_overlay_texture = SDL_CreateTextureFromSurface(renderer, textSurface);
            SDL_FreeSurface(textSurface);
        }
    }

    if (debug_overlay_texture) {
        int w, h;
        SDL_QueryTexture(debug_overlay_texture, NULL, NULL, &w, &h);
        
        // Draw background
        SDL_Rect bgRect = {10 - 4, 10 - 2, w + 8, h + 4};
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderFillRect(renderer, &bgRect);
        
        // Draw text
        SDL_Rect dst = {10, 10, w, h};
        SDL_RenderCopy(renderer, debug_overlay_texture, NULL, &dst);
    }
}

// Call once at startup
void debug_overlay_init(void) {
    if (TTF_Init() == -1) {
        debugPrint("TTF_Init failed: %s\n", TTF_GetError());
        return;
    }
    debugFont = TTF_OpenFont("D:\\media\\font.ttf", 16);
    if (!debugFont) {
        debugPrint("Failed to load font: %s\n", TTF_GetError());
    }
}

// Optimized version for Xbox with proper dirty rectangle tracking
void GNW95_ShowRect_Xbox(unsigned char* src, unsigned int srcPitch,
                        unsigned int a3, unsigned int srcX, unsigned int srcY,
                        unsigned int srcWidth, unsigned int srcHeight,
                        unsigned int destX, unsigned int destY) {
    // Copy to the 8-bit surface
    buf_to_buf(src + srcPitch * srcY + srcX, srcWidth, srcHeight, srcPitch,
               (unsigned char*)gSdlSurface->pixels + gSdlSurface->pitch * destY + destX,
               gSdlSurface->pitch);
    
    // Update dirty rectangle - ALWAYS update when something is drawn
    // This ensures fades work properly
    if (!full_update) {
        if (dirty_rect.w == 0) {
            // First dirty rectangle
            dirty_rect.x = static_cast<int>(destX);
            dirty_rect.y = static_cast<int>(destY);
            dirty_rect.w = static_cast<int>(srcWidth);
            dirty_rect.h = static_cast<int>(srcHeight);
        } else {
            // Expand dirty rectangle to include new area
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
        
        // Clamp to screen bounds
        if (dirty_rect.x < 0) dirty_rect.x = 0;
        if (dirty_rect.y < 0) dirty_rect.y = 0;
        if (dirty_rect.x + dirty_rect.w > gSdlSurface->w) {
            dirty_rect.w = gSdlSurface->w - dirty_rect.x;
        }
        if (dirty_rect.y + dirty_rect.h > gSdlSurface->h) {
            dirty_rect.h = gSdlSurface->h - dirty_rect.y;
        }
    }
    
    // Also update the texture surface (needed for proper rendering)
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
        // Force full update when palette changes for fade effects
        full_update = true;
        palette_changed = true;
#endif
        
        // Update the entire texture surface when palette changes
        SDL_BlitSurface(gSdlSurface, NULL, gSdlTextureSurface, NULL);
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
        // Force full update when palette changes for fade effects
        full_update = true;
        palette_changed = true;
#endif
        
        // Update the entire texture surface when palette changes
        SDL_BlitSurface(gSdlSurface, NULL, gSdlTextureSurface, NULL);
    }
}

// Helper function for screen fades - applies brightness multiplier to palette
static void ApplyPaletteBrightness(unsigned char* palette, float brightness) {
    // brightness should be between 0.0 (black) and 1.0 (full)
    brightness = (brightness < 0.0f) ? 0.0f : (brightness > 1.0f) ? 1.0f : brightness;
    
    unsigned char faded_palette[256 * 3];
    
    for (int i = 0; i < 256; i++) {
        faded_palette[i * 3] = (unsigned char)(palette[i * 3] * brightness);
        faded_palette[i * 3 + 1] = (unsigned char)(palette[i * 3 + 1] * brightness);
        faded_palette[i * 3 + 2] = (unsigned char)(palette[i * 3 + 2] * brightness);
    }
    
    GNW95_SetPalette(faded_palette);
}

// 0x4CB850 - Main blit function
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

#ifdef NXDK
    config_get_value(&game_config, GAME_CONFIG_DEBUG_KEY, GAME_CONFIG_PERFORMANCE_OVERLAY_KEY, &performance_overlay);
    if (performance_overlay) {
        debug_overlay_init();
    }
#endif

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
#else
    scr_blit = GNW95_ShowRect;
    mouse_blit = GNW95_ShowRect;
#endif

    return true;
}

void svga_exit()
{
#ifdef NXDK
    if (debug_overlay_texture) {
        SDL_DestroyTexture(debug_overlay_texture);
        debug_overlay_texture = NULL;
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
    // On Xbox, try to create an accelerated renderer first
    for (int i = 0; i < 2; i++) {
        int renderer_flags = (i == 0) ? SDL_RENDERER_ACCELERATED : SDL_RENDERER_SOFTWARE;
        
        gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, renderer_flags);
        if (gSdlRenderer != NULL) {
            break;
        }
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
    // On Xbox, use streaming texture for better performance
    gSdlTexture = SDL_CreateTexture(gSdlRenderer, 
                                    SDL_PIXELFORMAT_ARGB8888, 
                                    SDL_TEXTUREACCESS_STREAMING, 
                                    width, height);
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
    // Xbox-optimized rendering with fade support
    debug_update_fps();
    
    if (full_update || palette_changed) {
        // Full update needed (first frame, palette change, or fade effect)
        SDL_UpdateTexture(gSdlTexture, NULL, gSdlTextureSurface->pixels, 
                         gSdlTextureSurface->pitch);
        full_update = false;
        palette_changed = false;
        dirty_rect.w = 0;
        dirty_rect.h = 0;
    } else if (dirty_rect.w > 0 && dirty_rect.h > 0) {
        // Partial update for normal gameplay
        SDL_UpdateTexture(gSdlTexture, &dirty_rect,
                         (Uint8*)gSdlTextureSurface->pixels + 
                         dirty_rect.y * gSdlTextureSurface->pitch +
                         dirty_rect.x * SDL_BYTESPERPIXEL(gSdlTextureSurface->format->format),
                         gSdlTextureSurface->pitch);
        dirty_rect.w = 0;
        dirty_rect.h = 0;
    }
    
    SDL_RenderClear(gSdlRenderer);
    SDL_RenderCopy(gSdlRenderer, gSdlTexture, NULL, NULL);
    
    if (performance_overlay) {
        debug_draw_overlay(gSdlRenderer);
    }
    
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

// ============================================
// Screen Fade Helper Functions
// ============================================

// Function to perform a screen fade (used by the game)
void screen_fade_to_black() {
    // Fade from current palette to black
    for (int step = 10; step >= 0; step--) {
        float brightness = step / 10.0f;
        ApplyPaletteBrightness(current_palette, brightness);
        renderPresent();
        SDL_Delay(30); // Adjust timing as needed
    }
}

void screen_fade_from_black() {
    // Fade from black to current palette
    for (int step = 0; step <= 10; step++) {
        float brightness = step / 10.0f;
        ApplyPaletteBrightness(current_palette, brightness);
        renderPresent();
        SDL_Delay(30); // Adjust timing as needed
    }
}

void screen_fade_to_color(unsigned char r, unsigned char g, unsigned char b) {
    // Store original palette
    unsigned char original_palette[256 * 3];
    memcpy(original_palette, current_palette, sizeof(original_palette));
    
    // Create target color palette
    unsigned char target_palette[256 * 3];
    for (int i = 0; i < 256; i++) {
        target_palette[i * 3] = r;
        target_palette[i * 3 + 1] = g;
        target_palette[i * 3 + 2] = b;
    }
    
    // Cross-fade between original and target
    for (int step = 0; step <= 10; step++) {
        float t = step / 10.0f;
        unsigned char faded_palette[256 * 3];
        
        for (int i = 0; i < 256; i++) {
            faded_palette[i * 3] = (unsigned char)(original_palette[i * 3] * (1.0f - t) + r * t);
            faded_palette[i * 3 + 1] = (unsigned char)(original_palette[i * 3 + 1] * (1.0f - t) + g * t);
            faded_palette[i * 3 + 2] = (unsigned char)(original_palette[i * 3 + 2] * (1.0f - t) + b * t);
        }
        
        GNW95_SetPalette(faded_palette);
        renderPresent();
        SDL_Delay(30);
    }
}

} // namespace fallout