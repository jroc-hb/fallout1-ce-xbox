#ifndef FALLOUT_PLIB_GNW_POINTER_ASSIST_H_
#define FALLOUT_PLIB_GNW_POINTER_ASSIST_H_

#include <cmath>

namespace fallout {

// Shared by the map (gmouse_pointer_assist) and UI (win_pointer_assist)
// pointer assists for analog-stick mouse movement.
//
// (dx, dy) is this frame's pointer movement in pixels, adjusted in place.
// `over`: the pointer is on a target, so slow it down by `friction` (easy
// to stop on). Otherwise, if the nearest target is within `radius` px
// (`distance` from the pointer to its bounds, center at targetX/Y) and the
// pointer is heading roughly toward it, bend the path toward that center by
// up to `steer` without changing speed. Moving away is never resisted.
inline void pointer_assist_apply(float* dx, float* dy, int mouseX, int mouseY, bool over, int distance, int targetX, int targetY, int radius, float friction, float steer)
{
    if (over) {
        *dx *= friction;
        *dy *= friction;
        return;
    }

    if (distance > radius) {
        return;
    }

    float speed = std::sqrt(*dx * *dx + *dy * *dy);
    float toX = (float)(targetX - mouseX);
    float toY = (float)(targetY - mouseY);
    float toLength = std::sqrt(toX * toX + toY * toY);
    if (speed <= 0.0f || toLength <= 0.0f) {
        return;
    }

    float dirX = *dx / speed;
    float dirY = *dy / speed;
    toX /= toLength;
    toY /= toLength;
    if (dirX * toX + dirY * toY < 0.25f) {
        return;
    }

    float strength = steer * (1.0f - (float)distance / (float)(radius + 1));
    float bentX = dirX + toX * strength;
    float bentY = dirY + toY * strength;
    float bentLength = std::sqrt(bentX * bentX + bentY * bentY);
    *dx = bentX / bentLength * speed;
    *dy = bentY / bentLength * speed;
}

// Distance in pixels from (x, y) to the rectangle (inclusive), 0 inside.
inline int pointer_assist_distance(int x, int y, int left, int top, int right, int bottom)
{
    int outX = left - x > x - right ? left - x : x - right;
    int outY = top - y > y - bottom ? top - y : y - bottom;
    if (outX < 0) outX = 0;
    if (outY < 0) outY = 0;
    return (int)std::sqrt((float)(outX * outX + outY * outY));
}

} // namespace fallout

#endif /* FALLOUT_PLIB_GNW_POINTER_ASSIST_H_ */
