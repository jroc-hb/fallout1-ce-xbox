#include "fps_limiter.h"
#include "plib/gnw/bcdebug.h" // TEMP-HANG-DEBUG

#include <SDL3/SDL.h>

namespace fallout {

FpsLimiter::FpsLimiter(unsigned int fps)
    : _fps(fps)
    , _ticks(0)
{
}

void FpsLimiter::mark()
{
    _ticks = SDL_GetTicks();
}

void FpsLimiter::throttle() const
{
#ifdef NXDK
    // On Xbox, SDL_RenderPresent already blocks until vblank, and every loop
    // that throttles also presents. Sleeping here as well can push the next
    // present past a vblank and drop the frame rate to 30fps.
    return;
#endif

    Uint64 elapsed = SDL_GetTicks() - _ticks;
    if (1000 / _fps > elapsed) {
        BC_MAIN("fps:delay");
        SDL_Delay(static_cast<Uint32>(1000 / _fps - elapsed));
        BC_MAIN("fps:done");
    }
}

} // namespace fallout
