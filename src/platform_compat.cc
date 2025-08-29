#include "platform_compat.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
#include <direct.h>
#ifndef NXDK
#include <io.h>
#include <fcntl.h>
#endif

#ifndef O_WRONLY
#define O_WRONLY 0x01 // Define O_WRONLY if not defined
#endif
#include <stdio.h>
#include <stdlib.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#ifndef NXDK
#include <timeapi.h>
#endif
#else
#include <chrono>
#endif

#ifdef NXDK
#include "plib/gnw/debug.h"
#endif

#include <SDL.h>

namespace fallout {

int compat_stricmp(const char* string1, const char* string2)
{
#ifdef NXDK
    // NXDK TODO: Investigate further.... is the array busted?
    // NXDK SDL_strcasecmp uppercases before comparing, breaking compatibility with Fallout's sort order 
    char a = 0;
    char b = 0;
    while (*string1 && *string2) {
        a = SDL_tolower((unsigned char) *string1);
        b = SDL_tolower((unsigned char) *string2);
        if (a != b)
            break;
        ++string1;
        ++string2;
    }
    a = SDL_tolower(*string1);
    b = SDL_tolower(*string2);
    return (int) ((unsigned char) a - (unsigned char) b);
#else
    return SDL_strcasecmp(string1, string2);
#endif
}

int compat_strnicmp(const char* string1, const char* string2, size_t size)
{
#ifdef NXDK
    // NXDK TODO: Investigate further.... is the array busted?
    char a = 0;
    char b = 0;
    while (*string1 && *string2 && size) {
        a = SDL_tolower((unsigned char) *string1);
        b = SDL_tolower((unsigned char) *string2);
        if (a != b)
            break;
        ++string1;
        ++string2;
        --size;
    }
    if (size == 0) {
        return 0;
    } else {
        a = SDL_tolower((unsigned char) *string1);
        b = SDL_tolower((unsigned char) *string2);
        return (int) ((unsigned char) a - (unsigned char) b);
    }
#else
    return SDL_strncasecmp(string1, string2, size);
#endif
}

char* compat_strupr(char* string)
{
    return SDL_strupr(string);
}

char* compat_strlwr(char* string)
{
    return SDL_strlwr(string);
}

char* compat_itoa(int value, char* buffer, int radix)
{
    return SDL_itoa(value, buffer, radix);
}

void compat_splitpath(const char* path, char* drive, char* dir, char* fname, char* ext)
{
    const char* driveStart = path;
    if (path[0] == '/' && path[1] == '/') {
        path += 2;
        while (*path != '\0' && *path != '/' && *path != '.') {
            path++;
        }
    }

    if (drive != NULL) {
        size_t driveSize = path - driveStart;
        if (driveSize > COMPAT_MAX_DRIVE - 1) {
            driveSize = COMPAT_MAX_DRIVE - 1;
        }
        strncpy(drive, path, driveSize);
        drive[driveSize] = '\0';
    }

    const char* dirStart = path;
    const char* fnameStart = path;
    const char* extStart = NULL;

    const char* end = path;
    while (*end != '\0') {
        if (*end == '/') {
            fnameStart = end + 1;
        } else if (*end == '.') {
            extStart = end;
        }
        end++;
    }

    if (extStart == NULL) {
        extStart = end;
    }

    if (dir != NULL) {
        size_t dirSize = fnameStart - dirStart;
        if (dirSize > COMPAT_MAX_DIR - 1) {
            dirSize = COMPAT_MAX_DIR - 1;
        }
        strncpy(dir, path, dirSize);
        dir[dirSize] = '\0';
    }

    if (fname != NULL) {
        size_t fileNameSize = extStart - fnameStart;
        if (fileNameSize > COMPAT_MAX_FNAME - 1) {
            fileNameSize = COMPAT_MAX_FNAME - 1;
        }
        strncpy(fname, fnameStart, fileNameSize);
        fname[fileNameSize] = '\0';
    }

    if (ext != NULL) {
        size_t extSize = end - extStart;
        if (extSize > COMPAT_MAX_EXT - 1) {
            extSize = COMPAT_MAX_EXT - 1;
        }
        strncpy(ext, extStart, extSize);
        ext[extSize] = '\0';
    }
}

void compat_makepath(char* path, const char* drive, const char* dir, const char* fname, const char* ext)
{
    path[0] = '\0';

    if (drive != NULL) {
        if (*drive != '\0') {
            strcpy(path, drive);
            path = strchr(path, '\0');

            if (path[-1] == '/') {
                path--;
            } else {
                *path = '/';
            }
        }
    }

    if (dir != NULL) {
        if (*dir != '\0') {
            if (*dir != '/' && *path == '/') {
                path++;
            }

            strcpy(path, dir);
            path = strchr(path, '\0');

            if (path[-1] == '/') {
                path--;
            } else {
                *path = '/';
            }
        }
    }

    if (fname != NULL && *fname != '\0') {
        if (*fname != '/' && *path == '/') {
            path++;
        }

        strcpy(path, fname);
        path = strchr(path, '\0');
    } else {
        if (*path == '/') {
            path++;
        }
    }

    if (ext != NULL) {
        if (*ext != '\0') {
            if (*ext != '.') {
                *path++ = '.';
            }

            strcpy(path, ext);
            path = strchr(path, '\0');
        }
    }

    *path = '\0';
}

int compat_open(const char* filePath, int flags)
{
#ifdef NXDK
    const char* mode = (flags & O_WRONLY) ? "wb" : "rb";
    char nativePath[COMPAT_MAX_PATH] = {0};
    strcat(nativePath, filePath);
    compat_windows_path_to_native(nativePath);
    FILE* fp = fopen(nativePath, mode);
    return fp ? (int)(intptr_t)fp : -1;
#else
    const char* mode = (flags & O_WRONLY) ? "wb" : "rb";
    FILE* fp = fopen(filePath, mode);
    return fp ? reinterpret_cast<intptr_t>(fp) : -1;
#endif
}

int compat_close(int fileHandle)
{
    FILE* fp = reinterpret_cast<FILE*>(fileHandle);
    int result = fclose(fp);
    return result == 0 ? 0 : -1;
}

int compat_read(int fileHandle, void* buf, unsigned int size)
{
    FILE* fp = reinterpret_cast<FILE*>(fileHandle);
    int bytesRead = fread(buf, 1, size, fp);
    debug_printf("compat_read: handle=%d size=%u -> read=%d\n", fileHandle, size, bytesRead);
    return bytesRead;
}

int compat_write(int fileHandle, const void* buf, unsigned int size)
{
    FILE* fp = reinterpret_cast<FILE*>(fileHandle);
    int bytesWritten = fwrite(buf, 1, size, fp);
    debug_printf("compat_write: handle=%d size=%u -> written=%d\n", fileHandle, size, bytesWritten);
    return bytesWritten;
}

long compat_lseek(int fileHandle, long offset, int origin)
{
    FILE* fp = reinterpret_cast<FILE*>(fileHandle);
    long result = (fseek(fp, offset, origin) == 0) ? ftell(fp) : -1;
    debug_printf("compat_lseek: handle=%d offset=%ld origin=%d -> position=%ld\n",
                 fileHandle, offset, origin, result);
    return result;
}

long compat_tell(int fileHandle)
{
    FILE* fp = reinterpret_cast<FILE*>(fileHandle);
    long pos = ftell(fp);
    debug_printf("compat_tell: handle=%d -> position=%ld\n", fileHandle, pos);
    return pos;
}

long compat_filelength(int fileHandle)
{
    debug_printf("compat_filelength: %i\n", fileHandle);
    FILE* fp = reinterpret_cast<FILE*>(fileHandle);
    long originalOffset = ftell(fp);
    fseek(fp, 0, SEEK_END);
    long filesize = ftell(fp);
    fseek(fp, originalOffset, SEEK_SET);
    return filesize;
}

int compat_mkdir(const char* path)
{
    char nativePath[COMPAT_MAX_PATH];
    strcpy(nativePath, path);
    compat_windows_path_to_native(nativePath);
    compat_resolve_path(nativePath);

#ifdef _WIN32
    #ifdef NXDK
        int result = 0;
        result = CreateDirectoryA(nativePath, NULL);
        debug_printf("compat_mkdir: path=%s -> result=%d\n", nativePath, result);
        return result;
    #else
    return mkdir(nativePath);
    #endif
#else
    return mkdir(nativePath, 0755);
#endif
}

unsigned int compat_timeGetTime()
{
#ifdef NXDK
    static DWORD start = GetTickCount();
    DWORD now = GetTickCount();
    debug_printf("compat_timeGetTime %i\n", now - start);
    return now - start;
#elif defined(_WIN32)
    return timeGetTime();
#else
    static auto start = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return static_cast<unsigned int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
#endif
}

FILE* compat_fopen(const char* path, const char* mode)
{
    char nativePath[COMPAT_MAX_PATH];
#ifdef NXDK
    strcpy(nativePath, path);
    compat_windows_path_to_native(nativePath);

    FILE* fp = NULL;
    if (strcmp(mode, "rt") == 0) {
        // NXDK seemingly doesn't support "rt" mode, so we use "rb" instead
        fp = fopen(nativePath, "rb");
    } else if (strcmp(mode, "wt") == 0) {
        // NXDK seemingly doesn't support "wt" mode, so we use "wb" instead
        fp = fopen(nativePath, "wb");
    } else {
        fp = fopen(nativePath, mode);
    }

    if (!fp) {
        debug_printf("\nALERT! fopen failed for path: %s (mode: %s)\n", nativePath, mode);
    }

    return fp;
#else
    strcpy(nativePath, path);
    compat_windows_path_to_native(nativePath);
    compat_resolve_path(nativePath);
    return fopen(nativePath, mode);
#endif
}

int compat_remove(const char* path)
{
    debug_printf("compat_remove: %s\n", path);
    char nativePath[COMPAT_MAX_PATH];
    strcpy(nativePath, path);
    compat_windows_path_to_native(nativePath);
    compat_resolve_path(nativePath);
    return remove(nativePath);
}

int compat_rename(const char* oldFileName, const char* newFileName)
{
    debug_printf("compat_rename: %s to %s\n", oldFileName, newFileName);
    char nativeOldFileName[COMPAT_MAX_PATH];
    char nativeNewFileName[COMPAT_MAX_PATH];

    strcpy(nativeOldFileName, oldFileName);
    strcpy(nativeNewFileName, newFileName);
    compat_windows_path_to_native(nativeOldFileName);
    compat_resolve_path(nativeOldFileName);
    compat_windows_path_to_native(nativeNewFileName);
    compat_resolve_path(nativeNewFileName);

    return rename(nativeOldFileName, nativeNewFileName);
}

void compat_windows_path_to_native(char* path)
{
#ifndef _WIN32
    char* pch = path;
    while (*pch != '\0') {
        if (*pch == '\\') {
            *pch = '/';
        }
        pch++;
    }
#endif
}

void compat_resolve_path(char* path)
{
#ifndef _WIN32
    char* pch = path;

    DIR* dir;
    if (pch[0] == '/') {
        dir = opendir("/");
        pch++;
    } else {
        dir = opendir(".");
    }

    while (dir != NULL) {
        char* sep = strchr(pch, '/');
        size_t length;
        if (sep != NULL) {
            length = sep - pch;
        } else {
            length = strlen(pch);
        }

        bool found = false;

        struct dirent* entry = readdir(dir);
        while (entry != NULL) {
            if (strlen(entry->d_name) == length && compat_strnicmp(pch, entry->d_name, length) == 0) {
                strncpy(pch, entry->d_name, length);
                found = true;
                break;
            }
            entry = readdir(dir);
        }

        closedir(dir);
        dir = NULL;

        if (!found) {
            break;
        }

        if (sep == NULL) {
            break;
        }

        *sep = '\0';
        dir = opendir(path);
        *sep = '/';

        pch = sep + 1;
    }
#endif
}

char* compat_strdup(const char* string)
{
    debug_printf("compat_strdup: %s\n", string);
    return SDL_strdup(string);
}

// It's a replacement for compat_filelength(fileno(stream)) on platforms without
// fileno defined.
long getFileSize(FILE* stream)
{
    long originalOffset = ftell(stream);
    fseek(stream, 0, SEEK_END);
    long filesize = ftell(stream);
    fseek(stream, originalOffset, SEEK_SET);
    return filesize;
}

#ifdef NXDK
bool compat_delete_directory_recursive(const char *path)
{
    char searchPath[MAX_PATH];
    snprintf(searchPath, sizeof(searchPath), "%s\\*", path);

    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA(searchPath, &ffd);

    if (hFind == INVALID_HANDLE_VALUE) {
        return false;
    }

    do {
        if (strcmp(ffd.cFileName, ".") == 0 || strcmp(ffd.cFileName, "..") == 0)
            continue;

        char fullPath[MAX_PATH];
        snprintf(fullPath, sizeof(fullPath), "%s\\%s", path, ffd.cFileName);

        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Recurse into subdirectory
            compat_delete_directory_recursive(fullPath);
            RemoveDirectoryA(fullPath);
        } else {
            DeleteFileA(fullPath);
        }

    } while (FindNextFileA(hFind, &ffd) != 0);

