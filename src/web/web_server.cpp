// SPDX-License-Identifier: GPL-3.0-only
#include "web/web_server.hpp"
#include "web/web_auth.hpp"
#include "web/web_cert.hpp"
#include "web/web_lines.hpp"
#include "web/web_plain.hpp"
#include "web/web_report.hpp"
#include "web/web_login.hpp"
#include "app/psapp/conf.hpp"
#include "app/psapp/experimental_list.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/network.h"
#include "src/platform/common.h"
#include <Simple-Web-Server/server_https.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <map>
#include <vector>
#include <atomic>
#include <functional>
#include <optional>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <ps5/kernel.h>

using namespace std::literals;

// The page's files, embedded by CMakeLists.txt from src/web/page.
struct prospero_web_asset {
    const char *path, *type;
    const unsigned char *data;
    std::size_t size;
};
extern const prospero_web_asset prospero_web_assets[];
extern const std::size_t prospero_web_asset_count;
// src/relaunch_helper.cpp, built as a payload and embedded by CMakeLists.txt.
extern const unsigned char prospero_relaunch_helper[];
extern const std::size_t prospero_relaunch_helper_size;

namespace prospero::web {

namespace {
// Simple-Web-Server's HTTPS server, which looks at a connection's first byte before the TLS handshake: a plain HTTP
// request gets a redirect to https:// (web_plain.hpp) instead of a closed connection, which browsers show as an empty
// response (issue 31). Everything else is the library's accept() as it is.
class PageServer : public SimpleWeb::Server<SimpleWeb::HTTPS> {
public:
    using SimpleWeb::Server<SimpleWeb::HTTPS>::Server;

protected:
    void accept() override {
        auto connection = create_connection(*io_service, context);
        acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const SimpleWeb::error_code &ec) {
            auto lock = connection->handler_runner->continue_lock();
            if (!lock) return;
            if (ec != SimpleWeb::error::operation_aborted) this->accept();
            if (ec) return;
            SimpleWeb::error_code option_ec;
            connection->socket->lowest_layer().set_option(boost::asio::ip::tcp::no_delay(true), option_ec);
            connection->set_timeout(config.timeout_request);
            auto first = std::make_shared<unsigned char>(0);
            connection->socket->next_layer().async_receive(
                boost::asio::buffer(first.get(), 1), boost::asio::socket_base::message_peek,
                [this, connection, first](const SimpleWeb::error_code &ec, std::size_t bytes) {
                    auto lock = connection->handler_runner->continue_lock();
                    if (!lock) return;
                    if (ec || bytes == 0) return connection->cancel_timeout();
                    if (starts_tls(*first)) return handshake(connection);
                    answer_plain(connection);
                });
        });
    }

private:
    void handshake(const std::shared_ptr<Connection> &connection) {
        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);
        connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &ec) {
            session->connection->cancel_timeout();
            auto lock = session->connection->handler_runner->continue_lock();
            if (!lock) return;
            if (!ec) this->read(session);
            else if (this->on_error) this->on_error(session->request, ec);
        });
    }

    // Reads the request head (at most 4 KiB, within the request timeout), answers and closes. The internet gets nothing,
    // as on the page itself.
    void answer_plain(const std::shared_ptr<Connection> &connection) {
        auto head = std::make_shared<boost::asio::streambuf>(4096);
        boost::asio::async_read_until(
            connection->socket->next_layer(), *head, "\r\n\r\n",
            [this, connection, head](const SimpleWeb::error_code &, std::size_t) {
                auto lock = connection->handler_runner->continue_lock();
                if (!lock) return;
                SimpleWeb::error_code ec;
                const auto remote = connection->socket->lowest_layer().remote_endpoint(ec);
                bool lan = false;
                try {
                    lan = !ec && net::from_address(net::addr_to_normalized_string(remote.address())) != net::WAN;
                } catch (...) {
                }
                if (!lan) return connection->close();
                const auto &data = head->data();
                const std::string text(boost::asio::buffers_begin(data), boost::asio::buffers_end(data));
                auto answer = std::make_shared<std::string>(plain_http_answer(text, config.port));
                log_plain();
                boost::asio::async_write(connection->socket->next_layer(), boost::asio::buffer(*answer),
                                         [connection, answer](const SimpleWeb::error_code &, std::size_t) {
                                             connection->cancel_timeout();
                                             connection->close();
                                         });
            });
    }

    // Once a minute at most: a browser may retry, and the log is read from its end.
    static void log_plain() {
        static std::atomic<int64_t> last{0};
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t before = last.load();
        if (now - before < 60 && before != 0) return;
        if (!last.compare_exchange_strong(before, now)) return;
        BOOST_LOG(info) << "Web page: a request with http:// was sent to https://"sv;
    }
};

