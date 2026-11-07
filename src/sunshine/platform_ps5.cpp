// SPDX-License-Identifier: GPL-3.0-only
// Sunshine's platf:: layer for the PS5: networking, game audio from Avcap2, the native virtual pad, keyboard
// and mouse, and placeholders for what is not forwarded (touch screen, pen).
#include "audio/channel_map.hpp"
#include "input/gamepad_map.hpp"
#include "input/keyboard_map.hpp"
#include "input/pad_roles.hpp"
#include "input/pad_touch.hpp"
#include "platform/ps5/kbm.h"
#include "platform/ps5/pad.h"
#include "platform/ps5/profile_seat.h"
#include "platform/ps5/shared_capture.hpp"
#include "control/device_profiles.hpp"
#include "src/nvhttp.h"
#include "src/config.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <pthread_np.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <thread>
#include <boost/asio/ip/address.hpp>

using namespace std::literals;

extern "C" int sceNetGetMacAddress(uint8_t address[6], int flags);  // libSceNet, linked by default

namespace platf {

std::unique_ptr<deinit_t> init() { return std::make_unique<deinit_t>(); }

namespace publish {
// Upstream calls this from main.cpp, which the PS5 host replaces; mDNS runs from main_ps5.cpp
// (src/platform/ps5/mdns_responder.cpp, ROADMAP P9.2).
std::unique_ptr<deinit_t> start() { return nullptr; }
}

std::filesystem::path appdata() { return "/data/prosperosunshine"; }

std::string get_host_name() {
    char name[256] = {};
    if (gethostname(name, sizeof name - 1) == 0 && name[0]) return name;
    return "PS5";
}

std::string get_mac_address(const std::string_view &address) {
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) return "00:00:00:00:00:00";
    std::string interface_name, mac = "00:00:00:00:00:00";
    for (auto *a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || (a->ifa_addr->sa_family != AF_INET && a->ifa_addr->sa_family != AF_INET6)) continue;
        if (from_sockaddr(a->ifa_addr) == address) interface_name = a->ifa_name;
    }
    for (auto *a = list; a && !interface_name.empty(); a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_LINK || interface_name != a->ifa_name) continue;
        const auto *dl = reinterpret_cast<const sockaddr_dl *>(a->ifa_addr);
        if (dl->sdl_alen != 6) continue;
        const auto *b = reinterpret_cast<const unsigned char *>(LLADDR(dl));
        char buf[18];
        std::snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
        mac = buf;
    }
    freeifaddrs(list);
    // Without a match above, ask the network library.
    if (mac == "00:00:00:00:00:00") {
        uint8_t b[6] = {};
        if (sceNetGetMacAddress(b, 0) == 0) {
            char buf[18];
            std::snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
            mac = buf;
        }
    }
    return mac;
}

std::string from_sockaddr(const sockaddr *const addr) { return from_sockaddr_ex(addr).second; }

std::pair<std::uint16_t, std::string> from_sockaddr_ex(const sockaddr *const addr) {
    char text[INET6_ADDRSTRLEN] = {};
    std::uint16_t port = 0;
    if (addr->sa_family == AF_INET6) {
        const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(addr);
        inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof text);
        port = ntohs(in6->sin6_port);
    } else if (addr->sa_family == AF_INET) {
        const auto *in = reinterpret_cast<const sockaddr_in *>(addr);
        inet_ntop(AF_INET, &in->sin_addr, text, sizeof text);
        port = ntohs(in->sin_port);
    }
    return {port, text};
}

void set_thread_name(std::string_view name) {
    const std::string copy(name.substr(0, 15));
    pthread_set_name_np(pthread_self(), copy.c_str());
}

void adjust_thread_priority(thread_priority_e) {}
void streaming_will_start() {}
void streaming_will_stop() {}
void open_url(const std::string &) {}

std::unique_ptr<deinit_t> enable_socket_qos(uintptr_t, boost::asio::ip::address &, uint16_t, qos_data_type_e, bool) {
    return nullptr;
}

namespace {
socklen_t to_sockaddr(const boost::asio::ip::address &address, uint16_t port, sockaddr_storage &out) {
    std::memset(&out, 0, sizeof out);
    if (address.is_v6()) {
        auto *in6 = reinterpret_cast<sockaddr_in6 *>(&out);
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(port);
        const auto bytes = address.to_v6().to_bytes();
        std::memcpy(&in6->sin6_addr, bytes.data(), bytes.size());
        return sizeof(sockaddr_in6);
    }
    auto *in = reinterpret_cast<sockaddr_in *>(&out);
    in->sin_family = AF_INET;
    in->sin_port = htons(port);
    const auto bytes = address.to_v4().to_bytes();
    std::memcpy(&in->sin_addr, bytes.data(), bytes.size());
    return sizeof(sockaddr_in);
}

bool send_one(int fd, const sockaddr_storage &to, socklen_t to_len, const char *header, size_t header_size,
              const char *payload, size_t payload_size) {
    iovec iov[2];
    int count = 0;
    if (header_size) iov[count++] = {const_cast<char *>(header), header_size};
    iov[count++] = {const_cast<char *>(payload), payload_size};
    msghdr msg{};
    msg.msg_name = const_cast<sockaddr_storage *>(&to);
    msg.msg_namelen = to_len;
    msg.msg_iov = iov;
    msg.msg_iovlen = count;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (sendmsg(fd, &msg, 0) >= 0) return true;
        // A full send buffer on a nonblocking socket: give the NIC a moment instead of dropping.
        if (errno != EAGAIN && errno != ENOBUFS) return false;
        std::this_thread::sleep_for(100us);
    }
    return false;
}
}

bool send_batch(const batched_send_info_t &info) {
    sockaddr_storage to;
    const socklen_t to_len = to_sockaddr(info.target_address, info.target_port, to);
    for (size_t i = 0; i < info.block_count; ++i) {
        const auto block = info.buffer_for_payload_offset((info.block_offset + i) * info.payload_size);
        const size_t size = std::min(info.payload_size, block.size);
        if (!send_one(static_cast<int>(info.native_socket), to, to_len, info.headers ? info.headers + (info.block_offset + i) * info.header_size : nullptr,
                      info.headers ? info.header_size : 0, block.buffer, size))
            return false;
    }
    return true;
}

bool send(send_info_t &info) {
    sockaddr_storage to;
    const socklen_t to_len = to_sockaddr(info.target_address, info.target_port, to);
    return send_one(static_cast<int>(info.native_socket), to, to_len, info.header, info.header_size, info.payload,
                    info.payload_size);
}

namespace {
class sleep_timer final : public high_precision_timer {
public:
    void sleep_for(const std::chrono::nanoseconds &duration) override { std::this_thread::sleep_for(duration); }
    explicit operator bool() override { return true; }
};
}

std::unique_ptr<high_precision_timer> create_high_precision_timer() { return std::make_unique<sleep_timer>(); }

