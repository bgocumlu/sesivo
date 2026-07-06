#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

namespace {
namespace ui {
const juce::Colour bg_top{0xff141414};
const juce::Colour bg_bottom{0xff0f0f0f};
const juce::Colour title_bar{0xff181818};
const juce::Colour panel_top{0xff1e1e1e};
const juce::Colour panel_bottom{0xff161616};
const juce::Colour row{0xff222222};
const juce::Colour row_hover{0xff282828};
const juce::Colour row_selected{0xff201915};
const juce::Colour border{0xff373737};
const juce::Colour border_soft{0xff2b2b2b};
const juce::Colour text{0xffeeeeee};
const juce::Colour text_dim{0xffb6b2ae};
const juce::Colour text_faint{0xff8f8b87};
const juce::Colour accent{0xff875334};
const juce::Colour accent_hi{0xffa86842};
const juce::Colour green{0xff74c66a};
const juce::Colour amber{0xffc58a5a};
const juce::Colour red{0xffe36b5c};
const juce::Colour blue{0xff8a6e9e};

constexpr float radius = 4.0f;
constexpr int pad = 12;

juce::Font font(float size, bool bold = false) {
    return juce::Font(juce::FontOptions(size, bold ? juce::Font::bold
                                                   : juce::Font::plain));
}

void fill_panel(juce::Graphics& g, juce::Rectangle<int> bounds) {
    auto area = bounds.toFloat().reduced(0.5f);
    juce::ColourGradient grad(panel_top, area.getX(), area.getY(), panel_bottom,
                              area.getX(), area.getBottom(), false);
    g.setGradientFill(grad);
    g.fillRoundedRectangle(area, radius);
    g.setColour(border);
    g.drawRoundedRectangle(area, radius, 1.0f);
}

void fill_button(juce::Graphics& g, juce::Rectangle<int> bounds, bool primary = false,
                 bool disabled = false) {
    auto area = bounds.toFloat().reduced(0.5f);
    juce::ColourGradient grad(primary ? juce::Colour{0xff2d2927}
                                      : juce::Colour{0xff282828},
                              area.getX(), area.getY(),
                              primary ? juce::Colour{0xff211d1a}
                                      : juce::Colour{0xff1f1f1f},
                              area.getX(), area.getBottom(), false);
    if (disabled) {
        grad = juce::ColourGradient(juce::Colour{0xff1c1c1c}, area.getX(),
                                    area.getY(), juce::Colour{0xff151515},
                                    area.getX(), area.getBottom(), false);
    }
    g.setGradientFill(grad);
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

void stroke_arc(juce::Graphics& g, juce::Rectangle<float> bounds, float start,
                float end, float thickness) {
    juce::Path arc;
    arc.addArc(bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(),
               start, end, true);
    g.strokePath(arc, juce::PathStrokeType(thickness));
}

void draw_pill(juce::Graphics& g, juce::Rectangle<int> bounds,
               const juce::String& value) {
    auto area = bounds.toFloat().reduced(0.5f);
    g.setColour(juce::Colour{0xff2d2d2d});
    g.fillRoundedRectangle(area, 3.0f);
    g.setColour(juce::Colour{0xff444444});
    g.drawRoundedRectangle(area, 3.0f, 1.0f);
    draw_label(g, bounds.reduced(8, 0), value, 12.0f, text_dim);
}

void draw_meter(juce::Graphics& g, juce::Rectangle<int> bounds, float amount,
                juce::Colour active = accent) {
    auto area = bounds.toFloat();
    g.setColour(juce::Colour{0xff111111});
    g.fillRoundedRectangle(area, 2.0f);

    constexpr int bars = 22;
    constexpr float gap = 2.0f;
    const float bar_w = (area.getWidth() - gap * static_cast<float>(bars - 1)) /
                        static_cast<float>(bars);
    const int active_bars =
        juce::jlimit(0, bars, static_cast<int>(std::ceil(amount * bars)));

    for (int i = 0; i < bars; ++i) {
        const auto x = area.getX() + static_cast<float>(i) * (bar_w + gap);
        g.setColour(i < active_bars ? active : juce::Colour{0xff303030});
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
    lock
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
    draw_icon(g, bounds.removeFromLeft(34).reduced(5), icon);
    draw_label(g, bounds.reduced(0, 0), label, 14.0f, text_dim);
}

void draw_dropdown(juce::Graphics& g, juce::Rectangle<int> bounds,
                   const juce::String& label, Icon icon) {
    auto area = bounds.toFloat().reduced(0.5f);
    g.setColour(juce::Colour{0xff282828});
    g.fillRoundedRectangle(area, radius);
    g.setColour(border);
    g.drawRoundedRectangle(area, radius, 1.0f);
    draw_icon(g, bounds.removeFromLeft(42).reduced(9), icon);
    draw_label(g, bounds.removeFromLeft(bounds.getWidth() - 28), label, 14.0f,
               text);
    draw_icon(g, bounds.reduced(5), Icon::chevron);
}

void draw_title_bar(juce::Graphics& g, juce::Rectangle<int> bounds) {
    g.setColour(title_bar);
    g.fillRect(bounds);
    g.setColour(border_soft);
    g.drawHorizontalLine(bounds.getBottom(), 0.0f, static_cast<float>(bounds.getRight()));
    auto lights = bounds.withTrimmedLeft(24).withWidth(62);
    draw_dot(g, {static_cast<float>(lights.getX() + 10),
                 static_cast<float>(lights.getCentreY())}, juce::Colour{0xffef6155}, 6.0f);
    draw_dot(g, {static_cast<float>(lights.getX() + 32),
                 static_cast<float>(lights.getCentreY())}, juce::Colour{0xffffc85a}, 6.0f);
    draw_dot(g, {static_cast<float>(lights.getX() + 54),
                 static_cast<float>(lights.getCentreY())}, juce::Colour{0xff65cf6d}, 6.0f);
    draw_icon(g, bounds.withTrimmedLeft(bounds.getWidth() - 54).reduced(14),
              Icon::settings, text_dim);
}
} // namespace ui

struct ServerRow {
    const char* name;
    const char* address;
    int ping;
    ui::Icon icon;
    juce::Colour status;
};

struct RoomRow {
    const char* name;
    const char* tag_a;
    const char* tag_b;
    const char* host;
    const char* players;
    bool locked;
    bool disabled;
};

class StyleSandboxComponent final : public juce::Component {
public:
    StyleSandboxComponent() {
        setOpaque(true);
    }

    void paint(juce::Graphics& g) override {
        juce::ColourGradient background(ui::bg_top, 0.0f, 0.0f, ui::bg_bottom,
                                        0.0f, static_cast<float>(getHeight()),
                                        false);
        g.setGradientFill(background);
        g.fillAll();

        auto area = getLocalBounds();
        auto content = area.reduced(10, 10);
        auto bottom = content.removeFromBottom(126);
        content.removeFromBottom(10);
        auto left = content.removeFromLeft(315);
        content.removeFromLeft(10);
        draw_server_panel(g, left);
        draw_rooms_panel(g, content);
        draw_bottom_strip(g, bottom);
    }

private:
    void draw_server_panel(juce::Graphics& g, juce::Rectangle<int> bounds) {
        g.setColour(juce::Colour{0x33101010});
        g.fillRect(bounds);
        g.setColour(ui::border_soft);
        g.drawVerticalLine(bounds.getRight(), static_cast<float>(bounds.getY()),
                           static_cast<float>(bounds.getBottom()));

        auto area = bounds.reduced(12, 12);
        auto header = area.removeFromTop(42);
        ui::draw_label(g, header.removeFromLeft(160), "SERVERS", 15.0f,
                       ui::text, true);
        auto refresh = header.removeFromRight(42).reduced(2);
        ui::fill_button(g, refresh);
        ui::draw_icon(g, refresh.reduced(10), ui::Icon::refresh);
        header.removeFromRight(8);
        auto add_server = header.removeFromRight(42).reduced(2);
        ui::fill_button(g, add_server);
        ui::draw_icon(g, add_server.reduced(10), ui::Icon::plus);

        std::array<ServerRow, 6> servers{{
            {"Local Network", "10.0.0.15", 5, ui::Icon::network, ui::green},
            {"Studio Server", "192.168.1.50", 12, ui::Icon::server, ui::green},
            {"West Coast Hub", "us-west.example.net", 38, ui::Icon::globe, ui::green},
            {"East Coast Hub", "us-east.example.net", 42, ui::Icon::globe, ui::green},
            {"Europe Hub", "eu.example.net", 86, ui::Icon::globe, ui::amber},
            {"Asia Hub", "asia.example.net", 182, ui::Icon::globe, ui::red},
        }};

        for (int i = 0; i < static_cast<int>(servers.size()); ++i) {
            draw_server_row(g, area.removeFromTop(70), servers[static_cast<size_t>(i)],
                            i == 0);
        }

        auto add = bounds.reduced(12, 18).removeFromBottom(42);
        ui::fill_button(g, add);
        ui::draw_icon(g, add.removeFromLeft(44).reduced(10), ui::Icon::plus);
        ui::draw_label(g, add.reduced(2, 0), "Add Server", 14.5f, ui::text_dim);
    }

    void draw_server_row(juce::Graphics& g, juce::Rectangle<int> bounds,
                         const ServerRow& row, bool selected) {
        auto area = bounds.reduced(0, 4);
        auto fill = selected ? ui::row_selected : juce::Colours::transparentBlack;
        if (selected) {
            g.setColour(fill);
            g.fillRoundedRectangle(area.toFloat(), ui::radius);
            g.setColour(ui::accent.withAlpha(0.75f));
            g.drawRoundedRectangle(area.toFloat().reduced(0.5f), ui::radius, 1.0f);
        }
        g.setColour(ui::border_soft);
        g.drawHorizontalLine(bounds.getBottom() - 1,
                             static_cast<float>(bounds.getX() + 4),
                             static_cast<float>(bounds.getRight() - 4));
        ui::draw_icon(g, area.removeFromLeft(58).reduced(12), row.icon, ui::text_dim);
        auto ping = area.removeFromRight(84);
        ui::draw_label(g, ping.removeFromTop(28), juce::String(row.ping) + " ms",
                       14.0f, row.status, false, juce::Justification::centredRight);
        ui::draw_dot(g, {static_cast<float>(ping.getRight() - 8),
                         static_cast<float>(ping.getY() + 12)}, row.status, 4.0f);
        ui::draw_label(g, area.removeFromTop(30), row.name, 16.0f, ui::text, true);
        ui::draw_label(g, area, row.address, 13.5f, ui::text_dim);
    }

    void draw_rooms_panel(juce::Graphics& g, juce::Rectangle<int> bounds) {
        g.setColour(juce::Colour{0x22101010});
        g.fillRect(bounds);

        auto area = bounds.reduced(12, 10);
        auto top = area.removeFromTop(42);
        ui::draw_label(g, top.removeFromLeft(160), "ROOMS", 15.0f, ui::text, true);
        auto create = top.removeFromRight(110).reduced(0, 2);
        ui::fill_button(g, create, true);
        ui::draw_label(g, create, "Create", 14.5f, ui::text, true,
                       juce::Justification::centred);
        top.removeFromRight(10);
        auto search = top.removeFromRight(245).reduced(0, 2);
        ui::fill_button(g, search);
        ui::draw_icon(g, search.removeFromLeft(38).reduced(9), ui::Icon::search);
        ui::draw_label(g, search, "Search rooms", 14.0f, ui::text_dim);
        top.removeFromRight(10);
        ui::draw_toolbar_button(g, top.removeFromRight(90).reduced(0, 2), "Filter",
                                ui::Icon::filter);

        auto header = area.removeFromTop(42);
        g.setColour(juce::Colour{0xff252525});
        g.fillRoundedRectangle(header.toFloat(), ui::radius);
        g.setColour(ui::border_soft);
        g.drawRoundedRectangle(header.toFloat().reduced(0.5f), ui::radius, 1.0f);
        draw_room_header(g, header.reduced(20, 0));

        std::array<RoomRow, 7> rooms{{
            {"Late Night Jam", "Rock", "Open", "M. Stone", "3 / 6", false, false},
            {"Funk House", "Funk", "Invite", "GrooveMaster", "4 / 5", false, false},
            {"Acoustic Circle", "Acoustic", "Open", "PickinPaul", "2 / 4", false, false},
            {"Synthwave Lounge", "Electronic", "Open", "NeonRider", "3 / 6", false, false},
            {"Blues Basement", "Blues", "Invite", "Slowhand", "4 / 4", false, false},
            {"Metal Mayhem", "Metal", "Invite Only", "RiffLord", "6 / 6", true, true},
            {"Jazz Central", "Jazz", "Open", "BlueNote", "1 / 5", false, false},
        }};

        for (int i = 0; i < static_cast<int>(rooms.size()); ++i) {
            draw_room_row(g, area.removeFromTop(66), rooms[static_cast<size_t>(i)]);
        }

        auto scrollbar = bounds.reduced(8, 100).removeFromRight(6);
        g.setColour(juce::Colour{0xff10161a});
        g.fillRoundedRectangle(scrollbar.toFloat(), 3.0f);
        g.setColour(juce::Colour{0xff59656c});
        g.fillRoundedRectangle(scrollbar.removeFromTop(310).reduced(1, 4).toFloat(),
                               3.0f);
    }

    struct RoomColumns {
        int name;
        int host;
        int players;
        int access;
        int join;
    };

    RoomColumns room_columns(int width) const {
        auto host = 205;
        auto players = 145;
        auto access = 165;
        auto join = 180;
        auto name = width - host - players - access - join;

        if (name < 300) {
            const auto deficit = 300 - name;
            host -= juce::jmin(deficit / 2, 45);
            access -= juce::jmin(deficit / 3, 30);
            join -= juce::jmin(deficit / 3, 35);
            name = width - host - players - access - join;
        }

        const auto final_name = juce::jmax(240, name);
        return {final_name, host, players, access,
                juce::jmax(132, width - final_name - host - players - access)};
    }

    void draw_room_header(juce::Graphics& g, juce::Rectangle<int> bounds) {
        const auto columns = room_columns(bounds.getWidth());
        ui::draw_label(g, bounds.removeFromLeft(columns.name), "ROOM NAME", 12.0f,
                       ui::text_dim, true);
        ui::draw_label(g, bounds.removeFromLeft(columns.host), "HOST", 12.0f,
                       ui::text_dim, true);
        ui::draw_label(g, bounds.removeFromLeft(columns.players), "PLAYERS", 12.0f,
                       ui::text_dim, true);
        ui::draw_label(g, bounds.removeFromLeft(columns.access), "ACCESS", 12.0f,
                       ui::text_dim, true);
        ui::draw_label(g, bounds, "JOIN", 12.0f, ui::text_dim, true,
                       juce::Justification::centred);
    }

    void draw_room_row(juce::Graphics& g, juce::Rectangle<int> bounds,
                       const RoomRow& room) {
        auto row = bounds.reduced(0, 3);
        auto row_area = row.toFloat();
        juce::ColourGradient grad(room.disabled ? juce::Colour{0xff1d1d1d}
                                                : ui::row_hover,
                                  row_area.getX(), row_area.getY(),
                                  room.disabled ? juce::Colour{0xff151515}
                                                : ui::row,
                                  row_area.getX(), row_area.getBottom(), false);
        g.setGradientFill(grad);
        g.fillRoundedRectangle(row_area, ui::radius);
        g.setColour(ui::border_soft);
        g.drawRoundedRectangle(row_area.reduced(0.5f), ui::radius, 1.0f);

        auto area = row.reduced(20, 0);
        const auto columns = room_columns(area.getWidth());
        auto name = area.removeFromLeft(columns.name);
        auto title = name.removeFromTop(30);
        ui::draw_label(g, title, room.name, 17.0f,
                       room.disabled ? ui::text_faint : ui::text, true);
        if (room.locked) {
            ui::draw_icon(g, title.withTrimmedLeft(142).withWidth(22).reduced(3),
                          ui::Icon::lock, ui::text_faint);
        }
        auto tags = name.withHeight(22);
        ui::draw_pill(g, tags.removeFromLeft(70), room.tag_a);
        tags.removeFromLeft(6);
        ui::draw_pill(g, tags.removeFromLeft(76), room.tag_b);

        ui::draw_label(g, area.removeFromLeft(columns.host), room.host, 14.5f,
                       room.disabled ? ui::text_faint : ui::text);
        auto players = area.removeFromLeft(columns.players);
        ui::draw_icon(g, players.removeFromLeft(26).reduced(3), ui::Icon::users,
                      ui::text_dim);
        ui::draw_label(g, players, room.players, 14.5f,
                       room.disabled ? ui::text_faint : ui::text);
        ui::draw_label(g, area.removeFromLeft(columns.access),
                       room.locked ? "Invite only" : "Open", 14.0f,
                       room.locked ? ui::amber : ui::green);

        auto join = area.withWidth(columns.join);
        const auto menu_width = 44;
        const auto join_gap = 8;
        const auto button_width =
            juce::jlimit(82, 112, join.getWidth() - menu_width - join_gap);
        auto button = join.removeFromLeft(button_width).reduced(0, 13);
        ui::fill_button(g, button, true, room.disabled);
        ui::draw_label(g, button,
                       room.disabled ? "Full" : "Join", 14.5f,
                       room.disabled ? ui::text_faint : ui::text, true,
                       juce::Justification::centred);
        join.removeFromLeft(join_gap);
        auto menu = join.removeFromLeft(menu_width).reduced(0, 13);
        ui::fill_button(g, menu, false, room.disabled);
        ui::draw_icon(g, menu.reduced(12), ui::Icon::chevron,
                      room.disabled ? ui::text_faint : ui::text_dim);
    }

    void draw_bottom_strip(juce::Graphics& g, juce::Rectangle<int> bounds) {
        auto area = bounds;
        constexpr int gap = 8;
        auto connection = area.removeFromRight(185);
        area.removeFromRight(gap);
        auto audio = area.removeFromLeft(355);
        area.removeFromLeft(gap);
        auto input = area.removeFromLeft(260);
        area.removeFromLeft(gap);
        auto output = area.removeFromLeft(260);
        area.removeFromLeft(gap);
        auto profile = area;

        draw_audio_card(g, audio);
        draw_device_card(g, input, "INPUT", "Scarlett 2i2 USB", ui::Icon::mic);
        draw_device_card(g, output, "OUTPUT", "Scarlett 2i2 USB", ui::Icon::speaker);
        draw_network_card(g, profile);
        draw_session_card(g, connection);
    }

    void draw_audio_card(juce::Graphics& g, juce::Rectangle<int> bounds) {
        ui::fill_panel(g, bounds);
        auto area = bounds.reduced(16, 12);
        ui::draw_label(g, area.removeFromTop(18), "AUDIO", 12.0f, ui::text_dim, true);
        auto content = area.removeFromTop(52);
        ui::draw_icon(g, content.removeFromLeft(44).reduced(4), ui::Icon::wave,
                      ui::green);
        ui::draw_label(g, content.removeFromLeft(130), "Audio Ready", 17.0f,
                       ui::green, true);
        ui::draw_meter(g, content.reduced(4, 13), 0.62f, ui::accent_hi);
        auto scale = area.removeFromTop(22);
        ui::draw_label(g, scale.removeFromLeft(42), "-60", 11.0f, ui::text_faint);
        ui::draw_label(g, scale.removeFromLeft(42), "-48", 11.0f, ui::text_faint);
        ui::draw_label(g, scale.removeFromLeft(42), "-36", 11.0f, ui::text_faint);
        ui::draw_label(g, scale.removeFromLeft(42), "-24", 11.0f, ui::text_faint);
        ui::draw_label(g, scale.removeFromLeft(42), "-12", 11.0f, ui::text_faint);
        ui::draw_label(g, scale.removeFromLeft(34), "0", 11.0f, ui::red);
    }

    void draw_device_card(juce::Graphics& g, juce::Rectangle<int> bounds,
                          const juce::String& title, const juce::String& device,
                          ui::Icon icon) {
        ui::fill_panel(g, bounds);
        auto area = bounds.reduced(16, 12);
        ui::draw_label(g, area.removeFromTop(18), title, 12.0f, ui::text_dim, true);
        auto dropdown = area.removeFromTop(48).reduced(0, 4);
        ui::draw_dropdown(g, dropdown, device, icon);
        ui::draw_label(g, area.removeFromLeft(54), "48 kHz", 12.0f, ui::text_dim);
        ui::draw_label(g, area.removeFromLeft(50), "24-bit", 12.0f, ui::text_dim);
        ui::draw_label(g, area, "2 ch", 12.0f, ui::text_dim);
    }

    void draw_network_card(juce::Graphics& g, juce::Rectangle<int> bounds) {
        ui::fill_panel(g, bounds);
        auto area = bounds.reduced(16, 12);
        ui::draw_label(g, area.removeFromTop(18), "NETWORK", 12.0f,
                       ui::text_dim, true);
        auto row = area.removeFromTop(44).reduced(0, 4);
        ui::draw_icon(g, row.removeFromLeft(34).reduced(7), ui::Icon::wifi,
                      ui::text_dim);
        ui::draw_label(g, row.removeFromLeft(92), "Redundancy", 13.0f, ui::text_dim);
        ui::draw_label(g, row, "Auto", 14.0f, ui::text, true,
                       juce::Justification::centredRight);
        ui::draw_label(g, area.removeFromLeft(82), "Queue", 12.0f, ui::text_dim);
        ui::draw_label(g, area, "8 pkt", 12.0f, ui::text_dim, false,
                       juce::Justification::centredRight);
    }

    void draw_session_card(juce::Graphics& g, juce::Rectangle<int> bounds) {
        ui::fill_panel(g, bounds);
        auto area = bounds.reduced(16, 12);
        ui::draw_label(g, area.removeFromTop(18), "SESSION", 12.0f,
                       ui::text_dim, true);
        auto content = area.removeFromTop(48);
        ui::draw_icon(g, content.removeFromLeft(42).reduced(5), ui::Icon::wifi,
                      ui::green);
        ui::draw_label(g, content.removeFromLeft(94), "Joined", 14.5f,
                       ui::green);
        ui::draw_dot(g, {static_cast<float>(content.getRight() - 12),
                         static_cast<float>(content.getCentreY())}, ui::green, 6.0f);
        ui::draw_label(g, area.removeFromLeft(68), "Room 12", 12.0f, ui::text_dim);
        ui::draw_label(g, area, "RX/TX", 12.0f, ui::text_dim, false,
                       juce::Justification::centredRight);
    }
};

class SandboxWindow final : public juce::DocumentWindow {
public:
    SandboxWindow()
        : DocumentWindow("Sesivo UI Style Sandbox", juce::Colour{0xff181818},
                         juce::DocumentWindow::allButtons) {
        setUsingNativeTitleBar(true);
        setResizable(true, true);
        setResizeLimits(1180, 680, 2000, 1200);
        setContentOwned(new StyleSandboxComponent(), true);
        centreWithSize(1320, 780);
        setVisible(true);
    }

    void closeButtonPressed() override {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

class StyleSandboxApplication final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override {
        return "Sesivo UI Style Sandbox";
    }

    const juce::String getApplicationVersion() override {
        return "0.1";
    }

    bool moreThanOneInstanceAllowed() override {
        return true;
    }

    void initialise(const juce::String&) override {
        window_ = std::make_unique<SandboxWindow>();
    }

    void shutdown() override {
        window_ = nullptr;
    }

    void systemRequestedQuit() override {
        quit();
    }

    void anotherInstanceStarted(const juce::String&) override {}

private:
    std::unique_ptr<SandboxWindow> window_;
};
} // namespace

juce::JUCEApplicationBase* juce_CreateApplication() {
    return new StyleSandboxApplication();
}

int main() {
    juce::JUCEApplicationBase::createInstance = &juce_CreateApplication;
    return juce::JUCEApplicationBase::main();
}
