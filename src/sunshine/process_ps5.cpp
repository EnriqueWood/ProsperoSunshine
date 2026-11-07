// SPDX-License-Identifier: GPL-3.0-only
// Replaces Sunshine's process.cpp. The host offers the live PS5 screen and the console's installed games. "Launching" the
// screen starts nothing, it only marks a session as running so nvhttp/stream can proceed. A game starts unless another title
// is in front. Nothing here closes a game by default, because that would lose unsaved progress: ending a session leaves it
// running. Only with close_game_on_quit in sunshine.conf does Moonlight's Terminate App close the game the session started.
#include "apps/quit_policy.hpp"
#include "apps/titles.hpp"
#include "platform/ps5/title_launch.h"
#include "sunshine/platform_ps5.hpp"
#include "src/config.h"
#include "src/logging.h"
#include "src/process.h"
#include <cstdio>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>

using namespace std::literals;

// assets/ps5_screen.png, embedded by CMakeLists.txt.
extern const unsigned char prospero_cover_png[];
extern const std::size_t prospero_cover_png_size;

namespace proc {
proc_t proc;

namespace {
constexpr int screen_app_id = 1;

std::filesystem::path cover_path() { return platf::appdata() / "assets" / "ps5_screen.png"; }

// Clients fetch box art for every app; an empty answer makes some (Moonlight Web) drop the list.
void install_cover() {
    std::error_code ec;
    const auto path = cover_path();
    if (std::filesystem::file_size(path, ec) == prospero_cover_png_size) return;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(prospero_cover_png), static_cast<std::streamsize>(prospero_cover_png_size));
}

ctx_t make_app(std::string name, int id) {
    ctx_t app{};
    app.name = std::move(name);
    app.id = std::to_string(id);
    app.elevated = false;
    app.auto_detach = true;
    app.wait_all = false;
    app.exit_timeout = 5s;
    return app;
}

// A game keeps its title ID in cmd.
std::vector<ctx_t> all_apps() {
    std::vector<ctx_t> apps;
    // Moonlight sorts by name and shows no order of the host's. The space, which it doesn't trim or show, puts this one first.
    apps.push_back(make_app(" PS5 Screen", screen_app_id));
    for (const auto &title : prospero::list_titles("/user")) {
        auto app = make_app(title.name, title.app_id);
        app.cmd = title.id;
        app.image_path = title.cover.string();
        apps.push_back(std::move(app));
    }
    return apps;
}

// The settings app ends itself when this file appears. It has to go first: a title started while it is alive once blocked
// every later start until a restart (STATUS.md).
constexpr auto settings_app = "PPSA47989"sv;
constexpr auto settings_app_quit = "/data/prosperosunshine/app.quit";
// Where a title has no /data (9.60), the app sees this one as /app0/app.quit. It may not delete it, so the host does.
constexpr auto settings_app_quit_install = "/data/homebrew/PPSA47989/app.quit";
constexpr int launch_mount_busy = static_cast<int>(0x80940033);

// For close_game_on_quit: the game the last session started or found in front, with its app for /resume, and the game
// Terminate App closed last. The HTTPS thread, the stream's end (patch 0006) and its timer (patch 0007) reach these.
std::mutex quit_mutex;
std::string session_game, closed_game;
int session_app_id = 0;
ctx_t session_app;
std::chrono::steady_clock::time_point closed_at;

// Any /mnt/sandbox/<id>_* entry, empty or not: what ShadowMountPlus waits for before it finalizes the title's exit.
bool sandbox_entry_left(const std::string &id) {
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator("/mnt/sandbox", ec)) {
        const auto name = entry.path().filename().string();
        if (name.size() > id.size() && name.starts_with(id) && name[id.size()] == '_') return true;
    }
    return false;
}

// A start of the game Terminate App just closed waits until ShadowMountPlus has finalized that exit (quit_policy.hpp).
void wait_for_release(const std::string &id) {
    std::chrono::steady_clock::time_point closed;
    {
        std::lock_guard lock(quit_mutex);
        if (id != closed_game) return;
        closed = closed_at;
    }
    using std::chrono::duration_cast, std::chrono::milliseconds;
    std::optional<std::chrono::steady_clock::time_point> gone;
    bool waited = false;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        const bool left = sandbox_entry_left(id);
        if (left) gone.reset();
        else if (!gone) gone = now;
        if (prospero::released(duration_cast<milliseconds>(now - closed), left,
                               gone ? duration_cast<milliseconds>(now - *gone) : 0ms))
            break;
        waited = true;
        std::this_thread::sleep_for(100ms);
    }
    {
        std::lock_guard lock(quit_mutex);
        if (closed_game == id && closed_at == closed) closed_game.clear();
    }
    if (waited)
        BOOST_LOG(info) << "Starting "sv << id << ' '
                        << duration_cast<milliseconds>(std::chrono::steady_clock::now() - closed).count()
                        << " ms after Terminate App closed it, once ShadowMountPlus had released it"sv;
}

