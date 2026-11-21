// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ocgbridge/messages.h"

#include <algorithm>
#include <cstring>

#include "ocgapi_constants.h"

namespace ocgbridge {

std::vector<CardRef> parse_card_refs(const uint8_t* p, const uint8_t* end, int count, int entry_bytes) {
    std::vector<CardRef> result;
    if (count > 0) result.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count && p + entry_bytes <= end; i++, p += entry_bytes) {
        CardRef r;
        std::memcpy(&r.code, p, 4);
        r.ctrl = p[4]; r.loc = p[5]; r.seq = p[6]; r.pos = p[7];
        result.push_back(r);
    }
    return result;
}

// The u64 effect description at `offset` in each `entry_bytes` entry from `p` (activation lists).
static void read_descs(std::vector<CardRef>& refs, const uint8_t* p, const uint8_t* end, int entry_bytes, int offset) {
    for (auto& r : refs) {
        if (p + entry_bytes > end) break;
        std::memcpy(&r.desc, p + offset, 8);
        p += entry_bytes;
    }
}

IdleData parse_idle_data(const std::vector<uint8_t>& buf) {
    IdleData d;
    if (buf.size() < 2) return d;
    const uint8_t* p   = buf.data() + 2;
    const uint8_t* end = buf.data() + buf.size();
    auto read_u32 = [&]() -> int {
        if (p + 4 > end) return 0;
        uint32_t v; std::memcpy(&v, p, 4); p += 4; return static_cast<int>(v);
    };
    auto read_u8 = [&]() -> int { return (p < end) ? (int)(*p++) : 0; };

    int n;
    n = read_u32(); d.summonable  = parse_card_refs(p, end, n, 10); p = std::min(p + n * 10, end);
    n = read_u32(); d.sp_summon   = parse_card_refs(p, end, n, 10); p = std::min(p + n * 10, end);
    n = read_u32(); d.reposition  = parse_card_refs(p, end, n, 7);  p = std::min(p + n * 7,  end);
    n = read_u32(); d.mset        = parse_card_refs(p, end, n, 10); p = std::min(p + n * 10, end);
    n = read_u32(); d.sset        = parse_card_refs(p, end, n, 10); p = std::min(p + n * 10, end);
    n = read_u32(); d.activatable = parse_card_refs(p, end, n, 19); read_descs(d.activatable, p, end, 19, 10); p = std::min(p + n * 19, end);
    d.to_bp       = read_u8() != 0;
    d.to_ep       = read_u8() != 0;
    d.can_shuffle = read_u8() != 0;
    return d;
}

BattleData parse_battle_data(const std::vector<uint8_t>& buf) {
    BattleData d;
    if (buf.size() < 2) return d;
    const uint8_t* p   = buf.data() + 2;
    const uint8_t* end = buf.data() + buf.size();
    auto read_u32 = [&]() -> int {
        if (p + 4 > end) return 0;
        uint32_t v; std::memcpy(&v, p, 4); p += 4; return static_cast<int>(v);
    };
    auto read_u8 = [&]() -> int { return (p < end) ? (int)(*p++) : 0; };

    int n;
    n = read_u32(); d.activatable = parse_card_refs(p, end, n, 19); read_descs(d.activatable, p, end, 19, 10); p = std::min(p + n * 19, end);
    n = read_u32(); d.attackable  = parse_card_refs(p, end, n, 8);  p = std::min(p + n * 8,  end);
    d.to_m2 = read_u8() != 0;
    d.to_ep = read_u8() != 0;
    return d;
}

SelectCardData parse_select_card_data(const std::vector<uint8_t>& buf) {
    SelectCardData d;
    auto read_u32_at = [&](size_t off) -> uint32_t {
        uint32_t v = 0;
        if (buf.size() >= off + 4) std::memcpy(&v, buf.data() + off, 4);
        return v;
    };
    d.min = (buf.size() >= 7)  ? read_u32_at(3) : 1;
    d.max = (buf.size() >= 11) ? read_u32_at(7) : d.min;
    uint32_t count = (buf.size() >= 15) ? read_u32_at(11) : 0;
    if (count > 0) {
        const uint8_t* entries = buf.data() + 15;
        const uint8_t* end     = buf.data() + buf.size();
        d.candidates = parse_card_refs(entries, end, (int)count, 14);
    }
    return d;
}

