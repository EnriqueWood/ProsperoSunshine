// SPDX-License-Identifier: GPL-3.0-only
#include "app/psapp/host.hpp"

#include "app/psapp/experimental_list.h"

#include "app/psapp/autoload.hpp"
#include "app/psapp/io.hpp"
#include "app/psapp/web.hpp"
#include "platform/ps5/system.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <pthread.h>
#include <unistd.h>

namespace psapp
{

namespace
{

bool is_end(const std::string &line)
{
    return line == "END" || line.rfind("ERR", 0) == 0;
}

constexpr const char *kNeedsHost = "Start the host first: on this firmware the app reaches its files through it";

bool any_line(const std::string &)
{
    return true;
}

// The value of `name=` in a STATUS line ("OK running=1 app=PS5 Screen pending=0"); app= may hold
// spaces, so a value runs to the next " <word>=".
std::string status_field(const std::string &line, const std::string &name)
{
    const std::size_t start = line.find(" " + name + "=");
    if (start == std::string::npos)
        return {};
    const std::size_t value = start + name.size() + 2;
    std::size_t end = value;
    for (;;)
    {
        const std::size_t space = line.find(' ', end);
        if (space == std::string::npos)
            return line.substr(value);
        const std::size_t eq = line.find('=', space);
        const std::size_t next_space = line.find(' ', space + 1);
        if (eq != std::string::npos && (next_space == std::string::npos || eq < next_space))
            return line.substr(value, space - value);
        end = space + 1;
    }
}

// UTC as ISO 8601. The title's libc has no localtime_r, so the date is worked out here
// (days to civil date, Howard Hinnant's algorithm).
// The "name" of every entry of root.named_devices in sunshine_state.json. Sunshine writes the file
// with boost::property_tree, one key a line, so a scan for the keys is enough here.
std::vector<std::string> paired_names(const std::string &json)
{
    std::vector<std::string> names;
    std::size_t at = json.find("\"named_devices\"");
    if (at == std::string::npos)
        return names;
    const std::size_t end = json.rfind(']'); // the last one: a name may hold one too
    for (;;)
    {
        at = json.find("\"name\"", at);
        if (at == std::string::npos || (end != std::string::npos && at > end))
            break;
        const std::size_t open = json.find('"', json.find(':', at) + 1);
        const std::size_t close = open == std::string::npos ? open : json.find('"', open + 1);
        if (close == std::string::npos)
            break;
        names.push_back(json.substr(open + 1, close - open - 1));
        at = close + 1;
    }
    return names;
}

std::string iso_time()
{
    const long long now = static_cast<long long>(std::time(nullptr));
    long long days = now / 86400;
    const long long seconds = now % 86400;
    days += 719468;
    const long long era = days / 146097;
    const long long doe = days - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const long long day = doy - (153 * mp + 2) / 5 + 1;
    const long long month = mp < 10 ? mp + 3 : mp - 9;
    const long long year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    char text[40];
    std::snprintf(text, sizeof text, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld+0000", year, month, day,
                  seconds / 3600, seconds / 60 % 60, seconds % 60);
    return text;
}

void sleep_ms(int ms)
{
    hui::sys::sleep_us(static_cast<std::uint32_t>(ms) * 1000u);
}

} // namespace

Host::Host() = default;

Host::~Host()
{
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
    wake_.notify_all();
}

void Host::start()
{
    if (started_)
        return;
    started_ = true;
    pthread_t thread;
    // Never name threads: pthread_setname_np hangs on this platform (ps5-homebrew-ui).
    if (pthread_create(&thread, nullptr, &Host::thread_main, this) != 0)
        hui::sys::log("[PSAPP] worker thread creation failed");
    else
        pthread_detach(thread);
}

void *Host::thread_main(void *self)
{
    static_cast<Host *>(self)->run();
    return nullptr;
}

Snapshot Host::snapshot()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void Host::push(Job job)
{
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.push_back(std::move(job));
    wake_.notify_all();
}

void Host::submit_pin(const std::string &id, const std::string &pin, const std::string &name)
{
    push({Kind::pin, id, pin, name, {}, false});
}
void Host::deny_pairing(const std::string &id, const std::string &name)
{
    push({Kind::deny, id, name, {}, {}, false});
}
void Host::rename_device(const std::string &id, const std::string &name)
{
    push({Kind::rename_device, id, name, {}, {}, false});
}
void Host::remove_device(const std::string &id, const std::string &name)
{
    push({Kind::remove_device, id, name, {}, {}, false});
}
void Host::end_stream(const std::string &id, const std::string &name)
{
    push({Kind::end_stream, id, name, {}, {}, false});
}
void Host::set_web_login(const std::string &user, const std::string &password)
{
    push({Kind::web_set, user, password, {}, {}, false});
}
void Host::web_login_on()
{
    push({Kind::web_on, {}, {}, {}, {}, false});
}
void Host::web_login_off()
{
    push({Kind::web_off, {}, {}, {}, {}, false});
}
void Host::web_login_forget()
{
    push({Kind::web_forget, {}, {}, {}, {}, false});
}
void Host::start_host()
{
    push({Kind::start, {}, {}, {}, {}, false});
}
void Host::stop_host()
{
    push({Kind::stop, {}, {}, {}, {}, false});
}
void Host::restart_host()
{
    push({Kind::restart, {}, {}, {}, {}, false});
}
void Host::save_config(const Values &values, bool restart)
{
    push({Kind::save, {}, {}, {}, values, restart});
}
void Host::set_autoload(bool on)
{
    push({on ? Kind::autoload_on : Kind::autoload_off, {}, {}, {}, {}, false});
}
void Host::refresh()
{
    push({Kind::refresh, {}, {}, {}, {}, false});
}
void Host::make_report()
{
    push({Kind::report, {}, {}, {}, {}, false});
}
void Host::serve_report(bool on)
{
    serve_ = on;
}
void Host::set_app_version(const std::string &version)
{
    std::lock_guard<std::mutex> lock(mutex_);
    app_version_ = version;
}
std::string Host::report()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return report_;
}

