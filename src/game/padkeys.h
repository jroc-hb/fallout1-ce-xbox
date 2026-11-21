#ifndef FALLOUT_GAME_PADKEYS_H_
#define FALLOUT_GAME_PADKEYS_H_

namespace fallout {

// On-screen keyboard for the gamepad (NXDK): shown while a text prompt runs
// (the character's name, a save's description, Tell Me About). The prompt's
// loop passes each get_input() result through padkeys_frame, which returns
// it unchanged (a real keyboard still works) or, when the pad typed
// something, the key code it stands for: a character, KEY_BACKSPACE,
// KEY_RETURN (done) or KEY_ESCAPE (cancel).
//
// While it's up the pad belongs to it: d-pad picks a key (held: repeats),
// A types it, B deletes, X space, Y shift, START done, BACK cancel.

// fieldX/fieldY: where the text field is on screen (the keyboard goes
// below it, or above if there's no room). firstCapital: start with shift
// on for one letter (a name).
void padkeys_begin(int fieldX, int fieldY, bool firstCapital);
// Same, with the keyboard's top at screen row `top` (when below/above the
// field would cover something that matters, e.g. the dialogue's reply).
void padkeys_begin_at(int top, bool firstCapital);
int padkeys_frame(int keyCode);
void padkeys_end();

} // namespace fallout

#endif /* FALLOUT_GAME_PADKEYS_H_ */