using Server = PageServer;
using Response = std::shared_ptr<Server::Response>;
using Request = std::shared_ptr<Server::Request>;
using json = nlohmann::json;

constexpr const char *cookie_name = "pss";
// Every log is read from its end; the page shows this much.
constexpr size_t log_limit = 256 * 1024;

// "Remember me": the cookie lasts as long as the remembered token, 30 days.
const std::string remember_max_age = std::to_string(Remembered::lifetime);

// Sessions and lockout of one run of the server; its single thread is the only one that touches them. The remembered
// tokens come from their file at the start of the run, and every change goes back to it.
struct State {
    Sessions sessions;
    Remembered remembered;
    Lockout lockout;
    unsigned generation = 0;  // of the login this run serves

    bool signed_in(const std::string &token) {
        return sessions.use(token, Clock::now()) || remembered.use(token, web_login::wall_now(), web_login::token_hash);
    }
    void save_remembered() {
        if (const std::string problem = web_login::save_remembered(remembered, generation); !problem.empty())
            BOOST_LOG(warning) << "Web page: remembered sign-ins not stored: "sv << problem;
    }
};

std::string token_of(const Request &request) {
    const auto cookies = request->header.find("Cookie");
    return cookies == request->header.end() ? std::string() : cookie(cookies->second, cookie_name);
}

void send(const Response &response, SimpleWeb::StatusCode code, std::string_view body, const char *type,
          SimpleWeb::CaseInsensitiveMultimap headers = {}) {
    headers.emplace("Content-Type", type);
    if (headers.find("Cache-Control") == headers.end()) headers.emplace("Cache-Control", "no-store");
    headers.emplace("X-Content-Type-Options", "nosniff");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Referrer-Policy", "no-referrer");
    headers.emplace("Content-Security-Policy",
                    "default-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
    response->write(code, body, headers);
}

void send_json(const Response &response, const json &body, SimpleWeb::StatusCode code = SimpleWeb::StatusCode::success_ok,
               SimpleWeb::CaseInsensitiveMultimap headers = {}) {
    send(response, code, body.dump(-1, ' ', false, json::error_handler_t::replace), "application/json; charset=utf-8",
         std::move(headers));
}

void send_error(const Response &response, SimpleWeb::StatusCode code, std::string_view message) {
    send_json(response, json{{"error", message}}, code);
}

std::string address_of(const Request &request) {
    try {
        return net::addr_to_normalized_string(request->remote_endpoint().address());
    } catch (...) {
        return {};
    }
}

// The page answers the console itself, the LAN and Tailscale (100.64.0.0/10 counts as LAN in Sunshine), never the internet.
bool allowed(const Request &request) {
    const std::string address = address_of(request);
    try {
        return !address.empty() && net::from_address(address) != net::WAN;
    } catch (...) {
        return false;
    }
}

// What the lockout counts failed logins by: the address, and for IPv6 its /64, as one machine may pick many addresses in it.
std::string lockout_key(const Request &request) {
    try {
        const auto address = request->remote_endpoint().address();
        if (address.is_v6() && !address.to_v6().is_v4_mapped()) {
            auto bytes = address.to_v6().to_bytes();
            std::fill(bytes.begin() + 8, bytes.end(), 0);
            return boost::asio::ip::address_v6(bytes).to_string() + "/64";
        }
    } catch (...) {
    }
    return address_of(request);
}

std::string read_file(const std::filesystem::path &path) {
    std::ostringstream text;
    text << std::ifstream(path, std::ios::binary).rdbuf();
    return text.str();
}

// The last `limit` bytes of a file, without reading the rest: sunshine.log only rotates when the host starts.
std::string read_tail(const std::filesystem::path &path, size_t limit) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamoff size = in.tellg();
    const std::streamoff from = size > static_cast<std::streamoff>(limit) ? size - static_cast<std::streamoff>(limit) : 0;
    in.seekg(from);
    std::string text(static_cast<size_t>(size - from), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(in.gcount()));
    // From a cut start on, the first whole line.
    return from > 0 ? tail(text, text.size() - 1) : text;
}