void Host::set_busy(const std::string &text)
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_.busy = text;
}

void Host::finish(const std::string &message, bool ok)
{
    hui::sys::log("[PSAPP] %s: %s", ok ? "done" : "failed", message.c_str());
    std::lock_guard<std::mutex> lock(mutex_);
    state_.busy.clear();
    state_.message = message;
    state_.message_ok = ok;
    ++state_.message_serial;
}

void Host::run()
{
    hui::sys::log("[PSAPP] worker started");
    own_firmware_ = io::firmware_version();
    hui::sys::log("[PSAPP] firmware 0x%x", own_firmware_);
    prepare_files();
    read_files();
    poll();
    for (;;)
    {
        Job job;
        bool have = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, std::chrono::seconds(2), [this] { return stop_ || !jobs_.empty(); });
            if (stop_)
                return;
            if (!jobs_.empty())
            {
                job = std::move(jobs_.front());
                jobs_.pop_front();
                have = true;
            }
        }
        if (have)
        {
            switch (job.kind)
            {
            case Kind::pin:
                do_pin(job);
                break;
            case Kind::deny:
                do_deny(job);
                break;
            case Kind::rename_device:
                do_rename_device(job);
                break;
            case Kind::remove_device:
                do_remove_device(job);
                break;
            case Kind::end_stream:
                do_end_stream(job);
                break;
            case Kind::web_set:
            case Kind::web_on:
            case Kind::web_off:
            case Kind::web_forget:
                do_web(job);
                break;
            case Kind::start:
                do_start();
                break;
            case Kind::stop:
                do_stop();
                break;
            case Kind::restart:
                do_restart();
                break;
            case Kind::save:
                do_save(job);
                break;
            case Kind::autoload_on:
                do_autoload(true);
                break;
            case Kind::autoload_off:
                do_autoload(false);
                break;
            case Kind::refresh:
                read_files();
                finish("Read sunshine.conf and the autoload list again", true);
                break;
            case Kind::report:
                do_report();
                break;
            }
        }
        poll();
    }
}

