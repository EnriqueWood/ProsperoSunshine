// SPDX-License-Identifier: GPL-3.0-only
// Which console profiles a paired device may play as, set on the web page (at pairing, or later in Devices). A file of
// its own like device_modes.txt, <appdata>/device_profiles.txt, one "<uuid> <profiles>" a line, where <profiles> is
//   free     the device may pick any profile on the console: it can reach every profile's games and saves (risky)
//   none     no profile: the device may watch, but none of its controllers plays (0 profiles)
//   <list>   comma-separated, at least one of: a profile's user id in hex (its /user/home folder), and "guest" (Quick
//            Play, a one-time guest whose savegames aren't kept)
// A device without a line has not been set yet and plays as before this setting: the console asks for each controller.
// Portable, tested in tests/device_profiles_test.cpp.
#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace prospero {

struct ProfileSet {
    bool free = false;            // any profile; users and guest are then empty
    std::vector<uint32_t> users;  // sorted, without repeats
    bool guest = false;
    bool none() const { return !free && users.empty() && !guest; }
    bool operator==(const ProfileSet &) const = default;
};

// The console has at most 16 profiles (SCE_USER_SERVICE_MAX_REGISTER_USERS on the PS4).
inline constexpr std::size_t max_profiles = 16;

inline std::optional<uint32_t> parse_user_id(std::string_view text) {
    if (text.empty() || text.size() > 8) return std::nullopt;
    uint32_t value = 0;
    for (const char c : text) {
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return std::nullopt;
        value = value << 4 | static_cast<uint32_t>(digit);
    }
    // 0 is no user and 0xffffffff "none" for the system.
    if (value == 0 || value == 0xffffffffu) return std::nullopt;
    return value;
}

inline std::string format_user_id(uint32_t id) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text(8, '0');
    for (int i = 7; i >= 0; --i, id >>= 4) text[static_cast<std::size_t>(i)] = digits[id & 0xf];
    return text;
}