// Silent microphone: keeps the audio stream alive with correctly paced zero frames.
namespace {
class silent_mic final : public mic_t {
public:
    silent_mic(int channels, std::uint32_t rate, std::uint32_t frame_size)
        : samples_(static_cast<size_t>(channels) * frame_size),
          period_(std::chrono::nanoseconds(1'000'000'000LL * frame_size / rate)),
          next_(std::chrono::steady_clock::now()) {}
    capture_e sample(std::vector<float> &frame_buffer) override {
        next_ += period_;
        std::this_thread::sleep_until(next_);
        frame_buffer.assign(samples_, 0.0f);
        return capture_e::ok;
    }

private:
    size_t samples_;
    std::chrono::nanoseconds period_;
    std::chrono::steady_clock::time_point next_;
};

// Game audio from Avcap2 as raw PCM: float32, stereo, 48 kHz, 1024 frames per packet (R2). Sunshine's
// own Opus encoder then cuts the frames Moonlight negotiated (5 ms by default). With the app's "Surround 5.1/7.1"
// (surround_sound) and a client that asks for 5.1 or 7.1, the channel is read as encoder type 3, 12 channels, and each
// microphone gets its own layout from it (audio/channel_map.hpp). Without the setting, or when type 3 fails, stereo.
//
// When the capture session is lost (a game start takes the resource), replaced by a reopen, or not
// there yet as the stream starts, the audio channel is gone; the microphone then delivers paced
// silence and reopens its channel on the current session every half second, so the stream itself
// never ends because of audio.
//
// The console has one audio channel. With several streams (a second player), the microphone that opened it reads it
// and hands every packet to all microphones; the others only wait for theirs. When the reading one goes, the next to
// notice opens the channel anew.
class avcap_mic;
struct audio_hub {
    std::mutex mutex;
    std::condition_variable arrived;
    std::vector<avcap_mic *> mics;
    avcap_mic *reader = nullptr;
    int source_channels = 2;  // of the channel the reader opened: 2 (type 0) or 12 (type 3)
    int wanted_channels = 2;  // the widest layout any microphone asks for
    bool surround_enabled = false;  // the app's Surround switch
    // Type 3's level per channel, logged every 30 s: which channels carry sound on a console with surround output.
    std::array<double, prospero::kAvcapSurroundChannels> squares{};
    std::size_t frames = 0;
    std::chrono::steady_clock::time_point levels_at{};
};

class avcap_mic final : public mic_t {
public:
    static std::unique_ptr<mic_t> open(int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool surround) {
        int rc = 0;
        auto shared = prospero::SharedCapture::acquire(rc);
        if (!shared) BOOST_LOG(warning) << "PS5 audio: capture session unavailable: "sv << rc << ", retrying"sv;
        auto mic = std::unique_ptr<avcap_mic>(new avcap_mic(std::move(shared), channels, sample_rate, frame_size));
        {
            std::lock_guard lock(hub_.mutex);
            hub_.surround_enabled = surround;
        }
        // A title started with the stream can hold the capture for a moment. Returning no microphone here left the
        // whole stream silent: Sunshine only asks for a new one after a reinit, which the silent fallback never sends.
        if (mic->another_reads()) BOOST_LOG(info) << "PS5 audio: sharing the channel another stream reads"sv;
        else if (!mic->shared_ || !mic->open_channel(mic->wanted_channels()))
            mic->retry_at_ = std::chrono::steady_clock::now() + 500ms;
        return mic;
    }

    ~avcap_mic() override {
        if (open_) close_channel();
        std::lock_guard lock(hub_.mutex);
        std::erase(hub_.mics, this);
        hub_.wanted_channels = wanted_channels_locked();
    }

    capture_e sample(std::vector<float> &frame_buffer) override {
        const size_t needed = static_cast<size_t>(frame_size_) * static_cast<size_t>(channels_);
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            bool follower = false;
            {
                std::unique_lock lock(hub_.mutex);
                follower = hub_.reader && hub_.reader != this;
                if (follower) hub_.arrived.wait_for(lock, 100ms, [&] { return pending_.size() >= needed; });
                if (pending_.size() >= needed) return take(frame_buffer, needed);
            }
            // A reader that sends nothing for a while is treated like a lost channel: silence, and a new try later.
            if (follower) {
                if (std::chrono::steady_clock::now() - start > 500ms) return silence(frame_buffer);
                continue;
            }
            if (open_ && generation_ != shared_->generation()) {
                set_open(false);  // session was reopened
                surround_failed_ = false;  // a new session may allow type 3 again
            }
            if (!open_) return silence(frame_buffer);
            // A stream that joined with a wider layout makes the reader open the 12-channel bed.
            if (source_channels_ == 2 && !surround_failed_ && wanted_channels() > 2 && surround_enabled()) {
                close_channel();
                if (!open_channel(wanted_channels())) return silence(frame_buffer);
            }
            const int rc = shared_->with_session(
                [&](ps_capture *s) { return ps_capture_audio_read(s, packet_.data(), packet_.size(), header_); });
            if (rc < 0) {
                BOOST_LOG(warning) << "PS5 audio: ReadAudio failed: "sv << rc << ", reopening"sv;
                if (source_channels_ == prospero::kAvcapSurroundChannels) surround_failed_ = true;
                close_channel();
                return silence(frame_buffer);
            }
            if (rc == 0) {
                // A stalled channel (no packet for a while) is treated like a lost one.
                if (std::chrono::steady_clock::now() - start > 500ms) {
                    close_channel();
                    return silence(frame_buffer);
                }
                std::this_thread::sleep_for(1ms);
                continue;
            }
            const size_t frame_bytes = static_cast<size_t>(source_channels_) * sizeof(float);
            if (static_cast<size_t>(rc) % frame_bytes != 0) {
                BOOST_LOG(warning) << "PS5 audio: packet of "sv << rc << " bytes is not "sv << source_channels_
                                   << " channels, dropped"sv;
                continue;
            }
            hand_out(reinterpret_cast<const float *>(packet_.data()), static_cast<size_t>(rc) / sizeof(float));
        }
    }

private:
    avcap_mic(std::shared_ptr<prospero::SharedCapture> shared, int channels, std::uint32_t sample_rate,
              std::uint32_t frame_size)
        : shared_(std::move(shared)), channels_(channels), frame_size_(frame_size),
          period_(std::chrono::nanoseconds(1'000'000'000LL * frame_size / sample_rate)), packet_(0xC000) {
        std::lock_guard lock(hub_.mutex);
        hub_.mics.push_back(this);
        hub_.wanted_channels = std::max(hub_.wanted_channels, channels_);
    }

    bool another_reads() {
        std::lock_guard lock(hub_.mutex);
        return hub_.reader && hub_.reader != this;
    }

    int wanted_channels() {
        std::lock_guard lock(hub_.mutex);
        return hub_.wanted_channels;
    }

    // Caller holds hub_.mutex.
    static int wanted_channels_locked() {
        int wanted = 2;
        for (avcap_mic *mic : hub_.mics) wanted = std::max(wanted, mic->channels_);
        return wanted;
    }

    bool surround_enabled() {
        std::lock_guard lock(hub_.mutex);
        return hub_.surround_enabled;
    }

    void set_open(bool open) {
        open_ = open;
        std::lock_guard lock(hub_.mutex);
        if (open) {
            hub_.reader = this;
            pending_.clear();
        } else if (hub_.reader == this) {
            hub_.reader = nullptr;
            hub_.arrived.notify_all();
        }
    }

    // Every microphone gets the packet, in its own layout. One that falls behind keeps the last 200 ms, so its stream
    // doesn't lag.
    void hand_out(const float *samples, size_t count) {
        std::lock_guard lock(hub_.mutex);
        const int source = hub_.source_channels;
        const size_t frames = count / static_cast<size_t>(source);
        if (source == prospero::kAvcapSurroundChannels) log_levels(samples, frames);
        for (avcap_mic *mic : hub_.mics) {
            const size_t most = 48000 / 5 * static_cast<size_t>(mic->channels_);
            prospero::map_channels(samples, source, frames, mic->channels_, mic->pending_);
            if (mic->pending_.size() > most)
                mic->pending_.erase(mic->pending_.begin(),
                                    mic->pending_.begin() + static_cast<std::ptrdiff_t>(mic->pending_.size() - most));
        }
        hub_.arrived.notify_all();
    }

    // Caller holds hub_.mutex. pending_ is in this microphone's layout already.
    capture_e take(std::vector<float> &frame_buffer, size_t needed) {
        next_ = std::chrono::steady_clock::now();
        frame_buffer.assign(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(needed));
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(needed));
        return capture_e::ok;
    }

    // Caller holds hub_.mutex.
    static void log_levels(const float *samples, size_t frames) {
        constexpr int n = prospero::kAvcapSurroundChannels;
        for (size_t i = 0; i < frames; ++i)
            for (int c = 0; c < n; ++c) hub_.squares[c] += static_cast<double>(samples[i * n + c]) * samples[i * n + c];
        hub_.frames += frames;
        const auto now = std::chrono::steady_clock::now();
        if (now < hub_.levels_at) return;
        if (hub_.levels_at != std::chrono::steady_clock::time_point{} && hub_.frames) {
            std::string line;
            for (int c = 0; c < n; ++c) {
                char level[16];
                std::snprintf(level, sizeof level, " %.4f", std::sqrt(hub_.squares[c] / static_cast<double>(hub_.frames)));
                line += level;
            }
            BOOST_LOG(info) << "PS5 audio: 12-channel levels (RMS, channel 0 to 11):"sv << line;
        }
        hub_.squares = {};
        hub_.frames = 0;
        hub_.levels_at = now + 30s;
    }

    bool open_channel(int wanted) {
        // The generation is read under the lock that opens the channel. Read before it, a reopen in between (a game
        // starting) left the channel on the new session marked with the old generation: the next sample() then dropped
        // it without closing it, and every later open failed on the channel already there.
        unsigned generation = 0;
        int source = 2, surround_rc = 0;
        const bool try_surround = surround_enabled() && wanted > 2 && !surround_failed_;
        const int rc = shared_->exclusive([&](ps_capture *s) {
            generation = shared_->generation();
            if (try_surround) {
                surround_rc = ps_capture_audio_open(s, 3, 0, 0);
                if (!surround_rc) {
                    source = prospero::kAvcapSurroundChannels;
                    return 0;
                }
            }
            return ps_capture_audio_open(s, 0, 0, 0);
        });
        if (rc) {
            BOOST_LOG(warning) << "PS5 audio: OpenAudio failed: "sv << rc;
            return false;
        }
        if (try_surround && surround_rc) {
            surround_failed_ = true;
            BOOST_LOG(warning) << "PS5 audio: 12 channels refused ("sv << surround_rc << "), stereo"sv;
        } else if (try_surround) {
            BOOST_LOG(info) << "PS5 audio: 12 channels from the console for "sv << channels_ << " speakers"sv;
        }
        source_channels_ = source;
        {
            std::lock_guard lock(hub_.mutex);
            hub_.source_channels = source;
            hub_.levels_at = {};
        }
        generation_ = generation;
        set_open(true);
        return true;
    }

    void close_channel() {
        // After a reopen the channel died with the old session; closing would hit the new one. Checked under the lock,
        // so a reopen can't slip in between.
        shared_->exclusive([&](ps_capture *s) { return generation_ == shared_->generation() ? ps_capture_audio_close(s) : 0; });
        set_open(false);
    }

    capture_e silence(std::vector<float> &frame_buffer) {
        if (std::chrono::steady_clock::now() >= retry_at_ && !another_reads()) {
            // Only the session video opened: if video failed too, the stream is ending, and opening one here would hold
            // up its teardown.
            if (!shared_) shared_ = prospero::SharedCapture::attach();
            if (shared_ && open_channel(wanted_channels())) BOOST_LOG(info) << "PS5 audio: channel reopened"sv;
            retry_at_ = std::chrono::steady_clock::now() + 500ms;
        }
        {
            // What arrives during the silence would only play late.
            std::lock_guard lock(hub_.mutex);
            pending_.clear();
        }
        next_ = std::max(next_ + period_, std::chrono::steady_clock::now() - period_);
        std::this_thread::sleep_until(next_);
        frame_buffer.assign(static_cast<size_t>(frame_size_) * channels_, 0.0f);
        return capture_e::ok;
    }

    static inline audio_hub hub_;
    std::shared_ptr<prospero::SharedCapture> shared_;
    int channels_;
    int source_channels_ = 2;  // of the channel this microphone opened: 2 (type 0) or 12 (type 3)
    bool surround_failed_ = false;  // type 3 was refused or failed; don't retry until a new session
    std::uint32_t frame_size_;
    std::chrono::nanoseconds period_;
    std::vector<uint8_t> packet_;
    uint8_t header_[0x10] = {};
    std::vector<float> pending_;  // guarded by hub_.mutex
    bool open_ = false;
    unsigned generation_ = 0;
    std::chrono::steady_clock::time_point next_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point retry_at_{};
};

class silent_audio_control final : public audio_control_t {
public:
    int set_sink(const std::string &) override { return 0; }
    std::unique_ptr<mic_t> microphone(const std::uint8_t *, int channels, std::uint32_t sample_rate,
                                      std::uint32_t frame_size, bool, bool) override {
        if (sample_rate == 48000) {
            // The app's "Surround 5.1/7.1" (off by default). Sunshine doesn't know the key and leaves it alone.
            const auto settings = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
            const auto setting = settings.find("surround_sound");
            const bool surround = setting != settings.end() && setting->second == "1";
            if (auto mic = avcap_mic::open(channels, sample_rate, frame_size, surround)) {
                BOOST_LOG(info) << "PS5 audio: game audio via Avcap2, "sv << channels << " channels, "sv << frame_size
                                << " frames per packet"sv;
                return mic;
            }
        }
        BOOST_LOG(warning) << "PS5 audio: sending silence (rate "sv << sample_rate << ')';
        return std::make_unique<silent_mic>(channels, sample_rate, frame_size);
    }
    bool is_sink_available(const std::string &) override { return true; }
    std::optional<sink_t> sink_info() override {
        sink_t sink;
        sink.host = "ps5";
        return sink;
    }
};
}

std::unique_ptr<audio_control_t> audio_control() { return std::make_unique<silent_audio_control>(); }

// Input: one native virtual DualSense per Moonlight gamepad slot, and one keyboard and one mouse shared by all clients.
// A keyboard and a mouse on the console, with what they hold. The shared ones serve the devices that can choose any
// profile, as the console's own. A device with its own profiles gets its own, seated in the profile it plays as
// (kbm_for_input). Added on first use, removed when their device's last controller leaves or no session is left.
struct kbm_t {
    std::string owner;  // the device's certificate; empty for the shared ones
    uint32_t user = 0;  // the profile they are seated in; 0 for the shared ones
    ps_keyboard *keyboard = nullptr;
    ps_mouse *mouse = nullptr;
    bool keyboard_failed = false, mouse_failed = false;
    prospero::KeyboardState keys;
    uint8_t mouse_buttons = 0;
    prospero::WheelSteps wheel, tilt;
    std::chrono::steady_clock::time_point key_down_at, button_down_at;
    std::optional<std::pair<float, float>> pointer;  // the last absolute position, for abs_mouse
};

struct input_raw_t {
    std::mutex mutex;
    std::array<ps_pad *, MAX_GAMEPADS> pads{};
    // A guest's slot whose pad isn't open yet: it opens on the controller's first use (wakes_pad, GitHub issue 26).
    std::array<bool, MAX_GAMEPADS> waiting{};
    // A waiting pad of a device that may only watch: it has no profile to play as (none, or all taken, or the one it
    // played as was taken away). It holds no role; a press tries again, at most once a second, and the console says why
    // it doesn't join at most once every switch_cooldown.
    std::array<bool, MAX_GAMEPADS> spectating{};
    std::array<std::chrono::steady_clock::time_point, MAX_GAMEPADS> watch_tried{}, watch_told{};
    std::array<bool, MAX_GAMEPADS> active{};  // last state had something pressed or deflected
    std::array<bool, MAX_GAMEPADS> back{};    // Select is down
    std::array<bool, MAX_GAMEPADS> ps_held{}; // a guest's PS or Create press that was held back, logged once per press
    std::array<std::chrono::steady_clock::time_point, MAX_GAMEPADS> back_since{};
    std::array<feedback_queue_t, MAX_GAMEPADS> feedback{};
    std::array<uint8_t, MAX_GAMEPADS> client_index{};
    std::array<std::array<uint8_t, 2>, MAX_GAMEPADS> motors{};
    std::array<std::array<uint8_t, 3>, MAX_GAMEPADS> colors{};
    std::array<prospero::TriggerBlock, MAX_GAMEPADS> triggers{};
    std::array<prospero::PadTouch, MAX_GAMEPADS> touch{};
    kbm_t shared;                             // removed when no session is left (client_session)
    std::vector<std::unique_ptr<kbm_t>> own;  // one per device with its own profiles that used them
    int sessions = 0;
    uint32_t user = 0;
    // Profiles (device_profiles.hpp): the paired device each pad belongs to, by its certificate, and the profile the host
    // seated it in (0 when the console asked).
    std::array<std::string, MAX_GAMEPADS> owner{};
    std::array<uint32_t, MAX_GAMEPADS> seat{};
    // Since when a pad holds the switch (L1+L2+R1+R2, or the keyboard pad's Ctrl+Backspace), and whether it switched already.
    std::array<std::chrono::steady_clock::time_point, MAX_GAMEPADS> switch_since{};
    std::array<bool, MAX_GAMEPADS> switch_done{};
    std::array<prospero::SwitchBudget, MAX_GAMEPADS> switch_budget{};  // a round of switches, then switch_cooldown
    // The profiles this host signed in: it signs each out again once none of its pads sits in it (release_profile), so
    // the console's four sessions don't fill up with players who left.
    std::vector<uint32_t> host_signed_in;
    bool ps_all_players = false;  // ps_button_all_players: every pad may press PS and Create, not only the host's
    prospero::PadRoles<MAX_GAMEPADS> roles;  // who is the host: a paired device, not a slot (GitHub issue 15)
    std::jthread output_thread;
};

static void set_motors(input_raw_t &input, int nr, uint8_t large, uint8_t small) {
    if (input.motors[nr] == std::array<uint8_t, 2>{large, small}) return;
    input.motors[nr] = {large, small};
    BOOST_LOG(debug) << "PS5 virtual pad "sv << nr << " motors "sv << int(large) << ',' << int(small);
    if (input.feedback[nr]) input.feedback[nr]->raise(gamepad_feedback_msg_t::make_rumble(input.client_index[nr], large * 257, small * 257));
}

static void set_color(input_raw_t &input, int nr, uint8_t r, uint8_t g, uint8_t b) {
    if (input.colors[nr] == std::array<uint8_t, 3>{r, g, b}) return;
    input.colors[nr] = {r, g, b};
    BOOST_LOG(debug) << "PS5 virtual pad "sv << nr << " light bar "sv << int(r) << ',' << int(g) << ',' << int(b);
    if (input.feedback[nr]) input.feedback[nr]->raise(gamepad_feedback_msg_t::make_rgb_led(input.client_index[nr], r, g, b));
}

static void set_triggers(input_raw_t &input, int nr, const prospero::TriggerBlock &block) {
    if (input.triggers[nr] == block) return;
    const auto effects = prospero::map_trigger_effects(block, input.triggers[nr]);
    input.triggers[nr] = block;
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (const uint8_t byte : block) {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    BOOST_LOG(debug) << "PS5 virtual pad "sv << nr << " triggers "sv << hex;
    if (effects.flags && input.feedback[nr]) {
        input.feedback[nr]->raise(gamepad_feedback_msg_t::make_adaptive_triggers(input.client_index[nr], effects.flags, effects.type_left, effects.type_right, effects.left, effects.right));
    }
}

static void check_seat_switches(input_raw_t &input);

// Reads what the system wants from each pad at the cadence of Remote Play's own reader and passes on
// changes of the motors, the light bar and the trigger effects. Only titles using the classic two-motor rumble set the motors;
// see docs/re/pad-output.md.
static void poll_pad_output(std::stop_token stop, input_raw_t *input) {
    std::array<bool, MAX_GAMEPADS> failed{};
    while (!stop.stop_requested()) {
        {
            std::lock_guard lock(input->mutex);
            check_seat_switches(*input);
            for (int nr = 0; nr < MAX_GAMEPADS; ++nr) {
                if (!input->pads[nr]) {
                    failed[nr] = false;
                    continue;
                }
                ps_pad_output output{};
                if (const int rc = ps_pad_read_output(input->pads[nr], &output); rc) {
                    if (!failed[nr]) BOOST_LOG(warning) << "PS5 virtual pad "sv << nr << " output read failed: "sv << rc;
                    failed[nr] = true;
                    continue;
                }
                set_motors(*input, nr, output.motors[0], output.motors[1]);
                set_color(*input, nr, output.color[0], output.color[1], output.color[2]);
                prospero::TriggerBlock triggers;
                std::memcpy(triggers.data(), output.triggers, sizeof output.triggers);
                set_triggers(*input, nr, triggers);
            }
        }
        std::this_thread::sleep_for(10ms);
    }
}

// Removes a keyboard and mouse (they release what they hold) and forgets their state. Caller holds input.mutex.
static void close_kbm(kbm_t &kbm) {
    const char *whose = kbm.owner.empty() ? "" : " of a device's profile";
    if (kbm.keyboard) {
        const int rc = ps_keyboard_close(kbm.keyboard);
        BOOST_LOG(info) << "PS5 virtual keyboard"sv << whose << " detached ("sv << rc << ')';
    }
    if (kbm.mouse) {
        const int rc = ps_mouse_close(kbm.mouse);
        BOOST_LOG(info) << "PS5 virtual mouse"sv << whose << " detached ("sv << rc << ')';
    }
    kbm.keyboard = nullptr;
    kbm.mouse = nullptr;
    kbm.keyboard_failed = kbm.mouse_failed = false;
    kbm.keys.clear();
    kbm.mouse_buttons = 0;
    kbm.wheel.clear();
    kbm.tilt.clear();
    kbm.pointer.reset();
}

// A device's own keyboard and mouse go once its last controller leaves. Caller holds input.mutex.
static void close_own_kbm(input_raw_t &input, const std::string &owner) {
    for (auto it = input.own.begin(); it != input.own.end();) {
        if ((*it)->owner == owner) {
            close_kbm(**it);
            it = input.own.erase(it);
        } else {
            ++it;
        }
    }
}

static void close_keyboard_mouse(input_raw_t &input) {
    close_kbm(input.shared);
    for (auto &kbm : input.own) close_kbm(*kbm);
    input.own.clear();
}

namespace {
// The platform's input, for the control socket's thread (profiles_changed). input() sets it and freeInput clears it
// before anything goes; taken before input.mutex, never after.
std::mutex live_input_mutex;
input_raw_t *live_input = nullptr;
}  // namespace

void freeInput(input_raw_t *input) {
    if (!input) return;
    {
        std::lock_guard live(live_input_mutex);
        if (live_input == input) live_input = nullptr;
    }
    input->output_thread.request_stop();
    if (input->output_thread.joinable()) input->output_thread.join();
    for (auto *&pad : input->pads) {
        ps_pad_close(pad);
        pad = nullptr;
    }
    // The profiles it signed in go with it, but one in front on the console stays.
    const uint32_t front = input->host_signed_in.empty() ? 0 : ps_profile_foreground();
    for (const uint32_t user : input->host_signed_in)
        if (user != front) ps_profile_sign_out(user);
    close_keyboard_mouse(*input);
    delete input;
}

input_t input() {
    auto *raw = new input_raw_t;
    raw->user = ps_pad_default_user();
    // Not the user's id: this log goes into the report people post (app/src/psapp/diag.cpp).
    BOOST_LOG(info) << (raw->user ? "PS5 input: virtual pads ready"sv : "PS5 input: no user for virtual pads"sv);
    // The app's "PS for all players" (off by default). Sunshine doesn't know the key and leaves it alone.
    const auto settings = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
    const auto all = settings.find("ps_button_all_players");
    raw->ps_all_players = all != settings.end() && all->second == "1";
    BOOST_LOG(info) << (raw->ps_all_players ? "PS5 input: every player can press PS and Create"sv
                                            : "PS5 input: only the host can press PS and Create"sv);
    raw->output_thread = std::jthread(poll_pad_output, raw);
    {
        std::lock_guard live(live_input_mutex);
        live_input = raw;
    }
    return input_t{raw};
}

// A client only sends the sensors it is asked for, and a new client has to be asked anew. One without sensors ignores this.
static void request_motion(const input_raw_t &input, int nr) {
    if (!input.feedback[nr]) return;
    for (const uint8_t type : {prospero::moonlight::motion_accel, prospero::moonlight::motion_gyro}) {
        input.feedback[nr]->raise(gamepad_feedback_msg_t::make_motion_event_state(input.client_index[nr], type, 100));
    }
}

namespace {
// Which paired device each session's feedback queue belongs to: stream::session::start() tells (patch 0016), and
// alloc_gamepad, which only gets the queue, asks. The weak pointer lets an ended session's entry go.
struct gamepad_owner_t {
    std::weak_ptr<void> queue;
    const void *key;
    std::string owner;
};
std::mutex gamepad_owners_mutex;
std::vector<gamepad_owner_t> gamepad_owners;

std::string gamepad_owner(const feedback_queue_t &queue) {
    std::lock_guard lock(gamepad_owners_mutex);
    for (const auto &entry : gamepad_owners)
        if (entry.key == queue.get() && !entry.queue.expired()) return entry.owner;
    return {};
}
}  // namespace

static void greet_spectator(const std::string &cert);

void set_gamepad_owner(const feedback_queue_t &queue, std::string_view owner) {
    if (!queue) return;
    {
        std::lock_guard lock(gamepad_owners_mutex);
        std::erase_if(gamepad_owners, [&](const gamepad_owner_t &entry) { return entry.queue.expired() || entry.key == queue.get(); });
        gamepad_owners.push_back({queue, queue.get(), std::string(owner)});
    }
    greet_spectator(std::string(owner));
}

namespace {
// A paired device by its certificate: its uuid (what device_profiles.txt goes by) and its name, for notifications.
struct paired_device_t {
    std::string uuid, name;
};
paired_device_t paired_device(const std::string &cert) {
    if (cert.empty()) return {};
    for (const auto &client : nvhttp::get_all_clients()) {
        const std::string uuid = client.value("uuid", "");
        if (!uuid.empty() && nvhttp::get_cert_by_uuid(uuid) == cert) return {uuid, client.value("name", "")};
    }
    return {};
}

std::string profile_name(uint32_t user) {
    char name[64];
    ps_profile_name(user, name, sizeof name);
    return name[0] ? std::string(name) : "A player"s;
}

// What a new pad of this device gets: the console's question (profiles not set, or free choice), one of the listed
// profiles none of the device's other pads holds, or nothing (no profile, or none left).
struct seat_plan_t {
    enum { ask, profile, none } kind = ask;
    std::vector<uint32_t> users;  // profile: the free ones in the order to try; the first that can be signed in wins
    bool none_left = false;       // "none" because the listed profiles are all taken, not because there are none
};
std::optional<prospero::ProfileSet> profiles_of(const std::string &uuid) {
    if (uuid.empty()) return std::nullopt;
    return prospero::DeviceProfiles(platf::appdata() / "device_profiles.txt").get(uuid);
}

prospero::DeviceSeats seats() { return prospero::DeviceSeats(platf::appdata() / "device_seats.txt"); }

// Whether another pad of the same device sits in this profile.
bool seat_taken(const input_raw_t &input, int nr, uint32_t user) {
    for (int m = 0; m < MAX_GAMEPADS; ++m)
        if (m != nr && input.pads[m] && input.owner[m] == input.owner[nr] && input.seat[m] == user) return true;
    return false;
}

seat_plan_t seat_plan(const input_raw_t &input, int nr, const std::string &uuid) {
    const auto set = profiles_of(uuid);
    if (!set || set->free) return {};
    seat_plan_t plan{seat_plan_t::profile};
    // The profile this controller last switched to comes first, while the device may still use it.
    const uint32_t last = seats().get(uuid, input.client_index[nr]);
    if (last && std::find(set->users.begin(), set->users.end(), last) != set->users.end() && !seat_taken(input, nr, last))
        plan.users.push_back(last);
    for (const uint32_t user : set->users)
        if (user != last && !seat_taken(input, nr, user)) plan.users.push_back(user);
    if (!plan.users.empty()) return plan;
    // A limited device never gets the console's question: whoever answers it can pick any profile, and picking one in use
    // moves it off the controller using it (the login manager disconnects that one, seen on 10.20). A Quick Play guest
    // ("guest" in its profiles) would need the host to add one without the question, which isn't done yet.
    return {seat_plan_t::none, {}, !set->users.empty()};
}
}  // namespace

// A device that may only watch hears so as its stream starts, not at its first press: it has no profile to play as (a
// list with none, or only the Quick Play guest, which isn't there yet).
static void greet_spectator(const std::string &cert) {
    const auto device = paired_device(cert);
    const auto set = profiles_of(device.uuid);
    if (!set || set->free || !set->users.empty()) return;
    BOOST_LOG(info) << "PS5 input: a device that may only watch started streaming"sv;
    ps_notify(((device.name.empty() ? "This device"s : device.name) + " joined as a spectator (no profile to play as)").c_str());
}

// Signs a profile this host signed in out again once none of its pads sits in it, so players who left don't keep the
// console's four sessions. A profile in front on the console stays: someone there uses it. Caller holds input.mutex.
static void release_profile(input_raw_t &input, uint32_t user) {
    auto &mine = input.host_signed_in;
    if (!user || std::find(mine.begin(), mine.end(), user) == mine.end()) return;
    for (int m = 0; m < MAX_GAMEPADS; ++m)
        if (input.pads[m] && input.seat[m] == user) return;
    std::erase(mine, user);
    if (ps_profile_foreground() == user) {
        BOOST_LOG(info) << "PS5 input: a profile the host signed in stays signed in, it is in front on the console"sv;
        return;
    }
    const int rc = ps_profile_sign_out(user);
    BOOST_LOG(info) << "PS5 input: a profile the host signed in was signed out, its last pad left ("sv << rc << ')';
}

// What open_pad did: the pad is open, its device may only watch for now (no profile for it), or it failed.
enum class pad_open { opened, watching, failed };

// Caller holds input.mutex. No device names or profile ids in the log: it goes into the report people post.
static pad_open open_pad(input_raw_t &input, int nr, bool host) {
    auto *&pad = input.pads[nr];
    input.seat[nr] = 0;
    const auto device = paired_device(input.owner[nr]);
    const auto plan = seat_plan(input, nr, device.uuid);
    const std::string who = device.name.empty() ? "This device"s : device.name;
    if (plan.kind == seat_plan_t::none) {
        // Said once in a while: a device that may only watch tries again at every press. A device without any profile
        // was told so when its stream started (greet_spectator); one whose profiles are all taken hears it here.
        if (const auto now = std::chrono::steady_clock::now(); now - input.watch_told[nr] >= prospero::switch_cooldown) {
            input.watch_told[nr] = now;
            BOOST_LOG(info) << "PS5 virtual pad "sv << nr << " not added: "sv
                            << (plan.none_left ? "every profile of its device is taken"sv : "its device has no profile, it may only watch"sv);
            if (plan.none_left) ps_notify((who + " has no profile left for another controller").c_str());
        }
        return pad_open::watching;
    }
    if (plan.kind == seat_plan_t::profile) {
        // The first free profile that is signed in or can be. Signing one in fails when the console already has its four
        // sessions (0x80960101 on 10.20): another of the device's profiles may be signed in already.
        uint32_t user = 0;
        for (const uint32_t candidate : plan.users) {
            if (ps_profile_signed_in(candidate)) {
                user = candidate;
                break;
            }
            if (const int rc = ps_profile_sign_in(candidate); rc) {
                BOOST_LOG(error) << "PS5 virtual pad "sv << nr << ": signing a profile of its device in failed: "sv << rc;
                continue;
            }
            BOOST_LOG(info) << "PS5 virtual pad "sv << nr << ": its profile was signed in"sv;
            input.host_signed_in.push_back(candidate);
            user = candidate;
            break;
        }
        if (!user) {
            // It watches, as a device without a profile does: a press tries again at most once a second, and the console
            // says why at most once every switch_cooldown, not at every try.
            if (const auto now = std::chrono::steady_clock::now(); now - input.watch_told[nr] >= prospero::switch_cooldown) {
                input.watch_told[nr] = now;
                const std::string what = plan.users.size() == 1 ? profile_name(plan.users.front()) + " couldn't"
                                                                : "none of its free profiles could"s;
                ps_notify((who + "'s controller can't join: " + what +
                           " be signed in (the console takes four at most). Press a button to try again")
                              .c_str());
            }
            return pad_open::watching;
        }
        if (const int rc = ps_pad_open(user, &pad); rc) {
            BOOST_LOG(error) << "PS5 virtual pad "sv << nr << " failed: "sv << rc;
            pad = nullptr;
            release_profile(input, user);
            return pad_open::failed;
        }
        // At once: the console decides whether to ask about a new pad a few milliseconds after it is added.
        const auto bind_start = std::chrono::steady_clock::now();
        const int bind_rc = ps_profile_bind(ps_pad_device_id(pad), user);
        const auto bind_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - bind_start).count();
        if (bind_rc) {
            // Left open, the pad would get the console's question, which a limited device must not: it goes again.
            BOOST_LOG(warning) << "PS5 virtual pad "sv << nr << ": seating it in its profile failed ("sv << bind_rc << " after "sv
                               << bind_ms << " ms), so it was not added"sv;
            ps_pad_close(pad);
            pad = nullptr;
            release_profile(input, user);
            // It watches until a press tries again (at most once a second), and hears why at most once every
            // switch_cooldown: right after the host restarts, every try fails until libSceMbus is up.
            if (const auto now = std::chrono::steady_clock::now(); now - input.watch_told[nr] >= prospero::switch_cooldown) {
                input.watch_told[nr] = now;
                ps_notify((who + "'s controller couldn't join as " + profile_name(user) + ". Press a button to try again").c_str());
            }
            return pad_open::watching;
        }
        input.seat[nr] = user;
        BOOST_LOG(info) << "PS5 virtual pad "sv << nr << " attached in its device's profile"sv << (host ? " as the host's controller"sv : ""sv)
                        << " (bound after "sv << bind_ms << " ms)"sv;
        // With more profiles to switch to, the notification tells how, so nobody has to have read the web page's help.
        const auto set = profiles_of(device.uuid);
        const bool can_switch = set && set->users.size() > 1;
        ps_notify((profile_name(user) + " joined the party" +
                   (can_switch ? ". To switch profile: Ctrl+Backspace, or hold L1+L2+R1+R2" : ""))
                      .c_str());
        return pad_open::opened;
    }
    const int rc = ps_pad_open(host && plan.kind == seat_plan_t::ask ? input.user : PS_PAD_NO_USER, &pad);
    if (rc) {
        BOOST_LOG(error) << "PS5 virtual pad "sv << nr << " failed: "sv << rc;
        pad = nullptr;
        return pad_open::failed;
    }
    BOOST_LOG(info) << "PS5 virtual pad "sv << nr << " attached"sv
                    << (host ? " as the host's controller"sv
                             : input.roles.host_away() ? ", the console asks who uses it; the host is away and keeps the role"sv
                                                       : ", the console asks who uses it"sv);
    return pad_open::opened;
}

// Moves a seated pad to its device's next free profile, signing it in when needed: the pad is added anew in it, which a
// game sees as one player's controller leaving and the next one's coming. One notification says both. The choice is
// kept for this controller of the device (DeviceSeats). Returns whether it switched; round is how many profiles the device
// switches between. Caller holds input.mutex.
static bool switch_seat(input_raw_t &input, int nr, std::size_t &round) {
    const uint32_t current = input.seat[nr];
    const auto device = paired_device(input.owner[nr]);
    const std::string who = device.name.empty() ? "This device"s : device.name;
    const auto set = profiles_of(device.uuid);
    if (!input.pads[nr] || !current || !set || set->free) {
        BOOST_LOG(info) << "PS5 virtual pad "sv << nr << ": a profile switch was held, but its device's profiles aren't set"sv;
        ps_notify((who + " has no profiles to switch between: set them on the web page").c_str());
        return false;
    }
    round = set->users.size();
    const uint32_t next = prospero::next_seat(*set, current, [&](uint32_t user) { return seat_taken(input, nr, user); });
    if (!next) {
        BOOST_LOG(info) << "PS5 virtual pad "sv << nr << ": a profile switch was held, but no other profile is free"sv;
        ps_notify((set->users.size() > 1 ? "No other profile of " + who + " is free" : who + " has only one profile").c_str());
        return false;
    }
    if (!ps_profile_signed_in(next)) {
        if (const int rc = ps_profile_sign_in(next); rc) {
            BOOST_LOG(error) << "PS5 virtual pad "sv << nr << ": signing in the profile to switch to failed: "sv << rc;
            ps_notify((profile_name(next) + " couldn't be signed in for " + who).c_str());
            return false;
        }
        input.host_signed_in.push_back(next);
    }
    ps_pad_close(input.pads[nr]);
    input.pads[nr] = nullptr;
    input.seat[nr] = 0;
    input.colors[nr] = {};
    input.triggers[nr] = {};
    input.touch[nr].clear();
    uint32_t seated = next;
    if (ps_pad_open(next, &input.pads[nr]) != 0 || ps_profile_bind(ps_pad_device_id(input.pads[nr]), next) != 0) {
        BOOST_LOG(error) << "PS5 virtual pad "sv << nr << ": the switch failed, back to its profile"sv;
        if (input.pads[nr]) ps_pad_close(input.pads[nr]);
        input.pads[nr] = nullptr;
        seated = current;
        if (ps_pad_open(current, &input.pads[nr]) != 0 || ps_profile_bind(ps_pad_device_id(input.pads[nr]), current) != 0) {
            if (input.pads[nr]) ps_pad_close(input.pads[nr]);
            input.pads[nr] = nullptr;
            input.roles.detach(nr);
            ps_notify((who + "'s controller was lost switching profiles").c_str());
            return false;
        }
    }
    input.seat[nr] = seated;
    request_motion(input, nr);
    if (seated != next) return false;
    seats().remember(device.uuid, input.client_index[nr], next);
    BOOST_LOG(info) << "PS5 virtual pad "sv << nr << " switched to its device's next profile"sv;
    ps_notify((profile_name(current) + " passed the controller to " + profile_name(next)).c_str());
    release_profile(input, current);
    return true;
}

// A switch fires once a pad has held it for switch_hold; it fires again only after letting go. Caller holds input.mutex.
static void check_seat_switches(input_raw_t &input) {
    const auto now = std::chrono::steady_clock::now();
    for (int nr = 0; nr < MAX_GAMEPADS; ++nr) {
        if (input.switch_done[nr] || input.switch_since[nr].time_since_epoch().count() == 0) continue;
        if (now - input.switch_since[nr] < prospero::switch_hold) continue;
        input.switch_done[nr] = true;
        // While it waits, the player is told how long once; later switches in the same wait are only ignored.
        if (auto &budget = input.switch_budget[nr]; !budget.allowed(now)) {
            BOOST_LOG(info) << "PS5 virtual pad "sv << nr << ": a profile switch came while it waits after a round of switches, ignored"sv;
            if (budget.tell_once())
                ps_notify(("Wait " + std::to_string(budget.seconds_left(now)) + " s to switch profile again").c_str());
            continue;
        }
        std::size_t round = 0;
        if (switch_seat(input, nr, round)) input.switch_budget[nr].switched(now, round);
        else input.switch_budget[nr].refused(now);
    }
}

// A paired device's profiles changed: each of its controllers sitting in a profile it no longer has leaves it, and waits
// as one that only watches until a press joins it as a profile the device still has. The stream goes on. More profiles,
// or a free choice, take nothing away. A pad the console's question seated (its device could choose freely) leaves too
// when the device gets a list, as which profile it sits in isn't known.
void profiles_changed(const std::string &cert, const std::vector<uint32_t> &added) {
    if (cert.empty()) return;
    std::lock_guard live(live_input_mutex);
    if (!live_input) return;
    auto &input = *live_input;
    std::lock_guard lock(input.mutex);
    const auto device = paired_device(cert);
    const auto set = profiles_of(device.uuid);
    if (!set || set->free) return;
    const std::string who = device.name.empty() ? "This device"s : device.name;
    bool benched = false;
    for (int nr = 0; nr < MAX_GAMEPADS; ++nr) {
        if (!input.pads[nr] || input.owner[nr] != cert) continue;
        const uint32_t seated = input.seat[nr];
        if (seated && std::find(set->users.begin(), set->users.end(), seated) != set->users.end()) continue;
        set_motors(input, nr, 0, 0);
        set_triggers(input, nr, prospero::triggers_off);
        ps_pad_close(input.pads[nr]);  // sends a neutral state before removal
        input.pads[nr] = nullptr;
        input.seat[nr] = 0;
        input.roles.detach(nr);
        input.colors[nr] = {};
        input.triggers[nr] = {};
        input.touch[nr].clear();
        input.back[nr] = input.ps_held[nr] = false;
        input.switch_since[nr] = {};
        input.switch_done[nr] = false;
        input.waiting[nr] = input.spectating[nr] = true;
        input.watch_tried[nr] = input.watch_told[nr] = {};
        const bool other = std::any_of(set->users.begin(), set->users.end(), [&](uint32_t user) { return !seat_taken(input, nr, user); });
        BOOST_LOG(info) << "PS5 virtual pad "sv << nr << " left the profile its device no longer has; it watches until a press"sv
                        << (other ? " joins it as another"sv : ""sv);
        ps_notify(((seated ? who + " can no longer play as " + profile_name(seated) : who + "'s profiles changed") +
                   (other ? ". Press a button to join as another of its profiles" : ": it watches now"))
                      .c_str());
        release_profile(input, seated);
        benched = true;
    }
    // A device streaming now hears at once about profiles it got, not only at its next press: a spectator that it may
    // join, a player that it may switch. A controller just taken out of a profile was told already. The caller passes
    // added profiles only while the device streams (a session's input outlives it for a while).
    if (added.empty() || benched) return;
    bool playing = false;
    for (int nr = 0; nr < MAX_GAMEPADS; ++nr) playing = playing || (input.pads[nr] && input.owner[nr] == cert);
    std::string names;
    for (const uint32_t user : added) names += (names.empty() ? "" : ", ") + profile_name(user);
    BOOST_LOG(info) << "PS5 input: a streaming device got "sv << added.size() << " profile(s)"sv << (playing ? ", it plays"sv : ", it watches"sv);
    ps_notify((playing ? who + " can now also play as " + names + ". To switch profile: Ctrl+Backspace, or hold L1+L2+R1+R2"
                       : who + " can now play as " + names + ". Press a button to join")
                  .c_str());
}

int alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &, feedback_queue_t feedback) {
    if (id.globalIndex < 0 || id.globalIndex >= MAX_GAMEPADS) return -1;
    const std::string owner = gamepad_owner(feedback);
    std::lock_guard lock(input->mutex);
    input->feedback[id.globalIndex] = std::move(feedback);
    input->client_index[id.globalIndex] = id.clientRelativeIndex;
    auto *&pad = input->pads[id.globalIndex];
    if (!pad && !input->waiting[id.globalIndex]) {
        input->owner[id.globalIndex] = owner;
        // The host's pad plays as the console's user. Every other one, from another device or a second controller on the
        // host's, is a guest: the system asks who uses it, and they pick a user or a guest (pad_roles.hpp). The role is
        // the device's from the start; a guest's pad only opens on its first use, as the question can only be answered
        // from that controller and a client may announce one that never sends anything (gamepad_map.hpp, wakes_pad).
        const bool host = input->roles.attach(id.globalIndex, owner);
        // No device ids in the log: it goes into the report people post.
        if (!host) {
            input->waiting[id.globalIndex] = true;
            BOOST_LOG(info) << "PS5 virtual pad "sv << id.globalIndex << " announced, opens when it is used"sv
                            << (owner.empty() ? " (device unknown)"sv : ""sv);
        } else if (const auto opened = open_pad(*input, id.globalIndex, true); opened != pad_open::opened) {
            input->roles.detach(id.globalIndex);
            if (opened == pad_open::failed) return -1;
            // Its device may only watch: the slot stays, so a press joins it once the device has a profile for it.
            input->waiting[id.globalIndex] = input->spectating[id.globalIndex] = true;
            input->watch_tried[id.globalIndex] = std::chrono::steady_clock::now();
        }
    }
    request_motion(*input, id.globalIndex);
    return 0;
}

