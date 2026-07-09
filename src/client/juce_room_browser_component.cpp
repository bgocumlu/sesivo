#include "juce_room_browser_component.h"

#include "juce_theme.h"
#include "packet_builder.h"
#include "performer_join_token.h"
#include "protocol.h"
#include "secure_invite.h"

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

using asio::ip::udp;
using namespace std::chrono_literals;

namespace {
constexpr int BROWSER_HEADER_HEIGHT = 66;
constexpr int BROWSER_BOTTOM_HEIGHT = 78;
constexpr int SERVER_RAIL_WIDTH = 315;
constexpr int CONTENT_PAD = juce_theme::pad;
constexpr int CONTENT_GAP = juce_theme::gap;
constexpr int SERVER_ROW_HEIGHT = 70;
constexpr int ROOM_ROW_HEIGHT = 66;
constexpr int ROOM_STATUS_PAGE_LIMIT = 8;
constexpr const char* OFFICIAL_SERVERS_URL = "https://sesivo.app/api/servers.json";
constexpr int OFFICIAL_SERVERS_FETCH_TIMEOUT_MS = 5000;

namespace ui {
const juce::Colour background{0xff121315};
const juce::Colour panel{0xff191b1d};
const juce::Colour row{0xff222426};
const juce::Colour row_hover{0xff232528};
const juce::Colour row_selected{0xff2a241f};
const juce::Colour border{0xff343639};
const juce::Colour border_soft{0xff2c2e31};
const juce::Colour text{0xffe7ebee};
const juce::Colour text_dim{0xffaab3ba};
const juce::Colour text_faint{0xff79838b};
const juce::Colour accent{0xfffd943c};
const juce::Colour accent_hi{0xfffc8c3c};
const juce::Colour green{0xff7ccb4c};
const juce::Colour amber{0xfffd943c};
const juce::Colour red{0xffd9574f};

constexpr float radius = 2.0f;
constexpr int pad = 12;

juce::Font font(float size, bool bold = false) {
    return juce::Font(juce::FontOptions(size, bold ? juce::Font::bold
                                                   : juce::Font::plain));
}

void fill_panel(juce::Graphics& g, juce::Rectangle<int> bounds) {
    auto area = bounds.toFloat().reduced(0.5f);
    g.setColour(panel);
    g.fillRect(area);
    g.setColour(border);
    g.drawRect(area, 1.0f);
}

void fill_button(juce::Graphics& g, juce::Rectangle<int> bounds, bool primary = false,
                 bool disabled = false) {
    auto area = bounds.toFloat().reduced(0.5f);
    g.setColour(disabled ? juce::Colour{0xff17191b} : row);
    g.fillRoundedRectangle(area, radius);
    g.setColour(disabled ? border_soft : (primary ? accent : border));
    g.drawRoundedRectangle(area, radius, 1.0f);
}

void draw_label(juce::Graphics& g, juce::Rectangle<int> bounds,
                const juce::String& value, float size, juce::Colour colour,
                bool bold = false,
                juce::Justification justification = juce::Justification::centredLeft) {
    g.setColour(colour);
    g.setFont(font(size, bold));
    g.drawFittedText(value, bounds, justification, 1);
}

void draw_dot(juce::Graphics& g, juce::Point<float> centre, juce::Colour colour,
              float radius_value = 4.0f) {
    g.setColour(colour);
    g.fillEllipse(centre.x - radius_value, centre.y - radius_value,
                  radius_value * 2.0f, radius_value * 2.0f);
}

void draw_pill(juce::Graphics& g, juce::Rectangle<int> bounds,
               const juce::String& value) {
    auto area = bounds.toFloat().reduced(0.5f);
    g.setColour(row);
    g.fillRoundedRectangle(area, 3.0f);
    g.setColour(border);
    g.drawRoundedRectangle(area, 3.0f, 1.0f);
    draw_label(g, bounds.reduced(8, 0), value, 12.0f, text_dim);
}

void draw_meter(juce::Graphics& g, juce::Rectangle<int> bounds, float amount,
                juce::Colour active = accent) {
    auto area = bounds.toFloat();
    g.setColour(background);
    g.fillRoundedRectangle(area, 2.0f);

    constexpr int bars = 22;
    constexpr float gap = 2.0f;
    const float bar_w = (area.getWidth() - gap * static_cast<float>(bars - 1)) /
                        static_cast<float>(bars);
    const int active_bars =
        juce::jlimit(0, bars, static_cast<int>(std::ceil(amount * bars)));

    for (int i = 0; i < bars; ++i) {
        const auto x = area.getX() + static_cast<float>(i) * (bar_w + gap);
        g.setColour(i < active_bars ? active : border_soft);
        g.fillRoundedRectangle(x, area.getY() + 4.0f, bar_w,
                               area.getHeight() - 8.0f, 1.0f);
    }
}

enum class Icon {
    plus,
    refresh,
    filter,
    search,
    settings,
    chevron,
    network,
    server,
    globe,
    users,
    mic,
    speaker,
    wave,
    wifi,
    lock,
    link
};

juce::String hex_byte(int value) {
    auto hex = juce::String::toHexString(value);
    return hex.length() == 1 ? "0" + hex : hex;
}

juce::String svg_colour(juce::Colour colour) {
    return "#" + hex_byte(colour.getRed()) + hex_byte(colour.getGreen()) +
           hex_byte(colour.getBlue());
}

const char* icon_svg_body(Icon icon) {
    switch (icon) {
        case Icon::plus:
            return R"(<path d="M5 12h14"/><path d="M12 5v14"/>)";
        case Icon::refresh:
            return R"(<path d="M21 12a9 9 0 1 1-2.64-6.36L21 8"/><path d="M21 3v5h-5"/>)";
        case Icon::filter:
            return R"(<path d="M3 4h18l-7 8v7l-4 2v-9z"/>)";
        case Icon::search:
            return R"(<circle cx="11" cy="11" r="8"/><path d="m21 21-4.35-4.35"/>)";
        case Icon::settings:
            return R"(<path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.38a2 2 0 0 0-.73-2.73l-.15-.09a2 2 0 0 1-1-1.74v-.51a2 2 0 0 1 1-1.72l.15-.1a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z"/><circle cx="12" cy="12" r="3"/>)";
        case Icon::chevron:
            return R"(<path d="m6 9 6 6 6-6"/>)";
        case Icon::network:
            return R"(<rect x="9" y="2" width="6" height="6" rx="1"/><rect x="2" y="16" width="6" height="6" rx="1"/><rect x="16" y="16" width="6" height="6" rx="1"/><path d="M12 8v4"/><path d="M5 16v-2a2 2 0 0 1 2-2h10a2 2 0 0 1 2 2v2"/>)";
        case Icon::server:
            return R"(<rect x="2" y="3" width="20" height="7" rx="2"/><rect x="2" y="14" width="20" height="7" rx="2"/><path d="M6 6.5h.01"/><path d="M6 17.5h.01"/>)";
        case Icon::globe:
            return R"(<circle cx="12" cy="12" r="10"/><path d="M2 12h20"/><path d="M12 2a15.3 15.3 0 0 1 0 20"/><path d="M12 2a15.3 15.3 0 0 0 0 20"/>)";
        case Icon::users:
            return R"(<path d="M16 21v-2a4 4 0 0 0-4-4H6a4 4 0 0 0-4 4v2"/><circle cx="9" cy="7" r="4"/><path d="M22 21v-2a4 4 0 0 0-3-3.87"/><path d="M16 3.13a4 4 0 0 1 0 7.75"/>)";
        case Icon::mic:
            return R"(<path d="M12 2a3 3 0 0 0-3 3v7a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3z"/><path d="M19 10v2a7 7 0 0 1-14 0v-2"/><path d="M12 19v3"/>)";
        case Icon::speaker:
            return R"(<path d="M11 5 6 9H2v6h4l5 4z"/><path d="M15.54 8.46a5 5 0 0 1 0 7.07"/>)";
        case Icon::wave:
            return R"(<path d="M22 12h-4l-3 8L9 4l-3 8H2"/>)";
        case Icon::wifi:
            return R"(<path d="M5 13a10 10 0 0 1 14 0"/><path d="M8.5 16.5a5 5 0 0 1 7 0"/><path d="M12 20h.01"/>)";
        case Icon::lock:
            return R"(<rect x="3" y="11" width="18" height="11" rx="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/>)";
        case Icon::link:
            return R"(<path d="M10 13a5 5 0 0 0 7.07 0l2.12-2.12a5 5 0 0 0-7.07-7.07L11 4.93"/><path d="M14 11a5 5 0 0 0-7.07 0L4.81 13.12a5 5 0 0 0 7.07 7.07L13 19.07"/>)";
    }
    return "";
}

void draw_icon(juce::Graphics& g, juce::Rectangle<int> bounds, Icon icon,
               juce::Colour colour = text_dim) {
    const auto svg =
        juce::String(R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke=")") +
        svg_colour(colour) +
        R"(" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">)" +
        icon_svg_body(icon) + "</svg>";

    if (auto xml = juce::parseXML(svg)) {
        if (auto drawable = juce::Drawable::createFromSVG(*xml)) {
            drawable->drawWithin(g, bounds.toFloat().reduced(2.0f),
                                 juce::RectanglePlacement::centred, 1.0f);
        }
    }
}

void draw_toolbar_button(juce::Graphics& g, juce::Rectangle<int> bounds,
                         const juce::String& label, Icon icon) {
    fill_button(g, bounds);
    const auto label_font = font(14.0f);
    const int label_width =
        juce::GlyphArrangement::getStringWidthInt(label_font, label);
    constexpr int icon_size = 22;
    constexpr int gap = 7;
    const int group_width = icon_size + gap + label_width;
    auto group = juce::Rectangle<int>{0, 0, group_width, bounds.getHeight()}
                     .withCentre(bounds.getCentre());
    draw_icon(g, group.removeFromLeft(icon_size).reduced(2), icon);
    group.removeFromLeft(gap);
    draw_label(g, group, label, 14.0f, text_dim);
}

void draw_dropdown(juce::Graphics& g, juce::Rectangle<int> bounds,
                   const juce::String& label, Icon icon) {
    auto area = bounds.toFloat().reduced(0.5f);
    g.setColour(row);
    g.fillRoundedRectangle(area, radius);
    g.setColour(border);
    g.drawRoundedRectangle(area, radius, 1.0f);
    draw_icon(g, bounds.removeFromLeft(42).reduced(9), icon);
    draw_label(g, bounds.removeFromLeft(bounds.getWidth() - 28), label, 14.0f,
               text);
    draw_icon(g, bounds.reduced(5), Icon::chevron);
}

void draw_scrollbar(juce::Graphics& g, juce::Rectangle<int> bounds,
                    int content_height, int scroll_px) {
    if (bounds.isEmpty() || content_height <= bounds.getHeight()) {
        return;
    }

    auto track = bounds.withLeft(bounds.getRight() - 6).reduced(1, 5);
    if (track.getHeight() <= 0) {
        return;
    }

    const int max_scroll = content_height - bounds.getHeight();
    const int thumb_height = juce::jlimit(
        24, track.getHeight(),
        static_cast<int>(std::round(
            static_cast<double>(track.getHeight()) *
            static_cast<double>(bounds.getHeight()) /
            static_cast<double>(content_height))));
    const int travel = track.getHeight() - thumb_height;
    const int thumb_y =
        track.getY() +
        (max_scroll <= 0
             ? 0
             : static_cast<int>(std::round(
                   static_cast<double>(travel) *
                   static_cast<double>(juce::jlimit(0, max_scroll, scroll_px)) /
                   static_cast<double>(max_scroll))));

    g.setColour(border_soft.withAlpha(0.7f));
    g.fillRoundedRectangle(track.toFloat(), 2.0f);
    g.setColour(text_faint.withAlpha(0.75f));
    g.fillRoundedRectangle(
        juce::Rectangle<int>{track.getX(), thumb_y, track.getWidth(), thumb_height}
            .toFloat(),
        2.0f);
}
} // namespace ui

struct RoomColumns {
    int name;
    int players;
    int access;
    int join;
};

RoomColumns room_columns(int width) {
    auto players = 145;
    auto access = 165;
    auto join = 180;
    auto name = width - players - access - join;

    if (name < 300) {
        const auto deficit = 300 - name;
        access -= juce::jmin(deficit / 3, 30);
        join -= juce::jmin(deficit / 3, 35);
        name = width - players - access - join;
    }

    const auto final_name = juce::jmax(240, name);
    return {final_name, players, access,
            juce::jmax(132, width - final_name - players - access)};
}

std::string trim_copy(std::string value) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

