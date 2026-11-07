// SPDX-License-Identifier: GPL-3.0-only
#include "app/psapp/screen.hpp"

#include "app/psapp/experimental_list.h"
#include "app/psapp/icons.hpp"
#include "app/psapp/web.hpp"
#include "platform/ps5/system.hpp"
#include "ui/glyphs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace psapp
{

using hui::Direction;
using hui::gfx::Align;
using hui::gfx::Color;
using hui::gfx::Rect;
using icons::Icon;
using InputAction = hui::Action;

namespace
{

// Night blue with one strong colour: the sun's amber.
const Color kInk = Color::rgb(0xf6f3ee);
const Color kMuted = Color::rgb(0xaab1c8);
const Color kFaint = Color::rgb(0x6f7694);
const Color kAccent = Color::rgb(0xffa62b);
const Color kAccentDeep = Color::rgb(0xff6a1a);
const Color kGood = Color::rgb(0x45d39a);
const Color kBad = Color::rgb(0xff6b6b);
const Color kClear = Color::rgb(0x000000, 0.0f);

constexpr float kMessageSeconds = 7.0f;
constexpr float kArmSeconds = 4.0f;
constexpr int kDeviceRows = 6; // the device list's rows in view
const Rect kPairedDevices{1220.0f, 740.0f, 604.0f, 172.0f};
constexpr int kReportVisible = 19; // lines of the report on screen
constexpr float kReportLine = 26.0f;

const Icon kOptionIcons[] = {Icon::tag, Icon::log, Icon::timer, Icon::plug, Icon::lock, Icon::globe, Icon::sliders, Icon::home};
// option_specs(): two rows of four, then a row with the Experimental switches
constexpr int kRowLength = 4;
constexpr int kFirstSwitch = 8;
// The Experimental row: the firmware switches, then surround sound, which no firmware check locks.
constexpr int kSwitchTiles = PS_EXP_SWITCH_COUNT + 1;
constexpr int kTiles = kFirstSwitch + kSwitchTiles;
// Under the tiles: Save, Save and restart, Revert, and at the right end the web page.
constexpr int kSettingsActions = 4;
// The web page's sheet: its height, where its rows start in it, and their pitch.
constexpr float kWebSheetHeight = 770.0f;
constexpr float kWebRowsTop = 474.0f;
constexpr float kWebRowStep = 68.0f;
constexpr int kWebRows = 4;

float approach(float value, float target, float dt, float speed)
{
    return value + (target - value) * (1.0f - std::exp(-speed * dt));
}

float ease_out(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

std::string shorten(const hui::ui::FontRef &font, const std::string &text, float size, float width)
{
    if (font.measure(text, size) <= width)
        return text;
    return font.font->fit(text + " \xE2\x80\xA6", size, width);
}

} // namespace

Screen::Screen(Host &host, const hui::ui::Fonts &fonts) : host_(host), fonts_(fonts)
{
    style_.theme.focus = kAccent.with_alpha(0.3f);
    style_.theme.primary = kAccent;
    keyboard_.style.theme = style_.theme;
    keyboard_.style.theme.surface = Color::rgb(0x1a2244, 0.9f);
    keyboard_.style.theme.surface_high = Color::rgb(0x232d55);
    keyboard_.style.theme.text = kInk;
    keyboard_.style.theme.primary = kAccent;
    keyboard_.style.theme.on_primary = Color::rgb(0x2a1604);
    keyboard_.style.panel = false;
    keyboard_.style.surfaces = false; // flat caps: frosted keys pick up the sun behind the sheet
    // The kit's standard set (Square deletes, Triangle is space, L2 shift) with R2 as Done, as on the console's own
    // keyboard; the symbols move to R1, which no keyboard sheet uses. Options stays Done too (handle_sheet).
    keyboard_.style.bindings = hui::ui::KeyboardBindings::standard();
    keyboard_.style.bindings.layout = InputAction::page_next;
    keyboard_.style.bindings.done = InputAction::jump_next;
}

// ---- state helpers ----------------------------------------------------------------------------

bool Screen::dirty() const
{
    return edits_loaded_ && edits_ != snap_.config;
}

std::string Screen::value_text(int index) const
{
    const OptionSpec &spec = option_specs()[static_cast<std::size_t>(index)];
    if (switch_not_needed(index))
        return diag::experimental_no_effect(snap_.firmware, spec.key) ? ps_exp_switch_at(index - kFirstSwitch)->no_effect_word
                                                                      : "Verified here"; // five tiles leave no room for more
    const auto it = edits_.find(spec.key);
    if (it == edits_.end() || !it->second)
        return spec.default_label;
    const std::string &value = *it->second;
    if (spec.kind == OptionKind::choice)
    {
        for (const Choice &choice : spec.choices)
        {
            if (value == choice.value)
                return choice.label;
        }
    }
    if (spec.kind == OptionKind::number && std::string(spec.key) == "ping_timeout")
        return value + " ms";
    if (spec.kind == OptionKind::toggle)
        return value == "1" ? "On" : "Off";
    return value;
}

bool Screen::switch_not_needed(int index) const
{
    return index >= kFirstSwitch && index < kFirstSwitch + PS_EXP_SWITCH_COUNT &&
           !diag::experimental_needed(snap_.firmware, option_specs()[static_cast<std::size_t>(index)].key);
}

int Screen::visible_pairings() const
{
    // While someone streams, the streams card takes the second card's place.
    return static_cast<int>(std::min<std::size_t>(snap_.pairings.size(), streams_shown() ? 1 : 2));
}

int Screen::list_rows() const
{
    const bool streams = sheet_drawn_ == Sheet::streams || sheet_drawn_ == Sheet::end_stream;
    return static_cast<int>(streams ? snap_.streams.size() : snap_.devices.size());
}

float Screen::appear(int order) const
{
    return ease_out((page_age_ - 0.045f * static_cast<float>(order)) / 0.42f);
}

Rect Screen::action_rect(int index) const
{
    static const float widths[3] = {318.0f, 318.0f, 400.0f};
    float x = 96.0f;
    for (int i = 0; i < index; ++i)
        x += widths[i] + 20.0f;
    return {x, 532.0f, widths[index], 88.0f};
}

Rect Screen::pairing_rect(int index) const
{
    return {96.0f + static_cast<float>(index) * 562.0f, 740.0f, 538.0f, 196.0f};
}

Rect Screen::streams_rect() const
{
    // Beside a pairing card, or in the empty pairing card's place.
    const int cards = visible_pairings();
    return cards == 0 ? Rect{96.0f, 740.0f, 1100.0f, 172.0f} : Rect{96.0f + static_cast<float>(cards) * 562.0f, 740.0f, 538.0f, 172.0f};
}

Rect Screen::devices_card() const
{
    const int rows = std::clamp(list_rows(), 1, kDeviceRows);
    const float height = 262.0f + static_cast<float>(rows) * 90.0f;
    return {960.0f - 520.0f, 540.0f - height / 2.0f + 10.0f, 1040.0f, height};
}

Rect Screen::device_row_rect(int visible) const
{
    const Rect card = devices_card();
    return {card.x + 50.0f, card.y + 216.0f + static_cast<float>(visible) * 90.0f, card.w - 100.0f, 80.0f};
}

Rect Screen::tile_rect(int index) const
{
    // The gap before the switches holds the section's caption.
    if (index >= kFirstSwitch)
    {
        const float width = (1728.0f - 20.0f * static_cast<float>(kSwitchTiles - 1)) / static_cast<float>(kSwitchTiles);
        return {96.0f + static_cast<float>(index - kFirstSwitch) * (width + 20.0f), 594.0f, width, 124.0f};
    }
    constexpr float kNarrow = (1728.0f - 3.0f * 24.0f) / 4.0f;
    const int row = index / kRowLength, col = index % kRowLength;
    return {96.0f + static_cast<float>(col) * (kNarrow + 24.0f), 276.0f + 138.0f * static_cast<float>(row), kNarrow, 124.0f};
}

Rect Screen::settings_action_rect(int index) const
{
    if (index == 3)
        return {1824.0f - 360.0f, 736.0f, 360.0f, 84.0f}; // the web page, apart from the three about sunshine.conf
    static const float widths[3] = {250.0f, 400.0f, 340.0f};
    float x = 96.0f;
    for (int i = 0; i < index; ++i)
        x += widths[i] + 20.0f;
    return {x, 736.0f, widths[index], 84.0f};
}

Rect Screen::web_row_rect(int index) const
{
    const float top = 540.0f - kWebSheetHeight / 2.0f + 10.0f;
    return {660.0f, top + kWebRowsTop + static_cast<float>(index) * kWebRowStep, 600.0f, 60.0f};
}

Rect Screen::focus_rect() const
{
    if (page_ == Page::home)
        return home_row_ == 0                      ? action_rect(home_action_)
               : home_card_ < visible_pairings()   ? pairing_rect(home_card_)
               : home_card_ < devices_index()      ? streams_rect()
                                                   : kPairedDevices;
    if (page_ == Page::diagnostics)
        return {96.0f, 836.0f, 340.0f, 76.0f};
    return tile_ < kTiles ? tile_rect(tile_) : settings_action_rect(tile_ - kTiles);
}

// ---- update and input -------------------------------------------------------------------------

void Screen::update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    time_ += dt;
    message_age_ += dt;
    armed_age_ += dt;
    page_age_ += dt;
    snap_ = host_.snapshot();
    if (snap_.message_serial != seen_message_)
    {
        seen_message_ = snap_.message_serial;
        message_age_ = 0.0f;
        feedback.play(snap_.message_ok ? hui::audio::Cue::complete : hui::audio::Cue::error);
        // After a save or a reload the page shows the files again; other outcomes keep unsaved edits.
        if (reload_edits_)
            edits_loaded_ = false;
        reload_edits_ = false;
    }
    if (snap_.report_serial != seen_report_)
    {
        seen_report_ = snap_.report_serial;
        report_lines_.clear();
        const std::string text = host_.report();
        for (std::size_t at = 0; at < text.size();)
        {
            const std::size_t end = std::min(text.find('\n', at), text.size());
            report_lines_.push_back(text.substr(at, end - at));
            at = end + 1;
        }
        report_scroll_ = 0;
    }
    if (!edits_loaded_ && snap_.files_read && snap_.busy.empty())
    {
        edits_ = snap_.config;
        edits_loaded_ = true;
    }
    // Cards come and go before the paired devices (pairing requests, the streams card): a focus on them stays there.
    if (home_card_ == seen_devices_index_)
        home_card_ = devices_index();
    seen_devices_index_ = devices_index();
    // A new pairing request: a chime, and on the home page the focus goes to its card.
    if (snap_.pairings.size() > seen_pairings_)
    {
        feedback.play(hui::audio::Cue::notify);
        if (page_ == Page::home && sheet_ == Sheet::none)
        {
            home_row_ = 1;
            home_card_ = 0;
        }
    }
    seen_pairings_ = snap_.pairings.size();
    home_card_ = std::clamp(home_card_, 0, devices_index());

    // A button that is down while the app starts (the press that started it, or a pad that reports
    // one held, as the virtual pad of a host session without its client does) is ignored until it
    // is let go, so it never acts on the focused Stop host. Every other button works at once.
    if (time_ < 0.5f)
        blocked_ |= input.held;
    else
        blocked_ &= input.held;
    if (blocked_ != 0 && !blocked_logged_ && time_ > 2.0f)
    {
        hui::sys::log("[PSAPP] ignoring buttons held since the start: 0x%x", static_cast<unsigned>(blocked_));
        blocked_logged_ = true;
    }
    hui::InputFrame in = input;
    in.pressed &= ~blocked_;
    const InputAction directions[] = {InputAction::up, InputAction::down, InputAction::left, InputAction::right};
    const Direction navs[] = {Direction::up, Direction::down, Direction::left, Direction::right};
    for (int i = 0; i < 4; ++i)
    {
        if (in.nav == navs[i] && (blocked_ & hui::action_bit(directions[i])) != 0)
            in.nav = Direction::none;
    }
    if (time_ >= 0.5f)
    {
        if (sheet_ != Sheet::none)
            handle_sheet(in, feedback);
        else if (in.is_pressed(InputAction::page_prev) || in.is_pressed(InputAction::page_next))
        {
            const int next = static_cast<int>(page_) + (in.is_pressed(InputAction::page_prev) ? -1 : 1);
            switch_page(static_cast<Page>(std::clamp(next, 0, static_cast<int>(Page::diagnostics))), feedback);
        }
        else if (page_ == Page::home)
            handle_home(in, feedback);
        else if (page_ == Page::settings)
            handle_settings(in, feedback);
        else
            handle_diagnostics(in, feedback);
    }

    // Motion.
    const Rect target = focus_rect();
    if (snap_highlight_)
    {
        highlight_.snap(target);
        snap_highlight_ = false;
    }
    highlight_.target(target);
    highlight_.update(dt, style_);
    for (int i = 0; i < 3; ++i)
        action_amount_[i] = approach(action_amount_[i], page_ == Page::home && home_row_ == 0 && home_action_ == i ? 1.0f : 0.0f, dt, 14.0f);
    for (int i = 0; i < 2; ++i)
        card_amount_[i] = approach(card_amount_[i], page_ == Page::home && home_row_ == 1 && home_card_ == i ? 1.0f : 0.0f, dt, 14.0f);
    devices_amount_ = approach(devices_amount_, page_ == Page::home && home_row_ == 1 && home_card_ == devices_index() ? 1.0f : 0.0f, dt, 14.0f);
    streams_amount_ = approach(streams_amount_,
                               page_ == Page::home && home_row_ == 1 && streams_shown() && home_card_ == visible_pairings() ? 1.0f : 0.0f,
                               dt, 14.0f);
    for (int i = 0; i < kTiles + kSettingsActions; ++i)
        tile_amount_[i] = approach(tile_amount_[i], page_ == Page::settings && tile_ == i ? 1.0f : 0.0f, dt, 14.0f);
    report_amount_ = approach(report_amount_, page_ == Page::diagnostics ? 1.0f : 0.0f, dt, 14.0f);
    const float tab_target = static_cast<float>(static_cast<int>(page_));
    tab_x_ = tab_x_ < 0.0f ? tab_target : approach(tab_x_, tab_target, dt, 16.0f);
    sheet_amount_ = approach(sheet_amount_, sheet_ == Sheet::none ? 0.0f : 1.0f, dt, 16.0f);
    if (sheet_ == Sheet::none && sheet_amount_ < 0.01f)
        sheet_drawn_ = Sheet::none;

    const bool running = snap_.polled && snap_.running;
    const float spin_speed = live() ? 0.55f : running ? 0.12f : 0.0f;
    sun_spin_ += spin_speed * dt;
    sun_glow_ = approach(sun_glow_, live() ? 1.0f : running ? 0.7f : 0.15f, dt, 3.0f);
    sun_dim_ = approach(sun_dim_, running ? 0.0f : 0.85f, dt, 3.0f);
    if (live())
        pulse_ += dt;
    keyboard_.update(dt);
    if (sheet_ == Sheet::choice || sheet_ == Sheet::devices || sheet_ == Sheet::streams || sheet_ == Sheet::web)
    {
        sheet_highlight_.update(dt, style_);
    }
}

void Screen::switch_page(Page page, hui::ui::Feedback &feedback)
{
    if (page == page_)
    {
        feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.4f);
        return;
    }
    page_direction_ = static_cast<int>(page) > static_cast<int>(page_) ? 1 : -1;
    page_ = page;
    // The report is made when its page opens, and only then does the console offer it on the network.
    host_.serve_report(page == Page::diagnostics);
    if (page == Page::diagnostics)
        host_.make_report();
    page_age_ = 0.0f;
    snap_highlight_ = true;
    armed_ = -1;
    feedback.play(hui::audio::Cue::tab);
}

