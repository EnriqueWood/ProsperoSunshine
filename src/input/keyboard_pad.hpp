// SPDX-License-Identifier: GPL-3.0-only
// The keyboard and mouse of a Moonlight client without a controller (Windows virtual-key codes, Sunshine's button
// numbers, 120-step wheel) → a controller of its own, in Moonlight's gamepad state (platf::gamepad_state_t), with one
// mapping profile per kind of game. Portable so it can be unit-tested on the build host; patch 0017 feeds it from
// Sunshine's input.cpp.
#pragma once
#include "input/gamepad_map.hpp"
#include "input/keyboard_map.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prospero {

// What the pad sends: Moonlight's button bits, the triggers, and the sticks with +Y up.
struct PadInput {
    uint32_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    constexpr bool operator==(const PadInput &) const = default;
};

// What a key or mouse button does: one of Moonlight's buttons (the low bits), a trigger, a stick direction, or half
// deflection while held.
namespace pad_control {
inline constexpr uint64_t buttons = 0xFFFFFFFF;
inline constexpr uint64_t l2 = 1ull << 32, r2 = 1ull << 33;
inline constexpr uint64_t left_up = 1ull << 34, left_down = 1ull << 35, left_left = 1ull << 36, left_right = 1ull << 37;
inline constexpr uint64_t right_up = 1ull << 38, right_down = 1ull << 39, right_left = 1ull << 40, right_right = 1ull << 41;
inline constexpr uint64_t half = 1ull << 42;
}

// One profile per way of playing, so a game feels the way it does with keyboard and mouse on a PC. Every profile keeps
// Enter ✕, Backspace ○, Esc Options (pause), Tab the touchpad (map and inventory in most PS5 games), F1 PS and F2
// Create, and Ctrl+Backspace switches the pad to its device's next profile (gamepad_map.hpp, switch_seat). None
// uses Alt, so no profile can form Moonlight's Ctrl+Alt+Shift shortcuts. The key right of the space bar
// held gives the menu keys (standard's with W A S D on the D-pad), for a game's menus.
enum class Profile : uint8_t { standard, action, shooter, driving, fighting, platformer, sports };

inline constexpr std::array<std::string_view, 7> profile_names = {"standard", "action",     "shooter", "driving",
                                                                   "fighting", "platformer", "sports"};

constexpr std::string_view profile_name(Profile profile) { return profile_names[static_cast<size_t>(profile)]; }

constexpr std::optional<Profile> parse_profile(std::string_view name) {
    for (size_t i = 0; i < profile_names.size(); i++)
        if (profile_names[i] == name) return static_cast<Profile>(i);
    return std::nullopt;
}

struct KeyBinding {
    uint16_t vk;
    uint64_t control;
};

// Windows virtual-key codes, by position on a US keyboard.
namespace vk {
inline constexpr uint16_t backspace = 0x08, tab = 0x09, enter = 0x0D, shift = 0x10, ctrl = 0x11, esc = 0x1B;
inline constexpr uint16_t space = 0x20, left = 0x25, up = 0x26, right = 0x27, down = 0x28;
inline constexpr uint16_t numpad2 = 0x62, numpad4 = 0x64, numpad5 = 0x65, numpad6 = 0x66, numpad8 = 0x68;
inline constexpr uint16_t f1 = 0x70, f2 = 0x71;
inline constexpr uint16_t lshift = 0xA0, rshift = 0xA1, lctrl = 0xA2, rctrl = 0xA3, semicolon = 0xBA;
}

