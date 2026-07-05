// =============================================================================
// Zynlab-Style Jam Client UI
// =============================================================================

// Layout constants
static constexpr float TRACK_WIDTH = 140.0F;  // Wider strips
// FADER_HEIGHT removed - now dynamically calculated based on window size
static constexpr float METER_WIDTH  = 20.0F;
static constexpr float KNOB_SIZE    = 50.0F;
static constexpr float MASTER_WIDTH = 160.0F;  // Wider master

// Draw the master (your own audio) channel strip with WAV controls
static void draw_master_strip(ClientAppFacade& client, float available_height) {
    ImGuiStyle& style       = ImGui::GetStyle();
    float       strip_width = MASTER_WIDTH;
    float       line_height = ImGui::GetTextLineHeightWithSpacing();

    // Dynamic fader height - scale with available space, min 200, max based on window
    // Reserved space: title, mute btn, fader/meter, label, separator, latency section (4 lines),
    // separator, WAV section
    float fader_height = std::max(120.0F, available_height - 560.0F);

    // Padding constant
    constexpr float PADDING = 8.0F;

    ImGui::BeginChild("MasterStrip", ImVec2(strip_width, 0), ImGuiChildFlags_None);
    {
        float width = ImGui::GetContentRegionAvail().x - PADDING;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (PADDING / 2.0F));

        // Title
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2F, 0.4F, 0.6F, 1.0F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25F, 0.5F, 0.7F, 1.0F));
        ImGui::Button("YOU", ImVec2(width, 0));
        ImGui::PopStyleColor(2);

        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (PADDING / 2.0F));

        // Mute button - explicit MUTE/UNMUTE text
        bool mic_muted = client.get_mic_muted();
        ImGui::PushStyleColor(ImGuiCol_Button, mic_muted ? ImVec4(0.8F, 0.2F, 0.2F, 1.0F)
                                                         : ImVec4(0.2F, 0.5F, 0.3F, 1.0F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mic_muted ? ImVec4(0.9F, 0.3F, 0.3F, 1.0F)
                                                                : ImVec4(0.3F, 0.6F, 0.4F, 1.0F));
        if (ImGui::Button(mic_muted ? "UNMUTE" : "MUTE", ImVec2(width, 0))) {
            client.set_mic_muted(!mic_muted);
        }
        JamGui::ShowTooltipOnHover("Click to toggle microphone mute");
        ImGui::PopStyleColor(2);

        bool self_monitor = client.get_self_monitor_enabled();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        if (ImGui::Checkbox("Monitor##SelfMonitor", &self_monitor)) {
            client.set_self_monitor_enabled(self_monitor);
        }
        JamGui::ShowTooltipOnHover("Hear your microphone in the local output");

        ImGui::Spacing();

        // Level meter and fader section
        float own_level = client.get_own_audio_level();
        int   meter_val = static_cast<int>(own_level * fader_height);

        // Center the meter + fader
        float total_control_width = METER_WIDTH + style.ItemSpacing.x + METER_WIDTH;
        float offset              = (strip_width - total_control_width) / 2.0F;

        ImGui::SetCursorPosX(offset);
        JamGui::UvMeter("##MasterMeter", ImVec2(METER_WIDTH, fader_height), &meter_val, 0,
                        static_cast<int>(fader_height));
        ImGui::SameLine();

        // Master volume fader (0-200, 100 = unity gain)
        static int master_vol = 100;
        // Sync from client when not dragging
        if (!ImGui::IsItemActive()) {
            master_vol = static_cast<int>(client.get_input_gain() * 100.0F);
        }
        if (JamGui::Fader("##MasterFader", ImVec2(METER_WIDTH, fader_height), &master_vol, 0, 200,
                          "%d%%", 1.0F)) {
            client.set_input_gain(static_cast<float>(master_vol) / 100.0F);
        }

        ImGui::Spacing();

        // Label
        JamGui::TextCentered(ImVec2(strip_width, line_height), "master");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Codec:");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Opus");
        JamGui::ShowTooltipOnHover("Compressed internet mode");

        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Jitter floor:");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        int jitter_ms = client.get_opus_jitter_buffer_ms();
        ImGui::PushItemWidth(width - PADDING);
        if (ImGui::InputInt("##OpusJitterMs", &jitter_ms, 5, 10)) {
            client.set_opus_jitter_buffer_ms(std::max(jitter_ms, 0));
            if (client.get_opus_queue_limit_packets() <
                client.get_opus_jitter_buffer_packets()) {
                client.set_opus_queue_limit_packets(client.get_opus_jitter_buffer_packets());
            }
        }
        ImGui::PopItemWidth();
        const double packet_ms = client.get_opus_network_packet_ms();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        if (client.get_opus_auto_jitter_default()) {
            ImGui::Text("%d ms (%zu pkt) floor, %d ms (%zu pkt) start",
                        client.get_opus_jitter_buffer_ms(),
                        client.get_opus_jitter_buffer_packets(),
                        client.get_opus_auto_start_jitter_ms(),
                        client.get_opus_auto_start_jitter_packets());
        } else {
            ImGui::Text("%d ms (%zu pkt)", client.get_opus_jitter_buffer_ms(),
                        client.get_opus_jitter_buffer_packets());
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        bool auto_jitter_default = client.get_opus_auto_jitter_default();
        if (ImGui::Checkbox("Auto jitter##GlobalAutoJitter", &auto_jitter_default)) {
            client.set_opus_auto_jitter_default(auto_jitter_default);
        }
        JamGui::ShowTooltipOnHover("Use adaptive jitter as the default for participants without custom settings");

        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Queue limit:");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        int queue_limit_packets = static_cast<int>(client.get_opus_queue_limit_packets());
        ImGui::PushItemWidth(width - PADDING);
        if (ImGui::InputInt("##OpusQueueLimitPackets", &queue_limit_packets, 1, 4)) {
            client.set_opus_queue_limit_packets(
                static_cast<size_t>(std::max(queue_limit_packets, 0)));
        }
        ImGui::PopItemWidth();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("%zu pkt max", client.get_opus_queue_limit_packets());

        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Age limit:");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        int age_limit_ms = client.get_jitter_packet_age_limit_ms();
        ImGui::PushItemWidth(width - PADDING);
        if (ImGui::InputInt("##JitterPacketAgeLimitMs", &age_limit_ms, 5, 20)) {
            client.set_jitter_packet_age_limit_ms(age_limit_ms);
        }
        ImGui::PopItemWidth();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("%d ms max", client.get_jitter_packet_age_limit_ms());

        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Redundancy:");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        int redundancy_depth = client.get_opus_redundancy_depth_setting();
        char redundancy_preview[32];
        if (redundancy_depth == OPUS_REDUNDANCY_DEPTH_AUTO) {
            std::snprintf(redundancy_preview, sizeof(redundancy_preview), "Auto (%d)",
                          client.get_effective_opus_redundancy_depth());
        } else if (redundancy_depth == 0) {
            std::snprintf(redundancy_preview, sizeof(redundancy_preview), "Off");
        } else {
            std::snprintf(redundancy_preview, sizeof(redundancy_preview), "%d prev",
                          redundancy_depth);
        }
        ImGui::PushItemWidth(width - PADDING);
        if (ImGui::BeginCombo("##OpusRedundancyDepth", redundancy_preview)) {
            if (ImGui::Selectable("Auto", redundancy_depth == OPUS_REDUNDANCY_DEPTH_AUTO)) {
                client.set_opus_redundancy_depth(OPUS_REDUNDANCY_DEPTH_AUTO);
            }
            if (ImGui::Selectable("Off", redundancy_depth == 0)) {
                client.set_opus_redundancy_depth(0);
            }
            for (int depth = 1; depth <= MAX_OPUS_REDUNDANCY_DEPTH_PACKETS; ++depth) {
                char label[32];
                std::snprintf(label, sizeof(label), "%d previous", depth);
                if (ImGui::Selectable(label, redundancy_depth == depth)) {
                    client.set_opus_redundancy_depth(depth);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("%d prev effective", client.get_effective_opus_redundancy_depth());
        JamGui::ShowTooltipOnHover("Previous Opus packets carried in each UDP datagram");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ========== METRONOME SECTION ==========
        ClientAppFacade::MetronomeState metronome = client.get_metronome_state();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Metronome:");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (PADDING / 2.0F));
        static float metronome_draft_bpm = 120.0F;
        static bool metronome_bpm_editing = false;
        static bool metronome_bpm_dirty = false;
        static auto metronome_bpm_last_edit = std::chrono::steady_clock::now();
        constexpr auto METRONOME_BPM_DEBOUNCE = std::chrono::milliseconds(350);
        if (!metronome_bpm_editing && !metronome_bpm_dirty) {
            metronome_draft_bpm = metronome.bpm;
        }
        ImGui::PushItemWidth(width);
        if (ImGui::InputFloat("##MetronomeBpm", &metronome_draft_bpm, 1.0F, 5.0F, "%.1f BPM")) {
            metronome_draft_bpm = std::clamp(metronome_draft_bpm, 30.0F, 240.0F);
            metronome_bpm_dirty = true;
            metronome_bpm_last_edit = std::chrono::steady_clock::now();
        }
        metronome_bpm_editing = ImGui::IsItemActive();
        ImGui::PopItemWidth();
        if (metronome_bpm_dirty && !metronome_bpm_editing &&
            std::chrono::steady_clock::now() - metronome_bpm_last_edit >=
                METRONOME_BPM_DEBOUNCE) {
            client.commit_metronome_bpm(metronome_draft_bpm);
            metronome_bpm_dirty = false;
        }

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (PADDING / 2.0F));
        if (ImGui::Button(metronome.running ? "Stop##Metronome" : "Start##Metronome",
                          ImVec2((width - style.ItemSpacing.x) * 0.5F, 0))) {
            if (metronome.running) {
                client.stop_metronome();
            } else {
                client.start_metronome();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Tap##Metronome", ImVec2((width - style.ItemSpacing.x) * 0.5F, 0))) {
            client.tap_metronome_tempo();
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Beat: %u", metronome.beat_number);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Sync: %llu/%llu",
                    static_cast<unsigned long long>(metronome.sync_sent),
                    static_cast<unsigned long long>(metronome.sync_received));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Clock sync: %s", metronome.clock_ready ? "Locked" : "Syncing");
        char clock_tooltip[160];
        std::snprintf(clock_tooltip, sizeof(clock_tooltip),
                      "Raw monotonic-clock offset: %.2f ms. Large values are normal across machines.",
                      metronome.clock_offset_ms);
        JamGui::ShowTooltipOnHover(clock_tooltip);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ========== RECORDING SECTION ==========
        ClientAppFacade::RecordingState recording = client.get_recording_state();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Recording:");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (PADDING / 2.0F));
        if (recording.active) {
            if (ImGui::Button("Stop Recording", ImVec2(width, 0))) {
                client.stop_recording();
            }
        } else if (ImGui::Button("Start Recording", ImVec2(width, 0))) {
            client.start_recording();
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("%s", recording.active ? "REC" : "Idle");
        if (!recording.folder.empty()) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::TextWrapped("%s", recording.folder.c_str());
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Queued: %zu", recording.queued_blocks);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Dropped: %llu",
                    static_cast<unsigned long long>(recording.dropped_blocks));

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ========== PATH DIAGNOSTICS ==========
        ClientAppFacade::PathDiagnostics path = client.get_path_diagnostics();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Path:");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("RTT %.1f/%.1f/%.1f ms",
                    path.rtt_last_ms, path.rtt_avg_ms, path.rtt_max_ms);
        JamGui::ShowTooltipOnHover("RTT last / average / max since the current UDP path joined");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Ping loss %.1f%%", path.ping_gap_percent);
        char ping_tooltip[128];
        std::snprintf(ping_tooltip, sizeof(ping_tooltip),
                      "Missing ping replies: received=%u missing=%u consecutive=%u",
                      path.ping_received, path.ping_missing,
                      path.ping_consecutive_missing);
        JamGui::ShowTooltipOnHover(ping_tooltip);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Ingress loss %.1f%%", path.audio_ingress_gap_percent);
        char ingress_tooltip[128];
        std::snprintf(ingress_tooltip, sizeof(ingress_tooltip),
                      "Server-reported audio ingress: received=%u gaps=%u",
                      path.audio_ingress_received,
                      path.audio_ingress_gaps);
        JamGui::ShowTooltipOnHover(ingress_tooltip);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Total ~%.1f ms", path.total_estimate_ms);
        char total_tooltip[192];
        std::snprintf(total_tooltip, sizeof(total_tooltip),
                      "Input %.1f + Opus %.1f + RTT/2 %.1f + jitter %.1f + "
                      "output %.1f + TX q %.1f ms",
                      path.total_input_ms, path.total_opus_ms,
                      path.total_network_ms, path.total_jitter_ms,
                      path.total_output_ms, path.opus_send_queue_avg_ms);
        JamGui::ShowTooltipOnHover(total_tooltip);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        if (path.e2e_latency_samples > 0) {
            ImGui::Text("E2E %.1f/%.1f ms",
                        path.e2e_latency_avg_max_ms, path.e2e_latency_peak_ms);
            JamGui::ShowTooltipOnHover("Capture-to-playout average max / peak across participants");
        } else {
            ImGui::Text("E2E waiting");
            JamGui::ShowTooltipOnHover("Waiting for timestamped packets and server clock sync");
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("TX q %.2f/%.2f ms",
                    path.opus_send_queue_avg_ms, path.opus_send_queue_max_ms);
        JamGui::ShowTooltipOnHover("Opus sender queue age average / max");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Pace %.2f/%.2f ms", path.tx_pace_avg_ms, path.tx_pace_max_ms);
        JamGui::ShowTooltipOnHover("Audio packet send pacing average / max");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("RX q %zu/%zu/%zu",
                    path.rx_queue_current, path.rx_queue_avg_max, path.rx_queue_peak);
        JamGui::ShowTooltipOnHover("Receiver queue current total / worst average / worst max");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("PLC/Underrun %zu/%d", path.plc_frames, path.underruns);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Opus %u J%zu Q%zu",
                    client.get_opus_network_frame_count(),
                    client.get_opus_jitter_buffer_packets(),
                    client.get_opus_queue_limit_packets());
        JamGui::ShowTooltipOnHover("Current manual Opus frames, jitter packets, queue limit");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ========== LATENCY INFO (with padding) ==========
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ClientAppFacade::DeviceInfo device_info = client.get_device_info();
        AudioStream::LatencyInfo latency      = client.get_latency_info();
        AudioStream::AudioConfig audio_config = client.get_audio_config();
        ClientAppFacade::CallbackTimingInfo callback_timing =
            client.get_callback_timing_info();
        ImGui::Text("%s", device_info.output_api.c_str());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("In: %.1f ms", latency.input_latency_ms);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Out: %.1f ms", latency.output_latency_ms);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("SR: %d kHz", audio_config.sample_rate / 1000);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Buf: %d/%d", latency.actual_buffer_frames, latency.requested_buffer_frames);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Buf ms: %.2f", latency.buffer_duration_ms);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Cb: %.2f/%.2f ms", callback_timing.avg_ms, callback_timing.deadline_ms);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("Max: %.2f ms", callback_timing.max_ms);
        if (device_info.output_api.find("Windows Audio") != std::string::npos &&
            !latency.backend_latency_available) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::TextColored(ImVec4(1.0F, 0.6F, 0.2F, 1.0F), "Backend latency unknown");
        }
        if (callback_timing.over_deadline_count > 0) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::TextColored(ImVec4(1.0F, 0.6F, 0.2F, 1.0F), "Late: %llu",
                               static_cast<unsigned long long>(
                                   callback_timing.over_deadline_count));
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ========== WAV SECTION (with padding) ==========
        ClientAppFacade::WavState wav_state = client.get_wav_state();

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
        ImGui::Text("WAV File:");

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (PADDING / 2.0F));
        static char wav_file_path[512] = "";
        ImGui::PushItemWidth(width);
        ImGui::InputText("##WavPath", wav_file_path, sizeof(wav_file_path));
        ImGui::PopItemWidth();

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (PADDING / 2.0F));
        if (ImGui::Button("Load", ImVec2(width, 0))) {
            if (strlen(wav_file_path) > 0) {
                client.load_wav_file(wav_file_path);
            }
        }

        if (wav_state.is_loaded) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (PADDING / 2.0F));
            // Play/Pause button
            if (wav_state.is_playing) {
                if (ImGui::Button("Pause", ImVec2(width, 0))) {
                    client.wav_pause();
                }
            } else {
                if (ImGui::Button("Play", ImVec2(width, 0))) {
                    client.wav_play();
                }
            }

            // Progress/Seek
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (PADDING / 2.0F));
            float seek_pos = static_cast<float>(wav_state.position);
            float max_pos  = static_cast<float>(wav_state.total_frames);
            ImGui::PushItemWidth(width);
            if (wav_state.is_playing) {
                float progress = (max_pos > 0) ? seek_pos / max_pos : 0.0F;
                ImGui::ProgressBar(progress, ImVec2(width, 0), "");
            } else {
                if (ImGui::SliderFloat("##Seek", &seek_pos, 0.0F, max_pos, "%.0f")) {
                    client.wav_seek(static_cast<int64_t>(seek_pos));
                }
            }
            ImGui::PopItemWidth();

            // Volume
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::Text("Volume:");
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (PADDING / 2.0F));
            float wav_gain = wav_state.gain;
            ImGui::PushItemWidth(width);
            if (ImGui::SliderFloat("##WavVol", &wav_gain, 0.0F, 2.0F, "%.2f")) {
                client.set_wav_gain(wav_gain);
            }
            ImGui::PopItemWidth();

            // Mute local
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (PADDING / 2.0F));
            bool muted_local = wav_state.muted_local;
            if (ImGui::Checkbox("Mute Local##wav", &muted_local)) {
                client.set_wav_muted_local(muted_local);
            }
            JamGui::ShowTooltipOnHover("Mute locally but still send to others");
        }
    }
    ImGui::EndChild();
}