void Screen::handle_home(const hui::InputFrame &input, hui::ui::Feedback &feedback)
{
    const int cards = visible_pairings();
    switch (input.nav)
    {
    case Direction::up:
    case Direction::down: {
        const int row = input.nav == Direction::up ? 0 : 1;
        if (row == home_row_)
        {
            if (!input.nav_repeat)
                feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.4f);
            break;
        }
        home_row_ = row;
        feedback.play(hui::audio::Cue::focus);
        break;
    }
    case Direction::left:
    case Direction::right: {
        const int step = input.nav == Direction::left ? -1 : 1;
        int &index = home_row_ == 0 ? home_action_ : home_card_;
        const int count = home_row_ == 0 ? 3 : devices_index() + 1;
        if (index + step < 0 || index + step >= count)
        {
            if (!input.nav_repeat)
                feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.4f);
            break;
        }
        index += step;
        feedback.play(hui::audio::Cue::focus);
        break;
    }
    default:
        break;
    }
    if (input.is_pressed(InputAction::confirm))
    {
        if (home_row_ == 0)
        {
            run(static_cast<Action>(home_action_), feedback);
        }
        else if (home_card_ < cards)
        {
            const Pairing &p = snap_.pairings[static_cast<std::size_t>(home_card_)];
            target_ = p.id;
            target_name_ = p.name;
            pair_pin_.clear();
            open_sheet(Sheet::pin, "Pair " + diag::pairing_label(p.name), "Type the four digits Moonlight shows.", "");
            feedback.play(hui::audio::Cue::modal_open);
        }
        else
        {
            device_row_ = 0;
            device_top_ = 0;
            if (home_card_ < devices_index())
                open_streams();
            else
                open_devices();
            sheet_highlight_.snap(device_row_rect(0));
            feedback.play(hui::audio::Cue::modal_open);
        }
    }
    else if (input.is_pressed(InputAction::west) && home_row_ == 1 && home_card_ < cards)
    {
        // Square denies a request, after asking, as Square removes a paired device.
        if (busy())
        {
            feedback.play(hui::audio::Cue::error);
            return;
        }
        const Pairing &p = snap_.pairings[static_cast<std::size_t>(home_card_)];
        target_ = p.id;
        target_name_ = diag::pairing_label(p.name);
        pair_pin_.clear();
        open_sheet(Sheet::deny_pairing, "Deny " + target_name_ + "?",
                   "Moonlight is told no at once. It can ask again later.", "");
        feedback.play(hui::audio::Cue::modal_open);
    }
    if (input.is_pressed(InputAction::north))
    {
        host_.refresh();
        reload_edits_ = true;
        feedback.play(hui::audio::Cue::select);
    }
}

void Screen::handle_settings(const hui::InputFrame &input, hui::ui::Feedback &feedback)
{
    switch (input.nav)
    {
    case Direction::up:
    case Direction::down:
    case Direction::left:
    case Direction::right: {
        int next = tile_;
        if (tile_ < kTiles)
        {
            const bool switches = tile_ >= kFirstSwitch, second = !switches && tile_ >= kRowLength;
            const int col = switches ? tile_ - kFirstSwitch : tile_ % kRowLength;
            // Up and down go to the nearest tile: the two rows of four line up; the switches' row has its own count.
            const int last_switch = kSwitchTiles - 1;
            const int to_switch = kFirstSwitch + (col * last_switch + (kRowLength - 1) / 2) / (kRowLength - 1);
            const int from_switch = kRowLength + (col * (kRowLength - 1) + last_switch / 2) / std::max(last_switch, 1);
            if (input.nav == Direction::left)
                next = col > 0 ? tile_ - 1 : -1;
            else if (input.nav == Direction::right)
                next = col < (switches ? last_switch : kRowLength - 1) ? tile_ + 1 : -1;
            else if (input.nav == Direction::up)
                next = switches ? from_switch : second ? tile_ - kRowLength : -1;
            else
                next = switches ? kTiles + (col == last_switch ? 3 : std::min(col, 2))
                       : second ? std::min(to_switch, kFirstSwitch + last_switch)
                                : tile_ + kRowLength;
        }
        else
        {
            const int index = tile_ - kTiles;
            if (input.nav == Direction::left)
                next = index > 0 ? tile_ - 1 : -1;
            else if (input.nav == Direction::right)
                next = index < kSettingsActions - 1 ? tile_ + 1 : -1;
            else if (input.nav == Direction::up)
                next = kFirstSwitch + (index == 3 ? kSwitchTiles - 1 : std::min(index, kSwitchTiles - 1));
            else
                next = -1;
        }
        if (next < 0)
        {
            if (!input.nav_repeat)
                feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.4f);
            break;
        }
        tile_ = next;
        feedback.play(hui::audio::Cue::focus);
        break;
    }
    default:
        break;
    }
    if (input.is_pressed(InputAction::confirm))
    {
        if (tile_ >= kTiles)
        {
            run(static_cast<Action>(static_cast<int>(Action::save) + tile_ - kTiles), feedback);
        }
        else if (switch_not_needed(tile_))
        {
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.4f);
        }
        else if (option_specs()[static_cast<std::size_t>(tile_)].kind == OptionKind::toggle)
        {
            std::optional<std::string> &value = edits_[option_specs()[static_cast<std::size_t>(tile_)].key];
            value = value && *value == "1" ? std::nullopt : std::optional<std::string>("1");
            feedback.play(hui::audio::Cue::toggle);
        }
        else
        {
            open_tile(tile_);
            feedback.play(hui::audio::Cue::modal_open);
        }
    }
    if (input.is_pressed(InputAction::west) && tile_ < kTiles && !switch_not_needed(tile_))
    {
        const OptionSpec &spec = option_specs()[static_cast<std::size_t>(tile_)];
        if (edits_[spec.key])
        {
            edits_[spec.key] = std::nullopt;
            feedback.play(hui::audio::Cue::toggle);
        }
    }
    if (input.is_pressed(InputAction::north))
    {
        host_.refresh();
        reload_edits_ = true;
        feedback.play(hui::audio::Cue::select);
    }
}

void Screen::handle_diagnostics(const hui::InputFrame &input, hui::ui::Feedback &feedback)
{
    const int last = std::max(0, static_cast<int>(report_lines_.size()) - kReportVisible);
    int step = 0;
    if (input.nav == Direction::up || input.nav == Direction::down)
        step = input.nav == Direction::up ? -1 : 1;
    if (input.nav == Direction::left || input.nav == Direction::right)
        step = (input.nav == Direction::left ? -1 : 1) * kReportVisible;
    if (step != 0)
    {
        const int next = std::clamp(report_scroll_ + step, 0, last);
        if (next != report_scroll_)
            feedback.play(hui::audio::Cue::focus);
        else if (!input.nav_repeat)
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.4f);
        report_scroll_ = next;
    }
    if (input.is_pressed(InputAction::confirm) || input.is_pressed(InputAction::north))
    {
        if (busy())
        {
            feedback.play(hui::audio::Cue::error);
            return;
        }
        host_.make_report();
        feedback.play(hui::audio::Cue::select);
    }
}

void Screen::run(Action action, hui::ui::Feedback &feedback)
{
    if (action == Action::revert)
    {
        edits_ = snap_.config;
        feedback.play(hui::audio::Cue::back);
        return;
    }
    if (action == Action::web)
    {
        open_web();
        feedback.play(hui::audio::Cue::modal_open);
        return;
    }
    if (busy())
    {
        feedback.play(hui::audio::Cue::error);
        return;
    }
    // Stopping the host ends a running stream, and a player streaming this screen may be the one
    // pressing: such a press has to come twice.
    const bool ends_stream = live() && ((action == Action::start_stop && snap_.running) ||
                                                 action == Action::restart || action == Action::save_restart);
    if (ends_stream && (armed_ != static_cast<int>(action) || armed_age_ > kArmSeconds))
    {
        armed_ = static_cast<int>(action);
        armed_age_ = 0.0f;
        feedback.play(hui::audio::Cue::notify);
        return;
    }
    armed_ = -1;
    switch (action)
    {
    case Action::start_stop:
        if (snap_.running)
            host_.stop_host();
        else
            host_.start_host();
        break;
    case Action::restart:
        host_.restart_host();
        break;
    case Action::autoload:
        host_.set_autoload(!snap_.autoload);
        break;
    case Action::save:
    case Action::save_restart:
        host_.save_config(edits_, action == Action::save_restart);
        reload_edits_ = true;
        break;
    case Action::revert:
    case Action::web:
        break;
    }
    feedback.play(hui::audio::Cue::select);
}

void Screen::open_tile(int index)
{
    const OptionSpec &spec = option_specs()[static_cast<std::size_t>(index)];
    target_ = spec.key;
    const std::optional<std::string> &value = edits_[spec.key];
    switch (spec.kind)
    {
    case OptionKind::text:
        open_sheet(Sheet::text, spec.label, std::string(spec.help) + " Leave it empty for the default.",
                   value ? *value : std::string());
        break;
    case OptionKind::choice:
        choice_ = 0;
        for (std::size_t i = 0; value && i < spec.choices.size(); ++i)
        {
            if (*value == spec.choices[i].value)
                choice_ = static_cast<int>(i) + 1;
        }
        open_sheet(Sheet::choice, spec.label, spec.help, "");
        break;
    case OptionKind::number:
        number_default_ = !value;
        number_ = value ? std::strtol(value->c_str(), nullptr, 10) : spec.fallback;
        open_sheet(Sheet::number, spec.label, spec.help, "");
        break;
    case OptionKind::toggle:
        break; // flips on the tile
    }
}

void Screen::open_sheet(Sheet kind, const std::string &title, const std::string &help, const std::string &initial)
{
    web_step_ = WebStep::none; // the web page's steps set it again after this
    show_password_ = false;
    sheet_ = kind;
    sheet_drawn_ = kind;
    sheet_title_ = title;
    sheet_help_ = help;
    buffer_ = initial;
    sheet_error_.clear();
    if (kind == Sheet::text || kind == Sheet::pin)
    {
        if (kind == Sheet::text)
        {
            keyboard_.set_layouts({hui::ui::KeyboardLayout::letters(), hui::ui::KeyboardLayout::symbols()});
            keyboard_.style.max_length = 63;
            keyboard_.set_bounds({460.0f, 520.0f, 1000.0f, 380.0f});
        }
        else
        {
            keyboard_.set_layouts({hui::ui::KeyboardLayout::numeric()});
            keyboard_.style.max_length = 4;
            keyboard_.set_bounds({735.0f, 520.0f, 450.0f, 380.0f});
        }
        keyboard_.set_length(static_cast<int>(buffer_.size()));
        keyboard_.set_focus(0, 0);
        keyboard_.enter();
    }
    if (kind == Sheet::choice)
    {
        const Rect row{660.0f, 400.0f + static_cast<float>(choice_) * 76.0f, 600.0f, 64.0f};
        sheet_highlight_.snap(row);
    }
}

void Screen::close_sheet()
{
    sheet_ = Sheet::none;
}

void Screen::open_devices()
{
    renaming_ = false;
    target_.clear();
    // Which profiles a device plays as is set on the web page only, for now (src/control/device_profiles.hpp).
    open_sheet(Sheet::devices, "Paired devices", "Rename a device, or take its access away. Its profiles are set on the web page.", "");
}

void Screen::open_streams()
{
    renaming_ = false;
    target_.clear();
    open_sheet(Sheet::streams, "Streaming now", "End a device's stream. The game keeps running.", "");
}