namespace detail {
using namespace moonlight;
using namespace pad_control;

#define PROSPERO_PAD_COMMON                                                                                            \
    {vk::enter, a}, {vk::backspace, b}, {vk::esc, start}, {vk::tab, touchpad}, {vk::f1, home}, {vk::f2, back}
#define PROSPERO_PAD_ARROWS {vk::up, dpad_up}, {vk::down, dpad_down}, {vk::left, dpad_left}, {vk::right, dpad_right}
#define PROSPERO_PAD_WASD {'W', left_up}, {'S', left_down}, {'A', left_left}, {'D', left_right}
#define PROSPERO_PAD_IJKL {'I', right_up}, {'K', right_down}, {'J', right_left}, {'L', right_right}
#define PROSPERO_PAD_SHIFT(c) {vk::shift, c}, {vk::lshift, c}, {vk::rshift, c}
#define PROSPERO_PAD_CTRL(c) {vk::ctrl, c}, {vk::lctrl, c}, {vk::rctrl, c}

// Menus, mixed games, and the default: the controls of the issue this came from.
inline constexpr KeyBinding standard_keys[] = {
    PROSPERO_PAD_COMMON, PROSPERO_PAD_ARROWS, PROSPERO_PAD_WASD, PROSPERO_PAD_IJKL,
    {vk::space, a}, {'Q', x}, {'E', y}, {'Z', left_button}, {'C', right_button}, {'1', l2}, {'3', r2},
    {'F', left_stick}, {'G', right_stick}, {'T', touchpad},
};
// Third-person action, open worlds and souls-likes (God of War, Elden Ring, Horizon): Space jumps, Shift dodges and
// runs, R uses an item, Ctrl walks, and the mouse buttons attack and guard.
inline constexpr KeyBinding action_keys[] = {
    PROSPERO_PAD_COMMON, PROSPERO_PAD_ARROWS, PROSPERO_PAD_WASD, PROSPERO_PAD_IJKL,
    {vk::space, a}, PROSPERO_PAD_SHIFT(b), {'Q', x}, {'R', x}, {'E', y}, {'Z', left_button}, {'C', right_button},
    {'1', l2}, {'3', r2}, {'F', left_stick}, {'G', right_stick}, PROSPERO_PAD_CTRL(half),
};
// Shooters (Call of Duty, Battlefield, Destiny 2, Apex, Far Cry 6, Cyberpunk): the left hand moves, runs (Shift),
// jumps (Space), crouches and throws, E, F or R interact and reload (□, one button in these games), V strikes, the mouse
// aims and fires, the wheel changes weapon (△, their one button for it).
inline constexpr KeyBinding shooter_keys[] = {
    PROSPERO_PAD_COMMON, PROSPERO_PAD_ARROWS, PROSPERO_PAD_WASD, PROSPERO_PAD_IJKL,
    {vk::space, a}, {'C', b}, PROSPERO_PAD_CTRL(b), {'R', x}, {'E', x}, {'F', x}, {'Q', left_button}, {'G', right_button},
    PROSPERO_PAD_SHIFT(left_stick), {'V', right_stick}, {'1', l2}, {'3', r2},
};
// Driving (Gran Turismo 7, F1, Need for Speed): W and S are throttle and brake, A and D steer, all three ease in;
// Space is the handbrake, Shift nitro or DRS, Q and E the gears, F looks back, Ctrl halves throttle, brake and steering.
inline constexpr KeyBinding driving_keys[] = {
    PROSPERO_PAD_COMMON, PROSPERO_PAD_ARROWS, PROSPERO_PAD_IJKL,
    {'W', r2}, {'S', l2}, {'A', left_left}, {'D', left_right}, {vk::space, b}, PROSPERO_PAD_SHIFT(y), {'E', a},
    {'Q', x}, {'F', left_button}, {'R', right_button}, {'Z', left_button}, {'C', right_button}, {'1', l2}, {'3', r2},
    PROSPERO_PAD_CTRL(half),
};
// Fighting (Street Fighter 6, Tekken 8, Mortal Kombat 1): W A S D and Space (up) on the D-pad for exact diagonals,
// U I O / J K L the attack buttons as in Tekken 8 on PC, P and ; the other shoulder buttons.
inline constexpr KeyBinding fighting_keys[] = {
    PROSPERO_PAD_COMMON, PROSPERO_PAD_ARROWS,
    {'W', dpad_up}, {vk::space, dpad_up}, {'S', dpad_down}, {'A', dpad_left}, {'D', dpad_right},
    {'U', x}, {'I', y}, {'O', right_button}, {'J', a}, {'K', b}, {'L', r2}, {'P', left_button}, {vk::semicolon, l2},
};
// 2D platformers and metroidvanias (Hollow Knight, Celeste): the arrows move, Z X C / A S D are the buttons, as in
// Hollow Knight on PC.
inline constexpr KeyBinding platformer_keys[] = {
    {vk::enter, a}, {vk::backspace, b}, {vk::esc, start}, {vk::f1, home}, {vk::f2, back},
    {vk::up, left_up}, {vk::down, left_down}, {vk::left, left_left}, {vk::right, left_right},
    {'Z', a}, {'X', x}, {'C', r2}, {'A', b}, {'S', l2}, {'D', y}, {'E', right_button}, {vk::tab, left_button},
    {'I', touchpad},
};
// Sports (EA FC, NBA 2K): the right stick also on the numeric keypad for skill moves, Shift sprints, Q switches player.
inline constexpr KeyBinding sports_keys[] = {
    PROSPERO_PAD_COMMON, PROSPERO_PAD_ARROWS, PROSPERO_PAD_WASD, PROSPERO_PAD_IJKL,
    {vk::numpad8, right_up}, {vk::numpad2, right_down}, {vk::numpad4, right_left}, {vk::numpad6, right_right},
    {vk::space, a}, {vk::numpad5, x}, {'F', x}, {'R', y}, {'E', b}, PROSPERO_PAD_SHIFT(r2), PROSPERO_PAD_CTRL(l2),
    {'Q', left_button}, {'Z', left_button}, {'C', right_button}, {'1', l2}, {'3', r2},
};

#undef PROSPERO_PAD_COMMON
#undef PROSPERO_PAD_ARROWS
#undef PROSPERO_PAD_WASD
#undef PROSPERO_PAD_IJKL
#undef PROSPERO_PAD_SHIFT
#undef PROSPERO_PAD_CTRL
}  // namespace detail