int rebind_gamepad(input_t &input, const gamepad_id_t &id, feedback_queue_t feedback) {
    if (id.globalIndex < 0 || id.globalIndex >= MAX_GAMEPADS) return -1;
    std::lock_guard lock(input->mutex);
    input->feedback[id.globalIndex] = std::move(feedback);
    input->client_index[id.globalIndex] = id.clientRelativeIndex;
    request_motion(*input, id.globalIndex);
    return 0;
}

void free_gamepad(input_t &input, int nr) {
    if (nr < 0 || nr >= MAX_GAMEPADS) return;
    std::lock_guard lock(input->mutex);
    const uint32_t seated = input->seat[nr];
    input->seat[nr] = 0;
    input->switch_since[nr] = {};
    input->switch_done[nr] = false;
    if (input->waiting[nr]) {
        input->waiting[nr] = false;
        input->roles.detach(nr);
        input->owner[nr].clear();
        BOOST_LOG(info) << "PS5 virtual pad "sv << nr << (input->spectating[nr] ? " gone, it only watched"sv : " gone, never used"sv);
        input->spectating[nr] = false;
        return;
    }
    const std::string owner = std::exchange(input->owner[nr], std::string());
    if (!input->pads[nr]) return;
    set_motors(*input, nr, 0, 0);
    set_triggers(*input, nr, prospero::triggers_off);
    const int rc = ps_pad_close(input->pads[nr]);  // sends a neutral state before removal
    input->pads[nr] = nullptr;
    // Its device's own keyboard and mouse go with its last controller: from then on they play as a controller of their
    // own (keyboard_pad), as for any device without one.
    if (std::none_of(input->owner.begin(), input->owner.end(), [&](const std::string &o) { return o == owner; }))
        close_own_kbm(*input, owner);
    const bool was_host = input->roles.is_host(nr);
    input->roles.detach(nr);
    input->colors[nr] = {};  // a later pad in this slot reports its colour anew
    input->triggers[nr] = {};
    input->touch[nr].clear();
    input->back[nr] = false;
    input->ps_held[nr] = false;
    BOOST_LOG(info) << "PS5 virtual pad "sv << nr << " detached ("sv << rc << ')'
                    << (was_host ? (input->roles.host_away() ? ", the host's device keeps the role"sv : ", the host role is free"sv) : ""sv);
    release_profile(*input, seated);
}