// The data folder, and the host ELF the app carries copied to it when it is missing or another
// build. Start, restart and autoload all use that one copy.
void Host::prepare_files()
{
    if (!io::sandboxed() && !io::make_dirs(kDataDir))
        hui::sys::log("[PSAPP] cannot use %s: %s", kDataDir, io::describe_path(kDataDir).c_str());
    const bool bundled = io::exists(kBundledElf);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.bundled_elf = bundled;
    }
    if (io::sandboxed())
    {
        prepare_through_host();
        return;
    }
    if (bundled && !io::same_content(kBundledElf, kHostElf))
    {
        const std::string error = io::copy_file(kBundledElf, kHostElf);
        if (error.empty())
            hui::sys::log("[PSAPP] installed the bundled host to %s", kHostElf);
        else
            finish("Could not install the host: " + error, false);
    }
    // An autoload slot made by an earlier version of the app follows the installed host.
    std::string list;
    if (io::read_text(autoload::kListPath, &list) && autoload::contains(list, autoload::kSlotKey) &&
        io::exists(kHostElf) && !io::same_content(kHostElf, autoload::kSlotElf))
    {
        const std::string error = sync_slot();
        hui::sys::log("[PSAPP] autoload slot updated: %s", error.empty() ? "ok" : error.c_str());
    }
}

// The same where this title has no /data: the host copies what the app carries, and keeps an autoload slot current.
void Host::prepare_through_host()
{
    const io::Exchange install = io::exchange(kControlPort, "INSTALL\n", any_line, 30000);
    bridged_ = !install.connect_refused;
    if (!bridged_)
        return;
    const std::string answer = install.lines.empty() ? install.error : install.lines.front();
    hui::sys::log("[PSAPP] no /data here, going through the host; its copy of the ELF: %s", answer.c_str());
    if (answer.rfind("OK ", 0) != 0)
        return;
    const io::Exchange status = io::exchange(kControlPort, "AUTOLOAD\n", any_line, 10000);
    if (!status.lines.empty() && status_field(status.lines.front(), "list") == "1" && status_field(status.lines.front(), "slot") != "1")
    {
        const io::Exchange sync = io::exchange(kControlPort, "AUTOLOAD ON\n", any_line, 20000);
        hui::sys::log("[PSAPP] autoload slot updated by the host: %s", sync.lines.empty() ? sync.error.c_str() : sync.lines.front().c_str());
    }
}

void Host::read_files()
{
    Values values;
    std::string error;
    std::string text;
    if (io::read_text(kConfPath, &text))
    {
        const Config config = Config::parse(text);
        for (const OptionSpec &spec : option_specs())
            values[spec.key] = option_value(config, spec);
    }
    else
    {
        for (const OptionSpec &spec : option_specs())
            values[spec.key] = std::nullopt;
        if (io::exists(kConfPath))
            error = "sunshine.conf could not be read";
    }
    std::string state;
    std::vector<std::string> paired;
    if (io::read_text(kStatePath, &state))
        paired = paired_names(state);
    const std::string address = io::local_ipv4();
    std::string list, manager;
    const bool have_list = io::read_text(autoload::kListPath, &list);
    const bool have_manager = io::read_text(autoload::kConfigPath, &manager);
    if (!have_manager && io::exists(autoload::kConfigPath))
        hui::sys::log("[PSAPP] cannot read %s (errno %d)", autoload::kConfigPath, errno);
    bool autoload_on = have_list && autoload::contains(list, autoload::kSlotKey);
    // The manager may keep its config unreadable for a title; then nothing is known and no warning
    // is shown, unless the host (root) can tell.
    bool manager_on = !have_manager || autoload::manager_autoload_enabled(manager);
    if ((!have_list && io::exists(autoload::kListPath)) || (!have_manager && io::exists(autoload::kConfigPath)))
    {
        const io::Exchange reply = io::exchange(kControlPort, "AUTOLOAD\n", any_line, 3000);
        if (!reply.lines.empty() && reply.lines.front().rfind("OK ", 0) == 0)
        {
            const std::string &line = reply.lines.front();
            manager_on = status_field(line, "manager") == "1";
            if (!have_list)
            {
                autoload_on = status_field(line, "list") == "1";
                // prepare_files cannot keep this slot current: the host takes over.
                if (autoload_on && status_field(line, "slot") != "1")
                {
                    const io::Exchange sync = io::exchange(kControlPort, "AUTOLOAD ON\n", any_line, 20000);
                    hui::sys::log("[PSAPP] autoload slot updated by the host: %s",
                                  sync.lines.empty() ? sync.error.c_str() : sync.lines.front().c_str());
                }
            }
        }
    }
    const bool manager_present = io::exists("/data/pldmgr") || (io::sandboxed() && !io::port_open(kControlPort));
    const long long host_elf_size = io::file_size(kHostElf);
    std::lock_guard<std::mutex> lock(mutex_);
    state_.files_read = true;
    state_.config = values;
    state_.config_error = error;
    state_.autoload = autoload_on;
    state_.manager_autoload = manager_on;
    // Without /data and without the host nothing is known; do_autoload says what to do.
    state_.manager_present = manager_present;
    state_.host_elf_size = host_elf_size;
    state_.paired = paired;
    if (!address.empty())
        state_.address = address;
}

