// SPDX-License-Identifier: GPL-3.0-only
#include "control/menu_access.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <unistd.h>
using namespace prospero;

int main() {
    // The text forms.
    assert(parse_menu_profiles("none") && parse_menu_profiles("none")->users.empty());
    const auto two = parse_menu_profiles("1B1D9382,1b1d9381");
    assert(two && (two->users == std::vector<uint32_t>{0x1b1d9381, 0x1b1d9382}));  // sorted
    assert(two->has(0x1b1d9381) && two->has(0x1b1d9382) && !two->has(0x1b1d9383) && !two->has(0));
    assert(format_menu_profiles(*two) == "1b1d9381,1b1d9382");
    assert(format_menu_profiles(MenuProfiles{}) == "none");
    for (const char *bad : {"", ",", "1b1d9381,", ",1b1d9381", "1b1d9381,1b1d9381", "0", "ffffffff", "guest", "free",
                            "unset", "none,1b1d9381", "1b1d9381 1b1d9382"})
        assert(!parse_menu_profiles(bad));
    std::string many;
    for (int i = 1; i <= 17; ++i) many += (many.empty() ? "" : ",") + format_user_id(static_cast<uint32_t>(i));
    assert(!parse_menu_profiles(many));                            // more than the console's 16 profiles
    assert(parse_menu_profiles(many.substr(0, many.rfind(','))));  // 16 is fine

    // The file: none = not chosen; "none" = chosen, no profile; a damaged line gives fewer, never more.
    const auto dir = std::filesystem::temp_directory_path() / ("menu_access_test_" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    const auto file = dir / "menu_profiles.txt";
    assert(!read_menu_profiles(file));
    assert(write_menu_profiles(file, MenuProfiles{}));
    assert(read_menu_profiles(file) && read_menu_profiles(file)->users.empty());
    assert(write_menu_profiles(file, two));
    assert(read_menu_profiles(file) == two);
    std::ofstream(file) << "1b1d9382\r\nfree\n1b1d9381 \n\n1b1d9381\nzz\n";
    assert(read_menu_profiles(file) == two);
    std::ofstream(file) << "";
    assert(read_menu_profiles(file) && read_menu_profiles(file)->users.empty());  // an empty file: chosen, no profile
    assert(write_menu_profiles(file, std::nullopt));
    assert(!std::filesystem::exists(file) && !read_menu_profiles(file));
    assert(write_menu_profiles(file, std::nullopt));  // already gone
    std::filesystem::remove_all(dir);

    // Who decides a controller's PS and Create.
    const auto mine = parse_profiles("1b1d9381,1b1d9383");
    const auto spectator = parse_profiles("none");
    ProfileSet any;
    any.free = true;
    // Menus not chosen: the role decides, as before.
    assert(menu_access(std::nullopt, mine, 0x1b1d9381) == MenuAccess::by_role);
    // A device that can choose freely, or isn't set: the role decides too (the console seated it, which profile is unknown).
    assert(menu_access(two, any, 0) == MenuAccess::by_role);
    assert(menu_access(two, std::nullopt, 0) == MenuAccess::by_role);
    // A seated controller: its profile.
    assert(menu_access(two, mine, 0x1b1d9381) == MenuAccess::allowed);
    assert(menu_access(two, mine, 0x1b1d9383) == MenuAccess::denied);
    assert(menu_access(MenuProfiles{}, mine, 0x1b1d9381) == MenuAccess::denied);
    assert(menu_access(two, mine, 0) == MenuAccess::denied);  // never seated: no profile to go by

    // "PS5 Screen" in a device's list.
    assert(screen_offered(std::nullopt, mine) && screen_offered(std::nullopt, spectator));
    assert(screen_offered(two, any) && screen_offered(two, std::nullopt));
    assert(screen_offered(two, mine));                    // one of its profiles reaches the menus
    assert(!screen_offered(two, parse_profiles("1b1d9383")));
    assert(!screen_offered(two, spectator));
    assert(!screen_offered(two, parse_profiles("guest")));
    assert(!screen_offered(MenuProfiles{}, mine));
    return 0;
}