static std::vector<SumCardEntry> parse_sum_entries(const uint8_t*& p, const uint8_t* end, uint32_t count) {
    std::vector<SumCardEntry> result;
    for (uint32_t i = 0; i < count && p + 18 <= end; i++, p += 18) {
        SumCardEntry e;
        std::memcpy(&e.ref.code, p, 4);
        e.ref.ctrl = p[4]; e.ref.loc = p[5]; e.ref.seq = p[6]; e.ref.pos = p[7];
        std::memcpy(&e.sum_param, p + 14, 4);
        result.push_back(e);
    }
    return result;
}

SelectSumData parse_select_sum_data(const std::vector<uint8_t>& buf) {
    SelectSumData d;
    auto read_u32_at = [&](size_t off) -> uint32_t {
        uint32_t v = 0;
        if (buf.size() >= off + 4) std::memcpy(&v, buf.data() + off, 4);
        return v;
    };
    if (buf.size() < 3) return d;
    // The flag byte is 0 when the effect passed a real max, 1 otherwise; the
    // numeric max is clamped up to at least `min` on the wire even without
    // one, so the flag (not "max == 0") tells the engine's two modes apart.
    bool has_max = buf[2] == 0;
    d.acc = read_u32_at(3);
    d.min = read_u32_at(7);
    d.max = has_max ? read_u32_at(11) : 0;
    d.greater = !has_max;

    uint32_t mustCount = (buf.size() >= 19) ? read_u32_at(15) : 0;
    const uint8_t* p   = buf.data() + 19;
    const uint8_t* end = buf.data() + buf.size();
    d.must_select = parse_sum_entries(p, end, mustCount);

    if (p + 4 <= end) {
        uint32_t selectCount = 0;
        std::memcpy(&selectCount, p, 4);
        p += 4;
        d.select = parse_sum_entries(p, end, selectCount);
    }
    return d;
}

namespace {

struct SumItem {
    uint32_t lo;
    uint32_t hi; // 0 if the card has no second value
    int select_index; // -1 for a mandatory (must_select) item
};

// Depth-first search for an exact-sum combination (select_sum_check1): every
// item contributes its low or (if present) high value, mandatory items are
// always included, and the number of optional items lands in [min_count,
// max_count] (max_count < 0: unbounded). `budget` bounds the node visits.
bool search_select_sum(const std::vector<SumItem>& items, size_t idx, int32_t remaining,
                       int included, int min_count, int max_count,
                       std::vector<int>& chosen, long& budget) {
    if (--budget < 0) return false;
    if (idx == items.size()) {
        return remaining == 0 && included >= min_count &&
               (max_count < 0 || included <= max_count);
    }
    const SumItem& it = items[idx];
    bool optional = it.select_index >= 0;

    // Skipping (optional items only) first, so a solution with fewer cards wins.
    if (optional && search_select_sum(items, idx + 1, remaining, included, min_count, max_count, chosen, budget))
        return true;

    for (int opt = 0; opt < 2; ++opt) {
        if (opt == 1 && (it.hi == 0 || it.hi == it.lo)) break;
        uint32_t value = (opt == 0) ? it.lo : it.hi;
        if (value > (uint32_t)remaining) continue;
        int new_included = included + (optional ? 1 : 0);
        if (max_count >= 0 && new_included > max_count) continue;
        if (optional) chosen.push_back(it.select_index);
        if (search_select_sum(items, idx + 1, remaining - (int32_t)value, new_included, min_count, max_count, chosen, budget))
            return true;
        if (optional) chosen.pop_back();
    }
    return false;
}

} // namespace