std::string lowercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string normalized_id(std::string value, const std::string& fallback) {
    value = trim_copy(lowercase_copy(std::move(value)));
    std::string out;
    out.reserve(value.size());
    bool pending_dash = false;
    for (unsigned char c: value) {
        if (std::isalnum(c) || c == '_' || c == '.') {
            if (pending_dash && !out.empty()) {
                out.push_back('-');
            }
            pending_dash = false;
            out.push_back(static_cast<char>(c));
        } else if (c == '-' || std::isspace(c)) {
            pending_dash = true;
        }
        if (out.size() >= 63) {
            break;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out.empty() ? fallback : out;
}

std::string make_local_profile_id() {
    auto nonce = performer_join_token::random_nonce();
    if (nonce.size() > 12) {
        nonce.resize(12);
    }
    return "guest-" + nonce;
}

bool is_local_address(const std::string& address) {
    const auto value = lowercase_copy(address);
    return value == "127.0.0.1" || value == "localhost" || value == "::1";
}

std::string normalized_endpoint_address(std::string address) {
    return lowercase_copy(trim_copy(std::move(address)));
}

bool same_endpoint(const std::string& left_address, uint16_t left_port,
                   const std::string& right_address, uint16_t right_port) {
    return left_port == right_port &&
           normalized_endpoint_address(left_address) ==
               normalized_endpoint_address(right_address);
}

juce::String endpoint_label(const std::string& address, uint16_t port) {
    return juce::String(address) + ":" + juce::String(port);
}

std::string string_property(juce::DynamicObject& object, const char* key) {
    return trim_copy(object.getProperty(key).toString().toStdString());
}

struct ParsedOfficialRoomServers {
    bool valid = false;
    std::vector<SavedRoomServer> servers;
};

ParsedOfficialRoomServers parse_official_room_servers(const juce::var& root) {
    ParsedOfficialRoomServers parsed;
    auto* root_object = root.getDynamicObject();
    if (root_object == nullptr) {
        spdlog::warn("Official room server JSON is not an object");
        return parsed;
    }

    const auto servers_value = root_object->getProperty("servers");
    const auto* servers = servers_value.getArray();
    if (servers == nullptr) {
        spdlog::warn("Official room server fetch returned no servers array");
        return parsed;
    }

    parsed.valid = true;
    for (const auto& server_value: *servers) {
        auto* server_object = server_value.getDynamicObject();
        if (server_object == nullptr) {
            continue;
        }

        const auto enabled = server_object->getProperty("enabled");
        if (!enabled.isVoid() && !static_cast<bool>(enabled)) {
            continue;
        }

        SavedRoomServer server;
        server.name = string_property(*server_object, "name");
        server.address = string_property(*server_object, "address");
        const int port = static_cast<int>(server_object->getProperty("port"));
        if (server.address.empty() || port <= 0 || port > 65535) {
            continue;
        }
        server.port = static_cast<uint16_t>(port);
        parsed.servers.push_back(std::move(server));
    }
    spdlog::info("Parsed {} enabled official room server(s)",
                 parsed.servers.size());
    return parsed;
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

juce::String status_text_for_code(uint8_t status, const std::string& reason) {
    if (!reason.empty()) {
        return reason;
    }
    switch (status) {
        case ROOM_STATUS_BAD_REQUEST:
            return "Bad request";
        case ROOM_STATUS_NOT_FOUND:
            return "Room not found";
        case ROOM_STATUS_FORBIDDEN:
            return "Wrong password";
        case ROOM_STATUS_CONFLICT:
            return "Room already exists";
        case ROOM_STATUS_SERVER_ERROR:
            return "Server error";
        default:
            return "Request failed";
    }
}

bool room_access_requires_password(uint8_t access_mode) {
    return access_mode == ROOM_ACCESS_PASSWORD;
}

juce::String room_access_label(uint8_t access_mode) {
    switch (access_mode) {
        case ROOM_ACCESS_PASSWORD:
            return "Password";
        case ROOM_ACCESS_APPROVE:
            return "Approve";
        default:
            return "Open";
    }
}

juce::Colour room_access_colour(uint8_t access_mode) {
    switch (access_mode) {
        case ROOM_ACCESS_PASSWORD:
        case ROOM_ACCESS_APPROVE:
            return ui::amber;
        default:
            return ui::green;
    }
}

juce::String format_ping(double ms) {
    if (ms <= 0.0) {
        return "...";
    }
    if (ms < 10.0) {
        return juce::String(ms, 1) + " ms";
    }
    return juce::String(static_cast<int>(std::lround(ms))) + " ms";
}

juce::Colour ping_colour(double ms, bool ok) {
    if (!ok) {
        return ui::text_faint;
    }
    if (ms < 45.0) {
        return ui::green;
    }
    if (ms < 100.0) {
        return ui::amber;
    }
    return ui::red;
}

bool matches_api(const AudioStream::DeviceInfo& device, const std::string& api_name) {
    return api_name.empty() || api_name == "All" || device.api_name == api_name;
}

bool device_matches_api(AudioStream::DeviceIndex device_id, const std::string& api_name) {
    const auto* info = AudioStream::get_device_info(device_id);
    return info != nullptr && matches_api(*info, api_name);
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

void draw_header_metric(juce::Graphics& g, juce::Rectangle<int> bounds,
                        const juce::String& label, const juce::String& value,
                        juce::Colour value_colour = juce_theme::colour::text()) {
    g.setColour(juce_theme::colour::border_soft());
    g.drawVerticalLine(bounds.getX(), bounds.getY() + 4.0F,
                       bounds.getBottom() - 4.0F);

    auto text = bounds.reduced(18, 8);
    g.setFont(juce_theme::font(12.0F));
    g.setColour(juce_theme::colour::text_faint());
    g.drawFittedText(label, text.removeFromTop(17),
                     juce::Justification::centredLeft, 1);
    g.setFont(juce_theme::font(15.0F, true));
    g.setColour(value_colour);
    g.drawFittedText(value, text, juce::Justification::centredLeft, 1);
}

std::string read_required_text(juce::AlertWindow& alert, const char* editor_name) {
    return trim_copy(alert.getTextEditorContents(editor_name).toStdString());
}

void style_dialog_editor(juce::TextEditor& editor) {
    juce_theme::style_editor(editor, 14.0F);
    editor.setColour(juce::TextEditor::backgroundColourId, juce_theme::colour::row());
    editor.setColour(juce::CaretComponent::caretColourId,
                     juce_theme::colour::accent_hi());
    editor.setColour(juce::TextEditor::highlightColourId,
                     juce_theme::colour::accent_hi().withAlpha(0.82F));
    editor.setColour(juce::TextEditor::highlightedTextColourId,
                     juce_theme::colour::background());
}

void style_dialog_editors(juce::AlertWindow& alert,
                          std::initializer_list<const char*> editor_names) {
    for (const auto* name: editor_names) {
        if (auto* editor = alert.getTextEditor(name)) {
            style_dialog_editor(*editor);
        }
    }
}

uint8_t access_mode_from_create_combo(int selected_id) {
    switch (selected_id) {
        case 2:
            return ROOM_ACCESS_PASSWORD;
        case 3:
            return ROOM_ACCESS_APPROVE;
        default:
            return ROOM_ACCESS_OPEN;
    }
}

struct CreateRoomInput {
    std::string room_name;
    std::string display_name;
    std::string password;
    uint8_t access_mode = ROOM_ACCESS_OPEN;
};

struct AddServerInput {
    std::string name;
    std::string address;
    uint16_t port = 0;
};

class CreateRoomDialog final : public juce::Component {
public:
    CreateRoomDialog(juce::String display_name,
                     std::function<void(CreateRoomInput)> on_create)
        : on_create_(std::move(on_create)) {
        setSize(420, 320);

        room_label_.setText("Room", juce::dontSendNotification);
        name_label_.setText("Name", juce::dontSendNotification);
        access_label_.setText("Access", juce::dontSendNotification);
        password_label_.setText("Password", juce::dontSendNotification);
        status_label_.setText({}, juce::dontSendNotification);
        for (auto* label: {&room_label_, &name_label_, &access_label_,
                           &password_label_}) {
            juce_theme::style_label(*label, juce_theme::colour::text(), 12.5F);
        }
        juce_theme::style_label(status_label_, juce_theme::colour::warning(), 12.0F);

        room_editor_.setText("Untitled Room", juce::dontSendNotification);
        name_editor_.setText(display_name, juce::dontSendNotification);
        password_editor_.setTextToShowWhenEmpty("Room password",
                                                juce_theme::colour::text_faint());
        for (auto* editor: {&room_editor_, &name_editor_, &password_editor_}) {
            style_dialog_editor(*editor);
        }

        access_combo_.addItem("Open", 1);
        access_combo_.addItem("Password", 2);
        access_combo_.addItem("Approve", 3);
        access_combo_.setSelectedId(1, juce::dontSendNotification);
        access_combo_.onChange = [this]() { update_password_visibility(); };

        create_button_.setButtonText("Create");
        cancel_button_.setButtonText("Cancel");
        create_button_.onClick = [this]() { submit(); };
        cancel_button_.onClick = [this]() { close(); };

        addAndMakeVisible(room_label_);
        addAndMakeVisible(room_editor_);
        addAndMakeVisible(name_label_);
        addAndMakeVisible(name_editor_);
        addAndMakeVisible(access_label_);
        addAndMakeVisible(access_combo_);
        addAndMakeVisible(password_label_);
        addAndMakeVisible(password_editor_);
        addAndMakeVisible(status_label_);
        addAndMakeVisible(create_button_);
        addAndMakeVisible(cancel_button_);
        update_password_visibility();
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce_theme::colour::panel_bottom());
    }

    void resized() override {
        auto area = getLocalBounds().reduced(34, 20);
        auto buttons = area.removeFromBottom(40);
        area.removeFromBottom(10);

        layout_text_row(area, room_label_, room_editor_);
        area.removeFromTop(8);
        layout_text_row(area, name_label_, name_editor_);
        area.removeFromTop(8);
        layout_combo_row(area, access_label_, access_combo_);
        if (password_label_.isVisible()) {
            area.removeFromTop(8);
            layout_text_row(area, password_label_, password_editor_);
        }
        status_label_.setBounds(area.removeFromTop(22));

        const int button_width = 86;
        const int gap = 16;
        const int total = button_width * 2 + gap;
        buttons = buttons.withSizeKeepingCentre(total, 34);
        create_button_.setBounds(buttons.removeFromLeft(button_width).reduced(2));
        buttons.removeFromLeft(gap);
        cancel_button_.setBounds(buttons.removeFromLeft(button_width).reduced(2));
    }

private:
    static void layout_text_row(juce::Rectangle<int>& area, juce::Label& label,
                                juce::TextEditor& editor) {
        label.setBounds(area.removeFromTop(18));
        editor.setBounds(area.removeFromTop(28));
    }

    static void layout_combo_row(juce::Rectangle<int>& area, juce::Label& label,
                                 juce::ComboBox& combo) {
        label.setBounds(area.removeFromTop(18));
        combo.setBounds(area.removeFromTop(30));
    }

    void update_password_visibility() {
        const bool password_mode =
            access_mode_from_create_combo(access_combo_.getSelectedId()) ==
            ROOM_ACCESS_PASSWORD;
        password_label_.setVisible(password_mode);
        password_editor_.setVisible(password_mode);
        password_editor_.setEnabled(password_mode);
        if (!password_mode) {
            password_editor_.clear();
        }
        resized();
    }

    void submit() {
        CreateRoomInput input;
        input.room_name = trim_copy(room_editor_.getText().toStdString());
        input.display_name = trim_copy(name_editor_.getText().toStdString());
        input.password = password_editor_.getText().toStdString();
        input.access_mode = access_mode_from_create_combo(access_combo_.getSelectedId());

        if (input.room_name.empty() || input.display_name.empty()) {
            status_label_.setText("Room and name are required",
                                  juce::dontSendNotification);
            return;
        }
        if (input.access_mode == ROOM_ACCESS_PASSWORD && input.password.empty()) {
            status_label_.setText("Password is required for password rooms",
                                  juce::dontSendNotification);
            return;
        }
        if (on_create_) {
            on_create_(std::move(input));
        }
        close();
    }

    void close() {
        if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>()) {
            dialog->exitModalState(0);
        }
    }

    std::function<void(CreateRoomInput)> on_create_;
    juce::Label room_label_;
    juce::TextEditor room_editor_;
    juce::Label name_label_;
    juce::TextEditor name_editor_;
    juce::Label access_label_;
    juce::ComboBox access_combo_;
    juce::Label password_label_;
    juce::TextEditor password_editor_;
    juce::Label status_label_;
    juce::TextButton create_button_;
    juce::TextButton cancel_button_;
};

class OfficialServerFetchLinkButton final : public juce::Button {
public:
    OfficialServerFetchLinkButton() : juce::Button("Fetch official servers") {
        setButtonText("Fetch official servers");
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    void paintButton(juce::Graphics& g, bool highlighted, bool down) override {
        auto area = getLocalBounds();
        g.setFont(juce_theme::font(12.5F, false));
        g.setColour(down ? juce_theme::colour::accent()
                         : highlighted ? juce_theme::colour::text_dim()
                                       : juce_theme::colour::text_faint());
        g.drawFittedText(getButtonText(), area, juce::Justification::centred, 1);
    }
};

class AddServerDialog final : public juce::Component {
public:
    AddServerDialog(std::function<void(AddServerInput)> on_add,
                    std::function<void()> on_fetch_official)
        : on_add_(std::move(on_add)),
          on_fetch_official_(std::move(on_fetch_official)) {
        setSize(370, 294);

        name_label_.setText("Name", juce::dontSendNotification);
        address_label_.setText("Address", juce::dontSendNotification);
        port_label_.setText("Port", juce::dontSendNotification);
        status_label_.setText({}, juce::dontSendNotification);
        for (auto* label: {&name_label_, &address_label_, &port_label_}) {
            juce_theme::style_label(*label, juce_theme::colour::text(), 12.5F);
        }
        juce_theme::style_label(status_label_, juce_theme::colour::warning(), 12.0F);

        address_editor_.setText("127.0.0.1", juce::dontSendNotification);
        port_editor_.setText("9999", juce::dontSendNotification);
        port_editor_.setInputRestrictions(5, "0123456789");
        for (auto* editor: {&name_editor_, &address_editor_, &port_editor_}) {
            style_dialog_editor(*editor);
        }

        add_button_.setButtonText("Add");
        cancel_button_.setButtonText("Cancel");
        add_button_.onClick = [this]() { submit(); };
        cancel_button_.onClick = [this]() { close(); };
        fetch_link_.onClick = [this]() {
            if (on_fetch_official_) {
                on_fetch_official_();
            }
            close();
        };

        addAndMakeVisible(name_label_);
        addAndMakeVisible(name_editor_);
        addAndMakeVisible(address_label_);
        addAndMakeVisible(address_editor_);
        addAndMakeVisible(port_label_);
        addAndMakeVisible(port_editor_);
        addAndMakeVisible(status_label_);
        addAndMakeVisible(add_button_);
        addAndMakeVisible(cancel_button_);
        addAndMakeVisible(fetch_link_);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce_theme::colour::panel_bottom());
    }

    void resized() override {
        auto area = getLocalBounds().reduced(34, 20);

        layout_text_row(area, name_label_, name_editor_);
        area.removeFromTop(8);
        layout_text_row(area, address_label_, address_editor_);
        area.removeFromTop(8);
        layout_text_row(area, port_label_, port_editor_);
        status_label_.setBounds(area.removeFromTop(20));
        area.removeFromTop(8);

        const int button_width = 86;
        const int gap = 16;
        const int total = button_width * 2 + gap;
        auto buttons = area.removeFromTop(38);
        buttons = buttons.withSizeKeepingCentre(total, 34);
        add_button_.setBounds(buttons.removeFromLeft(button_width).reduced(2));
        buttons.removeFromLeft(gap);
        cancel_button_.setBounds(buttons.removeFromLeft(button_width).reduced(2));

        area.removeFromTop(4);
        fetch_link_.setBounds(area.removeFromTop(24).withSizeKeepingCentre(190, 24));
    }

private:
    static void layout_text_row(juce::Rectangle<int>& area, juce::Label& label,
                                juce::TextEditor& editor) {
        label.setBounds(area.removeFromTop(18));
        editor.setBounds(area.removeFromTop(28));
    }

    void submit() {
        AddServerInput input;
        input.name = trim_copy(name_editor_.getText().toStdString());
        input.address = trim_copy(address_editor_.getText().toStdString());
        const int port = port_editor_.getText().getIntValue();
        if (input.address.empty() || port <= 0 ||
            port > std::numeric_limits<uint16_t>::max()) {
            status_label_.setText("Server address and port are required",
                                  juce::dontSendNotification);
            return;
        }

        input.port = static_cast<uint16_t>(port);
        if (on_add_) {
            on_add_(std::move(input));
        }
        close();
    }

    void close() {
        if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>()) {
            dialog->exitModalState(0);
        }
    }

    std::function<void(AddServerInput)> on_add_;
    std::function<void()> on_fetch_official_;
    juce::Label name_label_;
    juce::TextEditor name_editor_;
    juce::Label address_label_;
    juce::TextEditor address_editor_;
    juce::Label port_label_;
    juce::TextEditor port_editor_;
    juce::Label status_label_;
    juce::TextButton add_button_;
    juce::TextButton cancel_button_;
    OfficialServerFetchLinkButton fetch_link_;
};

template <typename Request, typename Response>
Response send_control_request(const std::string& address, uint16_t port,
                              const Request& request, CtrlHdr::Cmd expected_type,
                              uint32_t request_id, double& round_trip_ms) {
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

    const auto sent_at = std::chrono::steady_clock::now();
    socket.send_to(asio::buffer(&request, sizeof(request)), endpoint);

    std::array<unsigned char, 2048> buffer{};
    udp::endpoint sender;
    const auto deadline = sent_at + 1100ms;
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
            const auto received_at = std::chrono::steady_clock::now();
            round_trip_ms =
                std::chrono::duration<double, std::milli>(received_at - sent_at)
                    .count();
            return response;
        }
        if (ec != asio::error::would_block && ec != asio::error::try_again) {
            throw std::runtime_error(ec.message());
        }
        std::this_thread::sleep_for(1ms);
    }
    throw std::runtime_error("server did not respond");
}
} // namespace