void Screen::open_web()
{
    renaming_ = false;
    pair_pin_.clear();
    target_.clear();
    web_user_.clear();
    open_sheet(Sheet::web, "Web page", "Settings, devices and streams from a browser on your network.", "");
    web_row_ = 0;
    while (web_row_ < kWebRows - 1 && !web_row_enabled(static_cast<WebRow>(web_row_)))
        ++web_row_;
    sheet_highlight_.snap(web_row_rect(web_row_));
}

bool Screen::web_row_enabled(WebRow row) const
{
    // The host keeps the login: without a running host that knows WEBLOGIN there is nothing to change.
    const bool host = snap_.polled && snap_.running && snap_.web_known;
    switch (row)
    {
    case WebRow::toggle:
        // Turn off while on; Turn on needs a stored login and a host that keeps one while off.
        return host && (snap_.web_enabled || (snap_.web_switchable && !snap_.web_user.empty()));
    case WebRow::set_login:
        return host;
    case WebRow::forget:
        return host && snap_.web_switchable && !snap_.web_user.empty();
    case WebRow::close:
        return true;
    }
    return false;
}

std::string Screen::web_address() const
{
    const auto base = snap_.config.find("port");
    const std::uint16_t port = web::port(base != snap_.config.end() ? base->second : std::nullopt);
    const std::string address = web::address(snap_.address, port);
    return address.empty() ? "Port " + std::to_string(port) + " on the console's IP" : address;
}

void Screen::handle_web(const hui::InputFrame &input, hui::ui::Feedback &feedback)
{
    // The rows that can't do anything now are skipped; the host's state may change under the focus.
    if (!web_row_enabled(static_cast<WebRow>(web_row_)))
    {
        web_row_ = 0;
        while (web_row_ < kWebRows - 1 && !web_row_enabled(static_cast<WebRow>(web_row_)))
            ++web_row_;
    }
    if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int step = input.nav == Direction::up ? -1 : 1;
        int next = web_row_ + step;
        while (next >= 0 && next < kWebRows && !web_row_enabled(static_cast<WebRow>(next)))
            next += step;
        if (next >= 0 && next < kWebRows)
        {
            web_row_ = next;
            feedback.play(hui::audio::Cue::focus);
        }
        else if (!input.nav_repeat)
        {
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.4f);
        }
    }
    sheet_highlight_.target(web_row_rect(web_row_));
    if (input.is_pressed(InputAction::back))
    {
        feedback.play(hui::audio::Cue::modal_close);
        close_sheet();
        return;
    }
    if (!input.is_pressed(InputAction::confirm))
        return;
    const WebRow row = static_cast<WebRow>(web_row_);
    if (row == WebRow::close)
    {
        feedback.play(hui::audio::Cue::modal_close);
        close_sheet();
        return;
    }
    if (busy() || !web_row_enabled(row))
    {
        feedback.play(hui::audio::Cue::error);
        return;
    }
    if (row == WebRow::toggle)
    {
        if (snap_.web_enabled)
            host_.web_login_off();
        else
            host_.web_login_on();
        feedback.play(hui::audio::Cue::toggle);
        return;
    }
    if (row == WebRow::forget)
    {
        open_sheet(Sheet::forget_web, "Forget the login?",
                   "The web page turns off. Turning it on again needs a new login.", "");
        feedback.play(hui::audio::Cue::modal_open);
        return;
    }
    open_sheet(Sheet::text, "Web page user", "1 to 32 letters, digits, '.', '_' or '-'. You log in with it.", snap_.web_user);
    web_step_ = WebStep::user;
    keyboard_.style.max_length = static_cast<int>(web::kUserMax);
    feedback.play(hui::audio::Cue::select);
}

void Screen::handle_sheet(const hui::InputFrame &input, hui::ui::Feedback &feedback)
{
    if (sheet_ == Sheet::web)
    {
        handle_web(input, feedback);
        return;
    }
    if (sheet_ == Sheet::devices)
    {
        const int count = static_cast<int>(snap_.devices.size());
        device_row_ = std::clamp(device_row_, 0, std::max(0, count - 1));
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int next = device_row_ + (input.nav == Direction::up ? -1 : 1);
            if (next >= 0 && next < count)
            {
                device_row_ = next;
                feedback.play(hui::audio::Cue::focus);
            }
            else if (!input.nav_repeat)
            {
                feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.4f);
            }
        }
        device_top_ = std::clamp(device_top_, std::max(0, device_row_ - kDeviceRows + 1), device_row_);
        device_top_ = std::clamp(device_top_, 0, std::max(0, count - kDeviceRows));
        sheet_highlight_.target(device_row_rect(device_row_ - device_top_));
        if (input.is_pressed(InputAction::back))
        {
            feedback.play(hui::audio::Cue::modal_close);
            close_sheet();
        }
        else if (count > 0 && (input.is_pressed(InputAction::confirm) || input.is_pressed(InputAction::west)))
        {
            if (busy())
            {
                feedback.play(hui::audio::Cue::error);
                return;
            }
            const diag::Device &device = snap_.devices[static_cast<std::size_t>(device_row_)];
            target_ = device.id;
            target_name_ = diag::device_label(device);
            if (input.is_pressed(InputAction::west))
            {
                open_sheet(Sheet::remove_device, "Remove " + target_name_ + "?",
                           device.streaming ? "Its stream ends now, and it has to pair again to come back."
                                            : "It has to pair again before it can stream here.",
                           "");
                feedback.play(hui::audio::Cue::modal_open);
            }
            else
            {
                renaming_ = true;
                pair_suggestion_ = device.name;
                open_sheet(Sheet::text, "Rename device", "So you can tell it apart later. Options keeps this name.", device.name);
                keyboard_.style.max_length = 24;
                feedback.play(hui::audio::Cue::select);
            }
        }
        return;
    }
    if (sheet_ == Sheet::streams)
    {
        // The list changes under the focus as streams start and end: it stays in range.
        const int count = static_cast<int>(snap_.streams.size());
        device_row_ = std::clamp(device_row_, 0, std::max(0, count - 1));
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int next = device_row_ + (input.nav == Direction::up ? -1 : 1);
            if (next >= 0 && next < count)
            {
                device_row_ = next;
                feedback.play(hui::audio::Cue::focus);
            }
            else if (!input.nav_repeat)
            {
                feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.4f);
            }
        }
        device_top_ = std::clamp(device_top_, std::max(0, device_row_ - kDeviceRows + 1), device_row_);
        device_top_ = std::clamp(device_top_, 0, std::max(0, count - kDeviceRows));
        sheet_highlight_.target(device_row_rect(device_row_ - device_top_));
        if (input.is_pressed(InputAction::back))
        {
            feedback.play(hui::audio::Cue::modal_close);
            close_sheet();
        }
        else if (count > 0 && input.is_pressed(InputAction::confirm))
        {
            const diag::Stream &stream = snap_.streams[static_cast<std::size_t>(device_row_)];
            if (busy() || stream.id.empty())
            {
                feedback.play(hui::audio::Cue::error);
                return;
            }
            target_ = stream.id;
            target_name_ = diag::stream_label(stream);
            open_sheet(Sheet::end_stream, "End the stream of " + target_name_ + "?",
                       count > 1 ? "The game and the other streams go on. It can connect again from Moonlight."
                                 : "The game stays open. It can connect again from Moonlight.",
                       "");
            feedback.play(hui::audio::Cue::modal_open);
        }
        return;
    }
    if (sheet_ == Sheet::end_stream)
    {
        if (input.is_pressed(InputAction::confirm))
        {
            host_.end_stream(target_, target_name_);
            feedback.play(hui::audio::Cue::select);
            open_streams();
        }
        else if (input.is_pressed(InputAction::back))
        {
            feedback.play(hui::audio::Cue::modal_close);
            open_streams();
        }
        return;
    }
    if (sheet_ == Sheet::deny_pairing)
    {
        if (input.is_pressed(InputAction::confirm))
        {
            if (busy())
            {
                feedback.play(hui::audio::Cue::error);
                return;
            }
            host_.deny_pairing(target_, target_name_);
            feedback.play(hui::audio::Cue::select);
            close_sheet();
        }
        else if (input.is_pressed(InputAction::back))
        {
            feedback.play(hui::audio::Cue::modal_close);
            close_sheet();
        }
        return;
    }
    if (sheet_ == Sheet::forget_web)
    {
        if (input.is_pressed(InputAction::confirm))
        {
            if (busy())
            {
                feedback.play(hui::audio::Cue::error);
                return;
            }
            host_.web_login_forget();
            feedback.play(hui::audio::Cue::select);
            open_web();
        }
        else if (input.is_pressed(InputAction::back))
        {
            feedback.play(hui::audio::Cue::modal_close);
            open_web();
        }
        return;
    }
    if (sheet_ == Sheet::remove_device)
    {
        if (input.is_pressed(InputAction::confirm))
        {
            host_.remove_device(target_, target_name_);
            feedback.play(hui::audio::Cue::select);
            open_devices();
        }
        else if (input.is_pressed(InputAction::back))
        {
            feedback.play(hui::audio::Cue::modal_close);
            open_devices();
        }
        return;
    }
    const OptionSpec *spec = find_option(target_);
    if (sheet_ == Sheet::choice && spec != nullptr)
    {
        const int count = static_cast<int>(spec->choices.size()) + 1;
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int next = choice_ + (input.nav == Direction::up ? -1 : 1);
            if (next >= 0 && next < count)
            {
                choice_ = next;
                feedback.play(hui::audio::Cue::focus);
            }
            else if (!input.nav_repeat)
            {
                feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.4f);
            }
        }
        sheet_highlight_.target({660.0f, 400.0f + static_cast<float>(choice_) * 76.0f, 600.0f, 64.0f});
        if (input.is_pressed(InputAction::confirm))
        {
            edits_[spec->key] = choice_ == 0 ? std::nullopt
                                             : std::optional<std::string>(
                                                   spec->choices[static_cast<std::size_t>(choice_ - 1)].value);
            feedback.play(hui::audio::Cue::select);
            close_sheet();
        }
        else if (input.is_pressed(InputAction::back))
        {
            feedback.play(hui::audio::Cue::modal_close);
            close_sheet();
        }
        return;
    }
    if (sheet_ == Sheet::number && spec != nullptr)
    {
        long step = 0;
        if (input.nav == Direction::left || input.nav == Direction::right)
            step = spec->step * (input.nav == Direction::left ? -1 : 1);
        if (input.nav == Direction::up || input.nav == Direction::down)
            step = spec->step * 10 * (input.nav == Direction::down ? -1 : 1);
        if (input.is_pressed(InputAction::page_prev) || input.is_pressed(InputAction::page_next))
            step = spec->step * 100 * (input.is_pressed(InputAction::page_prev) ? -1 : 1);
        if (step != 0)
        {
            const long next = std::clamp(number_default_ ? spec->fallback + step : number_ + step, spec->min, spec->max);
            number_default_ = false;
            if (next == number_ && !input.nav_repeat)
                feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.4f);
            else if (next != number_)
                feedback.play(hui::audio::Cue::slider);
            number_ = next;
        }
        if (input.is_pressed(InputAction::west))
        {
            number_default_ = true;
            number_ = spec->fallback;
            feedback.play(hui::audio::Cue::toggle);
        }
        if (input.is_pressed(InputAction::confirm))
        {
            edits_[spec->key] = number_default_ ? std::nullopt : std::optional<std::string>(std::to_string(number_));
            feedback.play(hui::audio::Cue::select);
            close_sheet();
        }
        else if (input.is_pressed(InputAction::back))
        {
            feedback.play(hui::audio::Cue::modal_close);
            close_sheet();
        }
        return;
    }

    // The keyboard: the PIN and the host name.
    if (web_step_ == WebStep::password && input.is_pressed(InputAction::r3))
    {
        show_password_ = !show_password_;
        feedback.play(hui::audio::Cue::toggle);
    }
    const hui::ui::Event event = keyboard_.handle(input, feedback);
    for (int i = 0; i < keyboard_.erased() && !buffer_.empty(); ++i)
        buffer_.pop_back();
    buffer_ += keyboard_.typed();
    keyboard_.set_length(static_cast<int>(buffer_.size()));
    if (!keyboard_.typed().empty() || keyboard_.erased() > 0)
        sheet_error_.clear();
    if (event == hui::ui::Event::cancelled)
    {
        pair_pin_.clear();
        feedback.play(hui::audio::Cue::modal_close);
        if (renaming_)
            open_devices();
        else if (web_step_ != WebStep::none)
            open_web(); // drops a typed password with the buffer
        else
            close_sheet();
        return;
    }
    if (event != hui::ui::Event::activated && !input.is_pressed(InputAction::menu))
        return;
    if (sheet_ == Sheet::pin)
    {
        if (buffer_.size() != 4)
        {
            sheet_error_ = "The PIN has four digits";
            feedback.play(hui::audio::Cue::error);
            return;
        }
        // Moonlight doesn't say which device it is, so the name is asked here, as Sunshine's web page does.
        pair_pin_ = buffer_;
        pair_suggestion_ = diag::suggested_device_name(
            target_name_, diag::paired_names(snap_.running && snap_.devices_known, snap_.devices, snap_.paired));
        open_sheet(Sheet::text, "Name this device", "So you can tell it apart later. Options keeps this name.", pair_suggestion_);
        keyboard_.style.max_length = 24;
        feedback.play(hui::audio::Cue::select);
        return;
    }
    if (web_step_ == WebStep::user)
    {
        const std::string problem = web::user_problem(buffer_);
        if (!problem.empty())
        {
            sheet_error_ = problem;
            feedback.play(hui::audio::Cue::error);
            return;
        }
        // The password next; the name stays for it.
        const std::string user = buffer_;
        open_sheet(Sheet::text, "Password for " + user, "8 to 128 characters, spaces count. It goes only to the host.", "");
        web_step_ = WebStep::password;
        web_user_ = user;
        keyboard_.style.max_length = static_cast<int>(web::kPasswordMax);
        feedback.play(hui::audio::Cue::select);
        return;
    }
    if (web_step_ == WebStep::password)
    {
        const std::string problem = web::password_problem(buffer_);
        if (!problem.empty())
        {
            sheet_error_ = problem;
            feedback.play(hui::audio::Cue::error);
            return;
        }
        if (busy())
        {
            sheet_error_ = "One moment: the host is busy with something else";
            feedback.play(hui::audio::Cue::error);
            return;
        }
        host_.set_web_login(web_user_, buffer_);
        std::fill(buffer_.begin(), buffer_.end(), '\0');
        buffer_.clear();
        feedback.play(hui::audio::Cue::select);
        open_web(); // the outcome shows there
        return;
    }
    if (!pair_pin_.empty())
    {
        const std::string name = diag::clean_device_name(buffer_);
        host_.submit_pin(target_, pair_pin_, name.empty() ? pair_suggestion_ : name);
        pair_pin_.clear();
        close_sheet();
        return;
    }
    if (renaming_)
    {
        const std::string name = diag::clean_device_name(buffer_);
        if (!name.empty() && name != pair_suggestion_)
            host_.rename_device(target_, name);
        feedback.play(hui::audio::Cue::select);
        open_devices();
        return;
    }
    if (spec == nullptr)
    {
        close_sheet();
        return;
    }
    if (buffer_.empty())
    {
        edits_[spec->key] = std::nullopt; // empty: the host's default
        close_sheet();
        return;
    }
    if (const auto problem = validate(*spec, buffer_))
    {
        sheet_error_ = std::string(spec->label) + " " + *problem;
        feedback.play(hui::audio::Cue::error);
        return;
    }
    edits_[spec->key] = buffer_;
    close_sheet();
}

