#include "fps_limiter.h"

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
    Uint64 elapsed = SDL_GetTicks() - _ticks;
    if (1000 / _fps > elapsed) {
        SDL_Delay(static_cast<Uint32>(1000 / _fps - elapsed));
    }
}

} // namespace fallout