bool settings_app_closed() {
    std::ofstream(settings_app_quit).flush();
    std::ofstream(settings_app_quit_install).flush();
    char running[16];
    bool closed = false;
    for (int i = 0; i < 50 && !closed; i++) {
        std::this_thread::sleep_for(100ms);
        closed = ps_title_running(running) <= 0 || settings_app != running;
    }
    // Left behind, they would close the app at its next start.
    std::remove(settings_app_quit);
    std::remove(settings_app_quit_install);
    return closed;
}

// A title other than id that is still open, from the folder the system keeps for each running title. A game opened before the
// settings app stays open behind it, but isn't the running big app once the app is gone: starting another title could close
// it. A title that is just ending gets a moment to go. Empty when /mnt/sandbox can't be read.
std::string other_open_title(const std::string &id) {
    for (int i = 0;; i++) {
        std::string open;
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator("/mnt/sandbox", ec)) {
            // <title id>_000: 9 characters, an underscore and an instance number.
            const auto name = entry.path().filename().string();
            if (name.size() != 13 || name[9] != '_') continue;
            // A folder left empty after ShadowMountPlus missed an exit belongs to no running title; the next start gets _001.
            std::error_code empty_ec;
            if (std::filesystem::is_empty(entry.path(), empty_ec)) continue;
            const auto title = name.substr(0, 9);
            if (!title.starts_with("NPXS") && title != settings_app && title != id) open = title;
        }
        if (open.empty() || i == 8) return open;
        std::this_thread::sleep_for(250ms);
    }
}

// False when another title is open and stays so: the stream would show that one, not the title the client asked for, or
// starting the new one could close it. in_front: the title is in front afterwards, as far as the system said.
bool start_title(const std::string &id, bool &in_front) {
    in_front = false;
    wait_for_release(id);
    char running[16];
    if (ps_title_running(running) > 0) {
        if (id == running) {
            BOOST_LOG(info) << "Title "sv << id << " is already in front"sv;
            in_front = true;
            return true;
        }
        if (settings_app != running || !settings_app_closed()) {
            BOOST_LOG(warning) << "Not starting "sv << id << ": "sv << running << " is in front and closing it could lose progress"sv;
            return false;
        }
    }
    if (const auto open = other_open_title(id); !open.empty()) {
        BOOST_LOG(warning) << "Not starting "sv << id << ": "sv << open << " is still open and starting another title could close it"sv;
        return false;
    }
    // Without a title the stream still shows the console.
    int rc = ps_title_launch(id.c_str());
    // ShadowMountPlus releases a title about a second after it ends, and refuses starts until then ("launch mount
    // unavailable ... Device busy" in its log). Seen right after the settings app closed itself, on firmware 10.20. Retried
    // only while nothing else is open: once ShadowMountPlus lets a start through, the system could close that title.
    for (auto until = std::chrono::steady_clock::now() + 3s;
         rc == launch_mount_busy && std::chrono::steady_clock::now() < until && other_open_title(id).empty();) {
        std::this_thread::sleep_for(500ms);
        rc = ps_title_launch(id.c_str());
    }
    if (rc < 0) BOOST_LOG(warning) << "Starting "sv << id << " failed: "sv << util::hex(rc).to_string_view();
    else BOOST_LOG(info) << "Started "sv << id;
    in_front = rc >= 0;
    return true;
}

class proc_deinit_t final : public platf::deinit_t {
public:
    ~proc_deinit_t() override { proc.terminate(); }
};
}

std::unique_ptr<platf::deinit_t> init() {
    install_cover();
    proc = proc_t({}, all_apps());
    return std::make_unique<proc_deinit_t>();
}

void refresh(const std::string &) {}

int proc_t::execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t>) {
    terminate();
    const auto app = std::find_if(_apps.begin(), _apps.end(), [&](const ctx_t &a) { return a.id == std::to_string(app_id); });
    if (app == _apps.end()) {
        BOOST_LOG(error) << "Couldn't find app with ID ["sv << app_id << ']';
        return 404;
    }
    _app_id = app_id;
    _app = *app;
    _app_launch_time = std::chrono::steady_clock::now();
    placebo = true;
    BOOST_LOG(info) << "Session app ["sv << _app.name << "] marked running"sv;
    bool in_front = false;
    if (!_app.cmd.empty() && !start_title(_app.cmd, in_front)) {
        // Refuse the launch, so the client doesn't name this title over a stream of the one in front. "PS5 Screen" still
        // streams whatever is in front. The game a session left in front stays listed: it's still the one to resume or quit.
        terminate();
        return 409;
    }
    std::lock_guard lock(quit_mutex);
    session_game = in_front ? _app.cmd : "";
    session_app_id = in_front ? app_id : 0;
    if (in_front) session_app = _app;
    return 0;
}

int proc_t::running() { return placebo ? _app_id : 0; }

