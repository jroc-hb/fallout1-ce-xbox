#include "game/stickwalk.h"

#include <cmath>

#include "game/actions.h"
#include "game/anim.h"
#include "game/combat.h"
#include "game/critter.h"
#include "game/gmouse.h"
#include "game/map.h"
#include "game/map_defs.h"
#include "game/object.h"
#include "game/protinst.h"
#include "game/tile.h"
#include "plib/gnw/gamepad.hpp"
#include "plib/gnw/gnw.h"
#include "plib/gnw/input.h"
#include "plib/gnw/svga.h"

namespace fallout {

// How it works: the stick never plans a path. Each time the player reaches a
// hex, anim.cc's object_move asks sw_steer for the next one (see
// anim_set_dude_steer) and walks on to it without stopping. Every step is a
// path of one hex, so when the stick is let go (or the next hex is blocked)
// the walk just ends at the hex being entered. Mid-step, sw_turn can turn the
// step around when the stick is pulled the other way.

// Look-ahead along the walked line, in screen pixels. Hexes have no straight
// up/down neighbour: aiming at a point this far ahead on a line through where
// the walk started makes a walk straight up alternate NE and NW steps
// instead of drifting off along one of them.
static constexpr float kLookAhead = 64.0f;
// Start a new line when the stick turns more than ~20 degrees.
static constexpr float kNewLineCos = 0.94f;
// Steps must head at least a little the stick's way (cosine of the angle
// between them), so a blocked walk slides along a slanted wall but never
// steps sideways or back.
static constexpr float kMinStepCos = 0.05f;
// Turn a step around mid-way when the stick points within ~60 degrees of
// straight back and clearly against the step.
static constexpr float kReverseCos = 0.5f;
static constexpr float kAgainstCos = -0.3f;
// Walk into a closed door within ~55 degrees of the stick to open it.
static constexpr float kDoorCos = 0.55f;
// Deflection from which the player runs instead of walking.
static constexpr float kRunDeflection = 0.85f;
// The walk stops if no update has come in this long (the option was turned
// off, or a loop stopped reading the stick).
static constexpr unsigned int kUpdateTimeoutMs = 200;
// Camera follow: fraction of the remaining distance closed per second.
static constexpr float kFollowRate = 8.0f;
// The follow camera keeps the player this far below the middle of the map
// view (screen pixels): one hex straight down (a SE + SW step is 24 px).
static constexpr int kFollowDropY = 24;

// A stick walk is in progress (or will start once the player is free).
static bool sw_walking = false;
static unsigned int sw_last_update = 0;
// Stick direction in screen space (y down), unit length.
static float sw_dir_x = 0.0f;
static float sw_dir_y = 0.0f;
static bool sw_run = false;
// The line being walked: through this tile, along this unit direction.
static int sw_origin_tile = -1;
static float sw_line_x = 0.0f;
static float sw_line_y = 0.0f;

// The camera is catching up with the player.
static bool sw_follow = false;
static float sw_follow_rem_x = 0.0f;
static float sw_follow_rem_y = 0.0f;

static bool sw_installed = false;

// A or RT, tapped or held, waiting for the game loop (stick_walk_run_pending).
static bool sw_use_pending = false;
static bool sw_menu_pending = false;
static bool sw_look_pending = false;
// The player is carrying out what A started (walking over, picking up):
// no highlight until that's done.
static bool sw_acting = false;
// The action menu is open (its loop reads input, which calls us again).
static bool sw_in_menu = false;
// The door this stick push already tried to open (compared, never used).
static Object* sw_door_tried = NULL;

// Screen-space unit vector of a hex step in `rotation`.
static void sw_step_dir(int rotation, float* x, float* y)
{
    const float dx = (float)off_tile[0][rotation];
    const float dy = (float)off_tile[1][rotation];
    const float length = std::sqrt(dx * dx + dy * dy);
    *x = dx / length;
    *y = dy / length;
}

// How well a hex step in `rotation` follows the stick (cosine).
static float sw_step_cos(int rotation)
{
    float x;
    float y;
    sw_step_dir(rotation, &x, &y);
    return x * sw_dir_x + y * sw_dir_y;
}

// The neighbour of the player's hex to step to next, or -1: of the free
// neighbours heading roughly the stick's way, the one nearest a point a
// little ahead on the walked line.
static int sw_choose_step()
{
    const int from = obj_dude->tile;
    const int elevation = obj_dude->elevation;

    int fromX;
    int fromY;
    int originX;
    int originY;
    tile_coord(from, &fromX, &fromY, elevation);
    tile_coord(sw_origin_tile, &originX, &originY, elevation);

    float along = (float)(fromX - originX) * sw_line_x + (float)(fromY - originY) * sw_line_y;
    if (along < 0.0f) {
        along = 0.0f;
    }
    const float targetX = (float)originX + sw_line_x * (along + kLookAhead);
    const float targetY = (float)originY + sw_line_y * (along + kLookAhead);

    int best = -1;
    float bestDistance = 0.0f;
    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        const int tile = tile_num_in_direction(from, rotation, 1);
        if (!hexGridTileIsValid(tile) || tile == from) {
            continue;
        }

        if (sw_step_cos(rotation) < kMinStepCos) {
            continue;
        }

        if (obj_blocking_at(obj_dude, tile, elevation) != NULL) {
            continue;
        }

        int x;
        int y;
        tile_coord(tile, &x, &y, elevation);
        const float dx = (float)x - targetX;
        const float dy = (float)y - targetY;
        const float distance = dx * dx + dy * dy;
        if (best == -1 || distance < bestDistance) {
            best = rotation;
            bestDistance = distance;
        }
    }

