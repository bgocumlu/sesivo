#include "juce_mixer_component.h"

#include "juce_theme.h"
#include "opus_network_clock.h"
#include "packet_builder.h"
#include "performer_join_token.h"
#include "protocol.h"
#include "secure_invite.h"

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using asio::ip::udp;
using namespace std::chrono_literals;

namespace {
constexpr int PAD = juce_theme::pad;
constexpr int GAP = juce_theme::gap;
constexpr int ROW = juce_theme::row_height;
constexpr int STATUS_HEIGHT = 66;
constexpr int BOTTOM_HEIGHT = 170;
constexpr std::array<int, 7> BUFFER_FRAME_OPTIONS{96, 120, 128, 240, 256, 480, 960};

struct DockLayout {
    juce::Rectangle<int> local_audio;
    juce::Rectangle<int> network;
    juce::Rectangle<int> redundancy;
    juce::Rectangle<int> metronome;
    juce::Rectangle<int> recording;
    juce::Rectangle<int> wav;
};

DockLayout make_dock_layout(juce::Rectangle<int> bounds) {
    const int width = bounds.getWidth();
    int local_width = juce::jlimit(285, 360, width * 25 / 100);
    int network_width = juce::jlimit(320, 430, width * 30 / 100);
    int tools_width = juce::jlimit(145, 175, width * 12 / 100);
    int wav_width = width - local_width - network_width - tools_width - (3 * GAP);

    if (wav_width < 260) {
        int deficit = 260 - wav_width;
        const int local_shrink = std::min(deficit / 2, local_width - 285);
        local_width -= local_shrink;
        deficit -= local_shrink;
        const int network_shrink = std::min(deficit, network_width - 300);
        network_width -= network_shrink;
        deficit -= network_shrink;
        const int tools_shrink = std::min(deficit, tools_width - 140);
        tools_width -= tools_shrink;
        wav_width = width - local_width - network_width - tools_width - (3 * GAP);
    }

    DockLayout layout;
    layout.local_audio = bounds.removeFromLeft(local_width);
    bounds.removeFromLeft(GAP);
    layout.network = bounds.removeFromLeft(network_width);
    bounds.removeFromLeft(GAP);
    layout.metronome = bounds.removeFromLeft(tools_width);
    bounds.removeFromLeft(GAP);
    layout.wav = bounds;
    return layout;
}

bool is_supported_buffer_frame_option(int frames) {
    return std::find(BUFFER_FRAME_OPTIONS.begin(), BUFFER_FRAME_OPTIONS.end(), frames) !=
           BUFFER_FRAME_OPTIONS.end();
}

juce::String format_sample_count(uint64_t value) {
    if (value == 0) {
        return "0";
    }

    juce::String out;
    int group_digits = 0;
    while (value > 0) {
        if (group_digits == 3) {
            out = "," + out;
            group_digits = 0;
        }
        out = juce::String(static_cast<int>(value % 10)) + out;
        value /= 10;
        ++group_digits;
    }
    return out;
}

juce::String buffer_match_suffix(const AudioStream::LatencyInfo& latency) {
    if (latency.requested_buffer_frames <= 0 || latency.actual_buffer_frames <= 0) {
        return {};
    }
    if (latency.requested_buffer_frames == latency.actual_buffer_frames) {
        return " match";
    }
    return " req " + juce::String(latency.requested_buffer_frames);
}

juce::String device_latency_warning(const AudioStream::LatencyInfo& latency,
                                    double device_path_ms) {
    if (latency.requested_buffer_frames > 0 && latency.actual_buffer_frames > 0 &&
        latency.requested_buffer_frames != latency.actual_buffer_frames) {
        return "Device changed buffer";
    }
    if (device_path_ms >= 20.0) {
        return "High device latency";
    }
    return {};
}

int label_line_count(const juce::String& text) {
    const auto bytes = text.toStdString();
    return static_cast<int>(std::count(bytes.begin(), bytes.end(), '\n')) + 1;
}

int diagnostics_label_height(const juce::String& text) {
    return std::max(ROW, (label_line_count(text) * 15) + 10);
}

juce::String opus_packet_label(int frames) {
    if (frames == opus_network_clock::LOW_LATENCY_FRAME_COUNT) {
        return "Low";
    }
    if (frames == opus_network_clock::FAST_FRAME_COUNT) {
        return "Fast";
    }
    if (frames == opus_network_clock::BALANCED_FRAME_COUNT) {
        return "Balanced";
    }
    return "Stable";
}

juce::String format_timecode(int64_t frames, int sample_rate) {
    if (sample_rate <= 0 || frames <= 0) {
        return "0:00";
    }

    int64_t seconds = frames / sample_rate;
    const int64_t hours = seconds / 3600;
    seconds %= 3600;
    const int64_t minutes = seconds / 60;
    seconds %= 60;

    auto two_digits = [](int64_t value) {
        return juce::String(value).paddedLeft('0', 2);
    };
    if (hours > 0) {
        return juce::String(hours) + ":" + two_digits(minutes) + ":" +
               two_digits(seconds);
    }
    return juce::String(minutes) + ":" + two_digits(seconds);
}

juce::String wav_time_label_text(const ClientAppFacade::WavState& wav) {
    return format_timecode(wav.position, wav.sample_rate) + " / " +
           format_timecode(wav.total_frames, wav.sample_rate);
}

juce::String redundancy_label(int depth, int effective_depth) {
    if (depth == OPUS_REDUNDANCY_DEPTH_AUTO) {
        return "Auto (" + juce::String(effective_depth) + ")";
    }
    if (depth == 0) {
        return "Off";
    }
    return juce::String(depth) + " prev";
}

void configure_linear_slider(juce::Slider& slider, double min_value, double max_value,
                             double interval, juce::String suffix = {}) {
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 74, 20);
    slider.setRange(min_value, max_value, interval);
    slider.setTextValueSuffix(suffix);
}

void add_all(juce::Component& parent, std::initializer_list<juce::Component*> children) {
    for (auto* child: children) {
        parent.addAndMakeVisible(child);
    }
}

std::filesystem::path unique_child_path(const std::filesystem::path& parent,
                                        const std::filesystem::path& source) {
    const auto base = source.filename().string();
    std::filesystem::path candidate = parent / source.filename();
    std::error_code ec;
    if (!std::filesystem::exists(candidate, ec)) {
        return candidate;
    }

    for (int suffix = 1; suffix < 1000; ++suffix) {
        candidate = parent / (base + "_" + std::to_string(suffix));
        ec.clear();
        if (!std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return parent / (base + "_copy");
}

bool save_recording_folder(const juce::File& recording_folder,
                           const juce::File& destination_root,
                           juce::File& saved_folder,
                           juce::String& error) {
    const auto source = std::filesystem::path{
        recording_folder.getFullPathName().toStdString()};
    const auto parent = std::filesystem::path{
        destination_root.getFullPathName().toStdString()};

    std::error_code ec;
    if (!std::filesystem::is_directory(source, ec)) {
        error = "Recording folder is missing";
        return false;
    }
    ec.clear();
    if (!std::filesystem::is_directory(parent, ec)) {
        error = "Choose an existing destination folder";
        return false;
    }

    auto target = unique_child_path(parent, source);
    ec.clear();
    if (std::filesystem::equivalent(source, target, ec)) {
        saved_folder = juce::File(target.string());
        return true;
    }

    ec.clear();
    std::filesystem::rename(source, target, ec);
    if (ec) {
        ec.clear();
        std::filesystem::copy(source, target,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::copy_symlinks,
                              ec);
        if (ec) {
            error = "Could not copy recording";
            return false;
        }

        ec.clear();
        std::filesystem::remove_all(source, ec);
        if (ec) {
            error = "Saved copy, but could not remove temp recording";
            saved_folder = juce::File(target.string());
            return false;
        }
    }

    saved_folder = juce::File(target.string());
    return true;
}

template <size_t N>
std::string fixed_string(const Bytes<N>& bytes) {
    const auto end = std::find(bytes.begin(), bytes.end(), '\0');
    return std::string(bytes.begin(), end);
}

template <size_t N>
void write_fixed(Bytes<N>& target, const std::string& value) {
    packet_builder::write_fixed(target, value);
}

template <typename Request, typename Response>
Response send_control_request(const std::string& address, uint16_t port,
                              const Request& request, CtrlHdr::Cmd expected_type,
                              uint32_t request_id) {
    asio::io_context context;
    udp::resolver resolver(context);
    auto endpoints = resolver.resolve(address, std::to_string(port));
    if (endpoints.empty()) {
        throw std::runtime_error("server address did not resolve");
    }

    udp::endpoint endpoint = *endpoints.begin();
    udp::socket socket(context);
    socket.open(endpoint.protocol());
    socket.non_blocking(true);
    socket.send_to(asio::buffer(&request, sizeof(request)), endpoint);

    std::array<unsigned char, 2048> buffer{};
    udp::endpoint sender;
    const auto deadline = std::chrono::steady_clock::now() + 1100ms;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code ec;
        const size_t bytes = socket.receive_from(asio::buffer(buffer), sender, 0, ec);
        if (!ec) {
            if (bytes < sizeof(Response)) {
                continue;
            }
            CtrlHdr ctrl{};
            std::memcpy(&ctrl, buffer.data(), sizeof(ctrl));
            if (ctrl.magic != CTRL_MAGIC || ctrl.type != expected_type) {
                continue;
            }
            Response response{};
            std::memcpy(&response, buffer.data(), sizeof(response));
            if (response.request_id != request_id) {
                continue;
            }
            return response;
        }
        if (ec != asio::error::would_block && ec != asio::error::try_again) {
            throw std::runtime_error(ec.message());
        }
        std::this_thread::sleep_for(1ms);
    }
    throw std::runtime_error("server did not respond");
}

juce::String room_admin_status_text(uint8_t status, const std::string& reason) {
    if (!reason.empty() && reason != "ok") {
        return reason;
    }
    switch (status) {
        case ROOM_STATUS_OK:
            return "Done";
        case ROOM_STATUS_BAD_REQUEST:
            return "Bad request";
        case ROOM_STATUS_NOT_FOUND:
            return "Not found";
        case ROOM_STATUS_FORBIDDEN:
            return "Not allowed";
        case ROOM_STATUS_CONFLICT:
            return "Conflict";
        case ROOM_STATUS_SERVER_ERROR:
            return "Server error";
        default:
            return "Request failed";
    }
}

juce::String admin_participant_label(const ParticipantInfo& participant) {
    juce::String name;
    if (!participant.display_name.empty()) {
        name = participant.display_name;
    } else if (!participant.profile_id.empty()) {
        name = participant.profile_id;
    } else {
        name = "User";
    }
    return name + " #" + juce::String(static_cast<int>(participant.id));
}

uint8_t access_mode_from_combo_id(int selected_id) {
    switch (selected_id) {
        case 2:
            return ROOM_ACCESS_PASSWORD;
        case 3:
            return ROOM_ACCESS_APPROVE;
        default:
            return ROOM_ACCESS_OPEN;
    }
}

int combo_id_for_access_mode(uint8_t access_mode) {
    switch (access_mode) {
        case ROOM_ACCESS_PASSWORD:
            return 2;
        case ROOM_ACCESS_APPROVE:
            return 3;
        default:
            return 1;
    }
}

class RoomSettingsDialog final : public juce::Component {
public:
    RoomSettingsDialog(uint8_t current_access_mode,
                       std::function<void(uint8_t, std::string)> on_apply)
        : on_apply_(std::move(on_apply)) {
        setSize(420, 218);
        title_.setText("Room Settings", juce::dontSendNotification);
        juce_theme::style_label(title_, juce_theme::colour::text(), 16.0F, true);
        access_label_.setText("Access", juce::dontSendNotification);
        juce_theme::style_label(access_label_, juce_theme::colour::text_dim(), 12.5F);
        password_label_.setText("Password", juce::dontSendNotification);
        juce_theme::style_label(password_label_, juce_theme::colour::text_dim(), 12.5F);
        status_label_.setText({}, juce::dontSendNotification);
        juce_theme::style_label(status_label_, juce_theme::colour::warning(), 12.0F);

        access_combo_.addItem("Open", 1);
        access_combo_.addItem("Password", 2);
        access_combo_.addItem("Approve", 3);
        access_combo_.setSelectedId(combo_id_for_access_mode(current_access_mode),
                                    juce::dontSendNotification);
        access_combo_.onChange = [this]() { update_password_visibility(); };

        password_editor_.setTextToShowWhenEmpty("Room password",
                                                juce_theme::colour::text_faint());
        juce_theme::style_editor(password_editor_, 14.0F);
        apply_button_.setButtonText("Apply");
        cancel_button_.setButtonText("Cancel");
        apply_button_.onClick = [this]() { apply(); };
        cancel_button_.onClick = [this]() { close(); };

        addAndMakeVisible(title_);
        addAndMakeVisible(access_label_);
        addAndMakeVisible(access_combo_);
        addAndMakeVisible(password_label_);
        addAndMakeVisible(password_editor_);
        addAndMakeVisible(status_label_);
        addAndMakeVisible(apply_button_);
        addAndMakeVisible(cancel_button_);
        update_password_visibility();
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce_theme::colour::panel_bottom());
    }

    void resized() override {
        auto area = getLocalBounds().reduced(20, 16);
        title_.setBounds(area.removeFromTop(24));
        area.removeFromTop(12);
        auto access_row = area.removeFromTop(32);
        access_label_.setBounds(access_row.removeFromLeft(82));
        access_combo_.setBounds(access_row);
        area.removeFromTop(10);
        auto password_row = area.removeFromTop(32);
        password_label_.setBounds(password_row.removeFromLeft(82));
        password_editor_.setBounds(password_row);
        area.removeFromTop(8);
        status_label_.setBounds(area.removeFromTop(22));
        auto buttons = area.removeFromBottom(34);
        cancel_button_.setBounds(buttons.removeFromRight(88).reduced(2));
        buttons.removeFromRight(8);
        apply_button_.setBounds(buttons.removeFromRight(88).reduced(2));
    }

private:
    void update_password_visibility() {
        const bool password_mode =
            access_mode_from_combo_id(access_combo_.getSelectedId()) == ROOM_ACCESS_PASSWORD;
        password_label_.setVisible(password_mode);
        password_editor_.setVisible(password_mode);
        if (!password_mode) {
            password_editor_.clear();
        }
    }

    void apply() {
        const uint8_t access_mode =
            access_mode_from_combo_id(access_combo_.getSelectedId());
        const auto password = password_editor_.getText().trim().toStdString();
        if (access_mode == ROOM_ACCESS_PASSWORD && password.empty()) {
            status_label_.setText("Password is required for password access",
                                  juce::dontSendNotification);
            return;
        }
        if (on_apply_) {
            on_apply_(access_mode, password);
        }
        close();
    }

    void close() {
        if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>()) {
            dialog->exitModalState(0);
        }
    }

    std::function<void(uint8_t, std::string)> on_apply_;
    juce::Label title_;
    juce::Label access_label_;
    juce::ComboBox access_combo_;
    juce::Label password_label_;
    juce::TextEditor password_editor_;
    juce::Label status_label_;
    juce::TextButton apply_button_;
    juce::TextButton cancel_button_;
};

