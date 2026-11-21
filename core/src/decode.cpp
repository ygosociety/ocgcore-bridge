// SPDX-License-Identifier: AGPL-3.0-or-later
// The session's state as JSON: the pending prompt, the field, a player's view.
#include "internal.h"

#include <cstring>

namespace ocgbridge {

uint8_t prompt_msg_of_kind(const std::string& kind) {
    static const std::pair<const char*, uint8_t> KINDS[] = {
        {"idle", MSG_SELECT_IDLECMD}, {"battle", MSG_SELECT_BATTLECMD}, {"chain", MSG_SELECT_CHAIN},
        {"card", MSG_SELECT_CARD}, {"tribute", MSG_SELECT_TRIBUTE}, {"unselectCard", MSG_SELECT_UNSELECT_CARD},
        {"effectYesNo", MSG_SELECT_EFFECTYN}, {"yesNo", MSG_SELECT_YESNO}, {"option", MSG_SELECT_OPTION},
        {"position", MSG_SELECT_POSITION}, {"place", MSG_SELECT_PLACE}, {"disfield", MSG_SELECT_DISFIELD},
        {"sum", MSG_SELECT_SUM}, {"counter", MSG_SELECT_COUNTER}, {"sortCard", MSG_SORT_CARD},
        {"sortChain", MSG_SORT_CHAIN}, {"announceRace", MSG_ANNOUNCE_RACE},
        {"announceAttribute", MSG_ANNOUNCE_ATTRIB}, {"announceCard", MSG_ANNOUNCE_CARD},
        {"announceNumber", MSG_ANNOUNCE_NUMBER}, {"rps", MSG_ROCK_PAPER_SCISSORS},
    };
    for (const auto& [name, msg] : KINDS)
        if (kind == name) return msg;
    return 0;
}

const char* prompt_kind(uint8_t msg) {
    switch (msg) {
        case MSG_SELECT_IDLECMD:       return "idle";
        case MSG_SELECT_BATTLECMD:     return "battle";
        case MSG_SELECT_CHAIN:         return "chain";
        case MSG_SELECT_CARD:          return "card";
        case MSG_SELECT_TRIBUTE:       return "tribute";
        case MSG_SELECT_UNSELECT_CARD: return "unselectCard";
        case MSG_SELECT_EFFECTYN:      return "effectYesNo";
        case MSG_SELECT_YESNO:         return "yesNo";
        case MSG_SELECT_OPTION:        return "option";
        case MSG_SELECT_POSITION:      return "position";
        case MSG_SELECT_PLACE:         return "place";
        case MSG_SELECT_DISFIELD:      return "disfield";
        case MSG_SELECT_SUM:           return "sum";
        case MSG_SELECT_COUNTER:       return "counter";
        case MSG_SORT_CARD:            return "sortCard";
        case MSG_SORT_CHAIN:           return "sortChain";
        case MSG_ANNOUNCE_RACE:        return "announceRace";
        case MSG_ANNOUNCE_ATTRIB:      return "announceAttribute";
        case MSG_ANNOUNCE_CARD:        return "announceCard";
        case MSG_ANNOUNCE_NUMBER:      return "announceNumber";
        case MSG_ROCK_PAPER_SCISSORS:  return "rps";
        default:                       return "other";
    }
}

static json card_ref_json(const CardRef& c) {
    return {{"code", c.code}, {"ctrl", c.ctrl}, {"loc", c.loc}, {"seq", c.seq}, {"pos", c.pos}};
}

static json card_refs_json(const std::vector<CardRef>& list) {
    json arr = json::array();
    for (const auto& c : list) arr.push_back(card_ref_json(c));
    return arr;
}

// An activation list: each entry also carries its effect's description
// ("desc", a decimal string: a u64 does not fit a JavaScript number).
static json activation_refs_json(const std::vector<CardRef>& list) {
    json arr = card_refs_json(list);
    for (size_t i = 0; i < list.size(); i++) arr[i]["desc"] = std::to_string(list[i].desc);
    return arr;
}

json Session::prompt_json() const {
    if (pending_msg_ == 0) return nullptr;
    const auto& buf = pending_buf_;
    json out = {
        {"msg", (int)pending_msg_},
        {"kind", prompt_kind(pending_msg_)},
        {"player", pending_player_},
        {"hint", prompt_hint_},
        {"raw", to_hex(buf)},
    };
    switch (pending_msg_) {
        case MSG_SELECT_IDLECMD: {
            IdleData d = parse_idle_data(buf);
            out["summonable"] = card_refs_json(d.summonable);
            out["spSummon"] = card_refs_json(d.sp_summon);
            out["reposition"] = card_refs_json(d.reposition);
            out["mset"] = card_refs_json(d.mset);
            out["sset"] = card_refs_json(d.sset);
            out["activatable"] = activation_refs_json(d.activatable);
            out["activatableSpeed"] = select_chain_speeds();
            out["toBattle"] = d.to_bp;
            out["toEnd"] = d.to_ep;
            out["canShuffle"] = d.can_shuffle;
            break;
        }
        case MSG_SELECT_BATTLECMD: {
            BattleData d = parse_battle_data(buf);
            out["attackable"] = card_refs_json(d.attackable);
            out["activatable"] = activation_refs_json(d.activatable);
            out["toMain2"] = d.to_m2;
            out["toEnd"] = d.to_ep;
            break;
        }
        case MSG_SELECT_CHAIN: {
            ChainData d = parse_chain_data(buf);
            out["activatable"] = activation_refs_json(d.candidates);
            out["forced"] = d.forced;
            out["speCount"] = d.spe_count;
            out["responding"] = d.spe_count != 0;
            out["triggers"] = select_chain_trigger_flags();
            out["timing"] = d.hint_timing;
            out["timingOpponent"] = d.hint_timing_opp;
            break;
        }
        case MSG_SELECT_CARD:
        case MSG_SELECT_TRIBUTE:
        case MSG_SELECT_UNSELECT_CARD: {
            PromptData d = parse_prompt(buf);
            json cands = json::array();
            for (size_t i = 0; i < d.cards.size(); i++) {
                json c = card_ref_json(d.cards[i]);
                if (pending_msg_ == MSG_SELECT_TRIBUTE) c["release"] = d.release[i];
                if (pending_msg_ == MSG_SELECT_UNSELECT_CARD) c["selected"] = i >= d.select_count;
                cands.push_back(c);
            }
            out["candidates"] = cands;
            out["min"] = d.min;
            out["max"] = d.max;
            out["cancelable"] = d.cancelable;
            if (pending_msg_ == MSG_SELECT_UNSELECT_CARD) out["finishable"] = d.finishable;
            break;
        }
        case MSG_SELECT_EFFECTYN: {
            PromptData d = parse_prompt(buf);
            out["code"] = d.code;
            out["card"] = card_ref_json(d.card);
            out["description"] = std::to_string(d.description);
            break;
        }
        case MSG_SELECT_YESNO:
            out["description"] = std::to_string(parse_prompt(buf).description);
            break;
        case MSG_SELECT_OPTION: {
            json opts = json::array();
            for (uint64_t o : parse_prompt(buf).options) opts.push_back(std::to_string(o));
            out["options"] = opts;
            break;
        }
        case MSG_SELECT_POSITION: {
            PromptData d = parse_prompt(buf);
            out["code"] = d.code;
            out["positions"] = d.positions;
            break;
        }
        case MSG_SELECT_PLACE:
        case MSG_SELECT_DISFIELD: {
            PromptData d = parse_prompt(buf);
            out["flag"] = d.flag;
            out["count"] = d.count;
            break;
        }
        case MSG_SELECT_SUM: {
            // Each card's value(s): sum = low 16 bits, sumHigh = the other value
            // it may count as (0 = none). must: always counted, not answerable.
            SelectSumData sd = parse_select_sum_data(buf);
            auto entry = [](const SumCardEntry& e) {
                json c = card_ref_json(e.ref);
                c["sum"] = e.sum_param & 0xffffu;
                c["sumHigh"] = e.sum_param >> 16;
                return c;
            };
            json cands = json::array(), must = json::array();
            for (const auto& e : sd.select) cands.push_back(entry(e));
            for (const auto& e : sd.must_select) must.push_back(entry(e));
            out["candidates"] = cands;
            out["must"] = must;
            out["acc"] = sd.acc;
            out["min"] = sd.min;
            out["max"] = sd.max;
            out["greater"] = sd.greater;
            break;
        }
        case MSG_SELECT_COUNTER: {
            SelectCounterData d = parse_select_counter_data(buf);
            json cands = json::array();
            for (const auto& c : d.candidates) {
                json j = card_ref_json(c.ref);
                j["counters"] = c.current_count;
                cands.push_back(j);
            }
            out["counterType"] = d.countertype;
            out["count"] = d.count;
            out["candidates"] = cands;
            break;
        }
        case MSG_SORT_CARD:
        case MSG_SORT_CHAIN: {
            // [count:u32] + count*(code:u32 ctrl:u8 loc:u32 seq:u32)
            json cands = json::array();
            uint32_t n = 0;
            if (buf.size() >= 6) std::memcpy(&n, buf.data() + 2, 4);
            for (uint32_t i = 0; i < n && 6 + (i + 1) * 13 <= buf.size(); ++i) {
                const uint8_t* e = buf.data() + 6 + i * 13;
                uint32_t code, loc, seq;
                std::memcpy(&code, e, 4);
                std::memcpy(&loc, e + 5, 4);
                std::memcpy(&seq, e + 9, 4);
                cands.push_back({{"code", code}, {"ctrl", e[4]}, {"loc", loc}, {"seq", seq}});
            }
            out["candidates"] = cands;
            break;
        }
        case MSG_ANNOUNCE_ATTRIB:
        case MSG_ANNOUNCE_RACE: {
            out["count"] = buf.size() > 2 ? buf[2] : 1;
            uint64_t available = 0;
            if (pending_msg_ == MSG_ANNOUNCE_ATTRIB && buf.size() >= 7) {
                uint32_t a; std::memcpy(&a, buf.data() + 3, 4); available = a;
            } else if (pending_msg_ == MSG_ANNOUNCE_RACE && buf.size() >= 11) {
                std::memcpy(&available, buf.data() + 3, 8);
            }
            out["available"] = std::to_string(available);
            break;
        }
        case MSG_ANNOUNCE_CARD:
        case MSG_ANNOUNCE_NUMBER: {
            json vals = json::array();
            const size_t count = buf.size() > 2 ? buf[2] : 0;
            for (size_t i = 0; i < count && 3 + (i + 1) * 8 <= buf.size(); ++i) {
                uint64_t v;
                std::memcpy(&v, buf.data() + 3 + i * 8, 8);
                vals.push_back(std::to_string(v));
            }
            out[pending_msg_ == MSG_ANNOUNCE_CARD ? "opcodes" : "options"] = vals;
            break;
        }
        default:
            break;
    }
    return out;
}

// ── The field ────────────────────────────────────────────────────────────────

// OCG_DuelQueryLocation's buffer: [total_size:u32] then, per zone slot, either
// a bare [0:u16] for an empty slot, or self-describing [size:u16][flag:u32][value]
// chunks ending with a QUERY_END chunk (the engine appends chunks beyond the
// ones asked for, so a card is read up to QUERY_END).
static json parse_zone_slots(const uint8_t* buf, uint32_t len) {
    json arr = json::array();
    const uint8_t* p = buf + 4;
    const uint8_t* end = buf + len;
    while (p + 2 <= end) {
        uint16_t size;
        std::memcpy(&size, p, 2);
        if (size == 0) {
            p += 2;
            arr.push_back(nullptr);
            continue;
        }
        uint32_t code = 0, position = 0;
        int32_t atk = 0, def = 0;
        bool has_atk = false, has_def = false;
        json overlay = json::array();
        bool first = true;
        for (;;) {
            if (!first) {
                if (p + 2 > end) break;
                std::memcpy(&size, p, 2);
            }
            first = false;
            p += 2;
            if (p + size > end) break;
            uint32_t flag = 0;
            if (size >= 4) std::memcpy(&flag, p, 4);
            if (flag == QUERY_CODE && size >= 8) std::memcpy(&code, p + 4, 4);
            else if (flag == QUERY_POSITION && size >= 8) std::memcpy(&position, p + 4, 4);
            else if (flag == QUERY_ATTACK && size >= 8) { std::memcpy(&atk, p + 4, 4); has_atk = true; }
            else if (flag == QUERY_DEFENSE && size >= 8) { std::memcpy(&def, p + 4, 4); has_def = true; }
            else if (flag == QUERY_OVERLAY_CARD && size >= 8) {
                uint32_t n = 0;
                std::memcpy(&n, p + 4, 4);
                for (uint32_t k = 0; k < n && 8u + (k + 1) * 4u <= size; ++k) {
                    uint32_t mc = 0;
                    std::memcpy(&mc, p + 8 + k * 4, 4);
                    overlay.push_back(mc);
                }
            }
            p += size;
            if (flag == QUERY_END) break;
        }
        json slot = {{"code", code}, {"position", position}};
        if (!overlay.empty()) slot["overlay"] = overlay;
        if (has_atk) slot["atk"] = atk;
        if (has_def) slot["def"] = def;
        arr.push_back(slot);
    }
    return arr;
}

static json query_zone(void* duel_ptr, uint8_t con, uint32_t loc) {
    OCG_QueryInfo info{};
    info.con = con;
    info.loc = loc;
    info.flags = QUERY_CODE | QUERY_POSITION |
                 (loc == LOCATION_MZONE ? (QUERY_OVERLAY_CARD | QUERY_ATTACK | QUERY_DEFENSE) : 0u);
    uint32_t len = 0;
    void* buf = OCG_DuelQueryLocation(static_cast<OCG_Duel>(duel_ptr), &len, &info);
    if (!buf || len < 4) return json::array();
    return parse_zone_slots(static_cast<const uint8_t*>(buf), len);
}

int Session::chain_length() const {
    if (!duel_) return 0;
    return static_cast<int>(static_cast<duel*>(duel_)->game_field->core.current_chain.size());
}

// The select list stays as the message was written from it while the engine waits.
json Session::select_chain_speeds() const {
    json speeds = json::array();
    if (!duel_) return speeds;
    for (const auto& ch : static_cast<duel*>(duel_)->game_field->core.select_chains)
        speeds.push_back(ch.triggering_effect ? ch.triggering_effect->get_speed() : 0);
    return speeds;
}

json Session::select_chain_trigger_flags() const {
    json flags = json::array();
    if (!duel_) return flags;
    for (const auto& ch : static_cast<duel*>(duel_)->game_field->core.select_chains)
        flags.push_back(ch.triggering_effect && (ch.triggering_effect->type & EFFECT_TYPE_TRIGGER_O) != 0);
    return flags;
}

bool Session::select_chain_has_optional_trigger() const {
    if (!duel_) return false;
    for (const auto& ch : static_cast<duel*>(duel_)->game_field->core.select_chains)
        if (ch.triggering_effect && (ch.triggering_effect->type & EFFECT_TYPE_TRIGGER_O)) return true;
    return false;
}

json Session::chain_links() const {
    json links = json::array();
    if (!duel_) return links;
    for (const auto& ch : static_cast<duel*>(duel_)->game_field->core.current_chain) {
        card* handler = ch.triggering_effect ? ch.triggering_effect->get_handler() : nullptr;
        links.push_back({
            {"code", handler ? handler->data.code : 0},
            {"ctrl", ch.triggering_controler},
            {"loc",  ch.triggering_location},
            {"seq",  ch.triggering_sequence},
        });
    }
    return links;
}

json Session::chain_cards_now() const {
    json chain = json::array();
    if (!duel_) return chain;
    for (const auto& ch : static_cast<duel*>(duel_)->game_field->core.current_chain) {
        card* h = ch.triggering_effect ? ch.triggering_effect->get_handler() : nullptr;
        if (!h) continue;
        chain.push_back({{"code", h->data.code}, {"ctrl", h->current.controler}, {"loc", h->current.location}, {"seq", h->current.sequence}});
    }
    return chain;
}

// GY / Extra Deck cards whose last move had REASON_DESTROY get "destroyed": true
// (the query lists a pile in the engine's own card order).
static json mark_destroyed(json zone, const card_vector& cards) {
    if (!zone.is_array() || zone.size() != cards.size()) return zone;
    for (size_t i = 0; i < cards.size(); ++i) {
        const card* c = cards[i];
        if (c && zone[i].is_object() && (c->current.reason & REASON_DESTROY)) zone[i]["destroyed"] = true;
    }
    return zone;
}

json Session::snapshot() const {
    if (!duel_) return json::array();
    auto* pduel = static_cast<duel*>(duel_);
    json players = json::array();
    for (uint8_t con = 0; con < 2; ++con) {
        const auto& pl = pduel->game_field->player[con];
        players.push_back({
            {"mzone", query_zone(duel_, con, LOCATION_MZONE)},
            {"szone", query_zone(duel_, con, LOCATION_SZONE)},
            {"hand", query_zone(duel_, con, LOCATION_HAND)},
            {"grave", mark_destroyed(query_zone(duel_, con, LOCATION_GRAVE), pl.list_grave)},
            {"banished", query_zone(duel_, con, LOCATION_REMOVED)},
            {"extra", mark_destroyed(query_zone(duel_, con, LOCATION_EXTRA), pl.list_extra)},
            {"deckCount", (int)query_zone(duel_, con, LOCATION_DECK).size()},
            {"lp", pl.lp},
        });
    }
    return players;
}

// One zone slot as `self` may see it: an opponent's face-down card has code 0 and no stats.
static json visible_card(card* c, int self) {
    if (!c) return nullptr;
    const bool hidden = c->current.controler != self && (c->current.position & POS_FACEDOWN);
    json j = {{"code", hidden ? 0u : c->data.code}, {"pos", c->current.position}};
    if (!hidden && c->current.location == LOCATION_MZONE) {
        j["atk"] = c->get_attack();
        j["def"] = c->get_defense();
        j["level"] = c->get_level();
        j["type"] = c->get_type();
    }
    return j;
}

json Session::context(int viewer) const {
    json out = {{"hint", prompt_hint_}, {"chainLength", chain_length()}};
    if (!duel_) return out;
    auto* pduel = static_cast<duel*>(duel_);
    if (effect* e = pduel->game_field->core.reason_effect) {
        card* h = e->get_handler();
        out["effect"] = {
            {"code", h ? h->data.code : 0u},
            {"ctrl", h ? h->current.controler : 0},
            {"category", e->category},
        };
    }
    out["chain"] = chain_cards_now();
    const int self = (viewer < 0 ? pending_player_ : viewer) & 1;
    json players = json::array();
    for (int pl = 0; pl < 2; ++pl) {
        const auto& p = pduel->game_field->player[pl];
        json mz = json::array(), sz = json::array(), gy = json::array();
        for (card* c : p.list_mzone) mz.push_back(visible_card(c, self));
        for (card* c : p.list_szone) sz.push_back(visible_card(c, self));
        for (card* c : p.list_grave) if (c) gy.push_back(c->data.code);
        json side = {{"lp", p.lp}, {"mzone", mz}, {"szone", sz}, {"grave", gy},
                     {"handCount", p.list_hand.size()}, {"deckCount", p.list_main.size()}};
        if (pl == self) {
            json hand = json::array();
            for (card* c : p.list_hand) if (c) hand.push_back(c->data.code);
            side["hand"] = hand;
        }
        players.push_back(side);
    }
    out["turnPlayer"] = engine_turn_player_;
    out["players"] = players;
    return out;
}

json Session::state_json() const {
    json out = {{"duel", duel_ ? "active" : "none"}};
    if (!duel_) return out;
    out["ended"] = game_over_;
    if (game_over_) {
        out["winner"] = win_player_;
        out["reason"] = win_reason_;
    }
    out["pending"] = prompt_json();
    out["responseReady"] = response_ready_;
    out["turn"] = turn_count_;
    out["turnPlayer"] = engine_turn_player_;
    out["phase"] = phase_;
    out["chain"] = chain_links();
    return out;
}

} // namespace ocgbridge