std::pair<std::vector<uint8_t>, bool> solve_select_sum(const SelectSumData& data) {
    std::vector<SumItem> items;
    items.reserve(data.must_select.size() + data.select.size());
    for (const auto& e : data.must_select)
        items.push_back({ e.sum_param & 0xffffu, e.sum_param >> 16, -1 });
    for (size_t i = 0; i < data.select.size(); ++i)
        items.push_back({ data.select[i].sum_param & 0xffffu, data.select[i].sum_param >> 16, (int)i });

    int min_count = (int)data.min;
    int max_count = data.max > 0 ? (int)data.max : -1;

    std::vector<int> chosen;
    long budget = 200000;
    if (!search_select_sum(items, 0, (int32_t)data.acc, 0, min_count, max_count, chosen, budget))
        return { {}, false };

    std::vector<uint8_t> idxs;
    idxs.reserve(chosen.size());
    for (int i : chosen) idxs.push_back(static_cast<uint8_t>(i));
    return { idxs, true };
}

bool select_sum_accepts(const SelectSumData& data, const std::vector<uint8_t>& idxs) {
    std::vector<uint32_t> params;
    std::vector<bool> seen(data.select.size(), false);
    for (uint8_t i : idxs) {
        if (i >= data.select.size() || seen[i]) return false;
        seen[i] = true;
    }
    for (const auto& e : data.must_select) params.push_back(e.sum_param);
    for (uint8_t i : idxs) params.push_back(data.select[i].sum_param);
    const int32_t acc = (int32_t)data.acc;
    if (!data.greater) {
        if (idxs.size() < data.min || idxs.size() > data.max) return false;
        std::vector<SumItem> items;
        for (uint32_t p : params) items.push_back({ p & 0xffffu, p >> 16, -1 });
        std::vector<int> chosen;
        long budget = 200000;
        return search_select_sum(items, 0, acc, 0, 0, -1, chosen, budget);
    }
    int32_t sum = 0, mx = 0, mn = 0x7fffffff;
    for (uint32_t op : params) {
        int32_t o1 = op & 0xffff, o2 = op >> 16;
        int32_t ms = (o2 && o2 < o1) ? o2 : o1;
        sum += ms;
        mx += (o2 > o1) ? o2 : o1;
        if (ms < mn) mn = ms;
    }
    return !(mx < acc || sum - mn >= acc);
}

std::pair<std::vector<uint8_t>, bool> solve_select_sum_checked(const SelectSumData& data) {
    auto exact = solve_select_sum(data);
    if (exact.second || !data.greater) return exact;
    // Fewest cards first, each size in list order (k-subsets, lexicographic).
    const size_t n = std::min<size_t>(data.select.size(), 255);
    long budget = 200000;
    for (size_t k = 0; k <= n; ++k) {
        std::vector<uint8_t> idx(k);
        for (size_t i = 0; i < k; ++i) idx[i] = static_cast<uint8_t>(i);
        while (true) {
            if (--budget < 0) return { {}, false };
            if (select_sum_accepts(data, idx)) return { idx, true };
            size_t i = k;
            while (i > 0 && idx[i - 1] == n - k + i - 1) --i;
            if (i == 0) break;
            ++idx[i - 1];
            for (size_t j = i; j < k; ++j) idx[j] = static_cast<uint8_t>(idx[j - 1] + 1);
        }
    }
    return { {}, false };
}

SelectCounterData parse_select_counter_data(const std::vector<uint8_t>& buf) {
    SelectCounterData d;
    if (buf.size() < 10) return d;
    std::memcpy(&d.countertype, buf.data() + 2, 2);
    std::memcpy(&d.count, buf.data() + 4, 2);
    uint32_t size = 0;
    std::memcpy(&size, buf.data() + 6, 4);

    const uint8_t* p   = buf.data() + 10;
    const uint8_t* end = buf.data() + buf.size();
    for (uint32_t i = 0; i < size && p + 9 <= end; i++, p += 9) {
        CounterCandidate c;
        std::memcpy(&c.ref.code, p, 4);
        c.ref.ctrl = p[4]; c.ref.loc = p[5]; c.ref.seq = p[6];
        std::memcpy(&c.current_count, p + 7, 2);
        d.candidates.push_back(c);
    }
    return d;
}