class ParticipantAdminRow final : public juce::Component {
public:
    ParticipantAdminRow(const ParticipantInfo& participant, bool waiting,
                        std::function<void(uint32_t)> on_approve,
                        std::function<void(uint32_t)> on_decline,
                        std::function<void(uint32_t)> on_kick)
        : participant_id_(participant.id),
          waiting_(waiting),
          on_approve_(std::move(on_approve)),
          on_decline_(std::move(on_decline)),
          on_kick_(std::move(on_kick)) {
        name_.setText(admin_participant_label(participant), juce::dontSendNotification);
        juce_theme::style_label(name_, juce_theme::colour::text(), 13.5F, true);
        addAndMakeVisible(name_);
        if (waiting_) {
            approve_button_.setButtonText(juce::String::fromUTF8("\xE2\x9C\x93"));
            decline_button_.setButtonText("X");
            approve_button_.onClick = [this]() { invoke(on_approve_); };
            decline_button_.onClick = [this]() { invoke(on_decline_); };
            addAndMakeVisible(approve_button_);
            addAndMakeVisible(decline_button_);
        } else {
            kick_button_.setButtonText("Kick");
            kick_button_.onClick = [this]() { invoke(on_kick_); };
            addAndMakeVisible(kick_button_);
        }
    }

    void paint(juce::Graphics& g) override {
        auto area = getLocalBounds().toFloat().reduced(0.5F, 2.0F);
        g.setColour(waiting_ ? juce::Colour{0xff2a241f}
                             : juce_theme::colour::row());
        g.fillRoundedRectangle(area, juce_theme::radius);
        g.setColour(waiting_ ? juce_theme::colour::accent()
                             : juce_theme::colour::border_soft());
        g.drawRoundedRectangle(area, juce_theme::radius, 1.0F);
    }

    void resized() override {
        auto row = getLocalBounds().reduced(10, 5);
        if (waiting_) {
            decline_button_.setBounds(row.removeFromRight(44).reduced(2));
            row.removeFromRight(6);
            approve_button_.setBounds(row.removeFromRight(44).reduced(2));
        } else {
            kick_button_.setBounds(row.removeFromRight(72).reduced(2));
        }
        name_.setBounds(row);
    }

private:
    void invoke(const std::function<void(uint32_t)>& callback) {
        if (callback) {
            callback(participant_id_);
        }
    }

    uint32_t participant_id_ = 0;
    bool waiting_ = false;
    std::function<void(uint32_t)> on_approve_;
    std::function<void(uint32_t)> on_decline_;
    std::function<void(uint32_t)> on_kick_;
    juce::Label name_;
    juce::TextButton approve_button_;
    juce::TextButton decline_button_;
    juce::TextButton kick_button_;
};

class ParticipantsDialog final : public juce::Component, private juce::Timer {
public:
    ParticipantsDialog(std::function<std::vector<ParticipantInfo>()> get_waiting,
                       std::function<std::vector<ParticipantInfo>()> get_participants,
                       std::function<void(uint32_t)> on_approve,
                       std::function<void(uint32_t)> on_decline,
                       std::function<void(uint32_t)> on_kick)
        : get_waiting_(std::move(get_waiting)),
          get_participants_(std::move(get_participants)),
          on_approve_(std::move(on_approve)),
          on_decline_(std::move(on_decline)),
          on_kick_(std::move(on_kick)) {
        setSize(540, 320);
        title_.setText("Participants", juce::dontSendNotification);
        juce_theme::style_label(title_, juce_theme::colour::text(), 16.0F, true);
        juce_theme::style_label(waiting_label_, juce_theme::colour::text_dim(), 12.5F, true);
        juce_theme::style_label(in_room_label_, juce_theme::colour::text_dim(), 12.5F, true);
        juce_theme::style_label(empty_label_, juce_theme::colour::text_faint(), 13.0F);
        close_button_.setButtonText("Close");
        close_button_.onClick = [this]() {
            if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>()) {
                dialog->exitModalState(0);
            }
        };
        viewport_.setScrollBarsShown(true, false);
        viewport_.setViewedComponent(&rows_content_, false);

        addAndMakeVisible(title_);
        rows_content_.addAndMakeVisible(waiting_label_);
        rows_content_.addAndMakeVisible(in_room_label_);
        rows_content_.addAndMakeVisible(empty_label_);
        addAndMakeVisible(viewport_);
        addAndMakeVisible(close_button_);
        refresh_rows(true);
        startTimerHz(4);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce_theme::colour::panel_bottom());
    }

    void resized() override {
        auto area = getLocalBounds().reduced(20, 16);
        title_.setBounds(area.removeFromTop(24));
        area.removeFromTop(10);
        close_button_.setBounds(getWidth() - 108, getHeight() - 50, 88, 30);
        area.removeFromBottom(48);
        viewport_.setBounds(area);
        layout_rows_content();
    }

private:
    void timerCallback() override {
        refresh_rows(false);
    }

    static std::string signature_for(const std::vector<ParticipantInfo>& waiting,
                                     const std::vector<ParticipantInfo>& participants) {
        std::string out;
        out.reserve((waiting.size() + participants.size()) * 24);
        auto append = [&](char prefix, const ParticipantInfo& info) {
            out.push_back(prefix);
            out += std::to_string(info.id);
            out.push_back(':');
            out += info.display_name;
            out.push_back(':');
            out += info.profile_id;
            out.push_back('|');
        };
        for (const auto& info: waiting) {
            append('w', info);
        }
        for (const auto& info: participants) {
            append('p', info);
        }
        return out;
    }

    void remove_rows() {
        for (auto& row: waiting_rows_) {
            rows_content_.removeChildComponent(row.get());
        }
        for (auto& row: participant_rows_) {
            rows_content_.removeChildComponent(row.get());
        }
        waiting_rows_.clear();
        participant_rows_.clear();
    }

    void layout_rows_content() {
        const int row_count = static_cast<int>(waiting_rows_.size() +
                                               participant_rows_.size());
        const bool empty = row_count == 0;
        const int content_height =
            20 + static_cast<int>(waiting_rows_.size()) * 36 + 10 + 20 +
            static_cast<int>(participant_rows_.size()) * 36 + (empty ? 28 : 8);
        rows_content_.setSize(juce::jmax(1, viewport_.getWidth() - 14),
                              juce::jmax(viewport_.getHeight(), content_height));

        auto area = rows_content_.getLocalBounds().reduced(0, 0);
        waiting_label_.setBounds(area.removeFromTop(20));
        for (auto& row: waiting_rows_) {
            row->setBounds(area.removeFromTop(36));
        }
        area.removeFromTop(10);
        in_room_label_.setBounds(area.removeFromTop(20));
        for (auto& row: participant_rows_) {
            row->setBounds(area.removeFromTop(36));
        }
        empty_label_.setBounds(area.removeFromTop(28));
    }

    void refresh_rows(bool force) {
        const auto waiting = get_waiting_ ? get_waiting_()
                                          : std::vector<ParticipantInfo>{};
        const auto participants = get_participants_ ? get_participants_()
                                                    : std::vector<ParticipantInfo>{};
        const auto signature = signature_for(waiting, participants);
        if (!force && signature == rows_signature_) {
            return;
        }
        rows_signature_ = signature;

        remove_rows();
        waiting_label_.setText(
            "Waiting (" + juce::String(static_cast<int>(waiting.size())) + ")",
            juce::dontSendNotification);
        in_room_label_.setText(
            "In room (" + juce::String(static_cast<int>(participants.size())) + ")",
            juce::dontSendNotification);
        empty_label_.setText(waiting.empty() && participants.empty()
                                 ? "No participants"
                                 : "",
                             juce::dontSendNotification);

        for (const auto& participant: waiting) {
            auto row = std::make_unique<ParticipantAdminRow>(
                participant, true, on_approve_, on_decline_, on_kick_);
            rows_content_.addAndMakeVisible(*row);
            waiting_rows_.push_back(std::move(row));
        }
        for (const auto& participant: participants) {
            auto row = std::make_unique<ParticipantAdminRow>(
                participant, false, on_approve_, on_decline_, on_kick_);
            rows_content_.addAndMakeVisible(*row);
            participant_rows_.push_back(std::move(row));
        }
        layout_rows_content();
        repaint();
    }

    std::function<std::vector<ParticipantInfo>()> get_waiting_;
    std::function<std::vector<ParticipantInfo>()> get_participants_;
    std::function<void(uint32_t)> on_approve_;
    std::function<void(uint32_t)> on_decline_;
    std::function<void(uint32_t)> on_kick_;
    std::string rows_signature_;
    juce::Label title_;
    juce::Viewport viewport_;
    juce::Component rows_content_;
    juce::Label waiting_label_;
    juce::Label in_room_label_;
    juce::Label empty_label_;
    juce::TextButton close_button_;
    std::vector<std::unique_ptr<ParticipantAdminRow>> waiting_rows_;
    std::vector<std::unique_ptr<ParticipantAdminRow>> participant_rows_;
};

