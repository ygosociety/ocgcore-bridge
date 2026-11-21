// SPDX-License-Identifier: AGPL-3.0-or-later
// MSG_ANNOUNCE_CARD carries a stack program (the opcodes of
// ocgapi_constants.h / constant.lua) instead of a candidate list, and the
// engine accepts a declared code only if the program evaluates non-zero for
// that card. This is a port of ygopro-core playerop.cpp is_declarable over the
// same card data the engine reads, so it gives the engine's own verdict.
#include "internal.h"

#include <cstring>

namespace ocgbridge {

namespace declare_op {
constexpr uint64_t ADD = 0x4000000000000000, SUB = 0x4000000100000000, MUL = 0x4000000200000000,
    DIV = 0x4000000300000000, AND = 0x4000000400000000, OR = 0x4000000500000000,
    NEG = 0x4000000600000000, NOT = 0x4000000700000000, BAND = 0x4000000800000000,
    BOR = 0x4000000900000000, BNOT = 0x4000001000000000, BXOR = 0x4000001100000000,
    LSHIFT = 0x4000001200000000, RSHIFT = 0x4000001300000000,
    ALLOW_ALIASES = 0x4000001400000000, ALLOW_TOKENS = 0x4000001500000000,
    ISCODE = 0x4000010000000000, ISSETCARD = 0x4000010100000000, ISTYPE = 0x4000010200000000,
    ISRACE = 0x4000010300000000, ISATTRIBUTE = 0x4000010400000000, GETCODE = 0x4000010500000000,
    GETTYPE = 0x4000010700000000, GETRACE = 0x4000010800000000, GETATTRIBUTE = 0x4000010900000000;
}

bool is_declarable(const CardEntry& cd, const std::vector<uint64_t>& ops) {
    using namespace declare_op;
    std::vector<int64_t> st;
    bool alias = false, token = false;
    auto pop = [&]() { int64_t v = st.back(); st.pop_back(); return v; };
    for (uint64_t op : ops) {
        switch (op) {
            case ADD: case SUB: case MUL: case DIV: case AND: case OR:
            case BAND: case BOR: case BXOR: case LSHIFT: case RSHIFT: {
                if (st.size() < 2) break;
                int64_t rhs = pop(), lhs = pop();
                int64_t r = 0;
                switch (op) {
                    case ADD: r = lhs + rhs; break;
                    case SUB: r = lhs - rhs; break;
                    case MUL: r = lhs * rhs; break;
                    case DIV: r = rhs ? lhs / rhs : 0; break;
                    case AND: r = lhs && rhs; break;
                    case OR: r = lhs || rhs; break;
                    case BAND: r = lhs & rhs; break;
                    case BOR: r = lhs | rhs; break;
                    case BXOR: r = lhs ^ rhs; break;
                    case LSHIFT: r = lhs << rhs; break;
                    default: r = lhs >> rhs; break;
                }
                st.push_back(r);
                break;
            }
            case NEG: if (!st.empty()) st.push_back(-pop()); break;
            case NOT: if (!st.empty()) st.push_back(!pop()); break;
            case BNOT: if (!st.empty()) st.push_back(~pop()); break;
            case ISCODE: if (!st.empty()) st.push_back(cd.code == static_cast<uint32_t>(pop())); break;
            case ISTYPE: if (!st.empty()) st.push_back(cd.type & pop()); break;
            case ISRACE: if (!st.empty()) st.push_back(static_cast<int64_t>(cd.race) & pop()); break;
            case ISATTRIBUTE: if (!st.empty()) st.push_back(cd.attribute & pop()); break;
            case GETCODE: st.push_back(cd.code); break;
            case GETTYPE: st.push_back(cd.type); break;
            case GETRACE: st.push_back(static_cast<int64_t>(cd.race)); break;
            case GETATTRIBUTE: st.push_back(cd.attribute); break;
            case ISSETCARD: {
                if (st.empty()) break;
                int32_t set_code = static_cast<int32_t>(pop());
                uint16_t settype = set_code & 0xfff, setsubtype = set_code & 0xf000;
                bool in_set = false;
                for (uint16_t s : cd.setcodes) if ((s & 0xfff) == settype && (s & 0xf000 & setsubtype) == setsubtype) { in_set = true; break; }
                st.push_back(in_set);
                break;
            }
            case ALLOW_ALIASES: alias = true; break;
            case ALLOW_TOKENS: token = true; break;
            default: st.push_back(static_cast<int64_t>(op)); break;
        }
    }
    if (st.size() != 1 || st.back() == 0) return false;
    // ygopro-core card.h's CARD_MARINE_DOLPHIN / CARD_TWINKLE_MOSS are always declarable.
    return cd.code == 78734254u || cd.code == 13857930u
        || ((alias || !cd.alias) && (token || (cd.type & (TYPE_MONSTER + TYPE_TOKEN)) != (TYPE_MONSTER + TYPE_TOKEN)));
}

// A code the engine accepts for this MSG_ANNOUNCE_CARD ([count:u8] + count*u64):
// preferably one in the opponent's deck, then the declaring player's own, then
// any known card. 0 when none fits.
uint32_t Session::pick_declarable_code(const std::vector<uint8_t>& buf) const {
    if (buf.size() < 3 || !cards_) return 0;
    uint8_t player = buf[1];
    uint8_t count = buf[2];
    std::vector<uint64_t> ops;
    for (size_t i = 0; i < count && 3 + (i + 1) * 8 <= buf.size(); ++i) {
        uint64_t v;
        std::memcpy(&v, buf.data() + 3 + i * 8, 8);
        ops.push_back(v);
    }
    auto ok = [&](uint32_t code) {
        const CardEntry* e = cards_->find(code);
        return e && is_declarable(*e, ops);
    };
    for (int side : { 1 - (player & 1), player & 1 })
        for (uint32_t code : deck_codes_[side])
            if (ok(code)) return code;
    for (const auto& [code, entry] : cards_->all())
        if (is_declarable(entry, ops)) return code;
    return 0;
}

} // namespace ocgbridge