// What the wheel taps: up (away from the user), down, tilt left, tilt right.
using Wheel = std::array<uint64_t, 4>;
inline constexpr Wheel wheel_dpad = {moonlight::dpad_up, moonlight::dpad_down, moonlight::dpad_left, moonlight::dpad_right};
// The weapon in a shooter: △ is its one button that cycles weapons, the tilt stays the D-pad's sides.
inline constexpr Wheel wheel_weapon = {moonlight::y, moonlight::y, moonlight::dpad_left, moonlight::dpad_right};

// What the mouse does in a profile.
struct ProfileMouse {
    bool moves = false;                 // the movement turns the right stick
    std::array<uint64_t, 6> buttons{};  // Sunshine's BUTTON_LEFT 1 ... BUTTON_X2 5
    Wheel wheel{};
};

struct ProfileMap {
    std::span<const KeyBinding> keys;
    ProfileMouse mouse;
    bool ease = false;           // throttle, brake and steering keys ease in over ease_time
    bool half_triggers = false;  // half deflection also halves the triggers
    std::span<const KeyBinding> overlay{};  // the vehicle layer's keys, over everything
    std::span<const KeyBinding> own{};      // the keys of a profile of the player's own, over keys
};

// A profile of the player's own (keyboard_profiles.txt, input/title_profile.hpp): a profile of this file with some keys
// and mouse buttons changed. A key changed to 0 does nothing.
struct CustomProfile {
    std::string name;
    Profile base = Profile::standard;
    std::vector<KeyBinding> keys;
    std::array<std::optional<uint64_t>, 6> mouse{};  // Sunshine's buttons 1 to 5
    std::array<std::optional<uint64_t>, 4> wheel{};  // up, down, tilt left, tilt right
};

constexpr ProfileMap profile_map(Profile profile) {
    using namespace moonlight;
    using namespace pad_control;
    // left, middle, right, back (X1), forward (X2)
    constexpr ProfileMouse camera{true, {0, r2, right_stick, l2, left_button, right_button}, wheel_dpad};
    switch (profile) {
    case Profile::standard: return {detail::standard_keys, camera};
    case Profile::action:
        return {detail::action_keys, {true, {0, right_button, right_stick, left_button, l2, r2}, wheel_dpad}};
    case Profile::shooter:
        return {detail::shooter_keys, {true, {0, r2, right_stick, l2, left_button, right_button}, wheel_weapon}};
    case Profile::driving:
        return {detail::driving_keys, {false, {0, r2, right_stick, l2, left_button, right_button}, wheel_dpad}, true,
                true};
    case Profile::fighting: return {detail::fighting_keys, {}};
    case Profile::platformer: return {detail::platformer_keys, {}};
    case Profile::sports: return {detail::sports_keys, {}};
    }
    return {detail::standard_keys, camera};
}

// The vehicle layer of the profiles that walk with W A S D, for open worlds with cars (GTA, Cyberpunk, Far Cry): Caps
// Lock switches it on and off, and its light shows which is on. Only these keys change, to throttle (R2), brake (L2) and
// steering, easing in as in the driving profile, Space to the handbrake (R1, as in GTA) and Ctrl to half; every other key
// and the mouse stay as on foot, so getting out, shooting from the car and the camera are where they were. The handbrake
// and the other vehicle buttons differ between games (○ in Far Cry 6 and Days Gone), hence no more than this.
inline constexpr uint16_t vehicle_key = 0x14;  // Caps Lock

namespace detail {
// The menu keys: W A S D on the D-pad, which every menu takes (some, the console's own among them, ignore the stick).
inline constexpr KeyBinding menu_keys[] = {
    {'W', moonlight::dpad_up}, {'S', moonlight::dpad_down}, {'A', moonlight::dpad_left}, {'D', moonlight::dpad_right},
};

inline constexpr KeyBinding vehicle_keys[] = {
    {'W', pad_control::r2}, {'S', pad_control::l2}, {'A', pad_control::left_left}, {'D', pad_control::left_right},
    {vk::space, moonlight::right_button}, {vk::ctrl, pad_control::half}, {vk::lctrl, pad_control::half},
    {vk::rctrl, pad_control::half},
};
}

