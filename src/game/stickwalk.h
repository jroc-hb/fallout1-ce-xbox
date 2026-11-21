#ifndef FALLOUT_GAME_STICKWALK_H_
#define FALLOUT_GAME_STICKWALK_H_

namespace fallout {

// Walking the player with an analog stick, like the console Diablo games:
// walk mode, toggled in-game with the left stick click ([control]
// stick_walk, GamepadToggleStickWalk).
//
// What the movement stick should do when it isn't walking the player.
typedef enum StickWalkResult {
    // Used: walking (or the action menu is open).
    STICK_WALK_HANDLED,
    // Combat on the map view: pan the camera.
    STICK_WALK_PAN,
    // A menu, dialogue or other window: move the pointer.
    STICK_WALK_POINTER,
} StickWalkResult;

// Call every frame with the movement stick's deflection past its deadzone
// (x right, y up, length 0..1) and the frame time in seconds.
StickWalkResult stick_walk_update(float x, float y, float dt);

// A (or RT) was tapped / held while stick walking hid the pointer: act on
// the outlined object next to the player, or open its action menu
// (gmouse.cc). Done by stick_walk_run_pending, from the game loop.
void stick_walk_request_use();
void stick_walk_request_menu();
// B while the pointer is hidden: examine the highlighted object.
void stick_walk_request_look();
// White tapped outside combat: start combat (aiming at the highlighted
// critter in walk mode). Ignored if the map loop doesn't run it within half
// a second (e.g. pressed in dialogue).
void stick_walk_request_combat();
void stick_walk_run_pending();

// While paused (a pad-driven menu such as the skill wheel is open) the stick
// neither walks nor moves anything.
void stick_walk_pause(bool paused);

// Walk mode's style: free movement (true, default) or hex by hex.
// `[control] stick_walk_free`.
bool stick_walk_free_style();
void stick_walk_set_free_style(bool free);

// Without saving the config (the caller saves).
void stick_walk_set_free_style_nosave(bool free);

// Dev menu (XBOX OPTS): free movement's pace (1 stride: the art's per-frame
// steps; 0 glide: constant speed) and stop (1 step: speed-up and walk on to a
// hex; 0 slide: instant and glide back). `[control] free_walk_pace/_stop`.
// The setter doesn't save the config.
int stick_walk_dev_pace();
int stick_walk_dev_stop();
void stick_walk_set_dev(int pace, int stop);

} // namespace fallout

#endif /* FALLOUT_GAME_STICKWALK_H_ */
