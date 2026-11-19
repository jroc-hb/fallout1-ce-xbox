#ifndef FALLOUT_GAME_SKILLWHEEL_H_
#define FALLOUT_GAME_SKILLWHEEL_H_

namespace fallout {

// The Skilldex as a radial menu around the player, for walk mode (the right
// stick click; see game.cc KEY_S). The left stick or the d-pad picks one
// of the eight skills, A uses it, B / START / another right click closes it.
// Returns a SKILLDEX_RC_* code like skilldex_select.
int skill_wheel_select();

// The skill's action for a SKILLDEX_RC_* code: straight on the object the
// player is next to (or on the player for First Aid / Doctor), otherwise the
// usual skill cursor to pick a target with the pointer.
void skill_wheel_use(int rc);

// The menus wheel (BACK): inventory, Pip-Boy, character, map, Skilldex,
// options, load, save. Returns the hotkey for the picked screen (to pass to
// game_handle_input) or -1. `openKey` (the key BACK sends) also closes it.
int menus_wheel_select(int openKey);

} // namespace fallout

#endif /* FALLOUT_GAME_SKILLWHEEL_H_ */