// The key right of the space bar, whatever the layout calls it: right Alt or AltGr (VK_RMENU, or VK_MENU marked
// extended), the right ⌘ of a Mac keyboard (VK_RWIN), 変換 or かな of a Japanese one (VK_CONVERT, VK_KANA) and 한/영 of
// a Korean one (VK_HANGUL, VK_KANA's code). Held, it gives the menu keys.
constexpr bool menu_key(uint16_t vk, bool extended) {
    return vk == 0xA5 || (vk == 0x12 && extended) || vk == 0x5C || vk == 0x1C || vk == 0x15;
}

constexpr bool has_vehicle_layer(Profile profile) {
    return profile == Profile::standard || profile == Profile::action || profile == Profile::shooter;
}

constexpr ProfileMap vehicle_map(Profile profile) {
    ProfileMap map = profile_map(profile);
    map.overlay = detail::vehicle_keys;
    map.ease = map.half_triggers = true;
    return map;
}

// What a key does with a map, 0 for a key that is not the pad's.
constexpr uint64_t key_control(const ProfileMap &map, uint16_t vk) {
    for (const auto &binding : map.overlay)
        if (binding.vk == vk) return binding.control;
    for (const auto &binding : map.own)
        if (binding.vk == vk) return binding.control;
    uint64_t control = 0;
    for (const auto &binding : map.keys)
        if (binding.vk == vk) control |= binding.control;
    return control;
}

constexpr uint64_t key_control(Profile profile, uint16_t vk) { return key_control(profile_map(profile), vk); }

constexpr uint64_t mouse_control(const ProfileMap &map, int button) {
    return button > 0 && button < static_cast<int>(map.mouse.buttons.size()) ? map.mouse.buttons[button] : 0;
}

constexpr uint64_t mouse_control(Profile profile, int button) { return mouse_control(profile_map(profile), button); }

// Moonlight for PC keeps a key pressed while Ctrl, Alt and Shift are all held for itself (quit, stats, mouse mode, paste,
// ...) and doesn't send it. Should a client send one anyway, it doesn't count for the pad; with one or two of the
// modifiers held, every key does.
constexpr bool moonlight_shortcut(uint16_t vk) {
    switch (vk) {
    case 'Q': case 'E': case 'S': case 'M': case 'C': case 'V': case 'L': case 'Z': case 'X': case 'D': case 'K':
        return true;
    default:
        return false;
    }
}

// The mouse sensitivity, a percentage of KeyboardPad::full_speed: from 10 to 1000 in sensitivity_levels steps either side
// of 100, each about 21 % more than the last, so a step feels the same anywhere. Ctrl+Alt and the wheel go a step at a
// time. Faster than 1000 % buys nothing: GTA III and Elden Ring cap how fast the right stick turns the camera.
inline constexpr int min_sensitivity = 10, max_sensitivity = 1000, sensitivity_levels = 24;

inline int sensitivity_of_level(int level) {
    const double share = static_cast<double>(std::clamp(level, -sensitivity_levels, sensitivity_levels) + sensitivity_levels) /
                         (2 * sensitivity_levels);
    return static_cast<int>(std::lround(min_sensitivity * std::pow(double(max_sensitivity) / min_sensitivity, share)));
}

// The nearest level to a percentage, -sensitivity_levels to sensitivity_levels.
inline int sensitivity_level(int percent) {
    const double share = std::log(double(std::clamp(percent, min_sensitivity, max_sensitivity)) / min_sensitivity) /
                         std::log(double(max_sensitivity) / min_sensitivity);
    return static_cast<int>(std::lround(share * 2 * sensitivity_levels)) - sensitivity_levels;
}

// Where a new client starts: the middle, 100 %.
inline const int default_sensitivity = sensitivity_of_level(0);

// sunshine.conf: keyboard_mouse_pad is on unless "0"; keyboard_mouse_sensitivity is a percentage where a new client
// starts, the middle unless set; keyboard_mouse_profile one of profile_names, standard unless set.
constexpr bool keyboard_pad_enabled(std::string_view value) { return value != "0"; }

inline int keyboard_pad_sensitivity(std::string_view value) {
    int percent = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), percent);
    if (ec != std::errc{} || end != value.data() + value.size()) return default_sensitivity;
    return std::clamp(percent, min_sensitivity, max_sensitivity);
}

// The mouse sensitivity each client chose for each profile with Ctrl+Alt and the wheel, in keyboard_sensitivity.txt next
// to sunshine.conf: "client profile = percent" a line. A client is client_key() of what identifies it to the host (its
// pairing certificate), never the certificate or its address; the profile is a profile's name or one of the player's own.
class SensitivityList {
public:
    static std::string client_key(std::string_view identity) {
        uint64_t hash = 14695981039346656037ull;  // FNV-1a
        for (const char c : identity) hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        static constexpr char digits[] = "0123456789abcdef";
        std::string key(16, '0');
        for (int i = 15; i >= 0; i--, hash >>= 4) key[i] = digits[hash & 15];
        return key;
    }

