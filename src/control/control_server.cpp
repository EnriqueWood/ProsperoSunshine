// SPDX-License-Identifier: GPL-3.0-only
#include "control/control_server.hpp"
#include "control/autoload_host.hpp"
#include "control/file_bridge.hpp"
#include "control/stream_list.hpp"
#include "control/http_guard.hpp"
#include "host_control.hpp"
#include "sunshine/platform_ps5.hpp"
#include "sunshine/video_ps5.hpp"
#include "web/web_login.hpp"
#include "platform/ps5/console_users.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/nvhttp.h"
#include "src/process.h"
#include "src/rtsp.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::literals;

namespace prospero {

namespace {
// A name in an answer line: Moonlight sends a device's name unchecked when it asks to pair, and a line break in it would
// add lines to the answer.
std::string one_line(std::string text) {
    for (char &c : text)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
    return text;
}

// The paired devices by client certificate: uuid and name.
std::map<std::string, std::pair<std::string, std::string>> devices_by_cert() {
    std::map<std::string, std::pair<std::string, std::string>> devices;
    for (const auto &client : nvhttp::get_all_clients()) {
        const std::string uuid = client.value("uuid", "");
        const std::string cert = nvhttp::get_cert_by_uuid(uuid);
        if (!cert.empty()) devices[cert] = {uuid, one_line(client.value("name", ""))};
    }
    return devices;
}

long long seconds_since(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point now) {
    if (start.time_since_epoch().count() == 0) return 0;  // not recorded yet
    return std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
}
}

ControlServer::ControlServer(std::shared_ptr<safe::event_t<bool>> shutdown)
    : shutdown_(std::move(shutdown)), modes_(platf::appdata() / "device_modes.txt"),
      profiles_(platf::appdata() / "device_profiles.txt") {
    nvhttp::set_launch_listener([this](const nvhttp::launch_client_t &client) {
        modes_.remember(client.uuid, format_mode(client.width, client.height, client.fps));
    });
}

ControlServer::~ControlServer() {
    nvhttp::set_launch_listener(nullptr);
}

std::string ControlServer::handle(const std::string &line, Caller caller) {
    std::lock_guard lock(mutex_);
    return handle_locked(line, caller);
}

// Once, on the first version with profiles: every device already paired keeps playing as it did, with the console asking
// who uses each controller and any profile allowed, now written down as "free" (the web page marks it risky). The marker
// file keeps it from running again, so a device paired later starts unset. Run on the first command that shows devices,
// when Sunshine has read its pairings.
void ControlServer::migrate_profiles() {
    if (profiles_migrated_) return;
    profiles_migrated_ = true;
    const auto marker = platf::appdata() / "device_profiles.migrated";
    std::error_code ec;
    if (std::filesystem::exists(marker, ec)) return;
    ProfileSet any;
    any.free = true;
    int kept = 0;
    for (const auto &client : nvhttp::get_all_clients()) {
        const std::string uuid = client.value("uuid", "");
        if (uuid.empty() || profiles_.get(uuid)) continue;
        profiles_.set(uuid, any);
        ++kept;
    }
    std::ofstream(marker) << "device profiles introduced; devices paired before kept a free choice\n";
    BOOST_LOG(info) << "Control: profiles introduced, "sv << kept << " device(s) paired before keep their free choice of profile"sv;
}

std::string ControlServer::handle_locked(const std::string &line, Caller caller) {
    auto &baseline = sent_[caller == Caller::web ? 1 : 0];
    std::istringstream in(line);
    std::string command;
    in >> command;
    if (command == "DEVICES" || command == "PROFILES" || command == "SETPROFILES") migrate_profiles();
    if (command == "STATUS") {
        const auto pending = nvhttp::get_pending_pairings().size();
        std::string answer = "OK running="s + (proc::proc.running() > 0 ? "1" : "0") +
                             " app=" + one_line(proc::proc.get_last_run_app_name()) + " pending=" + std::to_string(pending);
        // The client of the live stream; the name comes last because it may contain spaces.
        const auto client = rtsp_stream::session_count() > 0 ? nvhttp::get_last_launch_client() : nvhttp::launch_client_t{};
        return answer + " address=" + client.address + " client=" + one_line(client.name);
    }
    if (command == "PAIRINGS") {
        std::string out;
        for (const auto &p : nvhttp::get_pending_pairings()) out += "OK " + p.id + ' ' + one_line(p.name) + " " + p.address + '\n';
        return out + "END";
    }
    if (command == "PIN") {
        std::vector<std::string> args;
        for (std::string a; in >> a;) args.push_back(a);
        const auto pending = nvhttp::get_pending_pairings();
        // The name is the rest of the line, as it may hold spaces.
        const auto name_from = [&args](std::size_t first) {
            std::string joined;
            for (std::size_t i = first; i < args.size(); ++i) joined += (i > first ? " " : "") + args[i];
            return joined;
        };
        std::string id, pin, name;
        if (!args.empty() && args[0].size() <= 8) {  // PIN <pin> [name]: the only pending request
            if (pending.size() != 1) return "ERR pending=" + std::to_string(pending.size()) + ", use PIN <id> <pin>";
            id = pending.front().id;
            pin = args[0];
            name = args.size() > 1 ? name_from(1) : pending.front().name;
        } else if (args.size() >= 2) {
            id = args[0];
            pin = args[1];
            name = args.size() > 2 ? name_from(2) : "moonlight";
        } else {
            return "ERR usage: PIN <pin> [name] | PIN <id> <pin> [name]";
        }
        BOOST_LOG(info) << "Control: PIN submitted for a pending pairing"sv;
        return nvhttp::pin(id, pin, name) ? "OK paired" : "ERR pairing failed";
    }
    if (command == "CANCELPIN") {
        std::string id;
        in >> id;
        if (id.empty()) return "ERR usage: CANCELPIN <id>";
        // Moonlight gets a refusal at once instead of waiting for a PIN that never comes.
        if (!nvhttp::cancel_pairing(id)) return "ERR no such request";
        BOOST_LOG(info) << "Control: a pending pairing was denied"sv;
        return "OK cancelled";
    }
    if (command == "DEVICES") {
        // Every device with a running session streams, also one that joined another's stream.
        std::vector<std::string> live;
        for (const auto &session : rtsp_stream::session_infos()) live.push_back(session.client_cert);
        std::string out;
        for (const auto &client : nvhttp::get_all_clients()) {
            const std::string uuid = client.value("uuid", "");
            const std::string mode = modes_.mode(uuid);
            const std::string cert = live.empty() ? std::string() : nvhttp::get_cert_by_uuid(uuid);
            const bool streaming = !cert.empty() && std::find(live.begin(), live.end(), cert) != live.end();
            out += "OK " + uuid + (streaming ? " 1 " : " 0 ") + (mode.empty() ? "-" : mode) + ' ' + one_line(client.value("name", "")) +
                   '\n';
        }
        return out + "END";
    }
    if (command == "STREAMS") {
        const auto devices = devices_by_cert();
        std::vector<CaptureFacts> captures;
        for (const auto &running : video::running_captures()) {
            captures.push_back(CaptureFacts{running.channel, running.cert, running.joined, running.width, running.height, running.fps,
                                            static_cast<int>(running.average / 1000), running.video_format, running.hdr});
        }
        const auto now = std::chrono::steady_clock::now();
        std::string out;
        std::map<std::string, SentSample> sent;
        for (const auto &session : rtsp_stream::session_infos()) {
            StreamFacts facts{session.client_cert,  session.address,       session.width,
                              session.height,       session.fps,           session.bitrate,
                              session.video_format, session.dynamic_range > 0, seconds_since(session.started, now),
                              session.gamepads};
            // The rate since the last STREAMS, or on the first since the session started.
            const auto last = baseline.find(session.client_cert);
            if (last != baseline.end() && last->second.bytes <= session.video_bytes)
                facts.sent_kbps = sent_rate_kbps(session.video_bytes - last->second.bytes,
                                                 std::chrono::duration<double>(now - last->second.at).count());
            else
                facts.sent_kbps = sent_rate_kbps(session.video_bytes, std::chrono::duration<double>(now - session.started).count());
            facts.rtt_ms = session.rtt_ms > 0 ? static_cast<int>(session.rtt_ms) : -1;
            facts.recoveries = static_cast<int>(session.recoveries);
            if (!session.client_cert.empty()) sent[session.client_cert] = {session.video_bytes, now};
            const auto device = devices.find(session.client_cert);
            const bool known = device != devices.end();
            out += stream_line(facts, captures, known ? device->second.first : std::string(),
                               known ? device->second.second : std::string()) +
                   '\n';
        }
        baseline = std::move(sent);  // sessions that ended drop out
        return out + "END";
    }
    if (command == "ENDSTREAM") {
        std::string uuid;
        in >> uuid;
        if (uuid.empty()) return "ERR usage: ENDSTREAM <uuid>";
        const std::string cert = nvhttp::get_cert_by_uuid(uuid);
        if (cert.empty()) return "ERR no such device";
        const auto sessions = rtsp_stream::session_infos();
        if (std::none_of(sessions.begin(), sessions.end(), [&](const auto &s) { return s.client_cert == cert; }))
            return "ERR not streaming";
        // As a quit from that device while another streams (patch 0012): its sessions and gamepads end, the game and the
        // other devices' streams go on. It stays paired and may connect again.
        rtsp_stream::terminate_sessions_by_cert(cert);
        BOOST_LOG(info) << "Control: one device's stream was ended"sv;
        return "OK ended";
    }
    if (command == "RENAME") {
        std::string uuid, name;
        in >> uuid;
        std::getline(in >> std::ws, name);  // the rest of the line, as it may hold spaces
        if (uuid.empty() || name.empty()) return "ERR usage: RENAME <uuid> <name>";
        if (!nvhttp::rename_client(uuid, name)) return "ERR no such device";
        BOOST_LOG(info) << "Control: a paired device was renamed"sv;
        return "OK renamed";
    }
    if (command == "UNPAIR") {
        std::string uuid;
        in >> uuid;
        const std::string cert = nvhttp::get_cert_by_uuid(uuid);
        if (cert.empty() || !nvhttp::unpair_client(uuid)) return "ERR no such device";
        // Removing the pairing only stops new connections; a running stream of this device ends here.
        rtsp_stream::terminate_sessions_by_cert(cert);
        modes_.forget(uuid);
        profiles_.forget(uuid);
        DeviceSeats(platf::appdata() / "device_seats.txt").forget(uuid);
        BOOST_LOG(info) << "Control: a paired device was removed"sv;
        return "OK removed";
    }
    if (command == "USERS") {
        ps_console_user users[max_profiles];
        const int count = ps_console_users(users, static_cast<int>(max_profiles));
        if (count < 0) return "ERR the console's profiles can't be read (" + std::to_string(-count) + ")";
        std::string out;
        for (int i = 0; i < count; ++i) out += "OK " + format_user_id(users[i].id) + ' ' + one_line(users[i].name) + '\n';
        return out + "END";
    }
    if (command == "PROFILES") {
        std::string out;
        for (const auto &[uuid, set] : profiles_.all()) out += "OK " + uuid + ' ' + format_profiles(set) + '\n';
        return out + "END";
    }
    if (command == "SETPROFILES") {
        std::string uuid, text, extra;
        in >> uuid >> text;
        if (uuid.empty() || text.empty() || (in >> extra)) return "ERR usage: SETPROFILES <uuid> <profiles|unset>";
        if (nvhttp::get_cert_by_uuid(uuid).empty()) return "ERR no such device";
        if (text == "unset") {
            profiles_.set(uuid, std::nullopt);
            BOOST_LOG(info) << "Control: a paired device's profiles were unset"sv;
            return "OK saved";
        }
        const auto set = parse_profiles(text);
        if (!set) return "ERR the profiles are not valid";
        if (!set->users.empty()) {
            ps_console_user users[max_profiles];
            const int count = ps_console_users(users, static_cast<int>(max_profiles));
            if (count < 0) return "ERR the console's profiles can't be read";
            for (const uint32_t id : set->users) {
                if (std::none_of(users, users + count, [id](const ps_console_user &u) { return u.id == id; }))
                    return "ERR no profile " + format_user_id(id) + " on this console";
            }
        }
        const auto before = profiles_.get(uuid);
        profiles_.set(uuid, set);
        // A controller of the device sitting in a profile taken away leaves it and watches until a press joins it as one
        // the device still has; the stream goes on. Profiles added apply at the next press or switch.
        if (!before || !(*before == *set)) {
            // What it got beside what it had; coming from a free choice (or none set), a list takes away, adds nothing.
            // Only a device with a running session hears of them: one whose stream ended is told nothing.
            const std::string cert = nvhttp::get_cert_by_uuid(uuid);
            const auto sessions = rtsp_stream::session_infos();
            const bool streams = std::any_of(sessions.begin(), sessions.end(), [&](const auto &s) { return s.client_cert == cert; });
            std::vector<uint32_t> added;
            if (streams && before && !before->free)
                for (const uint32_t id : set->users)
                    if (std::find(before->users.begin(), before->users.end(), id) == before->users.end()) added.push_back(id);
            platf::profiles_changed(cert, added);
        }
        // No ids or names in the log: it goes into the report people post.
        BOOST_LOG(info) << "Control: a paired device's profiles were set ("sv
                        << (set->free ? "free choice"s
                            : set->none() ? "none"s
                            : set->users.empty() ? "guest only"s
                                                 : std::to_string(set->users.size()) + " profile(s)" + (set->guest ? " and guest" : ""))
                        << ')';
        return "OK saved";
    }
    if (command == "REPORT") {
        if (!report_listening_) return "ERR report port not open";
        report_until_ = std::chrono::steady_clock::now().time_since_epoch().count() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(report_window).count();
        return "OK port=" + std::to_string(report_port);
    }
    if (command == "AUTOLOAD") {
        std::string action;
        in >> action;
        if (action.empty()) return autoload_host::status();
        if (action != "ON" && action != "OFF") return "ERR usage: AUTOLOAD [ON|OFF]";
        BOOST_LOG(info) << "Control: autoload "sv << action;
        return autoload_host::set(action == "ON");
    }
    if (command == "FILE" || command == "INSTALL") {
        // For the app on the console only: not for the web page, and not over the LAN switch.
        if (!file_bridge::allowed(caller == Caller::web, client_local_)) return "ERR local clients only";
        std::string rest;
        std::getline(in >> std::ws, rest);
        return command == "FILE" ? file_bridge::file(rest) : file_bridge::install(rest == "CHECK");
    }
    if (command == "RELISTEN") {
        relisten_ = true;
        return "OK";
    }
    if (command == "WEBLOGIN") {
        std::string action, user;
        in >> action;
        if (action.empty())
            return "OK user=" + web_login::stored_user() + " enabled=" + (web_login::enabled() ? "1" : "0");
        if (action == "ON" || action == "OFF") {
            const bool on = action == "ON";
            if (!on && web_login::stored_user().empty()) return "OK off";  // off already
            const std::string problem = web_login::set_enabled(on);
            if (!problem.empty()) return "ERR " + problem;
            BOOST_LOG(info) << "Control: the web page was turned "sv << (on ? "on"sv : "off"sv);
            return on ? "OK on" : "OK off";
        }
        if (action == "FORGET") {
            web_login::forget();
            BOOST_LOG(info) << "Control: the web page's login was removed"sv;
            return "OK forgotten";
        }
        if (action != "SET") return "ERR usage: WEBLOGIN [SET <user> <password> | ON | OFF | FORGET]";
        in >> user;
        std::string password;
        std::getline(in, password);
        if (!password.empty() && password.front() == ' ') password.erase(0, 1);  // the one space after the name
        const std::string problem = web_login::set(user, password);
        if (!problem.empty()) return "ERR " + problem;
        BOOST_LOG(info) << "Control: the web page's login was set"sv;
        return "OK set";
    }
    if (command == "PROBE") {
        // The firmware probe stops the host, and with it any stream, so not while one runs. It runs as its own payload:
        // it asks STATUS again, quits this host and starts the installed one at the end (docs/firmware-probe.md).
        if (rtsp_stream::session_count() > 0) return "ERR a stream is running";
        std::string detail;
        if (!host_control::send_elf(host_control::kProbe, detail)) return "ERR " + detail;
        BOOST_LOG(info) << "Control: the firmware probe was sent to the ELF loader"sv;
        return "OK started";
    }
    if (command == "QUIT") {
        shutdown_->raise(true);
        return "OK bye";
    }
    return "ERR unknown command";
}

// The app's diagnostic report for a phone or PC. A title may not listen on the network (bind fails with
// EACCES), so the host does, and only answers while the app's page keeps asking for it.
void ControlServer::run_report(std::stop_token stop) {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return;
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(report_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof addr) || listen(listener, 4)) {
        BOOST_LOG(warning) << "Report port "sv << report_port << " failed to bind"sv;
        close(listener);
        return;
    }
    report_listening_ = true;
    while (!stop.stop_requested()) {
        pollfd pfd{listener, POLLIN, 0};
        if (poll(&pfd, 1, 250) <= 0) continue;
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            std::this_thread::sleep_for(100ms);  // no busy loop on a socket lost to rest mode
            continue;
        }
        if (std::chrono::steady_clock::now().time_since_epoch().count() < report_until_) {
            setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
            // The request does not matter; reading it keeps the close from resetting the answer.
            pollfd cfd{client, POLLIN, 0};
            char request[2048];
            if (poll(&cfd, 1, 500) > 0) (void)!recv(client, request, sizeof request, 0);
            std::ostringstream body;
            body << std::ifstream(platf::appdata() / "diagnostics.txt", std::ios::binary).rdbuf();
            const std::string text = body.str();
            const std::string answer = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " +
                                       std::to_string(text.size()) + "\r\nConnection: close\r\n\r\n" + text;
            for (size_t sent = 0; sent < answer.size();) {
                const ssize_t n = send(client, answer.data() + sent, answer.size() - sent, 0);
                if (n <= 0) break;
                sent += static_cast<size_t>(n);
            }
        }
        close(client);
    }
    close(listener);
}