void proc_t::terminate() {
    if (placebo) BOOST_LOG(info) << "Session app ["sv << _app.name << "] stopped"sv;
    placebo = false;
    _app_id = -1;
}

// With close_game_on_quit, the game a session started stays listed as running after the stream (serverinfo, /resume) while
// it is in front, as upstream keeps its app: Moonlight then offers Resume and Quit for it, and Quit sends /cancel. running()
// still turns 0 when the stream ends, which is what removes the virtual pads (patch 0006).
int game_left_running() {
    if (!config::ps5.close_game_on_quit) return 0;
    std::lock_guard lock(quit_mutex);
    if (session_game.empty()) return 0;
    char running[16] = {};
    if (ps_title_running(running) <= 0 || session_game != running) {
        session_game.clear();
        session_app_id = 0;
        return 0;
    }
    return session_app_id;
}

void proc_t::resume_left_game() {
    if (placebo || !game_left_running()) return;
    std::lock_guard lock(quit_mutex);
    if (!session_app_id) return;
    _app_id = session_app_id;
    _app = session_app;
    _app_launch_time = std::chrono::steady_clock::now();
    placebo = true;
    BOOST_LOG(info) << "Session app ["sv << _app.name << "] resumed"sv;
}

// Moonlight's Terminate App or Quit (/cancel, patch 0010), never a session's end or the host's shutdown. With
// close_game_on_quit, closes the game the session started or found in front, if it is still in front, as "Close Game" on the
// home screen does: its unsaved progress is lost. Moonlight asks first when quitting from its app list, but not for
// Ctrl+Alt+Shift+E, so turning the setting on is the consent.
void close_game_on_quit() {
    if (!config::ps5.close_game_on_quit) return;
    using std::chrono::duration_cast, std::chrono::milliseconds;
    std::string game;
    {
        std::lock_guard lock(quit_mutex);
        game.swap(session_game);
        session_app_id = 0;
    }
    char running[16] = {};
    const std::string in_front = ps_title_running(running) > 0 ? running : "";
    switch (prospero::quit_decision(true, game, in_front)) {
    case prospero::QuitDecision::off: return;
    case prospero::QuitDecision::no_game:
        BOOST_LOG(info) << "Terminate App: the session started no game, nothing to close"sv;
        return;
    case prospero::QuitDecision::not_in_front:
        BOOST_LOG(info) << "Not closing "sv << game << ": "sv << (in_front.empty() ? "nothing"s : in_front) << " is in front"sv;
        return;
    case prospero::QuitDecision::close: break;
    }
    BOOST_LOG(info) << "Closing "sv << game << " for Terminate App"sv;
    const auto start = std::chrono::steady_clock::now();
    const int rc = ps_title_close(game.c_str());
    if (rc <= 0) {
        BOOST_LOG(warning) << "Closing "sv << game << " failed: "sv
                           << (rc ? std::string(util::hex(rc).to_string_view()) : "no longer in front"s);
        return;
    }
    {
        std::lock_guard lock(quit_mutex);
        closed_game = game;
        // From the close's return, by when ShadowMountPlus has seen the exit and starts its own wait (quit_policy.hpp).
        closed_at = std::chrono::steady_clock::now();
    }
    BOOST_LOG(info) << "Closed "sv << game << " in "sv
                    << duration_cast<milliseconds>(std::chrono::steady_clock::now() - start).count() << " ms"sv;
}

// "PS5 Screen" streams the home screen, where the D-pad reaches Settings without a PS button, so a device none of whose
// profiles reaches the console's menus neither gets it in its list nor may start it (control/menu_access.hpp, patch 0023).
bool app_listed(const ctx_t &app, const std::string &client_cert) {
    return app.id != std::to_string(screen_app_id) || platf::screen_offered_to(client_cert);
}

std::string launch_refused(int app_id, const std::string &client_cert) {
    if (app_id != screen_app_id || platf::screen_offered_to(client_cert)) return {};
    BOOST_LOG(info) << "PS5 Screen refused: none of the device's profiles reaches the console's menus"sv;
    return "PS5 Screen isn't available to this device: none of its profiles reaches the console's menus. Pick a game.";
}

const std::vector<ctx_t> &proc_t::get_apps() const { return _apps; }
// nvhttp asks through this one for the client's list, so a game installed meanwhile shows up.
std::vector<ctx_t> &proc_t::get_apps() {
    _apps = all_apps();
    return _apps;
}

std::string proc_t::get_app_image(int app_id) {
    const auto app = std::find_if(_apps.begin(), _apps.end(), [&](const ctx_t &a) { return a.id == std::to_string(app_id); });
    if (app != _apps.end() && !app->image_path.empty() && std::ifstream(app->image_path).good()) return app->image_path;
    return cover_path().string();
}

std::string proc_t::get_last_run_app_name() { return _app.name.starts_with(' ') ? _app.name.substr(1) : _app.name; }

void proc_t::update_apps_and_env(proc_t &&other) { _apps = std::move(other._apps); }

proc_t::~proc_t() = default;
}