    static SensitivityList parse(std::string_view text) {
        SensitivityList list;
        while (!text.empty()) {
            const auto end = text.find('\n');
            std::string_view line = text.substr(0, end);
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
            line = line.substr(0, line.find('#'));
            const auto space = line.find(' '), equals = line.find('=');
            if (space == std::string_view::npos || equals == std::string_view::npos || space > equals) continue;
            const auto client = line.substr(0, space);
            auto profile = line.substr(space + 1, equals - space - 1);
            while (!profile.empty() && profile.back() == ' ') profile.remove_suffix(1);
            auto value = line.substr(equals + 1);
            while (!value.empty() && (value.front() == ' ')) value.remove_prefix(1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) value.remove_suffix(1);
            int percent = 0;
            const auto [at, ec] = std::from_chars(value.data(), value.data() + value.size(), percent);
            if (client.size() != 16 || profile.empty() || ec != std::errc{} || at != value.data() + value.size()) continue;
            list.set(std::string(client), std::string(profile), percent);
        }
        return list;
    }

    std::optional<int> find(std::string_view client, std::string_view profile) const {
        const auto it = entries_.find({std::string(client), std::string(profile)});
        if (it == entries_.end()) return std::nullopt;
        return it->second;
    }

    void set(std::string client, std::string profile, int percent) {
        entries_[{std::move(client), std::move(profile)}] = std::clamp(percent, min_sensitivity, max_sensitivity);
    }

    std::string text() const {
        std::string out = "# The mouse sensitivity of each client and profile, set with Ctrl+Alt and the wheel while playing.\n";
        for (const auto &[key, percent] : entries_) out += key.first + " " + key.second + " = " + std::to_string(percent) + "\n";
        return out;
    }

private:
    std::map<std::pair<std::string, std::string>, int> entries_;
};

// Each wheel step is a tap on one side of a D-pad axis, since a wheel has no press and release. Steps that come faster
// than the taps wait their turn; a step the other way takes one back.
class WheelTaps {
public:
    using clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds press{50}, gap{50};
    static constexpr int max_pending = 10;

    void add(int steps, clock::time_point now) {
        advance(now);
        pending_ = std::clamp(pending_ + steps, -max_pending, max_pending);
        advance(now);
    }

    // +1 or -1 while a tap holds that side down, else 0.
    int side(clock::time_point now) {
        advance(now);
        return pressing_ ? side_ : 0;
    }

    std::optional<clock::time_point> next_change(clock::time_point now) {
        advance(now);
        if (pressing_ || pending_) return until_;
        return std::nullopt;
    }

    void clear() { *this = {}; }

private:
    void advance(clock::time_point now) {
        while (until_ <= now) {
            if (pressing_) {
                pressing_ = false;
                until_ += gap;
            } else if (pending_) {
                side_ = pending_ > 0 ? 1 : -1;
                pending_ -= side_;
                pressing_ = true;
                until_ = now + press;
            } else {
                return;
            }
        }
    }

    int pending_ = 0;
    int side_ = 0;
    bool pressing_ = false;
    clock::time_point until_{};
};

// The keys, mouse buttons, mouse movement and wheel of one client, as a pad.
class KeyboardPad {
public:
    using clock = std::chrono::steady_clock;
    // The movement of this long ago sets the right stick, so it goes back to the centre this long after the mouse stops.
    static constexpr std::chrono::milliseconds mouse_window{50};
    // At sensitivity 100 the stick is fully deflected from this speed on.
    static constexpr double full_speed = 3000.0;  // counts per second
    // Games ignore a small deflection (their dead zone), so any movement starts this far out.
    static constexpr double lift = 0.2;
    // Throttle, brake and steering at full straight away spin the wheels and jerk the car; in the driving profile they
    // start at one step and reach full over this time, updated every step, and let go at once.
    static constexpr std::chrono::milliseconds ease_time{200}, ease_step{10};

    void set_sensitivity(int percent) { sensitivity_ = std::clamp(percent, min_sensitivity, max_sensitivity); }
    int sensitivity() const { return sensitivity_; }


    // A new profile lets everything go: a key held now means something else there. It starts on foot. own, if set, is a
    // profile of the player's own on top of profile, its base.
    void set_profile(Profile profile, std::shared_ptr<const CustomProfile> own = nullptr) {
        if (profile == profile_ && own == own_) return;
        profile_ = profile;
        own_ = std::move(own);
        clear();
    }

    const CustomProfile *own_profile() const { return own_.get(); }

    Profile profile() const { return profile_; }