void Host::poll()
{
    const io::Exchange reply = io::exchange(kControlPort, "STATUS\nPAIRINGS\n", is_end, 3000);
    bool running = false, streaming = false;
    std::string app, client, client_address, error;
    bool client_known = false;
    std::vector<Pairing> pairings;
    if (reply.connect_refused)
    {
        // Nobody listens: the host is not running.
    }
    else if (!reply.error.empty() && reply.lines.empty())
    {
        running = true; // something holds the port but did not answer (a PIN in progress)
        error = reply.error;
    }
    else
    {
        running = true;
        for (std::size_t i = 0; i < reply.lines.size(); ++i)
        {
            const std::string &line = reply.lines[i];
            if (i == 0)
            {
                if (line.rfind("OK ", 0) != 0)
                {
                    error = line;
                    break;
                }
                streaming = status_field(line, "running") == "1";
                app = status_field(line, "app");
                client = status_field(line, "client");
                client_address = status_field(line, "address");
                client_known = line.find(" client=") != std::string::npos;
                continue;
            }
            if (line == "END" || line.rfind("OK ", 0) != 0)
                continue;
            // "OK <id> <name> <address>": the name may hold spaces, the id and address do not.
            const std::string rest = line.substr(3);
            const std::size_t first = rest.find(' ');
            const std::size_t last = rest.rfind(' ');
            Pairing p;
            p.id = rest.substr(0, first);
            if (first != std::string::npos && last > first)
            {
                p.name = rest.substr(first + 1, last - first - 1);
                p.address = rest.substr(last + 1);
            }
            else if (first != std::string::npos)
            {
                p.name = rest.substr(first + 1);
            }
            pairings.push_back(p);
        }
    }
    // Without /data the files come from the host, so they are read again when it appears.
    if (io::sandboxed())
    {
        if (running && !bridged_)
        {
            prepare_files();
            read_files();
        }
        else if (!running)
        {
            bridged_ = false;
        }
    }
    // The host's breadcrumbs: small, and the file is there after a crash too.
    std::string host_file;
    io::read_text(kHostDiagPath, &host_file);
    const diag::HostLog host_log = diag::parse_host_log(host_file);
    const std::uint32_t firmware = own_firmware_ != 0 ? own_firmware_ : host_log.firmware;
    const diag::Notice notice = diag::notice(firmware, host_log, running);
    bool devices_known = false;
    std::vector<diag::Device> devices;
    if (running && error.empty())
    {
        const io::Exchange list = io::exchange(kControlPort, "DEVICES\n", is_end, 3000);
        devices_known = !list.lines.empty() && list.lines.back() == "END";
        for (const std::string &line : list.lines)
        {
            diag::Device device;
            if (diag::parse_device_line(line, &device))
                devices.push_back(device);
        }
    }
    bool streams_known = false;
    std::vector<diag::Stream> streams;
    if (running && error.empty() && devices_known)
    {
        const io::Exchange list = io::exchange(kControlPort, "STREAMS\n", is_end, 3000);
        streams_known = !list.lines.empty() && list.lines.back() == "END";
        for (const std::string &line : list.lines)
        {
            diag::Stream stream;
            if (streams_known && diag::parse_stream_line(line, &stream))
                streams.push_back(stream);
        }
    }
    // The web page's login: whether there is one, for whom, and whether the page is on.
    bool web_known = false;
    web::LoginState web_state;
    if (running && error.empty())
    {
        const io::Exchange reply = io::exchange(kControlPort, web::kQuery, any_line, 3000);
        if (!reply.lines.empty())
        {
            if (const auto state = web::parse_state(reply.lines.front()))
            {
                web_known = true;
                web_state = *state;
            }
        }
    }
    // The host offers the report for some seconds after each of these.
    bool served = false;
    if (serve_ && running)
    {
        const io::Exchange offer = io::exchange(kControlPort, "REPORT\n", any_line, 1000);
        served = !offer.lines.empty() && offer.lines.front().rfind("OK", 0) == 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    state_.report_served = served;
    state_.firmware = firmware;
    state_.notice = notice;
    state_.polled = true;
    state_.running = running;
    state_.streaming = streaming;
    state_.app = app;
    state_.client = client;
    state_.client_address = client_address;
    state_.client_known = client_known;
    state_.pairings = pairings;
    state_.poll_error = error;
    state_.devices_known = devices_known;
    state_.devices = devices;
    state_.streams_known = streams_known;
    state_.streams = streams;
    state_.web_known = web_known;
    state_.web_user = web_state.user;
    state_.web_enabled = web_state.enabled;
    state_.web_switchable = web_state.switchable;
}

bool Host::wait_control(bool open, int timeout_ms)
{
    for (int waited = 0; waited <= timeout_ms; waited += 250)
    {
        if (io::port_open(kControlPort, 250) == open)
            return true;
        sleep_ms(250);
    }
    return false;
}

std::string Host::launch()
{
    // The copy in the data folder, or the app's own where this title has no /data.
    const char *elf = !io::sandboxed() && io::exists(kHostElf) ? kHostElf : kBundledElf;
    if (!io::exists(elf))
        return std::string("no host at ") + kHostElf + " (" + io::describe_path(kHostElf) + ")";
    hui::sys::log("[PSAPP] sending %s to the ELF loader", elf);
    const std::string error = io::send_file(kLoaderPort, elf);
    if (!error.empty())
        return error;
    if (!wait_control(true, 30000))
        return "the host did not come up within 30 s (see sunshine.log)";
    return {};
}

std::string Host::quit_and_wait()
{
    const io::Exchange reply = io::exchange(kControlPort, "QUIT\n", any_line, 3000);
    if (reply.connect_refused)
        return {}; // not running
    if (!reply.error.empty())
        return "QUIT: " + reply.error;
    if (!wait_control(false, 20000))
        return "the host did not stop within 20 s";
    // The control socket closes first; give the rest of the host a moment to let go of its ports
    // and the capture service.
    sleep_ms(2000);
    return {};
}

std::string Host::sync_slot()
{
    if (!io::make_dirs(autoload::kSlotDir))
        return std::string("cannot create ") + autoload::kSlotDir;
    std::string error;
    if (!io::same_content(kHostElf, autoload::kSlotElf))
        error = io::copy_file(kHostElf, autoload::kSlotElf);
    if (error.empty())
        error = io::write_text(autoload::kSlotMeta, autoload::slot_meta("prospero_sunshine.elf", iso_time()));
    return error;
}

void Host::do_pin(const Job &job)
{
    set_busy("Pairing: waiting for Moonlight...");
    const std::string request = "PIN " + job.a + " " + job.b + (job.c.empty() ? "" : " " + job.c) + "\n";
    // The host answers once Moonlight finished its side of the handshake.
    const io::Exchange reply = io::exchange(kControlPort, request, any_line, 60000);
    if (!reply.error.empty())
    {
        finish("Pairing failed: " + reply.error, false);
        return;
    }
    const std::string answer = reply.lines.empty() ? std::string() : reply.lines.front();
    if (answer == "OK paired")
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++state_.paired_serial;
        }
        read_files();
        // Its profiles are chosen on the web page only, for now: until then the console asks who uses each controller.
        finish("Paired with " + (job.c.empty() ? job.a : job.c) + ". Choose its profiles on the web page.", true);
    }
    else
    {
        finish("Pairing failed (" + answer + "). Check the PIN Moonlight shows.", false);
    }
}