JuceRoomBrowserComponent::MonitorToggleButton::MonitorToggleButton()
    : juce::Button("Monitor") {
    setClickingTogglesState(true);
    setButtonText("Monitor off");
}

void JuceRoomBrowserComponent::MonitorToggleButton::paintButton(
    juce::Graphics& g, bool highlighted, bool down) {
    auto area = getLocalBounds().toFloat().reduced(0.5f);
    const bool active = getToggleState();
    const bool enabled = isEnabled();

    juce::Colour fill = active ? ui::row_selected : (highlighted ? ui::row_hover : ui::row);
    if (!enabled) {
        fill = juce::Colour{0xff17191b};
    } else if (down) {
        fill = juce::Colour{0xff1d1f21};
    }
    g.setColour(fill);
    g.fillRoundedRectangle(area, ui::radius);
    g.setColour(enabled ? (active ? ui::accent : ui::border) : ui::border_soft);
    g.drawRoundedRectangle(area, ui::radius, 1.0f);

    auto content = getLocalBounds().reduced(9, 0);
    ui::draw_icon(g, content.removeFromLeft(26).reduced(2), ui::Icon::wave,
                  active ? ui::green : ui::text_dim);
    content.removeFromLeft(5);
    ui::draw_label(g, content, getButtonText(), 13.5f,
                   enabled ? ui::text : ui::text_faint, true);
}

JuceRoomBrowserComponent::JuceRoomBrowserComponent(
    ClientAppFacade& client, JuceClientStartupOptions startup_options,
    std::function<void(JoinLaunch)> joined_callback)
    : client_(client),
      startup_options_(std::move(startup_options)),
      joined_callback_(std::move(joined_callback)),
      local_profile_id_(make_local_profile_id()) {
    configure_controls();
    const auto saved_display_name =
        load_client_display_name(startup_options_.config_path);
    if (!saved_display_name.empty()) {
        last_display_name_ = saved_display_name;
    }
    client_.set_self_monitor_enabled(false);
    if (client_.is_audio_stream_active()) {
        client_.stop_audio_stream();
    }

    load_servers();

    request_audio_device_refresh();
    next_auto_refresh_ = std::chrono::steady_clock::now();
    startTimerHz(30);
}

JuceRoomBrowserComponent::~JuceRoomBrowserComponent() {
    stopTimer();
    search_editor_.setLookAndFeel(nullptr);
    if (job_thread_.joinable()) {
        job_thread_.join();
    }
    official_server_fetch_.join();
    if (audio_device_job_thread_.joinable()) {
        audio_device_job_thread_.join();
    }
}

void JuceRoomBrowserComponent::configure_controls() {
    setOpaque(true);
    search_editor_.setFont(ui::font(14.0f));
    search_editor_.setMultiLine(false);
    search_editor_.setReturnKeyStartsNewLine(false);
    search_editor_.setScrollbarsShown(false);
    search_editor_.setJustification(juce::Justification::centredLeft);
    search_editor_.setBorder(juce::BorderSize<int>(0));
    search_editor_.setIndents(0, 0);
    search_editor_.setColour(juce::TextEditor::textColourId, ui::text);
    search_editor_.setColour(juce::TextEditor::highlightColourId,
                             ui::accent.withAlpha(0.4f));
    search_editor_.setColour(juce::TextEditor::highlightedTextColourId, ui::text);
    search_editor_.setColour(juce::TextEditor::backgroundColourId,
                             juce::Colours::transparentBlack);
    search_editor_.setColour(juce::TextEditor::outlineColourId,
                             juce::Colours::transparentBlack);
    search_editor_.setColour(juce::TextEditor::focusedOutlineColourId,
                             juce::Colours::transparentBlack);
    search_editor_.setTextToShowWhenEmpty("Search rooms", ui::text_faint);
    search_editor_.setLookAndFeel(&search_editor_look_and_feel_);
    search_editor_.onTextChange = [this]() {
        room_scroll_px_ = 0;
        clamp_scroll_offsets();
        repaint();
    };
    addAndMakeVisible(search_editor_);

    api_combo_.setTextWhenNothingSelected("API");
    input_combo_.setTextWhenNothingSelected("Input");
    output_combo_.setTextWhenNothingSelected("Output");
    monitor_toggle_.setButtonText("Monitor off");
    set_audio_preflight_controls_enabled(false);

    api_combo_.onChange = [this]() {
        if (updating_audio_controls_) {
            return;
        }
        const int old_api_index = selected_api_index_;
        const int selected_id = api_combo_.getSelectedId();
        selected_api_index_ = selected_id <= 1 ? -1 : selected_id - 2;
        if (old_api_index != selected_api_index_) {
            pending_input_ = default_device_for_api(input_devices_, available_apis_,
                                                    selected_api_index_, true);
            pending_output_ = default_device_for_api(output_devices_, available_apis_,
                                                     selected_api_index_, false);
            populate_audio_preflight_controls();
            apply_audio_preflight_selection(true);
        }
    };
    input_combo_.onChange = [this]() {
        if (updating_audio_controls_) {
            return;
        }
        pending_input_ = selected_input_device();
        apply_audio_preflight_selection(true);
    };
    output_combo_.onChange = [this]() {
        if (updating_audio_controls_) {
            return;
        }
        pending_output_ = selected_output_device();
        apply_audio_preflight_selection(true);
    };
    monitor_toggle_.onClick = [this]() { start_or_stop_monitor(); };

    addAndMakeVisible(api_combo_);
    addAndMakeVisible(input_combo_);
    addAndMakeVisible(output_combo_);
    addAndMakeVisible(monitor_toggle_);
}

void JuceRoomBrowserComponent::load_servers() {
    servers_.clear();

    auto make_server = [](std::string name, std::string address, uint16_t port) {
        BrowserServer server;
        server.address = address.empty() ? "127.0.0.1" : std::move(address);
        server.port = port == 0 ? 9999 : port;
        server.name = name.empty()
                          ? (is_local_address(server.address) ? "Local Network"
                                                              : server.address)
                          : std::move(name);
        return server;
    };

    auto saved_servers = load_saved_room_servers(startup_options_.config_path);
    room_servers_seeded_ = load_room_servers_seeded(startup_options_.config_path);

    bool saved_servers_changed = false;
    if (saved_servers.empty()) {
        saved_servers.push_back(SavedRoomServer{"Local Network", "127.0.0.1", 9999});
        saved_servers_changed = true;
    }

    if (saved_servers_changed) {
        save_saved_room_servers(startup_options_.config_path, saved_servers);
    }

    for (const auto& saved: saved_servers) {
        servers_.push_back(make_server(saved.name, saved.address, saved.port));
    }

    const bool has_startup_server =
        startup_options_.server_endpoint_explicit &&
        (!startup_options_.server_address.empty() || startup_options_.server_port != 0);
    auto startup_server = make_server(
        {}, startup_options_.server_address,
        startup_options_.server_port == 0 ? 9999 : startup_options_.server_port);

    selected_server_index_ = 0;
    if (has_startup_server) {
        const auto it = std::find_if(
            servers_.begin(), servers_.end(), [&](const BrowserServer& server) {
                return same_endpoint(server.address, server.port,
                                     startup_server.address, startup_server.port);
            });
        if (it != servers_.end()) {
            selected_server_index_ = static_cast<int>(std::distance(servers_.begin(), it));
        } else {
            servers_.insert(servers_.begin(), std::move(startup_server));
            selected_server_index_ = 0;
        }
    }

    if (!room_servers_seeded_) {
        request_official_server_refresh();
    }
}