void gamepad_update(input_t &input, int nr, const gamepad_state_t &state) {
    if (nr < 0 || nr >= MAX_GAMEPADS) return;
    std::lock_guard lock(input->mutex);
    if (input->waiting[nr]) {
        if (!prospero::wakes_pad(state.buttonFlags, state.lt, state.rt, state.lsX, state.lsY, state.rsX, state.rsY)) return;
        // A device that may only watch tries again at most once a second while it presses.
        const auto now = std::chrono::steady_clock::now();
        if (input->spectating[nr] && now - input->watch_tried[nr] < 1s) return;
        input->watch_tried[nr] = now;
        // This first use only brings up the console's question for the new pad (or, for a device with its own
        // profiles, seats it in the next one); the next press answers it. A watching pad takes a role anew.
        const bool host = input->spectating[nr] && input->roles.attach(nr, input->owner[nr]);
        switch (open_pad(*input, nr, host)) {
            case pad_open::opened:
                input->waiting[nr] = input->spectating[nr] = false;
                break;
            case pad_open::watching:
                input->spectating[nr] = true;
                input->roles.detach(nr);
                return;
            case pad_open::failed:
                input->waiting[nr] = input->spectating[nr] = false;
                input->roles.detach(nr);
                return;
        }
        request_motion(*input, nr);
    }
    if (!input->pads[nr]) return;
    // L1+L2+R1+R2 held for switch_hold, or the keyboard pad's Ctrl+Backspace at once: poll_pad_output switches the profile.
    if (!prospero::holds_switch(state.buttonFlags, state.lt, state.rt)) {
        input->switch_since[nr] = {};
        input->switch_done[nr] = false;
    } else if (input->switch_since[nr].time_since_epoch().count() == 0) {
        const auto now = std::chrono::steady_clock::now();
        input->switch_since[nr] = prospero::switch_at_once(state.buttonFlags) ? now - prospero::switch_hold : now;
    }
    const auto send = [&](uint32_t flags) {
        const auto mapped = prospero::map_gamepad(flags, state.lt, state.rt, state.lsX, state.lsY, state.rsX, state.rsY);
        const ps_pad_state pad{mapped.buttons, mapped.lx, mapped.ly, mapped.rx, mapped.ry, mapped.l2, mapped.r2};
        ps_pad_send(input->pads[nr], &pad);
    };
    // Sunshine turns a held Select into PS (back_button_timeout), but Select is Create here and pressing it
    // first would open the Create menu. So Create only goes out, as a tap, when Select comes up early. The
    // margin keeps Sunshine's own forced release at the timeout from counting as early.
    const auto role = input->roles.is_host(nr) ? prospero::PadRole::host : prospero::PadRole::guest;
    uint32_t flags = prospero::allowed_buttons(state.buttonFlags, role, input->ps_all_players);
    if ((state.buttonFlags & ~flags) && !(input->ps_held[nr]))
        BOOST_LOG(info) << "PS5 virtual pad "sv << nr << ": PS or Create ignored, only the host has them (ps_button_all_players)"sv;
    input->ps_held[nr] = state.buttonFlags & ~flags;
    bool create_tap = false;
    if (const auto timeout = config::input.back_button_timeout; timeout >= 0ms) {
        const bool back = flags & BACK;
        const auto now = std::chrono::steady_clock::now();
        if (back && !input->back[nr]) input->back_since[nr] = now;
        create_tap = !back && input->back[nr] && now - input->back_since[nr] < timeout - 20ms;
        input->back[nr] = back;
        flags &= ~BACK;
    }
    send(flags);
    if (create_tap) {
        send(flags | BACK);
        std::this_thread::sleep_for(50ms);
        send(flags);
    }
    // Sunshine neutralises held input with an all-zero state when a session ends (P4.3).
    const bool active = state.buttonFlags || state.lt || state.rt || state.lsX || state.lsY || state.rsX || state.rsY;
    if (input->active[nr] && !active) BOOST_LOG(debug) << "PS5 virtual pad "sv << nr << " released to neutral"sv;
    input->active[nr] = active;
}

