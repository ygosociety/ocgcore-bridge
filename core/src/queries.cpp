// SPDX-License-Identifier: AGPL-3.0-or-later
// Named queries (Session::register_query) and the built-in ones:
//   card       { code }            the card's data as loaded (null when unknown)
//   fieldCard  { ctrl, loc, seq }  the card at a place now, with its current stats (null when empty)
//   chain      {}                  the chain's links, each card where it is now
#include "internal.h"

#include <stdexcept>

namespace ocgbridge {

namespace {

uint32_t arg_u32(const json& args, const char* key) {
    if (!args.is_object() || !args.contains(key) || !args[key].is_number_integer() || args[key].get<int64_t>() < 0 ||
        args[key].get<int64_t>() > 0xFFFFFFFFLL)
        throw std::invalid_argument(std::string(key) + " must be a non-negative integer");
    return static_cast<uint32_t>(args[key].get<int64_t>());
}

json card_entry_json(const CardEntry& e) {
    json setcodes = json::array();
    for (uint16_t s : e.setcodes) setcodes.push_back(s);
    return {{"code", e.code}, {"alias", e.alias}, {"type", e.type}, {"atk", e.attack}, {"def", e.defense},
            {"level", e.pack_scales ? (e.level & 0xFFFF) : e.level}, {"race", std::to_string(e.race)},
            {"attribute", e.attribute}, {"lscale", e.lscale}, {"rscale", e.rscale},
            {"linkMarker", e.link_marker}, {"setcodes", std::move(setcodes)}};
}

card* card_at(void* duel_ptr, uint8_t ctrl, uint8_t loc, uint32_t seq) {
    if (!duel_ptr || ctrl > 1) return nullptr;
    auto& pl = static_cast<duel*>(duel_ptr)->game_field->player[ctrl];
    const card_vector* list = nullptr;
    switch (loc) {
        case LOCATION_MZONE: list = &pl.list_mzone; break;
        case LOCATION_SZONE: list = &pl.list_szone; break;
        case LOCATION_GRAVE: list = &pl.list_grave; break;
        case LOCATION_HAND: list = &pl.list_hand; break;
        case LOCATION_REMOVED: list = &pl.list_remove; break;
        case LOCATION_EXTRA: list = &pl.list_extra; break;
        case LOCATION_DECK: list = &pl.list_main; break;
        default: throw std::invalid_argument("loc must be one LOCATION_* bit of a zone or pile");
    }
    return seq < list->size() ? (*list)[seq] : nullptr;
}

} // namespace

void Session::register_query(const std::string& name, QueryHandler handler) {
    if (handler) queries_[name] = std::move(handler);
    else queries_.erase(name);
}

std::vector<std::string> Session::query_names() const {
    std::vector<std::string> out;
    for (const auto& [name, h] : queries_) out.push_back(name);
    return out;
}

json Session::query(const std::string& name, const json& args) {
    auto it = queries_.find(name);
    if (it == queries_.end()) throw std::out_of_range("unknown query: " + name);
    return it->second(*this, args.is_null() ? json::object() : args);
}

void Session::register_builtin_queries() {
    register_query("card", [](Session& s, const json& args) -> json {
        const CardEntry* e = s.cards() ? s.cards()->find(arg_u32(args, "code")) : nullptr;
        return e ? card_entry_json(*e) : json();
    });
    register_query("fieldCard", [](Session& s, const json& args) -> json {
        const uint32_t ctrl = arg_u32(args, "ctrl"), loc = arg_u32(args, "loc"), seq = arg_u32(args, "seq");
        if (!s.duel_) return nullptr;
        card* c = card_at(s.duel_, static_cast<uint8_t>(ctrl), static_cast<uint8_t>(loc), seq);
        if (!c) return nullptr;
        json j = {{"code", c->data.code}, {"alias", c->data.alias}, {"ctrl", c->current.controler},
                  {"loc", c->current.location}, {"seq", c->current.sequence}, {"pos", c->current.position},
                  {"owner", c->owner}, {"type", c->get_type()}};
        if (c->current.location == LOCATION_MZONE) {
            j["atk"] = c->get_attack();
            j["def"] = c->get_defense();
            j["level"] = c->get_level();
            j["rank"] = c->get_rank();
            j["link"] = c->get_link();
            j["attribute"] = c->get_attribute();
            j["race"] = std::to_string(c->get_race());
            json overlay = json::array();
            for (const card* m : c->xyz_materials) overlay.push_back(m ? m->data.code : 0u);
            j["overlay"] = std::move(overlay);
        }
        return j;
    });
    register_query("chain", [](Session& s, const json&) -> json { return s.chain_cards_now(); });
}

} // namespace ocgbridge