void JuceRoomBrowserComponent::save_servers(
    std::optional<bool> room_servers_seeded) const {
    std::vector<SavedRoomServer> saved;
    saved.reserve(servers_.size());
    for (const auto& server: servers_) {
        if (server.address.empty() || server.port == 0) {
            continue;
        }
        saved.push_back(SavedRoomServer{server.name, server.address, server.port});
    }
    save_saved_room_servers(startup_options_.config_path, saved, room_servers_seeded);
}

void JuceRoomBrowserComponent::request_official_server_refresh(bool manual) {
    if (official_server_fetch_.running()) {
        if (manual) {
            official_server_fetch_manual_ = true;
            status_text_ = "Fetching official servers...";
            repaint();
        }
        return;
    }

    official_server_fetch_manual_ = manual;
    if (manual) {
        status_text_ = "Fetching official servers...";
        repaint();
    }
    official_server_fetch_.start(OFFICIAL_SERVERS_URL,
                                 OFFICIAL_SERVERS_FETCH_TIMEOUT_MS);
}

void JuceRoomBrowserComponent::poll_official_server_refresh() {
    if (auto result = official_server_fetch_.poll()) {
        const bool manual = official_server_fetch_manual_;
        official_server_fetch_manual_ = false;
        apply_official_server_refresh_result(*result, manual);
    }
}

void JuceRoomBrowserComponent::apply_official_server_refresh_result(
    const HttpJsonFetchResult& result, bool manual) {
    if (!result.ok) {
        if (manual) {
            status_text_ = "Official server fetch failed";
            repaint();
        }
        spdlog::info("No official room servers were added");
        return;
    }

    const auto parsed = parse_official_room_servers(result.json);
    if (!parsed.valid) {
        if (manual) {
            status_text_ = "Official server list invalid";
            repaint();
        }
        spdlog::info("No official room servers were added");
        return;
    }

    if (parsed.servers.empty()) {
        if (!room_servers_seeded_) {
            save_servers(true);
            room_servers_seeded_ = true;
        }
        if (manual) {
            status_text_ = "No official servers found";
            repaint();
        }
        spdlog::info("No official room servers were added");
        return;
    }

    auto make_server = [](std::string name, std::string address, uint16_t port) {
        BrowserServer server;
        server.address = address.empty() ? "127.0.0.1" : std::move(address);
        server.port = port == 0 ? 9999 : port;
        server.name = name.empty()
                          ? (is_local_address(server.address) ? "Local Network"
                                                              : server.address)
                          : std::move(name);
        return server;
    };

    int added = 0;
    for (const auto& official: parsed.servers) {
        if (find_server_endpoint(official.address, official.port).has_value()) {
            continue;
        }
        servers_.push_back(make_server(official.name, official.address, official.port));
        ++added;
    }

    if (added > 0 || !room_servers_seeded_) {
        save_servers(true);
        room_servers_seeded_ = true;
    }
    if (manual) {
        status_text_ =
            added == 0
                ? juce::String("Official servers already in list")
                : juce::String(added) + " official server(s) added";
    }
    spdlog::info("Applied official room server fetch: {} added, {} total", added,
                 servers_.size());
    clamp_scroll_offsets();
    repaint();
}

void JuceRoomBrowserComponent::request_audio_device_refresh() {
    {
        std::lock_guard<std::mutex> lock(audio_device_job_mutex_);
        if (audio_device_job_running_) {
            return;
        }
        audio_device_job_running_ = true;
        audio_device_job_finished_ = false;
        audio_device_job_result_.reset();
    }

    if (audio_device_job_thread_.joinable()) {
        audio_device_job_thread_.join();
    }

    audio_preflight_status_ = "Loading devices...";
    sync_monitor_button_text();
    set_audio_preflight_controls_enabled(false);
    audio_device_job_thread_ = std::thread([this]() {
        auto result = load_audio_devices();
        std::lock_guard<std::mutex> lock(audio_device_job_mutex_);
        audio_device_job_result_ = std::move(result);
        audio_device_job_finished_ = true;
    });
}

JuceRoomBrowserComponent::AudioDeviceRefreshResult
JuceRoomBrowserComponent::load_audio_devices() {
    AudioDeviceRefreshResult result;
    try {
        result.input_devices = AudioStream::get_input_device_stubs();
        result.output_devices = AudioStream::get_output_device_stubs();
        result.available_apis = AudioStream::get_apis();

        const auto& preferences = startup_options_.audio_preferences;
        const bool has_required_api = !startup_options_.required_audio_api.empty();
        std::string target_api =
            has_required_api ? startup_options_.required_audio_api
                             : client_.get_audio_api_filter();
        if (target_api.empty()) {
            target_api = preferences.audio_api.empty() ? "All" : preferences.audio_api;
        }

        result.selected_api_index =
            api_index_for_name(result.available_apis, target_api);
        const auto resolved_api =
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
                result.status = juce::String("No ") + target_api + " duplex devices";
                return result;
            }
        }

        result.pending_input = client_.get_selected_input_device();
        result.pending_output = client_.get_selected_output_device();

        if (!contains_device_id(result.input_devices, result.pending_input)) {
            result.pending_input = AudioStream::NO_DEVICE;
        }
        if (!contains_device_id(result.output_devices, result.pending_output)) {
            result.pending_output = AudioStream::NO_DEVICE;
        }

        if (preferences.loaded && !has_required_api) {
            if (result.pending_input == AudioStream::NO_DEVICE) {
                result.pending_input = find_preferred_audio_device(
                    result.input_devices, preferences.input_device,
                    preferences.input_api, preferences.audio_api);
            }
            if (result.pending_output == AudioStream::NO_DEVICE) {
                result.pending_output = find_preferred_audio_device(
                    result.output_devices, preferences.output_device,
                    preferences.output_api, preferences.audio_api);
            }
        }

        if (result.pending_input == AudioStream::NO_DEVICE ||
            !device_matches_api(result.pending_input, resolved_api)) {
            result.pending_input = default_device_for_api(
                result.input_devices, result.available_apis, result.selected_api_index, true);
        }
        if (result.pending_output == AudioStream::NO_DEVICE ||
            !device_matches_api(result.pending_output, resolved_api)) {
            result.pending_output = default_device_for_api(
                result.output_devices, result.available_apis, result.selected_api_index,
                false);
        }

        client_.set_audio_api_filter(has_required_api ? target_api : resolved_api);
        const bool input_ok = result.pending_input != AudioStream::NO_DEVICE &&
                              client_.set_input_device(result.pending_input);
        const bool output_ok = result.pending_output != AudioStream::NO_DEVICE &&
                               client_.set_output_device(result.pending_output);
        if (input_ok && startup_options_.startup_input_channel_index.has_value()) {
            client_.set_input_channel_index(*startup_options_.startup_input_channel_index);
        }

        if (result.input_devices.empty() || result.output_devices.empty()) {
            result.status = "No audio devices";
        } else if (!input_ok || !output_ok) {
            result.status = "Choose devices";
        } else {
            result.status = "Monitor off";
        }
    } catch (const std::exception& e) {
        result.status = "Device load failed: " + juce::String(e.what());
    }
    return result;
}

void JuceRoomBrowserComponent::poll_audio_device_refresh() {
    std::optional<AudioDeviceRefreshResult> result;
    {
        std::lock_guard<std::mutex> lock(audio_device_job_mutex_);
        if (!audio_device_job_finished_) {
            return;
        }
        result = std::move(audio_device_job_result_);
        audio_device_job_result_.reset();
        audio_device_job_finished_ = false;
        audio_device_job_running_ = false;
    }

    if (audio_device_job_thread_.joinable()) {
        audio_device_job_thread_.join();
    }
    if (result.has_value()) {
        apply_audio_device_refresh_result(std::move(*result));
    }
}

void JuceRoomBrowserComponent::apply_audio_device_refresh_result(
    AudioDeviceRefreshResult result) {
    input_devices_ = std::move(result.input_devices);
    output_devices_ = std::move(result.output_devices);
    available_apis_ = std::move(result.available_apis);
    pending_input_ = result.pending_input;
    pending_output_ = result.pending_output;
    selected_api_index_ = result.selected_api_index;
    audio_preflight_status_ = result.status;
    audio_controls_loaded_ = !input_devices_.empty() && !output_devices_.empty();
    populate_audio_preflight_controls();
    set_audio_preflight_controls_enabled(audio_controls_loaded_);
    repaint();
}

void JuceRoomBrowserComponent::populate_audio_preflight_controls() {
    updating_audio_controls_ = true;

    api_combo_.clear(juce::dontSendNotification);
    api_combo_.addItem("All APIs", 1);
    for (const auto& api: available_apis_) {
        api_combo_.addItem(api.name, api.index + 2);
    }
    api_combo_.setSelectedId(selected_api_index_ < 0 ? 1 : selected_api_index_ + 2,
                             juce::dontSendNotification);

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
    monitor_toggle_.setToggleState(
        client_.get_self_monitor_enabled() && client_.is_audio_stream_active(),
        juce::dontSendNotification);
    sync_monitor_button_text();

    updating_audio_controls_ = false;
}

void JuceRoomBrowserComponent::set_audio_preflight_controls_enabled(bool enabled) {
    api_combo_.setEnabled(enabled);
    input_combo_.setEnabled(enabled);
    output_combo_.setEnabled(enabled);
    monitor_toggle_.setEnabled(enabled);
}

void JuceRoomBrowserComponent::apply_audio_preflight_selection(bool restart_monitor) {
    if (!audio_controls_loaded_) {
        return;
    }
    client_.set_audio_api_filter(selected_api_name().toStdString());
    const bool input_ok = pending_input_ != AudioStream::NO_DEVICE &&
                          client_.set_input_device(pending_input_);
    const bool output_ok = pending_output_ != AudioStream::NO_DEVICE &&
                           client_.set_output_device(pending_output_);
    if (input_ok && output_ok) {
        ::save_audio_device_preferences(
            startup_options_.config_path, selected_api_name().toStdString(),
            pending_input_, pending_output_, client_.get_audio_config().input_channel_index);
    }

    const bool should_monitor = monitor_toggle_.getToggleState();
    if (!should_monitor) {
        client_.set_self_monitor_enabled(false);
        if (client_.is_audio_stream_active()) {
            client_.stop_audio_stream();
        }
        audio_preflight_status_ = input_ok && output_ok ? "Monitor off" : "Choose devices";
        sync_monitor_button_text();
        repaint();
        return;
    }

    if (!input_ok || !output_ok) {
        audio_preflight_status_ = "Choose devices";
        monitor_toggle_.setToggleState(false, juce::dontSendNotification);
        client_.set_self_monitor_enabled(false);
        if (client_.is_audio_stream_active()) {
            client_.stop_audio_stream();
        }
        sync_monitor_button_text();
        repaint();
        return;
    }

    if (restart_monitor && client_.is_audio_stream_active()) {
        client_.stop_audio_stream();
    }
    client_.set_self_monitor_enabled(true);
    if (!client_.is_audio_stream_active()) {
        auto config = client_.get_audio_config();
        if (!client_.start_audio_stream(pending_input_, pending_output_, config)) {
            audio_preflight_status_ = AudioStream::get_last_error().empty()
                                          ? "Monitor failed"
                                          : juce::String(AudioStream::get_last_error());
            monitor_toggle_.setToggleState(false, juce::dontSendNotification);
            client_.set_self_monitor_enabled(false);
            sync_monitor_button_text();
            repaint();
            return;
        }
    }
    audio_preflight_status_ = "Monitoring";
    sync_monitor_button_text();
    repaint();
}

void JuceRoomBrowserComponent::start_or_stop_monitor() {
    if (updating_audio_controls_) {
        return;
    }
    if (!monitor_toggle_.getToggleState()) {
        client_.set_self_monitor_enabled(false);
        if (client_.is_audio_stream_active()) {
            client_.stop_audio_stream();
        }
        audio_preflight_status_ = audio_controls_loaded_ ? "Monitor off" : "Loading devices...";
        sync_monitor_button_text();
        repaint();
        return;
    }
    apply_audio_preflight_selection(true);
}

void JuceRoomBrowserComponent::sync_monitor_button_text() {
    monitor_toggle_.setButtonText(monitor_toggle_.getToggleState() ? "Monitoring"
                                                                   : audio_preflight_status_);
}

