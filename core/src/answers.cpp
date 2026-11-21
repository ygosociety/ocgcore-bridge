// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ocgbridge/answers.h"

#include <cstring>
#include <set>

#include "ocgapi_constants.h"
#include "ocgbridge/messages.h"

using json = nlohmann::json;

namespace ocgbridge {

namespace {

bool read_int(const json& data, const char* key, int64_t& out) {
    if (!data.is_object() || !data.contains(key) || !data[key].is_number_integer()) return false;
    out = data[key].get<int64_t>();
    return true;
}

void put_i32(std::vector<uint8_t>& out, int32_t v) {
    out.resize(4);
    std::memcpy(out.data(), &v, 4);
}

// Zone bit in a SELECT_PLACE flag (bit SET = unavailable), relative to the prompt's player.
uint32_t place_bit(int prompt_player, int sel_player, int loc, int seq) {
    uint32_t bit = 1u << seq;
    if (loc == LOCATION_SZONE) bit <<= 8;
    if (sel_player != prompt_player) bit <<= 16;
    return bit;
}

int popcount64(uint64_t v) { int c = 0; for (; v; v &= v - 1) ++c; return c; }

bool encode_idle(const std::vector<uint8_t>& prompt, const json& a, std::vector<uint8_t>& out, std::string& reason) {
    IdleData d = parse_idle_data(prompt);
    const std::string action = a.is_object() ? a.value("action", std::string{}) : std::string{};
    struct L { const char* name; int32_t code; const std::vector<CardRef>* list; };
    const L lists[] = {
        {"summon", 0, &d.summonable}, {"spsummon", 1, &d.sp_summon}, {"repos", 2, &d.reposition},
        {"mset", 3, &d.mset}, {"sset", 4, &d.sset}, {"activate", 5, &d.activatable},
    };
    for (const auto& l : lists) {
        if (action != l.name) continue;
        int64_t i;
        if (!read_int(a, "index", i)) { reason = "missing index"; return false; }
        if (i < 0 || i >= (int64_t)l.list->size()) { reason = "index out of range"; return false; }
        put_i32(out, l.code | static_cast<int32_t>(i << 16));
        return true;
    }
    if (action == "battle") { if (!d.to_bp) { reason = "can't enter the Battle Phase"; return false; } put_i32(out, 6); return true; }
    if (action == "end") { if (!d.to_ep) { reason = "can't enter the End Phase"; return false; } put_i32(out, 7); return true; }
    if (action == "shuffle") { if (!d.can_shuffle) { reason = "can't shuffle the hand"; return false; } put_i32(out, 8); return true; }
    reason = "unknown idle action";
    return false;
}

bool encode_battle(const std::vector<uint8_t>& prompt, const json& a, std::vector<uint8_t>& out, std::string& reason) {
    BattleData d = parse_battle_data(prompt);
    const std::string action = a.is_object() ? a.value("action", std::string{}) : std::string{};
    if (action == "activate" || action == "attack") {
        const auto& list = action == "activate" ? d.activatable : d.attackable;
        int64_t i;
        if (!read_int(a, "index", i)) { reason = "missing index"; return false; }
        if (i < 0 || i >= (int64_t)list.size()) { reason = "index out of range"; return false; }
        put_i32(out, (action == "activate" ? 0 : 1) | static_cast<int32_t>(i << 16));
        return true;
    }
    if (action == "main2") { if (!d.to_m2) { reason = "can't enter Main Phase 2"; return false; } put_i32(out, 2); return true; }
    if (action == "end") { if (!d.to_ep) { reason = "can't enter the End Phase"; return false; } put_i32(out, 3); return true; }
    reason = "unknown battle action";
    return false;
}

bool encode_place(const PromptData& d, const json& data, std::vector<uint8_t>& out, std::string& reason) {
    std::vector<json> places;
    if (data.contains("places") && data["places"].is_array())
        for (const auto& p : data["places"]) places.push_back(p);
    else
        places.push_back(data);
    if ((int)places.size() != (d.count ? d.count : 1)) { reason = "wrong number of zones"; return false; }
    uint32_t flag = d.flag;
    for (const auto& p : places) {
        int64_t pl, loc, seq;
        if (!read_int(p, "player", pl) || !read_int(p, "loc", loc) || !read_int(p, "seq", seq)) {
            reason = "zone needs player, loc and seq"; return false;
        }
        if (pl < 0 || pl > 1 || (loc != LOCATION_MZONE && loc != LOCATION_SZONE)) { reason = "bad zone"; return false; }
        if (seq < 0 || seq > (loc == LOCATION_MZONE ? 6 : 7)) { reason = "bad zone"; return false; }
        uint32_t bit = place_bit(d.player, (int)pl, (int)loc, (int)seq);
        if (bit & flag) { reason = "zone not available"; return false; }
        flag |= bit;
        out.push_back(static_cast<uint8_t>(pl));
        out.push_back(static_cast<uint8_t>(loc));
        out.push_back(static_cast<uint8_t>(seq));
    }
    return true;
}

} // namespace

bool encode_answer(const std::vector<uint8_t>& prompt, const json& data, std::vector<uint8_t>& out, std::string& reason) {
    out.clear();
    if (prompt.size() < 2) { reason = "no prompt to answer"; return false; }
    // Typed answers of a non-object are rejected by their own checks below.
    if (data.is_object() && data.contains("raw")) {
        if (!data["raw"].is_string() || !from_hex(data["raw"].get<std::string>(), out)) { reason = "raw must be a hex string"; return false; }
        return true;
    }
    if (data.is_object() && data.contains("i32")) {
        int64_t v;
        if (!read_int(data, "i32", v) || v < INT32_MIN || v > INT32_MAX) { reason = "i32 must be a 32-bit integer"; return false; }
        put_i32(out, static_cast<int32_t>(v));
        return true;
    }
    const uint8_t msg = prompt[0];
    PromptData d = parse_prompt(prompt);
    switch (msg) {
        case MSG_SELECT_IDLECMD: return encode_idle(prompt, data, out, reason);
        case MSG_SELECT_BATTLECMD: return encode_battle(prompt, data, out, reason);
        case MSG_SELECT_CHAIN: {
            ChainData cd = parse_chain_data(prompt);
            int64_t i;
            if (!read_int(data, "index", i)) { reason = "missing index"; return false; }
            if (i == -1) {
                if (cd.forced) { reason = "this chain window is forced"; return false; }
            } else if (i < 0 || i >= (int64_t)cd.candidates.size()) { reason = "index out of range"; return false; }
            put_i32(out, static_cast<int32_t>(i));
            return true;
        }
        case MSG_SELECT_CARD:
        case MSG_SELECT_TRIBUTE: {
            if (!data.contains("indices") || !data["indices"].is_array()) { reason = "missing indices"; return false; }
            std::vector<uint8_t> idxs;
            std::set<int64_t> seen;
            uint32_t release_sum = 0;
            for (const auto& v : data["indices"]) {
                if (!v.is_number_integer()) { reason = "bad index"; return false; }
                int64_t i = v.get<int64_t>();
                if (i < 0 || i >= (int64_t)d.cards.size() || i > 255) { reason = "index out of range"; return false; }
                if (!seen.insert(i).second) { reason = "duplicate index"; return false; }
                idxs.push_back(static_cast<uint8_t>(i));
                if (msg == MSG_SELECT_TRIBUTE) release_sum += d.release[static_cast<size_t>(i)];
            }
            if (idxs.empty() && d.cancelable) { put_i32(out, -1); return true; }
            if (idxs.size() > d.max) { reason = "too many cards"; return false; }
            if (msg == MSG_SELECT_CARD ? idxs.size() < d.min : release_sum < d.min) { reason = "too few cards"; return false; }
            out = encode_card_indices(idxs);
            return true;
        }
        case MSG_SELECT_SUM: {
            if (!data.contains("indices") || !data["indices"].is_array()) { reason = "missing indices"; return false; }
            SelectSumData sd = parse_select_sum_data(prompt);
            std::vector<uint8_t> idxs;
            for (const auto& v : data["indices"]) {
                if (!v.is_number_integer()) { reason = "bad index"; return false; }
                int64_t i = v.get<int64_t>();
                if (i < 0 || i >= (int64_t)sd.select.size() || i > 255) { reason = "index out of range"; return false; }
                idxs.push_back(static_cast<uint8_t>(i));
            }
            if (!select_sum_accepts(sd, idxs)) { reason = "cards don't make the sum"; return false; }
            out = encode_card_indices(idxs);
            return true;
        }
        case MSG_SELECT_UNSELECT_CARD: {
            int64_t i;
            if (!read_int(data, "index", i)) { reason = "missing index"; return false; }
            if (i == -1) {
                if (!d.finishable && !d.cancelable) { reason = "can't finish yet"; return false; }
                put_i32(out, -1);
                return true;
            }
            if (i < 0 || i >= (int64_t)d.cards.size()) { reason = "index out of range"; return false; }
            int32_t resp[2] = { 1, static_cast<int32_t>(i) };
            out.resize(8);
            std::memcpy(out.data(), resp, 8);
            return true;
        }
        case MSG_SELECT_EFFECTYN:
        case MSG_SELECT_YESNO: {
            int64_t yes;
            const bool is_bool = data.contains("yes") && data["yes"].is_boolean();
            if (!read_int(data, "yes", yes) && !is_bool) { reason = "missing yes"; return false; }
            if (is_bool) yes = data["yes"].get<bool>() ? 1 : 0;
            if (yes != 0 && yes != 1) { reason = "yes must be 0 or 1"; return false; }
            put_i32(out, static_cast<int32_t>(yes));
            return true;
        }
        case MSG_SELECT_OPTION: {
            int64_t o;
            if (!read_int(data, "option", o)) { reason = "missing option"; return false; }
            if (o < 0 || o >= (int64_t)d.options.size()) { reason = "option out of range"; return false; }
            put_i32(out, static_cast<int32_t>(o));
            return true;
        }
        case MSG_SELECT_POSITION: {
            int64_t pos;
            if (!read_int(data, "position", pos)) { reason = "missing position"; return false; }
            if ((pos != 0x1 && pos != 0x2 && pos != 0x4 && pos != 0x8) || !(pos & d.positions)) {
                reason = "position not offered"; return false;
            }
            put_i32(out, static_cast<int32_t>(pos));
            return true;
        }
        case MSG_SELECT_PLACE:
        case MSG_SELECT_DISFIELD:
            return encode_place(d, data, out, reason);
        case MSG_SELECT_COUNTER: {
            SelectCounterData cd = parse_select_counter_data(prompt);
            if (!data.is_object() || !data.contains("counts") || !data["counts"].is_array() || data["counts"].size() != cd.candidates.size()) {
                reason = "counts needs one number per candidate"; return false;
            }
            int64_t total = 0;
            std::vector<int16_t> counts;
            for (size_t i = 0; i < cd.candidates.size(); ++i) {
                const auto& v = data["counts"][i];
                if (!v.is_number_integer()) { reason = "bad count"; return false; }
                int64_t n = v.get<int64_t>();
                if (n < 0 || n > cd.candidates[i].current_count) { reason = "count out of range"; return false; }
                total += n;
                counts.push_back(static_cast<int16_t>(n));
            }
            if (total != cd.count) { reason = "counts must add up to " + std::to_string(cd.count); return false; }
            out.resize(counts.size() * 2);
            if (!counts.empty()) std::memcpy(out.data(), counts.data(), out.size());
            return true;
        }
        case MSG_SORT_CARD:
        case MSG_SORT_CHAIN: {
            uint32_t n = 0;
            if (prompt.size() >= 6) std::memcpy(&n, prompt.data() + 2, 4);
            if (!data.is_object() || !data.contains("order") || data["order"].is_null()) { out = { 0xFF }; return true; }
            if (!data["order"].is_array() || data["order"].size() != n) { reason = "order needs one position per card"; return false; }
            std::set<int64_t> seen;
            for (const auto& v : data["order"]) {
                if (!v.is_number_integer()) { reason = "bad position"; return false; }
                int64_t i = v.get<int64_t>();
                if (i < 0 || i >= (int64_t)n || !seen.insert(i).second) { reason = "order must be a permutation"; return false; }
                out.push_back(static_cast<uint8_t>(i));
            }
            return true;
        }
        case MSG_ANNOUNCE_ATTRIB:
        case MSG_ANNOUNCE_RACE: {
            const int count = prompt.size() > 2 ? prompt[2] : 1;
            uint64_t available = 0;
            if (msg == MSG_ANNOUNCE_ATTRIB && prompt.size() >= 7) { uint32_t a; std::memcpy(&a, prompt.data() + 3, 4); available = a; }
            if (msg == MSG_ANNOUNCE_RACE && prompt.size() >= 11) std::memcpy(&available, prompt.data() + 3, 8);
            uint64_t value = 0;
            if (data.is_object() && data.contains("value") && data["value"].is_string()) {
                try { value = std::stoull(data["value"].get<std::string>()); } catch (...) { reason = "bad value"; return false; }
            } else {
                int64_t v;
                if (!read_int(data, "value", v) || v < 0) { reason = "missing value"; return false; }
                value = static_cast<uint64_t>(v);
            }
            if (popcount64(value) != count || (value & ~available)) { reason = "value must be " + std::to_string(count) + " of the offered bits"; return false; }
            if (msg == MSG_ANNOUNCE_ATTRIB) { put_i32(out, static_cast<int32_t>(value)); return true; }
            out.resize(8);
            std::memcpy(out.data(), &value, 8);
            return true;
        }
        case MSG_ANNOUNCE_CARD: {
            int64_t code;
            if (!read_int(data, "code", code) || code <= 0 || code > INT32_MAX) { reason = "missing code"; return false; }
            put_i32(out, static_cast<int32_t>(code));
            return true;
        }
        case MSG_ANNOUNCE_NUMBER: {
            int64_t i;
            const int64_t n = prompt.size() > 2 ? prompt[2] : 0;
            if (!read_int(data, "index", i)) { reason = "missing index"; return false; }
            if (i < 0 || i >= n) { reason = "index out of range"; return false; }
            put_i32(out, static_cast<int32_t>(i));
            return true;
        }
        case MSG_ROCK_PAPER_SCISSORS: {
            int64_t h;
            if (!read_int(data, "hand", h) || h < 1 || h > 3) { reason = "hand must be 1, 2 or 3"; return false; }
            put_i32(out, static_cast<int32_t>(h));
            return true;
        }
        default:
            reason = "no prompt to answer";
            return false;
    }
}

} // namespace ocgbridge