bool matches_api(const AudioStream::DeviceInfo& device, const std::string& api_name) {
    return api_name.empty() || api_name == "All" || device.api_name == api_name;
}

int api_index_for_name(const std::vector<AudioStream::ApiInfo>& apis,
                       const std::string& api_name) {
    if (api_name.empty() || api_name == "All") {
        return -1;
    }
    for (const auto& api: apis) {
        if (api.name == api_name) {
            return api.index;
        }
    }
    return -1;
}

std::string api_name_for_index(const std::vector<AudioStream::ApiInfo>& apis,
                               int api_index) {
    if (api_index < 0) {
        return "All";
    }
    for (const auto& api: apis) {
        if (api.index == api_index) {
            return api.name;
        }
    }
    return "All";
}

bool contains_device_id(const std::vector<AudioStream::DeviceInfo>& devices,
                        AudioStream::DeviceIndex id) {
    return std::any_of(devices.begin(), devices.end(), [&](const auto& device) {
        return device.index == id;
    });
}

AudioStream::DeviceIndex first_device_for_api(
    const std::vector<AudioStream::DeviceInfo>& devices, const std::string& api_name) {
    const auto it = std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
        return matches_api(device, api_name);
    });
    return it == devices.end() ? AudioStream::NO_DEVICE : it->index;
}

AudioStream::DeviceIndex device_id_for_api(
    const std::vector<AudioStream::DeviceInfo>& devices,
    AudioStream::DeviceIndex id, const std::string& api_name) {
    const auto it = std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
        return device.index == id && matches_api(device, api_name);
    });
    return it == devices.end() ? AudioStream::NO_DEVICE : it->index;
}

AudioStream::DeviceIndex default_device_for_api(
    const std::vector<AudioStream::DeviceInfo>& devices,
    const std::vector<AudioStream::ApiInfo>& apis, int api_index, bool input) {
    if (api_index >= 0) {
        for (const auto& api: apis) {
            if (api.index != api_index) {
                continue;
            }
            const auto default_device =
                input ? api.default_input_device : api.default_output_device;
            if (contains_device_id(devices, default_device)) {
                return default_device;
            }
            return first_device_for_api(devices, api.name);
        }
    }

    const auto it = std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
        return input ? device.is_default_input : device.is_default_output;
    });
    if (it != devices.end()) {
        return it->index;
    }
    return devices.empty() ? AudioStream::NO_DEVICE : devices.front().index;
}

}  // namespace

JuceMixerComponent::JuceMixerComponent(
    ClientAppFacade& client, JuceClientStartupOptions startup_options,
    std::function<void()> leave_callback)
    : client_(client),
      startup_options_(std::move(startup_options)),
      leave_callback_(std::move(leave_callback)),
      participants_component_(client) {
    configure_controls();
    configure_device_controls();
    startTimerHz(30);
}

JuceMixerComponent::~JuceMixerComponent() {
    stopTimer();
    if (connection_job_thread_.joinable()) {
        connection_job_thread_.join();
    }
    if (device_job_thread_.joinable()) {
        device_job_thread_.join();
    }
    if (room_admin_job_thread_.joinable()) {
        room_admin_job_thread_.join();
    }
}

void JuceMixerComponent::configure_controls() {
    setOpaque(true);

    juce_theme::style_label(diagnostics_label_, juce_theme::colour::text_dim(), 12.0F);
    diagnostics_label_.setJustificationType(juce::Justification::topLeft);
    network_viewport_.setViewedComponent(&network_content_, false);
    network_viewport_.setScrollBarsShown(true, false);
    network_viewport_.setScrollBarThickness(6);
    auto configure_section = [](juce::Label& label, const juce::String& text) {
        label.setText(text, juce::dontSendNotification);
        juce_theme::style_label(label, juce_theme::colour::text(), 14.0F, true);
    };
    auto configure_caption = [](juce::Label& label, const juce::String& text) {
        label.setText(text, juce::dontSendNotification);
        juce_theme::style_label(label, juce_theme::colour::text_faint(), 12.0F);
    };
    configure_section(local_audio_label_, "Local Audio");
    configure_section(network_label_, "Network");
    configure_section(redundancy_section_label_, "Redundancy");
    configure_section(metronome_label_, "Metronome");
    configure_section(recording_label_, "Record");
    configure_section(wav_label_, "WAV");
    configure_section(room_admin_label_, "Room Admin");
    configure_caption(packet_label_, "Packet");
    configure_caption(jitter_label_, "Jitter");
    configure_caption(queue_label_, "Queue");
    configure_caption(age_limit_label_, "Age");
    configure_caption(redundancy_label_, "Mode");
    configure_caption(wav_position_label_, "0:00 / 0:00");
    configure_caption(wav_gain_label_, "Gain 1.00x");
    room_admin_status_label_.setText(room_admin_status_, juce::dontSendNotification);
    juce_theme::style_label(room_admin_status_label_, juce_theme::colour::text_dim(),
                            12.0F);
    recording_saved_label_.setText(juce::String(juce::CharPointer_UTF8("\xe2\x9c\x93")),
                                   juce::dontSendNotification);
    juce_theme::style_label(recording_saved_label_, juce_theme::colour::success(), 16.0F,
                            true);
    recording_saved_label_.setJustificationType(juce::Justification::centred);
    recording_saved_label_.setVisible(false);

    leave_button_.setButtonText("Leave");
    mic_mute_button_.setButtonText("Mute mic");
    mic_mute_button_.setClickingTogglesState(true);
    monitor_toggle_.setButtonText("Monitor");
    monitor_toggle_.setClickingTogglesState(true);
    configure_linear_slider(jitter_ms_slider_, 0.0, 200.0, 1.0, " ms");
    configure_linear_slider(queue_limit_slider_, 1.0, MAX_OPUS_QUEUE_LIMIT_PACKETS, 1.0,
                            " pkt");
    configure_linear_slider(age_limit_slider_, 1.0, MAX_JITTER_PACKET_AGE_MS, 1.0, " ms");
    auto_jitter_toggle_.setButtonText("Auto jitter");

    bpm_editor_.setInputRestrictions(6, "0123456789.");
    juce_theme::style_editor(bpm_editor_);
    metronome_start_stop_button_.setButtonText("Start");
    metronome_tap_button_.setButtonText("Tap");
    record_button_.setButtonText("Record");

    wav_path_editor_.setTextToShowWhenEmpty("WAV path",
                                            juce_theme::colour::text_faint());
    juce_theme::style_editor(wav_path_editor_);
    wav_path_editor_.setReadOnly(true);
    wav_load_button_.setButtonText("Load");
    wav_play_button_.setButtonText("Play");
    configure_linear_slider(wav_position_slider_, 0.0, 1.0, 1.0);
    wav_position_slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    configure_linear_slider(wav_gain_slider_, 0.0, 2.0, 0.01, "x");
    wav_gain_slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    wav_mute_toggle_.setButtonText("Local mute");
    room_settings_button_.setButtonText("Settings");
    room_participants_button_.setButtonText("Participants");
    room_close_button_.setButtonText("Close room");
    room_copy_invite_button_.setButtonText("Copy invite");

    add_all(*this, {&status_bar_, &leave_button_, &participants_component_,
                    &local_audio_label_, &network_viewport_,
                    &metronome_label_, &recording_label_, &wav_label_,
                    &room_admin_label_, &room_admin_status_label_,
                    &wav_position_label_, &wav_gain_label_,
                    &room_settings_button_, &room_participants_button_,
                    &room_close_button_, &room_copy_invite_button_,
                    &mic_mute_button_, &monitor_toggle_,
                    &bpm_editor_,
                    &metronome_start_stop_button_, &metronome_tap_button_,
                    &record_button_, &recording_saved_label_, &wav_path_editor_,
                    &wav_load_button_, &wav_play_button_, &wav_position_slider_,
                    &wav_gain_slider_, &wav_mute_toggle_});
    add_all(network_content_, {&network_label_, &packet_label_, &jitter_label_, &queue_label_,
                               &age_limit_label_, &opus_packet_combo_, &jitter_ms_slider_,
                               &queue_limit_slider_, &age_limit_slider_,
                               &redundancy_section_label_, &redundancy_label_,
                               &redundancy_combo_, &auto_jitter_toggle_, &diagnostics_label_});

    leave_button_.onClick = [this]() { leave_room(); };
    room_settings_button_.onClick = [this]() { request_room_settings_dialog(); };
    room_participants_button_.onClick = [this]() { request_participants_dialog(); };
    room_close_button_.onClick = [this]() { request_room_close(); };
    room_copy_invite_button_.onClick = [this]() { request_copy_invite(); };
    mic_mute_button_.onClick = [this]() {
        if (!updating_from_client_) {
            client_.set_mic_muted(mic_mute_button_.getToggleState());
        }
    };
    monitor_toggle_.onClick = [this]() {
        if (!updating_from_client_) {
            client_.set_self_monitor_enabled(monitor_toggle_.getToggleState());
        }
    };
    jitter_ms_slider_.onValueChange = [this]() {
        if (!updating_from_client_) {
            client_.set_opus_jitter_buffer_ms(
                std::max(0, static_cast<int>(std::lround(jitter_ms_slider_.getValue()))));
            if (client_.get_opus_queue_limit_packets() <
                client_.get_opus_jitter_buffer_packets()) {
                client_.set_opus_queue_limit_packets(
                    client_.get_opus_jitter_buffer_packets());
            }
        }
    };
    queue_limit_slider_.onValueChange = [this]() {
        if (!updating_from_client_) {
            client_.set_opus_queue_limit_packets(static_cast<size_t>(
                std::max(1, static_cast<int>(std::lround(queue_limit_slider_.getValue())))));
        }
    };
    age_limit_slider_.onValueChange = [this]() {
        if (!updating_from_client_) {
            client_.set_jitter_packet_age_limit_ms(
                std::max(1, static_cast<int>(std::lround(age_limit_slider_.getValue()))));
        }
    };
    auto_jitter_toggle_.onClick = [this]() {
        if (!updating_from_client_) {
            client_.set_opus_auto_jitter_default(auto_jitter_toggle_.getToggleState());
        }
    };
    redundancy_combo_.onChange = [this]() {
        if (updating_from_client_) {
            return;
        }
        const int selected = redundancy_combo_.getSelectedId();
        if (selected == 1) {
            client_.set_opus_redundancy_depth(OPUS_REDUNDANCY_DEPTH_AUTO);
        } else {
            client_.set_opus_redundancy_depth(selected - 2);
        }
    };
    bpm_editor_.onReturnKey = [this]() { commit_metronome_bpm(); };
    bpm_editor_.onFocusLost = [this]() { commit_metronome_bpm(); };
    metronome_start_stop_button_.onClick = [this]() {
        if (client_.get_metronome_state().running) {
            client_.stop_metronome();
        } else {
            commit_metronome_bpm();
            client_.start_metronome();
        }
    };
    metronome_tap_button_.onClick = [this]() { client_.tap_metronome_tempo(); };
    record_button_.onClick = [this]() { handle_record_button(); };
    wav_load_button_.onClick = [this]() { load_wav_file(); };
    wav_play_button_.onClick = [this]() {
        if (client_.get_wav_state().is_playing) {
            client_.wav_pause();
        } else {
            client_.wav_play();
        }
    };
    wav_position_slider_.onValueChange = [this]() {
        if (!updating_from_client_) {
            if (client_.get_wav_state().is_playing) {
                client_.wav_pause();
            }
            client_.wav_seek(static_cast<int64_t>(std::llround(wav_position_slider_.getValue())));
        }
    };
    wav_gain_slider_.onValueChange = [this]() {
        if (!updating_from_client_) {
            client_.set_wav_gain(static_cast<float>(wav_gain_slider_.getValue()));
        }
    };
    wav_mute_toggle_.onClick = [this]() {
        if (!updating_from_client_) {
            client_.set_wav_muted_local(wav_mute_toggle_.getToggleState());
        }
    };
}