void gamepad_touch(input_t &input, const gamepad_touch_t &touch) {
    const int nr = touch.id.globalIndex;
    if (nr < 0 || nr >= MAX_GAMEPADS) return;
    std::lock_guard lock(input->mutex);
    if (!input->pads[nr]) return;
    auto &fingers = input->touch[nr];
    // Sunshine sends this when a session ends and the pad stays. A last motion sample left in place would keep turning a
    // game's camera; a client sending it mid-stream only loses the 10 ms to its next sample.
    if (touch.eventType == prospero::moonlight::touch_cancel_all) {
        fingers.clear();
        ps_pad_rest(input->pads[nr]);
        return;
    }
    if (!fingers.update(touch.eventType, touch.pointerId, touch.x, touch.y)) return;
    std::array<ps_pad_touch, 2> points{};
    for (int i = 0; i < fingers.count(); i++) {
        const auto &point = fingers.points()[i];
        points[i] = {point.id, point.x, point.y};
    }
    ps_pad_set_touch(input->pads[nr], fingers.count(), points.data());
}

void gamepad_motion(input_t &input, const gamepad_motion_t &motion) {
    const int nr = motion.id.globalIndex;
    if (nr < 0 || nr >= MAX_GAMEPADS) return;
    std::lock_guard lock(input->mutex);
    if (!input->pads[nr]) return;
    if (motion.motionType == prospero::moonlight::motion_accel) {
        const float xyz[3] = {prospero::accel_to_g(motion.x), prospero::accel_to_g(motion.y), prospero::accel_to_g(motion.z)};
        ps_pad_set_acceleration(input->pads[nr], xyz);
    } else if (motion.motionType == prospero::moonlight::motion_gyro) {
        const float xyz[3] = {prospero::gyro_to_rad(motion.x), prospero::gyro_to_rad(motion.y), prospero::gyro_to_rad(motion.z)};
        ps_pad_set_angular_velocity(input->pads[nr], xyz);
    }
}