// Draw a participant channel strip
struct ParticipantQualityStatus {
    const char* label;
    const char* reason;
    const char* action;
    ImVec4 color;
};

static ParticipantQualityStatus participant_quality_status(const ParticipantInfo& p) {
    if (!p.buffer_ready) {
        return {"Recovering", "waiting for playout buffer",
                "wait; reconnect if it stays here",
                ImVec4(1.0F, 0.8F, 0.2F, 1.0F)};
    }

    if (p.opus_queue_limit_drops > 0 || p.opus_decode_buffer_overflow_drops > 0) {
        return {"Poor", "queue overflow/drop",
                "raise queue limit or reduce network burstiness",
                ImVec4(1.0F, 0.35F, 0.25F, 1.0F)};
    }

    if (p.jitter_age_drops > 0 || p.opus_age_limit_drops > 0) {
        return {"Jittery", "packet age limit",
                "raise age limit for testing; prefer Ethernet",
                ImVec4(1.0F, 0.65F, 0.25F, 1.0F)};
    }

    if (p.underrun_count > 0 || p.plc_count > 0) {
        return {"Jittery", "underrun/PLC",
                "raise jitter target or enable auto",
                ImVec4(1.0F, 0.65F, 0.25F, 1.0F)};
    }

    if (p.sequence_unresolved_gaps > 0) {
        return {"Jittery", "unrecovered packet gap",
                "use Ethernet or raise jitter target",
                ImVec4(1.0F, 0.65F, 0.25F, 1.0F)};
    }

    if (p.sequence_late_or_reordered > 0) {
        return {"Stable", "packet reorder recovered",
                "no change unless drops appear",
                ImVec4(0.35F, 0.85F, 0.45F, 1.0F)};
    }

    if (p.receiver_drift_ppm_abs_max > 100.0) {
        return {"Jittery", "clock drift",
                "record long-session drift data",
                ImVec4(1.0F, 0.65F, 0.25F, 1.0F)};
    }

    return {"Stable", "within current target",
            "no change",
            ImVec4(0.35F, 0.85F, 0.45F, 1.0F)};
}