bool write_file(const std::filesystem::path &path, const std::string &text) {
    const auto temp = std::filesystem::path(path.string() + ".web.tmp");  // the app writes through its own .tmp
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out.flush()) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    return !ec;
}

// "Restart host": the host can't send its own ELF to the loader, which would start the new host while the old one is
// still going away (on 10.20 the console hung once right then, PR 27). It writes its process id, hands the embedded
// relaunch helper to the loader, and only then stops; the helper waits until this process is gone and starts the host.
std::string start_relaunch() {
    const auto pid_file = platf::appdata() / "relaunch.pid";
    {
        std::ofstream out(pid_file, std::ios::trunc);
        out << getpid() << '\n';
        if (!out.flush()) return "cannot write " + pid_file.string();
    }
    const int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "no socket";
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    timeval timeout{5, 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9021);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string problem;
    if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) problem = "the ELF loader on port 9021 did not answer";
    for (size_t sent = 0; problem.empty() && sent < prospero_relaunch_helper_size;) {
        const ssize_t n = ::send(sock, prospero_relaunch_helper + sent, prospero_relaunch_helper_size - sent, 0);
        if (n <= 0) problem = "sending the relaunch helper to the ELF loader failed";
        else sent += static_cast<size_t>(n);
    }
    close(sock);
    if (!problem.empty()) {
        std::error_code ec;
        std::filesystem::remove(pid_file, ec);
    }
    return problem;
}

// sunshine.conf as the running host read it at its start: while the file differs, saved settings aren't applied yet.
std::string config_at_start;
bool settings_pending() { return read_file(config::sunshine.config_file) != config_at_start; }

// ps5-tailscale forwards its clients from the console itself, so they all arrive as 127.0.0.1 and share one lockout.
std::string_view forwarded(std::string_view address) {
    return address == "127.0.0.1" ? " (the console itself, or a local forward such as ps5-tailscale)"sv : ""sv;
}

std::string firmware_text() {
    const uint32_t firmware = kernel_get_fw_version();
    char text[16];
    std::snprintf(text, sizeof text, "%x.%02x", (firmware >> 24) & 0xff, (firmware >> 16) & 0xff);
    return text;
}

bool experimental(std::string_view key) {
    for (int i = 0; i < PS_EXP_SWITCH_COUNT; ++i)
        if (key == ps_exp_switch_at(i)->key) return true;
    return false;
}

// A word for the control line: letters, digits and '-' (uuids, pairing ids, PINs).
bool word(std::string_view text, size_t limit) {
    return !text.empty() && text.size() <= limit && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    });
}

// A device name: the rest of a control line, so no line breaks.
bool name_ok(std::string_view text) {
    return !text.empty() && text.size() <= 64 &&
           std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x20 && c != 0x7f; });
}

// Device lists the web page keeps in appdata, one uuid a line: web_trusted.txt, the devices the operator trusts to play as
// any profile (Devices shows them as Trusted, and the risky setup notice leaves them out), and web_risk_ack.txt, the risky
// devices the notice no longer names ("Don't show again"; Devices still marks them Risky).
std::vector<std::string> uuid_list(const char *name) {
    std::vector<std::string> uuids;
    std::istringstream in(read_file(platf::appdata() / name));
    for (std::string line; std::getline(in, line);)
        if (word(line, 64)) uuids.push_back(line);
    return uuids;
}