// ---- drawing: shared parts --------------------------------------------------------------------

void Screen::glass_card(hui::gfx::DrawList &list, std::uint32_t glass, const Rect &r, float radius, float focus) const
{
    list.shadow({r.x, r.y + 10.0f, r.w, r.h}, radius, 30.0f, Color::rgb(0x000000, 0.35f));
    list.glass(glass, r, radius, Color::rgb(0xffffff));
    list.rounded_rect(r, radius, mix(Color::rgb(0x18203f, 0.55f), Color::rgb(0x4a3215, 0.55f), focus));
    list.bordered_rect(r, radius, kClear, 1.5f, mix(Color::rgb(0xffffff, 0.10f), kAccent.with_alpha(0.9f), focus));
    // A thin light along the top edge, as on glass.
    list.gradient_rect({r.x + radius, r.y + 1.0f, r.w - 2.0f * radius, 2.0f}, 1.0f, Color::rgb(0xffffff, 0.16f),
                       Color::rgb(0xffffff, 0.0f));
}

void Screen::draw_button(hui::gfx::DrawList &list, std::uint32_t glass, const Rect &r, int icon,
                         const std::string &label, bool primary, float focus, bool enabled) const
{
    const float radius = r.h / 2.0f;
    const float grow = 4.0f * focus;
    const Rect b{r.x - grow, r.y - grow, r.w + 2 * grow, r.h + 2 * grow};
    Color ink = enabled ? kInk : kFaint;
    if (primary && enabled)
    {
        list.glow(b, radius, 26.0f, kAccent.with_alpha(0.25f + 0.3f * focus));
        list.gradient_rect_h(b, radius, kAccent, kAccentDeep);
        list.gradient_rect({b.x, b.y, b.w, b.h / 2.0f}, radius, Color::rgb(0xffffff, 0.22f), Color::rgb(0xffffff, 0.0f));
        ink = Color::rgb(0x2a1604);
    }
    else
    {
        glass_card(list, glass, b, radius, focus);
    }
    float x = b.x + 34.0f;
    if (focus > 0.5f && enabled)
    {
        // The Cross glyph on the focused button, as on the console's own buttons.
        hui::ui::draw_button(list, fonts_, primary ? hui::ui::GlyphStyle::mono(ink, Color::rgb(0xffffff, 0.0f))
                                                   : hui::ui::GlyphStyle::dark(),
                             hui::ui::Button::cross, x, b.cy(), 34.0f);
        x += 48.0f;
    }
    else if (icon >= 0)
    {
        icons::draw(list, static_cast<Icon>(icon), x + 14.0f, b.cy(), 30.0f, primary ? ink : kAccent);
        x += 48.0f;
    }
    hui::ui::text(list, fonts_.semibold, label, x, b.cy() + 10.0f, 29.0f, ink);
}

void Screen::draw_background(hui::gfx::DrawList &list) const
{
    // Warm light behind the emblem (home) or the tiles (settings), stronger while the host runs.
    const float home = std::max(0.0f, 1.0f - tab_x_);
    const float strength = 0.25f + 0.45f * sun_glow_;
    list.glow({1320.0f, 200.0f, 360.0f, 360.0f}, 180.0f, 520.0f, kAccentDeep.with_alpha(0.16f * strength * home));
    list.glow({1380.0f, 260.0f, 240.0f, 240.0f}, 120.0f, 260.0f, kAccent.with_alpha(0.22f * strength * home));
    list.glow({200.0f, 640.0f, 600.0f, 300.0f}, 150.0f, 420.0f, Color::rgb(0x5b3cc4, 0.14f));
    list.glow({900.0f, 300.0f, 600.0f, 300.0f}, 150.0f, 520.0f, kAccentDeep.with_alpha(0.10f * std::min(1.0f, tab_x_)));
    // Faint drifting embers.
    for (int i = 0; i < 18; ++i)
    {
        const float seed = static_cast<float>(i) * 12.9898f;
        const float x = std::fmod(std::fabs(std::sin(seed) * 43758.5f), 1.0f) * 1920.0f;
        const float speed = 8.0f + std::fmod(static_cast<float>(i) * 7.3f, 14.0f);
        const float y = 1080.0f - std::fmod(time_ * speed + static_cast<float>(i) * 97.0f, 1180.0f);
        const float alpha = 0.10f + 0.12f * (0.5f + 0.5f * std::sin(time_ * 1.3f + seed));
        list.circle(x + 18.0f * std::sin(time_ * 0.4f + seed), y, 2.0f + static_cast<float>(i % 3), kAccent.with_alpha(alpha));
    }
}

void Screen::draw_top_bar(hui::gfx::DrawList &list, std::uint32_t glass) const
{
    icons::sun(list, 112.0f, 78.0f, 15.0f, sun_spin_, 0.6f, sun_dim_ * 0.6f);
    hui::ui::text(list, fonts_.semibold, "PROSPEROSUNSHINE", 150.0f, 87.0f, 24.0f, kInk, Align::left, 6.0f);

    // Tabs between their L1 and R1 glyphs; the selected plate glides.
    const hui::ui::GlyphStyle glyphs = hui::ui::GlyphStyle::dark();
    const float tabs_x = 640.0f;
    hui::ui::draw_button(list, fonts_, glyphs, hui::ui::Button::l1, tabs_x, 78.0f, 30.0f);
    const float t0 = tabs_x + 64.0f, tw = 210.0f;
    const Rect plate{t0 + tab_x_ * (tw + 12.0f), 50.0f, tw, 56.0f};
    list.glass(glass, plate, 28.0f, Color::rgb(0xffffff));
    list.rounded_rect(plate, 28.0f, Color::rgb(0xffffff, 0.10f));
    list.bordered_rect(plate, 28.0f, kClear, 1.5f, kAccent.with_alpha(0.65f));
    const char *names[3] = {"Home", "Settings", "Diagnostics"};
    const Icon tab_icons[3] = {Icon::home, Icon::sliders, Icon::log};
    for (int i = 0; i < 3; ++i)
    {
        const float x = t0 + static_cast<float>(i) * (tw + 12.0f);
        const float selected = 1.0f - std::min(1.0f, std::fabs(tab_x_ - static_cast<float>(i)));
        const Color ink = mix(kMuted, kInk, selected);
        const float label_w = fonts_.semibold.measure(names[i], 26.0f);
        const float start = x + (tw - label_w - 40.0f) / 2.0f;
        icons::draw(list, tab_icons[i], start + 13.0f, 78.0f, 26.0f, mix(kMuted, kAccent, selected));
        hui::ui::text(list, fonts_.semibold, names[i], start + 40.0f, 87.0f, 26.0f, ink);
    }
    // A pairing request waits while the settings are shown.
    if (page_ != Page::home && !snap_.pairings.empty())
    {
        const float pulse = 0.6f + 0.4f * std::sin(time_ * 5.0f);
        list.glow({t0 + tw - 30.0f, 56.0f, 12.0f, 12.0f}, 6.0f, 10.0f, kAccent.with_alpha(0.5f * pulse));
        list.circle(t0 + tw - 24.0f, 62.0f, 7.0f, kAccent);
    }
    hui::ui::draw_button(list, fonts_, glyphs, hui::ui::Button::r1, t0 + 3.0f * tw + 24.0f + 18.0f, 78.0f, 30.0f);

    // The console's address (what Moonlight needs), and the version.
    float right = 1824.0f;
    if (!version_.empty())
    {
        right -= hui::ui::text(list, fonts_.regular, "v" + version_, right, 87.0f, 20.0f, kFaint, Align::right);
        right -= 28.0f;
    }
    if (!snap_.address.empty())
    {
        right -= hui::ui::text(list, fonts_.semibold, snap_.address, right, 87.0f, 24.0f, kInk, Align::right);
        icons::draw(list, Icon::wifi, right - 22.0f, 76.0f, 26.0f, kAccent);
    }
}

