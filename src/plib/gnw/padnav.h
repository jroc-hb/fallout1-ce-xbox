#ifndef FALLOUT_PLIB_GNW_PADNAV_H_
#define FALLOUT_PLIB_GNW_PADNAV_H_

namespace fallout {

// Menu navigation with the gamepad's d-pad, without the mouse pointer.
//
// In pad mode the topmost window with clickable buttons (the "nav window",
// found the way mouse clicks find their window: top down, stopping at a
// modal one) gets a focused button highlighted, the d-pad moves the focus
// to the nearest button in that direction, and A/RT presses it exactly as a
// click would (pressed on A down, released on A up). The pointer is hidden.
// Moving a pointer stick switches to pointer mode: the pointer comes back on
// the focused button and the frame goes. The d-pad switches back.
//
// Windows can opt out: WINDOW_NO_PAD_NAV (the map view, the HUD, the
// inventory: the d-pad means something else there) and WINDOW_PAD_SELF_NAV
// (screens that navigate with the d-pad themselves; only the pointer is
// hidden). Set them with padnav_set_window_flags.

// A d-pad press (dx, dy: -1/0/1). Returns true if it moved the focus (the
// caller then doesn't send the arrow key); false when there's no nav window
// or no button that way, so the arrow goes to the screen as usual.
bool padnav_dpad(int dx, int dy);

// A or RT pressed: presses the focused button. False if pad navigation
// isn't active (then it's an ordinary click).
bool padnav_activate();

// A or RT released: releases (clicks) the button padnav_activate pressed.
void padnav_release();

// Whether pad navigation is active now (pad mode with a nav window).
bool padnav_active();

// A pointer stick moved: switch to pointer mode.
void padnav_pointer_moved();

// Once a frame (process_bk): keeps the focus and its frame valid, hides the
// pointer in pad mode.
void padnav_update();

// The focus the screen would like (warp_mouse_to_button calls this).
void padnav_set_focus(int btn);

// Whether a menu opening should start in pad mode (true: walk mode is on, the
// pointer is only for when it's needed) or keep the current mode.
void padnav_set_pad_by_default(bool padByDefault);

// Whether pad mode is on (pointer hidden over menus).
bool padnav_pad_mode_on();

// Sliders and the like: while `btn` has the focus, d-pad left/right call
// `proc(btn, -1 / +1)` instead of moving the focus, and A does nothing
// (unless `pressable`: e.g. a DONE that also changes the value next to it).
// Pass NULL to remove it (do, before the button goes: button ids are reused).
typedef void PadNavAdjustProc(int btn, int dir);
void padnav_set_adjust(int btn, PadNavAdjustProc* proc, bool pressable = false);

// The highlight for `btn` covers this rectangle (in its window's
// coordinates) instead of the button itself: e.g. the whole plate a small
// red button sits on, label included. Dropped when the window goes.
void padnav_set_highlight_rect(int btn, int x, int y, int width, int height);

// GNW: a window is being deleted. Drops its highlight (so it can't outlive
// its menu), highlight rects and back key.
void padnav_window_changed(int win);

// B in pad mode is "back": the key B sends while pad navigation is active
// (or the pointer is hidden over a self-navigating window), as an SDL
// scancode: Escape unless the window asked for another one, 0 for nothing.
// -1 when B should be the usual right click.
int padnav_back_key();

// What B sends over `win` in pad mode instead of Escape (an SDL scancode, 0:
// nothing, e.g. the main menu, where Escape quits). Dropped when the window
// goes.
void padnav_set_back_key(int win, int scancode);

// For screens that navigate with the d-pad themselves (the inventory): the
// highlight (a brightened copy) over this rectangle of `win` (window
// coordinates), refreshed every frame while pad mode is on and no
// d-pad-navigated window is above. Replaces the previous one.
void padnav_show_area(int win, int x, int y, int width, int height);
void padnav_hide_area();

// Switches pad mode on or off directly (a self-navigating screen got a d-pad
// press in pointer mode).
void padnav_set_pad_mode(bool on);

// A self-navigating screen opened: starts in pad mode if that's the default
// (walk mode on), as menus padnav navigates do.
void padnav_menu_opened();

// Which of `win`'s buttons the d-pad can land on, asked every frame (e.g. the
// world map's town list: only the towns you know). One window at a time;
// dropped when it goes.
typedef bool PadNavFilterProc(int btn);
void padnav_set_filter(int win, PadNavFilterProc* proc);

// ORs WINDOW_NO_PAD_NAV / WINDOW_PAD_SELF_NAV into a window's flags.
void padnav_set_window_flags(int win, int flags);

} // namespace fallout

#endif /* FALLOUT_PLIB_GNW_PADNAV_H_ */