void Host::do_deny(const Job &job)
{
    set_busy("Denying " + job.b + "...");
    const io::Exchange reply = io::exchange(kControlPort, diag::deny_request(job.a), any_line, 5000);
    if (reply.connect_refused)
    {
        finish("The host is not running", false);
        return;
    }
    const std::string answer = reply.lines.empty() ? std::string() : reply.lines.front();
    switch (diag::parse_deny(answer))
    {
    case diag::DenyOutcome::denied:
        finish("Denied " + job.b + ". Moonlight was told no.", true);
        break;
    case diag::DenyOutcome::gone:
        finish(job.b + " is not waiting anymore", true);
        break;
    case diag::DenyOutcome::unsupported:
        finish("The running host can't deny yet. Restart it to use this app's host.", false);
        break;
    case diag::DenyOutcome::failed:
        finish("Could not deny " + job.b, false);
        break;
    }
}

void Host::do_rename_device(const Job &job)
{
    set_busy("Renaming...");
    const io::Exchange reply = io::exchange(kControlPort, "RENAME " + job.a + " " + job.b + "\n", any_line, 5000);
    const bool ok = reply.error.empty() && !reply.lines.empty() && reply.lines.front().rfind("OK", 0) == 0;
    read_files();
    finish(ok ? "Renamed it to " + job.b : "Could not rename the device", ok);
}