    return best;
}

// The closed door the stick is pushing the player into, if any.
static Object* sw_door_ahead()
{
    Object* best = NULL;
    float bestCos = kDoorCos;
    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        const float cosine = sw_step_cos(rotation);
        if (cosine < bestCos) {
            continue;
        }

        const int tile = tile_num_in_direction(obj_dude->tile, rotation, 1);
        if (!hexGridTileIsValid(tile)) {
            continue;
        }

        Object* blocker = obj_blocking_at(obj_dude, tile, obj_dude->elevation);
        if (blocker != NULL
            && FID_TYPE(blocker->fid) == OBJ_TYPE_SCENERY
            && obj_is_a_portal(blocker)
            && !obj_is_open(blocker)) {
            best = blocker;
            bestCos = cosine;
        }
    }
    return best;
}

static bool sw_fresh()
{
    if (elapsed_time(sw_last_update) > kUpdateTimeoutMs) {
        sw_walking = false;
    }
    return sw_walking;
}

// anim.cc's hook: the player reached a hex mid-walk.
static int sw_steer(Object* obj, int* anim)
{
    if (obj != obj_dude || !sw_fresh()) {
        return -1;
    }

    const int rotation = sw_choose_step();
    if (rotation == -1) {
        return -1;
    }

    *anim = sw_run ? ANIM_RUNNING : ANIM_WALK;
    return rotation;
}

// anim.cc's hook, every frame of a step in `rotation`: turn around now if
// the stick is pulled back the way the player came.
static int sw_turn(Object* obj, int rotation)
{
    if (obj != obj_dude || !sw_fresh()) {
        return -1;
    }

    const int reverse = (rotation + ROTATION_COUNT / 2) % ROTATION_COUNT;
    if (sw_step_cos(reverse) < kReverseCos || sw_step_cos(rotation) > kAgainstCos) {
        return -1;
    }

    // Past halfway the hex boundary (where it can turn anyway) is closer
    // than walking all the way back.
    const float stepX = (float)off_tile[0][rotation];
    const float stepY = (float)off_tile[1][rotation];
    const float progress = ((float)obj->x * stepX + (float)obj->y * stepY) / (stepX * stepX + stepY * stepY);
    if (progress > 0.5f) {
        return -1;
    }

    // The turned step ends on the hex behind the one it started from.
    const int tile = tile_num_in_direction(obj->tile, reverse, 1);
    if (!hexGridTileIsValid(tile) || obj_blocking_at(obj, tile, obj->elevation) != NULL) {
        return -1;
    }

    return reverse;
}

// Starts a one-hex walk the stick's way; sw_steer extends it.
static bool sw_start()
{
    const int rotation = sw_choose_step();
    if (rotation == -1) {
        return false;
    }

    const int tile = tile_num_in_direction(obj_dude->tile, rotation, 1);
    register_begin(ANIMATION_REQUEST_RESERVED);
    if (sw_run) {
        // Walks instead while sneaking or with a crippled leg.
        register_object_run_to_tile(obj_dude, tile, obj_dude->elevation, -1, 0);
    } else {
        register_object_move_to_tile(obj_dude, tile, obj_dude->elevation, -1, 0);
    }
    return register_end() == 0;
}

// Scrolls the map toward the player, a little behind like a follow camera.
static void sw_follow_camera(float dt)
{
    int x;
    int y;
    if (tile_coord(obj_dude->tile, &x, &y, obj_dude->elevation) != 0) {
        sw_follow = false;
        return;
    }
    x += 16 + obj_dude->x;
    y += 8 + obj_dude->y;

    // Just below the middle of the map view (above the interface bar), as
    // map_center_mouse plus kFollowDropY.
    const int centerX = (scr_size.lrx - scr_size.ulx + 1) / 2;
    const int centerY = (scr_size.lry - scr_size.uly - 99) / 2 + kFollowDropY;
    const float errorX = (float)(x - centerX);
    const float errorY = (float)(y - centerY);

    if (!sw_walking && std::fabs(errorX) < 2.0f && std::fabs(errorY) < 2.0f) {
        sw_follow = false;
        sw_follow_rem_x = 0.0f;
        sw_follow_rem_y = 0.0f;
        return;
    }

    const float k = std::fmin(1.0f, dt * kFollowRate);
    sw_follow_rem_x += errorX * k;
    sw_follow_rem_y += errorY * k;
    const int dx = (int)sw_follow_rem_x;
    const int dy = (int)sw_follow_rem_y;
    if (dx == 0 && dy == 0) {
        return;
    }
    sw_follow_rem_x -= (float)dx;
    sw_follow_rem_y -= (float)dy;

    // Against the edge of the map: nothing left to catch up.
    if (map_scroll_pixels(dx, dy) == -1 && !sw_walking) {
        sw_follow = false;
        sw_follow_rem_x = 0.0f;
        sw_follow_rem_y = 0.0f;
    }
}