std::vector<supported_gamepad_t> &supported_gamepads(input_t *) {
    static std::vector<supported_gamepad_t> pads{{"ds5", true, ""}};
    return pads;
}

platform_caps::caps_t get_capabilities() { return platform_caps::controller_touch; }

// Sunshine keeps one of these per stream session, and keeps it for a reconnect like its pads. The keyboard and mouse
// go when the last one does, so the console doesn't list them (nor games show their prompts) once nobody streams.
class client_session: public client_input_t {
public:
    explicit client_session(input_raw_t *input): input_(input) {
        std::lock_guard lock(input_->mutex);
        ++input_->sessions;
    }

    ~client_session() override {
        std::lock_guard lock(input_->mutex);
        if (--input_->sessions == 0) close_keyboard_mouse(*input_);
    }

private:
    input_raw_t *input_;
};

std::unique_ptr<client_input_t> allocate_client_input_context(input_t &input) {
    return std::make_unique<client_session>(input.get());
}

// Each device is added on first use; one that fails is not tried again until the sessions end. The system drops what
// a device reports right after it was added (a key at 0 ms was lost, at 50 ms taken, kbm_probe on 13.00), so the
// first report waits a little, once per session.
constexpr auto device_settle = 150ms;

// A key or button let go within a few ms of going down is not seen: the first key after the settle wait above went
// down and up 1 ms apart and was lost. So a release waits until its press has been in effect this long.
constexpr auto min_hold = 30ms;

