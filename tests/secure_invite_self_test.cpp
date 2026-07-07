#include "secure_invite.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_round_trip() {
    const auto link = make_secure_invite_link("example.com", 9999, "room a");

    std::string reason;
    const auto invite = parse_secure_invite_text("open " + link, reason);
    require(invite.has_value(), "invite link should parse");
    require(invite->server_address == "example.com", "server should round-trip");
    require(invite->server_port == 9999, "port should round-trip");
    require(invite->room_id == "room a", "room should round-trip");
    require(link.find("invite=") == std::string::npos,
            "shortcut invite should not contain media key material");
}

void test_detects_incomplete_launch() {
    require(contains_secure_invite_link(
                "\"SESIVO://join?server=example.com&port=9999\""),
            "launch detector should accept incomplete join urls");
    require(!contains_secure_invite_link("https://example.com/invite/123"),
            "launch detector should ignore non-sesivo urls");
}

void test_join_slash_form_parses() {
    std::string reason;
    const auto invite = parse_secure_invite_text(
        "sesivo://join/?server=example.com&port=9999&room=room-a",
        reason);
    require(invite.has_value(), "slash invite URL should parse");
    require(invite->room_id == "room-a", "slash invite room should parse");
}

void test_rejects_incomplete_links() {
    std::string reason;
    const auto invite =
        parse_secure_invite_text("sesivo://join?server=example.com&port=9999",
                                 reason);
    require(!invite.has_value(), "incomplete invite should reject");
    require(!reason.empty(), "rejection should explain why");
}

void test_room_match() {
    SecureInvite invite;
    invite.server_address = "127.0.0.1";
    invite.server_port = 9999;
    invite.room_id = "room-a";
    require(invite_matches_room(invite, "127.0.0.1", 9999, "room-a"),
            "matching room should accept");
    require(!invite_matches_room(invite, "127.0.0.1", 9998, "room-a"),
            "different port should reject");
    require(!invite_matches_room(invite, "127.0.0.1", 9999, "room-b"),
            "different room should reject");
}

}  // namespace

int main() {
    test_round_trip();
    test_detects_incomplete_launch();
    test_join_slash_form_parses();
    test_rejects_incomplete_links();
    test_room_match();
    std::cout << "secure invite self-test passed\n";
    return 0;
}