void JuceMixerComponent::configure_device_controls() {
    juce_theme::style_label(device_status_label_, juce_theme::colour::text_dim(), 12.5F);
    apply_audio_button_.setButtonText("Apply");
    start_stop_audio_button_.setButtonText("Start");
    reset_audio_button_.setButtonText("Reset");
    refresh_devices_button_.setButtonText("Refresh");

    add_all(*this, {&api_combo_, &input_combo_, &input_channel_combo_, &output_combo_,
                    &buffer_combo_, &apply_audio_button_,
                    &start_stop_audio_button_, &reset_audio_button_,
                    &refresh_devices_button_, &device_status_label_});

    set_device_status("Audio devices will load after the window opens...");
    set_device_controls_enabled(false);

    api_combo_.onChange = [this]() {
        if (updating_from_client_) {
            return;
        }
        const int old_api_index = selected_api_index_;
        const int selected_id = api_combo_.getSelectedId();
        selected_api_index_ = selected_id <= 1 ? -1 : selected_id - 2;
        apply_selected_api_to_pending_devices(old_api_index);
        populate_device_combos();
    };
    input_combo_.onChange = [this]() {
        if (!updating_from_client_) {
            pending_input_ = selected_input_device();
            pending_input_channel_ =
                std::clamp(pending_input_channel_, 0,
                           max_input_channels_for(pending_input_) - 1);
            populate_input_channel_combo();
        }
    };
    input_channel_combo_.onChange = [this]() {
        if (!updating_from_client_) {
            pending_input_channel_ = std::max(0, input_channel_combo_.getSelectedId() - 1);
        }
    };
    output_combo_.onChange = [this]() {
        if (!updating_from_client_) {
            pending_output_ = selected_output_device();
        }
    };
    buffer_combo_.onChange = [this]() {
        if (!updating_from_client_) {
            pending_buffer_frames_ = buffer_combo_.getSelectedId();
        }
    };
    opus_packet_combo_.onChange = [this]() {
        if (!updating_from_client_) {
            pending_opus_frames_per_packet_ = opus_packet_combo_.getSelectedId();
            auto_match_buffer_to_packet_frames(pending_opus_frames_per_packet_);
        }
    };

    apply_audio_button_.onClick = [this]() { apply_audio_settings(); };
    start_stop_audio_button_.onClick = [this]() { start_or_stop_audio(); };
    reset_audio_button_.onClick = [this]() { reset_audio_path(); };
    refresh_devices_button_.onClick = [this]() { refresh_audio_device_controls(); };

    pending_input_ = client_.get_selected_input_device();
    pending_output_ = client_.get_selected_output_device();
    pending_input_channel_ = client_.get_input_channel_index();
    pending_buffer_frames_ = client_.get_audio_config().frames_per_buffer;
    pending_opus_frames_per_packet_ = client_.get_opus_network_frame_count();
    selected_api_index_ = api_index_for_name(client_.get_audio_api_filter());

    populate_buffer_combo();
    populate_opus_packet_combo();
    populate_redundancy_combo();
}

void JuceMixerComponent::paint(juce::Graphics& g) {
    g.fillAll(juce_theme::colour::background());

    auto area = getLocalBounds().reduced(PAD);
    area.removeFromTop(STATUS_HEIGHT);
    area.removeFromTop(GAP);
    auto bottom = area.removeFromBottom(BOTTOM_HEIGHT);
    area.removeFromBottom(GAP);

    juce_theme::paint_panel(g, area);
    juce_theme::paint_panel(g, bottom);

    const auto dock = make_dock_layout(bottom);
    g.setColour(juce_theme::colour::border_soft().withAlpha(0.7F));
    const auto draw_separator = [&](juce::Rectangle<int> section) {
        const int x = section.getX() - (GAP / 2);
        g.drawVerticalLine(x, static_cast<float>(bottom.getY() + 12),
                           static_cast<float>(bottom.getBottom() - 12));
    };
    draw_separator(dock.network);
    draw_separator(dock.metronome);
    draw_separator(dock.wav);
}

void JuceMixerComponent::resized() {
    auto area = getLocalBounds().reduced(PAD);
    auto top = area.removeFromTop(STATUS_HEIGHT);
    status_bar_.setBounds(top);
    leave_button_.setBounds(top.removeFromRight(92).reduced(14, 18));
    area.removeFromTop(GAP);

    auto bottom_panel_area = area.removeFromBottom(BOTTOM_HEIGHT);
    area.removeFromBottom(GAP);

    const auto dock = make_dock_layout(bottom_panel_area);
    constexpr int control_gap = 6;
    auto set_title = [](juce::Label& label, juce::Rectangle<int>& panel) {
        label.setBounds(panel.removeFromTop(22));
        panel.removeFromTop(2);
    };
    auto set_labeled_row = [](juce::Rectangle<int> row, juce::Label& label,
                              juce::Component& control, int label_width = 56) {
        label.setBounds(row.removeFromLeft(label_width));
        control.setBounds(row.reduced(2));
    };

    auto local = dock.local_audio.reduced(12, 10);
    set_title(local_audio_label_, local);
    auto api_row = local.removeFromTop(ROW);
    api_combo_.setBounds(api_row.removeFromLeft(std::min(104, api_row.getWidth() / 3)).reduced(2));
    api_row.removeFromLeft(control_gap);
    input_combo_.setBounds(api_row.reduced(2));
    local.removeFromTop(4);
    auto io_row = local.removeFromTop(ROW);
    input_channel_combo_.setBounds(io_row.removeFromLeft(58).reduced(2));
    io_row.removeFromLeft(control_gap);
    output_combo_.setBounds(io_row.removeFromLeft(std::max(110, io_row.getWidth() - 88)).reduced(2));
    io_row.removeFromLeft(control_gap);
    buffer_combo_.setBounds(io_row.reduced(2));
    local.removeFromTop(4);
    auto monitor_row = local.removeFromTop(ROW);
    mic_mute_button_.setBounds(monitor_row.removeFromLeft(86).reduced(2));
    monitor_row.removeFromLeft(control_gap);
    monitor_toggle_.setBounds(monitor_row.removeFromLeft(86).reduced(2));
    local.removeFromTop(4);
    auto action_row = local.removeFromTop(ROW);
    apply_audio_button_.setBounds(action_row.removeFromLeft(58).reduced(2));
    action_row.removeFromLeft(control_gap);
    start_stop_audio_button_.setBounds(action_row.removeFromLeft(58).reduced(2));
    action_row.removeFromLeft(control_gap);
    reset_audio_button_.setBounds(action_row.removeFromLeft(58).reduced(2));
    action_row.removeFromLeft(control_gap);
    refresh_devices_button_.setBounds(action_row.removeFromLeft(70).reduced(2));
    device_status_label_.setBounds(action_row.reduced(2));

    network_viewport_.setBounds(dock.network.reduced(12, 10));
    layout_network_content();

    auto tools = dock.metronome.reduced(12, 10);
    set_title(metronome_label_, tools);
    auto bpm_row = tools.removeFromTop(ROW);
    bpm_editor_.setBounds(bpm_row.removeFromLeft(70).reduced(2));
    bpm_row.removeFromLeft(control_gap);
    metronome_start_stop_button_.setBounds(bpm_row.removeFromLeft(58).reduced(2));
    tools.removeFromTop(6);
    metronome_tap_button_.setBounds(tools.removeFromTop(ROW).reduced(2));
    tools.removeFromTop(12);
    set_title(recording_label_, tools);
    auto record_row = tools.removeFromTop(ROW);
    record_button_.setBounds(record_row.removeFromLeft(86).reduced(2));
    record_row.removeFromLeft(control_gap);
    recording_saved_label_.setBounds(record_row.removeFromLeft(24).reduced(2));

    auto wav = dock.wav.reduced(12, 10);
    set_title(wav_label_, wav);
    auto wav_file = wav.removeFromTop(ROW);
    wav_load_button_.setBounds(wav_file.removeFromRight(62).reduced(2));
    wav_file.removeFromRight(control_gap);
    wav_path_editor_.setBounds(wav_file.reduced(2));
    wav.removeFromTop(4);
    auto transport = wav.removeFromTop(ROW);
    wav_play_button_.setBounds(transport.removeFromLeft(58).reduced(2));
    transport.removeFromLeft(control_gap);
    wav_mute_toggle_.setBounds(transport.removeFromRight(96).reduced(2));
    transport.removeFromRight(control_gap);
    wav_position_label_.setBounds(transport.reduced(2));
    wav_position_label_.setJustificationType(juce::Justification::centred);
    wav.removeFromTop(4);
    wav_position_slider_.setBounds(wav.removeFromTop(ROW).reduced(2, 6));
    wav.removeFromTop(4);
    auto wav_gain = wav.removeFromTop(ROW);
    wav_gain_label_.setBounds(wav_gain.removeFromLeft(82).reduced(2));
    wav_gain_slider_.setBounds(wav_gain.reduced(2, 6));

    const bool admin_visible = has_room_admin();
    for (auto* component: {static_cast<juce::Component*>(&room_admin_label_),
                           static_cast<juce::Component*>(&room_admin_status_label_),
                           static_cast<juce::Component*>(&room_settings_button_),
                           static_cast<juce::Component*>(&room_participants_button_),
                           static_cast<juce::Component*>(&room_close_button_),
                           static_cast<juce::Component*>(&room_copy_invite_button_)}) {
        component->setVisible(admin_visible);
    }

    auto participants_area = area.reduced(12, 10);
    if (admin_visible) {
        auto admin_row = participants_area.removeFromTop(34);
        participants_area.removeFromTop(10);

        room_admin_label_.setBounds(admin_row.removeFromLeft(96));
        admin_row.removeFromLeft(control_gap);
        room_copy_invite_button_.setBounds(admin_row.removeFromLeft(104).reduced(2));
        admin_row.removeFromLeft(control_gap);
        room_settings_button_.setBounds(admin_row.removeFromLeft(92).reduced(2));
        admin_row.removeFromLeft(control_gap);
        room_participants_button_.setBounds(admin_row.removeFromLeft(128).reduced(2));
        admin_row.removeFromLeft(control_gap);
        room_close_button_.setBounds(admin_row.removeFromLeft(92).reduced(2));
        admin_row.removeFromLeft(control_gap);
        room_admin_status_label_.setBounds(admin_row.reduced(2));
    }

    participants_component_.setBounds(participants_area);
}

