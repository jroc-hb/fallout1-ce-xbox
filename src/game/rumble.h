#ifndef FALLOUT_GAME_RUMBLE_H_
#define FALLOUT_GAME_RUMBLE_H_

#include "game/object_types.h"

namespace fallout {

// Controller rumble for game events. A cue packs both motor strengths and a
// duration into an int, so an animation step can carry one
// (register_rumble_next) and play it on the frame the hit shows. 0 = none.
int rumble_cue(float low, float high, int durationMs);
void rumble_play_cue(int cue);
// The stronger of each motor and the longer duration of two cues.
int rumble_cue_merge(int a, int b);

// Rumble for `critter` taking `damage` with DAM_* `flags`: the player being
// hit, knocked down or killed, or the player landing a critical hit.
// `critical` is the attack's DAM_CRITICAL on a hit.
int rumble_cue_hit(Object* critter, int damage, int flags, bool critical, Object* attacker);

// An explosion at `tile`, fading out with distance from the player.
int rumble_cue_explosion(int tile, int elevation);

// Recoil for `attacker` firing `weapon` with attack animation `anim`: only
// the player, only guns (not throws), by weapon skill; bursts last longer.
int rumble_cue_fire(Object* attacker, Object* weapon, int hitMode, int anim);

// One poison damage tick.
void rumble_poison();

} // namespace fallout

#endif /* FALLOUT_GAME_RUMBLE_H_ */