// Adds {"uuids": [...]} to one of those lists, or takes them out; false with the error sent.
bool change_uuid_list(const Response &response, const json &body, const char *name, bool add) {
    const auto uuids = body.find("uuids");
    if (uuids == body.end() || !uuids->is_array() || uuids->empty() || uuids->size() > 64) {
        send_error(response, SimpleWeb::StatusCode::client_error_bad_request, "no devices");
        return false;
    }
    std::vector<std::string> list = uuid_list(name);
    for (const auto &uuid : *uuids) {
        if (!uuid.is_string() || !word(uuid.get<std::string>(), 64)) {
            send_error(response, SimpleWeb::StatusCode::client_error_bad_request, "no devices");
            return false;
        }
        const auto it = std::find(list.begin(), list.end(), uuid.get<std::string>());
        if (add && it == list.end()) list.push_back(uuid.get<std::string>());
        if (!add && it != list.end()) list.erase(it);
    }
    std::string text;
    for (const auto &uuid : list) text += uuid + '\n';
    if (!write_file(platf::appdata() / name, text)) {
        send_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, std::string(name) + " could not be written");
        return false;
    }
    return true;
}

std::optional<json> body_of(const Request &request) {
    json body = json::parse(request->content.string(), nullptr, false);
    if (body.is_discarded() || !body.is_object()) return std::nullopt;
    return body;
}

std::string text_field(const json &body, const char *key) {
    const auto it = body.find(key);
    return it != body.end() && it->is_string() ? it->get<std::string>() : std::string();
}

json records(const std::vector<std::string> &lines, Record (*parse)(std::string_view)) {
    json list = json::array();
    for (const auto &line : lines) list.push_back(parse(line));
    return list;
}

Record stream_record(std::string_view line) { return fields(line, "name"); }

// A control answer as the page gets it: "OK ..." is fine, "ERR ..." a 400 with the reason.
void send_answer(const Response &response, const std::string &answer) {
    if (answer.starts_with("OK")) {
        send_json(response, json{{"ok", true}, {"message", answer.size() > 3 ? answer.substr(3) : std::string()}});
    } else {
        send_error(response, SimpleWeb::StatusCode::client_error_bad_request,
                   answer.starts_with("ERR ") ? answer.substr(4) : answer);
    }
}

json settings_json() {
    const auto config = psapp::Config::parse(read_file(config::sunshine.config_file));
    json list = json::array();
    for (const auto &spec : psapp::option_specs()) {
        if (experimental(spec.key)) continue;  // the Experimental switches stay in the app, with its firmware checks
        json choices = json::array();
        for (const auto &choice : spec.choices) choices.push_back({{"value", choice.value}, {"label", choice.label}});
        const auto value = psapp::option_value(config, spec);
        const char *kinds[] = {"text", "choice", "number", "toggle"};
        list.push_back({{"key", spec.key},
                        {"label", spec.label},
                        {"help", spec.help},
                        {"kind", kinds[static_cast<int>(spec.kind)]},
                        {"default", spec.default_label},
                        {"choices", choices},
                        {"min", spec.min},
                        {"max", spec.max},
                        {"step", spec.step},
                        {"value", value ? json(*value) : json(nullptr)}});
    }
    return list;
}

// {"values": {"key": "value" or null}}: null, and a toggle's "0", remove the key so the host's default applies.
std::string save_settings(const json &body) {
    const auto values = body.find("values");
    if (values == body.end() || !values->is_object()) return "no values";
    const std::filesystem::path path = config::sunshine.config_file;
    auto config = psapp::Config::parse(read_file(path));
    for (const auto &[key, value] : values->items()) {
        const psapp::OptionSpec *spec = psapp::find_option(key);
        if (!spec || experimental(key)) return key + " is not a setting of the web page";
        if (value.is_null()) {
            config.set(key, std::nullopt);
            continue;
        }
        if (!value.is_string()) return key + " is not text";
        const std::string text = value.get<std::string>();
        if (const auto problem = psapp::validate(*spec, text)) return std::string(spec->label) + " " + *problem;
        if (spec->kind == psapp::OptionKind::toggle && text == "0") config.set(key, std::nullopt);
        else config.set(key, text);
    }
    if (!write_file(path, config.text())) return "sunshine.conf could not be written";
    BOOST_LOG(info) << "Web page: settings saved"sv;
    return {};
}