inline std::optional<ProfileSet> parse_profiles(std::string_view text) {
    ProfileSet set;
    if (text == "free") {
        set.free = true;
        return set;
    }
    if (text == "none") return set;
    if (text.empty()) return std::nullopt;
    while (true) {
        const std::size_t comma = text.find(',');
        const std::string_view item = text.substr(0, comma);
        if (item == "guest") {
            if (set.guest) return std::nullopt;
            set.guest = true;
        } else {
            const auto id = parse_user_id(item);
            if (!id || std::find(set.users.begin(), set.users.end(), *id) != set.users.end()) return std::nullopt;
            set.users.push_back(*id);
        }
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    if (set.users.size() > max_profiles) return std::nullopt;
    std::sort(set.users.begin(), set.users.end());
    return set;
}

inline std::string format_profiles(const ProfileSet &set) {
    if (set.free) return "free";
    if (set.none()) return "none";
    std::string text;
    for (const uint32_t id : set.users) text += (text.empty() ? "" : ",") + format_user_id(id);
    if (set.guest) text += text.empty() ? "guest" : ",guest";
    return text;
}

class DeviceProfiles {
public:
    explicit DeviceProfiles(std::filesystem::path file) : file_(std::move(file)) {
        std::ifstream in(file_);
        for (std::string line; std::getline(in, line);) {
            std::istringstream fields(line);
            std::string uuid, text, extra;
            if (!(fields >> uuid >> text) || (fields >> extra)) continue;
            if (const auto set = parse_profiles(text)) profiles_[uuid] = *set;
        }
    }

    // nullopt: the device has not been set (its line goes).
    void set(const std::string &uuid, const std::optional<ProfileSet> &set) {
        if (uuid.empty()) return;
        std::scoped_lock lock(mutex_);
        const auto it = profiles_.find(uuid);
        if (!set) {
            if (it == profiles_.end()) return;
            profiles_.erase(it);
        } else {
            if (it != profiles_.end() && it->second == *set) return;
            profiles_[uuid] = *set;
        }
        save();
    }

    void forget(const std::string &uuid) { set(uuid, std::nullopt); }

    std::optional<ProfileSet> get(const std::string &uuid) const {
        std::scoped_lock lock(mutex_);
        const auto it = profiles_.find(uuid);
        if (it == profiles_.end()) return std::nullopt;
        return it->second;
    }

    std::map<std::string, ProfileSet> all() const {
        std::scoped_lock lock(mutex_);
        return profiles_;
    }

private:
    void save() const {
        auto temporary = file_;
        temporary += ".new";
        {
            std::ofstream out(temporary, std::ios::trunc);
            for (const auto &[uuid, set] : profiles_) out << uuid << ' ' << format_profiles(set) << '\n';
            if (!out.flush()) return;
        }
        std::error_code ec;
        std::filesystem::rename(temporary, file_, ec);
    }

    std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::map<std::string, ProfileSet> profiles_;
};

// The profile each controller of a device last switched to (held L1+L2+R1+R2, or Ctrl+Backspace on a keyboard): the next time that
// controller joins, it starts there, while the device may still use it. <appdata>/device_seats.txt, one
// "<uuid> <controller number> <user id in hex>" a line. The controller number is Moonlight's for the device (0 to 15),
// so it only tells controllers apart by the order the device numbers them.
class DeviceSeats {
public:
    explicit DeviceSeats(std::filesystem::path file) : file_(std::move(file)) {
        std::ifstream in(file_);
        for (std::string line; std::getline(in, line);) {
            std::istringstream fields(line);
            std::string uuid, extra;
            int controller = -1;
            std::string user;
            if (!(fields >> uuid >> controller >> user) || (fields >> extra) || controller < 0 || controller > 15) continue;
            if (const auto id = parse_user_id(user)) seats_[{uuid, controller}] = *id;
        }
    }

    void remember(const std::string &uuid, int controller, uint32_t user) {
        if (uuid.empty() || controller < 0 || controller > 15 || user == 0 || user == 0xffffffffu) return;
        std::scoped_lock lock(mutex_);
        const auto key = std::make_pair(uuid, controller);
        const auto it = seats_.find(key);
        if (it != seats_.end() && it->second == user) return;
        seats_[key] = user;
        save();
    }

    // 0 when the controller has no profile it switched to.
    uint32_t get(const std::string &uuid, int controller) const {
        std::scoped_lock lock(mutex_);
        const auto it = seats_.find({uuid, controller});
        return it == seats_.end() ? 0 : it->second;
    }

    void forget(const std::string &uuid) {
        std::scoped_lock lock(mutex_);
        if (std::erase_if(seats_, [&](const auto &entry) { return entry.first.first == uuid; }) > 0) save();
    }

private:
    void save() const {
        auto temporary = file_;
        temporary += ".new";
        {
            std::ofstream out(temporary, std::ios::trunc);
            for (const auto &[key, user] : seats_) out << key.first << ' ' << key.second << ' ' << format_user_id(user) << '\n';
            if (!out.flush()) return;
        }
        std::error_code ec;
        std::filesystem::rename(temporary, file_, ec);
    }

    std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::map<std::pair<std::string, int>, uint32_t> seats_;
};

// The next profile a controller of a device switches to: after `current` in the device's list, skipping those `taken`
// by its other controllers, round to the start. 0 when there is no other one free.
template <class Taken>
uint32_t next_seat(const ProfileSet &set, uint32_t current, Taken &&taken) {
    const auto &users = set.users;
    const auto at = std::find(users.begin(), users.end(), current);
    const std::size_t start = at == users.end() ? users.size() - 1 : static_cast<std::size_t>(at - users.begin());
    for (std::size_t step = 1; step <= users.size(); ++step) {
        const uint32_t candidate = users[(start + step) % users.size()];
        if (candidate != current && !taken(candidate)) return candidate;
    }
    return 0;
}

}  // namespace prospero