void JuceRoomBrowserComponent::paint(juce::Graphics& g) {
    g.fillAll(juce_theme::colour::background());

    auto area = getLocalBounds().reduced(CONTENT_PAD);
    auto browser_header = area.removeFromTop(BROWSER_HEADER_HEIGHT);
    juce_theme::paint_panel(g, browser_header);

    auto header_area = browser_header.reduced(18, 0);
    auto brand = header_area.removeFromLeft(126);
    juce_theme::draw_wordmark(g, brand.reduced(0, 12), 27.0F);

    const auto server = selected_server();
    const auto& status = selected_status();
    const bool header_server_ok = status.ok;
    draw_header_metric(g, header_area.removeFromLeft(176), "Server",
                       server.name.empty() ? "-" : juce::String(server.name));
    draw_header_metric(g, header_area.removeFromLeft(178), "Endpoint",
                       server.port == 0 ? "-" : endpoint_label(server.address, server.port));
    draw_header_metric(g, header_area.removeFromLeft(116), "Status",
                       header_server_ok ? "Online" : "Offline",
                       header_server_ok ? juce_theme::colour::success()
                                        : juce_theme::colour::text_dim());
    draw_header_metric(g, header_area.removeFromLeft(112), "Ping",
                       header_server_ok ? format_ping(status.round_trip_ms) : "-",
                       ping_colour(status.round_trip_ms, header_server_ok));
    draw_header_metric(g, header_area.removeFromLeft(104), "Rooms",
                       juce::String(status.total_rooms));
    draw_header_metric(g, header_area.removeFromLeft(104), "Users",
                       juce::String(status.active_participants));

    area.removeFromTop(CONTENT_GAP);
    auto bottom = area.removeFromBottom(BROWSER_BOTTOM_HEIGHT);
    area.removeFromBottom(CONTENT_GAP);
    auto left = area.removeFromLeft(SERVER_RAIL_WIDTH);
    area.removeFromLeft(CONTENT_GAP);
    auto content = area;

    juce_theme::paint_panel(g, left);

    auto server_area = left.reduced(12, 12);
    auto server_header = server_area.removeFromTop(42);
    ui::draw_label(g, server_header.removeFromLeft(120), "SERVERS", 15.0f,
                   ui::text, true);
    refresh_button_bounds_ = server_header.removeFromRight(42).reduced(2);
    ui::fill_button(g, refresh_button_bounds_);
    ui::draw_icon(g, refresh_button_bounds_.reduced(10), ui::Icon::refresh);
    server_header.removeFromRight(8);
    edit_servers_button_bounds_ = server_header.removeFromRight(84).reduced(2);
    ui::draw_toolbar_button(g, edit_servers_button_bounds_, "Edit",
                            ui::Icon::settings);

    add_server_bottom_bounds_ = left.reduced(12, 18).removeFromBottom(42);
    server_list_area_ = server_area.withBottom(
        juce::jmax(server_area.getY(), add_server_bottom_bounds_.getY() - 8));
    clamp_scroll_offsets();

    {
        juce::Graphics::ScopedSaveState save_state(g);
        g.reduceClipRegion(server_list_area_);
        const int first_server =
            juce::jmax(0, server_scroll_px_ / SERVER_ROW_HEIGHT);
        int row_y = server_list_area_.getY() -
                    (server_scroll_px_ % SERVER_ROW_HEIGHT);
        for (int i = first_server;
             i < static_cast<int>(servers_.size()) &&
             row_y + SERVER_ROW_HEIGHT <= server_list_area_.getBottom();
             ++i, row_y += SERVER_ROW_HEIGHT) {
            auto row_bounds =
                juce::Rectangle<int>{server_list_area_.getX(), row_y,
                                     server_list_area_.getWidth(),
                                     SERVER_ROW_HEIGHT};
            auto row_area = row_bounds.reduced(0, 4);
            const bool selected = i == selected_server_index_;
            const auto& server = servers_[static_cast<size_t>(i)];
            if (selected) {
                g.setColour(ui::row_selected);
                g.fillRoundedRectangle(row_area.toFloat(), ui::radius);
            }
            g.setColour(ui::border_soft);
            g.drawHorizontalLine(row_bounds.getBottom() - 1,
                                 static_cast<float>(row_bounds.getX() + 4),
                                 static_cast<float>(row_bounds.getRight() - 4));
            auto row = row_area.reduced(6, 0);
            ui::draw_icon(g, row.removeFromLeft(50).reduced(9),
                          is_local_address(server.address) ? ui::Icon::network
                                                           : ui::Icon::globe,
                          ui::text_dim);
            auto ping = row.removeFromRight(72);
            const auto ping_colour_value =
                ping_colour(server.status.round_trip_ms, server.status.ok);
            auto ping_label = ping.removeFromTop(28).withTrimmedRight(14);
            ui::draw_label(g, ping_label,
                           server.status.ok ? format_ping(server.status.round_trip_ms)
                                            : "offline",
                           14.0f, ping_colour_value, false,
                           juce::Justification::centredRight);
            ui::draw_dot(g, {static_cast<float>(ping.getRight() - 4),
                             static_cast<float>(ping.getY() + 12)},
                         ping_colour_value, 4.0f);
            ui::draw_label(g, row.removeFromTop(30), server.name, 16.0f,
                           ui::text, true);
            ui::draw_label(g, row, endpoint_label(server.address, server.port),
                           13.5f, ui::text_dim);
        }
    }
    ui::draw_scrollbar(g, server_list_area_,
                       static_cast<int>(servers_.size()) * SERVER_ROW_HEIGHT,
                       server_scroll_px_);

    ui::fill_button(g, add_server_bottom_bounds_);
    auto add_label = add_server_bottom_bounds_;
    ui::draw_icon(g, add_label.removeFromLeft(44).reduced(10), ui::Icon::plus);
    ui::draw_label(g, add_label.reduced(2, 0), "Add Server", 14.5f, ui::text_dim);

    juce_theme::paint_panel(g, content);
    auto room_area = content.reduced(12, 10);
    auto top = room_area.removeFromTop(42);
    ui::draw_label(g, top.removeFromLeft(130), "ROOMS", 15.0f, ui::text, true);
    create_button_bounds_ = top.removeFromRight(110).reduced(0, 2);
    ui::fill_button(g, create_button_bounds_, true);
    ui::draw_label(g, create_button_bounds_, "Create", 14.5f, ui::text, true,
                   juce::Justification::centred);
    top.removeFromRight(10);
    join_invite_button_bounds_ = top.removeFromRight(134).reduced(0, 2);
    ui::draw_toolbar_button(g, join_invite_button_bounds_, "Join Invite",
                            ui::Icon::link);
    top.removeFromRight(10);
    auto search_container = top.removeFromRight(285).reduced(0, 2);
    ui::fill_button(g, search_container);
    ui::draw_icon(g, search_container.withWidth(38).reduced(9), ui::Icon::search);
    if (status.ok && status.truncated) {
        ui::draw_label(
            g, top.reduced(8, 0),
            "Showing " + juce::String(static_cast<int>(status.rooms.size())) +
                " of " + juce::String(status.total_rooms),
            13.0f, ui::text_faint);
    }

    room_area.removeFromTop(10);
    auto header = room_area.removeFromTop(42);
    g.setColour(ui::row);
    g.fillRoundedRectangle(header.toFloat(), ui::radius);
    g.setColour(ui::border_soft);
    g.drawRoundedRectangle(header.toFloat().reduced(0.5f), ui::radius, 1.0f);

    auto header_content = header.reduced(20, 0);
    const auto columns = room_columns(header_content.getWidth());
    ui::draw_label(g, header_content.removeFromLeft(columns.name), "ROOM NAME", 12.0f,
                   ui::text_dim, true);
    ui::draw_label(g, header_content.removeFromLeft(columns.players), "PLAYERS", 12.0f,
                   ui::text_dim, true);
    ui::draw_label(g, header_content.removeFromLeft(columns.access), "ACCESS", 12.0f,
                   ui::text_dim, true);
    ui::draw_label(g, header_content, "JOIN", 12.0f, ui::text_dim, true,
                   juce::Justification::centred);

    room_list_area_ = room_area;
    const auto visible = visible_room_indices();
    clamp_scroll_offsets();
    if (!selected_status().ok) {
        ui::draw_label(g, room_area.reduced(20), status_text_, 16.0f, ui::text_dim,
                       false, juce::Justification::centred);
    } else if (visible.empty()) {
        ui::draw_label(g, room_area.reduced(20),
                        selected_status().rooms.empty() ? "No rooms on this server"
                                                        : "No matching rooms",
                        16.0f, ui::text_dim, false, juce::Justification::centred);
    }

    if (!visible.empty()) {
        juce::Graphics::ScopedSaveState save_state(g);
        g.reduceClipRegion(room_list_area_);
        const int first_room = juce::jmax(0, room_scroll_px_ / ROOM_ROW_HEIGHT);
        int row_y = room_list_area_.getY() - (room_scroll_px_ % ROOM_ROW_HEIGHT);
        for (int visible_position = first_room;
             visible_position < static_cast<int>(visible.size()) &&
             row_y + ROOM_ROW_HEIGHT <= room_list_area_.getBottom();
             ++visible_position, row_y += ROOM_ROW_HEIGHT) {
            const int room_index = visible[static_cast<size_t>(visible_position)];
            const auto& room =
                selected_status().rooms[static_cast<size_t>(room_index)];
            auto row_bounds =
                juce::Rectangle<int>{room_list_area_.getX(), row_y,
                                     room_list_area_.getWidth(),
                                     ROOM_ROW_HEIGHT}
                    .reduced(0, 3);
            const bool selected = room_index == selected_room_index_;

            auto row_float = row_bounds.toFloat();
            g.setColour(selected ? ui::row_selected : ui::row);
            g.fillRoundedRectangle(row_float, ui::radius);
            g.setColour(ui::border_soft);
            g.drawRoundedRectangle(row_float.reduced(0.5f), ui::radius, 1.0f);

            auto row = row_bounds.reduced(20, 0);
            const auto row_columns = room_columns(row.getWidth());
            auto name = row.removeFromLeft(row_columns.name);
            auto title = name.removeFromTop(30);
            ui::draw_label(g, title,
                           room.room_name.empty() ? room.room_id : room.room_name,
                           17.0f, ui::text, true);
            if (room.access_mode == ROOM_ACCESS_PASSWORD) {
                ui::draw_icon(g, title.withTrimmedLeft(150).withWidth(22).reduced(3),
                              ui::Icon::lock, ui::text_faint);
            }
            auto tags = name.withHeight(22);
            ui::draw_pill(g, tags.removeFromLeft(70), "Live");
            tags.removeFromLeft(6);
            ui::draw_pill(g, tags.removeFromLeft(86),
                          room_access_label(room.access_mode));

            auto players = row.removeFromLeft(row_columns.players);
            ui::draw_icon(g, players.removeFromLeft(26).reduced(3), ui::Icon::users,
                          ui::text_dim);
            ui::draw_label(g, players, juce::String(room.participant_count), 14.5f,
                           ui::text);
            ui::draw_label(g, row.removeFromLeft(row_columns.access),
                           room_access_label(room.access_mode), 14.0f,
                           room_access_colour(room.access_mode));

            auto join = row.withWidth(row_columns.join);
            const auto menu_width = 44;
            const auto join_gap = 8;
            const auto button_width =
                juce::jlimit(82, 112, join.getWidth() - menu_width - join_gap);
            auto button = join.removeFromLeft(button_width).reduced(0, 13);
            ui::fill_button(g, button, true);
            ui::draw_label(g, button, "Join", 14.5f, ui::text, true,
                           juce::Justification::centred);
            join.removeFromLeft(join_gap);
            auto menu = join.removeFromLeft(menu_width).reduced(0, 13);
            ui::fill_button(g, menu);
            ui::draw_icon(g, menu.reduced(12), ui::Icon::chevron, ui::text_dim);
        }
    }
    ui::draw_scrollbar(g, room_list_area_,
                       static_cast<int>(visible.size()) * ROOM_ROW_HEIGHT,
                       room_scroll_px_);

    auto bottom_area = bottom;
    constexpr int gap = 8;
    const int audio_width =
        juce::jlimit(280, 360, bottom_area.getWidth() * 24 / 100);
    const int api_width =
        juce::jlimit(170, 230, bottom_area.getWidth() * 15 / 100);
    auto audio = bottom_area.removeFromLeft(audio_width);
    bottom_area.removeFromLeft(gap);
    auto api = bottom_area.removeFromLeft(api_width);
    bottom_area.removeFromLeft(gap);
    const int device_width = juce::jmax(220, (bottom_area.getWidth() - gap) / 2);
    auto input = bottom_area.removeFromLeft(device_width);
    bottom_area.removeFromLeft(gap);
    auto output = bottom_area;

    ui::fill_panel(g, audio);
    auto audio_content = audio.reduced(12, 8);
    ui::draw_label(g, audio_content.removeFromTop(16), "AUDIO", 12.0f,
                   ui::text_dim, true);
    audio_content.removeFromTop(6);
    auto audio_row = audio_content.removeFromTop(32);
    audio_row.removeFromLeft(138);
    ui::draw_meter(g, audio_row.reduced(4, 5),
                   juce::jlimit(0.0f, 1.0f, client_.get_own_audio_level()),
                   ui::accent_hi);

    auto draw_control_panel = [&](juce::Rectangle<int> bounds, const char* title,
                                  ui::Icon icon) {
        ui::fill_panel(g, bounds);
        auto content = bounds.reduced(12, 8);
        ui::draw_label(g, content.removeFromTop(16), title, 12.0f, ui::text_dim, true);
        content.removeFromTop(6);
        ui::draw_icon(g, content.removeFromTop(32).removeFromLeft(30).reduced(3),
                      icon, ui::text_dim);
    };
    draw_control_panel(api, "API", ui::Icon::settings);
    draw_control_panel(input, "INPUT", ui::Icon::mic);
    draw_control_panel(output, "OUTPUT", ui::Icon::speaker);
}