StickWalkResult stick_walk_update(float x, float y, float dt)
{
    if (!sw_installed) {
        anim_set_dude_steer(sw_steer, sw_turn);
        sw_installed = true;
    }

    sw_last_update = get_time();

    // The action menu's own loop is running: leave everything alone.
    if (sw_in_menu) {
        return STICK_WALK_HANDLED;
    }

    // Only on the map view outside combat, with no dialog or window over it.
    bool allowed = obj_dude != NULL
        && !isInCombat()
        && gmouse_scrolling_is_enabled()
        && !critter_is_dead(obj_dude);
    if (allowed) {
        const int centerX = (scr_size.lrx - scr_size.ulx + 1) / 2;
        const int centerY = (scr_size.lry - scr_size.uly - 99) / 2;
        allowed = win_get_top_win(centerX, centerY) == display_win;
    }
    if (allowed) {
        allowed = gmouse_walk_allowed();
    }
    if (!allowed) {
        // A dialog, menu or combat needs the pointer.
        gmouse_walk_show_pointer(false);
        sw_walking = false;
        sw_follow = false;
        sw_use_pending = false;
        sw_menu_pending = false;
        sw_acting = false;

        // Combat on the map view: pan, as with WALK off. Anywhere else (a
        // menu, dialogue, the inventory) the stick moves the pointer.
        if (obj_dude != NULL && isInCombat() && gmouse_scrolling_is_enabled()) {
            const int centerX = (scr_size.lrx - scr_size.ulx + 1) / 2;
            const int centerY = (scr_size.lry - scr_size.uly - 99) / 2;
            if (win_get_top_win(centerX, centerY) == display_win) {
                return STICK_WALK_PAN;
            }
        }
        return STICK_WALK_POINTER;
    }

    const float length = std::sqrt(x * x + y * y);
    if (length > 0.0f) {
        const float dirX = x / length;
        const float dirY = -y / length;
        // A new line to walk when starting or when the stick turns.
        if (!sw_walking || dirX * sw_line_x + dirY * sw_line_y < kNewLineCos) {
            sw_origin_tile = obj_dude->tile;
            sw_line_x = dirX;
            sw_line_y = dirY;
        }
        sw_dir_x = dirX;
        sw_dir_y = dirY;
        sw_run = length >= kRunDeflection;

        if (!anim_busy(obj_dude)) {
            sw_walking = sw_start();

            // Blocked by a closed door the stick points into: open it (once
            // per push, so a locked one doesn't repeat its message).
            if (!sw_walking) {
                Object* door = sw_door_ahead();
                if (door != NULL && door != sw_door_tried) {
                    sw_door_tried = door;
                    action_use_an_object(obj_dude, door);
                }
            }
        } else {
            // Mid-walk (ours or a clicked one) the hook takes over at the
            // next hex; in another animation it waits for it to finish.
            sw_walking = true;
        }
        if (sw_walking) {
            sw_follow = true;
        }
        // Walking hides the pointer until the pointer stick moves.
        gmouse_walk_hide_pointer();
        // The stick took over from whatever A started.
        sw_acting = false;
    } else {
        sw_walking = false;
        sw_door_tried = NULL;
    }

    // With the pointer hidden, the object next to the player is highlighted
    // for A (acted on in stick_walk_run_pending).
    if (sw_acting && !anim_busy(obj_dude)) {
        sw_acting = false;
    }
    if (gmouse_walk_pointer_hidden()) {
        gmouse_near_target_update(!sw_acting);
    }

    if (sw_follow) {
        sw_follow_camera(dt);
    }

    return STICK_WALK_HANDLED;
}

void stick_walk_pause(bool paused)
{
    sw_in_menu = paused;
    if (paused) {
        sw_walking = false;
    }
}

void stick_walk_request_use()
{
    sw_use_pending = true;
}

void stick_walk_request_look()
{
    sw_look_pending = true;
}

void stick_walk_request_menu()
{
    sw_menu_pending = true;
}

void stick_walk_run_pending()
{
    const bool use = sw_use_pending;
    const bool menu = sw_menu_pending;
    const bool look = sw_look_pending;
    sw_use_pending = false;
    sw_menu_pending = false;
    sw_look_pending = false;

    if (look && gmouse_walk_pointer_hidden()) {
        gmouse_near_target_look();
    }

    if ((!use && !menu) || sw_acting || !gmouse_walk_pointer_hidden()) {
        return;
    }

    if (menu) {
        sw_in_menu = true;
        const bool shown = gmouse_near_target_menu(GamepadClickHeld);
        sw_in_menu = false;
        if (shown) {
            // Whatever was picked (or nothing): no highlight until the
            // player is free again.
            sw_acting = true;
        }
    } else if (gmouse_near_target_use()) {
        sw_acting = true;
    }
}

} // namespace fallout
