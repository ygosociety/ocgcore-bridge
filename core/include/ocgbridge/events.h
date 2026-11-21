// SPDX-License-Identifier: AGPL-3.0-or-later
// Engine messages: names and a JSON decoding of the common ones.
//
// A message is one entry of OCG_DuelGetMessage's buffer without its length
// prefix: [type:u8][payload...]. decode_message() gives
//   { msg, name, ...fields }
// with fields for the messages listed in docs/protocol.md ("Events"); any
// other message is { msg, name }. Places are { ctrl, loc, seq, pos } in
// ygopro-core's numbers; 64-bit values are decimal strings.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace ocgbridge {

struct EngineMessage {
    uint8_t type{0};
    const uint8_t* data{nullptr}; // the message, type byte first
    size_t size{0};
    nlohmann::json decoded() const;
};

// "move" for MSG_MOVE, "newTurn" for MSG_NEW_TURN, ...: the MSG_* name in
// lowerCamelCase. "unknown" for a type ygopro-core does not define.
const char* message_name(uint8_t type);
// The reverse; 0 for an unknown name.
uint8_t message_of_name(const std::string& name);

nlohmann::json decode_message(const uint8_t* data, size_t size);

} // namespace ocgbridge