static void hold_from(std::chrono::steady_clock::time_point down) { std::this_thread::sleep_until(down + min_hold); }

namespace {
// Whose input this thread passes on (set_input_owner, patch 0022): the session's feedback queue, or null.
thread_local const feedback_queue_t *input_owner = nullptr;
}  // namespace

void set_input_owner(const feedback_queue_t *feedback_queue) { input_owner = feedback_queue; }

// Which keyboard and mouse on the console the input this thread passes on gets: the one rule, asked only by the seven
// ways in below (they get null and drop the input).
// - A device that can choose any profile (or not set yet): the shared ones, as the console's own, as before profiles.
// - A device with its own profiles: its own, seated in the profile its first controller plays as. The console doesn't ask
//   "Who's using this keyboard?" (or mouse), so nobody picks another profile with them, and the player types into text
//   fields as that profile. None while it has no controller seated: a spectator, or one whose keyboard and mouse play as a
//   controller of their own (keyboard_pad, which takes them before they get here).
// - Input without its device gets none: Sunshine's key repeat (repeat_key, on its task pool) and its nudge of the
//   pointer at a stream's start come that way, as could any other path. A repeat of a key that is held changes nothing.
// A release gets the shared ones (nothing stays held there); a device's own ones release everything when they go.
// Its profiles are read at most once a second. Caller holds input.mutex.
static kbm_t *kbm_for_input(input_raw_t &input, bool release) {
    const std::string cert = input_owner && *input_owner ? gamepad_owner(*input_owner) : std::string();
    if (cert.empty()) return release ? &input.shared : nullptr;
    thread_local std::string cached_cert;
    thread_local bool cached_limited = false;
    const auto now = std::chrono::steady_clock::now();
    thread_local std::chrono::steady_clock::time_point cached_at{};
    if (cert != cached_cert || now - cached_at > 1s) {
        const auto set = profiles_of(paired_device(cert).uuid);
        cached_limited = set && !set->free;
        cached_cert = cert;
        cached_at = now;
    }
    if (!cached_limited) return &input.shared;
    uint32_t user = 0;
    for (int nr = 0; nr < MAX_GAMEPADS && !user; ++nr)
        if (input.pads[nr] && input.owner[nr] == cert) user = input.seat[nr];
    auto it = std::find_if(input.own.begin(), input.own.end(), [&](const auto &kbm) { return kbm->owner == cert; });
    // Its player switched profile, or it lost the one they were in: the next use adds them anew where it plays now.
    if (it != input.own.end() && (*it)->user != user) {
        close_kbm(**it);
        input.own.erase(it);
        it = input.own.end();
    }
    if (!user) return nullptr;
    if (it == input.own.end()) {
        input.own.push_back(std::make_unique<kbm_t>());
        input.own.back()->owner = cert;
        input.own.back()->user = user;
        it = std::prev(input.own.end());
    }
    return it->get();
}