void Host::do_remove_device(const Job &job)
{
    set_busy("Removing " + job.b + "...");
    // The host answers once a running stream of this device has ended.
    const io::Exchange reply = io::exchange(kControlPort, "UNPAIR " + job.a + "\n", any_line, 20000);
    const bool ok = reply.error.empty() && !reply.lines.empty() && reply.lines.front().rfind("OK", 0) == 0;
    read_files();
    finish(ok ? "Removed " + job.b + ". It has to pair again to stream." : "Could not remove " + job.b, ok);
}

void Host::do_end_stream(const Job &job)
{
    set_busy("Ending the stream of " + job.b + "...");
    // The host answers once the device's stream has ended.
    const io::Exchange reply = io::exchange(kControlPort, "ENDSTREAM " + job.a + "\n", any_line, 20000);
    const std::string answer = reply.lines.empty() ? std::string() : reply.lines.front();
    if (reply.error.empty() && answer.rfind("OK", 0) == 0)
        finish("Ended the stream of " + job.b + ". The game keeps running.", true);
    else if (answer == "ERR not streaming")
        finish(job.b + " is not streaming anymore", true);
    else
        finish("Could not end the stream of " + job.b, false);
}

// The request line holds the password: it goes to the socket and nowhere else, and the messages (which
// finish() logs) hold only the user name and the host's reason.
void Host::do_web(const Job &job)
{
    const bool set = job.kind == Kind::web_set;
    std::string request, busy;
    switch (job.kind)
    {
    case Kind::web_set:
        request = web::set_request(job.a, job.b);
        busy = "Setting the web page's login...";
        break;
    case Kind::web_on:
        request = web::kOn;
        busy = "Turning the web page on...";
        break;
    case Kind::web_forget:
        request = web::kForget;
        busy = "Forgetting the web page's login...";
        break;
    default:
        request = web::kOff;
        busy = "Turning the web page off...";
        break;
    }
    set_busy(busy);
    // The host hashes the password before it answers.
    const io::Exchange reply = io::exchange(kControlPort, request, any_line, 15000);
    std::fill(request.begin(), request.end(), '\0');
    if (reply.connect_refused)
    {
        finish("Start the host first: it keeps the web page's login", false);
        return;
    }
    if (reply.lines.empty())
    {
        finish("Web page: " + (reply.error.empty() ? std::string("no answer") : reply.error), false);
        return;
    }
    const web::Outcome outcome = web::parse_outcome(reply.lines.front());
    if (outcome.unknown)
        finish("The running host has no web page yet. Restart it to use this app's host.", false);
    else if (!outcome.ok)
        finish("Web page: " + outcome.reason, false);
    else if (set)
        finish("The web page is on. Log in as " + job.a + ".", true);
    else if (job.kind == Kind::web_on)
    {
        std::string user;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            user = state_.web_user;
        }
        finish(user.empty() ? std::string("The web page is on") : "The web page is on. Log in as " + user + ".", true);
    }
    else if (job.kind == Kind::web_forget)
        finish("The web page's login is gone, and the page is off", true);
    else
        finish("The web page is off", true);
}

void Host::do_start()
{
    if (io::port_open(kControlPort))
    {
        finish("The host is running already", true);
        return;
    }
    set_busy("Starting the host...");
    const std::string error = launch();
    read_files();
    finish(error.empty() ? "The host is running" : "Start failed: " + error, error.empty());
}

void Host::do_stop()
{
    set_busy("Stopping the host...");
    const std::string error = quit_and_wait();
    finish(error.empty() ? "The host has stopped" : "Stop failed: " + error, error.empty());
}

void Host::do_restart()
{
    set_busy("Stopping the host...");
    std::string error = quit_and_wait();
    if (error.empty())
    {
        set_busy("Starting the host...");
        error = launch();
        read_files();
    }
    finish(error.empty() ? "The host restarted with the saved settings" : "Restart failed: " + error,
           error.empty());
}