void routes(Server &server, State &state, ControlServer &control, const std::shared_ptr<safe::event_t<bool>> &shutdown) {
    using SimpleWeb::StatusCode;
    // Every request goes through here: the address check, and for the API the session (and for a POST the header a
    // page from another site can't send without asking first).
    const auto guard = [&state](const Request &request, const Response &response, bool api, bool post) {
        if (!allowed(request)) {
            send(response, StatusCode::client_error_forbidden, "Only the local network and Tailscale reach this page.\n",
                 "text/plain; charset=utf-8");
            return false;
        }
        if (!api) return true;
        if (post && request->header.find("X-Prospero") == request->header.end()) {
            send_error(response, StatusCode::client_error_forbidden, "missing request header");
            return false;
        }
        if (!state.signed_in(token_of(request))) {
            send_error(response, StatusCode::client_error_unauthorized, "not logged in");
            return false;
        }
        return true;
    };
    const auto get = [&](const char *path, std::function<void(const Response &, const Request &)> handler) {
        server.resource[path]["GET"] = [guard, handler](Response response, Request request) {
            if (guard(request, response, true, false)) handler(response, request);
        };
    };
    const auto post = [&](const char *path, std::function<void(const Response &, const json &)> handler) {
        server.resource[path]["POST"] = [guard, handler](Response response, Request request) {
            if (!guard(request, response, true, true)) return;
            const auto body = body_of(request);
            if (!body) return send_error(response, StatusCode::client_error_bad_request, "the request is not a JSON object");
            handler(response, *body);
        };
    };

    // The page's files.
    server.default_resource["GET"] = [guard](Response response, Request request) {
        if (!guard(request, response, false, false)) return;
        const std::string path = request->path == "/" ? "/index.html" : request->path;
        for (size_t i = 0; i < prospero_web_asset_count; ++i) {
            const auto &asset = prospero_web_assets[i];
            if (path == asset.path)
                return send(response, StatusCode::success_ok,
                            std::string_view(reinterpret_cast<const char *>(asset.data), asset.size), asset.type);
        }
        send(response, StatusCode::client_error_not_found, "Not found\n", "text/plain; charset=utf-8");
    };

    // Login and logout.
    server.resource["^/api/login$"]["POST"] = [&state](Response response, Request request) {
        if (!allowed(request)) return send_error(response, StatusCode::client_error_forbidden, "not from this network");
        if (request->header.find("X-Prospero") == request->header.end())
            return send_error(response, StatusCode::client_error_forbidden, "missing request header");
        const std::string address = address_of(request);
        const std::string key = lockout_key(request);
        const auto now = Clock::now();
        if (const auto wait = state.lockout.wait(key, now); wait.count() > 0) {
            return send_json(response, json{{"error", "too many failed logins"}, {"wait", wait.count()}},
                             StatusCode::client_error_too_many_requests);
        }
        const auto body = body_of(request);
        const std::string user = body ? text_field(*body, "user") : std::string();
        const std::string password = body ? text_field(*body, "password") : std::string();
        bool remember = false;  // "Remember me on this device"
        if (body) {
            const auto it = body->find("remember");
            remember = it != body->end() && it->is_boolean() && it->get<bool>();
        }
        if (!web_login::verify(user, password)) {
            state.lockout.failed(key, now);
            BOOST_LOG(warning) << "Web page: failed login from "sv << address << forwarded(address);
            return send_error(response, StatusCode::client_error_unauthorized, "wrong user name or password");
        }
        state.lockout.succeeded(key);
        const std::string token = web_login::random_token();
        if (token.empty()) return send_error(response, StatusCode::server_error_internal_server_error, "no random numbers");
        std::string set_cookie = std::string(cookie_name) + "=" + token + "; Path=/; Secure; HttpOnly; SameSite=Strict";
        if (remember) {
            // Kept as a hash in a file, so it outlasts a restart of the host; the cookie lasts as long.
            state.remembered.add(web_login::token_hash(token), web_login::wall_now());
            state.save_remembered();
            set_cookie += "; Max-Age=" + remember_max_age;
        } else {
            state.sessions.add(token, now);
        }
        BOOST_LOG(info) << "Web page: login from "sv << address << forwarded(address) << (remember ? " (remembered for 30 days)"sv : ""sv);
        send_json(response, json{{"user", user}}, StatusCode::success_ok, {{"Set-Cookie", set_cookie}});
    };
    // Signing out also forgets every remembered browser (a lost phone, say), as long as it comes from one signed in.
    server.resource["^/api/logout$"]["POST"] = [&state](Response response, Request request) {
        const std::string token = token_of(request);
        if (request->header.find("X-Prospero") != request->header.end() && state.signed_in(token)) {
            state.sessions.remove(token);
            if (state.remembered.size() > 0) {
                state.remembered.clear();
                state.save_remembered();
                BOOST_LOG(info) << "Web page: signed out, remembered browsers forgotten"sv;
            }
        }
        send_json(response, json{{"ok", true}}, StatusCode::success_ok,
                  {{"Set-Cookie", std::string(cookie_name) + "=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0"}});
    };
    get("^/api/session$", [](const Response &response, const Request &) {
        send_json(response, json{{"user", web_login::user()}, {"settings_pending", settings_pending()}});
    });

    // What the host is doing: its status, pending pairings, paired devices and running streams.
    get("^/api/overview$", [&control](const Response &response, const Request &) {
        using Caller = ControlServer::Caller;
        const auto status_lines = ok_lines(control.handle("STATUS", Caller::web));
        json host{{"version", PROJECT_VERSION},
                  {"commit", PROJECT_VERSION_COMMIT},
                  {"firmware", firmware_text()},
                  {"name", config::nvhttp.sunshine_name},
                  {"port", net::map_port(0)},
                  {"settings_pending", settings_pending()}};
        // Each device with the profiles it may play as: null while they are not set.
        std::map<std::string, std::string> profiles;
        for (const auto &line : ok_lines(control.handle("PROFILES", Caller::web))) {
            const auto record = device_profiles(line);
            profiles[record.at("uuid")] = record.at("profiles");
        }
        json devices = records(ok_lines(control.handle("DEVICES", Caller::web)), device);
        for (auto &d : devices) {
            const auto it = profiles.find(d.value("uuid", ""));
            d["profiles"] = it == profiles.end() ? json(nullptr) : json(it->second);
        }
        send_json(response, json{{"host", host},
                                 {"status", status_lines.empty() ? json::object() : json(status(status_lines.front()))},
                                 {"pairings", records(ok_lines(control.handle("PAIRINGS", Caller::web)), pairing)},
                                 {"devices", devices},
                                 {"trusted", uuid_list("web_trusted.txt")},
                                 {"risk_ack", uuid_list("web_risk_ack.txt")},
                                 {"streams", records(ok_lines(control.handle("STREAMS", Caller::web)), stream_record)}});
    });
    // The console's profiles, for choosing which ones a device may play as. Names are shown only here, signed in.
    get("^/api/users$", [&control](const Response &response, const Request &) {
        const std::string answer = control.handle("USERS", ControlServer::Caller::web);
        if (answer.starts_with("ERR ")) return send_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, answer.substr(4));
        send_json(response, json{{"users", records(ok_lines(answer), console_user)}});
    });
    // A profile's picture, from the console's own profile cache; 404 for a profile without one.
    get("^/api/users/([0-9a-f]{8})/picture$", [](const Response &response, const Request &request) {
        std::string id = request->path_match[1].str();
        std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        // avatar.png is a PNG for a picture chosen on the console, and a JPEG under the same name for one from an account.
        const std::string picture = read_file("/system_data/priv/cache/profile/0x" + id + "/avatar.png");
        const char *type = picture.starts_with("\x89PNG\r\n\x1a\n") ? "image/png"
                           : picture.starts_with("\xff\xd8\xff")    ? "image/jpeg"
                                                                    : nullptr;
        if (!type) return send_error(response, SimpleWeb::StatusCode::client_error_not_found, "no picture");
        send(response, SimpleWeb::StatusCode::success_ok, picture, type, {{"Cache-Control", "private, max-age=3600"}});
    });
    // {"uuids": [...], "trusted": true | false}: "I trust this device" and "Stop trusting" in Devices.
    post("^/api/devices/trust$", [](const Response &response, const json &body) {
        const auto trust = body.find("trusted");
        if (trust == body.end() || !trust->is_boolean())
            return send_error(response, SimpleWeb::StatusCode::client_error_bad_request, "trusted must be true or false");
        const bool trusted = trust->get<bool>();
        if (!change_uuid_list(response, body, "web_trusted.txt", trusted)) return;
        BOOST_LOG(info) << "Web page: "sv << body.at("uuids").size() << " device(s) "sv << (trusted ? "trusted"sv : "no longer trusted"sv);
        send_json(response, json{{"ok", true}});
    });
    // "Don't show again" on the risky setup notice: {"uuids": [...]}, the devices it stops naming. Only the notice: Devices
    // still marks them Risky, and a device that turns risky later brings the notice back.
    post("^/api/risk/ack$", [](const Response &response, const json &body) {
        if (!change_uuid_list(response, body, "web_risk_ack.txt", true)) return;
        BOOST_LOG(info) << "Web page: the risky setup notice no longer names "sv << body.at("uuids").size() << " device(s)"sv;
        send_json(response, json{{"ok", true}});
    });
    // {"uuid": ..., "profiles": "free" | "none" | "unset" | ids and "guest" separated by commas}.
    post("^/api/devices/profiles$", [&control](const Response &response, const json &body) {
        const std::string uuid = text_field(body, "uuid"), profiles = text_field(body, "profiles");
        const bool plain = !profiles.empty() && profiles.size() <= 200 &&
                           std::all_of(profiles.begin(), profiles.end(), [](char c) {
                               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'F') || (c >= '0' && c <= '9') || c == ',';
                           });
        if (!word(uuid, 64) || !plain) return send_error(response, SimpleWeb::StatusCode::client_error_bad_request, "check the profiles");
        send_answer(response, control.handle("SETPROFILES " + uuid + " " + profiles, ControlServer::Caller::web));
    });
    post("^/api/pin$", [&control](const Response &response, const json &body) {
        const std::string id = text_field(body, "id"), pin = text_field(body, "pin"), name = text_field(body, "name");
        if (!word(pin, 8) || (!id.empty() && !word(id, 64)) || (!name.empty() && !name_ok(name)))
            return send_error(response, SimpleWeb::StatusCode::client_error_bad_request, "check the PIN and the name");
        std::string line = "PIN " + (id.empty() ? pin : id + " " + pin);
        if (!name.empty()) line += " " + name;
        send_answer(response, control.handle(line, ControlServer::Caller::web));
    });
    // Denies a pending pairing: Moonlight hears that the operator cancelled it, and the device may ask again at once.
    post("^/api/pin/cancel$", [&control](const Response &response, const json &body) {
        const std::string id = text_field(body, "id");
        if (!word(id, 64)) return send_error(response, SimpleWeb::StatusCode::client_error_bad_request, "no pairing request");
        send_answer(response, control.handle("CANCELPIN " + id, ControlServer::Caller::web));
    });
    post("^/api/devices/rename$", [&control](const Response &response, const json &body) {
        const std::string uuid = text_field(body, "uuid"), name = text_field(body, "name");
        if (!word(uuid, 64) || !name_ok(name))
            return send_error(response, SimpleWeb::StatusCode::client_error_bad_request, "check the name");
        send_answer(response, control.handle("RENAME " + uuid + " " + name, ControlServer::Caller::web));
    });
    post("^/api/devices/unpair$", [&control](const Response &response, const json &body) {
        const std::string uuid = text_field(body, "uuid");
        if (!word(uuid, 64)) return send_error(response, SimpleWeb::StatusCode::client_error_bad_request, "no device");
        send_answer(response, control.handle("UNPAIR " + uuid, ControlServer::Caller::web));
    });
    post("^/api/streams/end$", [&control](const Response &response, const json &body) {
        const std::string uuid = text_field(body, "uuid");
        if (!word(uuid, 64)) return send_error(response, SimpleWeb::StatusCode::client_error_bad_request, "no device");
        send_answer(response, control.handle("ENDSTREAM " + uuid, ControlServer::Caller::web));
    });

    // Settings: the app's list without the Experimental switches. They apply when the host starts again.
    get("^/api/settings$", [](const Response &response, const Request &) {
        send_json(response, json{{"options", settings_json()}});
    });
    post("^/api/settings$", [](const Response &response, const json &body) {
        const std::string problem = save_settings(body);
        if (!problem.empty()) return send_error(response, SimpleWeb::StatusCode::client_error_bad_request, problem);
        send_json(response, json{{"ok", true}, {"restart_needed", true}});
    });

    // Logs and restart.
    get("^/api/logs$", [](const Response &response, const Request &request) {
        const auto query = request->parse_query_string();
        const auto which = query.find("file");
        const bool diag = which != query.end() && which->second == "diag";
        const std::filesystem::path path =
            diag ? platf::appdata() / "host_diag.log" : std::filesystem::path(config::sunshine.log_file);
        send(response, SimpleWeb::StatusCode::success_ok, read_tail(path, log_limit), "text/plain; charset=utf-8");
    });
    // The same report as the app's Diagnostics page, built by the host: no addresses, names, ids or game titles.
    get("^/api/diagnostics$", [&control](const Response &response, const Request &) {
        BOOST_LOG(info) << "Web page: diagnostic report asked for"sv;
        send(response, SimpleWeb::StatusCode::success_ok, build_report(control), "text/plain; charset=utf-8");
    });
    post("^/api/restart$", [shutdown](const Response &response, const json &) {
        BOOST_LOG(info) << "Web page: restart asked for"sv;
        if (const std::string problem = start_relaunch(); !problem.empty()) {
            BOOST_LOG(error) << "Web page: restart failed: "sv << problem;
            return send_error(response, SimpleWeb::StatusCode::server_error_internal_server_error,
                              "The host can't restart itself: " + problem + ". It keeps running.");
        }
        send_json(response, json{{"ok", true}});
        // A moment for the answer to leave before the services stop.
        std::thread([shutdown] {
            std::this_thread::sleep_for(1s);
            shutdown->raise(true);
        }).detach();
    });
}
}  // namespace

