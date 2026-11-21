#include "game/stickwalk.h"

#include <cmath>

#include "game/actions.h"
#include "game/anim.h"
#include "game/combat.h"
#include "game/critter.h"
#include "game/gconfig.h"
#include "game/perk.h"
#include "game/art.h"
#include "game/gmouse.h"
#include "game/intface.h"
#include "game/map.h"
#include "game/map_defs.h"
#include "game/object.h"
#include "game/protinst.h"
#include "game/scripts.h"
#include "game/tile.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gamepad.hpp"
#include "plib/gnw/gnw.h"
#include "plib/gnw/input.h"
#include "plib/gnw/padnav.h"
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

// anim.cc's hook, as a step starts: its frame time in percent. A hex step
// is one hex in the world whichever way it goes, but zig-zagging only part
// of it is progress along the stick: cos of the angle between them, in
// world space (the screen squashes the vertical: a NE step is 16 x -12 px
// for 0.5 x 0.87 hexes).
static int sw_step_speed(Object* obj, int rotation)
{
    if (obj != obj_dude || !sw_fresh() || isInCombat()) {
        return 100;
    }

    float stickX = sw_dir_x / 32.0f;
    float stickY = -sw_dir_y / 13.86f;
    float stepX = (float)off_tile[0][rotation] / 32.0f;
    float stepY = -(float)off_tile[1][rotation] / 13.86f;
    const float stickLength = std::sqrt(stickX * stickX + stickY * stickY);
    const float stepLength = std::sqrt(stepX * stepX + stepY * stepY);
    if (stickLength <= 0.0f || stepLength <= 0.0f) {
        return 100;
    }
    float cosine = (stickX * stepX + stickY * stepY) / (stickLength * stepLength);
    if (cosine < 0.8f) {
        cosine = 0.8f; // sliding along a wall: don't rush
    }
    if (cosine > 1.0f) {
        cosine = 1.0f;
    }
    return (int)(cosine * 100.0f + 0.5f);
}

// --- Free movement ---------------------------------------------------------
//
// Walk mode's default style (`[control] stick_walk_free`, 1): outside combat
// the player moves continuously in the stick's direction instead of hex by
// hex. The position is the player's hex plus its pixel offset (obj->x/y, the
// same offset the engine uses mid-step), carried with sub-pixel precision.
// Crossing into another hex goes through obj_move_to_tile (exit grids, roofs,
// the see-through egg) and scr_chk_spatials_in, as a finished engine step
// does; the hex is always the one nearest the player's feet, so the offset
// stays within half a hex. The walk/run frames are advanced by distance, so
// the stride matches the speed. Releasing the stick glides back to the hex
// centre; anything that needs the player exactly on a hex (actions, menus,
// combat, the wheels) snaps there first (sw_free_snap).

// Hexes are regular in the world; on screen a step east is 32 px and a step
// north-east 16 x -12 px, so world units are x / 32 and -y / 13.86.
static constexpr float kWorldX = 32.0f;
static constexpr float kWorldY = 13.856f;
// Keep a facing until another is nearer the stick by this much (radians,
// 5 degrees): straight up sits between NE and NW, and flipping between them
// looked bad. Larger (it was ~0.15 cosine) and the facing lagged the stick
// visibly (user report: right -> down-right kept the right facing).
static constexpr float kFacingKeepAngle = 0.087f;
// Slowest walk, as a share of the full walk, for a light push.
static constexpr float kMinWalkShare = 0.45f;

static int sw_free_style = -1; // -1: not read yet
static bool sw_free_moving = false; // our walk/run art is on the player
static int sw_free_rotation = -1;
static float sw_free_x = 0.0f; // the player's offset from its hex centre
static float sw_free_y = 0.0f;
static int sw_free_tile = -1;
static float sw_free_frame = 0.0f; // progress through the current frame, 0..1
// Speeding up from a standstill (0..1).
static float sw_free_ramp = 0.0f;
// Stopping: the hex walked on to after the stick is let go (-1: none).
static int sw_free_stop_tile = -1;
// The last walking speed (share of the art's), kept while stopping.
static float sw_free_scale = 1.0f;
// The art the pace figures (sw_free_measure) are for.
static int sw_free_art_fid = -1;
static int sw_free_art_rotation = -1;

// Dev menu (XBOX OPTS): how free movement paces and stops, so the older
// versions can be compared. Pace 1 = stride (the art's own per-frame steps), 0
// = glide (constant speed); stop 1 = step (speed-up from a standstill, walk
// on to a hex centre on release), 0 = slide (full speed at once, glide back
// onto the current hex on release).
static int sw_dev_pace = -1;
static int sw_dev_stop = -1;