void Screen::draw_footer(hui::gfx::DrawList &list, std::uint32_t glass) const
{
    // The bar over the hints: an armed press, the request being worked on, or the last outcome.
    std::string text;
    Color dot = kAccent;
    float alpha = 0.0f;
    bool spinner = false;
    if (armed_ >= 0 && armed_age_ <= kArmSeconds && !busy())
    {
        text = "This ends the running stream. Press Cross again to go on.";
        alpha = 1.0f;
    }
    else if (busy())
    {
        text = snap_.busy;
        alpha = 1.0f;
        spinner = true;
    }
    else if (message_age_ < kMessageSeconds && !snap_.message.empty())
    {
        text = snap_.message;
        dot = snap_.message_ok ? kGood : kBad;
        alpha = std::min({1.0f, message_age_ / 0.25f, (kMessageSeconds - message_age_) / 0.6f});
    }
    if (alpha > 0.0f)
    {
        const float rise = (1.0f - ease_out(std::min(1.0f, message_age_ / 0.35f))) * (spinner ? 0.0f : 24.0f);
        const float w = std::min(1500.0f, fonts_.semibold.measure(text, 25.0f) + 130.0f);
        const Rect bar{960.0f - w / 2.0f, 940.0f + rise, w, 60.0f};
        list.push_opacity(alpha);
        glass_card(list, glass, bar, 30.0f, 0.0f);
        if (spinner)
        {
            list.arc(bar.x + 38.0f, bar.cy(), 13.0f, 4.0f, time_ * 6.0f, 4.2f, kAccent);
        }
        else
        {
            list.glow({bar.x + 30.0f, bar.cy() - 8.0f, 16.0f, 16.0f}, 8.0f, 10.0f, dot.with_alpha(0.5f));
            list.circle(bar.x + 38.0f, bar.cy(), 8.0f, dot);
        }
        hui::ui::text(list, fonts_.semibold, shorten(fonts_.semibold, text, 25.0f, w - 100.0f), bar.x + 68.0f,
                      bar.cy() + 9.0f, 25.0f, kInk);
        list.pop_opacity();
    }

    const hui::ui::GlyphStyle glyphs = hui::ui::GlyphStyle::dark();
    hui::ui::HintLayout layout;
    layout.cy = 1036.0f;
    layout.size = 34.0f;
    layout.text_size = 22.0f;
    if (sheet_ == Sheet::pin || sheet_ == Sheet::text)
    {
        // The password step adds R3 to show it; the other bound keys carry their button on the key.
        const bool password = web_step_ == WebStep::password;
        const hui::ui::Hint hints[] = {{hui::ui::Button::cross, "Type"},
                                       {hui::ui::Button::square, "Delete"},
                                       {hui::ui::Button::right_stick, show_password_ ? "Hide password" : "Show password"},
                                       {hui::ui::Button::r2, "Done"},
                                       {hui::ui::Button::circle, "Cancel"}};
        if (password)
        {
            hui::ui::draw_hints(list, fonts_, glyphs, hints, 5, 1824.0f, true, layout);
        }
        else
        {
            const hui::ui::Hint plain[] = {hints[0], hints[1], hints[3], hints[4]};
            hui::ui::draw_hints(list, fonts_, glyphs, plain, 4, 1824.0f, true, layout);
        }
    }
    else if (sheet_ == Sheet::deny_pairing)
    {
        const hui::ui::Hint hints[] = {{hui::ui::Button::cross, "Deny"}, {hui::ui::Button::circle, "Keep waiting"}};
        hui::ui::draw_hints(list, fonts_, glyphs, hints, 2, 1824.0f, true, layout);
    }
    else if (sheet_ == Sheet::forget_web)
    {
        const hui::ui::Hint hints[] = {{hui::ui::Button::cross, "Forget"}, {hui::ui::Button::circle, "Keep it"}};
        hui::ui::draw_hints(list, fonts_, glyphs, hints, 2, 1824.0f, true, layout);
    }
    else if (sheet_ == Sheet::number)
    {
        const hui::ui::Hint hints[] = {{hui::ui::Button::dpad, "Change"},
                                       {hui::ui::Button::l1, "Big steps", hui::ui::Button::r1},
                                       {hui::ui::Button::square, "Default"},
                                       {hui::ui::Button::cross, "OK"},
                                       {hui::ui::Button::circle, "Cancel"}};
        hui::ui::draw_hints(list, fonts_, glyphs, hints, 5, 1824.0f, true, layout);
    }
    else if (sheet_ == Sheet::choice)
    {
        const hui::ui::Hint hints[] = {{hui::ui::Button::cross, "Choose"}, {hui::ui::Button::circle, "Cancel"}};
        hui::ui::draw_hints(list, fonts_, glyphs, hints, 2, 1824.0f, true, layout);
    }
    else if (sheet_ == Sheet::devices)
    {
        // Without a device, only Close (the last hint) means anything.
        const hui::ui::Hint hints[] = {{hui::ui::Button::cross, "Rename"},
                                       {hui::ui::Button::square, "Remove"},
                                       {hui::ui::Button::circle, "Close"}};
        const bool any = !snap_.devices.empty();
        hui::ui::draw_hints(list, fonts_, glyphs, any ? hints : hints + 2, any ? 3 : 1, 1824.0f, true, layout);
    }
    else if (sheet_ == Sheet::remove_device)
    {
        const hui::ui::Hint hints[] = {{hui::ui::Button::cross, "Remove"}, {hui::ui::Button::circle, "Keep it"}};
        hui::ui::draw_hints(list, fonts_, glyphs, hints, 2, 1824.0f, true, layout);
    }
    else if (sheet_ == Sheet::streams)
    {
        // Without a stream, only Close (the last hint) means anything.
        const hui::ui::Hint hints[] = {{hui::ui::Button::cross, "End stream"}, {hui::ui::Button::circle, "Close"}};
        const bool any = !snap_.streams.empty();
        hui::ui::draw_hints(list, fonts_, glyphs, any ? hints : hints + 1, any ? 2 : 1, 1824.0f, true, layout);
    }
    else if (sheet_ == Sheet::end_stream)
    {
        const hui::ui::Hint hints[] = {{hui::ui::Button::cross, "End stream"}, {hui::ui::Button::circle, "Keep it"}};
        hui::ui::draw_hints(list, fonts_, glyphs, hints, 2, 1824.0f, true, layout);
    }
    else if (sheet_ == Sheet::web)
    {
        const hui::ui::Hint hints[] = {{hui::ui::Button::cross, "Select"}, {hui::ui::Button::circle, "Close"}};
        hui::ui::draw_hints(list, fonts_, glyphs, hints, 2, 1824.0f, true, layout);
    }
    else if (page_ == Page::home)
    {
        // On a pairing card, Square denies the request.
        const bool on_request = home_row_ == 1 && home_card_ < visible_pairings();
        const hui::ui::Hint hints[] = {{hui::ui::Button::cross, on_request ? "Enter PIN" : "Select"},
                                       {hui::ui::Button::square, "Deny"},
                                       {hui::ui::Button::triangle, "Reload"},
                                       {hui::ui::Button::l1, "Pages", hui::ui::Button::r1}};
        if (on_request)
        {
            hui::ui::draw_hints(list, fonts_, glyphs, hints, 4, 1824.0f, true, layout);
        }
        else
        {
            const hui::ui::Hint plain[] = {hints[0], hints[2], hints[3]};
            hui::ui::draw_hints(list, fonts_, glyphs, plain, 3, 1824.0f, true, layout);
        }
    }
    else if (page_ == Page::diagnostics)
    {
        const hui::ui::Hint hints[] = {{hui::ui::Button::dpad, "Scroll"},
                                       {hui::ui::Button::cross, "Collect again"},
                                       {hui::ui::Button::l1, "Pages", hui::ui::Button::r1}};
        hui::ui::draw_hints(list, fonts_, glyphs, hints, 3, 1824.0f, true, layout);
    }
    else
    {
        const hui::ui::Hint hints[] = {{hui::ui::Button::cross, "Change"},
                                       {hui::ui::Button::square, "Default"},
                                       {hui::ui::Button::triangle, "Reload"},
                                       {hui::ui::Button::l1, "Pages", hui::ui::Button::r1}};
        hui::ui::draw_hints(list, fonts_, glyphs, hints, 4, 1824.0f, true, layout);
    }
}

// ---- drawing: the home page -------------------------------------------------------------------

void Screen::draw_home(hui::gfx::DrawList &list, std::uint32_t glass) const
{
    const bool running = snap_.polled && snap_.running;
    std::string state = "Checking the host";
    std::string detail = "One moment.";
    if (snap_.polled && !snap_.running)
    {
        state = "Host stopped";
        detail = "Start it to stream this console to Moonlight.";
    }
    else if (running && !snap_.poll_error.empty())
    {
        state = "Host busy";
        detail = "It did not answer: " + snap_.poll_error;
    }
    else if (snap_.streaming && !live())
    {
        state = "Session paused";
        detail = "\"" + snap_.app + "\" is still open with no one connected. Resume or quit it in Moonlight.";
    }
    else if (snap_.streaming)
    {
        state = "Streaming";
        detail = snap_.streams.size() > 1 ? std::to_string(snap_.streams.size()) + " devices are playing \"" + snap_.app + "\"."
                 : snap_.client.empty()    ? "Moonlight is playing \"" + snap_.app + "\"."
                                      : snap_.client + (snap_.client_address.empty() ? "" : " (" + snap_.client_address + ")") +
                                            " is playing \"" + snap_.app + "\".";
    }
    else if (running)
    {
        state = "Ready to stream";
        detail = snap_.address.empty() ? "Add this console in Moonlight by its IP address."
                                       : "In Moonlight, add " + snap_.address + " and start \"PS5 Screen\".";
    }

    // The hero: caption, title, detail, chips.
    float a = appear(0);
    list.push_opacity(a);
    list.push_transform(1.0f, 0.0f, 0.0f, static_cast<float>(page_direction_) * 40.0f * (1.0f - a), 0.0f);
    list.rounded_rect({96.0f, 222.0f, 40.0f, 3.0f}, 1.5f, kAccent);
    hui::ui::text(list, fonts_.semibold, "HOST STATUS", 152.0f, 232.0f, 22.0f, kAccent, Align::left, 4.0f);
    hui::ui::text(list, fonts_.display, state, 90.0f, 350.0f, 104.0f, kInk);
    hui::ui::text(list, fonts_.regular, shorten(fonts_.regular, detail, 30.0f, 1120.0f), 96.0f, 408.0f, 30.0f, kMuted);
    list.pop_transform();
    list.pop_opacity();

    a = appear(1);
    list.push_opacity(a);
    list.push_transform(1.0f, 0.0f, 0.0f, static_cast<float>(page_direction_) * 40.0f * (1.0f - a), 0.0f);
    float x = 96.0f;
    auto chip = [&](Icon icon, const std::string &text, bool on) {
        const float w = fonts_.semibold.measure(text, 22.0f) + 74.0f;
        const Rect r{x, 444.0f, w, 50.0f};
        list.glass(glass, r, 25.0f, Color::rgb(0xffffff));
        list.rounded_rect(r, 25.0f, on ? kAccent.with_alpha(0.16f) : Color::rgb(0xffffff, 0.06f));
        list.bordered_rect(r, 25.0f, kClear, 1.5f, on ? kAccent.with_alpha(0.55f) : Color::rgb(0xffffff, 0.12f));
        icons::draw(list, icon, x + 30.0f, r.cy(), 22.0f, on ? kAccent : kMuted);
        hui::ui::text(list, fonts_.semibold, text, x + 52.0f, r.cy() + 8.0f, 22.0f, kInk);
        x += w + 14.0f;
    };
    const auto saved = [&](const char *key, const std::string &fallback) {
        const auto it = snap_.config.find(key);
        return it != snap_.config.end() && it->second ? *it->second : fallback;
    };
    chip(Icon::plug, "Port " + saved("port", "47989"), false);
    const std::string lan = saved("lan_encryption_mode", "0");
    chip(Icon::lock, lan == "0" ? "LAN unencrypted" : lan == "1" ? "LAN encrypted when supported" : "LAN encrypted", lan != "0");
    chip(Icon::boot, snap_.autoload ? "Starts at boot" : "Manual start", snap_.autoload);
    const std::size_t paired = diag::paired_names(snap_.running && snap_.devices_known, snap_.devices, snap_.paired).size();
    chip(Icon::monitor, std::to_string(paired) + (paired == 1 ? " paired device" : " paired devices"), false);
    list.pop_transform();
    list.pop_opacity();

    // The actions.
    a = appear(2);
    list.push_opacity(a);
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, 30.0f * (1.0f - a));
    const bool enabled = !busy();
    draw_button(list, glass, action_rect(0), static_cast<int>(Icon::power), running ? "Stop host" : "Start host",
                !running, action_amount_[0], enabled);
    draw_button(list, glass, action_rect(1), static_cast<int>(Icon::restart), "Restart host", false, action_amount_[1],
                enabled);
    {
        const Rect r = action_rect(2);
        draw_button(list, glass, r, static_cast<int>(Icon::boot), "Start at boot", false, action_amount_[2], enabled);
        const Rect track{r.x + r.w - 104.0f, r.cy() - 19.0f, 76.0f, 38.0f};
        list.rounded_rect(track, 19.0f, snap_.autoload ? kAccent : Color::rgb(0x3a4363));
        if (snap_.autoload)
            list.glow(track, 19.0f, 12.0f, kAccent.with_alpha(0.35f));
        const float knob = snap_.autoload ? track.x + track.w - 19.0f : track.x + 19.0f;
        list.circle(knob, track.cy(), 15.0f, Color::rgb(0xffffff));
    }
    // What the firmware or the last capture attempt means for streaming, above the boot hint.
    if (!snap_.notice.text.empty())
        hui::ui::text(list, fonts_.semibold, shorten(fonts_.semibold, snap_.notice.text, 22.0f, 1728.0f), 96.0f, 660.0f, 22.0f,
                      snap_.notice.bad ? kBad : kAccent);
    const float boot_hint_y = snap_.notice.text.empty() ? 660.0f : 690.0f;
    if (snap_.autoload && snap_.manager_present && !snap_.manager_autoload)
        hui::ui::text(list, fonts_.regular, "The Payload Manager's own autoload is off, so nothing starts at boot.",
                      action_rect(2).x, boot_hint_y, 20.0f, kAccent);
    else if (!snap_.manager_present)
        hui::ui::text(list, fonts_.regular, "Start at boot needs the Payload Manager (PLDM00001).", action_rect(2).x,
                      boot_hint_y, 20.0f, kFaint);
    list.pop_transform();
    list.pop_opacity();

    // The emblem: the sun turns while the host runs and sends rings out while it streams.
    a = appear(1);
    const float ex = 1500.0f, ey = 360.0f;
    list.push_opacity(a);
    list.push_transform(0.9f + 0.1f * a, ex, ey, 0.0f, 0.0f);
    if (live())
    {
        for (int i = 0; i < 3; ++i)
        {
            const float t = std::fmod(pulse_ * 0.45f + static_cast<float>(i) / 3.0f, 1.0f);
            list.ring(ex, ey, 150.0f + t * 230.0f, 3.0f, kAccent.with_alpha(0.5f * (1.0f - t)));
        }
    }
    const float breathe = running ? 1.0f + 0.02f * std::sin(time_ * 1.6f) : 1.0f;
    icons::sun(list, ex, ey, 112.0f * breathe, sun_spin_, sun_glow_, sun_dim_);
    // Its caption.
    const std::string badge = live() ? "LIVE" : snap_.streaming ? "PAUSED" : running ? "READY" : snap_.polled ? "OFF" : "...";
    const Color badge_color = live() ? kBad : snap_.streaming ? kAccent : running ? kGood : kFaint;
    const float bw = fonts_.semibold.measure(badge, 20.0f) + 60.0f;
    const Rect br{ex - bw / 2.0f, ey + 190.0f, bw, 42.0f};
    list.glass(glass, br, 21.0f, Color::rgb(0xffffff));
    list.rounded_rect(br, 21.0f, Color::rgb(0x10162e, 0.6f));
    const float blink = live() ? 0.55f + 0.45f * std::sin(time_ * 4.0f) : 1.0f;
    list.circle(br.x + 24.0f, br.cy(), 6.0f, badge_color.with_alpha(blink));
    hui::ui::text(list, fonts_.semibold, badge, br.x + 40.0f, br.cy() + 7.0f, 20.0f, kInk, Align::left, 3.0f);
    list.pop_transform();
    list.pop_opacity();

    // Pairing requests, and who is paired.
    a = appear(3);
    list.push_opacity(a);
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, 40.0f * (1.0f - a));
    list.rounded_rect({96.0f, 706.0f, 40.0f, 3.0f}, 1.5f, kAccent);
    const int cards = visible_pairings();
    hui::ui::text(list, fonts_.semibold, cards == 0 && streams_shown() ? "STREAMS" : "PAIRING", 152.0f, 716.0f, 22.0f, kAccent,
                  Align::left, 4.0f);
    if (cards == 0 && !streams_shown())
    {
        const Rect r{96.0f, 740.0f, 1100.0f, 172.0f};
        glass_card(list, glass, r, 26.0f, 0.0f);
        list.circle(r.x + 76.0f, r.cy(), 40.0f, Color::rgb(0xffffff, 0.06f));
        icons::draw(list, Icon::link, r.x + 76.0f, r.cy(), 38.0f, running ? kAccent : kFaint);
        hui::ui::text(list, fonts_.semibold, running ? "No one is waiting to pair" : "Pairing needs the host",
                      r.x + 144.0f, r.y + 72.0f, 32.0f, kInk);
        const std::string hint = running ? (snap_.address.empty() ? "In Moonlight, add this console and pick it. The PIN goes in here."
                                                                  : "In Moonlight, add " + snap_.address + " and pick it. The PIN goes in here.")
                                         : "Start the host, then pair from Moonlight.";
        hui::ui::text(list, fonts_.regular, shorten(fonts_.regular, hint, 24.0f, r.w - 180.0f), r.x + 144.0f,
                      r.y + 116.0f, 24.0f, kMuted);
    }
    for (int i = 0; i < cards; ++i)
    {
        const Pairing &p = snap_.pairings[static_cast<std::size_t>(i)];
        const Rect r = pairing_rect(i);
        const float f = card_amount_[i];
        const float pulse = 0.5f + 0.5f * std::sin(time_ * 4.0f);
        list.glow(r, 26.0f, 22.0f, kAccent.with_alpha(0.18f + 0.18f * pulse + 0.2f * f));
        glass_card(list, glass, r, 26.0f, std::max(f, 0.35f));
        list.circle(r.x + 68.0f, r.y + 70.0f, 36.0f, kAccent.with_alpha(0.18f));
        icons::draw(list, Icon::link, r.x + 68.0f, r.y + 70.0f, 34.0f, kAccent);
        hui::ui::text(list, fonts_.semibold, shorten(fonts_.semibold, diag::pairing_label(p.name), 32.0f, r.w - 150.0f),
                      r.x + 126.0f, r.y + 66.0f, 32.0f, kInk);
        hui::ui::text(list, fonts_.regular, p.address.empty() ? "wants to pair" : "from " + p.address + " wants to pair",
                      r.x + 126.0f, r.y + 100.0f, 22.0f, kMuted);
        const Rect pill{r.x + 30.0f, r.y + r.h - 66.0f, 230.0f, 46.0f};
        list.rounded_rect(pill, 23.0f, kAccent.with_alpha(0.18f + 0.3f * f));
        hui::ui::draw_button(list, fonts_, hui::ui::GlyphStyle::dark(), hui::ui::Button::cross, pill.x + 16.0f, pill.cy(), 28.0f);
        hui::ui::text(list, fonts_.semibold, "Enter PIN", pill.x + 58.0f, pill.cy() + 8.0f, 23.0f, kInk);
        const Rect deny{pill.x + pill.w + 16.0f, pill.y, 150.0f, pill.h};
        list.rounded_rect(deny, 23.0f, Color::rgb(0xffffff, 0.06f + 0.06f * f));
        hui::ui::draw_button(list, fonts_, hui::ui::GlyphStyle::dark(), hui::ui::Button::square, deny.x + 16.0f, deny.cy(), 28.0f);
        hui::ui::text(list, fonts_.semibold, "Deny", deny.x + 58.0f, deny.cy() + 8.0f, 23.0f, kMuted);
    }
    if (snap_.pairings.size() > static_cast<std::size_t>(cards))
        hui::ui::text(list, fonts_.regular, "+" + std::to_string(snap_.pairings.size() - static_cast<std::size_t>(cards)) + " more",
                      cards == 1 && streams_shown() ? 634.0f : 1196.0f, 726.0f, 20.0f, kFaint, Align::right);
    if (streams_shown())
    {
        // Who streams now: a row each for the first two, the rest in the list behind Cross.
        const Rect r = streams_rect();
        const float pulse = 0.5f + 0.5f * std::sin(time_ * 3.0f);
        list.glow(r, 26.0f, 18.0f, kGood.with_alpha(0.06f + 0.06f * pulse));
        glass_card(list, glass, r, 26.0f, streams_amount_);
        hui::ui::text(list, fonts_.semibold, "STREAMING NOW", r.x + 30.0f, r.y + 44.0f, 19.0f, kMuted, Align::left, 3.0f);
        const std::size_t shown = std::min<std::size_t>(snap_.streams.size(), 2);
        if (streams_amount_ > 0.01f)
        {
            list.push_opacity(streams_amount_);
            const float label = hui::ui::text(list, fonts_.semibold, "Manage", r.x + r.w - 30.0f, r.y + 45.0f, 21.0f, kInk, Align::right);
            hui::ui::draw_button(list, fonts_, hui::ui::GlyphStyle::dark(), hui::ui::Button::cross, r.x + r.w - 30.0f - label - 46.0f,
                                 r.y + 38.0f, 28.0f);
            list.pop_opacity();
        }
        else if (snap_.streams.size() > shown)
        {
            hui::ui::text(list, fonts_.regular, "+" + std::to_string(snap_.streams.size() - shown) + " more", r.x + r.w - 30.0f,
                          r.y + 45.0f, 20.0f, kFaint, Align::right);
        }
        for (std::size_t i = 0; i < shown; ++i)
        {
            const diag::Stream &stream = snap_.streams[i];
            const float cy = r.y + 90.0f + static_cast<float>(i) * 48.0f;
            list.circle(r.x + 40.0f, cy, 6.0f, kGood.with_alpha(0.55f + 0.45f * pulse));
            const std::string time = diag::duration_text(stream.seconds);
            const float time_w = hui::ui::text(list, fonts_.regular, time, r.x + r.w - 30.0f, cy + 8.0f, 20.0f, kFaint, Align::right);
            // The name, then what it gets in the room that is left.
            const float room = r.w - 60.0f - 30.0f - time_w - 20.0f;
            const std::string name = shorten(fonts_.semibold, diag::stream_label(stream), 22.0f, std::min(room, 260.0f));
            const float name_w = hui::ui::text(list, fonts_.semibold, name, r.x + 60.0f, cy + 8.0f, 22.0f, kInk);
            const std::string info = (stream.role == "joined" ? "joined, " : "") + diag::stream_info(stream);
            if (room - name_w - 16.0f > 60.0f)
                hui::ui::text(list, fonts_.regular, shorten(fonts_.regular, info, 20.0f, room - name_w - 16.0f),
                              r.x + 60.0f + name_w + 16.0f, cy + 8.0f, 20.0f, kMuted);
        }
    }
    {
        const Rect r = kPairedDevices;
        glass_card(list, glass, r, 26.0f, devices_amount_);
        hui::ui::text(list, fonts_.semibold, "PAIRED DEVICES", r.x + 30.0f, r.y + 44.0f, 19.0f, kMuted, Align::left, 3.0f);
        if (devices_amount_ > 0.01f)
        {
            list.push_opacity(devices_amount_);
            const float label = hui::ui::text(list, fonts_.semibold, "Manage", r.x + r.w - 30.0f, r.y + 45.0f, 21.0f, kInk, Align::right);
            hui::ui::draw_button(list, fonts_, hui::ui::GlyphStyle::dark(), hui::ui::Button::cross, r.x + r.w - 30.0f - label - 46.0f, r.y + 38.0f, 28.0f);
            list.pop_opacity();
        }
        const std::vector<std::string> paired = diag::paired_names(snap_.running && snap_.devices_known, snap_.devices, snap_.paired);
        if (paired.empty())
        {
            hui::ui::text(list, fonts_.regular, "None yet.", r.x + 30.0f, r.y + 96.0f, 24.0f, kFaint);
        }
        float cx = r.x + 30.0f, cy = r.y + 70.0f;
        for (std::size_t i = 0; i < paired.size(); ++i)
        {
            const std::string name = shorten(fonts_.semibold, paired[i], 21.0f, 220.0f);
            const float w = fonts_.semibold.measure(name, 21.0f) + 62.0f;
            if (cx + w > r.x + r.w - 24.0f)
            {
                cx = r.x + 30.0f;
                cy += 50.0f;
                if (cy > r.y + r.h - 44.0f)
                    break;
            }
            const Rect c{cx, cy, w, 40.0f};
            list.rounded_rect(c, 20.0f, Color::rgb(0xffffff, 0.07f));
            icons::draw(list, Icon::monitor, cx + 24.0f, c.cy(), 20.0f, kAccent);
            hui::ui::text(list, fonts_.semibold, name, cx + 44.0f, c.cy() + 7.0f, 21.0f, kInk);
            cx += w + 10.0f;
        }
    }
    list.pop_transform();
    list.pop_opacity();
}