void JuceRoomBrowserComponent::resized() {
    auto bounds = getLocalBounds().reduced(CONTENT_PAD);
    auto content = bounds;
    content.removeFromTop(BROWSER_HEADER_HEIGHT);
    content.removeFromTop(CONTENT_GAP);
    auto bottom = content.removeFromBottom(BROWSER_BOTTOM_HEIGHT);
    content.removeFromBottom(CONTENT_GAP);
    auto left = content.removeFromLeft(SERVER_RAIL_WIDTH);
    content.removeFromLeft(CONTENT_GAP);

    auto server_area = left.reduced(12, 12);
    server_area.removeFromTop(42);
    add_server_bottom_bounds_ = left.reduced(12, 18).removeFromBottom(42);
    server_list_area_ = server_area.withBottom(
        juce::jmax(server_area.getY(), add_server_bottom_bounds_.getY() - 8));

    auto room_area = content.reduced(12, 10);
    auto top = room_area.removeFromTop(42);
    top.removeFromLeft(130);
    top.removeFromRight(110);
    top.removeFromRight(10);
    top.removeFromRight(134);
    top.removeFromRight(10);
    auto search_container = top.removeFromRight(285).reduced(0, 2);
    search_editor_.setBounds(
        search_container.withTrimmedLeft(40).withTrimmedRight(10).reduced(0, 1));
    room_area.removeFromTop(10);
    room_area.removeFromTop(42);
    room_list_area_ = room_area;

    constexpr int gap = 8;
    const int audio_width =
        juce::jlimit(280, 360, bottom.getWidth() * 24 / 100);
    const int api_width = juce::jlimit(170, 230, bottom.getWidth() * 15 / 100);
    auto audio = bottom.removeFromLeft(audio_width);
    bottom.removeFromLeft(gap);
    auto api = bottom.removeFromLeft(api_width);
    bottom.removeFromLeft(gap);
    const int device_width = juce::jmax(220, (bottom.getWidth() - gap) / 2);
    auto input = bottom.removeFromLeft(device_width);
    bottom.removeFromLeft(gap);
    auto output = bottom;

    auto audio_controls = audio.reduced(12, 8);
    audio_controls.removeFromTop(16);
    audio_controls.removeFromTop(6);
    auto audio_row = audio_controls.removeFromTop(32);
    monitor_toggle_.setBounds(audio_row.removeFromLeft(138).reduced(0, 1));

    auto set_combo = [](juce::Rectangle<int> panel, juce::ComboBox& combo) {
        auto content_area = panel.reduced(12, 8);
        content_area.removeFromTop(16);
        content_area.removeFromTop(6);
        auto row = content_area.removeFromTop(32);
        row.removeFromLeft(34);
        combo.setBounds(row.reduced(0, 1));
    };
    set_combo(api, api_combo_);
    set_combo(input, input_combo_);
    set_combo(output, output_combo_);
    clamp_scroll_offsets();
}

void JuceRoomBrowserComponent::mouseDown(const juce::MouseEvent& event) {
    const auto position = event.getPosition();
    if (refresh_button_bounds_.contains(position)) {
        start_status_refresh(true);
        return;
    }
    if (edit_servers_button_bounds_.contains(position)) {
        start_edit_servers_flow();
        return;
    }
    if (add_server_bottom_bounds_.contains(position)) {
        start_add_server_flow();
        return;
    }
    if (create_button_bounds_.contains(position)) {
        start_create_flow();
        return;
    }
    if (join_invite_button_bounds_.contains(position)) {
        start_join_invite_flow();
        return;
    }

    const int server_index = server_row_at(position);
    if (server_index >= 0) {
        selected_server_index_ = server_index;
        selected_room_index_ = -1;
        room_scroll_px_ = 0;
        start_status_refresh(true);
        repaint();
        return;
    }

    const int room_index = room_row_at(position);
    if (room_index >= 0) {
        if (join_button_at(position)) {
            start_join_flow(room_index);
        } else {
            select_room(room_index);
        }
    }
}

void JuceRoomBrowserComponent::mouseDoubleClick(const juce::MouseEvent& event) {
    const int room_index = room_row_at(event.getPosition());
    if (room_index >= 0) {
        start_join_flow(room_index);
    }
}

void JuceRoomBrowserComponent::mouseWheelMove(
    const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) {
    if (std::abs(wheel.deltaY) < 0.001f) {
        return;
    }
    const int scroll_rows = wheel.deltaY < 0.0f ? 3 : -3;
    if (server_list_area_.contains(event.getPosition())) {
        server_scroll_px_ += scroll_rows * SERVER_ROW_HEIGHT;
        clamp_scroll_offsets();
        repaint();
        return;
    }
    if (room_list_area_.contains(event.getPosition())) {
        room_scroll_px_ += scroll_rows * ROOM_ROW_HEIGHT;
        clamp_scroll_offsets();
        repaint();
    }
}

void JuceRoomBrowserComponent::open_invite(std::string invite_text) {
    start_join_invite_flow(std::move(invite_text));
}

void JuceRoomBrowserComponent::timerCallback() {
    poll_job_result();
    poll_audio_device_refresh();
    poll_official_server_refresh();
    if (waiting_for_room_key_ && client_.consume_room_removed_by_server()) {
        cancel_waiting_join();
        status_text_ = "Request declined";
        repaint();
        return;
    }
    if (waiting_for_room_key_ && client_.has_media_key()) {
        finish_waiting_join();
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_auto_refresh_) {
        bool running = false;
        {
            std::lock_guard<std::mutex> lock(job_mutex_);
            running = job_running_;
        }
        if (!running) {
            start_status_refresh(false);
        }
        next_auto_refresh_ = now + 10s;
    }
    repaint();
}

void JuceRoomBrowserComponent::start_status_refresh(bool manual) {
    if (servers_.empty()) {
        if (manual) {
            status_text_ = "No server selected";
            repaint();
        }
        return;
    }
    const int server_index = selected_server_index_;
    const BrowserServer server = selected_server();
    const uint32_t request_id = next_request_id();
    if (manual) {
        status_text_ = "Refreshing...";
    }
    start_job([server, server_index, request_id]() {
        BrowserJobResult result;
        result.kind = JobKind::Status;
        result.server_index = server_index;
        result.server_address = server.address;
        result.server_port = server.port;

        try {
            uint16_t room_offset = 0;
            for (int page = 0; page < ROOM_STATUS_PAGE_LIMIT; ++page) {
                ServerStatusRequestHdr request{};
                request.magic = CTRL_MAGIC;
                request.type = CtrlHdr::Cmd::SERVER_STATUS_REQUEST;
                request.request_id = request_id;
                request.room_offset = room_offset;
                request.room_limit =
                    static_cast<uint8_t>(MAX_ROOM_STATUS_SUMMARIES);

                double rtt = 0.0;
                const auto response =
                    send_control_request<ServerStatusRequestHdr, ServerStatusResponseHdr>(
                        server.address, server.port, request,
                        CtrlHdr::Cmd::SERVER_STATUS_RESPONSE, request_id, rtt);
                result.status.ok = true;
                result.status.round_trip_ms = rtt;
                result.status.server_id = fixed_string(response.server_id);
                result.status.total_rooms = response.total_rooms;
                result.status.active_participants = response.active_participants;
                result.status.truncated = response.truncated != 0;
                result.status.token_auth_available =
                    response.token_auth_available != 0;

                for (uint8_t i = 0; i < response.room_count; ++i) {
                    BrowserRoom room;
                    room.room_id = fixed_string(response.rooms[i].room_id);
                    room.room_name = fixed_string(response.rooms[i].room_name);
                    room.room_instance_id =
                        fixed_string(response.rooms[i].room_instance_id);
                    room.access_epoch = response.rooms[i].access_epoch;
                    room.participant_count = response.rooms[i].participant_count;
                    room.access_mode = response.rooms[i].access_mode;
                    room.locked = room.access_mode == ROOM_ACCESS_PASSWORD;
                    result.status.rooms.push_back(std::move(room));
                }

                const size_t next_offset =
                    static_cast<size_t>(response.room_offset) + response.room_count;
                if (response.room_count == 0 || response.truncated == 0 ||
                    next_offset >= response.total_rooms) {
                    result.status.truncated = false;
                    break;
                }
                if (page + 1 == ROOM_STATUS_PAGE_LIMIT) {
                    result.status.truncated = true;
                    break;
                }
                room_offset = static_cast<uint16_t>(
                    std::min<size_t>(next_offset,
                                     std::numeric_limits<uint16_t>::max()));
            }
        } catch (const std::exception& e) {
            result.status.ok = false;
            result.status.reason = e.what();
            result.message = e.what();
        }
        return result;
    });
}

void JuceRoomBrowserComponent::start_join_flow(int room_index) {
    const auto status = selected_status();
    if (room_index < 0 || room_index >= static_cast<int>(status.rooms.size())) {
        return;
    }
    const auto room = status.rooms[static_cast<size_t>(room_index)];
    const auto server = selected_server();

    active_dialog_ = std::make_unique<juce::AlertWindow>(
        "Join Room", "", juce::AlertWindow::NoIcon, this);
    auto& alert = *active_dialog_;
    alert.addTextEditor("displayName", last_display_name_, "Name");
    if (room_access_requires_password(room.access_mode)) {
        alert.addTextEditor("password", "", "Password", true);
        style_dialog_editors(alert, {"displayName", "password"});
    } else {
        style_dialog_editors(alert, {"displayName"});
    }
    alert.addButton("Join", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert.addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    alert.enterModalState(
        true, juce::ModalCallbackFunction::create(
                  [safe_this = juce::Component::SafePointer<JuceRoomBrowserComponent>(this),
                   room, server](int result) {
                      auto* self = safe_this.getComponent();
                      if (self == nullptr || self->active_dialog_ == nullptr) {
                          return;
                      }
                      auto& dialog = *self->active_dialog_;
                      dialog.exitModalState(result);
                      dialog.setVisible(false);
                      if (result != 1) {
                          return;
                      }

                      const auto display_name =
                          read_required_text(dialog, "displayName");
                      if (display_name.empty()) {
                          self->status_text_ = "Name is required";
                          self->repaint();
                          return;
                      }
                      self->last_display_name_ = display_name;
                      const auto password =
                          room_access_requires_password(room.access_mode)
                              ? dialog.getTextEditorContents("password").toStdString()
                              : "";
                      const auto profile_id =
                          self->profile_id_for_display_name(display_name);
                      const int server_index = self->selected_server_index_;
                      const uint32_t request_id = self->next_request_id();

                      self->status_text_ =
                          "Joining " +
                          juce::String(room.room_name.empty() ? room.room_id
                                                              : room.room_name) +
                          "...";
                      self->start_job([self, server, server_index, room, profile_id,
                                       display_name, password,
                                       request_id]() {
                          BrowserJobResult result_value;
                          result_value.kind = JobKind::Join;
                          result_value.server_index = server_index;
                          result_value.server_address = server.address;
                          result_value.server_port = server.port;
                          result_value.display_name = display_name;

                          RoomJoinTokenRequestHdr request{};
                          request.magic = CTRL_MAGIC;
                          request.type = CtrlHdr::Cmd::ROOM_JOIN_TOKEN_REQUEST;
                          request.request_id = request_id;
                          write_fixed(request.room_id, room.room_id);
                          write_fixed(request.profile_id, profile_id);
                          write_fixed(request.display_name, display_name);
                          write_fixed(request.password_hash,
                                      self->password_hash(password));

                          try {
                              double rtt = 0.0;
                              const auto response = send_control_request<
                                  RoomJoinTokenRequestHdr, RoomJoinTokenResponseHdr>(
                                  server.address, server.port, request,
                                  CtrlHdr::Cmd::ROOM_JOIN_TOKEN_RESPONSE, request_id,
                                  rtt);
                              result_value.ticket.status = response.status;
                              result_value.ticket.reason = fixed_string(response.reason);
                              result_value.ticket.room_id = fixed_string(response.room_id);
                              result_value.ticket.room_name =
                                  fixed_string(response.room_name);
                              result_value.ticket.room_instance_id =
                                  fixed_string(response.room_instance_id);
                              result_value.ticket.access_epoch =
                                  response.access_epoch;
                              result_value.ticket.access_mode =
                                  response.access_mode;
                              result_value.ticket.join_token =
                                  fixed_string(response.join_token);
                              result_value.ticket.ok =
                                  response.status == ROOM_STATUS_OK &&
                                  !result_value.ticket.join_token.empty();
                              if (!result_value.ticket.ok) {
                                  result_value.message =
                                      status_text_for_code(response.status,
                                                           result_value.ticket.reason)
                                          .toStdString();
                                  return result_value;
                              }
                              if (result_value.ticket.room_instance_id !=
                                      room.room_instance_id ||
                                  result_value.ticket.access_epoch !=
                                      room.access_epoch) {
                                  result_value.message =
                                      "Room changed; refresh and use a new invite";
                                  return result_value;
                              }

                              self->client_.join_room(
                                  server.address, server.port,
                                  result_value.ticket.room_id, profile_id,
                                  display_name, result_value.ticket.join_token,
                                  {}, room.access_mode);
                              result_value.joined =
                                  room.access_mode != ROOM_ACCESS_APPROVE;
                              result_value.waiting_for_room_key =
                                  room.access_mode == ROOM_ACCESS_APPROVE;
                              result_value.joined_room_id =
                                  result_value.ticket.room_id;
                              result_value.access_mode = room.access_mode;
                          } catch (const std::exception& e) {
                              result_value.message = e.what();
                          }
                          return result_value;
                      });
                  }),
        false);
}

void JuceRoomBrowserComponent::start_join_invite_flow(std::string initial_invite) {
    juce::String initial_invite_text(initial_invite);
    if (initial_invite.empty()) {
        std::string invite_reason;
        const auto clipboard_text = juce::SystemClipboard::getTextFromClipboard();
        if (parse_secure_invite_text(clipboard_text.toStdString(), invite_reason)
                .has_value()) {
            initial_invite_text = clipboard_text;
        }
    }

    active_dialog_ = std::make_unique<juce::AlertWindow>(
        "Join Invite", "", juce::AlertWindow::NoIcon, this);
    auto& alert = *active_dialog_;
    alert.addTextEditor("invite", initial_invite_text, "Invite");
    style_dialog_editors(alert, {"invite"});
    alert.addButton("Open", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert.addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    alert.enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safe_this = juce::Component::SafePointer<JuceRoomBrowserComponent>(this)](
                int result) {
                auto* self = safe_this.getComponent();
                if (self == nullptr || self->active_dialog_ == nullptr) {
                    return;
                }
                auto& dialog = *self->active_dialog_;
                dialog.exitModalState(result);
                dialog.setVisible(false);
                if (result != 1) {
                    return;
                }

                std::string reason;
                const auto invite = parse_secure_invite_text(
                    dialog.getTextEditorContents("invite").toStdString(), reason);
                if (!invite.has_value()) {
                    self->status_text_ = reason;
                    self->repaint();
                    return;
                }

                if (const auto existing = self->find_server_endpoint(
                        invite->server_address, invite->server_port)) {
                    self->focus_server(*existing);
                } else {
                    BrowserServer server;
                    server.name = "Invite";
                    server.address = invite->server_address;
                    server.port = invite->server_port;
                    self->servers_.insert(self->servers_.begin(), std::move(server));
                    self->focus_server(0);
                    self->save_servers();
                }
                self->pending_invite_join_ = PendingInviteJoin{
                    invite->server_address, invite->server_port, invite->room_id};
                self->selected_room_index_ = -1;
                self->status_text_ = "Opening invite...";
                self->start_status_refresh(true);
            }),
        false);
}

