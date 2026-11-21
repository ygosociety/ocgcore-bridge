// SPDX-License-Identifier: AGPL-3.0-or-later
// Card data for the engine's card reader.
//
// Two sources:
//  - a YGOPRODeck-style JSON card list ({ "data": [ { id, type, race, ... } ] }),
//    with an optional setcodes supplement (data/setcodes.json, generated from
//    BabelCDB by tools/gen_setcodes.py) and the archetype constants of the
//    card scripts as a fallback for archetype membership;
//  - BabelCDB / EDOPro .cdb databases (SQLite), when built with SQLite.
// Sources add up: a code loaded twice keeps the first entry.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ocgbridge {

struct CardEntry {
    uint32_t code{0};
    uint32_t type{0};
    int32_t  attack{0};
    int32_t  defense{0};
    // As handed to the engine's card reader. The JSON source packs the scales
    // the way it always has: (lscale << 24) | (rscale << 16) | level.
    uint32_t level{0};
    uint64_t race{0};
    uint32_t attribute{0};
    uint32_t lscale{0};
    uint32_t rscale{0};
    uint32_t link_marker{0};
    uint32_t alias{0};
    // Archetype setcodes (Card:IsSetCard), at most four.
    std::vector<uint16_t> setcodes;
    // The JSON source packs the scales into `level` (see above); a .cdb entry
    // passes `level` as is.
    bool pack_scales{true};
};

struct JsonSourceOptions {
    // The setcodes supplement ({ "setcodes", "aliases", "tokens" }); empty = none.
    std::string setcodes_json;
    // archetype_setcode_constants.lua of the card scripts: an archetype NAME
    // ("Elemental HERO") becomes SET_ELEMENTAL_HERO's value when the supplement
    // has no setcodes for the card; empty = none.
    std::string archetype_constants;
};

class CardDatabase {
public:
    // Returns the number of cards the source added; -1 (with `error`) when it
    // could not be read.
    long load_ygoprodeck_json(const std::string& path, const JsonSourceOptions& opts, std::string* error = nullptr);
    long load_cdb(const std::string& path, std::string* error = nullptr);
    static bool cdb_supported();

    const CardEntry* find(uint32_t code) const;
    const std::map<uint32_t, CardEntry>& all() const { return cards_; }
    // A card's other artwork codes (JSON source: its card_images ids).
    const std::vector<uint32_t>* art_codes(uint32_t code) const;
    size_t size() const { return cards_.size(); }
    void add(const CardEntry& e) { cards_.emplace(e.code, e); }
    void clear() { cards_.clear(); art_codes_.clear(); }

private:
    std::map<uint32_t, CardEntry> cards_;
    std::map<uint32_t, std::vector<uint32_t>> art_codes_;
};

} // namespace ocgbridge
