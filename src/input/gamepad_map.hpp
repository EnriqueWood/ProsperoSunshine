// SPDX-License-Identifier: GPL-3.0-only
// Moonlight gamepad state (Xbox layout, platf::gamepad_state_t) → PS5 virtual DualSense state.
// Portable so it can be unit-tested on the build host; docs/input-mapping.md lists the table.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace prospero {

// Moonlight button bits (Sunshine's platform/common.h).
namespace moonlight {
inline constexpr uint32_t dpad_up = 0x0001, dpad_down = 0x0002, dpad_left = 0x0004, dpad_right = 0x0008;
inline constexpr uint32_t start = 0x0010, back = 0x0020, left_stick = 0x0040, right_stick = 0x0080;
inline constexpr uint32_t left_button = 0x0100, right_button = 0x0200, home = 0x0400;
inline constexpr uint32_t a = 0x1000, b = 0x2000, x = 0x4000, y = 0x8000;
inline constexpr uint32_t touchpad = 0x100000;
// Not Moonlight's (it uses bits up to 0x200000): the keyboard pad sets it for Ctrl+Backspace (keyboard_pad.hpp), which
// switches the pad to its device's next profile at once; map_gamepad never passes it on to the console.
inline constexpr uint32_t switch_seat = 0x40000000;
// L1+L2+R1+R2 held together for switch_hold does the same on a controller (device_profiles.hpp): the buttons here and
// both triggers pulled to switch_trigger. Moonlight's on-screen controller has all four but no L3 or R3. It has to be
// held, as a game may have all four down for a moment.
inline constexpr uint32_t switch_chord = left_button | right_button;
}
// Nearly all the way: a trigger half pulled in a racing game doesn't count. The on-screen controller sends 255.
inline constexpr uint8_t switch_trigger = 200;
inline constexpr std::chrono::seconds switch_hold{2};
// Every switch puts a notification on the screen of everyone streaming, and Ctrl+Backspace could fill it. So a controller
// switches freely once round its device's profiles, and then, or after a refused switch (no other profile free, only
// one profile), waits switch_cooldown, ignoring its switches meanwhile. One per controller.
inline constexpr std::chrono::seconds switch_cooldown{20};
class SwitchBudget {
public:
    using clock = std::chrono::steady_clock;
    bool allowed(clock::time_point now) const { return now >= until_; }
    // While it waits: whole seconds left, rounded up.
    long long seconds_left(clock::time_point now) const {
        if (now >= until_) return 0;
        return std::chrono::ceil<std::chrono::seconds>(until_ - now).count();
    }
    // The player is told once per wait (the first switch that comes during it), so the wait's own notice can't fill the
    // screen either.
    bool tell_once() { return !std::exchange(told_, true); }
    // round: how many profiles the device switches between; that many switches make a round.
    void switched(clock::time_point now, std::size_t round) {
        if (++count_ >= std::max<std::size_t>(round, 1)) rest(now);
    }
    void refused(clock::time_point now) { rest(now); }

private:
    void rest(clock::time_point now) {
        until_ = now + switch_cooldown;
        count_ = 0;
        told_ = false;
    }
    clock::time_point until_{};
    std::size_t count_ = 0;
    bool told_ = false;
};
constexpr bool switch_at_once(uint32_t flags) { return flags & moonlight::switch_seat; }
constexpr bool holds_switch(uint32_t flags, uint8_t lt, uint8_t rt) {
    return switch_at_once(flags) ||
           ((flags & moonlight::switch_chord) == moonlight::switch_chord && lt >= switch_trigger && rt >= switch_trigger);
}

// ScePadButton bits (src/platform/ps5/pad.h).
namespace dualsense {
inline constexpr uint32_t create = 0x1, l3 = 0x2, r3 = 0x4, options = 0x8;
inline constexpr uint32_t up = 0x10, right = 0x20, down = 0x40, left = 0x80;
inline constexpr uint32_t l2 = 0x100, r2 = 0x200, l1 = 0x400, r1 = 0x800;
inline constexpr uint32_t triangle = 0x1000, circle = 0x2000, cross = 0x4000, square = 0x8000;
inline constexpr uint32_t ps = 0x10000, touchpad = 0x100000;
}

struct PadState {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2;
};