    FindClose(hFind);

    // Remove the now-empty root directory
    return RemoveDirectoryA(path) != 0;
}

bool compat_copy_directory_recursive(const char *srcDir, const char *dstDir)
{
    // Create the destination directory if it doesn't exist
    CreateDirectoryA(dstDir, NULL);

    char searchPath[MAX_PATH];
    snprintf(searchPath, sizeof(searchPath), "%s\\*", srcDir);

    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA(searchPath, &ffd);

    if (hFind == INVALID_HANDLE_VALUE) {
        return false;
    }

    do {
        if (strcmp(ffd.cFileName, ".") == 0 || strcmp(ffd.cFileName, "..") == 0)
            continue;

        char srcPath[MAX_PATH], dstPath[MAX_PATH];
        snprintf(srcPath, sizeof(srcPath), "%s\\%s", srcDir, ffd.cFileName);
        snprintf(dstPath, sizeof(dstPath), "%s\\%s", dstDir, ffd.cFileName);

        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Recursively copy subdirectory
            if (!compat_copy_directory_recursive(srcPath, dstPath)) {
                FindClose(hFind);
                return false;
            }
        } else {
            // Copy file
            if (!CopyFileA(srcPath, dstPath, FALSE)) {
               debug_printf("Failed to copy file: %s -> %s\n", srcPath, dstPath);
            }
        }

    } while (FindNextFileA(hFind, &ffd) != 0);

    FindClose(hFind);
    return true;
}
#endif

} // namespace fallout