void JuceRoomBrowserComponent::start_create_flow() {
    if (servers_.empty() || selected_server_index_ < 0 ||
        selected_server_index_ >= static_cast<int>(servers_.size())) {
        status_text_ = "No server selected";
        repaint();
        return;
    }

    auto* content = new CreateRoomDialog(
        last_display_name_,
        [safe_this = juce::Component::SafePointer<JuceRoomBrowserComponent>(this)](
            CreateRoomInput input) {
                auto* self = safe_this.getComponent();
                if (self == nullptr) {
                    return;
                }
                self->last_display_name_ = input.display_name;
                const auto media_secret = make_media_secret();
                const auto room_id = normalized_id(input.room_name, "room");
                const auto profile_id =
                    self->profile_id_for_display_name(input.display_name);
                const auto server = self->selected_server();
                const int server_index = self->selected_server_index_;
                const uint32_t request_id = self->next_request_id();

                self->status_text_ =
                    "Creating " + juce::String(input.room_name) + "...";
                self->start_job([self, server, server_index, room_id,
                                 room_name = input.room_name, profile_id,
                                 display_name = input.display_name,
                                 password = input.password, media_secret,
                                 access_mode = input.access_mode, request_id]() {
                    BrowserJobResult result_value;
                    result_value.kind = JobKind::Create;
                    result_value.server_index = server_index;
                    result_value.server_address = server.address;
                    result_value.server_port = server.port;
                    result_value.display_name = display_name;

                    RoomCreateRequestHdr request{};
                    request.magic = CTRL_MAGIC;
                    request.type = CtrlHdr::Cmd::ROOM_CREATE_REQUEST;
                    request.request_id = request_id;
                    write_fixed(request.room_id, room_id);
                    write_fixed(request.room_name, room_name);
                    write_fixed(request.profile_id, profile_id);
                    write_fixed(request.display_name, display_name);
                    if (access_mode == ROOM_ACCESS_PASSWORD) {
                        write_fixed(request.password_hash,
                                    self->password_hash(password));
                    }
                    request.access_mode = access_mode;

                    try {
                        double rtt = 0.0;
                        const auto response =
                            send_control_request<RoomCreateRequestHdr,
                                                 RoomCreateResponseHdr>(
                                server.address, server.port, request,
                                CtrlHdr::Cmd::ROOM_CREATE_RESPONSE, request_id, rtt);
                        result_value.ticket.status = response.status;
                        result_value.ticket.reason = fixed_string(response.reason);
                        result_value.ticket.room_id = fixed_string(response.room_id);
                        result_value.ticket.room_name = fixed_string(response.room_name);
                        result_value.ticket.room_instance_id =
                            fixed_string(response.room_instance_id);
                        result_value.ticket.access_epoch = response.access_epoch;
                        result_value.ticket.access_mode = response.access_mode;
                        result_value.ticket.join_token =
                            fixed_string(response.join_token);
                        result_value.ticket.admin_token =
                            fixed_string(response.admin_token);
                        result_value.ticket.ok =
                            response.status == ROOM_STATUS_OK &&
                            !result_value.ticket.join_token.empty();
                        if (!result_value.ticket.ok) {
                            result_value.message =
                                status_text_for_code(response.status,
                                                     result_value.ticket.reason)
                                    .toStdString();
                            return result_value;
                        }

                        self->client_.join_room(server.address, server.port,
                                                result_value.ticket.room_id,
                                                profile_id, display_name,
                                                result_value.ticket.join_token,
                                                media_secret,
                                                response.access_mode);
                        result_value.joined = true;
                        result_value.joined_room_id = result_value.ticket.room_id;
                        result_value.media_secret = media_secret;
                        result_value.access_mode = response.access_mode;
                    } catch (const std::exception& e) {
                        result_value.message = e.what();
                    }
                    return result_value;
                });
            });

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Create Room";
    options.content.setOwned(content);
    options.componentToCentreAround = this;
    options.dialogBackgroundColour = juce_theme::colour::panel_bottom();
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = false;
    options.launchAsync();
}

void JuceRoomBrowserComponent::start_edit_servers_flow() {
    if (servers_.empty() || selected_server_index_ < 0 ||
        selected_server_index_ >= static_cast<int>(servers_.size())) {
        status_text_ = "No server selected";
        repaint();
        return;
    }

    const int edit_index = selected_server_index_;
    const auto server = servers_[static_cast<size_t>(edit_index)];
    active_dialog_ = std::make_unique<juce::AlertWindow>(
        "Edit Server", "", juce::AlertWindow::NoIcon, this);
    auto& alert = *active_dialog_;
    alert.addTextEditor("name", server.name, "Name");
    alert.addTextEditor("address", server.address, "Address");
    alert.addTextEditor("port", juce::String(server.port), "Port");
    style_dialog_editors(alert, {"name", "address", "port"});
    alert.addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    if (servers_.size() > 1) {
        alert.addButton("Remove", 2);
    }
    alert.addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    alert.enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safe_this = juce::Component::SafePointer<JuceRoomBrowserComponent>(this),
             edit_index](int result) {
                auto* self = safe_this.getComponent();
                if (self == nullptr || self->active_dialog_ == nullptr) {
                    return;
                }
                auto& dialog = *self->active_dialog_;
                dialog.exitModalState(result);
                dialog.setVisible(false);

                if (edit_index < 0 ||
                    edit_index >= static_cast<int>(self->servers_.size())) {
                    self->status_text_ = "Server selection changed";
                    self->repaint();
                    return;
                }

                if (result == 2) {
                    if (self->servers_.size() <= 1) {
                        self->status_text_ = "Keep at least one server";
                        self->repaint();
                        return;
                    }
                    self->servers_.erase(self->servers_.begin() + edit_index);
                    self->selected_server_index_ =
                        std::clamp(edit_index, 0,
                                   static_cast<int>(self->servers_.size()) - 1);
                    self->selected_room_index_ = -1;
                    self->status_text_ = "Server removed";
                    self->save_servers();
                    self->start_status_refresh(true);
                    self->repaint();
                    return;
                }

                if (result != 1) {
                    return;
                }

                BrowserServer updated =
                    self->servers_[static_cast<size_t>(edit_index)];
                updated.name = read_required_text(dialog, "name");
                updated.address = read_required_text(dialog, "address");
                const auto port_text = read_required_text(dialog, "port");
                const int port = juce::String(port_text).getIntValue();
                if (updated.address.empty() || port <= 0 ||
                    port > std::numeric_limits<uint16_t>::max()) {
                    self->status_text_ = "Server address and port are required";
                    self->repaint();
                    return;
                }
                updated.port = static_cast<uint16_t>(port);
                if (const auto existing = self->find_server_endpoint(
                        updated.address, updated.port, edit_index)) {
                    self->focus_server(*existing);
                    self->status_text_ = "Server already exists";
                    self->repaint();
                    return;
                }
                if (updated.name.empty()) {
                    updated.name = is_local_address(updated.address) ? "Local Network"
                                                                     : updated.address;
                }
                updated.status = {};
                updated.last_refresh = {};
                self->servers_[static_cast<size_t>(edit_index)] = std::move(updated);
                self->selected_server_index_ = edit_index;
                self->selected_room_index_ = -1;
                self->save_servers();
                self->start_status_refresh(true);
                self->repaint();
            }),
        false);
}

void JuceRoomBrowserComponent::start_add_server_flow() {
    auto* content = new AddServerDialog(
        [safe_this = juce::Component::SafePointer<JuceRoomBrowserComponent>(this)](
            AddServerInput input) {
            auto* self = safe_this.getComponent();
            if (self == nullptr) {
                return;
            }

            BrowserServer server;
            server.address = std::move(input.address);
            server.port = input.port;
            if (const auto existing =
                    self->find_server_endpoint(server.address, server.port)) {
                self->focus_server(*existing);
                self->status_text_ = "Server already exists";
                self->repaint();
                return;
            }

            server.name = std::move(input.name);
            if (server.name.empty()) {
                server.name = is_local_address(server.address) ? "Local Network"
                                                               : server.address;
            }
            self->servers_.push_back(std::move(server));
            self->focus_server(static_cast<int>(self->servers_.size()) - 1);
            self->save_servers();
            self->start_status_refresh(true);
            self->repaint();
        },
        [safe_this = juce::Component::SafePointer<JuceRoomBrowserComponent>(this)]() {
            auto* self = safe_this.getComponent();
            if (self != nullptr) {
                self->request_official_server_refresh(true);
            }
        });

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Add Server";
    options.content.setOwned(content);
    options.componentToCentreAround = this;
    options.dialogBackgroundColour = juce_theme::colour::panel_bottom();
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = false;
    options.launchAsync();
}

void JuceRoomBrowserComponent::start_job(std::function<BrowserJobResult()> work) {
    {
        std::lock_guard<std::mutex> lock(job_mutex_);
        if (job_running_) {
            status_text_ = "Request already running";
            return;
        }
        job_running_ = true;
        job_finished_ = false;
        job_result_.reset();
    }
    if (job_thread_.joinable()) {
        job_thread_.join();
    }
    job_thread_ = std::thread([this, work = std::move(work)]() mutable {
        auto result = work();
        std::lock_guard<std::mutex> lock(job_mutex_);
        job_result_ = std::move(result);
        job_finished_ = true;
    });
}

void JuceRoomBrowserComponent::poll_job_result() {
    std::optional<BrowserJobResult> result;
    {
        std::lock_guard<std::mutex> lock(job_mutex_);
        if (!job_finished_) {
            return;
        }
        result = std::move(job_result_);
        job_result_.reset();
        job_finished_ = false;
        job_running_ = false;
    }
    if (job_thread_.joinable()) {
        job_thread_.join();
    }
    if (!result.has_value()) {
        return;
    }

    if (result->kind == JobKind::Status) {
        apply_status(std::move(*result));
    } else {
        apply_ticket_result(*result);
    }
}

