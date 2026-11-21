// SPDX-License-Identifier: AGPL-3.0-or-later
// Protocol v1 <-> libocgbridge (docs/protocol.md).
#pragma once
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ocgbridge/session.h"

namespace ocgbridge::protocol {

constexpr int VERSION = 1;

// A request the server rejects: { ok: false, error: { code, message } }.
struct Error : std::runtime_error {
    std::string code;
    Error(std::string c, const std::string& message) : std::runtime_error(message), code(std::move(c)) {}
};

// init -> the duel setup, the responder configuration and the session options.
struct InitOptions {
    DuelSetup setup;
    ResponderConfig responder;
    bool chain_start_snapshots{true};
    bool record_messages{false};
    std::string scripts; // empty: keep the server's
    std::optional<std::vector<std::string>> script_dirs;
    std::optional<std::vector<std::string>> preload;
    // init.events: messages sent to the caller as events (all: every one), and
    // messages dropped from everything the caller sees.
    bool events_all{false};
    std::set<uint8_t> events;
    std::set<uint8_t> drop;
};
InitOptions parse_init(const nlohmann::json& msg);
Hints parse_hints(const nlohmann::json& j);
nlohmann::json answered_json(const std::vector<Answered>& log);
const char* status_name(AnswerStatus s);
// A list of message types by name ("move") or number; "*" is every message.
std::set<uint8_t> message_types(const nlohmann::json& j, const char* what, bool* all = nullptr);

} // namespace ocgbridge::protocol
