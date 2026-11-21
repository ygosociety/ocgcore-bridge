// SPDX-License-Identifier: AGPL-3.0-or-later
#include "protocol.h"

using json = nlohmann::json;

namespace ocgbridge::protocol {

namespace {

uint32_t u32(const json& j, const char* key, uint32_t def) {
    if (!j.contains(key) || j[key].is_null()) return def;
    if (!j[key].is_number_unsigned()) throw Error("bad_request", std::string(key) + " must be a non-negative integer");
    return j[key].get<uint32_t>();
}

std::vector<uint32_t> codes(const json& j, const char* key) {
    std::vector<uint32_t> out;
    if (!j.contains(key) || j[key].is_null()) return out;
    if (!j[key].is_array()) throw Error("bad_request", std::string(key) + " must be an array of card codes");
    for (const auto& v : j[key]) {
        if (!v.is_number_unsigned()) throw Error("bad_request", std::string(key) + " must be an array of card codes");
        out.push_back(v.get<uint32_t>());
    }
    return out;
}

ZoneHint zone(const json& j, const char* what) {
    if (!j.is_object()) throw Error("bad_request", std::string(what) + " must be { loc, seq }");
    return ZoneHint{ u32(j, "loc", 0), u32(j, "seq", 0) };
}

std::vector<ZoneHint> zones(const json& j, const char* key) {
    std::vector<ZoneHint> out;
    if (!j.contains(key)) return out;
    if (!j[key].is_array()) throw Error("bad_request", std::string(key) + " must be an array of { loc, seq }");
    for (const auto& z : j[key]) out.push_back(zone(z, key));
    return out;
}

// Prompt kinds by name; "*" is every in-effect kind (all but idle, battle, chain).
std::set<uint8_t> kinds(const json& j, const char* what) {
    std::set<uint8_t> out;
    if (j.is_null()) return out;
    if (!j.is_array()) throw Error("bad_request", std::string(what) + " must be an array of prompt kinds");
    for (const auto& k : j) {
        if (k.is_string() && k.get<std::string>() == "*") {
            const auto& all = in_effect_prompt_msgs();
            out.insert(all.begin(), all.end());
            continue;
        }
        const uint8_t m = k.is_string() ? prompt_msg_of_kind(k.get<std::string>()) : 0;
        if (!m) throw Error("bad_request", std::string("unknown prompt kind in ") + what + ": " + k.dump());
        out.insert(m);
    }
    return out;
}

std::vector<std::string> strings(const json& j, const char* what) {
    if (!j.is_array()) throw Error("bad_request", std::string(what) + " must be an array of strings");
    std::vector<std::string> out;
    for (const auto& v : j) {
        if (!v.is_string()) throw Error("bad_request", std::string(what) + " must be an array of strings");
        out.push_back(v.get<std::string>());
    }
    return out;
}

PlayerSetup player(const json& p) {
    if (!p.is_object()) throw Error("bad_request", "each player must be an object");
    PlayerSetup out;
    out.main = codes(p, "main");
    out.extra = codes(p, "extra");
    out.lp = u32(p, "lp", out.lp);
    out.starting_hand = u32(p, "startingHand", out.starting_hand);
    out.draw_per_turn = u32(p, "drawPerTurn", out.draw_per_turn);
    if (p.contains("board")) {
        if (!p["board"].is_array()) throw Error("bad_request", "board must be an array of cards");
        for (const auto& c : p["board"]) {
            if (!c.is_object()) throw Error("bad_request", "board must be an array of cards");
            BoardCard b;
            b.code = u32(c, "code", 0);
            b.loc = u32(c, "loc", 0);
            b.seq = u32(c, "seq", 0);
            b.pos = u32(c, "pos", 0);
            b.materials = codes(c, "materials");
            out.board.push_back(std::move(b));
        }
    }
    return out;
}

} // namespace

std::set<uint8_t> message_types(const json& j, const char* what, bool* all) {
    std::set<uint8_t> out;
    if (j.is_null()) return out;
    if (!j.is_array()) throw Error("bad_request", std::string(what) + " must be an array of message names or numbers");
    for (const auto& v : j) {
        if (v.is_string() && v.get<std::string>() == "*") {
            if (!all) throw Error("bad_request", std::string("\"*\" is not allowed in ") + what);
            *all = true;
            continue;
        }
        uint8_t t = 0;
        if (v.is_string()) t = message_of_name(v.get<std::string>());
        else if (v.is_number_unsigned() && v.get<uint64_t>() <= 255) t = static_cast<uint8_t>(v.get<uint64_t>());
        if (!t) throw Error("bad_request", std::string("unknown message in ") + what + ": " + v.dump());
        out.insert(t);
    }
    return out;
}

InitOptions parse_init(const json& msg) {
    InitOptions o;
    if (msg.contains("scripts")) {
        if (!msg["scripts"].is_string()) throw Error("bad_request", "scripts must be a directory path");
        o.scripts = msg["scripts"].get<std::string>();
    }
    DuelSetup& s = o.setup;
    if (msg.contains("seed") && !msg["seed"].is_null()) {
        const json& seed = msg["seed"];
        if (seed.is_number_unsigned()) {
            s.seed = expand_seed(seed.get<uint64_t>());
        } else if (seed.is_array() && seed.size() == 4) {
            for (size_t i = 0; i < 4; ++i) {
                if (!seed[i].is_number_unsigned()) throw Error("bad_request", "seed words must be non-negative integers");
                s.seed[i] = seed[i].get<uint64_t>();
            }
        } else {
            throw Error("bad_request", "seed must be a non-negative integer or an array of 4");
        }
    }
    if (msg.contains("rules") && !msg["rules"].is_null()) {
        auto f = msg["rules"].is_string() ? rules_flags(msg["rules"].get<std::string>()) : std::nullopt;
        if (!f) throw Error("bad_request", "unknown rules: " + msg["rules"].dump());
        s.flags = *f;
        s.flags_set = true;
    }
    if (msg.contains("flags") && !msg["flags"].is_null()) {
        if (!msg["flags"].is_number_unsigned()) throw Error("bad_request", "flags must be a non-negative integer");
        s.flags = (s.flags_set ? s.flags : 0) | msg["flags"].get<uint64_t>();
        s.flags_set = true;
    }
    if (msg.contains("pseudoShuffle")) s.pseudo_shuffle = msg["pseudoShuffle"].get<bool>();
    if (msg.contains("shuffleDecks")) s.shuffle_decks = msg["shuffleDecks"].get<bool>();
    if (msg.contains("attackFirstTurn")) s.attack_first_turn = msg["attackFirstTurn"].get<bool>();
    const bool puzzle = msg.contains("puzzleScript") && msg["puzzleScript"].is_string();
    if (puzzle && !msg.contains("players")) {
        // A puzzle script sets up both players itself.
    } else if (!msg.contains("players") || !msg["players"].is_array() || msg["players"].size() != 2) {
        throw Error("bad_request", "players must be an array of two players");
    } else {
        s.players[0] = player(msg["players"][0]);
        s.players[1] = player(msg["players"][1]);
    }
    if (msg.contains("scriptOverrides")) {
        if (!msg["scriptOverrides"].is_object()) throw Error("bad_request", "scriptOverrides must map card codes to files");
        for (const auto& [code, file] : msg["scriptOverrides"].items()) {
            if (!file.is_string()) continue;
            try { s.script_overrides[static_cast<uint32_t>(std::stoul(code))] = file.get<std::string>(); }
            catch (...) { throw Error("bad_request", "scriptOverrides keys must be card codes"); }
        }
    }
    if (msg.contains("puzzleScript") && !msg["puzzleScript"].is_null()) {
        if (!msg["puzzleScript"].is_string()) throw Error("bad_request", "puzzleScript must be a file path");
        s.puzzle_script = msg["puzzleScript"].get<std::string>();
    }
    if (msg.contains("responder")) {
        const json& r = msg["responder"];
        if (!r.is_object()) throw Error("bad_request", "responder must be an object");
        if (r.contains("sum")) {
            const std::string v = r["sum"].is_string() ? r["sum"].get<std::string>() : "";
            if (v == "exact") o.responder.sum_solver = SumSolver::Exact;
            else if (v == "checked") o.responder.sum_solver = SumSolver::Checked;
            else throw Error("bad_request", "responder.sum must be \"exact\" or \"checked\"");
        }
        if (r.contains("ask")) {
            if (!r["ask"].is_array() || r["ask"].size() > 2) throw Error("bad_request", "responder.ask must be [kinds of player 0, kinds of player 1]");
            for (size_t pl = 0; pl < r["ask"].size(); ++pl) o.responder.ask_msgs[pl] = kinds(r["ask"][pl], "responder.ask");
        }
        if (r.contains("holdKinds")) o.responder.hold_msgs = kinds(r["holdKinds"], "responder.holdKinds");
        if (r.contains("holdTrivial")) o.responder.hold_trivial = r["holdTrivial"].get<bool>();
        if (r.contains("effectYesNoRepeatLimit")) o.responder.effectyn_repeat_limit = r["effectYesNoRepeatLimit"].get<int>();
        if (r.contains("manual")) o.responder.manual_msgs = kinds(r["manual"], "responder.manual");
        if (r.contains("defaults")) {
            if (!r["defaults"].is_object()) throw Error("bad_request", "responder.defaults must map prompt kinds to answers");
            for (const auto& [kind, answer] : r["defaults"].items()) {
                const uint8_t m = prompt_msg_of_kind(kind);
                if (!m) throw Error("bad_request", "unknown prompt kind in responder.defaults: " + kind);
                if (!answer.is_null()) o.responder.default_answers[m] = answer;
            }
        }
    }
    if (msg.contains("chainStartSnapshots")) o.chain_start_snapshots = msg["chainStartSnapshots"].get<bool>();
    if (msg.contains("recordMessages")) o.record_messages = msg["recordMessages"].get<bool>();
    if (msg.contains("scriptDirs") && !msg["scriptDirs"].is_null()) o.script_dirs = strings(msg["scriptDirs"], "scriptDirs");
    if (msg.contains("preload") && !msg["preload"].is_null()) o.preload = strings(msg["preload"], "preload");
    if (msg.contains("events") && !msg["events"].is_null()) {
        const json& e = msg["events"];
        if (!e.is_object()) throw Error("bad_request", "events must be { subscribe, drop }");
        if (e.contains("subscribe")) o.events = message_types(e["subscribe"], "events.subscribe", &o.events_all);
        if (e.contains("drop")) o.drop = message_types(e["drop"], "events.drop");
    }
    return o;
}

Hints parse_hints(const json& j) {
    Hints h;
    if (j.is_null()) return h;
    if (!j.is_object()) throw Error("bad_request", "hints must be an object");
    if (j.contains("place") && !j["place"].is_null()) h.place = zone(j["place"], "place");
    h.tributes = zones(j, "tributes");
    h.materials = zones(j, "materials");
    if (j.contains("attackTarget") && !j["attackTarget"].is_null()) h.attack_target = zone(j["attackTarget"], "attackTarget");
    if (j.contains("deferCardSelect")) h.defer_card_select = j["deferCardSelect"].get<bool>();
    if (j.contains("ask")) h.ask = j["ask"].get<bool>();
    if (j.contains("autorespond")) h.autorespond = j["autorespond"].get<bool>();
    if (j.contains("hold")) {
        if (!j["hold"].is_array()) throw Error("bad_request", "hold must be an array of players");
        for (const auto& p : j["hold"]) {
            if (!p.is_number_integer() || p.get<int>() < 0 || p.get<int>() > 1) throw Error("bad_request", "hold players are 0 or 1");
            h.hold_players |= static_cast<uint8_t>(1u << p.get<int>());
        }
    }
    return h;
}

json answered_json(const std::vector<Answered>& log) {
    json out = json::array();
    for (const auto& a : log)
        out.push_back({{"msg", a.msg}, {"kind", prompt_kind(a.msg)}, {"player", a.player}, {"raw", to_hex(a.raw)},
                       {"directed", a.directed}, {"asked", a.asked}, {"replaced", a.replaced}});
    return out;
}

const char* status_name(AnswerStatus s) {
    switch (s) {
        case AnswerStatus::Answered: return "answered";
        case AnswerStatus::Held: return "held";
        default: return "none";
    }
}

} // namespace ocgbridge::protocol