static int sw_dev_read(const char* key, int* cache)
{
    if (*cache == -1) {
        int value = 1;
        config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, key, &value);
        *cache = value != 0 ? 1 : 0;
    }
    return *cache;
}

int stick_walk_dev_pace()
{
    return sw_dev_read("free_walk_pace", &sw_dev_pace);
}

int stick_walk_dev_stop()
{
    return sw_dev_read("free_walk_stop", &sw_dev_stop);
}

void stick_walk_set_dev(int pace, int stop)
{
    sw_dev_pace = pace != 0 ? 1 : 0;
    sw_dev_stop = stop != 0 ? 1 : 0;
    config_set_value(&game_config, GAME_CONFIG_CONTROL_KEY, "free_walk_pace", sw_dev_pace);
    config_set_value(&game_config, GAME_CONFIG_CONTROL_KEY, "free_walk_stop", sw_dev_stop);
}

bool stick_walk_free_style()
{
    if (sw_free_style == -1) {
        int value = 1;
        config_get_value(&game_config, GAME_CONFIG_CONTROL_KEY, "stick_walk_free", &value);
        sw_free_style = value != 0 ? 1 : 0;
    }
    return sw_free_style == 1;
}

static void sw_free_snap();

void stick_walk_set_free_style(bool free)
{
    if (!free) {
        sw_free_snap();
    }
    sw_free_style = free ? 1 : 0;
    config_set_value(&game_config, GAME_CONFIG_CONTROL_KEY, "stick_walk_free", sw_free_style);
    gconfig_save();
}

void stick_walk_set_free_style_nosave(bool free)
{
    sw_free_style = free ? 1 : 0;
    config_set_value(&game_config, GAME_CONFIG_CONTROL_KEY, "stick_walk_free", sw_free_style);
}

static int sw_free_round(float value)
{
    return value >= 0.0f ? (int)(value + 0.5f) : -(int)(-value + 0.5f);
}

// World length of a screen vector, in hexes.
static float sw_world_length(float x, float y)
{
    const float wx = x / kWorldX;
    const float wy = y / kWorldY;
    return std::sqrt(wx * wx + wy * wy);
}

// The art's pace: how far each frame carries the player (the per-frame
// shifts baked into it, as world hexes; frame k's is the step into it) and
// its fps. Free movement moves by these, spread smoothly over each frame,
// so a planted foot stays put as in the engine's own walk.
#define SW_FREE_MAX_FRAMES 32
static float sw_free_shift[SW_FREE_MAX_FRAMES];
static int sw_free_frames = 0;
static int sw_free_fps = 10;

static void sw_free_measure(int fid, int rotation)
{
    if (fid == sw_free_art_fid && rotation == sw_free_art_rotation) {
        return;
    }
    sw_free_art_fid = fid;
    sw_free_art_rotation = rotation;
    sw_free_frames = 0;
    sw_free_fps = 10;

    CacheEntry* handle;
    Art* art = art_ptr_lock(fid, &handle);
    if (art == NULL) {
        return;
    }
    int frames = art_frame_max_frame(art);
    if (frames > SW_FREE_MAX_FRAMES) {
        frames = SW_FREE_MAX_FRAMES;
    }
    for (int frame = 0; frame < frames; frame++) {
        int x;
        int y;
        art_frame_hot(art, frame, rotation, &x, &y);
        sw_free_shift[frame] = sw_world_length((float)x, (float)y);
    }
    sw_free_frames = frames;
    if (art_frame_fps(art) > 0) {
        sw_free_fps = art_frame_fps(art);
    }
    art_ptr_unlock(handle);
}

static int sw_free_fid(int anim, int rotation)
{
    const int fid = obj_dude->fid;
    return art_id(FID_TYPE(fid), fid & 0xFFF, anim, (fid & 0xF000) >> 12, rotation + 1);
}

// The player stands where it is (on its hex centre or not).
static void sw_free_stand(Rect* dirty)
{
    Rect rect;
    int fid = sw_free_fid(ANIM_STAND, obj_dude->rotation);
    if (art_exists(fid)) {
        obj_change_fid(obj_dude, fid, &rect);
        rect_min_bound(dirty, &rect, dirty);
    }
    obj_set_frame(obj_dude, 0, &rect);
    rect_min_bound(dirty, &rect, dirty);
    sw_free_moving = false;
}