std::pair<std::vector<int16_t>, bool> solve_select_counter(const SelectCounterData& data) {
    std::vector<int16_t> resp(data.candidates.size(), 0);
    int32_t remaining = data.count;
    for (size_t i = 0; i < data.candidates.size() && remaining > 0; ++i) {
        int32_t take = std::min<int32_t>(remaining, data.candidates[i].current_count);
        resp[i] = static_cast<int16_t>(take);
        remaining -= take;
    }
    if (remaining > 0) return { {}, false };
    return { resp, true };
}

int find_by_code(const std::vector<CardRef>& list, uint32_t code) {
    for (int i = 0; i < (int)list.size(); i++)
        if (list[i].code == code) return i;
    return -1;
}

int find_by_loc_seq(const std::vector<CardRef>& list, uint32_t loc, uint32_t seq) {
    for (int i = 0; i < (int)list.size(); i++)
        if (list[i].loc == static_cast<uint8_t>(loc) && list[i].seq == static_cast<uint8_t>(seq)) return i;
    return -1;
}

int find_by_ref(const std::vector<CardRef>& list, uint32_t code, int loc, int seq, int ctrl) {
    for (int i = 0; i < (int)list.size(); i++) {
        const auto& r = list[i];
        if (r.code == code && r.loc == loc && r.seq == seq && (ctrl < 0 || r.ctrl == ctrl)) return i;
    }
    return -1;
}

ChainData parse_chain_data(const std::vector<uint8_t>& buf) {
    ChainData d;
    if (buf.size() > 2) d.spe_count = buf[2];
    if (buf.size() > 3) d.forced = buf[3] != 0;
    if (buf.size() >= 8) std::memcpy(&d.hint_timing, buf.data() + 4, 4);
    if (buf.size() >= 12) std::memcpy(&d.hint_timing_opp, buf.data() + 8, 4);
    if (buf.size() < 16) return d;
    uint32_t count = 0;
    std::memcpy(&count, buf.data() + 12, 4);
    const uint8_t* p   = buf.data() + 16;
    const uint8_t* end = buf.data() + buf.size();
    for (uint32_t i = 0; i < count && p + 23 <= end; i++, p += 23) {
        CardRef r;
        std::memcpy(&r.code, p, 4);
        r.ctrl = p[4]; r.loc = p[5]; r.seq = p[6]; r.pos = p[10];
        std::memcpy(&r.desc, p + 14, 8);
        d.candidates.push_back(r);
    }
    return d;
}

bool is_prompt_message(uint8_t t) {
    return t == MSG_SELECT_BATTLECMD || t == MSG_SELECT_IDLECMD   ||
           t == MSG_SELECT_EFFECTYN  || t == MSG_SELECT_YESNO     ||
           t == MSG_SELECT_OPTION    || t == MSG_SELECT_CARD      ||
           t == MSG_SELECT_CHAIN     || t == MSG_SELECT_PLACE     ||
           t == MSG_SELECT_POSITION  || t == MSG_SELECT_TRIBUTE   ||
           t == MSG_SELECT_SUM       || t == MSG_SELECT_DISFIELD  ||
           t == MSG_SELECT_UNSELECT_CARD ||
           t == MSG_SORT_CHAIN       || t == MSG_SELECT_COUNTER   ||
           t == MSG_SORT_CARD        || t == MSG_ROCK_PAPER_SCISSORS ||
           t == MSG_ANNOUNCE_RACE    || t == MSG_ANNOUNCE_ATTRIB  ||
           t == MSG_ANNOUNCE_CARD    || t == MSG_ANNOUNCE_NUMBER;
}