void Host::do_save(const Job &job)
{
    set_busy("Saving...");
    if (io::sandboxed() && !io::port_open(kControlPort))
    {
        finish(kNeedsHost, false);
        return;
    }
    std::string text;
    // A file that can't be read now must not be replaced by one with only these settings.
    if (!io::read_text(kConfPath, &text) && errno != ENOENT)
    {
        finish("Saving failed: sunshine.conf could not be read", false);
        return;
    }
    Config config = Config::parse(text);
    for (const auto &[key, value] : job.values)
    {
        const OptionSpec *spec = find_option(key);
        if (spec == nullptr)
            continue;
        if (value)
        {
            if (const auto problem = validate(*spec, *value))
            {
                finish(std::string(spec->label) + " " + *problem, false);
                return;
            }
        }
        config.set(key, value);
    }
    if (!io::make_dirs(kDataDir))
    {
        finish(std::string("Cannot use ") + kDataDir + ": " + io::describe_path(kDataDir), false);
        return;
    }
    const std::string error = io::write_text(kConfPath, config.text());
    read_files();
    if (!error.empty())
    {
        finish("Saving failed: " + error, false);
        return;
    }
    if (!job.flag)
    {
        finish(io::port_open(kControlPort) ? "Saved. Restart the host to use the new settings." : "Saved",
               true);
        return;
    }
    do_restart();
}

// The edit as this title may do it. Error text, empty on success.
std::string Host::autoload_direct(bool on)
{
    if (io::sandboxed())
        return "this title has no /data";
    std::string list;
    const bool have = io::read_text(autoload::kListPath, &list);
    if (!have && io::exists(autoload::kListPath))
        return "cannot read autoload.txt: " + io::open_error(autoload::kListPath);
    if (!on)
        return have ? io::write_text(autoload::kListPath, autoload::without_entry(list, autoload::kSlotKey)) : std::string();
    std::string error = sync_slot();
    if (error.empty())
        error = io::write_text(autoload::kListPath, autoload::with_entry(list, autoload::kSlotKey));
    return error;
}

void Host::do_autoload(bool on)
{
    set_busy(on ? "Turning autoload on..." : "Turning autoload off...");
    std::string error;
    if (io::sandboxed() && !io::port_open(kControlPort))
        error = kNeedsHost;
    else if (on && !io::exists("/data/pldmgr"))
        error = "the Payload Manager is not installed (/data/pldmgr is missing)";
    else if (on && !io::exists(kHostElf))
        error = std::string("no host at ") + kHostElf;
    else
    {
        error = autoload_direct(on);
        // The manager keeps files it saved to itself (mode 0600), and a title may not touch those. The
        // host runs as root.
        if (!error.empty())
        {
            hui::sys::log("[PSAPP] autoload directly: %s", error.c_str());
            const io::Exchange reply = io::exchange(kControlPort, on ? "AUTOLOAD ON\n" : "AUTOLOAD OFF\n", any_line, 20000);
            if (!reply.lines.empty() && reply.lines.front().rfind("OK ", 0) == 0)
                error.clear();
            else if (reply.connect_refused)
                error += ". Start the host, then try again.";
            else if (!reply.lines.empty())
                error += " (host: " + reply.lines.front() + ")";
        }
    }
    read_files();
    if (!error.empty())
        finish("Autoload: " + error, false);
    else
        finish(on ? "The Payload Manager starts the host at boot" : "The host no longer starts at boot", true);
}

// Everything someone else needs to tell why streaming fails here, fit to paste into a public issue:
// no addresses, device names, ids or game titles (diag::scrub).
bool Host::same_as_bundled()
{
    if (!io::sandboxed())
        return io::same_content(kBundledElf, kHostElf);
    const io::Exchange check = io::exchange(kControlPort, "INSTALL CHECK\n", any_line, 10000);
    return !check.lines.empty() && check.lines.front() == "OK same";
}

