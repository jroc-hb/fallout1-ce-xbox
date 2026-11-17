#include "game/rumble.h"

#include <algorithm>

#include "game/anim.h"
#include "game/item.h"
#include "game/object.h"
#include "game/skill_defs.h"
#include "game/stat.h"
#include "game/stat_defs.h"
#include "game/tile.h"
#include "plib/gnw/gamepad.hpp"

namespace fallout {

// Explosions further than this many hexes from the player aren't felt.
#define RUMBLE_EXPLOSION_RANGE 16

static int rumble_byte(float value)
{
    return std::clamp(static_cast<int>(value * 255.0f + 0.5f), 0, 255);
}

int rumble_cue(float low, float high, int durationMs)
{
    int duration = std::clamp((durationMs + 5) / 10, 0, 255);
    if (duration == 0 || (rumble_byte(low) == 0 && rumble_byte(high) == 0)) {
        return 0;
    }

    return rumble_byte(low) | (rumble_byte(high) << 8) | (duration << 16);
}

void rumble_play_cue(int cue)
{
    if (cue == 0) {
        return;
    }

    float low = static_cast<float>(cue & 0xFF) / 255.0f;
    float high = static_cast<float>((cue >> 8) & 0xFF) / 255.0f;
    int durationMs = ((cue >> 16) & 0xFF) * 10;
    GamepadRumble(low, high, durationMs);
}

int rumble_cue_merge(int a, int b)
{
    int merged = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        merged |= std::max((a >> shift) & 0xFF, (b >> shift) & 0xFF) << shift;
    }
    return merged;
}

int rumble_cue_fire(Object* attacker, Object* weapon, int hitMode, int anim)
{
    if (attacker == NULL || attacker != obj_dude || weapon == NULL || anim == ANIM_THROW_ANIM) {
        return 0;
    }

    bool burst = anim == ANIM_FIRE_BURST || anim == ANIM_FIRE_CONTINUOUS;
    switch (item_w_skill(weapon, hitMode)) {
    case SKILL_BIG_GUNS:
        // A heavy thump on the big motor.
        return rumble_cue(0.7f, 0.4f, burst ? 450 : 140);
    case SKILL_ENERGY_WEAPONS:
        // A light electric buzz on the small motor.
        return rumble_cue(0.1f, 0.5f, burst ? 300 : 80);
    default:
        return rumble_cue(0.35f, 0.5f, burst ? 320 : 90);
    }
}

int rumble_cue_hit(Object* critter, int damage, int flags, bool critical, Object* attacker)
{
    if (critter == NULL || obj_dude == NULL) {
        return 0;
    }

    if (critter == obj_dude) {
        if ((flags & DAM_DEAD) != 0) {
            return rumble_cue(1.0f, 0.8f, 900);
        }

        // Scale with the share of the player's health the hit took: a third
        // of it or more is the full jolt.
        int maxHp = std::max(1, stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS));
        float share = std::clamp(static_cast<float>(damage) * 3.0f / static_cast<float>(maxHp), 0.0f, 1.0f);
        if (damage <= 0 && (flags & (DAM_KNOCKED_DOWN | DAM_KNOCKED_OUT)) == 0) {
            return 0;
        }

        float low = 0.35f + 0.65f * share;
        float high = 0.2f + 0.5f * share;
        int durationMs = 120 + static_cast<int>(280.0f * share);

        if (critical) {
            low = 1.0f;
            high = std::max(high, 0.7f);
            durationMs = std::max(durationMs, 400);
        }

        if ((flags & (DAM_KNOCKED_DOWN | DAM_KNOCKED_OUT)) != 0) {
            low = 1.0f;
            durationMs = std::max(durationMs, 500);
        }

        return rumble_cue(low, high, durationMs);
    }

    // The player's critical hit: a short, sharp kick.
    if (critical && attacker == obj_dude) {
        return rumble_cue(0.45f, 0.75f, 160);
    }

    return 0;
}

int rumble_cue_explosion(int tile, int elevation)
{
    if (obj_dude == NULL || tile == -1 || elevation != obj_dude->elevation) {
        return 0;
    }

    int distance = tile_dist(obj_dude->tile, tile);
    if (distance > RUMBLE_EXPLOSION_RANGE) {
        return 0;
    }

    float nearness = 1.0f - static_cast<float>(distance) / static_cast<float>(RUMBLE_EXPLOSION_RANGE + 1);
    return rumble_cue(0.25f + 0.75f * nearness, 0.5f * nearness, 250 + static_cast<int>(350.0f * nearness));
}

void rumble_poison()
{
    rumble_play_cue(rumble_cue(0.3f, 0.0f, 100));
}

} // namespace fallout
