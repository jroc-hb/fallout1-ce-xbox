#include "game/amutex.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef NXDK
//debug logging
#include <xboxkrnl/xboxkrnl.h>
#endif

namespace fallout {

#ifdef _WIN32
// 0x540010
static HANDLE autorun_mutex;
#endif

// 0x413450
bool autorun_mutex_create()
{
#ifdef _WIN32
    // NXDK: Needs casting
#ifdef NXDK
    autorun_mutex = CreateMutexA((LPSECURITY_ATTRIBUTES)NULL, FALSE, "InterplayGenericAutorunMutex");
#else
    autorun_mutex = CreateMutexA(NULL, FALSE, "InterplayGenericAutorunMutex");
    #endif
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(autorun_mutex);
        return false;
    }
#endif

    return true;
}

// 0x413490
void autorun_mutex_destroy()
{
#ifdef _WIN32
    if (autorun_mutex != NULL) {
        CloseHandle(autorun_mutex);
    }
#endif
}

} // namespace fallout