void JuceMixerComponent::layout_network_content() {
    const auto viewport = network_viewport_.getBounds();
    if (viewport.isEmpty()) {
        return;
    }

    constexpr int control_gap = 6;
    auto set_title = [](juce::Label& label, juce::Rectangle<int>& panel) {
        label.setBounds(panel.removeFromTop(22));
        panel.removeFromTop(2);
    };
    auto set_labeled_row = [](juce::Rectangle<int> row, juce::Label& label,
                              juce::Component& control, int label_width = 56) {
        label.setBounds(row.removeFromLeft(label_width));
        control.setBounds(row.reduced(2));
    };

    auto network = juce::Rectangle<int>(0, 0, std::max(1, viewport.getWidth() - 20), 1000);
    set_title(network_label_, network);
    set_labeled_row(network.removeFromTop(ROW), packet_label_, opus_packet_combo_, 52);
    network.removeFromTop(4);
    set_labeled_row(network.removeFromTop(ROW), jitter_label_, jitter_ms_slider_, 52);
    network.removeFromTop(4);
    set_labeled_row(network.removeFromTop(ROW), queue_label_, queue_limit_slider_, 52);
    network.removeFromTop(4);
    set_labeled_row(network.removeFromTop(ROW), age_limit_label_, age_limit_slider_, 52);
    network.removeFromTop(10);
    set_title(redundancy_section_label_, network);
    set_labeled_row(network.removeFromTop(ROW), redundancy_label_, redundancy_combo_, 52);
    network.removeFromTop(4);
    auto_jitter_toggle_.setBounds(network.removeFromTop(ROW).reduced(2));
    network.removeFromTop(10);

    const int diagnostics_height = diagnostics_label_height(diagnostics_label_.getText());
    diagnostics_label_.setBounds(network.removeFromTop(diagnostics_height).reduced(2, 4));

    const int content_height = std::max(viewport.getHeight(), diagnostics_label_.getBottom() + control_gap);
    network_content_.setSize(std::max(1, viewport.getWidth() - 20), content_height);
}

void JuceMixerComponent::timerCallback() {
    poll_connection_start();
    poll_audio_device_refresh();
    poll_room_admin_job();
    if (client_.consume_room_removed_by_server()) {
        if (leave_callback_) {
            auto callback = leave_callback_;
            juce::MessageManager::callAsync([callback = std::move(callback)]() mutable {
                callback();
            });
        }
        return;
    }
    if (!startup_connection_started_ && startup_options_.auto_connect) {
        --connection_delay_ticks_;
        if (connection_delay_ticks_ <= 0) {
            startup_connection_started_ = true;
            request_connection_start();
        }
    }
    if (!startup_device_refresh_started_) {
        --device_load_delay_ticks_;
        if (device_load_delay_ticks_ <= 0) {
            startup_device_refresh_started_ = true;
            request_audio_device_refresh(startup_options_.auto_start_audio);
        }
    }
    refresh_live_state();
}

void JuceMixerComponent::refresh_live_state() {
    updating_from_client_ = true;

    bool device_job_active = false;
    {
        std::lock_guard<std::mutex> lock(device_job_mutex_);
        device_job_active = device_job_running_;
    }
    const auto latency =
        device_job_active ? AudioStream::LatencyInfo{} : client_.get_latency_info();
    const auto callback_timing = client_.get_callback_timing_info();
    const auto path = client_.get_path_diagnostics();
    const auto metronome = client_.get_metronome_state();
    const auto recording = client_.get_recording_state();
    const auto wav = client_.get_wav_state();
    const double now_ms = juce::Time::getMillisecondCounterHiRes();

    status_bar_.refresh(client_, startup_options_, connection_status_);

    mic_mute_button_.setToggleState(client_.get_mic_muted(), juce::dontSendNotification);
    monitor_toggle_.setToggleState(client_.get_self_monitor_enabled(),
                                   juce::dontSendNotification);
    jitter_ms_slider_.setValue(client_.get_opus_jitter_buffer_ms(),
                               juce::dontSendNotification);
    queue_limit_slider_.setValue(static_cast<double>(client_.get_opus_queue_limit_packets()),
                                 juce::dontSendNotification);
    age_limit_slider_.setValue(client_.get_jitter_packet_age_limit_ms(),
                               juce::dontSendNotification);
    auto_jitter_toggle_.setToggleState(client_.get_opus_auto_jitter_default(),
                                       juce::dontSendNotification);

    const int redundancy_depth = client_.get_opus_redundancy_depth_setting();
    redundancy_combo_.setText(redundancy_label(redundancy_depth,
                                               client_.get_effective_opus_redundancy_depth()),
                              juce::dontSendNotification);

    if (!bpm_editor_.hasKeyboardFocus(true)) {
        bpm_editor_.setText(juce::String(metronome.bpm, 1), false);
    }
    metronome_start_stop_button_.setButtonText(metronome.running ? "Stop" : "Start");
    record_button_.setButtonText(recording_save_pending_ ? "Saving"
                                                         : (recording.active ? "Stop" : "Record"));
    record_button_.setEnabled(!recording_save_pending_);
    recording_saved_label_.setVisible(recording_saved_until_ms_ > now_ms);
    wav_play_button_.setButtonText(wav.is_playing ? "Pause" : "Play");
    wav_play_button_.setEnabled(wav.is_loaded);
    wav_position_slider_.setEnabled(wav.is_loaded);
    wav_mute_toggle_.setEnabled(wav.is_loaded);
    wav_gain_slider_.setValue(wav.gain, juce::dontSendNotification);
    wav_gain_slider_.setEnabled(wav.is_loaded);
    wav_position_label_.setText(wav_time_label_text(wav), juce::dontSendNotification);
    wav_gain_label_.setText("Gain " + juce::String(wav.gain, 2) + "x",
                            juce::dontSendNotification);
    wav_mute_toggle_.setToggleState(wav.muted_local, juce::dontSendNotification);
    wav_position_slider_.setRange(0.0, static_cast<double>(std::max<int64_t>(wav.total_frames, 1)),
                                  1.0);
    wav_position_slider_.setValue(static_cast<double>(wav.position),
                                  juce::dontSendNotification);

    const double reported_device_ms = latency.input_latency_ms + latency.output_latency_ms;
    const double fallback_device_ms = latency.buffer_duration_ms > 0.0
                                          ? latency.buffer_duration_ms * 2.0
                                          : 0.0;
    const double device_path_ms = reported_device_ms > 0.0 ? reported_device_ms
                                                           : fallback_device_ms;
    const auto warning = device_latency_warning(latency, device_path_ms);
    const juce::String warning_line = warning.isEmpty() ? juce::String{} : "\n" + warning;
    diagnostics_label_.setText(
        "Opus " + juce::String(client_.get_opus_network_frame_count()) + " frames / " +
            juce::String(client_.get_opus_network_packet_ms(), 1) + " ms\n" +
            "E2E avg " + juce::String(path.e2e_latency_avg_max_ms, 1) + " ms\n" +
            "E2E peak " + juce::String(path.e2e_latency_peak_ms, 1) + " ms\n" +
            "Samples " + format_sample_count(path.e2e_latency_samples) + "\n" +
            "Device " + juce::String(device_path_ms, 1) + " ms\n" +
            "In " + juce::String(latency.input_latency_ms, 1) + " / out " +
            juce::String(latency.output_latency_ms, 1) + " ms\n" +
            "Buffer " + juce::String(latency.actual_buffer_frames) +
            buffer_match_suffix(latency) + warning_line + "\n" +
            "Callback avg " + juce::String(callback_timing.avg_ms, 2) + " ms\n" +
            "Callback max " + juce::String(callback_timing.max_ms, 2) + " ms\n" +
            "Late " + juce::String(static_cast<int>(callback_timing.over_deadline_count)),
        juce::dontSendNotification);
    layout_network_content();

    start_stop_audio_button_.setButtonText(client_.is_audio_stream_active() ? "Stop"
                                                                            : "Start");
    apply_audio_button_.setEnabled(device_controls_loaded_ && has_pending_audio_changes());

    updating_from_client_ = false;

    if (now_ms - last_participant_refresh_ms_ > 30.0) {
        last_participant_refresh_ms_ = now_ms;
        refresh_room_admin_controls(client_.get_participant_info());
    }
}

void JuceMixerComponent::refresh_audio_device_controls() {
    request_audio_device_refresh(false);
}

void JuceMixerComponent::request_audio_device_refresh(bool auto_start_audio) {
    {
        std::lock_guard<std::mutex> lock(device_job_mutex_);
        if (device_job_running_) {
            set_device_status("Audio devices are already loading...");
            return;
        }
        device_job_running_ = true;
        device_job_finished_ = false;
        device_job_result_.reset();
    }

    if (device_job_thread_.joinable()) {
        device_job_thread_.join();
    }

    set_device_controls_enabled(false);
    set_device_status(auto_start_audio ? "Loading audio devices and starting audio..."
                                       : "Loading audio devices...");

    device_job_thread_ = std::thread([this, auto_start_audio]() {
        auto result = load_audio_devices(auto_start_audio);
        std::lock_guard<std::mutex> lock(device_job_mutex_);
        device_job_result_ = std::move(result);
        device_job_finished_ = true;
    });
}

