#ifndef FALLOUT_PLIB_GNW_SVGA_TYPES_H_
#define FALLOUT_PLIB_GNW_SVGA_TYPES_H_

namespace fallout {

// NOTE: These typedefs always appear in this order in every implementation file
// with extended debug info. However `mouse.c` does not have DirectX types
// implying it does not include `svga.h` which does so to expose primary
// DirectDraw objects.

typedef void(UpdatePaletteFunc)();
typedef void(ResetModeFunc)();
typedef int(SetModeFunc)();
typedef void(ScreenTransBlitFunc)(unsigned char* srcBuf, unsigned int srcW, unsigned int srcH, unsigned int subX, unsigned int subY, unsigned int subW, unsigned int subH, unsigned int dstX, unsigned int dstY, unsigned char trans);
typedef void(ScreenBlitFunc)(unsigned char* srcBuf, unsigned int srcW, unsigned int srcH, unsigned int subX, unsigned int subY, unsigned int subW, unsigned int subH, unsigned int dstX, unsigned int dstY);

typedef struct VideoOptions {
    int width;
    int height;
    bool fullscreen;
    int scale;
#ifdef NXDK
    // XBOX OPTS display settings (f1_res.ini [XBOX]). width/height above are
    // the video mode; svga_init replaces them with the size the game draws at.
    int uiScale; // 0: 1x, 1: 1.33x, 2: 1.5x (720p only)
    int safeArea; // black border on each edge, percent of the screen (0-8)
#endif
} VideoOptions;

} // namespace fallout

#endif /* FALLOUT_PLIB_GNW_SVGA_TYPES_H_ */
