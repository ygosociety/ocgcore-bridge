// SPDX-License-Identifier: AGPL-3.0-or-later
// Answers as JSON -> the engine's response bytes, checked against the prompt.
//
// Every prompt kind accepts:
//   { "raw": "<hex>" }   the response bytes as is (not checked)
//   { "i32": n }         a 32-bit response (not checked)
// and its own typed form (see docs/protocol.md, "Answers"):
//   idle      { action: "summon"|"spsummon"|"repos"|"mset"|"sset"|"activate", index }
//             | { action: "battle"|"end"|"shuffle" }
//   battle    { action: "activate"|"attack", index } | { action: "main2"|"end" }
//   chain     { index }            (-1 passes; not when forced)
//   card, tribute, sum { indices } ([] cancels a cancelable card / tribute prompt)
//   unselectCard { index }         (-1 finishes / cancels)
//   effectYesNo, yesNo { yes: bool | 0 | 1 }
//   option    { option }           an index into options
//   position  { position }         one POS_* bit on offer
//   place, disfield { player, loc, seq } | { places: [{ player, loc, seq }, ...] }
//   counter   { counts: [n, ...] } counters to remove, per candidate
//   sortCard, sortChain { order: [i, ...] | null }   null keeps the engine's order
//   announceRace, announceAttribute { value }        a bitmask of `count` bits
//   announceCard { code }
//   announceNumber { index }
//   rps       { hand: 1 | 2 | 3 }  rock, paper, scissors
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ocgbridge {

// `prompt`: the pending prompt message (Session::pending_buf()). false with
// `reason` when the answer is not a legal one for it.
bool encode_answer(const std::vector<uint8_t>& prompt, const nlohmann::json& answer,
                   std::vector<uint8_t>& out, std::string& reason);

} // namespace ocgbridge
