// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ocgbridge/cards.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace ocgbridge {

namespace {

void trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    size_t b = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, b - a + 1);
}

// archetype_setcode_constants.lua: `SET_ELEMENTAL_HERO = 0x3008` lines.
std::unordered_map<std::string, uint16_t> load_archetype_constants(const std::string& path) {
    std::unordered_map<std::string, uint16_t> out;
    if (path.empty()) return out;
    std::ifstream f(path);
    if (!f.is_open()) return out;
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string lhs = line.substr(0, eq);
        std::string rhs = line.substr(eq + 1);
        trim(lhs);
        trim(rhs);
        if (lhs.rfind("SET_", 0) != 0) continue;
        try {
            out[lhs.substr(4)] = static_cast<uint16_t>(std::stoul(rhs, nullptr, 16));
        } catch (...) {}
    }
    return out;
}

// The setcodes supplement (tools/gen_setcodes.py): { "setcodes": { "<code>": [setcode, ...] },
// "aliases": { "<code>": alias }, "tokens": { "<code>": [type, atk, def, level, race, attribute] } }.
struct Supplement {
    std::unordered_map<uint32_t, std::vector<uint16_t>> setcodes;
    std::unordered_map<uint32_t, uint32_t> aliases;
    std::vector<CardEntry> tokens;
};

Supplement load_supplement(const std::string& path) {
    Supplement s;
    if (path.empty()) return s;
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[ocgcore-bridge] no setcodes supplement at %s: archetypes come from names\n", path.c_str());
        return s;
    }
    try {
        json doc;
        f >> doc;
        const json table = doc.value("setcodes", json::object());
        for (const auto& [code, list] : table.items()) {
            std::vector<uint16_t> codes;
            for (const auto& v : list)
                if (v.is_number_unsigned() && v.get<uint32_t>() <= 0xFFFF && codes.size() < 4) codes.push_back(static_cast<uint16_t>(v.get<uint32_t>()));
            if (!codes.empty()) s.setcodes[static_cast<uint32_t>(std::stoul(code))] = std::move(codes);
        }
        const json token_table = doc.value("tokens", json::object());
        for (const auto& [code, v] : token_table.items()) {
            if (!v.is_array() || v.size() < 6) continue;
            CardEntry t;
            t.code      = static_cast<uint32_t>(std::stoul(code));
            t.type      = v[0].get<uint32_t>();
            t.attack    = v[1].get<int32_t>();
            t.defense   = v[2].get<int32_t>();
            t.level     = v[3].get<uint32_t>();
            t.race      = v[4].get<uint64_t>();
            t.attribute = v[5].get<uint32_t>();
            s.tokens.push_back(t);
        }
        const json aliases = doc.value("aliases", json::object());
        for (const auto& [code, alias] : aliases.items())
            if (alias.is_number_unsigned() && alias.get<uint32_t>()) s.aliases[static_cast<uint32_t>(std::stoul(code))] = alias.get<uint32_t>();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[ocgcore-bridge] setcodes supplement parse error: %s: archetypes come from names\n", ex.what());
        return Supplement{};
    }
    return s;
}