JuceMixerComponent::AudioDeviceRefreshResult JuceMixerComponent::load_audio_devices(
    bool auto_start_audio) {
    AudioDeviceRefreshResult result;
    result.pending_buffer_frames = client_.get_audio_config().frames_per_buffer;
    result.pending_opus_frames_per_packet = client_.get_opus_network_frame_count();
    result.pending_input_channel = client_.get_input_channel_index();

    try {
        result.input_devices = AudioStream::get_input_device_stubs();
        result.output_devices = AudioStream::get_output_device_stubs();
        result.available_apis = AudioStream::get_apis();

        const auto& preferences = startup_options_.audio_preferences;
        const bool has_required_api =
            !startup_options_.required_audio_api.empty();
        std::string target_api = has_required_api ? startup_options_.required_audio_api
                                                  : client_.get_audio_api_filter();
        if (target_api.empty()) {
            target_api = preferences.audio_api;
        }
        if (target_api.empty()) {
            target_api = "All";
        }

        result.selected_api_index = ::api_index_for_name(result.available_apis, target_api);
        const std::string resolved_api =
            api_name_for_index(result.available_apis, result.selected_api_index);

        if (has_required_api) {
            const bool has_input =
                first_device_for_api(result.input_devices, target_api) !=
                AudioStream::NO_DEVICE;
            const bool has_output =
                first_device_for_api(result.output_devices, target_api) !=
                AudioStream::NO_DEVICE;
            client_.set_audio_api_filter(target_api);
            if (!has_input || !has_output) {
                result.status = juce::String("Required audio API '") + target_api +
                                "' does not have both input and output devices";
                return result;
            }
        }

        if (!has_required_api) {
            result.pending_input = device_id_for_api(
                result.input_devices, client_.get_selected_input_device(), target_api);
            result.pending_output = device_id_for_api(
                result.output_devices, client_.get_selected_output_device(), target_api);
        }

        if (preferences.loaded && !has_required_api) {
            if (result.pending_input == AudioStream::NO_DEVICE) {
                result.pending_input = find_preferred_audio_device(
                    result.input_devices, preferences.input_device, preferences.input_api,
                    preferences.audio_api);
            }
            if (result.pending_output == AudioStream::NO_DEVICE) {
                result.pending_output = find_preferred_audio_device(
                    result.output_devices, preferences.output_device, preferences.output_api,
                    preferences.audio_api);
            }
        }

        if (result.pending_input == AudioStream::NO_DEVICE) {
            result.pending_input = default_device_for_api(
                result.input_devices, result.available_apis, result.selected_api_index, true);
        }
        if (result.pending_output == AudioStream::NO_DEVICE) {
            result.pending_output = default_device_for_api(
                result.output_devices, result.available_apis, result.selected_api_index,
                false);
        }

        const auto startup_channel = startup_options_.startup_input_channel_index;
        const auto preferred_channel = preferences.input_channel_index;
        result.pending_input_channel =
            startup_channel.value_or(preferred_channel.value_or(result.pending_input_channel));

        client_.set_audio_api_filter(has_required_api ? target_api : resolved_api);
        bool input_applied = result.pending_input != AudioStream::NO_DEVICE &&
                             client_.set_input_device(result.pending_input);
        bool output_applied = result.pending_output != AudioStream::NO_DEVICE &&
                              client_.set_output_device(result.pending_output);
        if (input_applied) {
            client_.set_input_channel_index(result.pending_input_channel);
            result.pending_input_channel = client_.get_input_channel_index();
        }
        if (input_applied && output_applied) {
            client_.save_audio_device_preferences();
        }

        if (auto_start_audio && input_applied && output_applied) {
            result.auto_start_attempted = true;
            AudioStream::AudioConfig config = client_.get_audio_config();
            result.auto_start_succeeded =
                client_.start_audio_stream(result.pending_input, result.pending_output,
                                           config);
        }

        if (result.input_devices.empty() || result.output_devices.empty()) {
            result.status = "No duplex audio devices found";
        } else if (!input_applied || !output_applied) {
            result.status = "Audio devices loaded; choose input and output";
        } else if (result.auto_start_attempted && result.auto_start_succeeded) {
            result.status = "Audio devices ready; audio started";
        } else if (result.auto_start_attempted) {
            const auto& error = AudioStream::get_last_error();
            result.status = error.empty() ? "Audio devices ready; auto-start failed"
                                          : "Auto-start failed: " + juce::String(error);
        } else {
            result.status = "Audio devices ready";
        }
    } catch (const std::exception& e) {
        result.status = "Audio device load failed: " + juce::String(e.what());
    }

    return result;
}

void JuceMixerComponent::poll_audio_device_refresh() {
    std::optional<AudioDeviceRefreshResult> result;
    {
        std::lock_guard<std::mutex> lock(device_job_mutex_);
        if (!device_job_finished_) {
            return;
        }
        result = std::move(device_job_result_);
        device_job_result_.reset();
        device_job_finished_ = false;
        device_job_running_ = false;
    }

    if (device_job_thread_.joinable()) {
        device_job_thread_.join();
    }
    if (result.has_value()) {
        apply_audio_device_refresh_result(std::move(*result));
    }
}

void JuceMixerComponent::apply_audio_device_refresh_result(
    AudioDeviceRefreshResult result) {
    input_devices_ = std::move(result.input_devices);
    output_devices_ = std::move(result.output_devices);
    available_apis_ = std::move(result.available_apis);
    selected_api_index_ = result.selected_api_index;
    pending_input_ = result.pending_input;
    pending_output_ = result.pending_output;
    pending_input_channel_ = result.pending_input_channel;
    pending_buffer_frames_ = result.pending_buffer_frames;
    pending_opus_frames_per_packet_ = result.pending_opus_frames_per_packet;

    device_controls_loaded_ = true;
    set_device_controls_enabled(true);
    populate_api_combo();
    populate_device_combos();
    populate_buffer_combo();
    populate_opus_packet_combo();
    set_device_status(result.status);
}

void JuceMixerComponent::request_connection_start() {
    if (startup_options_.server_address.empty() || startup_options_.server_port == 0) {
        connection_status_ = "No server target configured";
        return;
    }

    {
        std::lock_guard<std::mutex> lock(connection_job_mutex_);
        if (connection_job_running_) {
            connection_status_ = "Connection already starting...";
            return;
        }
        connection_job_running_ = true;
        connection_job_finished_ = false;
        connection_job_result_.reset();
    }

    if (connection_job_thread_.joinable()) {
        connection_job_thread_.join();
    }

    connection_status_ = "Connecting...";
    connection_job_thread_ = std::thread([this]() {
        auto result = start_connection();
        std::lock_guard<std::mutex> lock(connection_job_mutex_);
        connection_job_result_ = std::move(result);
        connection_job_finished_ = true;
    });
}

JuceMixerComponent::ConnectionResult JuceMixerComponent::start_connection() {
    ConnectionResult result;
    try {
        client_.start_connection(startup_options_.server_address,
                                 startup_options_.server_port);
        result.started = true;
        result.status = "Join sent";
    } catch (const std::exception& e) {
        result.status = "Connection failed: " + juce::String(e.what());
    }
    return result;
}

void JuceMixerComponent::poll_connection_start() {
    std::optional<ConnectionResult> result;
    {
        std::lock_guard<std::mutex> lock(connection_job_mutex_);
        if (!connection_job_finished_) {
            return;
        }
        result = std::move(connection_job_result_);
        connection_job_result_.reset();
        connection_job_finished_ = false;
        connection_job_running_ = false;
    }

    if (connection_job_thread_.joinable()) {
        connection_job_thread_.join();
    }
    if (result.has_value()) {
        connection_status_ = result->status;
    }
}

void JuceMixerComponent::set_device_controls_enabled(bool enabled) {
    api_combo_.setEnabled(enabled);
    input_combo_.setEnabled(enabled);
    input_channel_combo_.setEnabled(enabled);
    output_combo_.setEnabled(enabled);
    buffer_combo_.setEnabled(enabled);
    opus_packet_combo_.setEnabled(enabled);
    apply_audio_button_.setEnabled(enabled && has_pending_audio_changes());
    start_stop_audio_button_.setEnabled(enabled);
    reset_audio_button_.setEnabled(enabled);
    refresh_devices_button_.setEnabled(enabled);
}

void JuceMixerComponent::populate_api_combo() {
    updating_from_client_ = true;
    api_combo_.clear(juce::dontSendNotification);
    api_combo_.addItem("All APIs", 1);
    for (const auto& api: available_apis_) {
        api_combo_.addItem(api.name, api.index + 2);
    }
    api_combo_.setSelectedId(selected_api_index_ < 0 ? 1 : selected_api_index_ + 2,
                             juce::dontSendNotification);
    updating_from_client_ = false;
}

void JuceMixerComponent::populate_device_combos() {
    updating_from_client_ = true;

    const auto api_name = selected_api_name();
    input_combo_.clear(juce::dontSendNotification);
    int combo_id = 1;
    int selected_combo_id = 0;
    for (const auto& device: input_devices_) {
        if (api_name != "All" && device.api_name != api_name.toStdString()) {
            continue;
        }
        input_combo_.addItem(device.name + " (" + device.api_name + ")", combo_id);
        if (device.index == pending_input_) {
            selected_combo_id = combo_id;
        }
        ++combo_id;
    }
    input_combo_.setSelectedId(selected_combo_id, juce::dontSendNotification);

    output_combo_.clear(juce::dontSendNotification);
    combo_id = 1;
    selected_combo_id = 0;
    for (const auto& device: output_devices_) {
        if (api_name != "All" && device.api_name != api_name.toStdString()) {
            continue;
        }
        output_combo_.addItem(device.name + " (" + device.api_name + ")", combo_id);
        if (device.index == pending_output_) {
            selected_combo_id = combo_id;
        }
        ++combo_id;
    }
    output_combo_.setSelectedId(selected_combo_id, juce::dontSendNotification);

    updating_from_client_ = false;
    populate_input_channel_combo();
}

void JuceMixerComponent::populate_input_channel_combo() {
    updating_from_client_ = true;
    input_channel_combo_.clear(juce::dontSendNotification);
    const int channel_count = max_input_channels_for(pending_input_);
    pending_input_channel_ = std::clamp(pending_input_channel_, 0, channel_count - 1);
    for (int channel = 0; channel < channel_count; ++channel) {
        input_channel_combo_.addItem(juce::String(channel + 1), channel + 1);
    }
    input_channel_combo_.setSelectedId(pending_input_channel_ + 1,
                                       juce::dontSendNotification);
    updating_from_client_ = false;
}

void JuceMixerComponent::populate_buffer_combo() {
    updating_from_client_ = true;
    buffer_combo_.clear(juce::dontSendNotification);
    for (const int frames: BUFFER_FRAME_OPTIONS) {
        buffer_combo_.addItem(juce::String(frames), frames);
    }
    buffer_combo_.setSelectedId(pending_buffer_frames_, juce::dontSendNotification);
    updating_from_client_ = false;
}

void JuceMixerComponent::populate_opus_packet_combo() {
    updating_from_client_ = true;
    opus_packet_combo_.clear(juce::dontSendNotification);
    const int frame_options[] = {opus_network_clock::LOW_LATENCY_FRAME_COUNT,
                                 opus_network_clock::FAST_FRAME_COUNT,
                                 opus_network_clock::BALANCED_FRAME_COUNT,
                                 opus_network_clock::STABLE_FRAME_COUNT};
    for (const int frames: frame_options) {
        opus_packet_combo_.addItem(juce::String(frames) + " " + opus_packet_label(frames),
                                   frames);
    }
    opus_packet_combo_.setSelectedId(pending_opus_frames_per_packet_,
                                     juce::dontSendNotification);
    updating_from_client_ = false;
}