// ---- drawing: the settings page ---------------------------------------------------------------

void Screen::draw_settings(hui::gfx::DrawList &list, std::uint32_t glass) const
{
    float a = appear(0);
    list.push_opacity(a);
    list.push_transform(1.0f, 0.0f, 0.0f, static_cast<float>(page_direction_) * 40.0f * (1.0f - a), 0.0f);
    const float tw = hui::ui::text(list, fonts_.display, "Settings", 90.0f, 250.0f, 72.0f, kInk);
    hui::ui::text(list, fonts_.regular, "sunshine.conf on this console. The host reads it when it starts.", 120.0f + tw,
                  248.0f, 24.0f, kMuted);
    list.pop_transform();
    list.pop_opacity();

    // The Experimental section's caption, in the gap above its row.
    a = appear(8);
    list.push_opacity(a);
    list.rounded_rect({96.0f, 566.0f, 40.0f, 3.0f}, 1.5f, kAccent);
    const float caption = hui::ui::text(list, fonts_.semibold, "EXPERIMENTAL", 152.0f, 576.0f, 22.0f, kAccent, Align::left, 4.0f);
    const std::string where = snap_.firmware != 0 ? "Firmware " + diag::firmware_text(snap_.firmware) : std::string("this firmware");
    bool any_needed = false;
    for (int i = kFirstSwitch; i < kTiles; ++i)
        any_needed = any_needed || !switch_not_needed(i);
    const std::string note = any_needed ? "For what is untested on " + where + ". Off by default; what fails turns itself off."
                                        : "Nothing to switch on " + where + ": all of this is verified there.";
    hui::ui::text(list, fonts_.regular, note, 176.0f + caption, 576.0f, 22.0f, kMuted);
    list.pop_opacity();

    for (int i = 0; i < kTiles; ++i)
    {
        const OptionSpec &spec = option_specs()[static_cast<std::size_t>(i)];
        const bool toggle = spec.kind == OptionKind::toggle;
        const bool inert = switch_not_needed(i);
        const auto edited = edits_.find(spec.key);
        const auto saved = snap_.config.find(spec.key);
        const bool changed = edits_loaded_ && edited != edits_.end() && saved != snap_.config.end() &&
                             edited->second != saved->second;
        const bool is_default = edited == edits_.end() || !edited->second;
        const bool on = toggle && !is_default && *edited->second == "1";
        const float f = tile_amount_[i];
        a = appear(1 + i);
        const Rect base = tile_rect(i);
        list.push_opacity(a);
        list.push_transform(1.0f + 0.02f * f, base.cx(), base.cy(), 0.0f, 30.0f * (1.0f - a));
        if (f > 0.01f)
            list.glow(base, 24.0f, 26.0f, kAccent.with_alpha(0.28f * f));
        glass_card(list, glass, base, 24.0f, f);
        // The switches' row is narrower: the same parts closer together, with the switch on the value's line.
        const bool narrow = i >= kFirstSwitch;
        const float text_x = base.x + (narrow ? 62.0f : 90.0f);
        icons::draw(list, narrow ? Icon::sliders : kOptionIcons[i], base.x + (narrow ? 34.0f : 46.0f), base.cy(), narrow ? 30.0f : 34.0f,
                    inert ? kFaint : kAccent);
        hui::ui::text(list, fonts_.semibold, narrow ? shorten(fonts_.semibold, spec.label, 26.0f, base.w - 80.0f) : std::string(spec.label),
                      text_x, base.y + 52.0f, narrow ? 26.0f : 30.0f, inert ? kMuted : kInk);
        hui::ui::text(list, fonts_.semibold, shorten(fonts_.semibold, value_text(i), 25.0f, base.w - (inert ? 80.0f : 230.0f)), text_x,
                      base.y + 94.0f, 25.0f, is_default || inert ? kMuted : mix(kInk, kAccent, 0.35f));
        if (toggle && !inert)
        {
            const Rect track{base.x + base.w - 96.0f, base.y + 68.0f, 76.0f, 38.0f};
            list.rounded_rect(track, 19.0f, on ? kAccent : Color::rgb(0x3a4363));
            if (on)
                list.glow(track, 19.0f, 12.0f, kAccent.with_alpha(0.35f));
            list.circle(on ? track.x + track.w - 19.0f : track.x + 19.0f, track.cy(), 15.0f, Color::rgb(0xffffff));
        }
        // On the value's line: default or changed. A switch shows that it is off by itself, and the narrow row has no room
        // for the tag: the Save button shows a change there.
        const std::string tag = narrow ? "" : changed ? "UNSAVED" : is_default && !toggle ? "DEFAULT" : "";
        if (!tag.empty())
        {
            const float w = fonts_.semibold.measure(tag, 16.0f) + 34.0f;
            const Rect t{base.x + base.w - w - (toggle ? 108.0f : 22.0f), base.y + 72.0f, w, 32.0f};
            list.rounded_rect(t, 16.0f, changed ? kAccent.with_alpha(0.22f) : Color::rgb(0xffffff, 0.07f));
            hui::ui::text(list, fonts_.semibold, tag, t.cx(), t.cy() + 6.0f, 16.0f, changed ? kAccent : kMuted,
                          Align::center, 2.0f);
        }
        list.pop_transform();
        list.pop_opacity();
    }

    a = appear(12);
    list.push_opacity(a);
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, 30.0f * (1.0f - a));
    const bool enabled = !busy();
    draw_button(list, glass, settings_action_rect(0), static_cast<int>(Icon::save), dirty() ? "Save" : "Saved",
                dirty(), tile_amount_[kTiles], enabled && dirty());
    draw_button(list, glass, settings_action_rect(1), static_cast<int>(Icon::restart), "Save and restart host", false,
                tile_amount_[kTiles + 1], enabled);
    draw_button(list, glass, settings_action_rect(2), static_cast<int>(Icon::undo), "Revert changes", false,
                tile_amount_[kTiles + 2], dirty());
    const bool web_on = snap_.web_known && snap_.web_enabled;
    draw_button(list, glass, settings_action_rect(3), static_cast<int>(Icon::globe),
                !snap_.web_known ? "Web page" : web_on ? "Web page: on" : "Web page: off", false, tile_amount_[kTiles + 3], true);

    // What the focused part does.
    std::string help;
    std::string extra;
    if (tile_ < kTiles && switch_not_needed(tile_))
    {
        help = diag::experimental_no_effect(snap_.firmware, option_specs()[static_cast<std::size_t>(tile_)].key)
                   ? std::string(ps_exp_switch_at(tile_ - kFirstSwitch)->no_effect_help)
                   : "Verified on " + where + ": it is there without a switch, and this switch changes nothing.";
    }
    else if (tile_ < kTiles)
    {
        const OptionSpec &spec = option_specs()[static_cast<std::size_t>(tile_)];
        help = spec.help;
        extra = std::string("Default: ") + spec.default_label;
    }
    else if (tile_ == kTiles)
        help = "Writes the changes to sunshine.conf. They apply when the host starts next.";
    else if (tile_ == kTiles + 1)
        help = "Writes the changes and restarts the host, which ends a running stream.";
    else if (tile_ == kTiles + 2)
        help = "Puts back what sunshine.conf holds now.";
    else
    {
        help = "The host's page for a browser on your network: " + web_address();
        extra = !snap_.polled            ? std::string()
                : !snap_.running         ? std::string("Host stopped")
                : !snap_.web_known       ? std::string("Not in this host")
                : snap_.web_enabled      ? "On for " + snap_.web_user
                : snap_.web_user.empty() ? std::string("Off: no login")
                                         : "Off, login " + snap_.web_user + " kept";
    }
    const Rect bar{96.0f, 840.0f, 1728.0f, 64.0f};
    glass_card(list, glass, bar, 18.0f, 0.0f);
    list.circle(bar.x + 32.0f, bar.cy(), 6.0f, kAccent);
    hui::ui::text(list, fonts_.regular, help, bar.x + 56.0f, bar.cy() + 8.0f, 24.0f, kInk);
    if (!extra.empty())
        hui::ui::text(list, fonts_.regular, extra, bar.x + bar.w - 30.0f, bar.cy() + 8.0f, 22.0f, kMuted, Align::right);
    list.pop_transform();
    list.pop_opacity();
}