void JuceRoomBrowserComponent::apply_status(BrowserJobResult result) {
    if (result.server_index >= 0 &&
        result.server_index < static_cast<int>(servers_.size())) {
        auto& server = servers_[static_cast<size_t>(result.server_index)];
        server.status = std::move(result.status);
        server.last_refresh = std::chrono::steady_clock::now();
    }
    if (selected_status().ok) {
        status_text_ =
            selected_status().rooms.empty()
                ? "No rooms on this server"
                : selected_status().truncated
                      ? "Showing " +
                            juce::String(static_cast<int>(
                                selected_status().rooms.size())) +
                            " of " +
                            juce::String(selected_status().total_rooms) +
                            " rooms"
                      : "Ready";
    } else {
        status_text_ = result.message.empty() ? "Server offline" : result.message;
    }
    clamp_scroll_offsets();
    if (result.status.ok && pending_invite_join_.has_value() &&
        pending_invite_join_->server_address == result.server_address &&
        pending_invite_join_->server_port == result.server_port) {
        const auto room_id = pending_invite_join_->room_id;
        const auto it = std::find_if(
            selected_status().rooms.begin(), selected_status().rooms.end(),
            [&](const BrowserRoom& room) { return room.room_id == room_id; });
        if (it != selected_status().rooms.end()) {
            const int room_index =
                static_cast<int>(std::distance(selected_status().rooms.begin(), it));
            pending_invite_join_.reset();
            selected_room_index_ = room_index;
            juce::MessageManager::callAsync(
                [safe_this = juce::Component::SafePointer<JuceRoomBrowserComponent>(this),
                 room_index]() {
                    if (auto* self = safe_this.getComponent()) {
                        self->start_join_flow(room_index);
                    }
                });
        } else {
            status_text_ = "Invite room is not listed on this server";
            pending_invite_join_.reset();
        }
    }
    repaint();
}

void JuceRoomBrowserComponent::apply_ticket_result(const BrowserJobResult& result) {
    auto make_launch = [&]() {
        return JoinLaunch{result.server_address, result.server_port,
                          result.joined_room_id, result.ticket.room_name,
                          result.ticket.admin_token,
                          result.ticket.room_instance_id,
                          result.ticket.access_epoch,
                          result.media_secret,
                          result.access_mode == ROOM_ACCESS_OPEN
                              ? result.ticket.access_mode
                              : result.access_mode};
    };
    if (result.waiting_for_room_key) {
        remember_display_name(result.display_name);
        show_waiting_for_room_key(make_launch());
        return;
    }
    if (result.joined) {
        remember_display_name(result.display_name);
        status_text_ = "Joined";
        if (joined_callback_) {
            auto callback = joined_callback_;
            JoinLaunch launch = make_launch();
            juce::MessageManager::callAsync(
                [callback = std::move(callback), launch = std::move(launch)]() mutable {
                    callback(std::move(launch));
                });
        }
        return;
    }
    status_text_ = result.message.empty() ? "Join failed" : result.message;
    repaint();
}

void JuceRoomBrowserComponent::remember_display_name(const std::string& display_name) {
    const auto name = trim_copy(display_name);
    if (name.empty()) {
        return;
    }
    last_display_name_ = name;
    save_client_display_name(startup_options_.config_path, name);
}

void JuceRoomBrowserComponent::show_waiting_for_room_key(JoinLaunch launch) {
    pending_join_launch_ = std::move(launch);
    waiting_for_room_key_ = true;
    status_text_ = "Waiting for host approval...";

    active_dialog_ = std::make_unique<juce::AlertWindow>(
        "Waiting for Host", "Waiting for the host to approve access.",
        juce::AlertWindow::NoIcon, this);
    auto& alert = *active_dialog_;
    alert.addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    alert.enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safe_this = juce::Component::SafePointer<JuceRoomBrowserComponent>(this)](
                int) {
                if (auto* self = safe_this.getComponent()) {
                    if (self->waiting_for_room_key_) {
                        self->cancel_waiting_join();
                    }
                }
            }),
        false);
}

void JuceRoomBrowserComponent::finish_waiting_join() {
    if (!waiting_for_room_key_ || !pending_join_launch_.has_value()) {
        return;
    }
    auto launch = std::move(*pending_join_launch_);
    launch.media_secret = client_.current_media_secret();
    pending_join_launch_.reset();
    waiting_for_room_key_ = false;
    if (active_dialog_ != nullptr) {
        active_dialog_->exitModalState(1);
        active_dialog_->setVisible(false);
        active_dialog_.reset();
    }
    status_text_ = "Joined";
    if (joined_callback_) {
        auto callback = joined_callback_;
        juce::MessageManager::callAsync(
            [callback = std::move(callback), launch = std::move(launch)]() mutable {
                callback(std::move(launch));
            });
    }
}

void JuceRoomBrowserComponent::cancel_waiting_join() {
    waiting_for_room_key_ = false;
    pending_join_launch_.reset();
    client_.stop_connection();
    if (active_dialog_ != nullptr) {
        active_dialog_->exitModalState(0);
        active_dialog_->setVisible(false);
        active_dialog_.reset();
    }
    status_text_ = "Join canceled";
    repaint();
}

uint32_t JuceRoomBrowserComponent::next_request_id() {
    return request_id_.fetch_add(1, std::memory_order_relaxed);
}

JuceRoomBrowserComponent::BrowserServer JuceRoomBrowserComponent::selected_server() const {
    if (selected_server_index_ >= 0 &&
        selected_server_index_ < static_cast<int>(servers_.size())) {
        return servers_[static_cast<size_t>(selected_server_index_)];
    }
    return {};
}

const JuceRoomBrowserComponent::ServerStatus&
JuceRoomBrowserComponent::selected_status() const {
    static const ServerStatus empty_status{};
    if (selected_server_index_ >= 0 &&
        selected_server_index_ < static_cast<int>(servers_.size())) {
        return servers_[static_cast<size_t>(selected_server_index_)].status;
    }
    return empty_status;
}

std::vector<int> JuceRoomBrowserComponent::visible_room_indices() const {
    std::vector<int> indices;
    const auto& status = selected_status();
    const auto filter = lowercase_copy(search_editor_.getText().toStdString());
    for (int i = 0; i < static_cast<int>(status.rooms.size()); ++i) {
        if (filter.empty()) {
            indices.push_back(i);
            continue;
        }
        const auto& room = status.rooms[static_cast<size_t>(i)];
        const auto haystack =
            lowercase_copy(room.room_name + " " + room.room_id + " " +
                           selected_server().name + " " + status.server_id);
        if (haystack.find(filter) != std::string::npos) {
            indices.push_back(i);
        }
    }
    return indices;
}

void JuceRoomBrowserComponent::clamp_scroll_offsets() {
    const int visible_server_rows =
        std::max(1, server_list_area_.getHeight() / SERVER_ROW_HEIGHT);
    const int server_max_scroll =
        std::max(0, (static_cast<int>(servers_.size()) - visible_server_rows) *
                        SERVER_ROW_HEIGHT);
    server_scroll_px_ = juce::jlimit(0, server_max_scroll, server_scroll_px_);

    const auto visible = visible_room_indices();
    const int visible_room_rows =
        std::max(1, room_list_area_.getHeight() / ROOM_ROW_HEIGHT);
    const int room_max_scroll =
        std::max(0, (static_cast<int>(visible.size()) - visible_room_rows) *
                        ROOM_ROW_HEIGHT);
    room_scroll_px_ = juce::jlimit(0, room_max_scroll, room_scroll_px_);
}

std::optional<int> JuceRoomBrowserComponent::find_server_endpoint(
    const std::string& address, uint16_t port,
    std::optional<int> ignored_index) const {
    for (int i = 0; i < static_cast<int>(servers_.size()); ++i) {
        if (ignored_index.has_value() && i == *ignored_index) {
            continue;
        }
        const auto& server = servers_[static_cast<size_t>(i)];
        if (same_endpoint(server.address, server.port, address, port)) {
            return i;
        }
    }
    return std::nullopt;
}

void JuceRoomBrowserComponent::focus_server(int index) {
    if (index < 0 || index >= static_cast<int>(servers_.size())) {
        return;
    }

    selected_server_index_ = index;
    selected_room_index_ = -1;
    room_scroll_px_ = 0;

    const int visible_server_rows =
        std::max(1, server_list_area_.getHeight() / SERVER_ROW_HEIGHT);
    const int visible_height = visible_server_rows * SERVER_ROW_HEIGHT;
    const int selected_top = selected_server_index_ * SERVER_ROW_HEIGHT;
    const int selected_bottom = selected_top + SERVER_ROW_HEIGHT;
    if (selected_top < server_scroll_px_) {
        server_scroll_px_ = selected_top;
    } else if (selected_bottom > server_scroll_px_ + visible_height) {
        server_scroll_px_ = selected_bottom - visible_height;
    }
    clamp_scroll_offsets();
}

int JuceRoomBrowserComponent::server_row_at(juce::Point<int> position) const {
    if (!server_list_area_.contains(position)) {
        return -1;
    }
    const int visible_height =
        (server_list_area_.getHeight() / SERVER_ROW_HEIGHT) * SERVER_ROW_HEIGHT;
    const int local_y = position.y - server_list_area_.getY();
    if (local_y < 0 || local_y >= visible_height) {
        return -1;
    }
    const int index = (local_y + server_scroll_px_) / SERVER_ROW_HEIGHT;
    return index >= 0 && index < static_cast<int>(servers_.size()) ? index : -1;
}

int JuceRoomBrowserComponent::room_row_at(juce::Point<int> position) const {
    if (!room_list_area_.contains(position)) {
        return -1;
    }
    const int visible_height =
        (room_list_area_.getHeight() / ROOM_ROW_HEIGHT) * ROOM_ROW_HEIGHT;
    const int local_y = position.y - room_list_area_.getY();
    if (local_y < 0 || local_y >= visible_height) {
        return -1;
    }
    const int visible_position = (local_y + room_scroll_px_) / ROOM_ROW_HEIGHT;
    const auto visible = visible_room_indices();
    if (visible_position < 0 ||
        visible_position >= static_cast<int>(visible.size())) {
        return -1;
    }
    return visible[static_cast<size_t>(visible_position)];
}

bool JuceRoomBrowserComponent::join_button_at(juce::Point<int> position) const {
    if (!room_list_area_.contains(position)) {
        return false;
    }
    const int visible_height =
        (room_list_area_.getHeight() / ROOM_ROW_HEIGHT) * ROOM_ROW_HEIGHT;
    const int local_y = position.y - room_list_area_.getY();
    if (local_y < 0 || local_y >= visible_height) {
        return false;
    }
    const int visible_position = (local_y + room_scroll_px_) / ROOM_ROW_HEIGHT;
    const auto visible = visible_room_indices();
    if (visible_position < 0 ||
        visible_position >= static_cast<int>(visible.size())) {
        return false;
    }
    auto row = room_list_area_.withY(
                       room_list_area_.getY() +
                       visible_position * ROOM_ROW_HEIGHT - room_scroll_px_)
                   .withHeight(ROOM_ROW_HEIGHT)
                   .reduced(0, 3)
                   .reduced(20, 0);
    const auto columns = room_columns(row.getWidth());
    row.removeFromLeft(columns.name);
    row.removeFromLeft(columns.players);
    row.removeFromLeft(columns.access);
    auto join = row.withWidth(columns.join);
    const auto button_width = juce::jlimit(82, 112, join.getWidth() - 52);
    auto button = join.removeFromLeft(button_width).reduced(0, 13);
    return button.contains(position);
}

void JuceRoomBrowserComponent::select_room(int room_index) {
    selected_room_index_ = room_index;
    repaint();
}

juce::String JuceRoomBrowserComponent::selected_api_name() const {
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

AudioStream::DeviceIndex JuceRoomBrowserComponent::selected_input_device() const {
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

AudioStream::DeviceIndex JuceRoomBrowserComponent::selected_output_device() const {
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

std::string JuceRoomBrowserComponent::password_hash(const std::string& password) const {
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

std::string JuceRoomBrowserComponent::profile_id_for_display_name(
    const std::string& display_name) const {
    constexpr size_t max_profile_id_bytes = 63;
    const std::string suffix = "-" + local_profile_id_;
    if (suffix.size() >= max_profile_id_bytes) {
        return local_profile_id_.substr(0, max_profile_id_bytes);
    }

    auto base = normalized_id(display_name, "player");
    base.resize(std::min(base.size(), max_profile_id_bytes - suffix.size()));
    while (!base.empty() && base.back() == '-') {
        base.pop_back();
    }
    if (base.empty()) {
        return local_profile_id_;
    }
    return base + suffix;
}