static void draw_participant_strip(ClientAppFacade& client, const ParticipantInfo& p, int index,
                                   float available_height) {
    ImGuiStyle& style       = ImGui::GetStyle();
    float       strip_width = TRACK_WIDTH;
    float       line_height = ImGui::GetTextLineHeightWithSpacing();

    // Dynamic fader height - scale with available space
    // Reserve more space for: title, mute btn, pan knob, label, separator, stats section (expanded)
    float fader_height = std::max(200.0F, available_height - 330.0F);

    // Padding constant
    constexpr float PADDING = 8.0F;

    // Get track color based on index
    ImVec4 track_color         = JamGui::GetTrackColor(index, 0.6F, 0.6F);
    ImVec4 track_color_hovered = JamGui::GetTrackColor(index, 0.7F, 0.7F);
    ImVec4 track_color_active  = JamGui::GetTrackColor(index, 0.8F, 0.8F);

    // Background tint for highlighted/selected
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 0.02F));

    ImGui::PushID(static_cast<int>(p.id));
    ImGui::BeginChild("ParticipantStrip", ImVec2(strip_width, 0), ImGuiChildFlags_None);
    {
        float width = ImGui::GetContentRegionAvail().x - PADDING;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (PADDING / 2.0F));

        // Push track-specific colors for title
        ImGui::PushStyleColor(ImGuiCol_Button, track_color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, track_color_hovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, track_color_active);

        // Participant name button (title)
        char fallback_name_buf[32];
        std::snprintf(fallback_name_buf, sizeof(fallback_name_buf), "User #%u", p.id);
        const std::string participant_name =
            p.display_name.empty() ? std::string(fallback_name_buf) : p.display_name;
        ImGui::Button(participant_name.c_str(), ImVec2(width, 0));
        ImGui::PopStyleColor(3);

        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (PADDING / 2.0F));

        // Mute button - explicit MUTE/UNMUTE text
        bool muted = p.is_muted;
        ImGui::PushStyleColor(ImGuiCol_Button, muted ? ImVec4(0.8F, 0.2F, 0.2F, 1.0F)
                                                     : ImVec4(0.2F, 0.5F, 0.3F, 1.0F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, muted ? ImVec4(0.9F, 0.3F, 0.3F, 1.0F)
                                                            : ImVec4(0.3F, 0.6F, 0.4F, 1.0F));
        char mute_label[32];
        std::snprintf(mute_label, sizeof(mute_label), muted ? "UNMUTE##%u" : "MUTE##%u", p.id);
        if (ImGui::Button(mute_label, ImVec2(width, 0))) {
            client.set_participant_muted(p.id, !muted);
        }
        JamGui::ShowTooltipOnHover(muted ? "Click to unmute" : "Click to mute");
        ImGui::PopStyleColor(2);

        ImGui::Spacing();

        // Pan knob at TOP - use local cache to prevent jitter during drag
        static std::unordered_map<uint32_t, float> pan_cache;
        if (!pan_cache.contains(p.id)) {
            pan_cache[p.id] = p.pan * 127.0F;
        }
        bool  knob_active = false;
        float pan_val     = pan_cache[p.id];

        float knob_offset = (strip_width - KNOB_SIZE) / 2.0F;
        ImGui::SetCursorPosX(knob_offset);
        if (JamGui::Knob("pan", &pan_val, 0.0F, 127.0F, ImVec2(KNOB_SIZE, KNOB_SIZE), "Pan")) {
            pan_cache[p.id] = pan_val;
            client.set_participant_pan(p.id, pan_val / 127.0F);
            knob_active = true;
        }
        // Update cache from server when not dragging
        if (!knob_active && !ImGui::IsItemActive()) {
            pan_cache[p.id] = p.pan * 127.0F;
        }

        ImGui::Spacing();

        // Level meter and volume fader
        int meter_val = static_cast<int>(p.audio_level * fader_height);

        // Center the meter + fader
        float total_control_width = METER_WIDTH + style.ItemSpacing.x + METER_WIDTH;
        float offset              = (strip_width - total_control_width) / 2.0F;

        ImGui::SetCursorPosX(offset);
        JamGui::UvMeter("##meter", ImVec2(METER_WIDTH, fader_height), &meter_val, 0,
                        static_cast<int>(fader_height));
        ImGui::SameLine();

        // Volume fader - 0-200 range, 100 = unity gain (use local cache to prevent jitter)
        static std::unordered_map<uint32_t, int> vol_cache;
        if (!vol_cache.contains(p.id) || !ImGui::IsItemActive()) {
            vol_cache[p.id] = static_cast<int>(p.gain * 100.0F);
        }
        int vol = vol_cache[p.id];
        vol     = std::clamp(vol, 0, 200);
        if (JamGui::Fader("##vol", ImVec2(METER_WIDTH, fader_height), &vol, 0, 200, "%d%%", 1.0F)) {
            vol_cache[p.id] = vol;
            client.set_participant_gain(p.id, static_cast<float>(vol) / 100.0F);
        }

        ImGui::Spacing();

        // Participant label (lowercase to avoid ID conflict with title button)
        char label_buf[32];
        std::snprintf(label_buf, sizeof(label_buf), "user %u", p.id);
        JamGui::TextCentered(ImVec2(strip_width, line_height), label_buf);

        ImGui::Spacing();
        ImGui::Separator();

        // Connection stats section at bottom (open by default, with padding)
        char stats_label[32];
        std::snprintf(stats_label, sizeof(stats_label), "Stats##%u", p.id);
        if (ImGui::CollapsingHeader(stats_label, ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto quality = participant_quality_status(p);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::TextColored(quality.color, "Quality: %s", quality.label);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::Text("Reason: %s", quality.reason);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::TextWrapped("Action: %s", quality.action);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::Text("Queue: %zu", p.queue_size);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::Text("Q avg/max: %zu/%zu", p.queue_size_avg, p.queue_size_max);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::Text("Q drift: %.2f", p.queue_drift_packets);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::Text("Jitter target:%s",
                        p.opus_jitter_auto_enabled
                            ? " auto"
                            : (p.opus_jitter_manual_override ? " custom" : " default"));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            bool auto_jitter = p.opus_jitter_auto_enabled;
            if (ImGui::Checkbox("Auto##ParticipantJitterAuto", &auto_jitter)) {
                client.set_participant_opus_auto_jitter(p.id, auto_jitter);
            }
            JamGui::ShowTooltipOnHover("Automatically raise this participant's jitter on instability");
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            int participant_jitter_ms = static_cast<int>(std::lround(
                static_cast<double>(p.jitter_buffer_min_packets) *
                client.get_opus_network_packet_ms()));
            ImGui::PushItemWidth(width - 42.0F);
            if (ImGui::InputInt("##ParticipantJitterMs", &participant_jitter_ms, 5, 10)) {
                client.set_participant_opus_jitter_buffer_ms(
                    p.id, std::max(participant_jitter_ms, 0));
            }
            ImGui::PopItemWidth();
            ImGui::SameLine();
            if (ImGui::Button("D##ParticipantJitterDefault")) {
                client.reset_participant_opus_jitter_buffer_packets(p.id);
            }
            JamGui::ShowTooltipOnHover("Use global default jitter for this participant");
            const double packet_ms =
                p.last_packet_frame_count > 0
                    ? (static_cast<double>(p.last_packet_frame_count) * 1000.0 / 48000.0)
                    : 0.0;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::Text("%d ms (%zu pkt)",
                        static_cast<int>(std::lround(
                            static_cast<double>(p.jitter_buffer_min_packets) * packet_ms)),
                        p.jitter_buffer_min_packets);
            if (p.opus_jitter_auto_enabled ||
                p.opus_jitter_auto_increases > 0 ||
                p.opus_jitter_auto_decreases > 0) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
                ImGui::Text("Auto inc/dec: %llu/%llu",
                            static_cast<unsigned long long>(p.opus_jitter_auto_increases),
                            static_cast<unsigned long long>(p.opus_jitter_auto_decreases));
            }
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::Text("Queue limit: %zu pkt", p.opus_queue_limit_packets);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::Text("Frames pkt/cb: %zu/%zu", p.last_packet_frame_count,
                        p.last_callback_frame_count);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::Text("Decoded: %zu frames", p.opus_pcm_buffered_frames);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::Text("Dec pkts: %llu",
                        static_cast<unsigned long long>(
                            p.opus_packets_decoded_in_callback));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::Text("Age: %.1f ms", p.packet_age_avg_ms);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::Text("Max age: %.1f ms", p.packet_age_max_ms);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            if (p.capture_to_playout_latency_samples > 0) {
                ImGui::Text("E2E: %.1f ms", p.capture_to_playout_latency_avg_ms);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
                ImGui::Text("Max E2E: %.1f ms", p.capture_to_playout_latency_max_ms);
            } else {
                ImGui::Text("E2E: waiting");
            }
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
            ImGui::Text("Drift ppm: %.1f avg", p.receiver_drift_ppm_avg);
            if (p.sequence_gaps > 0 || p.sequence_late_or_reordered > 0 ||
                p.sequence_unresolved_gaps > 0) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
                ImGui::TextColored(ImVec4(1.0F, 0.6F, 0.2F, 1.0F),
                                   "Seq gap/rec/unres/late: %llu/%llu/%llu/%llu",
                                   static_cast<unsigned long long>(p.sequence_gaps),
                                   static_cast<unsigned long long>(
                                       p.sequence_gap_recoveries),
                                   static_cast<unsigned long long>(
                                       p.sequence_unresolved_gaps),
                                   static_cast<unsigned long long>(
                                       p.sequence_late_or_reordered));
            }
            if (p.jitter_depth_drops > 0 || p.jitter_age_drops > 0) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
                ImGui::TextColored(ImVec4(1.0F, 0.6F, 0.2F, 1.0F), "Drop q/age: %llu/%llu",
                                   static_cast<unsigned long long>(p.jitter_depth_drops),
                                   static_cast<unsigned long long>(p.jitter_age_drops));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
                ImGui::TextColored(
                    ImVec4(1.0F, 0.6F, 0.2F, 1.0F), "Why: %llu/%llu/%llu/%llu",
                    static_cast<unsigned long long>(p.opus_queue_limit_drops),
                    static_cast<unsigned long long>(p.opus_age_limit_drops),
                    static_cast<unsigned long long>(p.opus_decode_buffer_overflow_drops),
                    static_cast<unsigned long long>(p.opus_target_trim_drops));
            } else if (p.opus_target_trim_drops > 0) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
                ImGui::TextColored(ImVec4(1.0F, 0.6F, 0.2F, 1.0F), "Target trim: %llu",
                                   static_cast<unsigned long long>(
                                       p.opus_target_trim_drops));
            }
            if (p.underrun_count > 0) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
                ImGui::TextColored(ImVec4(1.0F, 0.6F, 0.2F, 1.0F), "Underruns: %d",
                                   p.underrun_count);
            }
            if (p.plc_count > 0) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
                ImGui::Text("PLC: %zu", p.plc_count);
            }
            if (!p.buffer_ready) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PADDING);
                ImGui::TextColored(ImVec4(1.0F, 0.8F, 0.2F, 1.0F), "Buffering...");
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopID();

    ImGui::PopStyleColor();  // ChildBg
}

