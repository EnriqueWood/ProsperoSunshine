// SPDX-License-Identifier: GPL-3.0-only
#include "input/keyboard_map.hpp"
#include <cassert>
#include <iostream>
using namespace prospero;

int main() {
    assert(vk_usage('A') == 0x04 && vk_usage('Z') == 0x1D);
    assert(vk_usage('1') == 0x1E && vk_usage('9') == 0x26 && vk_usage('0') == 0x27);
    assert(vk_usage(0x0D) == 0x28 && vk_usage(0x1B) == 0x29 && vk_usage(0x20) == 0x2C);
    assert(vk_usage(0x70) == 0x3A && vk_usage(0x7B) == 0x45 && vk_usage(0x87) == 0x73);
    assert(vk_usage(0x27) == 0x4F && vk_usage(0x25) == 0x50 && vk_usage(0x28) == 0x51 && vk_usage(0x26) == 0x52);
    // Print Screen and Pause open the console's menus; the rest of the keyboard doesn't (10.20).
    static_assert(opens_console_menu(vk_usage(0x2C)) && opens_console_menu(vk_usage(0x13)));
    for (const uint16_t vk : {uint16_t('A'), uint16_t('0'), uint16_t(0x0D), uint16_t(0x1B), uint16_t(0x5D), uint16_t(0x70),
                              uint16_t(0x71), uint16_t(0x87), uint16_t(0x26), uint16_t(0x24), uint16_t(0x91), uint16_t(0x03)})
        assert(!opens_console_menu(vk_usage(vk)));
    assert(!opens_console_menu(0));
    assert(vk_usage(0x60) == 0x62 && vk_usage(0x61) == 0x59 && vk_usage(0x69) == 0x61);
    assert(vk_usage(0xBF) == 0x38 && vk_usage(0xE2) == 0x64);
    assert(vk_usage(0x10) == 0 && vk_usage(0xA0) == 0 && vk_usage(0x5B) == 0);  // modifiers have no usage here
    assert(vk_usage(0xAD) == 0);  // volume mute: not forwarded
    assert(vk_modifier(0xA0) == 0x02 && vk_modifier(0x10) == 0x02 && vk_modifier(0xA1) == 0x20);
    assert(vk_modifier(0x11) == 0x01 && vk_modifier(0xA5) == 0x40 && vk_modifier(0x5C) == 0x80);
    assert(vk_modifier('A') == 0);

    assert(ascii_key('a').usage == 0x04 && !ascii_key('a').shift);
    assert(ascii_key('Q').usage == 0x14 && ascii_key('Q').shift);
    assert(ascii_key('0').usage == 0x27 && !ascii_key('0').shift);
    assert(ascii_key(')').usage == 0x27 && ascii_key(')').shift);
    assert(ascii_key('@').usage == 0x1F && ascii_key('@').shift);
    assert(ascii_key(' ').usage == 0x2C && ascii_key('\n').usage == 0x28);
    assert(ascii_key('?').usage == 0x38 && ascii_key('?').shift);
    assert(ascii_key('/').usage == 0x38 && !ascii_key('/').shift);
    assert(ascii_key('"').usage == 0x34 && ascii_key('"').shift);
    assert(ascii_key('\x7f').usage == 0 && ascii_key('\0').usage == 0);

    KeyboardState keys;
    assert(keys.empty());
    assert(keys.update('A', false));
    assert(!keys.update('A', false));  // a repeat while held
    assert(keys.update(0x27, false));
    assert(keys.count() == 2 && keys.usages()[0] == 0x04 && keys.usages()[1] == 0x4F);
    assert(keys.update('A', true));
    assert(keys.count() == 1 && keys.usages()[0] == 0x4F);
    assert(!keys.update('A', true));
    assert(!keys.update(0xAD, false));  // no usage: ignored
    assert(keys.update(0x27, true) && keys.empty());

    // Sunshine presses the generic Shift around a key that needs it while the client may hold left Shift.
    assert(keys.update(0xA0, false) && keys.modifiers() == 0x02);
    assert(!keys.update(0x10, false));
    assert(!keys.update(0x10, true) && keys.modifiers() == 0x02);
    assert(keys.update(0xA0, true) && keys.modifiers() == 0);
    assert(keys.update(0xA3, false) && keys.update(0xA4, false) && keys.modifiers() == 0x14);
    keys.clear();
    assert(keys.empty());

    for (uint16_t vk = 'A'; vk < 'A' + KeyboardState::max_keys; vk++) assert(keys.update(vk, false));
    assert(!keys.update('Z', false) && keys.count() == KeyboardState::max_keys);
    assert(keys.update('B', true) && keys.update('Z', false) && keys.usages()[KeyboardState::max_keys - 1] == 0x1D);

    assert(mouse_button_bit(1) == 0x1 && mouse_button_bit(2) == 0x4 && mouse_button_bit(3) == 0x2);
    assert(mouse_button_bit(4) == 0x8 && mouse_button_bit(5) == 0x10 && mouse_button_bit(0) == 0 && mouse_button_bit(6) == 0);

    WheelSteps wheel;
    assert(wheel.add(120) == 1);
    assert(wheel.add(-240) == -2);
    assert(wheel.add(60) == 0 && wheel.add(60) == 1);  // high-resolution wheels add up
    assert(wheel.add(-30) == 0 && wheel.add(-100) == -1);
    wheel.clear();
    assert(wheel.add(120 * 200) == 127 && wheel.add(0) == 73);

    std::cout << "PASS: keyboard and mouse map (keys, modifiers, ASCII, held keys, buttons, wheel steps)\n";
}