void JuceMixerComponent::auto_match_buffer_to_packet_frames(int packet_frames) {
    if (!is_supported_buffer_frame_option(packet_frames)) {
        return;
    }

    pending_buffer_frames_ = packet_frames;
    buffer_combo_.setSelectedId(pending_buffer_frames_, juce::dontSendNotification);
    set_device_status("Matched buffer to " + juce::String(packet_frames) +
                      " frame packets");
}

void JuceMixerComponent::populate_redundancy_combo() {
    updating_from_client_ = true;
    redundancy_combo_.clear(juce::dontSendNotification);
    redundancy_combo_.addItem("Auto", 1);
    redundancy_combo_.addItem("Off", 2);
    for (int depth = 1; depth <= MAX_OPUS_REDUNDANCY_DEPTH_PACKETS; ++depth) {
        redundancy_combo_.addItem(juce::String(depth) + " previous", depth + 2);
    }
    const int current_depth = client_.get_opus_redundancy_depth_setting();
    redundancy_combo_.setSelectedId(current_depth == OPUS_REDUNDANCY_DEPTH_AUTO
                                        ? 1
                                        : current_depth + 2,
                                    juce::dontSendNotification);
    updating_from_client_ = false;
}

void JuceMixerComponent::apply_audio_settings() {
    const bool restart_needed = pending_stream_restart_needed();
    client_.set_audio_api_filter(selected_api_name().toStdString());
    const bool input_ok = client_.set_input_device(pending_input_);
    if (input_ok) {
        client_.set_input_channel_index(pending_input_channel_);
    }
    const bool output_ok = client_.set_output_device(pending_output_);
    client_.set_requested_frames_per_buffer(pending_buffer_frames_);
    client_.set_opus_network_frame_count(pending_opus_frames_per_packet_);
    if (input_ok && output_ok) {
        client_.save_audio_device_preferences();
    } else {
        set_device_status("Choose valid input and output devices");
        return;
    }

    if (client_.is_audio_stream_active() && restart_needed) {
        client_.swap_audio_devices(pending_input_, pending_output_);
    }
}

void JuceMixerComponent::start_or_stop_audio() {
    if (client_.is_audio_stream_active()) {
        client_.stop_audio_stream();
        return;
    }

    if (pending_input_ == AudioStream::NO_DEVICE || pending_output_ == AudioStream::NO_DEVICE) {
        set_device_status("Select input and output devices before starting audio");
        return;
    }

    apply_audio_settings();
    AudioStream::AudioConfig config = client_.get_audio_config();
    if (!client_.start_audio_stream(pending_input_, pending_output_, config)) {
        set_device_status(AudioStream::get_last_error());
    }
}

void JuceMixerComponent::reset_audio_path() {
    client_.reset_audio_path();
    refresh_audio_device_controls();
}

void JuceMixerComponent::commit_metronome_bpm() {
    const float bpm = std::max(1.0F, bpm_editor_.getText().getFloatValue());
    client_.commit_metronome_bpm(bpm);
}

void JuceMixerComponent::handle_record_button() {
    if (recording_save_pending_) {
        return;
    }

    const auto recording = client_.get_recording_state();
    if (recording.active) {
        const juce::File recording_folder(recording.folder);
        client_.stop_recording();
        if (recording_folder.isDirectory()) {
            request_recording_destination(recording_folder);
        } else {
            set_device_status("Recording stopped");
        }
        return;
    }

    recording_saved_until_ms_ = 0.0;
    recording_saved_label_.setVisible(false);
    if (client_.start_recording()) {
        set_device_status("Recording started");
    } else {
        set_device_status("Recording failed");
    }
}

void JuceMixerComponent::request_recording_destination(const juce::File& recording_folder) {
    if (!recording_folder.isDirectory()) {
        set_device_status("Recording folder is missing");
        return;
    }

    recording_save_pending_ = true;
    recording_saved_until_ms_ = 0.0;
    recording_saved_label_.setVisible(false);
    record_button_.setButtonText("Saving");
    record_button_.setEnabled(false);
    set_device_status("Choose where to save recording...");

    const auto start_location = juce::File::getSpecialLocation(juce::File::userMusicDirectory);
    recording_folder_chooser_ =
        std::make_unique<juce::FileChooser>("Save recording to folder", start_location);
    recording_folder_chooser_->launchAsync(
        juce::FileBrowserComponent::openMode |
            juce::FileBrowserComponent::canSelectDirectories,
        [safe_this = juce::Component::SafePointer<JuceMixerComponent>(this),
         recording_folder](const juce::FileChooser& chooser) {
            if (auto* self = safe_this.getComponent()) {
                const auto destination = chooser.getResult();
                if (destination.isDirectory()) {
                    self->save_recording_to_destination(recording_folder, destination);
                } else {
                    self->recording_save_pending_ = false;
                    self->record_button_.setEnabled(true);
                    self->set_device_status("Recording kept: " +
                                            recording_folder.getFullPathName());
                }
            }
        });
}

void JuceMixerComponent::save_recording_to_destination(
    const juce::File& recording_folder,
    const juce::File& destination_root) {
    juce::File saved_folder;
    juce::String error;
    const bool saved =
        save_recording_folder(recording_folder, destination_root, saved_folder, error);

    recording_save_pending_ = false;
    record_button_.setEnabled(true);
    if (saved) {
        set_device_status("Saved recording: " + saved_folder.getFullPathName());
        show_recording_saved_tick();
    } else {
        set_device_status(error.isNotEmpty() ? error : "Could not save recording");
    }
}

void JuceMixerComponent::show_recording_saved_tick() {
    recording_saved_until_ms_ = juce::Time::getMillisecondCounterHiRes() + 2000.0;
    recording_saved_label_.setVisible(true);
}

void JuceMixerComponent::load_wav_file() {
    const auto start_location =
        last_wav_file_.existsAsFile()
            ? last_wav_file_.getParentDirectory()
            : juce::File::getSpecialLocation(juce::File::userMusicDirectory);
    const juce::Component::SafePointer<JuceMixerComponent> safe_this(this);
    wav_file_chooser_ = std::make_unique<juce::FileChooser>(
        "Load WAV file", start_location, "*.wav;*.wave");
    wav_file_chooser_->launchAsync(
        juce::FileBrowserComponent::openMode |
            juce::FileBrowserComponent::canSelectFiles,
        [safe_this](const juce::FileChooser& chooser) {
            if (auto* self = safe_this.getComponent()) {
                const auto file = chooser.getResult();
                if (file.existsAsFile()) {
                    self->load_wav_path(file);
                }
            }
        });
}

void JuceMixerComponent::load_wav_path(const juce::File& file) {
    last_wav_file_ = file;
    if (client_.load_wav_file(file.getFullPathName().toStdString())) {
        wav_path_editor_.setText(file.getFileName(), false);
        wav_position_slider_.setValue(0.0, juce::dontSendNotification);
        set_device_status("Loaded WAV: " + file.getFileName());
    } else {
        set_device_status("Could not load WAV: " + file.getFileName());
    }
}

void JuceMixerComponent::refresh_room_admin_controls(
    const std::vector<ParticipantInfo>& participants) {
    const bool admin = has_room_admin();
    bool running = false;
    {
        std::lock_guard<std::mutex> lock(room_admin_job_mutex_);
        running = room_admin_job_running_;
    }

    room_settings_button_.setEnabled(admin && !running);
    room_close_button_.setEnabled(admin && !running);
    const auto waiting = client_.get_waiting_participant_info();
    room_participants_button_.setEnabled(admin && !running);
    room_participants_button_.setButtonText(
        waiting.empty()
            ? "Participants"
            : "Participants (" + juce::String(static_cast<int>(waiting.size())) + ")");
    room_participants_button_.setToggleState(!waiting.empty(), juce::dontSendNotification);
    room_copy_invite_button_.setEnabled(admin);
}

void JuceMixerComponent::request_room_settings_dialog() {
    if (!has_room_admin()) {
        set_room_admin_status("Only the room creator can change this room");
        return;
    }

    auto* content = new RoomSettingsDialog(
        startup_options_.access_mode,
        [safe_this = juce::Component::SafePointer<JuceMixerComponent>(this)](
            uint8_t access_mode, std::string password) {
            if (auto* self = safe_this.getComponent()) {
                self->request_room_access_change(access_mode, std::move(password));
            }
        });
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Room Settings";
    options.content.setOwned(content);
    options.componentToCentreAround = this;
    options.dialogBackgroundColour = juce_theme::colour::panel_bottom();
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = false;
    options.launchAsync();
}

void JuceMixerComponent::request_room_access_change(uint8_t access_mode,
                                                    std::string password) {
    if (!has_room_admin()) {
        set_room_admin_status("Only the room creator can change this room");
        return;
    }
    std::string hash;
    if (access_mode == ROOM_ACCESS_PASSWORD) {
        if (password.empty()) {
            set_room_admin_status("Password is required for password access");
            return;
        }
        hash = password_hash(password);
    }
    set_room_admin_status("Saving room settings...");
    start_room_admin_job(ROOM_ADMIN_CHANGE_ACCESS, 0, std::move(hash), false,
                         access_mode);
}

void JuceMixerComponent::request_participants_dialog() {
    if (!has_room_admin()) {
        set_room_admin_status("Only the room creator can manage participants");
        return;
    }

    const auto safe_this = juce::Component::SafePointer<JuceMixerComponent>(this);
    auto* content = new ParticipantsDialog(
        [safe_this]() {
            if (auto* self = safe_this.getComponent()) {
                return self->client_.get_waiting_participant_info();
            }
            return std::vector<ParticipantInfo>{};
        },
        [safe_this]() {
            if (auto* self = safe_this.getComponent()) {
                return self->client_.get_participant_info();
            }
            return std::vector<ParticipantInfo>{};
        },
        [safe_this](uint32_t participant_id) {
            if (auto* self = safe_this.getComponent()) {
                self->request_room_approve(participant_id);
            }
        },
        [safe_this](uint32_t participant_id) {
            if (auto* self = safe_this.getComponent()) {
                self->request_room_decline(participant_id);
            }
        },
        [safe_this](uint32_t participant_id) {
            if (auto* self = safe_this.getComponent()) {
                self->request_room_kick(participant_id);
            }
        });
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Participants";
    options.content.setOwned(content);
    options.componentToCentreAround = this;
    options.dialogBackgroundColour = juce_theme::colour::panel_bottom();
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = false;
    options.launchAsync();
}

void JuceMixerComponent::request_room_approve(uint32_t participant_id) {
    if (!has_room_admin()) {
        set_room_admin_status("Only the room creator can approve");
        return;
    }
    if (client_.approve_waiting_participant(participant_id)) {
        set_room_admin_status("Participant approved");
    } else {
        set_room_admin_status("Approve failed");
    }
}

void JuceMixerComponent::request_room_decline(uint32_t participant_id) {
    request_room_kick(participant_id);
}