// ---- drawing: the diagnostics page ------------------------------------------------------------

void Screen::draw_diagnostics(hui::gfx::DrawList &list, std::uint32_t glass) const
{
    float a = appear(0);
    list.push_opacity(a);
    list.push_transform(1.0f, 0.0f, 0.0f, static_cast<float>(page_direction_) * 40.0f * (1.0f - a), 0.0f);
    const float tw = hui::ui::text(list, fonts_.display, "Diagnostics", 90.0f, 250.0f, 72.0f, kInk);
    hui::ui::text(list, fonts_.regular, "For a bug report. No addresses, device names, ids or game titles in it.", 120.0f + tw, 248.0f, 24.0f,
                  kMuted);
    list.pop_transform();
    list.pop_opacity();

    a = appear(1);
    list.push_opacity(a);
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, 30.0f * (1.0f - a));
    const Rect panel{96.0f, 286.0f, 1728.0f, 530.0f};
    glass_card(list, glass, panel, 24.0f, 0.0f);
    const int count = static_cast<int>(report_lines_.size());
    if (count == 0)
        hui::ui::text(list, fonts_.regular, "Collecting...", panel.x + 30.0f, panel.y + 50.0f, 24.0f, kMuted);
    for (int i = 0; i < kReportVisible && report_scroll_ + i < count; ++i)
    {
        const std::string &line = report_lines_[static_cast<std::size_t>(report_scroll_ + i)];
        hui::ui::text(list, fonts_.mono, shorten(fonts_.mono, line, 19.0f, panel.w - 80.0f), panel.x + 30.0f,
                      panel.y + 34.0f + static_cast<float>(i) * kReportLine, 19.0f, kInk);
    }
    if (count > kReportVisible)
    {
        // Where the visible part sits in the report.
        const float track = panel.h - 40.0f;
        const float size = std::max(30.0f, track * static_cast<float>(kReportVisible) / static_cast<float>(count));
        const float at = (track - size) * static_cast<float>(report_scroll_) / static_cast<float>(count - kReportVisible);
        list.rounded_rect({panel.x + panel.w - 18.0f, panel.y + 20.0f, 6.0f, track}, 3.0f, Color::rgb(0xffffff, 0.08f));
        list.rounded_rect({panel.x + panel.w - 18.0f, panel.y + 20.0f + at, 6.0f, size}, 3.0f, kAccent);
    }

    const bool enabled = !busy();
    draw_button(list, glass, focus_rect(), static_cast<int>(Icon::restart), "Collect again", false, report_amount_, enabled);
    // Where to get it from: another device on the network, or the console's file system.
    const float x = 470.0f;
    std::string web;
    if (snap_.report_served && !snap_.address.empty())
        web = "On your phone or PC, open http://" + snap_.address + ":" + std::to_string(kReportPort) + " and copy the text.";
    else if (snap_.polled && !snap_.running)
        web = "Start the host to open this from a phone or PC.";
    else
        web = "The network copy is not available.";
    hui::ui::text(list, fonts_.semibold, shorten(fonts_.semibold, web, 25.0f, 1354.0f), x, 866.0f, 25.0f, kInk);
    hui::ui::text(list, fonts_.regular, std::string("Or fetch the file ") + kReportPath + " over FTP.", x, 900.0f, 22.0f, kMuted);
    list.pop_transform();
    list.pop_opacity();
}

void Screen::draw_page(hui::gfx::DrawList &list, std::uint32_t glass) const
{
    draw_top_bar(list, glass);
    if (page_ == Page::home)
        draw_home(list, glass);
    else if (page_ == Page::settings)
        draw_settings(list, glass);
    else
        draw_diagnostics(list, glass);

    // The focus ring glides between the focused parts.
    if (sheet_ == Sheet::none)
    {
        const Rect h = highlight_.rect(time_);
        const bool pill = page_ == Page::home ? home_row_ == 0 : page_ == Page::diagnostics || tile_ >= kTiles;
        const float radius = pill ? h.h / 2.0f + 6.0f : 30.0f;
        const float breathe = 0.75f + 0.25f * std::sin(time_ * 3.0f);
        list.bordered_rect(h.inset(-6.0f), radius, kClear, 3.0f, kAccent.with_alpha(0.95f * breathe));
    }
    if (!has_sheet())
        draw_footer(list, glass);
}

// ---- drawing: the sheet -----------------------------------------------------------------------

void Screen::draw_web(hui::gfx::DrawList &list, const Rect &card) const
{
    // Whether it is on and for whom.
    const bool on = snap_.web_known && snap_.web_enabled;
    std::string state;
    Color dot = kFaint;
    if (!snap_.polled)
        state = "Checking the host...";
    else if (!snap_.running)
        state = "The host is stopped. Start it to set up the web page.";
    else if (!snap_.web_known)
        state = "The running host has no web page. Restart it to use this app's host.";
    else if (!on && snap_.web_user.empty())
    {
        state = "Off. Set a login to turn it on.";
        dot = kMuted;
    }
    else if (!on)
    {
        state = "Off. The login " + snap_.web_user + " is kept for Turn on.";
        dot = kMuted;
    }
    else
    {
        state = "On. Log in as " + snap_.web_user + ".";
        dot = kGood;
    }
    const float state_w = fonts_.semibold.measure(state, 26.0f);
    const float sx = card.cx() - (state_w + 30.0f) / 2.0f;
    if (on)
        list.glow({sx - 8.0f, card.y + 245.0f, 16.0f, 16.0f}, 8.0f, 10.0f, dot.with_alpha(0.5f));
    list.circle(sx, card.y + 253.0f, 8.0f, dot);
    hui::ui::text(list, fonts_.semibold, state, sx + 30.0f, card.y + 262.0f, 26.0f, kInk);

    // The address to open.
    const Rect field{card.x + 90.0f, card.y + 292.0f, card.w - 180.0f, 92.0f};
    list.rounded_rect(field, 22.0f, Color::rgb(0x0a0f22, 0.8f));
    list.bordered_rect(field, 22.0f, kClear, 1.5f, on ? kAccent.with_alpha(0.8f) : Color::rgb(0xffffff, 0.12f));
    hui::ui::text(list, fonts_.semibold, shorten(fonts_.semibold, web_address(), 38.0f, field.w - 60.0f), field.cx(),
                  field.y + 60.0f, 38.0f, on ? kInk : kMuted, Align::center);
    hui::ui::text(list, fonts_.regular, "Open it in a browser on a phone or PC in this network.", card.cx(), card.y + 418.0f,
                  22.0f, kMuted, Align::center);
    hui::ui::text(list, fonts_.regular, "The browser warns about the host's own certificate the first time.", card.cx(),
                  card.y + 448.0f, 22.0f, kFaint, Align::center);

    // The rows, at the same place as in handle_web.
    const Rect h = sheet_highlight_.rect(time_);
    list.rounded_rect(h, 18.0f, kAccent.with_alpha(0.18f));
    list.bordered_rect(h, 18.0f, kClear, 2.5f, kAccent);
    const char *labels[kWebRows] = {snap_.web_enabled ? "Turn off" : "Turn on",
                                    snap_.web_user.empty() ? "Set login" : "Change login", "Forget login", "Close"};
    const Icon row_icons[kWebRows] = {Icon::power, Icon::lock, Icon::trash, Icon::undo};
    for (int i = 0; i < kWebRows; ++i)
    {
        const Rect row = web_row_rect(i);
        const bool enabled = web_row_enabled(static_cast<WebRow>(i));
        const Color ink = !enabled ? kFaint : i == web_row_ ? kInk : kMuted;
        icons::draw(list, row_icons[i], row.x + 40.0f, row.cy(), 28.0f, enabled ? kAccent : kFaint);
        hui::ui::text(list, fonts_.semibold, labels[i], row.x + 76.0f, row.cy() + 9.0f, 26.0f, ink);
    }
}