void ControlServer::run(std::stop_token stop) {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return;
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    // The socket has no authentication: only the PS5 app on the console reaches it, unless the
    // development switch file exists.
    std::error_code ec;
    const bool lan = std::filesystem::exists(platf::appdata() / "control_lan", ec);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof addr) || listen(listener, 2)) {
        BOOST_LOG(error) << "Control socket failed to bind port "sv << port;
        close(listener);
        return;
    }
    BOOST_LOG(info) << "Control socket on port "sv << port << (lan ? " (LAN, development switch control_lan)"sv : " (local only)"sv);
    while (!stop.stop_requested()) {
        pollfd pfd{listener, POLLIN, 0};
        if (poll(&pfd, 1, 250) <= 0) continue;
        sockaddr_in peer{};
        socklen_t peer_size = sizeof peer;
        const int client = accept(listener, reinterpret_cast<sockaddr *>(&peer), &peer_size);
        if (client < 0) {
            std::this_thread::sleep_for(100ms);
            continue;
        }
        client_local_ = peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        std::string buffer;
        char chunk[16384];
        bool refused = false;
        // One client at a time; requests are short and rare.
        while (!stop.stop_requested()) {
            pollfd cfd{client, POLLIN, 0};
            if (poll(&cfd, 1, 250) <= 0) continue;
            const ssize_t n = recv(client, chunk, sizeof chunk, 0);
            if (n <= 0) break;
            buffer.append(chunk, static_cast<size_t>(n));
            for (size_t nl; (nl = buffer.find('\n')) != std::string::npos;) {
                std::string line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (control::http_request_line(line)) {
                    // A browser: its POST body would run as commands (http_guard.hpp). Nothing is answered.
                    BOOST_LOG(warning) << "Control socket: an HTTP request was refused, the connection closed"sv;
                    refused = true;
                    break;
                }
                const std::string answer = handle(line) + '\n';
                for (size_t sent = 0; sent < answer.size();) {
                    const ssize_t count = send(client, answer.data() + sent, answer.size() - sent, 0);
                    if (count <= 0) break;
                    sent += static_cast<size_t>(count);
                }
            }
            if (refused || buffer.size() > largest_request) break;
        }
        close(client);
    }
    close(listener);
}

}