    // The menu keys, standard's with W A S D on the D-pad, while the key right of the space bar is held or no game is
    // running (the home screen and the console's menus, with no game open or suspended); the vehicle layer waits meanwhile.
    bool in_menu() const { return menu_ || outside_; }
    bool menu_held() const { return menu_; }

    // No game running: the console's own screens, walked with the menu keys whatever the profile.
    void set_outside_game(bool outside) {
        if (outside == outside_) return;
        let_go();
        outside_ = outside;
        map_ = make_map();
    }
    bool in_vehicle() const { return vehicle_; }

    // Returns whether it changed what the pad holds. Keys that are not the pad's change nothing.
    // extended: the client marked the key extended (Moonlight's MODIFIER_EXTENDED), as Windows marks right Alt.
    bool key(uint16_t vk, bool release, clock::time_point now = {}, bool extended = false) {
        const auto index = static_cast<uint8_t>(vk);
        // The key right of the space bar held: the menu keys, for a game's menus in any profile. Down and up, everything
        // lets go: a key held now means something else after.
        if (menu_key(vk, extended)) {
            if (const uint8_t bit = vk_modifier(vk)) {
                uint8_t &held = vk == 0x12 ? generic_modifiers_ : sided_modifiers_;
                held = release ? held & ~(bit == 0x04 ? 0x40 : bit) : held | (bit == 0x04 ? 0x40 : bit);
            }
            menu_keys_held_[index] = !release;
            const bool menu = menu_keys_held_.any();
            if (menu == menu_) return false;
            let_go();
            menu_ = menu;
            map_ = make_map();
            return true;
        }
        if (const uint8_t bit = vk_modifier(vk)) {
            const bool generic = vk == vk::shift || vk == vk::ctrl || vk == 0x12;
            uint8_t &held = generic ? generic_modifiers_ : sided_modifiers_;
            held = release ? held & ~bit : held | bit;
            // Left Alt is no built-in profile's (some PC games use it, so a profile of one's own may): while it is held
            // and not the pad's, the player is reaching for a Moonlight shortcut (Ctrl+Alt+Shift and a key), so Ctrl and
            // Shift let go of what they hold and don't count until it is up again.
            if (bit == 0x04 && !key_control(map_, vk)) {
                if (release) return false;
                bool changed = false;
                for (int i = 0; i < 256; i++) {
                    if (held_[i] && vk_modifier(static_cast<uint16_t>(i))) {
                        held_[i] = 0;
                        changed = true;
                    }
                }
                return changed;
            }
            if (!release && (bit & 0x33) && left_alt_unbound()) return false;
        }
        if (vk == vehicle_key && has_vehicle_layer(profile_)) {
            if (release || in_menu()) return false;
            // The other layer: everything held lets go, as with a new profile.
            let_go();
            vehicle_ = !vehicle_;
            map_ = make_map();
            return true;
        }
        if (release) {
            const bool held = held_[index] != 0;
            held_[index] = 0;
            return held;
        }
        // Ctrl+Backspace switches the pad to its device's next profile (gamepad_map.hpp, switch_seat): with a Ctrl held,
        // Backspace does that instead of ○. Two keys every keyboard has without Fn, easy to tell someone on the phone.
        if (vk == vk::backspace && ((generic_modifiers_ | sided_modifiers_) & 0x11)) {
            if (held_[index]) return false;
            held_[index] = moonlight::switch_seat;
            since_[index] = now;
            return true;
        }
        const uint64_t control = key_control(map_, vk);
        if (!control || held_[index] || (all_modifiers() && moonlight_shortcut(vk))) return false;
        held_[index] = control;
        since_[index] = now;
        return true;
    }

    bool mouse_button(int button, bool release) {
        if (!mouse_control(map_, button) || buttons_[button] == !release) return false;
        buttons_[button] = !release;
        return true;
    }

    void mouse_move(int dx, int dy, clock::time_point now) {
        position_.reset();
        if (map_.mouse.moves) add_motion(dx, dy, now);
    }

    // Moonlight's absolute mode: the difference to the last position.
    void mouse_position(float x, float y, clock::time_point now) {
        const auto last = std::exchange(position_, std::pair{x, y});
        if (!last || !map_.mouse.moves) return;
        add_motion(static_cast<int>(std::lround(x - last->first)), static_cast<int>(std::lround(y - last->second)), now);
    }

    // Positive is away from the user (up) and to the right. Returns whether Ctrl+Alt made it change the sensitivity.
    bool scroll(int distance, clock::time_point now) {
        const uint8_t held = generic_modifiers_ | sided_modifiers_;
        if ((held & 0x11) && (held & 0x44)) {
            // Ctrl+Alt with the wheel: the sensitivity a level up or down, instead of the pad.
            const int steps = sensitivity_wheel_.add(distance);
            if (!steps) return false;
            const int before = sensitivity_;
            set_sensitivity(sensitivity_of_level(sensitivity_level(sensitivity_) + steps));
            return sensitivity_ != before;
        }
        if (map_.mouse.wheel[0] || map_.mouse.wheel[1]) vertical_.add(wheel_.add(distance), now);
        return false;
    }
    void hscroll(int distance, clock::time_point now) {
        if (map_.mouse.wheel[2] || map_.mouse.wheel[3]) horizontal_.add(tilt_.add(distance), now);
    }

