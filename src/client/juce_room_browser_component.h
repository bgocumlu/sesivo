#pragma once

#include "client_app_facade.h"
#include "http_json_fetch_job.h"
#include "juce_startup_options.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class JuceRoomBrowserComponent final : public juce::Component, private juce::Timer {
public:
    struct JoinLaunch {
        std::string server_address;
        uint16_t server_port = 0;
        std::string room_id;
        std::string room_name;
        std::string room_admin_token;
        std::string room_instance_id;
        uint32_t access_epoch = 0;
        std::string media_secret;
        uint8_t access_mode = ROOM_ACCESS_OPEN;
    };

    JuceRoomBrowserComponent(ClientAppFacade& client,
                             JuceClientStartupOptions startup_options,
                             std::function<void(JoinLaunch)> joined_callback);
    ~JuceRoomBrowserComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event,
                        const juce::MouseWheelDetails& wheel) override;
    void open_invite(std::string invite_text);

private:
    class MonitorToggleButton final : public juce::Button {
    public:
        MonitorToggleButton();

    private:
        void paintButton(juce::Graphics& g, bool highlighted, bool down) override;
    };

    class BorderlessTextEditorLookAndFeel final : public juce::LookAndFeel_V4 {
    public:
        void fillTextEditorBackground(juce::Graphics&, int, int,
                                      juce::TextEditor&) override {}
        void drawTextEditorOutline(juce::Graphics&, int, int,
                                   juce::TextEditor&) override {}
    };

    struct BrowserRoom {
        std::string room_id;
        std::string room_name;
        std::string room_instance_id;
        uint32_t access_epoch = 0;
        uint16_t participant_count = 0;
        bool locked = false;
        uint8_t access_mode = ROOM_ACCESS_OPEN;
    };

    struct ServerStatus {
        bool ok = false;
        bool token_auth_available = false;
        bool truncated = false;
        std::string reason;
        std::string server_id;
        uint16_t total_rooms = 0;
        uint16_t active_participants = 0;
        double round_trip_ms = 0.0;
        std::vector<BrowserRoom> rooms;
    };

    struct BrowserServer {
        std::string name;
        std::string address;
        uint16_t port = 0;
        ServerStatus status;
        std::chrono::steady_clock::time_point last_refresh{};
    };

    struct PendingInviteJoin {
        std::string server_address;
        uint16_t server_port = 0;
        std::string room_id;
    };

    enum class JobKind {
        Status,
        Join,
        Create,
    };

    struct TicketResult {
        bool ok = false;
        uint8_t status = 0;
        std::string room_id;
        std::string room_name;
        std::string room_instance_id;
        uint32_t access_epoch = 0;
        uint8_t access_mode = ROOM_ACCESS_OPEN;
        std::string join_token;
        std::string admin_token;
        std::string reason;
    };

    struct BrowserJobResult {
        JobKind kind = JobKind::Status;
        int server_index = -1;
        std::string server_address;
        uint16_t server_port = 0;
        ServerStatus status;
        TicketResult ticket;
        bool joined = false;
        bool waiting_for_room_key = false;
        std::string joined_room_id;
        std::string display_name;
        std::string media_secret;
        uint8_t access_mode = ROOM_ACCESS_OPEN;
        std::string message;
    };

    struct AudioDeviceRefreshResult {
        std::vector<AudioStream::DeviceInfo> input_devices;
        std::vector<AudioStream::DeviceInfo> output_devices;
        std::vector<AudioStream::ApiInfo> available_apis;
        AudioStream::DeviceIndex pending_input = AudioStream::NO_DEVICE;
        AudioStream::DeviceIndex pending_output = AudioStream::NO_DEVICE;
        int selected_api_index = -1;
        juce::String status;
    };

    void timerCallback() override;
    void configure_controls();
    void request_audio_device_refresh();
    AudioDeviceRefreshResult load_audio_devices();
    void poll_audio_device_refresh();
    void apply_audio_device_refresh_result(AudioDeviceRefreshResult result);
    void populate_audio_preflight_controls();
    void set_audio_preflight_controls_enabled(bool enabled);
    void apply_audio_preflight_selection(bool restart_monitor);
    void start_or_stop_monitor();
    void sync_monitor_button_text();
    void load_servers();
    void save_servers(std::optional<bool> room_servers_seeded = std::nullopt) const;
    void request_official_server_refresh(bool manual = false);
    void poll_official_server_refresh();
    void apply_official_server_refresh_result(const HttpJsonFetchResult& result,
                                              bool manual);
    void start_status_refresh(bool manual);
    void start_join_flow(int room_index);
    void start_join_invite_flow(std::string initial_invite = {});
    void start_create_flow();
    void start_add_server_flow();
    void start_edit_servers_flow();
    void start_job(std::function<BrowserJobResult()> work);
    void poll_job_result();
    void apply_status(BrowserJobResult result);
    void apply_ticket_result(const BrowserJobResult& result);
    void remember_display_name(const std::string& display_name);
    void show_waiting_for_room_key(JoinLaunch launch);
    void finish_waiting_join();
    void cancel_waiting_join();

    uint32_t next_request_id();
    BrowserServer selected_server() const;
    const ServerStatus& selected_status() const;
    std::vector<int> visible_room_indices() const;
    void clamp_scroll_offsets();
    std::optional<int> find_server_endpoint(const std::string& address,
                                            uint16_t port,
                                            std::optional<int> ignored_index =
                                                std::nullopt) const;
    void focus_server(int index);
    int server_row_at(juce::Point<int> position) const;
    int room_row_at(juce::Point<int> position) const;
    bool join_button_at(juce::Point<int> position) const;
    void select_room(int room_index);

    juce::String selected_api_name() const;
    AudioStream::DeviceIndex selected_input_device() const;
    AudioStream::DeviceIndex selected_output_device() const;
    std::string password_hash(const std::string& password) const;
    std::string profile_id_for_display_name(const std::string& display_name) const;

    ClientAppFacade& client_;
    JuceClientStartupOptions startup_options_;
    std::function<void(JoinLaunch)> joined_callback_;

    std::vector<BrowserServer> servers_;
    int selected_server_index_ = 0;
    int selected_room_index_ = -1;
    bool room_servers_seeded_ = false;
    bool official_server_fetch_manual_ = false;
    std::string last_display_name_ = "Player";
    std::string local_profile_id_;
    juce::String status_text_ = "Choose a server";
    juce::String audio_preflight_status_ = "Loading devices...";
    std::chrono::steady_clock::time_point next_auto_refresh_{};
    std::atomic<uint32_t> request_id_{1};
    bool updating_audio_controls_ = false;
    bool audio_device_job_running_ = false;
    bool audio_device_job_finished_ = false;
    bool audio_controls_loaded_ = false;

    BorderlessTextEditorLookAndFeel search_editor_look_and_feel_;
    juce::TextEditor search_editor_;
    juce::ComboBox api_combo_;
    juce::ComboBox input_combo_;
    juce::ComboBox output_combo_;
    MonitorToggleButton monitor_toggle_;
    juce::Rectangle<int> refresh_button_bounds_;
    juce::Rectangle<int> edit_servers_button_bounds_;
    juce::Rectangle<int> add_server_bottom_bounds_;
    juce::Rectangle<int> join_invite_button_bounds_;
    juce::Rectangle<int> create_button_bounds_;
    juce::Rectangle<int> server_list_area_;
    juce::Rectangle<int> room_list_area_;
    int server_scroll_px_ = 0;
    int room_scroll_px_ = 0;
    std::unique_ptr<juce::AlertWindow> active_dialog_;
    std::optional<JoinLaunch> pending_join_launch_;
    std::optional<PendingInviteJoin> pending_invite_join_;
    bool waiting_for_room_key_ = false;

    std::mutex job_mutex_;
    std::thread job_thread_;
    bool job_running_ = false;
    bool job_finished_ = false;
    std::optional<BrowserJobResult> job_result_;

    HttpJsonFetchJob official_server_fetch_;

    std::mutex audio_device_job_mutex_;
    std::thread audio_device_job_thread_;
    std::optional<AudioDeviceRefreshResult> audio_device_job_result_;
    std::vector<AudioStream::DeviceInfo> input_devices_;
    std::vector<AudioStream::DeviceInfo> output_devices_;
    std::vector<AudioStream::ApiInfo> available_apis_;
    AudioStream::DeviceIndex pending_input_ = AudioStream::NO_DEVICE;
    AudioStream::DeviceIndex pending_output_ = AudioStream::NO_DEVICE;
    int selected_api_index_ = -1;
};
