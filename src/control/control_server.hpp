// SPDX-License-Identifier: GPL-3.0-only
// Development control socket (ROADMAP P2.3). One line per request, one line per answer:
//   STATUS                      -> "OK running=<0|1> app=<name> pending=<n> address=<ip> client=<name>"
//                                  (address and client empty while no stream runs; client is the rest
//                                  of the line and may contain spaces)
//   PAIRINGS                    -> "OK <id> <client name>" per pending request, then "END"
//   PIN <pin> [name]            -> approves the only pending request ("OK paired" / "ERR ...")
//   PIN <id> <pin> [name]       -> approves the given request
//   CANCELPIN <id>              -> denies the given request; Moonlight is told at once ("OK cancelled" /
//                                  "ERR no such request" when it is gone or the id is wrong)
//   DEVICES                     -> "OK <uuid> <streaming 0|1> <last mode or -> <name>" per paired device, then "END"
//   STREAMS                     -> per running stream "OK uuid=<uuid> address=<ip> codec=<H.264|HEVC|AV1> hdr=<0|1>
//                                  mode=<WxH@fps> kbps=<n> secs=<n> pads=<n> role=<capture|joined|starting> channel=<0|1|->
//                                  sent_kbps=<n|-> rtt_ms=<n|-> recoveries=<n> name=<name>", then "END" (stream_list.hpp;
//                                  "-" for an unknown value; sent_kbps since the last STREAMS, or since the start)
//   ENDSTREAM <uuid>            -> ends that device's stream; the game and the others' streams go on ("OK ended" / "ERR ...")
//   RENAME <uuid> <name>      -> gives a paired device another name ("OK renamed" / "ERR ...")
//   UNPAIR <uuid>               -> removes a paired device and ends its stream ("OK removed" / "ERR ..."); its profiles go
//   USERS                       -> "OK <user id in hex> <name>" per profile on the console, signed in or not, then "END"
//                                  (console_users.h; the name is the rest of the line)
//   PROFILES                    -> "OK <uuid> <profiles>" per device whose profiles are set, then "END"
//                                  (device_profiles.hpp: free, none, or ids and "guest" separated by commas)
//   SETPROFILES <uuid> <profiles|unset> -> sets which profiles a paired device may play as; "unset" takes the setting
//                                  away ("OK saved" / "ERR ..."). Every id must be a profile on the console.
//   REPORT                      -> "OK port=<n>": for some seconds, <appdata>/diagnostics.txt is on that port
//   AUTOLOAD [ON|OFF]           -> the Payload Manager's autoload list, for the app (autoload_host.hpp)
//   WEBLOGIN                    -> "OK user=<name> enabled=<0|1>": the stored login, also while the page is off; the
//                                  name is empty without one (enabled=0 then). The page serves while enabled=1.
//   WEBLOGIN SET <user> <pass>  -> sets the web page's login and turns it on ("OK set"); the password is the rest of the line
//   WEBLOGIN ON                 -> turns the page on with the stored login ("OK on" / "ERR no login")
//   WEBLOGIN OFF                -> turns the page off and keeps the login ("OK off", also without one)
//   WEBLOGIN FORGET             -> removes the login, which turns the page off ("OK forgotten")
//   PROBE                       -> sends <appdata>/firmware_probe.elf to the ELF loader ("OK started" / "ERR ..."), refused
//                                  while a stream runs; the probe then stops the host and starts it again
//                                  (docs/firmware-probe.md). For the app's Diagnostics page and a web page later.
//   FILE <request>, INSTALL     -> the host's files for an app that has no /data, from this console only (file_bridge.hpp)
//   QUIT                        -> raises the host's shutdown event
// It has no authentication, so it listens on 127.0.0.1 only (the PS5 app); the empty file
// <appdata>/control_lan opens it to the LAN for development on trusted networks (ROADMAP P9.1).
#pragma once
#include "control/device_modes.hpp"
#include "control/device_profiles.hpp"
#include "src/thread_safe.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>

namespace prospero {

class ControlServer {
public:
    static constexpr uint16_t port = 47995;
    static constexpr uint16_t report_port = 47996;
    explicit ControlServer(std::shared_ptr<safe::event_t<bool>> shutdown);
    ~ControlServer();
    void run(std::stop_token stop);
    void run_report(std::stop_token stop);
    // Who asks: the app over the socket or the web page. Each has its own STREAMS rate baseline.
    enum class Caller { app, web };
    // Handles one request line; the web page's thread and the socket's call it.
    std::string handle(const std::string &line, Caller caller = Caller::app);
    // Whether RELISTEN was asked for since the last call.
    bool take_relisten() { return relisten_.exchange(false); }

private:
    // The app asks every two seconds while its page is open; this outlasts a missed turn or two.
    static constexpr std::chrono::seconds report_window{8};
    // A FILE PUT is the longest request: the app's diagnostic report, in hex.
    static constexpr size_t largest_request = 2u << 20;
    std::shared_ptr<safe::event_t<bool>> shutdown_;
    DeviceModes modes_;
    DeviceProfiles profiles_;
    std::atomic_bool report_listening_{false};
    std::atomic_bool relisten_{false};
    std::atomic_bool client_local_{true};
    std::atomic<std::chrono::steady_clock::rep> report_until_{0};
    std::string handle_locked(const std::string &line, Caller caller);
    void migrate_profiles();
    bool profiles_migrated_ = false;
    std::mutex mutex_;
    // Each session's video bytes at the caller's last STREAMS, by client certificate: the rate is the difference.
    struct SentSample {
        unsigned long long bytes;
        std::chrono::steady_clock::time_point at;
    };
    std::map<std::string, SentSample> sent_[2];
};

}