// "Elemental HERO" -> "ELEMENTAL_HERO" (archetype_setcode_constants.lua's names).
std::string normalize_archetype_name(const std::string& archetype) {
    std::string out;
    out.reserve(archetype.size());
    for (unsigned char c : archetype) {
        if (std::isalnum(c)) out += static_cast<char>(std::toupper(c));
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

constexpr uint32_t T_MONSTER    = 0x1;
constexpr uint32_t T_SPELL      = 0x2;
constexpr uint32_t T_TRAP       = 0x4;
constexpr uint32_t T_NORMAL     = 0x10;
constexpr uint32_t T_EFFECT     = 0x20;
constexpr uint32_t T_FUSION     = 0x40;
constexpr uint32_t T_RITUAL     = 0x80;
constexpr uint32_t T_SPIRIT     = 0x200;
constexpr uint32_t T_UNION      = 0x400;
constexpr uint32_t T_GEMINI     = 0x800;
constexpr uint32_t T_TUNER      = 0x1000;
constexpr uint32_t T_SYNCHRO    = 0x2000;
constexpr uint32_t T_TOKEN      = 0x4000;
constexpr uint32_t T_QUICKPLAY  = 0x10000;
constexpr uint32_t T_CONTINUOUS = 0x20000;
constexpr uint32_t T_EQUIP      = 0x40000;
constexpr uint32_t T_FIELD      = 0x80000;
constexpr uint32_t T_COUNTER    = 0x100000;
constexpr uint32_t T_FLIP       = 0x200000;
constexpr uint32_t T_TOON       = 0x400000;
constexpr uint32_t T_XYZ        = 0x800000;
constexpr uint32_t T_PENDULUM   = 0x1000000;
constexpr uint32_t T_LINK       = 0x4000000;

uint32_t parse_type(const std::string& t, const std::string& r) {
    if (t == "Spell Card") {
        uint32_t v = T_SPELL;
        if (r == "Continuous") v |= T_CONTINUOUS;
        else if (r == "Quick-Play") v |= T_QUICKPLAY;
        else if (r == "Equip")      v |= T_EQUIP;
        else if (r == "Field")      v |= T_FIELD;
        else if (r == "Ritual")     v |= T_RITUAL;
        return v;
    }
    if (t == "Trap Card") {
        uint32_t v = T_TRAP;
        if (r == "Continuous") v |= T_CONTINUOUS;
        else if (r == "Counter") v |= T_COUNTER;
        return v;
    }
    if (t == "Skill Card") return T_SPELL;

    uint32_t v = T_MONSTER;
    if (t.find("Normal")   != std::string::npos && t.find("Tuner") == std::string::npos) v |= T_NORMAL;
    if (t.find("Effect")   != std::string::npos) v |= T_EFFECT;
    if (t.find("Fusion")   != std::string::npos) v |= T_FUSION;
    if (t.find("Ritual")   != std::string::npos) v |= T_RITUAL;
    if (t.find("Spirit")   != std::string::npos) v |= T_SPIRIT;
    if (t.find("Union")    != std::string::npos) v |= T_UNION;
    if (t.find("Gemini")   != std::string::npos) v |= T_GEMINI;
    if (t.find("Tuner")    != std::string::npos) v |= T_TUNER;
    if (t.find("Synchro")  != std::string::npos) v |= T_SYNCHRO;
    if (t.find("Token")    != std::string::npos) v |= T_TOKEN;
    if (t.find("Flip")     != std::string::npos) v |= T_FLIP;
    if (t.find("Toon")     != std::string::npos) v |= T_TOON;
    if (t.find("XYZ")      != std::string::npos) v |= T_XYZ;
    if (t.find("Pendulum") != std::string::npos) v |= T_PENDULUM;
    if (t.find("Link")     != std::string::npos) v |= T_LINK;
    if ((v & (T_EFFECT|T_FUSION|T_RITUAL|T_SYNCHRO|T_XYZ|T_LINK|T_TOKEN)) == 0 &&
        t.find("Normal") == std::string::npos) v |= T_NORMAL;
    return v;
}

// The "type" string drops sub-types ("Fusion Monster" for every Fusion, "Tuner
// Monster" for effect Tuners); "typeline" (["Fiend", "Fusion", "Effect"], race
// first) lists them, so a monster that has one takes its sub-type bits from it.
uint32_t parse_typeline(const json& typeline, uint32_t type) {
    static const std::unordered_map<std::string, uint32_t> BITS = {
        {"Normal", T_NORMAL},   {"Effect", T_EFFECT},     {"Fusion", T_FUSION},
        {"Ritual", T_RITUAL},   {"Spirit", T_SPIRIT},     {"Union", T_UNION},
        {"Gemini", T_GEMINI},   {"Tuner", T_TUNER},       {"Synchro", T_SYNCHRO},
        {"Flip", T_FLIP},       {"Toon", T_TOON},         {"Xyz", T_XYZ},
        {"Pendulum", T_PENDULUM}, {"Link", T_LINK},
    };
    if (!(type & T_MONSTER) || !typeline.is_array() || typeline.empty()) return type;
    uint32_t v = T_MONSTER | (type & T_TOKEN);
    for (const auto& t : typeline) {
        if (!t.is_string()) continue;
        auto it = BITS.find(t.get<std::string>());
        if (it != BITS.end()) v |= it->second;
    }
    return v;
}

uint64_t parse_race(const std::string& r) {
    static const std::unordered_map<std::string, uint64_t> RACES = {
        {"Warrior", 0x1}, {"Spellcaster", 0x2}, {"Fairy", 0x4}, {"Fiend", 0x8},
        {"Zombie", 0x10}, {"Machine", 0x20}, {"Aqua", 0x40}, {"Pyro", 0x80},
        {"Rock", 0x100}, {"Winged Beast", 0x200}, {"Plant", 0x400}, {"Insect", 0x800},
        {"Thunder", 0x1000}, {"Dragon", 0x2000}, {"Beast", 0x4000}, {"Beast-Warrior", 0x8000},
        {"Dinosaur", 0x10000}, {"Fish", 0x20000}, {"Sea Serpent", 0x40000}, {"Reptile", 0x80000},
        {"Psychic", 0x100000}, {"Divine-Beast", 0x200000}, {"Creator God", 0x400000}, {"Wyrm", 0x800000},
        {"Cyberse", 0x1000000}, {"Illusion", 0x2000000},
    };
    auto it = RACES.find(r);
    return it == RACES.end() ? 0 : it->second;
}

uint32_t parse_attribute(const std::string& a) {
    if (a == "EARTH")  return 0x01;
    if (a == "WATER")  return 0x02;
    if (a == "FIRE")   return 0x04;
    if (a == "WIND")   return 0x08;
    if (a == "LIGHT")  return 0x10;
    if (a == "DARK")   return 0x20;
    if (a == "DIVINE") return 0x40;
    return 0;
}

uint32_t parse_link_markers(const json& markers) {
    static const std::unordered_map<std::string, uint32_t> BITS = {
        {"Bottom-Left", 0x001}, {"Bottom", 0x002}, {"Bottom-Right", 0x004},
        {"Left",        0x008}, {"Right",  0x020},
        {"Top-Left",    0x040}, {"Top",    0x080}, {"Top-Right",    0x100},
    };
    uint32_t v = 0;
    if (!markers.is_array()) return v;
    for (const auto& m : markers) {
        if (!m.is_string()) continue;
        auto it = BITS.find(m.get<std::string>());
        if (it != BITS.end()) v |= it->second;
    }
    return v;
}

} // namespace

long CardDatabase::load_ygoprodeck_json(const std::string& path, const JsonSourceOptions& opts, std::string* error) {
    std::ifstream f(path);
    if (!f.is_open()) {
        if (error) *error = "cannot open " + path;
        return -1;
    }
    json doc;
    try { f >> doc; } catch (const std::exception& ex) {
        if (error) *error = std::string("parse error: ") + ex.what();
        return -1;
    }
    auto int_field = [](const json& obj, const char* key, auto def) {
        auto it = obj.find(key);
        if (it == obj.end() || it->is_null()) return decltype(def)(def);
        return it->get<decltype(def)>();
    };

    const auto archetypes = load_archetype_constants(opts.archetype_constants);
    const Supplement sup = load_supplement(opts.setcodes_json);

    // Loaded on its own first: a code listed twice keeps its last entry, and an
    // artwork never replaces a card listed under that code.
    std::map<uint32_t, CardEntry> cards;
    std::map<uint32_t, std::vector<uint32_t>> arts;
    size_t alt_arts = 0;
    for (const auto& card : doc.value("data", json::array())) {
        try {
            uint32_t id = int_field(card, "id", 0u);
            if (!id) continue;
            CardEntry e;
            e.code  = id;
            e.alias = int_field(card, "alias", 0u);
            const uint32_t listed_alias = e.alias;
            if (!e.alias) {
                auto ra = sup.aliases.find(id);
                if (ra != sup.aliases.end() && ra->second != id) e.alias = ra->second;
            }
            e.attack      = int_field(card, "atk",        0);
            e.defense     = int_field(card, "def",        0);
            e.level       = int_field(card, "level",      0u);
            e.lscale      = int_field(card, "lscale",     0u);
            e.rscale      = int_field(card, "rscale",     0u);
            e.link_marker = int_field(card, "linkmarker", 0u);

            std::string type_str = card.value("type",      std::string{});
            std::string race_str = card.value("race",      std::string{});
            std::string attr_str = card.value("attribute", std::string{});
            if (!type_str.empty()) {
                e.type      = parse_typeline(card.value("typeline", json::array()), parse_type(type_str, race_str));
                e.attribute = parse_attribute(attr_str);
                if (e.type & T_MONSTER) e.race = parse_race(race_str);
            }

            // Link monsters: the rating is "linkval" and the arrows "linkmarkers".
            if (e.type & T_LINK) {
                if (!e.level) e.level = int_field(card, "linkval", 0u);
                if (!e.link_marker) e.link_marker = parse_link_markers(card.value("linkmarkers", json::array()));
            }

            auto real = sup.setcodes.find(id);
            if (real == sup.setcodes.end() && e.alias) real = sup.setcodes.find(e.alias);
            if (real != sup.setcodes.end()) {
                e.setcodes = real->second;
            } else {
                std::string archetype_str = card.value("archetype", std::string{});
                if (!archetype_str.empty()) {
                    auto it = archetypes.find(normalize_archetype_name(archetype_str));
                    if (it != archetypes.end()) e.setcodes = {it->second};
                }
            }

            cards[id] = e;

            // Alternate artworks (card_images ids): the same card under another
            // code, aliased to it, as EDOPro's database has them.
            for (const auto& img : card.value("card_images", json::array())) {
                uint32_t art = int_field(img, "id", 0u);
                if (!art || art == id) continue;
                arts[id].push_back(art);
                if (cards.count(art)) continue;
                CardEntry alt = e;
                alt.code = art;
                // An artwork is this card, never what the card is "treated as".
                alt.alias = listed_alias ? listed_alias : id;
                cards[art] = alt;
                alt_arts++;
            }
        } catch (...) {}
    }
    size_t tokens = 0;
    for (const auto& t : sup.tokens) {
        if (cards.count(t.code)) continue;
        CardEntry e = t;
        auto real = sup.setcodes.find(t.code);
        if (real != sup.setcodes.end()) e.setcodes = real->second;
        cards[t.code] = e;
        tokens++;
    }
    long added = 0;
    for (auto& [code, e] : cards)
        if (cards_.emplace(code, std::move(e)).second) added++;
    for (auto& [code, list] : arts) {
        auto& dst = art_codes_[code];
        dst.insert(dst.end(), list.begin(), list.end());
    }
    std::fprintf(stderr, "[ocgcore-bridge] %s: %ld cards (%zu alternate artworks, %zu tokens from the supplement)\n",
                 path.c_str(), added, alt_arts, tokens);
    return added;
}

const CardEntry* CardDatabase::find(uint32_t code) const {
    auto it = cards_.find(code);
    return it == cards_.end() ? nullptr : &it->second;
}

const std::vector<uint32_t>* CardDatabase::art_codes(uint32_t code) const {
    auto it = art_codes_.find(code);
    return it == art_codes_.end() ? nullptr : &it->second;
}

} // namespace ocgbridge