// The host's pad plays as the console's user; every other pad is a guest the console asked about (pad_roles.hpp). Unless
// the owner allows it for all (ps_button_all_players), a guest's PS and Create don't go through, so a guest can't leave
// the game, open the control center, reach the settings or open the Create menu, which captures and broadcasts what the
// console shows. Sunshine has already turned a held Select into home here, so that way is closed too.
enum class PadRole { host, guest };
constexpr uint32_t allowed_buttons(uint32_t flags, PadRole role, bool all_players) {
    return role == PadRole::host || all_players ? flags : flags & ~(moonlight::home | moonlight::back);
}

// A trigger counts as pressed (the digital L2/R2 bit) above this analog value.
inline constexpr uint8_t trigger_threshold = 8;

// A guest's pad opens on the console only once its controller is used (GitHub issue 26): some clients announce a
// controller that never sends anything (Steam Input's virtual one next to the real one, on-screen controls), and the
// console's "Who's using this controller?" for it can only be answered by that controller, so it blocked the screen.
// A button, a trigger past the threshold or a stick pushed a quarter of the way counts; stick drift doesn't.
inline constexpr int stick_wake = 8192;
constexpr bool wakes_pad(uint32_t flags, uint8_t lt, uint8_t rt, int16_t ls_x, int16_t ls_y, int16_t rs_x, int16_t rs_y) {
    const auto far = [](int16_t v) { return v >= stick_wake || v <= -stick_wake; };
    return flags || lt >= trigger_threshold || rt >= trigger_threshold || far(ls_x) || far(ls_y) || far(rs_x) || far(rs_y);
}

// Moonlight sticks are signed 16-bit with +Y up; DualSense sticks are 0..255 with 0 at the top.
constexpr uint8_t stick_axis(int16_t value, bool invert) {
    int v = (static_cast<int>(value) + 32768) >> 8;  // 0..255
    if (invert) v = 255 - v;
    return static_cast<uint8_t>(v);
}

constexpr PadState map_gamepad(uint32_t flags, uint8_t lt, uint8_t rt, int16_t ls_x, int16_t ls_y, int16_t rs_x,
                               int16_t rs_y) {
    struct Pair { uint32_t from, to; };
    constexpr Pair table[] = {
        {moonlight::a, dualsense::cross},         {moonlight::b, dualsense::circle},
        {moonlight::x, dualsense::square},        {moonlight::y, dualsense::triangle},
        {moonlight::dpad_up, dualsense::up},      {moonlight::dpad_down, dualsense::down},
        {moonlight::dpad_left, dualsense::left},  {moonlight::dpad_right, dualsense::right},
        {moonlight::left_button, dualsense::l1},  {moonlight::right_button, dualsense::r1},
        {moonlight::left_stick, dualsense::l3},   {moonlight::right_stick, dualsense::r3},
        {moonlight::start, dualsense::options},   {moonlight::back, dualsense::create},
        {moonlight::home, dualsense::ps},         {moonlight::touchpad, dualsense::touchpad},
    };
    uint32_t buttons = 0;
    for (const auto &p : table)
        if (flags & p.from) buttons |= p.to;
    if (lt >= trigger_threshold) buttons |= dualsense::l2;
    if (rt >= trigger_threshold) buttons |= dualsense::r2;
    return {buttons, stick_axis(ls_x, false), stick_axis(ls_y, true), stick_axis(rs_x, false), stick_axis(rs_y, true),
            lt, rt};
}

// Moonlight's event flags in its trigger effect message.
inline constexpr uint8_t trigger_flag_right = 0x04, trigger_flag_left = 0x08;

// The block the console keeps for a pad: a mask, then mode and ten parameters for L2 and for R2, one spare byte
// (docs/re/adaptive-triggers.md).
using TriggerBlock = std::array<uint8_t, 24>;
inline constexpr TriggerBlock triggers_off = {0x03, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x05};

struct TriggerEffects {
    uint8_t flags;  // the triggers that differ from the last block
    uint8_t type_left, type_right;
    std::array<uint8_t, 10> left, right;
};

constexpr TriggerEffects map_trigger_effects(const TriggerBlock &block, const TriggerBlock &last) {
    TriggerEffects effects{};
    effects.type_left = block[1];
    effects.type_right = block[12];
    for (int i = 0; i < 10; ++i) {
        effects.left[i] = block[2 + i];
        effects.right[i] = block[13 + i];
    }
    for (int i = 1; i <= 11; ++i) {
        if (block[i] != last[i]) effects.flags |= trigger_flag_left;
        if (block[11 + i] != last[11 + i]) effects.flags |= trigger_flag_right;
    }
    return effects;
}

}