// Back onto the hex centre at once, standing.
static void sw_free_snap()
{
    if (obj_dude == NULL) {
        return;
    }
    Rect dirty;
    obj_bound(obj_dude, &dirty);
    if (obj_dude->x != 0 || obj_dude->y != 0) {
        Rect rect;
        obj_offset(obj_dude, -obj_dude->x, -obj_dude->y, &rect);
        rect_min_bound(&dirty, &rect, &dirty);
    }
    sw_free_x = 0.0f;
    sw_free_y = 0.0f;
    if (sw_free_moving) {
        sw_free_stand(&dirty);
    }
    tile_refresh_rect(&dirty, obj_dude->elevation);
}

// The neighbour (rotation) whose centre is nearer the point (offset from the
// current hex centre) than the current one's, or -1.
static int sw_free_nearest(float x, float y)
{
    int best = -1;
    float bestDistance = sw_world_length(x, y);
    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        const float distance = sw_world_length(x - (float)off_tile[0][rotation], y - (float)off_tile[1][rotation]);
        if (distance < bestDistance) {
            best = rotation;
            bestDistance = distance;
        }
    }
    return best;
}

// Whether the player can stand on the hex next to it in `rotation`.
static bool sw_free_open(int rotation)
{
    const int tile = tile_num_in_direction(obj_dude->tile, rotation, 1);
    return hexGridTileIsValid(tile) && obj_blocking_at(obj_dude, tile, obj_dude->elevation) == NULL;
}

// Whether moving the player by (dx, dy) is possible (not into a blocked hex).
static bool sw_free_can_move(float dx, float dy)
{
    const int rotation = sw_free_nearest(sw_free_x + dx, sw_free_y + dy);
    return rotation == -1 || sw_free_open(rotation);
}

// Moves the player by (dx, dy) screen px, crossing into the next hex when
// that one is nearer. The caller checked it's free.
static void sw_free_move(float dx, float dy, Rect* dirty)
{
    sw_free_x += dx;
    sw_free_y += dy;

    Rect rect;
    const int rotation = sw_free_nearest(sw_free_x, sw_free_y);
    if (rotation != -1) {
        const int tile = tile_num_in_direction(obj_dude->tile, rotation, 1);
        sw_free_x -= (float)off_tile[0][rotation];
        sw_free_y -= (float)off_tile[1][rotation];

        // As a finished engine step: the hex (exit grids, roofs, the egg),
        // then the new hex's spatial scripts.
        obj_move_to_tile(obj_dude, tile, obj_dude->elevation, &rect);
        rect_min_bound(dirty, &rect, dirty);
        sw_free_tile = obj_dude->tile;
        scr_chk_spatials_in(obj_dude, obj_dude->tile, obj_dude->elevation);
    }

    const int offsetX = sw_free_round(sw_free_x) - obj_dude->x;
    const int offsetY = sw_free_round(sw_free_y) - obj_dude->y;
    if (offsetX != 0 || offsetY != 0) {
        obj_offset(obj_dude, offsetX, offsetY, &rect);
        rect_min_bound(dirty, &rect, dirty);
    }
}

// Angle (radians, 0..pi) between a facing and a screen direction.
static float sw_free_angle_to(int rotation, float dirX, float dirY)
{
    float x;
    float y;
    sw_step_dir(rotation, &x, &y);
    float cosine = x * dirX + y * dirY;
    float sine = x * dirY - y * dirX;
    return std::fabs(std::atan2(sine, cosine));
}

// The facing nearest the direction (screen, unit), keeping the current one
// unless another is nearer by kFacingKeepAngle (only so straight up, halfway
// between NE and NW, doesn't flicker).
static int sw_free_facing(float dirX, float dirY, int current)
{
    int best = -1;
    float bestAngle = 10.0f;
    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        const float angle = sw_free_angle_to(rotation, dirX, dirY);
        if (angle < bestAngle) {
            best = rotation;
            bestAngle = angle;
        }
    }
    if (current >= 0 && current < ROTATION_COUNT && best != current
        && sw_free_angle_to(current, dirX, dirY) - bestAngle < kFacingKeepAngle) {
        return current;
    }
    return best;
}

// Time to reach full speed from a standstill.
static constexpr float kFreeRampSeconds = 0.12f;

