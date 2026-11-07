// SPDX-License-Identifier: GPL-3.0-only
// Which console profiles reach the console's own menus from Moonlight, chosen on the web page (Devices, Console menus).
// <appdata>/menu_profiles.txt, one profile's user id in hex a line, or the single line "none" when no profile does.
// - A controller the host seated in such a profile has PS and Create (also the keyboard pad's F1 and F2 and the PS of a
//   held Select); one seated in another profile has neither, and while no game is in front its input waits.
// - A device with at least one such profile gets "PS5 Screen" in its app list; any other device with profiles of its own
//   doesn't, and the host refuses to start it for that device.
// Without the file the menus aren't chosen and everything goes as before: PS and Create by the host's role or
// ps_button_all_players (pad_roles.hpp), "PS5 Screen" for every device. So it goes for a device that can choose freely, or
// whose profiles aren't set: which profile the console seated its controllers in isn't known.
// Portable, tested in tests/menu_access_test.cpp.
#pragma once
#include "control/device_profiles.hpp"
#include "input/gamepad_map.hpp"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace prospero {

struct MenuProfiles {
    std::vector<uint32_t> users;  // sorted, without repeats; empty: no profile reaches the menus
    bool has(uint32_t user) const { return user && std::binary_search(users.begin(), users.end(), user); }
    bool operator==(const MenuProfiles &) const = default;
};

// "none", or user ids in hex separated by commas: the control socket's and the web page's form.
inline std::optional<MenuProfiles> parse_menu_profiles(std::string_view text) {
    MenuProfiles menus;
    if (text == "none") return menus;
    if (text.empty()) return std::nullopt;
    while (true) {
        const std::size_t comma = text.find(',');
        const auto id = parse_user_id(text.substr(0, comma));
        if (!id || std::find(menus.users.begin(), menus.users.end(), *id) != menus.users.end()) return std::nullopt;
        menus.users.push_back(*id);
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    if (menus.users.size() > max_profiles) return std::nullopt;
    std::sort(menus.users.begin(), menus.users.end());
    return menus;
}

inline std::string format_menu_profiles(const MenuProfiles &menus) {
    if (menus.users.empty()) return "none";
    std::string text;
    for (const uint32_t id : menus.users) text += (text.empty() ? "" : ",") + format_user_id(id);
    return text;
}

// The file: nullopt when there is none (not chosen). A line that isn't a profile id is skipped, so a damaged file gives
// fewer profiles the menus, never more.
inline std::optional<MenuProfiles> read_menu_profiles(const std::filesystem::path &file) {
    std::ifstream in(file);
    if (!in) return std::nullopt;
    MenuProfiles menus;
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const auto id = parse_user_id(line);
        if (id && std::find(menus.users.begin(), menus.users.end(), *id) == menus.users.end()) menus.users.push_back(*id);
    }
    std::sort(menus.users.begin(), menus.users.end());
    if (menus.users.size() > max_profiles) menus.users.resize(max_profiles);
    return menus;
}

// nullopt removes the file (not chosen). Written whole, then renamed over the old one.
inline bool write_menu_profiles(const std::filesystem::path &file, const std::optional<MenuProfiles> &menus) {
    std::error_code ec;
    if (!menus) {
        std::filesystem::remove(file, ec);
        return !ec;
    }
    auto temporary = file;
    temporary += ".new";
    {
        std::ofstream out(temporary, std::ios::trunc);
        if (menus->users.empty()) out << "none\n";
        for (const uint32_t id : menus->users) out << format_user_id(id) << '\n';
        if (!out.flush()) return false;
    }
    std::filesystem::rename(temporary, file, ec);
    return !ec;
}

// What decides PS and Create for a controller of a device (its profiles, nullopt while not set) that the host seated in
// `seat` (0 when the console asked who uses it).
inline MenuAccess menu_access(const std::optional<MenuProfiles> &menus, const std::optional<ProfileSet> &device, uint32_t seat) {
    if (!menus || !device || device->free) return MenuAccess::by_role;
    return menus->has(seat) ? MenuAccess::allowed : MenuAccess::denied;
}

// Whether a device gets "PS5 Screen": the menus not chosen, a device that can choose freely or isn't set, or one of its
// profiles reaches the menus.
inline bool screen_offered(const std::optional<MenuProfiles> &menus, const std::optional<ProfileSet> &device) {
    if (!menus || !device || device->free) return true;
    return std::any_of(device->users.begin(), device->users.end(), [&](uint32_t user) { return menus->has(user); });
}

}  // namespace prospero