WebServer::WebServer(ControlServer &control, std::shared_ptr<safe::event_t<bool>> shutdown)
    : control_(control), shutdown_(std::move(shutdown)) {
    config_at_start = read_file(config::sunshine.config_file);
}

void WebServer::run(std::stop_token stop) {
    bool said_off = false;
    while (!stop.stop_requested()) {
        // The generation first: a login removed after it is a change the watcher sees.
        const unsigned generation = web_login::generation();
        if (web_login::user().empty()) {
            if (!said_off) BOOST_LOG(info) << (web_login::stored_user().empty() ? "Web page off: no login set in the app"sv : "Web page off: turned off in the app, the login is kept"sv);
            said_off = true;
            std::this_thread::sleep_for(500ms);
            continue;
        }
        said_off = false;
        State state;
        state.generation = generation;
        state.remembered = web_login::load_remembered();
        const uint16_t port = net::map_port(1);
        bool failed = false;
        try {
            // The page's own certificate (web_cert.hpp); Sunshine's as a fallback. They load here, and that throws when
            // they can't be read.
            std::string cert_problem;
            const auto files = web_cert::ensure(cert_problem);
            if (!files) BOOST_LOG(warning) << "Web page: own certificate failed ("sv << cert_problem << "), using the host's"sv;
            Server server(files ? files->cert : config::nvhttp.cert, files ? files->key : config::nvhttp.pkey);
            server.config.reuse_address = true;
            server.config.address = net::get_bind_address(net::af_from_enum_string(config::sunshine.address_family));
            server.config.port = port;
            server.config.max_request_streambuf_size = 64 * 1024;
            server.config.timeout_request = 10;
            server.config.timeout_content = 60;
            routes(server, state, control_, shutdown_);
            // start() blocks; this thread stops it when the host stops or the login changes. A stop() before start() has
            // bound does nothing, so it asks again until start() returns.
            std::atomic_bool done{false};
            std::thread watcher([&] {
                while (!done) {
                    if (stop.stop_requested() || web_login::generation() != generation) server.stop();
                    std::this_thread::sleep_for(250ms);
                }
            });
            try {
                BOOST_LOG(info) << "Web page on port "sv << port;
                server.start();
            } catch (const std::exception &e) {
                BOOST_LOG(error) << "Web page failed on port "sv << port << ": "sv << e.what();
                failed = true;
            }
            done = true;
            watcher.join();
        } catch (const std::exception &e) {
            BOOST_LOG(error) << "Web page could not start: "sv << e.what();
            failed = true;
        }
        for (int i = 0; failed && i < 40 && !stop.stop_requested(); ++i) std::this_thread::sleep_for(250ms);
    }
}


}  // namespace prospero::web