// The point (offset from the current hex centre, screen px) of hex `tile`,
// if it's the current one or a neighbour.
static bool sw_free_tile_offset(int tile, float* x, float* y)
{
    if (tile == obj_dude->tile) {
        *x = 0.0f;
        *y = 0.0f;
        return true;
    }
    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        if (tile_num_in_direction(obj_dude->tile, rotation, 1) == tile) {
            *x = (float)off_tile[0][rotation];
            *y = (float)off_tile[1][rotation];
            return true;
        }
    }
    return false;
}

// Plays the art on by `frames` (fractional) and returns the distance
// (hexes) its steps cover meanwhile; *advanced gets the whole frames passed.
static float sw_free_play(float frames, int* frame, int* advanced)
{
    float distance = 0.0f;
    *advanced = 0;
    if (sw_free_frames <= 0) {
        return 0.0f;
    }
    while (frames > 0.0f) {
        const float part = std::fmin(frames, 1.0f - sw_free_frame);
        const int next = (*frame + 1) % sw_free_frames;
        // During frame k the player covers the step into frame k + 1, so
        // it's where the engine would put it each time the image changes.
        distance += sw_free_shift[next] * part;
        sw_free_frame += part;
        frames -= part;
        if (sw_free_frame >= 0.999f) {
            sw_free_frame = 0.0f;
            *frame = next;
            (*advanced)++;
        }
    }
    return distance;
}