std::string Host::build_report()
{
    const Snapshot snap = snapshot();
    std::string app_version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        app_version = app_version_;
    }
    std::string host_file, log_file;
    const bool have_host_file = io::read_text(kHostDiagPath, &host_file);
    const bool have_log_file = io::read_text(kHostLogPath, &log_file, 4u << 20);
    const diag::HostLog host_log = diag::parse_host_log(host_file);

    std::string out = "ProsperoSunshine diagnostics\n\n";
    const auto line = [&out](const std::string &name, const std::string &value) { out += name + ": " + value + "\n"; };
    line("App", app_version.empty() ? "unknown" : app_version);
    char code[16];
    std::snprintf(code, sizeof code, "%#x", snap.firmware);
    line("Firmware", snap.firmware == 0 ? std::string("unknown")
                                        : diag::firmware_text(snap.firmware) + " (" + code + ")" +
                                              (diag::firmware_verified(snap.firmware) ? ", verified" : ", untested"));
    line("Host", std::string(snap.running ? "running" : "not running") + (snap.streaming ? ", streaming" : "") +
                     (host_log.version.empty() ? "" : ", version " + host_log.version + ", commit " +
                                                          (host_log.commit.empty() ? "unknown" : host_log.commit)));
    line("Host ELF", std::to_string(snap.host_elf_size) + " bytes, " +
                         (same_as_bundled() ? "same as the app's" : "differs from the app's"));
    line("Capture", diag::capture_summary(host_log));
    line("Notice", snap.notice.text.empty() ? "none" : snap.notice.text);
    line("Paired devices", std::to_string(diag::paired_names(snap.running && snap.devices_known, snap.devices, snap.paired).size()));
    line("Network", snap.address.empty() ? "no LAN address" : "has a LAN address");
    const auto port = [](std::uint16_t number) { return std::string(io::port_open(number) ? "open" : "closed"); };
    line("Ports", "ELF loader " + std::to_string(kLoaderPort) + " " + port(kLoaderPort) + ", control " +
                      std::to_string(kControlPort) + " " + port(kControlPort) + ", stream " +
                      std::to_string(kStreamPort) + " " + port(kStreamPort));
    if (!snap.poll_error.empty())
        line("Control socket", snap.poll_error);
    line("Web page", !snap.web_known      ? "unknown"
                     : snap.web_enabled   ? "on"
                     : snap.web_user.empty() ? "off, no login"
                                             : "off, login kept");

    out += "\nPaths (this title's view, uid " + std::to_string(::getuid()) + "):\n";
    for (const char *path : {kDataDir, kConfPath, kHostElf, kHostDiagPath, kHostLogPath, "/data/pldmgr", autoload::kListPath,
                             autoload::kConfigPath, autoload::kSlotDir, autoload::kSlotElf})
        out += std::string("  ") + path + ": " + io::describe_path(path) + "\n";

    out += "\nSettings changed from the defaults:\n";
    bool any = false;
    for (const auto &[key, value] : snap.config)
    {
        // The console's name stays out; the other options say nothing about anyone.
        if (!value)
            continue;
        out += "  " + key + " = " + (key == "sunshine_name" ? std::string("<name>") : *value) + "\n";
        any = true;
    }
    if (!any)
        out += "  none\n";

    std::vector<std::string> names = snap.paired;
    for (const diag::Device &device : snap.devices)
        names.push_back(device.name);
    const auto name = snap.config.find("sunshine_name");
    if (name != snap.config.end() && name->second)
        names.push_back(*name->second);
    if (!snap.client.empty())
        names.push_back(snap.client);
    std::vector<std::string> switches;
    for (int i = 0; i < PS_EXP_SWITCH_COUNT; ++i)
    {
        const char *key = ps_exp_switch_at(i)->key;
        const auto it = snap.config.find(key);
        if (it != snap.config.end() && it->second && *it->second == "1")
            switches.push_back(key);
    }
    out += "\nExperimental switches:\n";
    out += diag::scrub(diag::experimental_report(host_log, switches), names);
    out += "\nHost breadcrumbs (host_diag.log):\n";
    out += have_host_file ? diag::scrub(diag::tail(host_file, 120), names) : "  no file: " + io::open_error(kHostDiagPath) + "\n";
    out += "\nsunshine.log, last lines:\n";
    out += have_log_file ? diag::scrub(diag::tail(log_file, 80), names) : "  no file: " + io::open_error(kHostLogPath) + "\n";
    return out;
}

void Host::do_report()
{
    set_busy("Collecting the report...");
    read_files();
    poll();
    std::string text = build_report();
    const std::string error = io::write_text(kReportPath, text);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        report_ = std::move(text);
        ++state_.report_serial;
    }
    finish(error.empty() ? std::string("Report saved to ") + kReportPath : "Saving the report failed: " + error, error.empty());
}

} // namespace psapp