void JuceMixerComponent::request_room_kick(uint32_t participant_id) {
    if (!has_room_admin()) {
        set_room_admin_status("Only the room creator can kick");
        return;
    }
    if (participant_id == 0) {
        set_room_admin_status("Choose a participant");
        return;
    }

    set_room_admin_status("Removing participant...");
    start_room_admin_job(ROOM_ADMIN_KICK_PARTICIPANT, participant_id, {}, false);
}

void JuceMixerComponent::request_room_close() {
    if (!has_room_admin()) {
        set_room_admin_status("Only the room creator can close this room");
        return;
    }
    set_room_admin_status("Closing room...");
    start_room_admin_job(ROOM_ADMIN_CLOSE_ROOM, 0, {}, true);
}

void JuceMixerComponent::request_copy_invite() {
    if (!has_room_admin()) {
        set_room_admin_status("Only the room creator can copy invites");
        return;
    }

    juce::SystemClipboard::copyTextToClipboard(invite_text());
    set_room_admin_status("Invite copied");
}

void JuceMixerComponent::start_room_admin_job(uint8_t command,
                                              uint32_t target_participant_id,
                                              std::string hash,
                                              bool closes_room,
                                              uint8_t access_mode) {
    {
        std::lock_guard<std::mutex> lock(room_admin_job_mutex_);
        if (room_admin_job_running_) {
            set_room_admin_status("Room admin request is already running");
            return;
        }
        room_admin_job_running_ = true;
        room_admin_job_finished_ = false;
        room_admin_job_result_.reset();
    }

    if (room_admin_job_thread_.joinable()) {
        room_admin_job_thread_.join();
    }

    room_admin_job_thread_ =
        std::thread([this, command, target_participant_id,
                     hash = std::move(hash), closes_room, access_mode]() {
            auto result =
                run_room_admin_command(command, target_participant_id, hash,
                                       closes_room, access_mode);
            std::lock_guard<std::mutex> lock(room_admin_job_mutex_);
            room_admin_job_result_ = std::move(result);
            room_admin_job_finished_ = true;
        });
}

JuceMixerComponent::RoomAdminResult JuceMixerComponent::run_room_admin_command(
    uint8_t command, uint32_t target_participant_id,
    const std::string& hash, bool closes_room, uint8_t access_mode) {
    RoomAdminResult result;

    const auto connected_port = client_.get_server_port();
    const std::string address =
        client_.get_server_address().empty() ? startup_options_.server_address
                                             : client_.get_server_address();
    const uint16_t port =
        connected_port == 0 ? startup_options_.server_port : connected_port;
    const std::string room_id = client_.get_room_id();
    if (address.empty() || port == 0 || room_id.empty()) {
        result.status = "Not connected to a room";
        return result;
    }

    RoomAdminRequestHdr request{};
    request.magic = CTRL_MAGIC;
    request.type = CtrlHdr::Cmd::ROOM_ADMIN_REQUEST;
    request.request_id = room_admin_request_id_.fetch_add(1, std::memory_order_relaxed);
    request.command = command;
    request.access_mode = access_mode;
    request.target_participant_id = target_participant_id;
    write_fixed(request.room_id, room_id);
    write_fixed(request.admin_token, startup_options_.room_admin_token);
    write_fixed(request.password_hash, hash);

    try {
        const auto response =
            send_control_request<RoomAdminRequestHdr, RoomAdminResponseHdr>(
                address, port, request, CtrlHdr::Cmd::ROOM_ADMIN_RESPONSE,
                request.request_id);
        const auto reason = fixed_string(response.reason);
        result.ok = response.status == ROOM_STATUS_OK;
        result.closed = result.ok && closes_room;
        result.access_epoch = response.access_epoch;
        result.access_mode = response.access_mode;
        if (!result.ok) {
            result.status = room_admin_status_text(response.status, reason);
            return result;
        }

        switch (command) {
            case ROOM_ADMIN_CHANGE_PASSWORD:
                result.status = hash.empty() ? "Password cleared" : "Password set";
                break;
            case ROOM_ADMIN_CHANGE_ACCESS:
                result.status = "Room settings saved";
                break;
            case ROOM_ADMIN_KICK_PARTICIPANT:
                result.status = "Participant kicked";
                break;
            case ROOM_ADMIN_CLOSE_ROOM:
                result.status = "Room closed";
                break;
            case ROOM_ADMIN_ROTATE_MEDIA_KEY:
                result.status = "Room key rotated";
                break;
            default:
                result.status = "Done";
                break;
        }

        const bool should_rotate_media_key =
            command == ROOM_ADMIN_CHANGE_PASSWORD ||
            command == ROOM_ADMIN_CHANGE_ACCESS ||
            command == ROOM_ADMIN_KICK_PARTICIPANT ||
            command == ROOM_ADMIN_ROTATE_MEDIA_KEY;
        const auto current_secret = client_.current_media_secret();
        if (should_rotate_media_key && !current_secret.empty()) {
            const auto new_secret = make_media_secret();
            if (client_.rotate_media_key(new_secret)) {
                result.media_key_rotated = true;
                result.new_media_secret = new_secret;
                if (command != ROOM_ADMIN_ROTATE_MEDIA_KEY) {
                    result.status += "; room key rotated";
                }
            } else {
                result.status += "; room key rotation failed";
            }
        }
    } catch (const std::exception& e) {
        result.status = e.what();
    }

    return result;
}

void JuceMixerComponent::poll_room_admin_job() {
    std::optional<RoomAdminResult> result;
    {
        std::lock_guard<std::mutex> lock(room_admin_job_mutex_);
        if (!room_admin_job_finished_) {
            return;
        }
        result = std::move(room_admin_job_result_);
        room_admin_job_result_.reset();
        room_admin_job_finished_ = false;
        room_admin_job_running_ = false;
    }
    if (room_admin_job_thread_.joinable()) {
        room_admin_job_thread_.join();
    }
    if (result.has_value()) {
        apply_room_admin_result(std::move(*result));
    }
}

void JuceMixerComponent::apply_room_admin_result(RoomAdminResult result) {
    set_room_admin_status(result.status);
    if (!result.ok) {
        return;
    }
    if (result.access_mode == ROOM_ACCESS_OPEN ||
        result.access_mode == ROOM_ACCESS_PASSWORD ||
        result.access_mode == ROOM_ACCESS_APPROVE) {
        startup_options_.access_mode = result.access_mode;
        client_.set_room_access_mode(result.access_mode);
    }
    if (result.access_epoch != 0) {
        startup_options_.access_epoch = result.access_epoch;
    }
    if (result.media_key_rotated) {
        startup_options_.media_secret = std::move(result.new_media_secret);
    }
    if (result.closed) {
        client_.stop_connection();
        if (leave_callback_) {
            auto callback = leave_callback_;
            juce::MessageManager::callAsync([callback = std::move(callback)]() mutable {
                callback();
            });
        }
    }
}

void JuceMixerComponent::leave_room() {
    client_.stop_connection();
    auto callback = leave_callback_;
    if (callback) {
        juce::MessageManager::callAsync([callback = std::move(callback)]() mutable {
            callback();
        });
    }
}

void JuceMixerComponent::apply_selected_api_to_pending_devices(int old_api_index) {
    if (old_api_index == selected_api_index_ || selected_api_index_ < 0) {
        return;
    }

    const juce::String api_name = selected_api_name();
    for (const auto& device: input_devices_) {
        if (device.api_name == api_name.toStdString()) {
            pending_input_ = device.index;
            pending_input_channel_ = 0;
            break;
        }
    }
    for (const auto& device: output_devices_) {
        if (device.api_name == api_name.toStdString()) {
            pending_output_ = device.index;
            break;
        }
    }
}

juce::String JuceMixerComponent::selected_api_name() const {
    if (selected_api_index_ < 0) {
        return "All";
    }
    for (const auto& api: available_apis_) {
        if (api.index == selected_api_index_) {
            return api.name;
        }
    }
    return "All";
}

int JuceMixerComponent::api_index_for_name(const std::string& api_name) const {
    if (api_name.empty() || api_name == "All") {
        return -1;
    }
    for (const auto& api: available_apis_) {
        if (api.name == api_name) {
            return api.index;
        }
    }
    return -1;
}

int JuceMixerComponent::max_input_channels_for(AudioStream::DeviceIndex device_index) const {
    const auto active_device_info = client_.get_device_info();
    if (device_index == client_.get_selected_input_device()) {
        return std::max(active_device_info.input_channels, 1);
    }
    for (const auto& device: input_devices_) {
        if (device.index == device_index) {
            return std::max(device.max_input_channels, 1);
        }
    }
    return 1;
}

AudioStream::DeviceIndex JuceMixerComponent::selected_input_device() const {
    const auto api_name = selected_api_name();
    const int selected_id = input_combo_.getSelectedId();
    int combo_id = 1;
    for (const auto& device: input_devices_) {
        if (api_name != "All" && device.api_name != api_name.toStdString()) {
            continue;
        }
        if (combo_id == selected_id) {
            return device.index;
        }
        ++combo_id;
    }
    return AudioStream::NO_DEVICE;
}

AudioStream::DeviceIndex JuceMixerComponent::selected_output_device() const {
    const auto api_name = selected_api_name();
    const int selected_id = output_combo_.getSelectedId();
    int combo_id = 1;
    for (const auto& device: output_devices_) {
        if (api_name != "All" && device.api_name != api_name.toStdString()) {
            continue;
        }
        if (combo_id == selected_id) {
            return device.index;
        }
        ++combo_id;
    }
    return AudioStream::NO_DEVICE;
}

bool JuceMixerComponent::has_room_admin() const {
    return !startup_options_.room_admin_token.empty();
}

std::string JuceMixerComponent::password_hash(const std::string& password) const {
    if (password.empty()) {
        return {};
    }
    const std::vector<unsigned char> bytes(password.begin(), password.end());
    const auto digest = performer_join_token::try_sha256(bytes);
    if (!digest.has_value()) {
        return {};
    }
    return performer_join_token::hex(*digest);
}

juce::String JuceMixerComponent::invite_text() const {
    const auto connected_port = client_.get_server_port();
    const std::string address =
        client_.get_server_address().empty() ? startup_options_.server_address
                                             : client_.get_server_address();
    const uint16_t port =
        connected_port == 0 ? startup_options_.server_port : connected_port;
    const std::string room_id = client_.get_room_id();

    return juce::String(make_secure_invite_link(address, port, room_id));
}

bool JuceMixerComponent::has_pending_audio_changes() const {
    return pending_stream_restart_needed() ||
           pending_opus_frames_per_packet_ != client_.get_opus_network_frame_count() ||
           selected_api_name().toStdString() != client_.get_audio_api_filter();
}

bool JuceMixerComponent::pending_stream_restart_needed() const {
    return pending_input_ != client_.get_selected_input_device() ||
           pending_output_ != client_.get_selected_output_device() ||
           pending_input_channel_ != client_.get_input_channel_index() ||
           pending_buffer_frames_ != client_.get_audio_config().frames_per_buffer;
}

void JuceMixerComponent::set_device_status(const juce::String& text) {
    device_status_label_.setText(text, juce::dontSendNotification);
}

void JuceMixerComponent::set_room_admin_status(const juce::String& text) {
    room_admin_status_ = text;
    room_admin_status_label_.setText(room_admin_status_, juce::dontSendNotification);
}
