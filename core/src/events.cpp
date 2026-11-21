// SPDX-License-Identifier: AGPL-3.0-or-later
// Engine message names and decoding (events.h). Layouts are ygopro-core's
// (field.cpp, operations.cpp, processor.cpp, libduel.cpp): a place is the
// engine's loc_info, [ctrl:u8][loc:u8][seq:u32][pos:u32].
#include "internal.h"

#include <cstring>

namespace ocgbridge {

namespace {

struct NameEntry { uint8_t type; const char* name; };

const NameEntry NAMES[] = {
    {MSG_RETRY, "retry"},
    {MSG_HINT, "hint"},
    {MSG_WAITING, "waiting"},
    {MSG_START, "start"},
    {MSG_WIN, "win"},
    {MSG_UPDATE_DATA, "updateData"},
    {MSG_UPDATE_CARD, "updateCard"},
    {MSG_REQUEST_DECK, "requestDeck"},
    {MSG_SELECT_BATTLECMD, "selectBattlecmd"},
    {MSG_SELECT_IDLECMD, "selectIdlecmd"},
    {MSG_SELECT_EFFECTYN, "selectEffectyn"},
    {MSG_SELECT_YESNO, "selectYesno"},
    {MSG_SELECT_OPTION, "selectOption"},
    {MSG_SELECT_CARD, "selectCard"},
    {MSG_SELECT_CHAIN, "selectChain"},
    {MSG_SELECT_PLACE, "selectPlace"},
    {MSG_SELECT_POSITION, "selectPosition"},
    {MSG_SELECT_TRIBUTE, "selectTribute"},
    {MSG_SORT_CHAIN, "sortChain"},
    {MSG_SELECT_COUNTER, "selectCounter"},
    {MSG_SELECT_SUM, "selectSum"},
    {MSG_SELECT_DISFIELD, "selectDisfield"},
    {MSG_SORT_CARD, "sortCard"},
    {MSG_SELECT_UNSELECT_CARD, "selectUnselectCard"},
    {MSG_CONFIRM_DECKTOP, "confirmDecktop"},
    {MSG_CONFIRM_CARDS, "confirmCards"},
    {MSG_SHUFFLE_DECK, "shuffleDeck"},
    {MSG_SHUFFLE_HAND, "shuffleHand"},
    {MSG_REFRESH_DECK, "refreshDeck"},
    {MSG_SWAP_GRAVE_DECK, "swapGraveDeck"},
    {MSG_SHUFFLE_SET_CARD, "shuffleSetCard"},
    {MSG_REVERSE_DECK, "reverseDeck"},
    {MSG_DECK_TOP, "deckTop"},
    {MSG_SHUFFLE_EXTRA, "shuffleExtra"},
    {MSG_NEW_TURN, "newTurn"},
    {MSG_NEW_PHASE, "newPhase"},
    {MSG_CONFIRM_EXTRATOP, "confirmExtratop"},
    {MSG_MOVE, "move"},
    {MSG_POS_CHANGE, "posChange"},
    {MSG_SET, "set"},
    {MSG_SWAP, "swap"},
    {MSG_FIELD_DISABLED, "fieldDisabled"},
    {MSG_SUMMONING, "summoning"},
    {MSG_SUMMONED, "summoned"},
    {MSG_SPSUMMONING, "spsummoning"},
    {MSG_SPSUMMONED, "spsummoned"},
    {MSG_FLIPSUMMONING, "flipsummoning"},
    {MSG_FLIPSUMMONED, "flipsummoned"},
    {MSG_CHAINING, "chaining"},
    {MSG_CHAINED, "chained"},
    {MSG_CHAIN_SOLVING, "chainSolving"},
    {MSG_CHAIN_SOLVED, "chainSolved"},
    {MSG_CHAIN_END, "chainEnd"},
    {MSG_CHAIN_NEGATED, "chainNegated"},
    {MSG_CHAIN_DISABLED, "chainDisabled"},
    {MSG_CARD_SELECTED, "cardSelected"},
    {MSG_RANDOM_SELECTED, "randomSelected"},
    {MSG_BECOME_TARGET, "becomeTarget"},
    {MSG_DRAW, "draw"},
    {MSG_DAMAGE, "damage"},
    {MSG_RECOVER, "recover"},
    {MSG_EQUIP, "equip"},
    {MSG_LPUPDATE, "lpupdate"},
    {MSG_UNEQUIP, "unequip"},
    {MSG_CARD_TARGET, "cardTarget"},
    {MSG_CANCEL_TARGET, "cancelTarget"},
    {MSG_PAY_LPCOST, "payLpcost"},
    {MSG_ADD_COUNTER, "addCounter"},
    {MSG_REMOVE_COUNTER, "removeCounter"},
    {MSG_ATTACK, "attack"},
    {MSG_BATTLE, "battle"},
    {MSG_ATTACK_DISABLED, "attackDisabled"},
    {MSG_DAMAGE_STEP_START, "damageStepStart"},
    {MSG_DAMAGE_STEP_END, "damageStepEnd"},
    {MSG_MISSED_EFFECT, "missedEffect"},
    {MSG_BE_CHAIN_TARGET, "beChainTarget"},
    {MSG_CREATE_RELATION, "createRelation"},
    {MSG_RELEASE_RELATION, "releaseRelation"},
    {MSG_TOSS_COIN, "tossCoin"},
    {MSG_TOSS_DICE, "tossDice"},
    {MSG_ROCK_PAPER_SCISSORS, "rockPaperScissors"},
    {MSG_HAND_RES, "handRes"},
    {MSG_ANNOUNCE_RACE, "announceRace"},
    {MSG_ANNOUNCE_ATTRIB, "announceAttrib"},
    {MSG_ANNOUNCE_CARD, "announceCard"},
    {MSG_ANNOUNCE_NUMBER, "announceNumber"},
    {MSG_CARD_HINT, "cardHint"},
    {MSG_TAG_SWAP, "tagSwap"},
    {MSG_RELOAD_FIELD, "reloadField"},
    {MSG_AI_NAME, "aiName"},
    {MSG_SHOW_HINT, "showHint"},
    {MSG_PLAYER_HINT, "playerHint"},
    {MSG_MATCH_KILL, "matchKill"},
    {MSG_CUSTOM_MSG, "customMsg"},
    {MSG_REMOVE_CARDS, "removeCards"},
};

// A bounds-checked little-endian reader; reads past the end give 0 and mark
// the reader bad.
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok{true};
    template <typename T> T get() {
        T v{};
        if (p + sizeof(T) > end) { ok = false; p = end; return v; }
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    nlohmann::json place() {
        const uint8_t ctrl = get<uint8_t>();
        const uint8_t loc = get<uint8_t>();
        const uint32_t seq = get<uint32_t>();
        const uint32_t pos = get<uint32_t>();
        return {{"ctrl", ctrl}, {"loc", loc}, {"seq", seq}, {"pos", pos}};
    }
};

} // namespace

const char* message_name(uint8_t type) {
    for (const auto& e : NAMES)
        if (e.type == type) return e.name;
    return "unknown";
}

uint8_t message_of_name(const std::string& name) {
    for (const auto& e : NAMES)
        if (name == e.name) return e.type;
    return 0;
}

nlohmann::json decode_message(const uint8_t* data, size_t size) {
    using nlohmann::json;
    if (!data || size == 0) return json();
    const uint8_t t = data[0];
    json out = {{"msg", t}, {"name", message_name(t)}};
    Reader r{data + 1, data + size};
    json f = json::object();
    switch (t) {
        case MSG_NEW_TURN: f["player"] = r.get<uint8_t>(); break;
        case MSG_NEW_PHASE: f["phase"] = r.get<uint16_t>(); break;
        case MSG_WIN: f["player"] = r.get<uint8_t>(); f["reason"] = r.get<uint8_t>(); break;
        case MSG_HINT:
            f["hintType"] = r.get<uint8_t>();
            f["player"] = r.get<uint8_t>();
            f["data"] = std::to_string(r.get<uint64_t>());
            break;
        case MSG_DRAW: {
            f["player"] = r.get<uint8_t>();
            const uint32_t n = r.get<uint32_t>();
            json cards = json::array();
            for (uint32_t i = 0; i < n && r.ok; ++i) {
                const uint32_t code = r.get<uint32_t>();
                const uint32_t pos = r.get<uint32_t>();
                cards.push_back({{"code", code}, {"pos", pos}});
            }
            f["cards"] = std::move(cards);
            break;
        }
        case MSG_DAMAGE:
        case MSG_RECOVER:
        case MSG_PAY_LPCOST:
            f["player"] = r.get<uint8_t>();
            f["amount"] = r.get<uint32_t>();
            break;
        case MSG_LPUPDATE:
            f["player"] = r.get<uint8_t>();
            f["lp"] = r.get<uint32_t>();
            break;
        case MSG_MOVE:
            f["code"] = r.get<uint32_t>();
            f["from"] = r.place();
            f["to"] = r.place();
            f["reason"] = r.get<uint32_t>();
            break;
        case MSG_SUMMONING:
        case MSG_SPSUMMONING:
        case MSG_FLIPSUMMONING:
        case MSG_SET:
            f["code"] = r.get<uint32_t>();
            f["card"] = r.place();
            break;
        case MSG_POS_CHANGE:
            f["code"] = r.get<uint32_t>();
            f["ctrl"] = r.get<uint8_t>();
            f["loc"] = r.get<uint8_t>();
            f["seq"] = r.get<uint8_t>();
            f["from"] = r.get<uint8_t>();
            f["to"] = r.get<uint8_t>();
            break;
        case MSG_CHAINING: {
            f["code"] = r.get<uint32_t>();
            f["card"] = r.place();
            const uint8_t ctrl = r.get<uint8_t>();
            const uint8_t loc = r.get<uint8_t>();
            const uint32_t seq = r.get<uint32_t>();
            f["trigger"] = {{"ctrl", ctrl}, {"loc", loc}, {"seq", seq}};
            f["desc"] = std::to_string(r.get<uint64_t>());
            f["link"] = r.get<uint32_t>();
            break;
        }
        case MSG_CHAINED:
        case MSG_CHAIN_SOLVING:
        case MSG_CHAIN_SOLVED:
        case MSG_CHAIN_NEGATED:
        case MSG_CHAIN_DISABLED:
            f["link"] = r.get<uint8_t>();
            break;
        case MSG_ATTACK: {
            f["attacker"] = r.place();
            json target = r.place();
            f["target"] = target["loc"] == 0 ? json() : target; // a direct attack
            break;
        }
        case MSG_SHUFFLE_DECK:
        case MSG_SHUFFLE_EXTRA:
        case MSG_SHUFFLE_HAND:
            f["player"] = r.get<uint8_t>();
            break;
        case MSG_TOSS_COIN:
        case MSG_TOSS_DICE: {
            f["player"] = r.get<uint8_t>();
            const uint8_t n = r.get<uint8_t>();
            json results = json::array();
            for (uint8_t i = 0; i < n && r.ok; ++i) results.push_back(r.get<uint8_t>());
            f["results"] = std::move(results);
            break;
        }
        case MSG_CONFIRM_CARDS: {
            f["player"] = r.get<uint8_t>();
            const uint32_t n = r.get<uint32_t>();
            json cards = json::array();
            for (uint32_t i = 0; i < n && r.ok; ++i) {
                const uint32_t code = r.get<uint32_t>();
                const uint8_t ctrl = r.get<uint8_t>();
                const uint8_t loc = r.get<uint8_t>();
                const uint32_t seq = r.get<uint32_t>();
                cards.push_back({{"code", code}, {"ctrl", ctrl}, {"loc", loc}, {"seq", seq}});
            }
            f["cards"] = std::move(cards);
            break;
        }
        case MSG_BECOME_TARGET: {
            const uint32_t n = r.get<uint32_t>();
            json cards = json::array();
            for (uint32_t i = 0; i < n && r.ok; ++i) cards.push_back(r.place());
            f["cards"] = std::move(cards);
            break;
        }
        default:
            return out;
    }
    if (!r.ok) return out; // shorter than its layout: the name only
    out.update(f);
    return out;
}

nlohmann::json EngineMessage::decoded() const { return decode_message(data, size); }

} // namespace ocgbridge