    // movement false leaves out the mouse's movement and the wheel's taps: what the keys and mouse buttons alone hold.
    PadInput state(clock::time_point now, bool movement = true) {
        // How far each control is in, 0..1: a key eases in in the driving profile, everything else is fully in.
        std::array<double, 64> level{};
        const auto hold = [&](uint64_t control, double amount) {
            for (int bit = 0; control; bit++, control >>= 1)
                if (control & 1) level[bit] = std::max(level[bit], amount);
        };
        for (int vk = 0; vk < 256; vk++) {
            if (!held_[vk]) continue;
            double amount = 1.0;
            if (map_.ease && (held_[vk] & eased))
                amount = std::min(1.0, std::chrono::duration<double>(now - since_[vk] + ease_step) / ease_time);
            hold(held_[vk], amount);
        }
        for (int button = 0; button < static_cast<int>(buttons_.size()); button++)
            if (buttons_[button]) hold(mouse_control(map_, button), 1.0);
        if (const int side = movement ? vertical_.side(now) : 0) hold(map_.mouse.wheel[side > 0 ? 0 : 1], 1.0);
        if (const int side = movement ? horizontal_.side(now) : 0) hold(map_.mouse.wheel[side > 0 ? 3 : 2], 1.0);
        const auto in = [&](uint64_t control) { return level[std::countr_zero(control)]; };
        const bool half = in(pad_control::half) > 0;

        PadInput pad;
        for (int bit = 0; bit < 32; bit++)
            if (level[bit] > 0) pad.buttons |= 1u << bit;
        // Opposite sides of the D-pad cancel out, as on the sticks (fighting games call this neutral SOCD).
        for (const uint32_t pair : {moonlight::dpad_up | moonlight::dpad_down, moonlight::dpad_left | moonlight::dpad_right})
            if ((pad.buttons & pair) == pair) pad.buttons &= ~pair;

        const double triggers = half && map_.half_triggers ? 0.5 : 1.0;
        pad.lt = static_cast<uint8_t>(std::lround(in(pad_control::l2) * triggers * 255));
        pad.rt = static_cast<uint8_t>(std::lround(in(pad_control::r2) * triggers * 255));
        const double left = half ? 0.5 : 1.0;
        pad.lx = axis(in(pad_control::left_right) - in(pad_control::left_left), left, 0);
        pad.ly = axis(in(pad_control::left_up) - in(pad_control::left_down), left, 0);
        const auto [mx, my] = movement ? mouse_stick(now) : std::pair{0, 0};
        pad.rx = axis(in(pad_control::right_right) - in(pad_control::right_left), 1.0, mx);
        pad.ry = axis(in(pad_control::right_up) - in(pad_control::right_down), 1.0, my);
        return pad;
    }

    // Whether a key or mouse button holds one of the pad's controls, which is what adds the pad: the mouse's movement and
    // the wheel alone don't, and neither do modifiers or Ctrl's half deflection with nothing to halve.
    bool pressed(clock::time_point now) { return state(now, false) != PadInput{}; }

    // Forget the mouse's movement and the wheel's taps; what the keys and buttons hold stays.
    void drop_movement() {
        motion_count_ = 0;
        vertical_.clear();
        horizontal_.clear();
    }

    // When the state changes without input: the mouse movement leaving the window, a wheel tap starting or ending, a
    // key still easing in.
    std::optional<clock::time_point> next_change(clock::time_point now) {
        expire(now);
        std::optional<clock::time_point> next;
        const auto earliest = [&](std::optional<clock::time_point> at) {
            if (at && (!next || *at < *next)) next = at;
        };
        if (motion_count_) earliest(motion_[motion_start_].at + mouse_window);
        earliest(vertical_.next_change(now));
        earliest(horizontal_.next_change(now));
        if (map_.ease) {
            for (int vk = 0; vk < 256; vk++)
                if ((held_[vk] & eased) && now < since_[vk] + ease_time - ease_step)
                    earliest(std::min(now + ease_step, since_[vk] + ease_time - ease_step));
        }
        return next;
    }

