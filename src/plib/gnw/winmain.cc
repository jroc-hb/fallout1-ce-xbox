#include "plib/gnw/winmain.h"

#include <stdlib.h>

#include <SDL.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "game/main.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/svga.h"

#if __APPLE__ && TARGET_OS_IOS
#include "platform/ios/paths.h"
#endif

#ifdef NXDK
#include <nxdk/mount.h>
#include <nxdk/path.h>
#include <assert.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#endif

namespace fallout {

// 0x53A290
bool GNW95_isActive = false;

#if _WIN32
// 0x53A294
HANDLE GNW95_mutex = NULL;
#endif

// 0x6B0760
char GNW95_title[256];

int main(int argc, char* argv[])
{
    int rc;

#if _WIN32
    GNW95_mutex = CreateMutexA(0, TRUE, "GNW95MUTEX");
    if (GetLastError() != ERROR_SUCCESS) {
        return 0;
    }
#endif

#if __APPLE__ && TARGET_OS_IOS
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    chdir(iOSGetDocumentsPath());
#endif

#if __APPLE__ && TARGET_OS_OSX
    char* basePath = SDL_GetBasePath();
    chdir(basePath);
    SDL_free(basePath);
#endif

#if __ANDROID__
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    chdir(SDL_AndroidGetExternalStoragePath());
#endif

    SDL_ShowCursor(SDL_DISABLE);

    GNW95_isActive = true;
    rc = gnw_main(argc, argv);

#if _WIN32
    CloseHandle(GNW95_mutex);
#endif

    return rc;
}

} // namespace fallout

int main(int argc, char* argv[])
{
    #ifdef NXDK
    DbgPrint("\n\n\n##################### Starting Fallout #####################\n");
    // NXDK: CMake doesn't automount the D: drive, so we need to do it manually
    if (!nxIsDriveMounted('D')) {
        DbgPrint("Mounting D because it is not mounted\n");
        // D: doesn't exist yet, so we create it
        CHAR targetPath[MAX_PATH];
        nxGetCurrentXbeNtPath(targetPath);

        // Cut off the XBE file name by inserting a null-terminator
        char *filenameStr;
        filenameStr = strrchr(targetPath, '\\');
        assert(filenameStr != NULL);
        *(filenameStr + 1) = '\0';

        // Mount the obtained path as D:
        BOOL success;
        success = nxMountDrive('D', targetPath);
        DbgPrint("Mounted D: %s\n", success ? "success" : "failed");
        assert(success);
    }
    #endif

    return fallout::main(argc, argv);
}