namespace {
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok{true};
    bool need(size_t n) { if (p + n > end) { ok = false; return false; } return true; }
    uint8_t u8() { if (!need(1)) return 0; return *p++; }
    uint32_t u32() { uint32_t v = 0; if (!need(4)) return 0; std::memcpy(&v, p, 4); p += 4; return v; }
    uint64_t u64() { uint64_t v = 0; if (!need(8)) return 0; std::memcpy(&v, p, 8); p += 8; return v; }
    CardRef code_loc_info() {
        CardRef r;
        r.code = u32();
        r.ctrl = u8(); r.loc = u8();
        r.seq = static_cast<uint8_t>(u32());
        r.pos = static_cast<uint8_t>(u32());
        return r;
    }
    void loc_info(CardRef& r) {
        r.ctrl = u8(); r.loc = u8();
        r.seq = static_cast<uint8_t>(u32());
        r.pos = static_cast<uint8_t>(u32());
    }
};
} // namespace

PromptData parse_prompt(const std::vector<uint8_t>& buf) {
    PromptData d;
    if (buf.size() < 2) return d;
    d.msg = buf[0];
    d.player = buf[1];
    Reader r{ buf.data() + 2, buf.data() + buf.size() };
    switch (d.msg) {
        case MSG_SELECT_CARD: {
            d.cancelable = r.u8() != 0;
            d.min = r.u32(); d.max = r.u32();
            uint32_t n = r.u32();
            for (uint32_t i = 0; i < n && r.ok; i++) { CardRef c = r.code_loc_info(); if (r.ok) d.cards.push_back(c); }
            break;
        }
        case MSG_SELECT_TRIBUTE: {
            d.cancelable = r.u8() != 0;
            d.min = r.u32(); d.max = r.u32();
            uint32_t n = r.u32();
            for (uint32_t i = 0; i < n && r.ok; i++) {
                CardRef c;
                c.code = r.u32(); c.ctrl = r.u8(); c.loc = r.u8();
                c.seq = static_cast<uint8_t>(r.u32());
                uint8_t rel = r.u8();
                if (!r.ok) break;
                d.cards.push_back(c);
                d.release.push_back(rel);
            }
            break;
        }
        case MSG_SELECT_UNSELECT_CARD: {
            d.finishable = r.u8() != 0;
            d.cancelable = r.u8() != 0;
            d.min = r.u32(); d.max = r.u32();
            uint32_t n = r.u32();
            for (uint32_t i = 0; i < n && r.ok; i++) { CardRef c = r.code_loc_info(); if (r.ok) d.cards.push_back(c); }
            d.select_count = static_cast<uint32_t>(d.cards.size());
            uint32_t n2 = r.u32();
            for (uint32_t i = 0; i < n2 && r.ok; i++) { CardRef c = r.code_loc_info(); if (r.ok) d.cards.push_back(c); }
            break;
        }
        case MSG_SELECT_OPTION: {
            uint8_t n = r.u8();
            for (uint8_t i = 0; i < n && r.ok; i++) { uint64_t v = r.u64(); if (r.ok) d.options.push_back(v); }
            break;
        }
        case MSG_SELECT_YESNO:
            d.description = r.u64();
            break;
        case MSG_SELECT_EFFECTYN:
            d.code = r.u32();
            d.card.code = d.code;
            r.loc_info(d.card);
            d.description = r.u64();
            break;
        case MSG_SELECT_POSITION:
            d.code = r.u32();
            d.positions = r.u8();
            break;
        case MSG_SELECT_PLACE:
        case MSG_SELECT_DISFIELD:
            d.count = r.u8();
            d.flag = r.u32();
            break;
        default:
            break;
    }
    return d;
}

uint64_t lowest_n_bits(uint64_t available, int count) {
    uint64_t result = 0;
    for (int i = 0; i < count && available != 0; ++i) {
        uint64_t bit = available & (~available + 1);
        result |= bit;
        available &= ~bit;
    }
    return result;
}

