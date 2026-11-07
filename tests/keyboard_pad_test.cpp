// SPDX-License-Identifier: GPL-3.0-only
#include "input/keyboard_pad.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
using namespace prospero;
using namespace std::chrono_literals;

namespace {
constexpr uint16_t enter = 0x0D, backspace = 0x08, esc = 0x1B, tab = 0x09, space = 0x20;
constexpr uint16_t left = 0x25, up = 0x26, right = 0x27, down = 0x28, f1 = 0x70, f2 = 0x71, f5 = 0x74;
constexpr uint16_t shift = 0x10, ctrl = 0x11, alt = 0x12, lshift = 0xA0, lctrl = 0xA2, ralt = 0xA5;

uint32_t press(KeyboardPad &pad, uint16_t vk, std::chrono::steady_clock::time_point at) {
    pad.key(vk, false, at);
    const uint32_t buttons = pad.state(at).buttons;
    pad.key(vk, true, at);
    return buttons;
}

// Every profile shares the menu keys and leaves Alt free.
constexpr bool common_keys(Profile profile) {
    using namespace moonlight;
    const bool alt_free = !key_control(profile, 0x12) && !key_control(profile, 0xA4) && !key_control(profile, 0xA5);
    return key_control(profile, enter) == a && key_control(profile, backspace) == b && key_control(profile, esc) == start &&
           key_control(profile, f1) == home && key_control(profile, f2) == back && !key_control(profile, f5) && alt_free;
}
}

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    KeyboardPad pad;
    assert(pad.profile() == Profile::standard);
    assert(pad.state(t0) == PadInput{});
    assert(!pad.next_change(t0));

    // Standard: each key as in the table.
    struct Key { uint16_t vk; uint32_t button; };
    const Key buttons[] = {
        {up, moonlight::dpad_up}, {down, moonlight::dpad_down}, {left, moonlight::dpad_left}, {right, moonlight::dpad_right},
        {enter, moonlight::a}, {space, moonlight::a}, {backspace, moonlight::b}, {esc, moonlight::start},
        {'Q', moonlight::x}, {'E', moonlight::y}, {'Z', moonlight::left_button}, {'C', moonlight::right_button},
        {'F', moonlight::left_stick}, {'G', moonlight::right_stick}, {tab, moonlight::touchpad}, {'T', moonlight::touchpad},
        {f2, moonlight::back}, {f1, moonlight::home},
    };
    for (const auto &k : buttons) {
        assert(press(pad, k.vk, t0) == k.button);
        assert(pad.state(t0) == PadInput{});
    }
    assert(!pad.key('H', false) && !pad.key(0xC0, false));  // PS and Create moved to F1 and F2
    assert(pad.key('1', false) && pad.state(t0).lt == 255 && pad.state(t0).rt == 0);
    assert(pad.key('3', false) && pad.state(t0).rt == 255 && pad.state(t0).buttons == 0);
    pad.clear();
    for (const auto profile : {Profile::standard, Profile::action, Profile::shooter, Profile::driving, Profile::fighting,
                               Profile::platformer, Profile::sports})
        assert(common_keys(profile));

    // Two keys for one button: the button stays down until both are up. A repeat changes nothing.
    assert(pad.key(enter, false) && pad.key(space, false));
    assert(!pad.key(enter, false));
    assert(pad.key(space, true) && pad.state(t0).buttons == moonlight::a);
    assert(pad.key(enter, true) && pad.state(t0).buttons == 0);
    assert(!pad.key(enter, true));  // already up

    // Keys that are not the pad's, and modifiers in the standard profile, change nothing.
    assert(!pad.key('Y', false) && !pad.key(0x72, false) && !pad.key(shift, false) && !pad.key(lctrl, false));
    assert(pad.state(t0) == PadInput{});
    pad.clear();

    // Sticks: full deflection, diagonals, opposite keys cancel out. The D-pad's opposite sides cancel too.
    pad.key('W', false);
    assert(pad.state(t0).ly == 32767 && pad.state(t0).lx == 0);
    pad.key('D', false);
    assert(pad.state(t0).ly == 32767 && pad.state(t0).lx == 32767);
    pad.key('S', false);
    assert(pad.state(t0).ly == 0 && pad.state(t0).lx == 32767);
    pad.key('A', false);
    assert(pad.state(t0).lx == 0 && pad.state(t0).ly == 0);
    pad.key('W', true);
    pad.key('D', true);
    assert(pad.state(t0).ly == -32767 && pad.state(t0).lx == -32767);
    pad.clear();
    pad.key('I', false);
    pad.key('J', false);
    assert(pad.state(t0).ry == 32767 && pad.state(t0).rx == -32767 && pad.state(t0).lx == 0);
    pad.key('K', false);
    pad.key('L', false);
    assert(pad.state(t0).ry == 0 && pad.state(t0).rx == 0);
    pad.clear();
    pad.key(up, false);
    pad.key(left, false);
    assert(pad.state(t0).buttons == (moonlight::dpad_up | moonlight::dpad_left));
    pad.key(down, false);
    assert(pad.state(t0).buttons == moonlight::dpad_left);
    pad.clear();

    // Moonlight's shortcuts: only with Ctrl, Alt and Shift all held, and only its keys.
    pad.key(lctrl, false);
    pad.key(lshift, false);
    assert(pad.key('W', false) && pad.key('Q', false));  // two modifiers: the keys count
    pad.key('W', true);
    pad.key('Q', true);
    pad.key(ralt, false);  // right Alt: now all three
    for (const uint16_t vk : {'Q', 'E', 'S', 'M', 'C', 'V', 'L', 'Z', 'X', 'D', 'K'}) assert(!pad.key(vk, false));
    assert(pad.state(t0) == PadInput{});
    assert(pad.key('W', false) && pad.key(enter, false));  // not shortcut keys (right Alt: the menu keys, W on the D-pad)
    assert(pad.state(t0).buttons == (moonlight::dpad_up | moonlight::a));
    // After a shortcut Moonlight sends a key-up for every held key: they come up as usual.
    assert(pad.key('W', true) && pad.key(enter, true) && !pad.key('Q', true));
    assert(pad.state(t0) == PadInput{});
    pad.key(ralt, true);
    assert(pad.key('Q', false));  // Alt up again: Q is square
    pad.clear();
    // The generic and the sided code of a modifier: it is held while either is.
    pad.key(ctrl, false);
    pad.key(alt, false);
    pad.key(shift, false);
    pad.key(lshift, false);
    pad.key(shift, true);
    assert(!pad.key('S', false));
    pad.key(lshift, true);
    assert(pad.key('S', false));
    pad.clear();

    // Mouse buttons; X1 and Z share L1.
    assert(pad.mouse_button(1, false) && pad.state(t0).rt == 255);
    assert(pad.mouse_button(3, false) && pad.state(t0).lt == 255);
    assert(pad.mouse_button(2, false) && pad.state(t0).buttons == moonlight::right_stick);
    assert(pad.mouse_button(5, false) && (pad.state(t0).buttons & moonlight::right_button));
    assert(pad.mouse_button(4, false) && pad.key('Z', false));
    assert(pad.mouse_button(4, true) && (pad.state(t0).buttons & moonlight::left_button));
    assert(pad.key('Z', true) && !(pad.state(t0).buttons & moonlight::left_button));
    assert(!pad.mouse_button(4, true) && !pad.mouse_button(0, false) && !pad.mouse_button(6, false));
    pad.key('1', false);
    pad.mouse_button(3, true);
    assert(pad.state(t0).lt == 255);  // the key still holds L2
    pad.clear();

    // Mouse movement: proportional to the speed, at least the lift, up to full; back to the centre after the window.
    pad.mouse_move(15, 0, t0);  // 300 counts/s: a tenth of full speed
    const auto slow = pad.state(t0);
    assert(slow.rx == static_cast<int>(std::lround((KeyboardPad::lift + (1 - KeyboardPad::lift) * 0.1) * 32767)));
    assert(slow.ry == 0 && slow.lx == 0);
    assert(pad.next_change(t0) == t0 + KeyboardPad::mouse_window);
    assert(pad.state(t0 + KeyboardPad::mouse_window - 1ms).rx == slow.rx);
    assert(pad.state(t0 + KeyboardPad::mouse_window) == PadInput{});
    assert(!pad.next_change(t0 + KeyboardPad::mouse_window));
    pad.mouse_move(0, 1000, t0);  // far beyond full speed, downwards
    assert(pad.state(t0).ry == -32767 && pad.state(t0).rx == 0);
    pad.clear();
    // A diagonal keeps its direction.
    pad.mouse_move(-1000, -1000, t0);
    assert(pad.state(t0).rx == -pad.state(t0).ry && pad.state(t0).rx < -23000 && pad.state(t0).rx > -23200);
    pad.clear();
    // Several reports add up within the window, and the oldest leaves first.
    pad.mouse_move(15, 0, t0);
    pad.mouse_move(15, 0, t0 + 20ms);
    assert(pad.state(t0 + 20ms).rx > slow.rx);
    assert(pad.next_change(t0 + 20ms) == t0 + KeyboardPad::mouse_window);
    assert(pad.state(t0 + KeyboardPad::mouse_window).rx == slow.rx);
    assert(pad.state(t0 + 70ms) == PadInput{});
    // More reports than the buffer holds still count.
    pad.clear();
    for (int i = 0; i < 200; i++) pad.mouse_move(1, 0, t0);
    assert(pad.state(t0).rx == 32767);
    pad.clear();
    // Sensitivity scales the speed.
    pad.set_sensitivity(200);
    pad.mouse_move(15, 0, t0);
    assert(pad.state(t0).rx == static_cast<int>(std::lround((KeyboardPad::lift + (1 - KeyboardPad::lift) * 0.2) * 32767)));
    pad.clear();  // keeps the sensitivity
    pad.mouse_move(15, 0, t0);
    assert(pad.state(t0).rx > slow.rx);
    pad.set_sensitivity(100);
    pad.clear();
    // The mouse and I J K L add up.
    pad.key('J', false);
    pad.mouse_move(15, 0, t0);
    assert(pad.state(t0).rx == slow.rx - 32767);
    pad.mouse_move(1000, 0, t0);
    assert(pad.state(t0).rx == 0);
    pad.key('L', false);
    pad.key('J', true);
    assert(pad.state(t0).rx == 32767);  // clamped
    pad.clear();
    // Absolute mode: the difference to the last position; a relative report starts over.
    pad.mouse_position(100, 100, t0);
    assert(pad.state(t0) == PadInput{});
    pad.mouse_position(115, 100, t0);
    assert(pad.state(t0).rx == slow.rx);
    pad.mouse_move(0, 0, t0);
    pad.mouse_position(500, 500, t0 + 60ms);
    assert(pad.state(t0 + 60ms) == PadInput{});
    pad.clear();

    // What adds the pad: a key or mouse button that is one of its controls, not the mouse's movement or the wheel, so a
    // friend who only watches and nudges the mouse takes no player slot.
    {
        KeyboardPad fresh;
        fresh.mouse_move(1000, 0, t0);
        fresh.scroll(120, t0);
        fresh.hscroll(120, t0);
        assert(fresh.state(t0) != PadInput{} && !fresh.pressed(t0));
        fresh.drop_movement();  // the movement before the pad doesn't count once it is there
        assert(fresh.state(t0) == PadInput{} && !fresh.next_change(t0));
        assert(!fresh.key(vk::lshift, false) && !fresh.pressed(t0));  // a modifier without a control
        assert(!fresh.key('M', false) && !fresh.pressed(t0));         // a key without a control
        assert(fresh.key('W', false) && fresh.pressed(t0));
        fresh.key('W', true);
        assert(fresh.mouse_button(1, false) && fresh.pressed(t0));  // R2
        fresh.mouse_button(1, true);
        assert(!fresh.pressed(t0));
        // Ctrl in action halves the sticks: alone, as for Ctrl+Alt and the wheel, it deflects nothing.
        fresh.set_profile(Profile::action);
        assert(fresh.key(vk::lctrl, false) && !fresh.pressed(t0));
        assert(fresh.key('D', false) && fresh.pressed(t0) && fresh.state(t0).lx == 16384);
        // In driving, W eases in from its first step.
        fresh.set_profile(Profile::driving);
        assert(fresh.key('W', false, t0) && fresh.pressed(t0) && fresh.state(t0).rt > 0);
    }
    // input.cpp tries a press on a copy first, so a press that gets no pad leaves the pad as it was.
    {
        KeyboardPad held;
        held.set_profile(Profile::shooter);
        held.key(vk::lshift, false, t0);  // L3 in shooter
        KeyboardPad probe = held;
        assert(!probe.key('M', false, t0));  // M has no control in shooter
        assert(probe.pressed(t0));           // Shift still holds L3 in the copy
        KeyboardPad other = held;
        assert(other.key('W', false, t0) && other.state(t0).ly > 0);
        assert(held.state(t0).ly == 0 && held.state(t0).buttons == moonlight::left_stick);
        // Alt first, then Ctrl: Ctrl doesn't count while left Alt (no control of shooter) is held, so Ctrl+Alt and the
        // wheel add no pad that way, even where Ctrl is a button.
        KeyboardPad watcher;
        watcher.set_profile(Profile::shooter);
        assert(!watcher.key(0xA4, false, t0) && !watcher.key(vk::lctrl, false, t0) && !watcher.pressed(t0));
        KeyboardPad ctrl_first;
        ctrl_first.set_profile(Profile::shooter);
        assert(ctrl_first.key(vk::lctrl, false, t0) && ctrl_first.pressed(t0));  // ○: Ctrl first is a press
    }

    // Wheel: each step a tap of 50 ms on the D-pad, a pause, then the next.
    pad.scroll(120, t0);
    assert(pad.state(t0).buttons == moonlight::dpad_up);
    assert(pad.next_change(t0) == t0 + WheelTaps::press);
    assert(pad.state(t0 + WheelTaps::press).buttons == 0);
    assert(!pad.next_change(t0 + WheelTaps::press));
    pad.scroll(60, t0 + 1s);  // half a step from a high-resolution wheel: nothing yet
    assert(pad.state(t0 + 1s).buttons == 0);
    pad.scroll(60, t0 + 1s);
    assert(pad.state(t0 + 1s).buttons == moonlight::dpad_up);
    pad.clear();
    pad.scroll(-240, t0);  // two steps down
    assert(pad.state(t0).buttons == moonlight::dpad_down);
    assert(pad.state(t0 + 50ms).buttons == 0);
    assert(pad.next_change(t0 + 50ms) == t0 + 100ms);
    assert(pad.state(t0 + 100ms).buttons == moonlight::dpad_down);
    assert(pad.state(t0 + 150ms).buttons == 0 && !pad.next_change(t0 + 150ms));
    pad.clear();
    pad.scroll(-120 * 3, t0);
    pad.scroll(120, t0 + 10ms);  // one back: two taps down in all
    assert(pad.state(t0 + 100ms).buttons == moonlight::dpad_down);
    assert(pad.state(t0 + 200ms).buttons == 0 && !pad.next_change(t0 + 200ms));
    pad.clear();
    pad.scroll(120 * 50, t0);  // a long spin gives at most ten taps
    int taps = 0;
    for (auto at = t0; const auto next = pad.next_change(at); at = *next) {
        assert(*next - at == 50ms);
        taps += pad.state(at).buttons == moonlight::dpad_up;
    }
    assert(taps == WheelTaps::max_pending);
    pad.clear();
    pad.hscroll(120, t0);
    assert(pad.state(t0).buttons == moonlight::dpad_right);
    pad.hscroll(-120, t0 + 60ms);
    assert(pad.state(t0 + 100ms).buttons == moonlight::dpad_left);
    pad.clear();
    // A tap that starts late still lasts 50 ms.
    pad.scroll(240, t0);
    assert(pad.state(t0 + 130ms).buttons == moonlight::dpad_up);
    assert(pad.next_change(t0 + 130ms) == t0 + 180ms);
    pad.clear();
    // The wheel and the arrows on the same D-pad axis cancel out like any opposite sides.
    pad.key(left, false);
    pad.scroll(-120, t0);
    assert(pad.state(t0).buttons == (moonlight::dpad_left | moonlight::dpad_down));
    pad.key(up, false);
    assert(pad.state(t0).buttons == moonlight::dpad_left);
    pad.clear();
    // The next change is the earliest one.
    pad.scroll(120, t0);
    pad.mouse_move(5, 0, t0 + 30ms);
    assert(pad.next_change(t0 + 30ms) == t0 + 50ms);
    assert(pad.next_change(t0 + 50ms) == t0 + 80ms);
    pad.clear();
    assert(pad.state(t0) == PadInput{} && !pad.next_change(t0));

    // Profiles: names, and a new profile lets go of what the old one held.
    for (const auto name : profile_names) assert(profile_name(*parse_profile(name)) == name);
    assert(!parse_profile("racing") && !parse_profile(""));
    pad.key('W', false);
    pad.mouse_button(1, false);
    pad.set_profile(Profile::action);
    assert(pad.profile() == Profile::action && pad.state(t0) == PadInput{});
    assert(!pad.key('W', true));  // the press belonged to the old profile and is gone

    // Action: Space jumps, Shift dodges, R uses an item, Ctrl walks; the mouse buttons attack and guard.
    assert(press(pad, space, t0) == moonlight::a && press(pad, lshift, t0) == moonlight::b && press(pad, 'R', t0) == moonlight::x);
    pad.key('W', false);
    pad.key(lctrl, false);
    assert(pad.state(t0).ly == 16384 && pad.state(t0).buttons == 0);  // walking: half the stick
    pad.key(lctrl, true);
    assert(pad.state(t0).ly == 32767);
    pad.key('W', true);
    assert(pad.mouse_button(1, false) && pad.state(t0).buttons == moonlight::right_button);
    assert(pad.mouse_button(3, false) && pad.state(t0).buttons == (moonlight::right_button | moonlight::left_button));
    pad.clear();
    assert(pad.mouse_button(4, false) && pad.state(t0).lt == 255);
    assert(pad.mouse_button(5, false) && pad.state(t0).rt == 255);
    pad.clear();
    pad.key('1', false);
    pad.key(lctrl, false);
    assert(pad.state(t0).lt == 255);  // walking leaves the triggers alone
    pad.clear();
    // Right Alt held: standard's keys, so a Moonlight shortcut finds the pad let go and presses nothing.
    pad.key(lshift, false);
    assert(pad.state(t0).buttons == moonlight::b);
    assert(pad.key(ralt, false) && pad.state(t0) == PadInput{});
    assert(!pad.key(lctrl, false) && !pad.key('Q', false) && pad.state(t0) == PadInput{});
    pad.key(ralt, true);
    pad.key(lctrl, true);
    pad.key(lshift, true);
    assert(pad.key(lshift, false) && pad.state(t0).buttons == moonlight::b);
    pad.clear();

    // Left Alt is no built-in profile's: held, Ctrl and Shift let go and don't count, for a Moonlight shortcut.
    pad.key(lshift, false);
    assert(pad.state(t0).buttons == moonlight::b);
    assert(pad.key(0xA4, false) && pad.state(t0) == PadInput{});
    assert(!pad.key(lctrl, false) && pad.state(t0) == PadInput{});
    assert(pad.key('W', false) && pad.state(t0).ly == 32767);  // other keys count
    pad.key(0xA4, true);
    pad.key(lctrl, true);
    pad.key(lshift, true);
    pad.clear();
    // The key right of the space bar held gives the menu keys in any profile: standard's, with W A S D on the D-pad,
    // which every menu takes; Caps Lock waits. Up again, the profile is back.
    pad.set_profile(Profile::driving);
    pad.key('W', false, t0);
    assert(pad.state(t0).rt == 13);
    assert(pad.key(ralt, false) && pad.state(t0) == PadInput{});
    assert(pad.key('W', false, t0) && pad.state(t0).buttons == moonlight::dpad_up && pad.state(t0).rt == 0);  // W is up
    assert(pad.key('A', false, t0) && pad.state(t0).buttons == (moonlight::dpad_up | moonlight::dpad_left));
    pad.key('A', true, t0);
    pad.key('W', true, t0);
    assert(pad.state(t0) == PadInput{} && pad.in_menu() && pad.menu_held());
    assert(press(pad, space, t0) == moonlight::a && press(pad, enter, t0) == moonlight::a);
    assert(pad.key(ralt, true) && pad.state(t0) == PadInput{});
    pad.key('W', false, t0);
    assert(pad.state(t0).rt == 13 && pad.state(t0).ly == 0);
    pad.clear();
    pad.set_profile(Profile::action);
    pad.key(ralt, false);
    assert(!pad.key(0x14, false) && !pad.in_vehicle());  // no vehicle layer while it is held
    pad.key(ralt, true);
    pad.key(0x14, false);
    assert(pad.in_vehicle());
    pad.key(ralt, false);
    pad.key('W', false, t0);
    assert(pad.state(t0).buttons == moonlight::dpad_up);  // the menu keys over the vehicle layer
    pad.key(ralt, true);
    assert(pad.in_vehicle());           // which is back after
    pad.clear();
    pad.set_profile(Profile::fighting);
    pad.key(ralt, false);
    pad.mouse_move(15, 0, t0);
    assert(pad.state(t0).rx == slow.rx);  // standard's mouse too
    pad.key(ralt, true);
    pad.clear();
    // Whatever the layout calls the key right of the space bar: VK_MENU marked extended (Windows' right Alt), the
    // right ⌘ of a Mac, 変換 or かな of a Japanese keyboard, 한/영 of a Korean one. Left Alt is not it.
    for (const uint16_t key : {uint16_t(0x5C), uint16_t(0x1C), uint16_t(0x15)}) {
        assert(pad.key(key, false, t0) && pad.menu_held());
        pad.key('W', false, t0);
        assert(pad.state(t0).buttons == moonlight::dpad_up);
        assert(pad.key(key, true, t0) && !pad.menu_held() && pad.state(t0) == PadInput{});
    }
    assert(pad.key(alt, false, t0, true) && pad.menu_held());
    assert(pad.key(alt, true, t0, true) && !pad.menu_held());
    assert(!pad.key(alt, false, t0) && !pad.menu_held());  // the side-less code unmarked: left Alt
    pad.key(alt, true, t0);
    assert(!pad.key(0xA4, false, t0) && !pad.menu_held());
    pad.key(0xA4, true, t0);
    // AltGr on Windows comes as left Ctrl and right Alt: Ctrl lets go when the menu keys begin.
    pad.set_profile(Profile::action);
    pad.key(lctrl, false, t0);
    pad.key('W', false, t0);
    assert(pad.state(t0).ly == 16384);
    assert(pad.key(ralt, false, t0) && pad.state(t0) == PadInput{});
    pad.key(ralt, true, t0);
    pad.key(lctrl, true, t0);
    pad.clear();
    // Two such keys at once: the menu keys until both are up.
    pad.key(ralt, false, t0);
    pad.key(0x5C, false, t0);
    pad.key(ralt, true, t0);
    assert(pad.menu_held());
    pad.key(0x5C, true, t0);
    assert(!pad.menu_held());
    pad.clear();
    // No game in front (the console's own screens): the menu keys too, whatever the profile, until a game is.
    pad.set_profile(Profile::driving);
    pad.set_outside_game(true);
    assert(pad.in_menu() && !pad.menu_held());
    assert(pad.key('W', false, t0) && pad.state(t0).buttons == moonlight::dpad_up && pad.state(t0).rt == 0);
    pad.set_profile(Profile::action);  // a new profile keeps it
    assert(pad.in_menu() && pad.state(t0) == PadInput{});
    pad.key('S', false, t0);
    assert(pad.state(t0).buttons == moonlight::dpad_down);
    assert(!pad.key(0x14, false, t0) && !pad.in_vehicle());  // no vehicle layer there
    pad.set_outside_game(false);
    assert(!pad.in_menu() && pad.state(t0) == PadInput{});
    pad.key('S', false, t0);
    assert(pad.state(t0).ly == -32767 && pad.state(t0).buttons == 0);
    pad.clear();
    pad.set_profile(Profile::action);

    // Shooter: Shift runs, C or Ctrl crouch, Q throws; the wheel changes weapon and its tilt is the D-pad.
    pad.set_profile(Profile::shooter);
    assert(press(pad, lshift, t0) == moonlight::left_stick && press(pad, 'C', t0) == moonlight::b);
    assert(press(pad, lctrl, t0) == moonlight::b && press(pad, 'Q', t0) == moonlight::left_button);
    assert(press(pad, 'G', t0) == moonlight::right_button && press(pad, 'V', t0) == moonlight::right_stick);
    assert(press(pad, 'E', t0) == moonlight::x && press(pad, 'F', t0) == moonlight::x && press(pad, 'R', t0) == moonlight::x);
    pad.scroll(-120, t0);
    assert(pad.state(t0).buttons == moonlight::y);
    pad.clear();
    pad.scroll(120, t0);
    assert(pad.state(t0).buttons == moonlight::y);
    pad.clear();
    pad.hscroll(-120, t0);
    assert(pad.state(t0).buttons == moonlight::dpad_left);
    pad.clear();
    pad.mouse_move(15, 0, t0);
    assert(pad.state(t0).rx == slow.rx);
    assert(pad.mouse_button(1, false) && pad.state(t0).rt == 255);
    pad.clear();

    // Driving: throttle, brake and steering ease in over 200 ms and let go at once; Ctrl halves them.
    pad.set_profile(Profile::driving);
    assert(pad.key('W', false, t0));
    const auto step = KeyboardPad::ease_step, full = KeyboardPad::ease_time - KeyboardPad::ease_step;
    assert(pad.state(t0).rt == 13 && pad.state(t0).ly == 0);  // one step in
    assert(pad.next_change(t0) == t0 + step);
    assert(pad.state(t0 + 90ms).rt == 128);
    assert(pad.state(t0 + full).rt == 255 && !pad.next_change(t0 + full));
    pad.key('D', false, t0 + full);
    assert(pad.state(t0 + full).lx == 1638);
    assert(pad.state(t0 + 2 * full).lx == 32767);
    pad.key(lctrl, false);
    assert(pad.state(t0 + 2 * full).rt == 128 && pad.state(t0 + 2 * full).lx == 16384);
    pad.key('D', true);
    pad.key('W', true);
    assert(pad.state(t0 + 2 * full) == PadInput{});
    pad.clear();
    assert(press(pad, space, t0) == moonlight::b && press(pad, lshift, t0) == moonlight::y);
    assert(press(pad, 'E', t0) == moonlight::a && press(pad, 'Q', t0) == moonlight::x && press(pad, 'F', t0) == moonlight::left_button);
    pad.mouse_move(1000, 0, t0);  // the mouse doesn't steer, nor look around
    assert(pad.state(t0) == PadInput{});
    assert(pad.mouse_button(3, false) && pad.state(t0).lt == 255);  // the mouse buttons stay throttle and brake
    pad.clear();

    // The vehicle layer: Caps Lock switches action, shooter and standard to throttle, brake and steering on W A S D,
    // the handbrake on Space and half on Ctrl, and back, letting go of everything. The other keys and the mouse stay as
    // on foot. Other profiles ignore Caps Lock.
    constexpr uint16_t caps = 0x14;
    for (const auto profile : {Profile::action, Profile::shooter, Profile::standard}) {
        pad.set_profile(profile);
        pad.key('W', false, t0);
        assert(pad.state(t0).ly == 32767);
        assert(pad.key(caps, false, t0) && pad.in_vehicle() && pad.state(t0) == PadInput{});
        assert(!pad.key(caps, true, t0) && pad.in_vehicle());  // its release does nothing
        assert(!pad.key('W', true, t0));                       // let go with the switch
        pad.key('W', false, t0);
        assert(pad.state(t0).rt == 13 && pad.state(t0).ly == 0);  // throttle, easing in
        assert(pad.state(t0 + 190ms).rt == 255);
        assert(press(pad, space, t0) == moonlight::right_button);  // handbrake, as in GTA
        pad.key('D', false, t0);
        pad.key(lctrl, false, t0);
        assert(pad.state(t0 + 190ms).lx == 16384 && pad.state(t0 + 190ms).rt == 128);  // Ctrl halves steering and throttle
        pad.key('D', true, t0);
        pad.key(lctrl, true, t0);
        assert(press(pad, 'E', t0) == static_cast<uint32_t>(key_control(profile, 'E')));  // the rest stays on foot
        assert(press(pad, enter, t0) == moonlight::a);
        assert(press(pad, 'I', t0) == 0 && pad.mouse_button(1, false) && pad.mouse_button(1, true));
        pad.mouse_move(15, 0, t0);
        assert(pad.state(t0).rx == slow.rx);                   // the camera stays on the mouse
        assert(pad.key(caps, false, t0) && !pad.in_vehicle() && pad.state(t0) == PadInput{});
        pad.key('W', false, t0);
        assert(pad.state(t0).ly == 32767 && pad.state(t0).rt == 0);  // on foot again
        pad.clear();
    }
    pad.key(caps, false, t0);
    pad.set_profile(Profile::sports);
    assert(!pad.in_vehicle());  // a new profile starts on foot
    for (const auto profile : {Profile::driving, Profile::fighting, Profile::platformer, Profile::sports}) {
        pad.set_profile(profile);
        assert(!pad.key(caps, false, t0) && !pad.in_vehicle());
    }
    pad.set_profile(Profile::action);
    pad.key(caps, false, t0);
    pad.clear();
    assert(!pad.in_vehicle());  // the end of a stream goes back on foot too

    // Fighting: W A S D and Space on the D-pad, opposite sides neutral; U I O / J K L attack; no mouse.
    pad.set_profile(Profile::fighting);
    pad.key('S', false);
    pad.key('D', false);
    assert(pad.state(t0).buttons == (moonlight::dpad_down | moonlight::dpad_right) && pad.state(t0).lx == 0);
    pad.key('A', false);
    assert(pad.state(t0).buttons == moonlight::dpad_down);
    pad.clear();
    assert(press(pad, space, t0) == moonlight::dpad_up);
    assert(press(pad, 'U', t0) == moonlight::x && press(pad, 'I', t0) == moonlight::y && press(pad, 'O', t0) == moonlight::right_button);
    assert(press(pad, 'J', t0) == moonlight::a && press(pad, 'K', t0) == moonlight::b && press(pad, 'P', t0) == moonlight::left_button);
    assert(pad.key('L', false) && pad.state(t0).rt == 255 && pad.key(0xBA, false) && pad.state(t0).lt == 255);
    pad.clear();
    assert(!pad.mouse_button(1, false));
    pad.mouse_move(1000, 0, t0);
    pad.scroll(120, t0);
    assert(pad.state(t0) == PadInput{} && !pad.next_change(t0));

    // Platformer: the arrows move, Z X C / A S D the buttons, Tab L1; no mouse.
    pad.set_profile(Profile::platformer);
    pad.key(left, false);
    assert(pad.state(t0).lx == -32767 && pad.state(t0).buttons == 0);
    pad.clear();
    assert(press(pad, 'Z', t0) == moonlight::a && press(pad, 'X', t0) == moonlight::x && press(pad, 'A', t0) == moonlight::b);
    assert(press(pad, 'D', t0) == moonlight::y && press(pad, 'E', t0) == moonlight::right_button && press(pad, tab, t0) == moonlight::left_button);
    assert(press(pad, 'I', t0) == moonlight::touchpad && !pad.key('W', false));
    assert(pad.key('C', false) && pad.state(t0).rt == 255);
    pad.clear();
    assert(!pad.mouse_button(1, false));

    // Sports: the right stick also on the numeric keypad, Shift sprints (R2), Ctrl L2, Q switches player; no mouse.
    pad.set_profile(Profile::sports);
    pad.key(0x68, false);
    pad.key(0x64, false);
    assert(pad.state(t0).ry == 32767 && pad.state(t0).rx == -32767);
    pad.clear();
    assert(press(pad, 0x65, t0) == moonlight::x && press(pad, 'Q', t0) == moonlight::left_button && press(pad, 'E', t0) == moonlight::b);
    pad.key(lshift, false);
    pad.key(lctrl, false);
    assert(pad.state(t0).rt == 255 && pad.state(t0).lt == 255);
    pad.clear();
    pad.mouse_move(1000, 0, t0);
    assert(pad.state(t0) == PadInput{});

    // A profile of the player's own: its keys and mouse buttons over its base, a key set to none doing nothing; the
    // vehicle layer still goes over it.
    auto own = std::make_shared<CustomProfile>();
    own->name = "mine";
    own->base = Profile::action;
    own->keys = {{space, moonlight::right_button}, {'Q', 0}, {'B', moonlight::x}, {0x12, pad_control::half}, {0xA4, pad_control::half}};
    own->mouse[1] = pad_control::r2;
    own->wheel[0] = own->wheel[1] = moonlight::left_button;  // both ways: L1, the next weapon in the GTA Trilogy
    pad.set_profile(Profile::action, own);
    pad.scroll(120, t0);
    assert(pad.state(t0).buttons == moonlight::left_button);
    pad.clear();
    pad.scroll(-120, t0);
    assert(pad.state(t0).buttons == moonlight::left_button);
    pad.clear();
    pad.hscroll(120, t0);
    assert(pad.state(t0).buttons == moonlight::dpad_right);  // the tilt as in action
    pad.clear();
    assert(pad.own_profile() == own.get() && pad.profile() == Profile::action);
    assert(press(pad, space, t0) == moonlight::right_button && press(pad, 'B', t0) == moonlight::x);
    assert(!pad.key('Q', false) && press(pad, 'E', t0) == moonlight::y);  // Q none; E as in action
    pad.key('W', false, t0);
    pad.key(0xA4, false, t0);  // left Alt walks, as in Elden Ring on a PC
    assert(pad.state(t0).ly == 16384);
    pad.key(lshift, false, t0);
    assert(pad.state(t0).buttons == moonlight::b);  // and Shift still counts with it
    pad.key(lshift, true, t0);
    pad.key(0xA4, true, t0);
    pad.key('W', true, t0);
    assert(pad.mouse_button(1, false) && pad.state(t0).rt == 255 && pad.mouse_button(1, true));
    pad.key(caps, false, t0);
    assert(press(pad, space, t0) == moonlight::right_button && press(pad, 'B', t0) == moonlight::x);
    pad.key('W', false, t0);
    assert(pad.state(t0).rt == 13);
    pad.set_profile(Profile::action);  // the same base without the own profile is a new profile
    assert(!pad.own_profile() && !pad.in_vehicle() && pad.state(t0) == PadInput{});
    assert(press(pad, space, t0) == moonlight::a);

    // The sensitivity's levels: 10 to 3000 in even steps, the middle where a new client starts.
    assert(sensitivity_of_level(-sensitivity_levels) == min_sensitivity && sensitivity_of_level(sensitivity_levels) == max_sensitivity);
    assert(default_sensitivity == 100 && sensitivity_of_level(0) == 100 && sensitivity_level(100) == 0);
    const double step_ratio = std::pow(double(max_sensitivity) / min_sensitivity, 1.0 / (2 * sensitivity_levels));
    for (int level = -sensitivity_levels; level <= sensitivity_levels; level++) {
        assert(sensitivity_level(sensitivity_of_level(level)) == level);
        if (level > -sensitivity_levels) {
            const double ratio = double(sensitivity_of_level(level)) / sensitivity_of_level(level - 1);
            assert(ratio > step_ratio * 0.9 && ratio < step_ratio * 1.1);  // the same step everywhere, but for rounding
        }
    }
    assert(sensitivity_level(1) == -sensitivity_levels && sensitivity_level(100000) == sensitivity_levels);

    // Ctrl+Alt with the wheel: a level a step, nothing to the pad; either Alt, within the limits.
    pad.set_profile(Profile::shooter);
    pad.set_sensitivity(default_sensitivity);
    assert(!pad.scroll(120, t0) && pad.state(t0).buttons == moonlight::y);  // without the combo the wheel is the weapon
    pad.clear();
    pad.key(lctrl, false);
    assert(!pad.scroll(120, t0));  // Ctrl alone isn't the combo: a weapon tap
    pad.clear();
    pad.key(lctrl, false);
    pad.key(0xA4, false);
    assert(pad.scroll(120, t0) && pad.sensitivity() == sensitivity_of_level(1) && pad.state(t0) == PadInput{});
    assert(pad.scroll(-240, t0) && pad.sensitivity() == sensitivity_of_level(-1));
    assert(!pad.scroll(60, t0) && pad.scroll(60, t0) && pad.sensitivity() == default_sensitivity);  // high-resolution wheels add up
    pad.set_sensitivity(sensitivity_of_level(7) + 1);
    assert(pad.scroll(120, t0) && pad.sensitivity() == sensitivity_of_level(8));  // from anywhere, the next level
    pad.key(0xA4, true);
    pad.key(ralt, false);
    assert(pad.scroll(120 * 100, t0) && pad.sensitivity() == max_sensitivity);
    assert(!pad.scroll(120, t0));  // at the top already
    pad.set_sensitivity(min_sensitivity);
    assert(!pad.scroll(-120, t0) && pad.sensitivity() == min_sensitivity);
    pad.key(ralt, true);
    pad.key(lctrl, true);
    assert(!pad.scroll(120, t0));
    pad.set_sensitivity(100);
    pad.clear();

    // The list of the sensitivities each client chose, per profile.
    const auto client = SensitivityList::client_key("-----BEGIN CERTIFICATE-----\nMIIB...");
    assert(client.size() == 16 && client != SensitivityList::client_key("another client"));
    assert(SensitivityList::client_key("") == "cbf29ce484222325");  // FNV-1a's offset basis
    SensitivityList list;
    assert(!list.find(client, "shooter"));
    list.set(client, "shooter", 180);
    list.set(client, "gta", 50000);
    list.set("0123456789abcdef", "action", 120);
    assert(list.find(client, "shooter") == 180 && list.find(client, "gta") == max_sensitivity && !list.find(client, "action"));
    const auto saved = SensitivityList::parse(list.text());
    assert(saved.find(client, "shooter") == 180 && saved.find("0123456789abcdef", "action") == 120);
    assert(saved.text() == list.text() && list.text().starts_with("# The mouse sensitivity"));
    const auto read = SensitivityList::parse("0123456789abcdef shooter = 170\r\nbad line\nshort shooter = 1\n0123456789abcdef = 5\n"
                                             "0123456789abcdef driving = fast\n# 0123456789abcdef sports = 200\n");
    assert(read.find("0123456789abcdef", "shooter") == 170 && !read.find("0123456789abcdef", "sports"));
    assert(SensitivityList::parse(read.text()).text() == read.text());

    // Settings from sunshine.conf.
    static_assert(keyboard_pad_enabled("") && keyboard_pad_enabled("1") && !keyboard_pad_enabled("0"));
    assert(keyboard_pad_sensitivity("") == default_sensitivity && keyboard_pad_sensitivity("250") == 250);
    assert(keyboard_pad_sensitivity("5") == min_sensitivity && keyboard_pad_sensitivity("50000") == max_sensitivity);
    assert(keyboard_pad_sensitivity("fast") == default_sensitivity && keyboard_pad_sensitivity("150%") == default_sensitivity);

    // Ctrl+Backspace switches the profile (the switch bit, never ○); Backspace alone stays ○, also after.
    {
        KeyboardPad sw;
        const auto t = std::chrono::steady_clock::now();
        for (const uint16_t ctrl : {uint16_t(0x11), uint16_t(0xA2), uint16_t(0xA3)}) {
            sw.set_profile(Profile::standard);
            sw.key(ctrl, false, t);
            assert(sw.key(backspace, false, t));
            assert(sw.state(t).buttons == moonlight::switch_seat);
            assert(sw.pressed(t));  // a switch also adds the pad, as any control does
            assert(sw.key(backspace, true, t));
            sw.key(ctrl, true, t);
            assert(press(sw, backspace, t) == moonlight::b);
        }
        // In shooter, Ctrl is ○ (crouch): the combination holds Ctrl's ○ and the switch, not a second ○ from Backspace.
        sw.set_profile(Profile::shooter);
        sw.key(0xA2, false, t);
        sw.key(backspace, false, t);
        assert(sw.state(t).buttons == (moonlight::b | moonlight::switch_seat));
        sw.clear();
    }

    std::cout << "PASS: keyboard pad (keys, sticks, shortcuts, mouse, wheel taps, seven profiles, easing, vehicle layer, "
                 "what adds the pad, sensitivity per client, settings, Ctrl+Backspace switches the profile)\n";
}