// Seats a keyboard or mouse of a device's own in its profile right after adding it, as open_pad does a pad. The bus
// knows them by the add handle with the device type above it, type << 48: 3 for a keyboard (0x3000000660100 in the login
// manager's log), 4 for a mouse (0x4000000670000). The bare handle is the fallback. False leaves it unseated: the
// caller removes it.
static bool seat_kbm_device(uint32_t handle, uint32_t user, const char *what, uint64_t type) {
    const uint64_t forms[] = {handle | (type << 48), handle};
    int rc = 0;
    for (const uint64_t device : forms) {
        rc = ps_profile_bind(device, user);
        if (rc == 0) {
            char text[64];
            std::snprintf(text, sizeof text, "handle %#x, bound as %#llx", handle, static_cast<unsigned long long>(device));
            BOOST_LOG(info) << "PS5 virtual "sv << what << " of a device's profile seated ("sv << text << ')';
            return true;
        }
    }
    BOOST_LOG(warning) << "PS5 virtual "sv << what << " of a device's profile not seated ("sv << rc << "), so it was removed"sv;
    return false;
}

// The keyboard and the mouse of a kbm_t, added on first use. A release gets the one there is, never a new one.
static ps_keyboard *keyboard_of(input_raw_t &input, kbm_t &kbm, bool release) {
    if (release) return kbm.keyboard;
    if (!kbm.keyboard && !kbm.keyboard_failed) {
        if (const int rc = ps_keyboard_open(kbm.user ? kbm.user : input.user, &kbm.keyboard); rc) {
            BOOST_LOG(error) << "PS5 virtual keyboard failed: "sv << rc;
            kbm.keyboard_failed = true;
        } else if (kbm.user && !seat_kbm_device(ps_keyboard_device_id(kbm.keyboard), kbm.user, "keyboard", 3)) {
            ps_keyboard_close(kbm.keyboard);
            kbm.keyboard = nullptr;
            kbm.keyboard_failed = true;
        } else {
            BOOST_LOG(info) << "PS5 virtual keyboard attached"sv << (kbm.user ? " in its device's profile"sv : ""sv);
            std::this_thread::sleep_for(device_settle);
        }
    }
    return kbm.keyboard;
}

static ps_mouse *mouse_of(input_raw_t &input, kbm_t &kbm, bool release) {
    if (release) return kbm.mouse;
    if (!kbm.mouse && !kbm.mouse_failed) {
        if (const int rc = ps_mouse_open(kbm.user ? kbm.user : input.user, &kbm.mouse); rc) {
            BOOST_LOG(error) << "PS5 virtual mouse failed: "sv << rc;
            kbm.mouse_failed = true;
        } else if (kbm.user && !seat_kbm_device(ps_mouse_device_id(kbm.mouse), kbm.user, "mouse", 4)) {
            ps_mouse_close(kbm.mouse);
            kbm.mouse = nullptr;
            kbm.mouse_failed = true;
        } else {
            BOOST_LOG(info) << "PS5 virtual mouse attached"sv << (kbm.user ? " in its device's profile"sv : ""sv);
            std::this_thread::sleep_for(device_settle);
        }
    }
    return kbm.mouse;
}

static void send_mouse(kbm_t &kbm, ps_mouse *mouse, int dx, int dy, int8_t wheel = 0, int8_t tilt = 0) {
    // A large jump goes out in several reports rather than wrapping around.
    do {
        const auto x = static_cast<int16_t>(std::clamp(dx, -32767, 32767));
        const auto y = static_cast<int16_t>(std::clamp(dy, -32767, 32767));
        ps_mouse_send(mouse, x, y, kbm.mouse_buttons, wheel, tilt);
        dx -= x;
        dy -= y;
        wheel = tilt = 0;
    } while (dx || dy);
}

void move_mouse(input_t &input, int deltaX, int deltaY) {
    std::lock_guard lock(input->mutex);
    kbm_t *kbm = kbm_for_input(*input, false);
    if (!kbm) return;
    // Sunshine nudges the pointer by a pixel and back at every stream start; that alone does not add a mouse.
    if (!kbm->mouse && std::abs(deltaX) <= 1 && std::abs(deltaY) <= 1) return;
    ps_mouse *mouse = mouse_of(*input, *kbm, false);
    if (!mouse) return;
    kbm->pointer.reset();
    send_mouse(*kbm, mouse, deltaX, deltaY);
}

void move_mouse(input_t &input, const touch_port_t &, int deltaX, int deltaY) { move_mouse(input, deltaX, deltaY); }

// The console's pointer is relative only, so a client in absolute mode moves it by the difference to its last position.
void abs_mouse(input_t &input, const touch_port_t &, float x, float y) {
    std::lock_guard lock(input->mutex);
    kbm_t *kbm = kbm_for_input(*input, false);
    if (!kbm) return;
    ps_mouse *mouse = mouse_of(*input, *kbm, false);
    if (!mouse) return;
    const auto last = std::exchange(kbm->pointer, std::pair{x, y});
    if (!last) return;
    send_mouse(*kbm, mouse, static_cast<int>(std::lround(x - last->first)), static_cast<int>(std::lround(y - last->second)));
}

void button_mouse(input_t &input, int button, bool release) {
    const uint8_t bit = prospero::mouse_button_bit(button);
    if (!bit) return;
    std::lock_guard lock(input->mutex);
    kbm_t *kbm = kbm_for_input(*input, release);
    if (!kbm) return;
    const uint8_t buttons = release ? kbm->mouse_buttons & ~bit : kbm->mouse_buttons | bit;
    if (buttons == kbm->mouse_buttons) return;
    ps_mouse *mouse = mouse_of(*input, *kbm, release);
    if (!mouse) return;
    kbm->mouse_buttons = buttons;
    if (release) hold_from(kbm->button_down_at);
    send_mouse(*kbm, mouse, 0, 0);
    if (!release) kbm->button_down_at = std::chrono::steady_clock::now();
}

void scroll(input_t &input, int distance) {
    std::lock_guard lock(input->mutex);
    kbm_t *kbm = kbm_for_input(*input, false);
    if (!kbm) return;
    ps_mouse *mouse = mouse_of(*input, *kbm, false);
    if (!mouse) return;
    if (const int8_t steps = kbm->wheel.add(distance)) send_mouse(*kbm, mouse, 0, 0, steps, 0);
}

void hscroll(input_t &input, int distance) {
    std::lock_guard lock(input->mutex);
    kbm_t *kbm = kbm_for_input(*input, false);
    if (!kbm) return;
    ps_mouse *mouse = mouse_of(*input, *kbm, false);
    if (!mouse) return;
    if (const int8_t steps = kbm->tilt.add(distance)) send_mouse(*kbm, mouse, 0, 0, 0, steps);
}

void keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t, bool) {
    std::lock_guard lock(input->mutex);
    kbm_t *kbm = kbm_for_input(*input, release);
    if (!kbm) return;
    // Only a key that changes what is held asks for the keyboard (an unmapped key or a repeat adds none).
    auto keys = kbm->keys;
    if (!keys.update(modcode, release)) return;
    ps_keyboard *keyboard = keyboard_of(*input, *kbm, release);
    if (!keyboard) return;
    kbm->keys = keys;
    if (release) hold_from(kbm->key_down_at);
    ps_keyboard_send(keyboard, kbm->keys.modifiers(), kbm->keys.usages(), kbm->keys.count());
    if (!release) kbm->key_down_at = std::chrono::steady_clock::now();
}

// Text from a client's on-screen keyboard: printable ASCII is typed key by key as on a US keyboard, and what the
// client holds stays held. Other characters have no key and are dropped.
void unicode(input_t &input, const char *utf8, int size) {
    std::lock_guard lock(input->mutex);
    kbm_t *kbm = kbm_for_input(*input, false);
    if (!kbm) return;
    ps_keyboard *keyboard = keyboard_of(*input, *kbm, false);
    if (!keyboard) return;
    const auto &held = kbm->keys;
    int dropped = 0;
    for (int i = 0; i < size; i++) {
        const auto key = prospero::ascii_key(utf8[i]);
        if (!key.usage) {
            dropped += (static_cast<uint8_t>(utf8[i]) & 0xC0) != 0x80;  // a UTF-8 sequence counts once
            continue;
        }
        std::array<uint8_t, prospero::KeyboardState::max_keys> usages{};
        const auto count = std::min<uint8_t>(held.count(), prospero::KeyboardState::max_keys - 1);
        std::copy_n(held.usages(), count, usages.begin());
        usages[count] = key.usage;
        const uint8_t modifiers = held.modifiers() | (key.shift ? 0x02 : 0);
        ps_keyboard_send(keyboard, modifiers, usages.data(), count + 1);
        std::this_thread::sleep_for(min_hold);
        ps_keyboard_send(keyboard, held.modifiers(), held.usages(), held.count());
        std::this_thread::sleep_for(min_hold);
    }
    if (dropped) BOOST_LOG(debug) << "PS5 virtual keyboard: "sv << dropped << " characters without a key"sv;
}

void touch_update(client_input_t *, const touch_port_t &, const touch_input_t &) {}
void pen_update(client_input_t *, const touch_port_t &, const pen_input_t &) {}
void gamepad_battery(input_t &, const gamepad_battery_t &) {}
}