void Screen::draw_sheet(hui::gfx::DrawList &list, std::uint32_t glass) const
{
    if (sheet_drawn_ == Sheet::none)
        return;
    const float a = ease_out(sheet_amount_);
    list.push_opacity(a);
    list.rounded_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f, Color::rgb(0x04060e, 0.55f));
    const bool keys = sheet_drawn_ == Sheet::pin || sheet_drawn_ == Sheet::text;
    const OptionSpec *spec = find_option(target_);
    float height = keys ? 780.0f : 640.0f;
    if (sheet_drawn_ == Sheet::choice && spec != nullptr)
        height = 300.0f + static_cast<float>(spec->choices.size() + 1) * 76.0f;
    if (sheet_drawn_ == Sheet::remove_device || sheet_drawn_ == Sheet::end_stream || sheet_drawn_ == Sheet::deny_pairing ||
        sheet_drawn_ == Sheet::forget_web)
        height = 270.0f;
    if (sheet_drawn_ == Sheet::web)
        height = kWebSheetHeight;
    const float width = sheet_drawn_ == Sheet::text ? 1120.0f : 880.0f;
    const Rect card = sheet_drawn_ == Sheet::devices || sheet_drawn_ == Sheet::streams
                          ? devices_card()
                          : Rect{960.0f - width / 2.0f, 540.0f - height / 2.0f + 10.0f, width, height};
    list.push_transform(0.94f + 0.06f * a, card.cx(), card.cy(), 0.0f, 30.0f * (1.0f - a));
    list.shadow(card, 34.0f, 60.0f, Color::rgb(0x000000, 0.55f));
    list.glass(glass, card, 34.0f, Color::rgb(0xffffff));
    list.rounded_rect(card, 34.0f, Color::rgb(0x121a36, 0.82f));
    list.bordered_rect(card, 34.0f, kClear, 1.5f, Color::rgb(0xffffff, 0.14f));
    list.gradient_rect({card.x, card.y, card.w, 120.0f}, 34.0f, kAccent.with_alpha(0.14f), kAccent.with_alpha(0.0f));

    const bool device_sheet = sheet_drawn_ == Sheet::devices || sheet_drawn_ == Sheet::remove_device || renaming_ ||
                              sheet_drawn_ == Sheet::streams || sheet_drawn_ == Sheet::end_stream;
    const bool web_sheet = sheet_drawn_ == Sheet::web || sheet_drawn_ == Sheet::forget_web || web_step_ != WebStep::none;
    const Icon icon = sheet_drawn_ == Sheet::pin || sheet_drawn_ == Sheet::deny_pairing ? Icon::link
                      : web_sheet                ? Icon::globe
                      : device_sheet             ? Icon::monitor
                      : spec != nullptr          ? kOptionIcons[std::min<std::size_t>(6, static_cast<std::size_t>(spec - option_specs().data()))]
                                                 : Icon::link; // the device's name when pairing
    icons::draw(list, icon, card.cx(), card.y + 62.0f, 40.0f, kAccent);
    hui::ui::text(list, fonts_.display, sheet_title_, card.cx(), card.y + 140.0f, 46.0f, kInk, Align::center);
    hui::ui::text(list, fonts_.regular, sheet_help_, card.cx(), card.y + 184.0f, 24.0f, kMuted, Align::center);

    if (sheet_drawn_ == Sheet::pin)
    {
        const float size = 104.0f, gap = 24.0f;
        const float x0 = card.cx() - (4 * size + 3 * gap) / 2.0f;
        for (int i = 0; i < 4; ++i)
        {
            const Rect cell{x0 + static_cast<float>(i) * (size + gap), card.y + 220.0f, size, 120.0f};
            const bool current = static_cast<int>(buffer_.size()) == i;
            const bool filled = i < static_cast<int>(buffer_.size());
            if (current)
                list.glow(cell, 20.0f, 18.0f, kAccent.with_alpha(0.3f));
            list.rounded_rect(cell, 20.0f, Color::rgb(0x0a0f22, 0.8f));
            list.bordered_rect(cell, 20.0f, kClear, current ? 3.0f : 1.5f,
                               current ? kAccent : filled ? Color::rgb(0xffffff, 0.3f) : Color::rgb(0xffffff, 0.12f));
            if (filled)
                hui::ui::text(list, fonts_.mono, std::string(1, buffer_[static_cast<std::size_t>(i)]), cell.cx(),
                              cell.y + 84.0f, 62.0f, kInk, Align::center);
            else if (current && std::fmod(time_, 1.0f) < 0.6f)
                list.rounded_rect({cell.cx() - 2.0f, cell.y + 34.0f, 4.0f, 52.0f}, 2.0f, kAccent);
        }
    }
    else if (sheet_drawn_ == Sheet::text)
    {
        const Rect field{card.x + 90.0f, card.y + 220.0f, card.w - 180.0f, 96.0f};
        list.rounded_rect(field, 22.0f, Color::rgb(0x0a0f22, 0.8f));
        list.bordered_rect(field, 22.0f, kClear, 3.0f, kAccent);
        float w = 0.0f;
        if (web_step_ == WebStep::password)
        {
            // A dot a character, as many as fit, and the count on the right; R3 shows the text itself (its end where
            // it is longer than the field), which is only drawn, never logged.
            const float count_w = buffer_.empty() ? 0.0f
                                                  : hui::ui::text(list, fonts_.regular, std::to_string(buffer_.size()),
                                                                  field.x + field.w - 30.0f, field.y + 60.0f, 24.0f, kMuted,
                                                                  Align::right);
            const float room = field.w - 90.0f - count_w;
            if (show_password_)
            {
                std::size_t from = 0;
                while (from < buffer_.size() && fonts_.semibold.measure(buffer_.substr(from), 38.0f) > room)
                    ++from;
                // The ellipsis takes about one character's room.
                const std::string shown =
                    from == 0 ? buffer_ : "\xE2\x80\xA6" + buffer_.substr(std::min(from + 1, buffer_.size()));
                w = hui::ui::text(list, fonts_.semibold, shown, field.x + 30.0f, field.y + 62.0f, 38.0f, kInk);
            }
            else
            {
                const int fit = static_cast<int>(room / 22.0f);
                const int dots = std::min(static_cast<int>(buffer_.size()), std::max(0, fit));
                for (int i = 0; i < dots; ++i)
                    list.circle(field.x + 40.0f + static_cast<float>(i) * 22.0f, field.cy(), 7.0f, kInk);
                w = static_cast<float>(dots) * 22.0f;
            }
        }
        else
        {
            w = hui::ui::text(list, fonts_.semibold, buffer_, field.x + 30.0f, field.y + 62.0f, 38.0f, kInk);
        }
        if (buffer_.empty())
            hui::ui::text(list, fonts_.regular,
                          spec != nullptr                  ? std::string("Default (network name)")
                          : web_step_ == WebStep::user     ? std::string("User name")
                          : web_step_ == WebStep::password ? std::string("Password")
                                                           : pair_suggestion_,
                          field.x + 30.0f, field.y + 62.0f, 32.0f, kFaint);
        if (std::fmod(time_, 1.0f) < 0.6f)
            list.rounded_rect({field.x + 34.0f + (buffer_.empty() ? 0.0f : w), field.y + 24.0f, 3.0f, 50.0f}, 1.5f, kAccent);
    }
    if (keys)
    {
        if (!sheet_error_.empty())
            hui::ui::text(list, fonts_.semibold, sheet_error_, card.cx(), card.y + 380.0f, 24.0f, kBad, Align::center);
        hui::ui::Canvas canvas{list, fonts_, glass, time_};
        keyboard_.draw(canvas);
    }

    if (sheet_drawn_ == Sheet::choice && spec != nullptr)
    {
        // The rows sit at the same place as in handle_sheet.
        const float shift = card.y + 220.0f - 400.0f;
        list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, shift);
        const Rect h = sheet_highlight_.rect(time_);
        list.rounded_rect(h, 18.0f, kAccent.with_alpha(0.18f));
        list.bordered_rect(h, 18.0f, kClear, 2.5f, kAccent);
        const auto it = edits_.find(spec->key);
        int selected = 0;
        for (std::size_t i = 0; it != edits_.end() && it->second && i < spec->choices.size(); ++i)
        {
            if (*it->second == spec->choices[i].value)
                selected = static_cast<int>(i) + 1;
        }
        for (int i = 0; i <= static_cast<int>(spec->choices.size()); ++i)
        {
            const Rect row{660.0f, 400.0f + static_cast<float>(i) * 76.0f, 600.0f, 64.0f};
            const std::string label = i == 0 ? std::string("Default (") + spec->default_label + ")"
                                             : spec->choices[static_cast<std::size_t>(i - 1)].label;
            list.ring(row.x + 40.0f, row.cy(), 13.0f, 2.5f, i == selected ? kAccent : kMuted);
            if (i == selected)
                list.circle(row.x + 40.0f, row.cy(), 6.5f, kAccent);
            hui::ui::text(list, fonts_.semibold, label, row.x + 72.0f, row.cy() + 9.0f, 26.0f, i == choice_ ? kInk : kMuted);
        }
        list.pop_transform();
    }
    if (sheet_drawn_ == Sheet::web)
        draw_web(list, card);
    if (sheet_drawn_ == Sheet::devices)
    {
        const int count = static_cast<int>(snap_.devices.size());
        if (count == 0)
        {
            const std::string empty = !snap_.running       ? "Start the host to see and change the paired devices."
                                      : !snap_.devices_known ? "Restart the host to change the paired devices here."
                                                             : "No device is paired yet.";
            hui::ui::text(list, fonts_.regular, empty, card.cx(), device_row_rect(0).cy() + 8.0f, 26.0f, kMuted, Align::center);
        }
        else
        {
            const Rect h = sheet_highlight_.rect(time_);
            list.rounded_rect(h, 18.0f, kAccent.with_alpha(0.18f));
            list.bordered_rect(h, 18.0f, kClear, 2.5f, kAccent);
        }
        for (int i = device_top_; i < count && i < device_top_ + kDeviceRows; ++i)
        {
            const diag::Device &device = snap_.devices[static_cast<std::size_t>(i)];
            const Rect row = device_row_rect(i - device_top_);
            const bool focused = i == device_row_;
            icons::draw(list, Icon::monitor, row.x + 44.0f, row.cy(), 30.0f, device.streaming ? kGood : kAccent);
            hui::ui::text(list, fonts_.semibold, shorten(fonts_.semibold, diag::device_label(device), 28.0f, row.w - 120.0f),
                          row.x + 86.0f, row.y + 36.0f, 28.0f, focused ? kInk : kMuted);
            hui::ui::text(list, fonts_.regular, diag::device_info(device), row.x + 86.0f, row.y + 66.0f, 21.0f,
                          device.streaming ? kGood : focused ? kMuted : kFaint);
        }
        if (count > kDeviceRows)
        {
            // Where the rows in view sit in the list.
            const Rect first = device_row_rect(0);
            const float span = 90.0f * static_cast<float>(kDeviceRows) - 10.0f;
            const float x = first.x + first.w + 16.0f;
            list.rounded_rect({x, first.y, 5.0f, span}, 2.5f, Color::rgb(0xffffff, 0.1f));
            list.rounded_rect({x, first.y + span * static_cast<float>(device_top_) / static_cast<float>(count), 5.0f,
                               span * static_cast<float>(kDeviceRows) / static_cast<float>(count)},
                              2.5f, kAccent);
        }
    }
    if (sheet_drawn_ == Sheet::streams)
    {
        const int count = static_cast<int>(snap_.streams.size());
        if (count == 0)
        {
            hui::ui::text(list, fonts_.regular, "No one is streaming now.", card.cx(), device_row_rect(0).cy() + 8.0f, 26.0f, kMuted,
                          Align::center);
        }
        else
        {
            const Rect h = sheet_highlight_.rect(time_);
            list.rounded_rect(h, 18.0f, kAccent.with_alpha(0.18f));
            list.bordered_rect(h, 18.0f, kClear, 2.5f, kAccent);
        }
        for (int i = device_top_; i < count && i < device_top_ + kDeviceRows; ++i)
        {
            const diag::Stream &stream = snap_.streams[static_cast<std::size_t>(i)];
            const Rect row = device_row_rect(i - device_top_);
            const bool focused = i == device_row_;
            icons::draw(list, Icon::monitor, row.x + 44.0f, row.cy(), 30.0f, kGood);
            // The time on the right; the address after the name, where there is room.
            const std::string right = stream.role == "starting" ? std::string("Starting") : "for " + diag::duration_text(stream.seconds);
            const float right_w = hui::ui::text(list, fonts_.regular, right, row.x + row.w - 24.0f, row.y + 36.0f, 21.0f,
                                                focused ? kMuted : kFaint, Align::right);
            const float room = row.w - 86.0f - 24.0f - right_w - 24.0f;
            const float name_w = hui::ui::text(list, fonts_.semibold, shorten(fonts_.semibold, diag::stream_label(stream), 28.0f, room * 0.7f),
                                               row.x + 86.0f, row.y + 36.0f, 28.0f, focused ? kInk : kMuted);
            if (!stream.address.empty() && room - name_w - 14.0f > 80.0f)
                hui::ui::text(list, fonts_.regular, shorten(fonts_.regular, stream.address, 21.0f, room - name_w - 14.0f),
                              row.x + 86.0f + name_w + 14.0f, row.y + 36.0f, 21.0f, kFaint);
            // What really reaches the device on the right, under the time: the rate it gets, the round trip and its losses.
            const std::string network = diag::stream_network(stream);
            const float network_w = network.empty() ? 0.0f
                                                    : hui::ui::text(list, fonts_.regular, network, row.x + row.w - 24.0f, row.y + 66.0f,
                                                                    21.0f, focused ? kMuted : kFaint, Align::right) +
                                                          24.0f;
            const std::string info = (stream.role == "joined" ? "Joined, " : "") + diag::stream_info(stream);
            hui::ui::text(list, fonts_.regular, shorten(fonts_.regular, info, 21.0f, row.w - 110.0f - network_w), row.x + 86.0f,
                          row.y + 66.0f, 21.0f, kGood);
        }
        if (count > kDeviceRows)
        {
            const Rect first = device_row_rect(0);
            const float span = 90.0f * static_cast<float>(kDeviceRows) - 10.0f;
            const float x = first.x + first.w + 16.0f;
            list.rounded_rect({x, first.y, 5.0f, span}, 2.5f, Color::rgb(0xffffff, 0.1f));
            list.rounded_rect({x, first.y + span * static_cast<float>(device_top_) / static_cast<float>(count), 5.0f,
                               span * static_cast<float>(kDeviceRows) / static_cast<float>(count)},
                              2.5f, kAccent);
        }
    }
    if (sheet_drawn_ == Sheet::number && spec != nullptr)
    {
        const std::string text = number_default_ ? std::string("Default") : std::to_string(number_);
        const float cy = card.y + 360.0f;
        hui::ui::text(list, fonts_.display, text, card.cx(), cy + 40.0f, number_default_ ? 72.0f : 110.0f, kInk, Align::center);
        if (number_default_)
            hui::ui::text(list, fonts_.regular, spec->default_label, card.cx(), cy + 90.0f, 26.0f, kMuted, Align::center);
        // Arrows and the range.
        for (int side = -1; side <= 1; side += 2)
        {
            const float x = card.cx() + static_cast<float>(side) * 330.0f;
            const Rect r{x - 36.0f, cy - 36.0f, 72.0f, 72.0f};
            list.rounded_rect(r, 36.0f, Color::rgb(0xffffff, 0.08f));
            list.triangle({x - 14.0f, cy - 14.0f, 28.0f, 28.0f}, kAccent, 0.0f, side < 0 ? -1.5708f : 1.5708f);
        }
        const float t = static_cast<float>(number_ - spec->min) / static_cast<float>(std::max(1L, spec->max - spec->min));
        const Rect track{card.x + 140.0f, card.y + 520.0f, card.w - 280.0f, 8.0f};
        list.rounded_rect(track, 4.0f, Color::rgb(0xffffff, 0.1f));
        list.gradient_rect_h({track.x, track.y, std::max(8.0f, track.w * t), track.h}, 4.0f, kAccent, kAccentDeep);
        hui::ui::text(list, fonts_.regular, std::to_string(spec->min), track.x, track.y + 40.0f, 20.0f, kFaint);
        hui::ui::text(list, fonts_.regular, std::to_string(spec->max), track.x + track.w, track.y + 40.0f, 20.0f, kFaint,
                      Align::right);
    }
    list.pop_transform();
    list.pop_opacity();
    draw_footer(list, glass);
}

} // namespace psapp
