#ifndef FALLOUT_FPS_LIMITER_H_
#define FALLOUT_FPS_LIMITER_H_

#include <SDL3/SDL.h>

namespace fallout {

class FpsLimiter {
public:
    FpsLimiter(unsigned int fps = 60);
    void mark();
    void throttle() const;

private:
    const unsigned int _fps;
    Uint64 _ticks;
};

} // namespace fallout

#endif /* FALLOUT_FPS_LIMITER_H_ */