// One frame of free movement. `deflection` 0..1 (0: the stick is let go:
// walk on to a hex centre and stand). Returns whether the player moved.
static bool sw_free_update(float deflection, float dt)
{
    // Someone else moved the player (an engine walk, a map change, a
    // script): start from where it is now.
    if (obj_dude->tile != sw_free_tile
        || sw_free_round(sw_free_x) != obj_dude->x
        || sw_free_round(sw_free_y) != obj_dude->y) {
        sw_free_tile = obj_dude->tile;
        sw_free_x = (float)obj_dude->x;
        sw_free_y = (float)obj_dude->y;
        sw_free_stop_tile = -1;
        if (sw_free_moving && FID_ANIM_TYPE(obj_dude->fid) != ANIM_WALK && FID_ANIM_TYPE(obj_dude->fid) != ANIM_RUNNING) {
            sw_free_moving = false;
        }
    }

    // An engine animation (a clicked walk, a door, a script) has the
    // player: wait for it.
    if (anim_busy(obj_dude)) {
        sw_free_moving = false;
        sw_free_stop_tile = -1;
        return false;
    }

    Rect dirty;
    obj_bound(obj_dude, &dirty);

    const bool stopping = deflection <= 0.0f;
    if (stopping && !sw_free_moving) {
        // Standing: on its hex centre, nothing to do; off it (stopped
        // against a wall), walk the few pixels there.
        sw_free_ramp = 0.0f;
        if (obj_dude->x == 0 && obj_dude->y == 0) {
            return false;
        }
        sw_free_rotation = obj_dude->rotation;
        sw_free_scale = kMinWalkShare;
    }

    bool run;
    int rotation;
    float dirX; // world direction of the move (unit)
    float dirY;
    float left = 1000.0f; // stopping: the distance to the stop point (hexes)

    if (!stopping) {
        sw_free_stop_tile = -1;
        sw_free_ramp = stick_walk_dev_stop() ? std::fmin(1.0f, sw_free_ramp + dt / kFreeRampSeconds) : 1.0f;
        run = deflection >= kRunDeflection
            && (obj_dude->data.critter.combat.results & DAM_CRIP_LEG_ANY) == 0
            && !(is_pc_flag(PC_FLAG_SNEAKING) && !perk_level(PERK_SILENT_RUNNING));
        rotation = sw_free_facing(sw_dir_x, sw_dir_y, sw_free_moving ? sw_free_rotation : obj_dude->rotation);
        dirX = sw_dir_x / kWorldX;
        dirY = sw_dir_y / kWorldY;
        sw_free_scale = run ? 1.0f : std::fmax(kMinWalkShare, std::fmin(1.0f, deflection / kRunDeflection));
    } else {
        // Let go: carry on to the nearest hex centre half a hex ahead (not
        // back), in the same stride, then stand.
        run = FID_ANIM_TYPE(obj_dude->fid) == ANIM_RUNNING;
        rotation = sw_free_rotation;
        float stopX = 0.0f;
        float stopY = 0.0f;
        if (!stick_walk_dev_stop()) {
            // Older stop: glide straight back onto the hex it's on.
            sw_free_stop_tile = obj_dude->tile;
        }
        if (sw_free_stop_tile == -1 || !sw_free_tile_offset(sw_free_stop_tile, &stopX, &stopY)) {
            float ux;
            float uy;
            sw_step_dir(rotation, &ux, &uy);
            // Half a hex ahead along the facing, in screen px.
            const float aheadLength = sw_world_length(ux, uy);
            const float aheadX = sw_free_x + ux / aheadLength * 0.5f;
            const float aheadY = sw_free_y + uy / aheadLength * 0.5f;
            const int nearest = sw_free_nearest(aheadX, aheadY);
            sw_free_stop_tile = obj_dude->tile;
            if (nearest != -1 && sw_free_open(nearest)) {
                sw_free_stop_tile = tile_num_in_direction(obj_dude->tile, nearest, 1);
            }
            sw_free_tile_offset(sw_free_stop_tile, &stopX, &stopY);
        }
        dirX = (stopX - sw_free_x) / kWorldX;
        dirY = (stopY - sw_free_y) / kWorldY;
        left = std::sqrt(dirX * dirX + dirY * dirY);
        // Facing the way it goes (back onto the hex after a wall: turned).
        const float screenX = stopX - sw_free_x;
        const float screenY = stopY - sw_free_y;
        const float screenLength = std::sqrt(screenX * screenX + screenY * screenY);
        if (screenLength > 2.0f && stick_walk_dev_stop()) {
            rotation = sw_free_facing(screenX / screenLength, screenY / screenLength, rotation);
        }
        if (left < 0.01f) {
            // There: stand on the hex centre.
            Rect rect;
            if (obj_dude->x != 0 || obj_dude->y != 0) {
                obj_offset(obj_dude, -obj_dude->x, -obj_dude->y, &rect);
                rect_min_bound(&dirty, &rect, &dirty);
            }
            sw_free_x = 0.0f;
            sw_free_y = 0.0f;
            sw_free_stop_tile = -1;
            sw_free_ramp = 0.0f;
            sw_free_stand(&dirty);
            tile_refresh_rect(&dirty, obj_dude->elevation);
            return false;
        }
    }

    const float dirLength = std::sqrt(dirX * dirX + dirY * dirY);
    if (dirLength <= 0.0f) {
        return false;
    }
    dirX /= dirLength;
    dirY /= dirLength;

    // The walk or run art for that facing.
    int anim = run ? ANIM_RUNNING : ANIM_WALK;
    int fid = sw_free_fid(anim, rotation);
    if (!art_exists(fid)) {
        anim = ANIM_WALK;
        fid = sw_free_fid(anim, rotation);
        if (!art_exists(fid)) {
            return false;
        }
    }
    sw_free_measure(fid, rotation);

    // Switch the art (facing, walk/run), keeping the place in the stride.
    // (A critter's fid is the same for every facing: compare the rotation too.)
    Rect rect;
    if (!sw_free_moving || obj_dude->fid != fid || obj_dude->rotation != rotation) {
        int frame = obj_dude->frame;
        if (!sw_free_moving) {
            frame = 0;
            sw_free_frame = 0.0f;
        }
        if (frame >= sw_free_frames) {
            frame = 0;
        }
        obj_change_fid(obj_dude, fid, &rect);
        rect_min_bound(&dirty, &rect, &dirty);
        obj_set_rotation(obj_dude, rotation, &rect);
        rect_min_bound(&dirty, &rect, &dirty);
        obj_set_frame(obj_dude, frame, &rect);
        rect_min_bound(&dirty, &rect, &dirty);
        sw_free_moving = true;
    }
    sw_free_rotation = rotation;

    // Play the art on and move by its steps (slower for a light push or
    // while speeding up; at full pace while stopping).
    const float scale = stopping ? sw_free_scale : sw_free_scale * (0.3f + 0.7f * sw_free_ramp);
    int frame = obj_dude->frame;
    int advanced = 0;
    const float framesToPlay = dt * (float)sw_free_fps * scale;
    float hexes = sw_free_play(framesToPlay, &frame, &advanced);
    if (!stick_walk_dev_pace() && sw_free_frames > 0) {
        // Older pace: the cycle's average step, at a constant speed.
        float cycle = 0.0f;
        for (int index = 0; index < sw_free_frames; index++) {
            cycle += sw_free_shift[index];
        }
        hexes = framesToPlay * cycle / (float)sw_free_frames;
    }
    if (stopping && hexes > left) {
        hexes = left;
    }

    float moveX = dirX * hexes * kWorldX;
    float moveY = dirY * hexes * kWorldY;

    // Blocked: slide along the wall, else stop. A closed door stops it:
    // pushing into one opens it (stick_walk_update), sliding would walk past.
    if (!sw_free_can_move(moveX, moveY)) {
        const int blockedRotation = sw_free_nearest(sw_free_x + moveX, sw_free_y + moveY);
        Object* blocker = NULL;
        if (blockedRotation != -1) {
            const int tile = tile_num_in_direction(obj_dude->tile, blockedRotation, 1);
            blocker = hexGridTileIsValid(tile) ? obj_blocking_at(obj_dude, tile, obj_dude->elevation) : NULL;
        }
        if (blocker != NULL && FID_TYPE(blocker->fid) == OBJ_TYPE_SCENERY && obj_is_a_portal(blocker) && !obj_is_open(blocker)) {
            moveX = 0.0f;
            moveY = 0.0f;
        } else {
            // Take off the part heading into the blocked hex (towards its
            // centre, in world space): what's left runs along the border
            // between the two hexes. Twice, for a corner of two.
            float wx = moveX / kWorldX;
            float wy = moveY / kWorldY;
            for (int pass = 0; pass < 2; pass++) {
                const int blocked = sw_free_nearest(sw_free_x + wx * kWorldX, sw_free_y + wy * kWorldY);
                if (blocked == -1 || sw_free_open(blocked)) {
                    break;
                }
                float nx = (float)off_tile[0][blocked] / kWorldX;
                float ny = (float)off_tile[1][blocked] / kWorldY;
                const float nlen = std::sqrt(nx * nx + ny * ny);
                nx /= nlen;
                ny /= nlen;
                const float into = wx * nx + wy * ny;
                if (into <= 0.0f) {
                    break;
                }
                wx -= into * nx;
                wy -= into * ny;
            }
            moveX = wx * kWorldX;
            moveY = wy * kWorldY;
            if (!sw_free_can_move(moveX, moveY)) {
                moveX = 0.0f;
                moveY = 0.0f;
            }
        }

        if (moveX == 0.0f && moveY == 0.0f) {
            // Against a wall: stand where it is (and maybe open the door
            // there); it settles onto the hex when the stick is let go.
            sw_free_ramp = 0.0f;
            sw_free_stand(&dirty);
            tile_refresh_rect(&dirty, obj_dude->elevation);
            return false;
        }
    }

    sw_free_move(moveX, moveY, &dirty);
    if (advanced > 0) {
        obj_set_frame(obj_dude, frame, &rect);
        rect_min_bound(&dirty, &rect, &dirty);
    }

    tile_refresh_rect(&dirty, obj_dude->elevation);
    return true;
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
        anim_set_dude_step_speed(sw_step_speed);
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
        // A dialog, menu or combat needs the pointer, and the player on a hex.
        if (obj_dude != NULL && !anim_busy(obj_dude)) {
            sw_free_snap();
        }
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

    // Back on the map after a menu navigated with the d-pad: the pointer
    // stays hidden (it's only shown when the pointer stick is used).
    if (padnav_pad_mode_on() && !gmouse_walk_pointer_hidden()) {
        gmouse_walk_hide_pointer();
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

        if (stick_walk_free_style()) {
            sw_walking = sw_free_update(length, dt);
            if (!sw_walking && !anim_busy(obj_dude)) {
                Object* door = sw_door_ahead();
                if (door != NULL && door != sw_door_tried) {
                    sw_door_tried = door;
                    sw_free_snap();
                    action_use_an_object(obj_dude, door);
                }
            }
        } else if (!anim_busy(obj_dude)) {
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
        if (stick_walk_free_style() && sw_free_update(0.0f, dt)) {
            sw_follow = true;
        }
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
        if (obj_dude != NULL && !anim_busy(obj_dude)) {
            sw_free_snap();
        }
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

static bool sw_combat_pending = false;
static unsigned int sw_combat_time = 0;

void stick_walk_request_combat()
{
    sw_combat_pending = true;
    sw_combat_time = get_time();
}

void stick_walk_run_pending()
{
    if ((sw_combat_pending || sw_use_pending || sw_menu_pending || sw_look_pending)
        && obj_dude != NULL && !anim_busy(obj_dude)) {
        // Actions start from a hex.
        sw_free_snap();
    }

    const bool fight = sw_combat_pending && elapsed_time(sw_combat_time) < 500;
    sw_combat_pending = false;
    if (fight && !isInCombat() && intface_is_enabled()) {
        // As the keyboard's A (game.cc), plus the aim.
        gmouse_combat_aim_prepare();
        combat(NULL);
        gmouse_combat_aim_clear();
        return;
    }

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