    // Lets everything go and goes back on foot; the profile, the sensitivity and whether a game is in front stay.
    void clear() {
        const int sensitivity = sensitivity_;
        const Profile profile = profile_;
        auto own = std::move(own_);
        const bool outside = outside_;
        *this = {};
        sensitivity_ = sensitivity;
        profile_ = profile;
        own_ = std::move(own);
        outside_ = outside;
        map_ = make_map();
    }

private:
    struct Motion {
        clock::time_point at;
        int dx, dy;
    };

    static constexpr uint64_t eased = pad_control::l2 | pad_control::r2 | pad_control::left_left | pad_control::left_right;

    bool left_alt_unbound() const {
        return ((generic_modifiers_ | sided_modifiers_) & 0x04) && !key_control(map_, 0x12) && !key_control(map_, 0xA4);
    }

    // Everything held lets go, as with a new profile, but the modifiers held stay known.
    void let_go() {
        held_ = {};
        buttons_ = {};
        motion_count_ = 0;
        vertical_.clear();
        horizontal_.clear();
    }

    ProfileMap make_map() const {
        if (in_menu()) {
            ProfileMap map = profile_map(Profile::standard);
            map.overlay = detail::menu_keys;
            return map;
        }
        ProfileMap map = vehicle_ ? vehicle_map(profile_) : profile_map(profile_);
        if (own_) {
            map.own = own_->keys;
            for (size_t button = 0; button < map.mouse.buttons.size(); button++)
                if (own_->mouse[button]) map.mouse.buttons[button] = *own_->mouse[button];
            for (size_t side = 0; side < map.mouse.wheel.size(); side++)
                if (own_->wheel[side]) map.mouse.wheel[side] = *own_->wheel[side];
        }
        return map;
    }

    bool all_modifiers() const {
        const uint8_t held = generic_modifiers_ | sided_modifiers_;
        return (held & 0x11) && (held & 0x22) && (held & 0x44);  // Ctrl, Shift and Alt, either side
    }

    static int16_t axis(double keys, double scale, int from_mouse) {
        const long value = std::lround(keys * scale * 32767) + from_mouse;
        return static_cast<int16_t>(std::clamp(value, -32767L, 32767L));
    }

    void add_motion(int dx, int dy, clock::time_point now) {
        if (!dx && !dy) return;
        expire(now);
        if (motion_count_ == static_cast<int>(motion_.size())) {
            // More reports than fit in the window: the newest one takes this movement too.
            Motion &newest = motion_[(motion_start_ + motion_count_ - 1) % motion_.size()];
            newest.dx += dx;
            newest.dy += dy;
            return;
        }
        motion_[(motion_start_ + motion_count_++) % motion_.size()] = {now, dx, dy};
    }

    void expire(clock::time_point now) {
        while (motion_count_ && motion_[motion_start_].at + mouse_window <= now) {
            motion_start_ = (motion_start_ + 1) % static_cast<int>(motion_.size());
            --motion_count_;
        }
    }

    // The speed over the window, faster for more deflection up to full; the stick keeps the movement's direction.
    std::pair<int, int> mouse_stick(clock::time_point now) {
        expire(now);
        double dx = 0, dy = 0;
        for (int i = 0; i < motion_count_; i++) {
            const Motion &m = motion_[(motion_start_ + i) % motion_.size()];
            dx += m.dx;
            dy += m.dy;
        }
        const double seconds = std::chrono::duration<double>(mouse_window).count();
        const double speed = std::hypot(dx, dy) / seconds;
        if (speed == 0) return {0, 0};
        const double scaled = speed * sensitivity_ / 100.0 / full_speed;
        const double deflection = std::min(1.0, lift + (1.0 - lift) * scaled) * 32767.0;
        const double length = std::hypot(dx, dy);
        // The mouse reports +Y downwards, the stick upwards.
        return {static_cast<int>(std::lround(dx / length * deflection)), static_cast<int>(std::lround(-dy / length * deflection))};
    }

    Profile profile_ = Profile::standard;
    std::shared_ptr<const CustomProfile> own_;
    ProfileMap map_ = profile_map(Profile::standard);  // the profile's, or the vehicle layer's, with own_ on top
    bool vehicle_ = false;
    bool menu_ = false;     // the key right of the space bar is held: the menu keys
    bool outside_ = false;  // no game running: the menu keys
    std::bitset<256> menu_keys_held_;
    std::array<uint64_t, 256> held_{};  // what each held key does, fixed when it went down
    std::array<clock::time_point, 256> since_{};
    std::array<bool, 6> buttons_{};
    uint8_t generic_modifiers_ = 0, sided_modifiers_ = 0;
    std::array<Motion, 64> motion_{};
    int motion_start_ = 0, motion_count_ = 0;
    std::optional<std::pair<float, float>> position_;
    WheelSteps wheel_, tilt_, sensitivity_wheel_;
    WheelTaps vertical_, horizontal_;
    int sensitivity_ = 100;
};

}  // namespace prospero