// Draw bottom device selector bar (horizontal)
static void draw_bottom_bar(ClientAppFacade& client) {
    static std::vector<AudioStream::DeviceInfo> input_devices;
    static std::vector<AudioStream::DeviceInfo> output_devices;
    static std::vector<AudioStream::ApiInfo>    available_apis;
    static int                                  selected_api        = -1;
    static AudioStream::DeviceIndex             pending_input       = AudioStream::NO_DEVICE;
    static int                                  pending_input_channel = 0;
    static AudioStream::DeviceIndex             pending_output      = AudioStream::NO_DEVICE;
    static int                                  pending_buffer_frames = 0;
    static int                                  pending_opus_frames_per_packet = 0;
    static bool                                 devices_initialized = false;

    auto refresh_device_lists = [&]() {
        input_devices  = AudioStream::get_input_device_stubs();
        output_devices = AudioStream::get_output_device_stubs();
        available_apis = AudioStream::get_apis();
    };

    if (!devices_initialized) {
        refresh_device_lists();
    }

    auto selected_api_name = [&]() -> std::string {
        if (selected_api < 0) {
            return "All";
        }
        for (const auto& api: available_apis) {
            if (api.index == selected_api) {
                return api.name;
            }
        }
        return "All";
    };

    auto api_index_for_name = [&](const std::string& api_name) {
        if (api_name.empty() || api_name == "All") {
            return -1;
        }
        for (const auto& api: available_apis) {
            if (api.name == api_name) {
                return api.index;
            }
        }
        return -1;
    };

    auto max_input_channels_for = [&](AudioStream::DeviceIndex device_index) {
        const auto active_device_info = client.get_device_info();
        if (device_index == client.get_selected_input_device()) {
            return std::max(active_device_info.input_channels, 1);
        }
        for (const auto& dev: input_devices) {
            if (dev.index == device_index) {
                return std::max(dev.max_input_channels, 1);
            }
        }
        return 1;
    };

    if (!devices_initialized) {
        pending_input         = client.get_selected_input_device();
        pending_input_channel = client.get_input_channel_index();
        pending_output        = client.get_selected_output_device();
        pending_buffer_frames = client.get_audio_config().frames_per_buffer;
        pending_opus_frames_per_packet = client.get_opus_network_frame_count();
        selected_api = api_index_for_name(client.get_audio_api_filter());
        devices_initialized   = true;
    }
    pending_input_channel =
        std::clamp(pending_input_channel, 0, max_input_channels_for(pending_input) - 1);

    // API selector
    ImGui::AlignTextToFramePadding();
    ImGui::Text("API:");
    ImGui::SameLine();
    ImGui::PushItemWidth(100);
    const char* api_preview = (selected_api < 0) ? "All" : nullptr;
    for (const auto& api: available_apis) {
        if (api.index == selected_api) {
            api_preview = api.name.c_str();
            break;
        }
    }
    if (api_preview == nullptr) {
        api_preview = "All";
    }
    if (ImGui::BeginCombo("##ApiSelect", api_preview)) {
        if (ImGui::Selectable("All APIs", selected_api < 0)) {
            selected_api = -1;
        }
        for (const auto& api: available_apis) {
            char api_label[128];
            std::snprintf(api_label, sizeof(api_label), "%s##api_%d", api.name.c_str(), api.index);
            bool is_selected = (api.index == selected_api);
            if (ImGui::Selectable(api_label, is_selected)) {
                int old_api  = selected_api;
                selected_api = api.index;

                // Auto-switch: when user selects an API, automatically switch to first devices with
                // that API
                if (old_api != selected_api && selected_api >= 0) {
                    // Find first available input device with this API
                    AudioStream::DeviceIndex new_input = AudioStream::NO_DEVICE;
                    for (const auto& dev: input_devices) {
                        if (dev.api_name == api.name) {
                            new_input = dev.index;
                            break;
                        }
                    }

                    // Find first available output device with this API
                    AudioStream::DeviceIndex new_output = AudioStream::NO_DEVICE;
                    for (const auto& dev: output_devices) {
                        if (dev.api_name == api.name) {
                            new_output = dev.index;
                            break;
                        }
                    }

                    // Switch if we found both devices (preferred)
                    if (new_input != AudioStream::NO_DEVICE &&
                        new_output != AudioStream::NO_DEVICE) {
                        pending_input  = new_input;
                        pending_input_channel = 0;
                        pending_output = new_output;
                    } else if (new_input != AudioStream::NO_DEVICE) {
                        // Found input but not output - switch input only
                        pending_input = new_input;
                        pending_input_channel = 0;
                    } else if (new_output != AudioStream::NO_DEVICE) {
                        // Found output but not input - switch output only
                        pending_output = new_output;
                    }
                    // If neither found, just keep filter active (user can manually select)
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Input:");
    ImGui::SameLine();
    ImGui::PushItemWidth(250);
    std::string input_preview = "Select...";
    for (const auto& dev: input_devices) {
        if (dev.index == pending_input) {
            input_preview = dev.name;
            break;
        }
    }
    if (ImGui::BeginCombo("##InputDev", input_preview.c_str())) {
        for (const auto& dev: input_devices) {
            const std::string api_filter = selected_api_name();
            if (api_filter != "All" && dev.api_name != api_filter) {
                continue;
            }
            char dev_label[256];
            std::snprintf(dev_label, sizeof(dev_label), "%s (%s)##dev_%d", dev.name.c_str(),
                          dev.api_name.c_str(), dev.index);
            if (ImGui::Selectable(dev_label, dev.index == pending_input)) {
                pending_input = dev.index;
                if (const auto* info = AudioStream::get_device_info(dev.index)) {
                    for (auto& cached: input_devices) {
                        if (cached.index == dev.index) {
                            cached.max_input_channels = info->max_input_channels;
                            cached.sample_rates = info->sample_rates;
                            cached.default_sample_rate = info->default_sample_rate;
                            break;
                        }
                    }
                }
                pending_input_channel =
                    std::clamp(pending_input_channel, 0,
                               max_input_channels_for(pending_input) - 1);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Ch:");
    ImGui::SameLine();
    ImGui::PushItemWidth(55);
    const int pending_input_channels = max_input_channels_for(pending_input);
    char input_channel_preview[16];
    std::snprintf(input_channel_preview, sizeof(input_channel_preview), "%d",
                  pending_input_channel + 1);
    if (ImGui::BeginCombo("##InputChannel", input_channel_preview)) {
        for (int channel = 0; channel < pending_input_channels; ++channel) {
            char channel_label[24];
            std::snprintf(channel_label, sizeof(channel_label), "%d##input_channel_%d",
                          channel + 1, channel);
            if (ImGui::Selectable(channel_label, channel == pending_input_channel)) {
                pending_input_channel = channel;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Output:");
    ImGui::SameLine();
    ImGui::PushItemWidth(250);
    std::string output_preview = "Select...";
    for (const auto& dev: output_devices) {
        if (dev.index == pending_output) {
            output_preview = dev.name;
            break;
        }
    }
    if (ImGui::BeginCombo("##OutputDev", output_preview.c_str())) {
        for (const auto& dev: output_devices) {
            const std::string api_filter = selected_api_name();
            if (api_filter != "All" && dev.api_name != api_filter) {
                continue;
            }
            char dev_label[256];
            std::snprintf(dev_label, sizeof(dev_label), "%s (%s)##dev_%d", dev.name.c_str(),
                          dev.api_name.c_str(), dev.index);
            if (ImGui::Selectable(dev_label, dev.index == pending_output)) {
                pending_output = dev.index;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Buffer:");
    ImGui::SameLine();
    ImGui::PushItemWidth(90);
    const int buffer_options[] = {96, 120, 128, 240, 256};
    char buffer_preview[32];
    std::snprintf(buffer_preview, sizeof(buffer_preview), "%d", pending_buffer_frames);
    if (ImGui::BeginCombo("##BufferFrames", buffer_preview)) {
        for (int frames: buffer_options) {
            char label[48];
            if (frames == 96) {
                std::snprintf(label, sizeof(label), "%d Ultra##buffer_%d", frames, frames);
            } else if (frames == 120) {
                std::snprintf(label, sizeof(label), "%d Low##buffer_%d", frames, frames);
            } else if (frames == 240) {
                std::snprintf(label, sizeof(label), "%d Safe##buffer_%d", frames, frames);
            } else {
                std::snprintf(label, sizeof(label), "%d##buffer_%d", frames, frames);
            }
            if (ImGui::Selectable(label, frames == pending_buffer_frames)) {
                pending_buffer_frames = frames;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Opus:");
    ImGui::SameLine();
    ImGui::PushItemWidth(135);
    const int opus_packet_options[] = {opus_network_clock::LOW_LATENCY_FRAME_COUNT,
                                       opus_network_clock::FAST_FRAME_COUNT,
                                       opus_network_clock::BALANCED_FRAME_COUNT,
                                       opus_network_clock::STABLE_FRAME_COUNT};
    auto opus_packet_label = [](int frames) {
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
    };
    char opus_packet_preview[48];
    std::snprintf(opus_packet_preview, sizeof(opus_packet_preview), "%d %s",
                  pending_opus_frames_per_packet,
                  opus_packet_label(pending_opus_frames_per_packet));
    if (ImGui::BeginCombo("##OpusPacketFrames", opus_packet_preview)) {
        for (int frames: opus_packet_options) {
            char label[56];
            std::snprintf(label, sizeof(label), "%d %s##opus_packet_%d", frames,
                          opus_packet_label(frames), frames);
            if (ImGui::Selectable(label, frames == pending_opus_frames_per_packet)) {
                pending_opus_frames_per_packet = frames;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
    JamGui::ShowTooltipOnHover("Opus network packet size; smaller is lower latency, larger is safer");

    ImGui::SameLine();

    // Check if devices changed
    AudioStream::DeviceIndex active_input  = client.get_selected_input_device();
    AudioStream::DeviceIndex active_output = client.get_selected_output_device();
    const int active_input_channel = client.get_input_channel_index();
    bool stream_restart_needed =
        (pending_input != active_input) || (pending_output != active_output) ||
        (pending_input_channel != active_input_channel) ||
        (pending_buffer_frames != client.get_audio_config().frames_per_buffer);
    bool opus_packet_changed =
        (pending_opus_frames_per_packet != client.get_opus_network_frame_count());
    bool devices_changed = stream_restart_needed || opus_packet_changed;
    static auto last_apply_time = std::chrono::steady_clock::time_point{};
    static auto last_reset_time = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    constexpr auto APPLY_COOLDOWN = std::chrono::milliseconds(1500);
    constexpr auto RESET_COOLDOWN = std::chrono::milliseconds(3000);
    const bool apply_cooling_down =
        last_apply_time.time_since_epoch().count() != 0 &&
        now - last_apply_time < APPLY_COOLDOWN;

    if (devices_changed) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8F, 0.6F, 0.2F, 1.0F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9F, 0.7F, 0.3F, 1.0F));
        if (apply_cooling_down) {
            ImGui::BeginDisabled();
            ImGui::Button("APPLYING");
            ImGui::EndDisabled();
        } else if (ImGui::Button("APPLY")) {
            last_apply_time = now;
            client.set_audio_api_filter(selected_api_name());
            client.set_input_device(pending_input);
            client.set_input_channel_index(pending_input_channel);
            client.set_output_device(pending_output);
            client.set_requested_frames_per_buffer(pending_buffer_frames);
            client.set_opus_network_frame_count(pending_opus_frames_per_packet);
            client.save_audio_device_preferences();
            if (client.is_audio_stream_active() && stream_restart_needed) {
                client.swap_audio_devices(pending_input, pending_output);
            }
        }
        ImGui::PopStyleColor(2);
    } else {
        bool is_active = client.is_audio_stream_active();
        if (is_active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7F, 0.2F, 0.2F, 1.0F));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8F, 0.3F, 0.3F, 1.0F));
            if (ImGui::Button("STOP")) {
                client.stop_audio_stream();
            }
            ImGui::PopStyleColor(2);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2F, 0.6F, 0.3F, 1.0F));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3F, 0.7F, 0.4F, 1.0F));
            if (ImGui::Button("START")) {
                if (pending_input != AudioStream::NO_DEVICE &&
                    pending_output != AudioStream::NO_DEVICE) {
                    client.set_audio_api_filter(selected_api_name());
                    client.set_input_device(pending_input);
                    client.set_input_channel_index(pending_input_channel);
                    client.set_output_device(pending_output);
                    client.set_requested_frames_per_buffer(pending_buffer_frames);
                    client.set_opus_network_frame_count(pending_opus_frames_per_packet);
                    client.save_audio_device_preferences();
                    AudioStream::AudioConfig config = client.get_audio_config();
                    client.start_audio_stream(pending_input, pending_output, config);
                }
            }
            ImGui::PopStyleColor(2);
        }
    }

    ImGui::SameLine();
    const bool reset_cooling_down =
        last_reset_time.time_since_epoch().count() != 0 &&
        now - last_reset_time < RESET_COOLDOWN;
    if (reset_cooling_down) {
        ImGui::BeginDisabled();
        ImGui::Button("RESET");
        ImGui::EndDisabled();
    } else if (ImGui::Button("RESET")) {
        last_reset_time = now;
        client.reset_audio_path();
    }
    JamGui::ShowTooltipOnHover(
        "Manual audio path reset: restarts the local stream and clears local audio queues. "
        "It keeps the current UDP session joined.");

    ImGui::SameLine();
    if (ImGui::Button("REFRESH")) {
        refresh_device_lists();
        pending_input_channel =
            std::clamp(pending_input_channel, 0, max_input_channels_for(pending_input) - 1);
    }
    JamGui::ShowTooltipOnHover("Refresh the audio device list");

    // Show error message if any
    const std::string& last_error = AudioStream::get_last_error();
    if (!last_error.empty()) {
        ImGui::TextColored(ImVec4(1.0F, 0.3F, 0.3F, 1.0F), "Error: %s", last_error.c_str());
    }
}

void draw_client_ui(ClientAppFacade& client) {
    // Apply zynlab theme on first frame
    static bool theme_applied = false;
    if (!theme_applied) {
        JamGui::ApplyZynlabTheme();
        theme_applied = true;
    }

    // Cache participant info
    static std::vector<ParticipantInfo> cached_participants;
    static int                          frame_counter = 0;
    if (frame_counter++ % 4 == 0) {
        cached_participants = client.get_participant_info();
    }

    // Main mixer window
    ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Jam Client", nullptr, ImGuiWindowFlags_MenuBar)) {
        // Menu bar with connection info
        if (ImGui::BeginMenuBar()) {
            // Connection status
            std::string server_info =
                client.get_server_address() + ":" + std::to_string(client.get_server_port());
            ImGui::Text("Server: %s", server_info.c_str());

            ImGui::Separator();

            // Room
            ImGui::Text("Room: %s", client.get_room_id().c_str());

            ImGui::Separator();

            // RTT
            double rtt = client.get_rtt_ms();
            if (rtt > 0) {
                ImGui::Text("RTT: %.1f ms", rtt);
            } else {
                ImGui::Text("RTT: --");
            }

            ImGui::Separator();

            // Participants count
            ImGui::Text("Users: %zu", cached_participants.size());

            ImGui::Separator();

            // Total bytes sent/received (throttled updates to reduce CPU usage)
            static std::string cached_rx_str = "0 B";
            static std::string cached_tx_str = "0 B";
            static auto        last_update   = std::chrono::steady_clock::now();

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update).count() >=
                1000) {
                uint64_t total_rx = client.get_total_bytes_rx();
                uint64_t total_tx = client.get_total_bytes_tx();

                // Format as KB or MB
                auto format_bytes = [](uint64_t bytes) -> std::string {
                    if (bytes < 1024) {
                        return std::to_string(bytes) + " B";
                    }
                    if (bytes < static_cast<uint64_t>(1024 * 1024)) {
                        return std::to_string(bytes / 1024) + " KB";
                    }
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.2f MB",
                                  static_cast<double>(bytes) / (1024.0 * 1024.0));
                    return std::string(buf);
                };

                cached_rx_str = format_bytes(total_rx);
                cached_tx_str = format_bytes(total_tx);
                last_update   = now;
            }

            ImGui::Text("RX: %s", cached_rx_str.c_str());
            ImGui::SameLine();
            ImGui::Text("TX: %s", cached_tx_str.c_str());
            JamGui::ShowTooltipOnHover("Total bytes received / transmitted");

            ImGui::Separator();

            // Audio status
            bool is_active = client.is_audio_stream_active();
            if (is_active) {
                ImGui::TextColored(ImVec4(0.3F, 0.9F, 0.3F, 1.0F), "CONNECTED");
            } else {
                ImGui::TextColored(ImVec4(0.9F, 0.5F, 0.2F, 1.0F), "DISCONNECTED");
            }

            ImGui::Separator();

            // FPS
            ImGui::Text("%.0f FPS", ImGui::GetIO().Framerate);

            ImGui::EndMenuBar();
        }

        // Get available height for channel strips
        float available_height =
            ImGui::GetContentRegionAvail().y - 65;  // Reserve space for device bar + error

        // Horizontal scrolling mixer area
        ImGui::BeginChild("Mixer", ImVec2(0, available_height), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5, 10));

            // Draw master strip
            draw_master_strip(client, available_height);
            ImGui::SameLine();

            // Space between master and participants
            // ImGui::Dummy(ImVec2(1, 0));
            // ImGui::SameLine();

            // Draw participant strips
            int index = 0;
            for (const auto& p: cached_participants) {
                draw_participant_strip(client, p, index++, available_height);
                ImGui::SameLine();
            }

            // Empty space at the end for scrolling
            ImGui::Dummy(ImVec2(20, 0));

            ImGui::PopStyleVar();
        }
        ImGui::EndChild();

        ImGui::Separator();

        // WAV playback controls at bottom
        draw_bottom_bar(client);
    }
    ImGui::End();
}
