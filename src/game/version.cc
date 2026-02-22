#include "game/version.h"

#include <stdio.h>

namespace fallout {

// 0x4A10C0
char* getverstr(char* dest, size_t size)
{
#ifdef NXDK // v1.0
    snprintf(dest, size, "FALLOUT-CE XBOX %d.%d", VERSION_MAJOR, VERSION_MINOR);
#else
    snprintf(dest, size, "FALLOUT %d.%d", VERSION_MAJOR, VERSION_MINOR);
#endif
    return dest;
}

} // namespace fallout