std::vector<uint8_t> place_response(uint8_t playerid, int count, uint32_t flag, const ZoneHint* preferred) {
    std::vector<uint8_t> resp;
    if (count > 0) resp.reserve(static_cast<size_t>(count) * 3);

    for (int zi = 0; zi < count; ++zi) {
        uint8_t sel_pl   = playerid;
        uint8_t loc_byte = LOCATION_MZONE;
        uint8_t seq_byte = 0;
        bool    have_zone = false;

        if (preferred) {
            loc_byte = (preferred->loc == LOCATION_SZONE) ? LOCATION_SZONE : LOCATION_MZONE;
            seq_byte = static_cast<uint8_t>(preferred->seq);
            sel_pl = playerid;
            uint32_t bit = (loc_byte == LOCATION_SZONE) ? (1u << (seq_byte + 8)) : (1u << seq_byte);
            have_zone = !(bit & flag);
        }

        if (!have_zone) {
            // The first free zone: own Monster Zones (bits 0-6), own Spell & Trap
            // Zones (8-15, Field and Pendulum Zones included), then the
            // opponent's (16-22, 24-31): effects that place a card on the
            // opponent's field only leave those clear.
            sel_pl = playerid;
            bool found = false;
            for (uint8_t s = 0; s < 7 && !found; ++s)
                if (!((1u << s) & flag)) { loc_byte = LOCATION_MZONE; seq_byte = s; found = true; }
            for (uint8_t s = 0; s < 8 && !found; ++s)
                if (!((1u << (s + 8)) & flag)) { loc_byte = LOCATION_SZONE; seq_byte = s; found = true; }
            for (uint8_t s = 0; s < 7 && !found; ++s)
                if (!((1u << (s + 16)) & flag)) { sel_pl = 1 - playerid; loc_byte = LOCATION_MZONE; seq_byte = s; found = true; }
            for (uint8_t s = 0; s < 8 && !found; ++s)
                if (!((1u << (s + 24)) & flag)) { sel_pl = 1 - playerid; loc_byte = LOCATION_SZONE; seq_byte = s; found = true; }
            // Nothing free: Monster Zone 0, and the engine reports the error.
        }

        uint32_t used_bit = (sel_pl == playerid)
            ? ((loc_byte == LOCATION_SZONE) ? (1u << (seq_byte + 8))  : (1u << seq_byte))
            : ((loc_byte == LOCATION_SZONE) ? (1u << (seq_byte + 24)) : (1u << (seq_byte + 16)));

        resp.push_back(sel_pl);
        resp.push_back(loc_byte);
        resp.push_back(seq_byte);
        flag |= used_bit;
    }
    return resp;
}

std::vector<uint8_t> encode_i32(int32_t v) {
    std::vector<uint8_t> out(4);
    std::memcpy(out.data(), &v, 4);
    return out;
}

std::vector<uint8_t> encode_card_indices(const std::vector<uint8_t>& indices) {
    std::vector<uint8_t> resp(8 + indices.size());
    int32_t  type = 2;
    uint32_t size = static_cast<uint32_t>(indices.size());
    std::memcpy(resp.data(),     &type, 4);
    std::memcpy(resp.data() + 4, &size, 4);
    if (!indices.empty()) std::memcpy(resp.data() + 8, indices.data(), indices.size());
    return resp;
}

std::vector<uint8_t> encode_card_bitmask(uint32_t bitmask) {
    std::vector<uint8_t> resp(8);
    int32_t type = 3;
    std::memcpy(resp.data(),     &type,    4);
    std::memcpy(resp.data() + 4, &bitmask, 4);
    return resp;
}

std::string to_hex(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s[2 * i]     = digits[data[i] >> 4];
        s[2 * i + 1] = digits[data[i] & 0xf];
    }
    return s;
}

bool from_hex(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.size() % 2) return false;
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = val(hex[i]), lo = val(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>(hi << 4 | lo));
    }
    return true;
}

} // namespace ocgbridge
