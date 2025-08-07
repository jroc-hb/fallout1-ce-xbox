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

int main(int argc, char* argv[])
{
#ifdef NXDK
    DbgPrint("\n\n\n##################### Starting Fallout #####################\n");
    BOOL success;

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
        success = nxMountDrive('D', targetPath);
        DbgPrint("Mounted D: %s\n", success ? "success" : "failed");
        assert(success);
    }

    // NXDK: Mount the E: drive for writable data
    success = nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    DbgPrint("Mounted E: %s\n", success ? "success" : "failed");
    assert(success);

    BOOL dirSuccess;

    DbgPrint("Creating directory E:\\UDATA\n");
    dirSuccess = CreateDirectoryA("E:\\UDATA", NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    DbgPrint("CreateDirectoryA E:\\UDATA: %s\n", dirSuccess ? "success" : "failed");
    assert(dirSuccess);

    DbgPrint("Creating directory E:\\UDATA\\FALLOUT1\n");
    dirSuccess = CreateDirectoryA("E:\\UDATA\\FALLOUT1", NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    DbgPrint("CreateDirectoryA E:\\UDATA\\FALLOUT1: %s\n", dirSuccess ? "success" : "failed");
    assert(dirSuccess);

    DbgPrint("Creating directory E:\\UDATA\\FALLOUT1\\data\n");
    dirSuccess = CreateDirectoryA("E:\\UDATA\\FALLOUT1\\data", NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    DbgPrint("CreateDirectoryA E:\\UDATA\\FALLOUT1\\data: %s\n", dirSuccess ? "success" : "failed");
    assert(dirSuccess);

    DbgPrint("Creating directory E:\\UDATA\\FALLOUT1\\data\\SAVEGAME\n");
    dirSuccess = CreateDirectoryA("E:\\UDATA\\FALLOUT1\\data\\SAVEGAME", NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    DbgPrint("CreateDirectoryA E:\\UDATA\\FALLOUT1\\data\\SAVEGAME: %s\n", dirSuccess ? "success" : "failed");
    assert(dirSuccess);

    // Install fallout.cfg and f1_res.ini to HDD
    if (GetFileAttributesA("E:\\UDATA\\FALLOUT1\\fallout.cfg") == INVALID_FILE_ATTRIBUTES) {
        DbgPrint("Copying fallout.cfg to E:\\UDATA\\FALLOUT1\\fallout.cfg\n");
        BOOL copySuccess = CopyFileA("D:\\fallout.cfg", "E:\\UDATA\\FALLOUT1\\fallout.cfg", FALSE);
        DbgPrint("CopyFileA fallout.cfg: %s\n", copySuccess ? "success" : "failed");
        assert(copySuccess);
    }
    if (GetFileAttributesA("E:\\UDATA\\FALLOUT1\\f1_res.ini") == INVALID_FILE_ATTRIBUTES) {
        DbgPrint("Copying f1_res.ini to E:\\UDATA\\FALLOUT1\\f1_res.ini\n");
        BOOL copySuccess = CopyFileA("D:\\f1_res.ini", "E:\\UDATA\\FALLOUT1\\f1_res.ini", FALSE);
        DbgPrint("CopyFileA f1_res.ini: %s\n", copySuccess ? "success" : "failed");
        assert(copySuccess);
    }

    // Register save directory by installing TitleImage.xbx and TitleMeta.xbx to HDD
    if (GetFileAttributesA("E:\\UDATA\\FALLOUT1\\TitleImage.xbx") == INVALID_FILE_ATTRIBUTES) {
        DbgPrint("Copying TitleImage.xbx to E:\\UDATA\\FALLOUT1\\TitleImage.xbx\n");
        BOOL copySuccess = CopyFileA("D:\\TitleImage.xbx", "E:\\UDATA\\FALLOUT1\\TitleImage.xbx", FALSE);
        DbgPrint("CopyFileA TitleImage.xbx: %s\n", copySuccess ? "success" : "failed");
        assert(copySuccess);
    }
    fallout::create_root_titlemeta();

    // Install DATA/TEXT files to HDD (Should work for all languages)
    auto CopyDirectoryRecursive = [](const char* srcDir, const char* dstDir, auto& self) -> void {
        WIN32_FIND_DATAA findData;
        char searchPath[MAX_PATH];
        snprintf(searchPath, MAX_PATH, "%s\\*", srcDir);

        HANDLE hFind = FindFirstFileA(searchPath, &findData);
        if (hFind == INVALID_HANDLE_VALUE) {
            DbgPrint("FindFirstFileA failed for %s\n", srcDir);
            return;
        }

        do {
            if (strcmp(findData.cFileName, ".") == 0 || strcmp(findData.cFileName, "..") == 0) {
                continue;
            }

            char srcPath[MAX_PATH];
            char dstPath[MAX_PATH];
            snprintf(srcPath, MAX_PATH, "%s\\%s", srcDir, findData.cFileName);
            snprintf(dstPath, MAX_PATH, "%s\\%s", dstDir, findData.cFileName);

            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                DbgPrint("Creating directory: %s\n", dstPath);
                BOOL dirSuccess = CreateDirectoryA(dstPath, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
                DbgPrint("CreateDirectoryA %s: %s\n", dstPath, dirSuccess ? "success" : "failed");
                if (dirSuccess) {
                    self(srcPath, dstPath, self);
                }
            } else {
                DbgPrint("Copying file: %s -> %s\n", srcPath, dstPath);
                BOOL copySuccess = CopyFileA(srcPath, dstPath, FALSE);
                DbgPrint("CopyFileA %s: %s\n", dstPath, copySuccess ? "success" : "failed");
            }
        } while (FindNextFileA(hFind, &findData));
        FindClose(hFind);
    };

    // Check if E:\UDATA\FALLOUT1\DATA\TEXT exists
    if (GetFileAttributesA("E:\\UDATA\\FALLOUT1\\DATA\\TEXT") == INVALID_FILE_ATTRIBUTES) {
        DbgPrint("E:\\UDATA\\FALLOUT1\\DATA\\TEXT does not exist, creating...\n");

        // Create E:\UDATA\FALLOUT1\DATA if needed
        if (GetFileAttributesA("E:\\UDATA\\FALLOUT1\\DATA") == INVALID_FILE_ATTRIBUTES) {
            DbgPrint("Creating directory E:\\UDATA\\FALLOUT1\\DATA\n");
            BOOL dirSuccess = CreateDirectoryA("E:\\UDATA\\FALLOUT1\\DATA", NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
            DbgPrint("CreateDirectoryA E:\\UDATA\\FALLOUT1\\DATA: %s\n", dirSuccess ? "success" : "failed");
            assert(dirSuccess);
        }

        // Create E:\UDATA\FALLOUT1\DATA\TEXT
        DbgPrint("Creating directory E:\\UDATA\\FALLOUT1\\DATA\\TEXT\n");
        BOOL dirSuccess = CreateDirectoryA("E:\\UDATA\\FALLOUT1\\DATA\\TEXT", NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
        DbgPrint("CreateDirectoryA E:\\UDATA\\FALLOUT1\\DATA\\TEXT: %s\n", dirSuccess ? "success" : "failed");
        assert(dirSuccess);

        // Recursively copy D:\DATA\TEXT to E:\UDATA\FALLOUT1\DATA\TEXT
        DbgPrint("Recursively copying D:\\DATA\\TEXT to E:\\UDATA\\FALLOUT1\\DATA\\TEXT\n");
        CopyDirectoryRecursive("D:\\DATA\\TEXT", "E:\\UDATA\\FALLOUT1\\DATA\\TEXT", CopyDirectoryRecursive);
        DbgPrint("Finished copying D:\\DATA\\TEXT\n");
    } else {
        DbgPrint("E:\\UDATA\\FALLOUT1\\DATA\\TEXT already exists, skipping copy.\n");
    }

    // Install DATA folder "save" metadata for the Xbox dashboard
    if (GetFileAttributesA("E:\\UDATA\\FALLOUT1\\DATA\\SaveImage.xbx") == INVALID_FILE_ATTRIBUTES) {
        BOOL copySuccess = CopyFileA("D:\\DATA\\SaveImage.xbx", "E:\\UDATA\\FALLOUT1\\DATA\\SaveImage.xbx", FALSE);
        assert(copySuccess);
    }
    if (GetFileAttributesA("E:\\UDATA\\FALLOUT1\\DATA\\SaveMeta.xbx") == INVALID_FILE_ATTRIBUTES) {
        BOOL copySuccess = CopyFileA("D:\\DATA\\SaveMeta.xbx", "E:\\UDATA\\FALLOUT1\\DATA\\SaveMeta.xbx", FALSE);
        assert(copySuccess);
    }

    // Sync save slot data from the main Fallout UDATA directory to the DATA/SAVEGAME directory
    if (fallout::InitSyncXboxSaveSlots()) {
        DbgPrint("Failed to sync save slots.\n");
    } else {
        DbgPrint("Save slot sync complete.\n");
    }
#endif

    return fallout::main(argc, argv);
}
