#include "game/version.h"

#include <stdio.h>

namespace fallout {

// 0x4A10C0
char* getverstr(char* dest, size_t size)
{
#ifdef NXDK // v0.1.0-alpha
    snprintf(dest, size, "FALLOUT1-CE XBOX %d.%d", VERSION_MAJOR, VERSION_MINOR);
#else
    snprintf(dest, size, "FALLOUT %d.%d", VERSION_MAJOR, VERSION_MINOR);
#endif
    return dest;
}

} // namespace fallout
