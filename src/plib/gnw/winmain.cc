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
#include "game/loadsave.h"
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

#ifdef NXDK
// Helper functions
static void EnsureDir(const char* path) {
    BOOL ok = CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    DbgPrint("EnsureDir %s: %s\n", path, ok ? "success" : "failed");
    assert(ok);
}

static void EnsureFileCopy(const char* src, const char* dst) {
    if (GetFileAttributesA(dst) == INVALID_FILE_ATTRIBUTES) {
        DbgPrint("Copying %s -> %s\n", src, dst);
        BOOL ok = CopyFileA(src, dst, FALSE);
        DbgPrint("CopyFileA %s: %s\n", dst, ok ? "success" : "failed");
        assert(ok);
    } else {
        DbgPrint("EnsureFileCopy %s: %s\n", src, "exists");
    }
}
#endif

int main(int argc, char* argv[])
{
#ifdef NXDK
    DbgPrint("\n\n\n##################### Starting Fallout #####################\n");

    // NXDK: CMake doesn't automount the D: drive, so we need to do it manually
    if (!nxIsDriveMounted('D')) {
        CHAR targetPath[MAX_PATH];
        nxGetCurrentXbeNtPath(targetPath);
        char* slash = strrchr(targetPath, '\\');
        assert(slash);
        *(slash + 1) = '\0';
        BOOL ok = nxMountDrive('D', targetPath);
        DbgPrint("Mounted D: %s\n", ok ? "success" : "failed");
        assert(ok);
    }

    // Mount E:
    BOOL ok = nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    DbgPrint("Mounted E: %s\n", ok ? "success" : "failed");
    assert(ok);

    // Ensure save directories
    EnsureDir("E:\\UDATA");
    EnsureDir("E:\\UDATA\\FALLOUT1");
    EnsureDir("E:\\UDATA\\FALLOUT1\\data");
    EnsureDir("E:\\UDATA\\FALLOUT1\\data\\SAVEGAME");

    // Ensure config files
    EnsureFileCopy("D:\\fallout.cfg", "E:\\UDATA\\FALLOUT1\\fallout.cfg");
    EnsureFileCopy("D:\\f1_res.ini", "E:\\UDATA\\FALLOUT1\\f1_res.ini");

    // Register save dir metadata
    EnsureFileCopy("D:\\TitleImage.xbx", "E:\\UDATA\\FALLOUT1\\TitleImage.xbx");
    fallout::create_root_titlemeta();

    // Recursive directory copier
    auto CopyDirRecursive = [](const char* srcDir, const char* dstDir, auto& self) {
        WIN32_FIND_DATAA fd;
        char search[MAX_PATH]; snprintf(search, MAX_PATH, "%s\\*", srcDir);
        HANDLE hFind = FindFirstFileA(search, &fd);
        if (hFind == INVALID_HANDLE_VALUE) return;
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char src[MAX_PATH], dst[MAX_PATH];
            snprintf(src, MAX_PATH, "%s\\%s", srcDir, fd.cFileName);
            snprintf(dst, MAX_PATH, "%s\\%s", dstDir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                EnsureDir(dst);
                self(src, dst, self);
            } else {
                EnsureFileCopy(src, dst);
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    };

    // Ensure DATA/TEXT
    if (GetFileAttributesA("E:\\UDATA\\FALLOUT1\\DATA\\TEXT") == INVALID_FILE_ATTRIBUTES) {
        EnsureDir("E:\\UDATA\\FALLOUT1\\DATA");
        EnsureDir("E:\\UDATA\\FALLOUT1\\DATA\\TEXT");
        DbgPrint("Recursively copying D:\\DATA\\TEXT...\n");
        CopyDirRecursive("D:\\DATA\\TEXT", "E:\\UDATA\\FALLOUT1\\DATA\\TEXT", CopyDirRecursive);
    }

    // Dashboard save metadata
    EnsureFileCopy("D:\\DATA\\SaveImage.xbx", "E:\\UDATA\\FALLOUT1\\DATA\\SaveImage.xbx");
    EnsureFileCopy("D:\\DATA\\SaveMeta.xbx", "E:\\UDATA\\FALLOUT1\\DATA\\SaveMeta.xbx");

    // Sync save slots
    if (fallout::InitSyncXboxSaveSlots())
        DbgPrint("Failed to sync save slots.\n");
    else
        DbgPrint("Save slot sync complete.\n");
#endif

    return fallout::main(argc, argv);
}