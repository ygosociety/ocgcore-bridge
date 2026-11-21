// SPDX-License-Identifier: AGPL-3.0-or-later
// Unit tests for libocgbridge: message decoding, answer encoding, solvers,
// card sources. No card scripts needed (the golden tests run real duels).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "ocgapi_constants.h"
#include "ocgbridge/answers.h"
#include "ocgbridge/cards.h"
#include "ocgbridge/events.h"
#include "ocgbridge/messages.h"
#include "ocgbridge/session.h"

using namespace ocgbridge;
using json = nlohmann::json;

static int g_failed = 0, g_checks = 0;
static std::string g_test;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::fprintf(stderr, "FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_test.c_str(), #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; auto va = (a); auto vb = (b); if (!(va == vb)) { ++g_failed; std::fprintf(stderr, "FAIL %s:%d [%s] %s == %s\n", __FILE__, __LINE__, g_test.c_str(), #a, #b); } } while (0)

static std::vector<std::pair<const char*, std::function<void()>>>& tests() {
    static std::vector<std::pair<const char*, std::function<void()>>> t;
    return t;
}
struct Reg { Reg(const char* n, std::function<void()> f) { tests().emplace_back(n, std::move(f)); } };
#define TEST(name) static void name(); static Reg reg_##name(#name, name); static void name()

// Little-endian message builder.
struct W {
    std::vector<uint8_t> b;
    W& u8(uint8_t v) { b.push_back(v); return *this; }
    W& u16(uint16_t v) { for (int i = 0; i < 2; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); return *this; }
    W& u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); return *this; }
    W& u64(uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); return *this; }
    W& loc_info(uint8_t ctrl, uint8_t loc, uint32_t seq, uint32_t pos) { return u8(ctrl).u8(loc).u32(seq).u32(pos); }
};

static std::vector<uint8_t> answer(const std::vector<uint8_t>& prompt, const json& a, bool* ok = nullptr, std::string* why = nullptr) {
    std::vector<uint8_t> out;
    std::string reason;
    bool r = encode_answer(prompt, a, out, reason);
    if (ok) *ok = r;
    if (why) *why = reason;
    return r ? out : std::vector<uint8_t>{};
}

static int32_t i32_of(const std::vector<uint8_t>& v) { int32_t x = 0; if (v.size() >= 4) std::memcpy(&x, v.data(), 4); return x; }

TEST(hex_roundtrip) {
    std::vector<uint8_t> v{0x00, 0x0b, 0xff, 0x7a};
    CHECK_EQ(to_hex(v), std::string("000bff7a"));
    std::vector<uint8_t> back;
    CHECK(from_hex("000BFF7a", back));
    CHECK(back == v);
    CHECK(!from_hex("abc", back));
    CHECK(!from_hex("zz", back));
}

// A real MSG_SELECT_IDLECMD (a turn-1 Main Phase with three Spells in hand).
TEST(idle_decode) {
    std::vector<uint8_t> buf;
    CHECK(from_hex("0b0000000000000000000000000000000000030000004a704903000202000000ef2751000002030000007df6bf00000204000000010000004a7049030002020000000000a0049734000000000101", buf));
    IdleData d = parse_idle_data(buf);
    CHECK(d.summonable.empty());
    CHECK_EQ(d.sset.size(), size_t(3));
    CHECK_EQ(d.sset[0].code, 55144522u);
    CHECK_EQ(d.sset[1].code, 5318639u);
    CHECK_EQ(d.sset[2].code, 12580477u);
    CHECK_EQ(d.sset[2].seq, 4);
    CHECK_EQ(d.activatable.size(), size_t(1));
    CHECK_EQ(d.activatable[0].code, 55144522u);
    CHECK_EQ(d.activatable[0].desc, 57823222300672ull);
    CHECK(!d.to_bp);
    CHECK(d.to_ep);
    CHECK(d.can_shuffle);
    bool ok;
    CHECK_EQ(i32_of(answer(buf, {{"action", "sset"}, {"index", 2}}, &ok)), (4 | (2 << 16)));
    CHECK(ok);
    CHECK_EQ(i32_of(answer(buf, {{"action", "activate"}, {"index", 0}})), 5);
    CHECK_EQ(i32_of(answer(buf, {{"action", "end"}})), 7);
    answer(buf, {{"action", "battle"}}, &ok);
    CHECK(!ok);
    answer(buf, {{"action", "summon"}, {"index", 0}}, &ok);
    CHECK(!ok);
    CHECK_EQ(i32_of(answer(buf, {{"i32", 6}})), 6); // unchecked
}

TEST(battle_decode) {
    W w;
    w.u8(MSG_SELECT_BATTLECMD).u8(1);
    w.u32(1).u32(1234).u8(1).u8(LOCATION_MZONE).u32(2).u64(77).u8(0);                                    // activatable
    w.u32(2).u32(10).u8(1).u8(LOCATION_MZONE).u8(0).u8(0).u32(11).u8(1).u8(LOCATION_MZONE).u8(3).u8(1); // attackable
    w.u8(1).u8(0);
    BattleData d = parse_battle_data(w.b);
    CHECK_EQ(d.activatable.size(), size_t(1));
    CHECK_EQ(d.activatable[0].desc, 77ull);
    CHECK_EQ(d.attackable.size(), size_t(2));
    CHECK_EQ(d.attackable[1].code, 11u);
    CHECK_EQ(d.attackable[1].seq, 3);
    CHECK(d.to_m2);
    CHECK(!d.to_ep);
    CHECK_EQ(i32_of(answer(w.b, {{"action", "attack"}, {"index", 1}})), (1 | (1 << 16)));
    CHECK_EQ(i32_of(answer(w.b, {{"action", "main2"}})), 2);
    bool ok;
    answer(w.b, {{"action", "end"}}, &ok);
    CHECK(!ok);
}

static std::vector<uint8_t> chain_msg(bool forced) {
    W w;
    w.u8(MSG_SELECT_CHAIN).u8(0).u8(1).u8(forced ? 1 : 0).u32(0x4).u32(0x8);
    w.u32(2);
    w.u32(500).loc_info(0, LOCATION_HAND, 1, POS_FACEUP_ATTACK).u64(8000).u8(0);
    w.u32(600).loc_info(0, LOCATION_SZONE, 2, POS_FACEDOWN_DEFENSE).u64(9000).u8(0);
    return w.b;
}

TEST(chain_decode) {
    auto buf = chain_msg(false);
    ChainData d = parse_chain_data(buf);
    CHECK(!d.forced);
    CHECK_EQ(d.spe_count, 1);
    CHECK_EQ(d.hint_timing, 0x4u);
    CHECK_EQ(d.hint_timing_opp, 0x8u);
    CHECK_EQ(d.candidates.size(), size_t(2));
    CHECK_EQ(d.candidates[1].code, 600u);
    CHECK_EQ(d.candidates[1].loc, LOCATION_SZONE);
    CHECK_EQ(d.candidates[1].pos, POS_FACEDOWN_DEFENSE);
    CHECK_EQ(d.candidates[1].desc, 9000ull);
    CHECK_EQ(i32_of(answer(buf, {{"index", -1}})), -1);
    CHECK_EQ(i32_of(answer(buf, {{"index", 1}})), 1);
    bool ok;
    answer(chain_msg(true), {{"index", -1}}, &ok);
    CHECK(!ok);
    answer(buf, {{"index", 2}}, &ok);
    CHECK(!ok);
}

static std::vector<uint8_t> select_card_msg(bool cancelable, uint32_t min, uint32_t max) {
    W w;
    w.u8(MSG_SELECT_CARD).u8(0).u8(cancelable ? 1 : 0).u32(min).u32(max).u32(3);
    w.u32(1).loc_info(0, LOCATION_DECK, 0, POS_FACEDOWN_DEFENSE);
    w.u32(2).loc_info(0, LOCATION_DECK, 1, POS_FACEDOWN_DEFENSE);
    w.u32(3).loc_info(1, LOCATION_MZONE, 2, POS_FACEUP_ATTACK);
    return w.b;
}

TEST(select_card) {
    auto buf = select_card_msg(true, 1, 2);
    SelectCardData sd = parse_select_card_data(buf);
    CHECK_EQ(sd.min, 1u);
    CHECK_EQ(sd.max, 2u);
    CHECK_EQ(sd.candidates.size(), size_t(3));
    CHECK_EQ(sd.candidates[2].ctrl, 1);
    PromptData pd = parse_prompt(buf);
    CHECK(pd.cancelable);
    CHECK_EQ(pd.cards.size(), size_t(3));
    CHECK_EQ(find_by_loc_seq(pd.cards, LOCATION_MZONE, 2), 2);
    CHECK_EQ(find_by_loc_seq(pd.cards, LOCATION_MZONE, 1), -1);
    auto r = answer(buf, {{"indices", {0, 2}}});
    CHECK(r == encode_card_indices({0, 2}));
    CHECK_EQ(i32_of(answer(buf, {{"indices", json::array()}})), -1); // cancel
    bool ok; std::string why;
    answer(buf, {{"indices", {0, 0}}}, &ok, &why);
    CHECK(!ok); CHECK_EQ(why, std::string("duplicate index"));
    answer(buf, {{"indices", {0, 1, 2}}}, &ok, &why);
    CHECK(!ok); CHECK_EQ(why, std::string("too many cards"));
    answer(select_card_msg(false, 2, 2), {{"indices", {0}}}, &ok, &why);
    CHECK(!ok); CHECK_EQ(why, std::string("too few cards"));
    answer(buf, json::array(), &ok, &why);
    CHECK(!ok); CHECK_EQ(why, std::string("missing indices"));
}

TEST(select_tribute) {
    W w;
    w.u8(MSG_SELECT_TRIBUTE).u8(0).u8(0).u32(2).u32(2).u32(2);
    w.u32(1).u8(0).u8(LOCATION_MZONE).u32(0).u8(2); // counts as 2 tributes
    w.u32(2).u8(0).u8(LOCATION_MZONE).u32(1).u8(1);
    PromptData d = parse_prompt(w.b);
    CHECK_EQ(d.cards.size(), size_t(2));
    CHECK_EQ(int(d.release[0]), 2);
    CHECK(answer(w.b, {{"indices", {0}}}) == encode_card_indices({0}));
    bool ok;
    answer(w.b, {{"indices", {1}}}, &ok);
    CHECK(!ok);
    CHECK(encode_card_bitmask(5) == std::vector<uint8_t>({3, 0, 0, 0, 5, 0, 0, 0}));
}

static std::vector<uint8_t> sum_msg(bool greater, uint32_t acc, uint32_t min, uint32_t max, std::vector<uint32_t> params) {
    W w;
    w.u8(MSG_SELECT_SUM).u8(0).u8(greater ? 1 : 0).u32(acc).u32(min).u32(max).u32(0).u32(static_cast<uint32_t>(params.size()));
    uint32_t i = 0;
    for (uint32_t p : params) { w.u32(100 + i).loc_info(0, LOCATION_HAND, i, 0).u32(p); ++i; }
    return w.b;
}

TEST(select_sum) {
    // Levels 4, 4, 3 and a card that counts as 2 or 6: exact 8 with 1..3 cards.
    auto buf = sum_msg(false, 8, 1, 3, {4, 4, 3, 2 | (6 << 16)});
    SelectSumData sd = parse_select_sum_data(buf);
    CHECK(!sd.greater);
    CHECK_EQ(sd.select.size(), size_t(4));
    auto [idx, found] = solve_select_sum(sd);
    CHECK(found);
    CHECK(select_sum_accepts(sd, idx));
    CHECK(!select_sum_accepts(sd, {2, 3}));   // 3 + 2 or 3 + 6: never 8
    CHECK(select_sum_accepts(sd, {0, 3, 2}) == false); // 4 + 3 + (2|6)
    CHECK(!select_sum_accepts(sd, {0}));
    CHECK(select_sum_accepts(sd, {0, 1}));
    CHECK(!select_sum_accepts(sd, {0, 0}));
    // "Total 8 or more" with no exact sum: the checked solver finds the fewest cards.
    auto gbuf = sum_msg(true, 8, 1, 0, {3, 6, 7});
    SelectSumData gd = parse_select_sum_data(gbuf);
    CHECK(gd.greater);
    CHECK(!solve_select_sum(gd).second);
    auto [g, gfound] = solve_select_sum_checked(gd);
    CHECK(gfound);
    CHECK(select_sum_accepts(gd, g));
    CHECK_EQ(g.size(), size_t(2));
    bool ok;
    answer(gbuf, {{"indices", {0}}}, &ok);
    CHECK(!ok);
    CHECK(answer(gbuf, {{"indices", {1, 2}}}, &ok) == encode_card_indices({1, 2}));
    CHECK(ok);
}

TEST(select_sum_fewest_first) {
    // The exact solver prefers fewer cards: the single 8 rather than 4 + 4.
    auto buf = sum_msg(false, 8, 1, 3, {4, 4, 8});
    auto [idx, found] = solve_select_sum(parse_select_sum_data(buf));
    CHECK(found);
    CHECK(idx == std::vector<uint8_t>({2}));
}

TEST(select_counter) {
    W w;
    w.u8(MSG_SELECT_COUNTER).u8(0).u16(0x1).u16(3).u32(2);
    w.u32(1).u8(0).u8(LOCATION_SZONE).u8(0).u16(2);
    w.u32(2).u8(0).u8(LOCATION_MZONE).u8(1).u16(5);
    SelectCounterData d = parse_select_counter_data(w.b);
    CHECK_EQ(d.count, 3);
    CHECK_EQ(d.candidates.size(), size_t(2));
    auto [counts, found] = solve_select_counter(d);
    CHECK(found);
    CHECK(counts == std::vector<int16_t>({2, 1}));
    auto r = answer(w.b, {{"counts", {0, 3}}});
    CHECK(r == std::vector<uint8_t>({0, 0, 3, 0}));
    bool ok;
    answer(w.b, {{"counts", {3, 0}}}, &ok);
    CHECK(!ok);
}

TEST(place) {
    // Own Monster Zones 0-1 taken; everything else free.
    const uint32_t flag = 0x3;
    ZoneHint want{LOCATION_MZONE, 3};
    CHECK(place_response(0, 1, flag, &want) == std::vector<uint8_t>({0, LOCATION_MZONE, 3}));
    ZoneHint taken{LOCATION_MZONE, 1};
    CHECK(place_response(0, 1, flag, &taken) == std::vector<uint8_t>({0, LOCATION_MZONE, 2}));
    ZoneHint st{LOCATION_SZONE, 2};
    CHECK(place_response(1, 1, flag, &st) == std::vector<uint8_t>({1, LOCATION_SZONE, 2}));
    // Two picks: the preferred zone, then the first free one.
    CHECK(place_response(0, 2, flag, &want) == std::vector<uint8_t>({0, LOCATION_MZONE, 3, 0, LOCATION_MZONE, 2}));
    // Only the opponent's Monster Zone 4 is free.
    const uint32_t opp_only = ~(1u << (16 + 4));
    CHECK(place_response(0, 1, opp_only, nullptr) == std::vector<uint8_t>({1, LOCATION_MZONE, 4}));
    // Own Spell & Trap Zone when the Monster Zones are all taken.
    CHECK(place_response(0, 1, 0x7F, nullptr) == std::vector<uint8_t>({0, LOCATION_SZONE, 0}));

    W w;
    w.u8(MSG_SELECT_PLACE).u8(0).u8(1).u32(flag);
    bool ok; std::string why;
    CHECK(answer(w.b, {{"player", 0}, {"loc", LOCATION_MZONE}, {"seq", 4}}) == std::vector<uint8_t>({0, LOCATION_MZONE, 4}));
    answer(w.b, {{"player", 0}, {"loc", LOCATION_MZONE}, {"seq", 0}}, &ok, &why);
    CHECK(!ok); CHECK_EQ(why, std::string("zone not available"));
    answer(w.b, {{"places", json::array()}}, &ok, &why);
    CHECK(!ok); CHECK_EQ(why, std::string("wrong number of zones"));
}

TEST(small_prompts) {
    W yn; yn.u8(MSG_SELECT_YESNO).u8(0).u64(1234);
    CHECK_EQ(parse_prompt(yn.b).description, 1234ull);
    CHECK_EQ(i32_of(answer(yn.b, {{"yes", true}})), 1);
    CHECK_EQ(i32_of(answer(yn.b, {{"yes", 0}})), 0);
    bool ok;
    answer(yn.b, {{"yes", 2}}, &ok);
    CHECK(!ok);

    W eyn; eyn.u8(MSG_SELECT_EFFECTYN).u8(1).u32(42).loc_info(1, LOCATION_MZONE, 3, POS_FACEUP_ATTACK).u64(99);
    PromptData e = parse_prompt(eyn.b);
    CHECK_EQ(e.code, 42u);
    CHECK_EQ(e.card.seq, 3);
    CHECK_EQ(e.description, 99ull);

    W opt; opt.u8(MSG_SELECT_OPTION).u8(0).u8(2).u64(10).u64(20);
    CHECK_EQ(parse_prompt(opt.b).options.size(), size_t(2));
    CHECK_EQ(i32_of(answer(opt.b, {{"option", 1}})), 1);
    answer(opt.b, {{"option", 2}}, &ok);
    CHECK(!ok);

    W pos; pos.u8(MSG_SELECT_POSITION).u8(0).u32(42).u8(POS_FACEUP_ATTACK | POS_FACEUP_DEFENSE);
    CHECK_EQ(i32_of(answer(pos.b, {{"position", POS_FACEUP_DEFENSE}})), (int32_t)POS_FACEUP_DEFENSE);
    answer(pos.b, {{"position", POS_FACEDOWN_DEFENSE}}, &ok);
    CHECK(!ok);

    W uns; uns.u8(MSG_SELECT_UNSELECT_CARD).u8(0).u8(1).u8(0).u32(0).u32(2).u32(1);
    uns.u32(5).loc_info(0, LOCATION_GRAVE, 0, 0).u32(0);
    PromptData u = parse_prompt(uns.b);
    CHECK(u.finishable);
    CHECK_EQ(u.select_count, 1u);
    CHECK_EQ(i32_of(answer(uns.b, {{"index", -1}})), -1);
    CHECK_EQ(answer(uns.b, {{"index", 0}}).size(), size_t(8));

    W rps; rps.u8(MSG_ROCK_PAPER_SCISSORS).u8(0);
    CHECK_EQ(i32_of(answer(rps.b, {{"hand", 3}})), 3);

    W attr; attr.u8(MSG_ANNOUNCE_ATTRIB).u8(0).u8(1).u32(0x30);
    CHECK_EQ(i32_of(answer(attr.b, {{"value", 0x20}})), 0x20);
    answer(attr.b, {{"value", 0x01}}, &ok);
    CHECK(!ok);
    CHECK_EQ(lowest_n_bits(0x34, 2), 0x14ull);

    W race; race.u8(MSG_ANNOUNCE_RACE).u8(0).u8(1).u64(0x2000ull | (1ull << 40));
    CHECK_EQ(answer(race.b, {{"value", std::to_string(1ull << 40)}}).size(), size_t(8));

    W num; num.u8(MSG_ANNOUNCE_NUMBER).u8(0).u8(3).u64(1).u64(2).u64(3);
    CHECK_EQ(i32_of(answer(num.b, {{"index", 2}})), 2);

    W sort; sort.u8(MSG_SORT_CHAIN).u8(0).u32(2);
    sort.u32(1).u8(0).u32(LOCATION_MZONE).u32(0).u32(2).u8(0).u32(LOCATION_MZONE).u32(1);
    CHECK(answer(sort.b, {{"order", nullptr}}) == std::vector<uint8_t>({0xFF}));
    CHECK(answer(sort.b, {{"order", {1, 0}}}) == std::vector<uint8_t>({1, 0}));
    answer(sort.b, {{"order", {0, 0}}}, &ok);
    CHECK(!ok);

    CHECK(answer(sort.b, {{"raw", "0a0b"}}) == std::vector<uint8_t>({0x0a, 0x0b}));
    answer({}, {{"raw", "00"}}, &ok);
    CHECK(!ok);
}

TEST(rules_and_seed) {
    CHECK(rules_flags("mr5").value() == static_cast<uint64_t>(DUEL_MODE_MR5));
    CHECK(rules_flags("goat").value() == static_cast<uint64_t>(DUEL_MODE_GOAT));
    CHECK(rules_flags("mr2").value() == static_cast<uint64_t>(DUEL_MODE_MR2));
    CHECK(!rules_flags("edison").has_value());
    auto a = expand_seed(42), b = expand_seed(42), c = expand_seed(43);
    CHECK(a == b);
    CHECK(a != c);
    // SplitMix64's first output for seed 0.
    CHECK_EQ(expand_seed(0)[0], 0xE220A8397B1DCDAFull);
}

TEST(prompt_kinds) {
    CHECK_EQ(prompt_msg_of_kind("card"), MSG_SELECT_CARD);
    CHECK_EQ(prompt_msg_of_kind("announceAttribute"), MSG_ANNOUNCE_ATTRIB);
    CHECK_EQ(prompt_msg_of_kind("nope"), 0);
    CHECK_EQ(std::string(prompt_kind(MSG_SELECT_UNSELECT_CARD)), std::string("unselectCard"));
    CHECK_EQ(std::string(prompt_kind(250)), std::string("other"));
    CHECK(is_prompt_message(MSG_SELECT_SUM));
    CHECK(!is_prompt_message(MSG_MOVE));
}

static std::string fixture(const char* name) { return std::string(OCGBRIDGE_FIXTURES) + "/" + name; }

TEST(cards_json) {
    CardDatabase db;
    JsonSourceOptions opts;
    opts.setcodes_json = fixture("mini-setcodes.json");
    opts.archetype_constants = fixture("archetype_setcode_constants.lua");
    std::string err;
    long n = db.load_ygoprodeck_json(fixture("mini-cards.json"), opts, &err);
    CHECK_EQ(n, 8L); // 6 cards, 1 artwork, 1 token
    const CardEntry* w = db.find(100);
    CHECK(w != nullptr);
    if (w) {
        CHECK_EQ(w->type, 0x21u);       // Monster | Effect
        CHECK_EQ(w->race, 0x1ull);      // Warrior
        CHECK_EQ(w->attribute, 0x20u);  // DARK
        CHECK(w->setcodes == std::vector<uint16_t>({0x115})); // from the archetype name
    }
    const CardEntry* art = db.find(1001);
    CHECK(art && art->alias == 100);
    CHECK(db.art_codes(100) && db.art_codes(100)->at(0) == 1001u);
    const CardEntry* f = db.find(200);
    CHECK(f && f->type == (0x1u | 0x20u | 0x40u)); // the typeline's Effect + Fusion
    CHECK(f && f->setcodes == std::vector<uint16_t>({4357, 115}));
    CHECK(f && f->alias == 201);
    const CardEntry* l = db.find(300);
    CHECK(l && l->level == 2 && l->link_marker == (0x002u | 0x080u));
    const CardEntry* q = db.find(400);
    CHECK(q && q->type == (0x2u | 0x10000u));
    const CardEntry* c = db.find(500);
    CHECK(c && c->type == (0x4u | 0x100000u));
    const CardEntry* t = db.find(600);
    CHECK(t && t->type == (0x1u | 0x10u | 0x1000u)); // no typeline: read as a Normal Tuner
    const CardEntry* tok = db.find(900);
    CHECK(tok && tok->type == 16401u);
    CHECK(db.load_ygoprodeck_json(fixture("missing.json"), opts, &err) == -1);
}

TEST(cards_cdb) {
    CardDatabase db;
    std::string err;
    long n = db.load_cdb(fixture("mini.cdb"), &err);
    if (!CardDatabase::cdb_supported()) { CHECK_EQ(n, -1L); return; }
    CHECK_EQ(n, 5L);
    const CardEntry* m = db.find(100);
    CHECK(m && m->setcodes == std::vector<uint16_t>({0x115, 0x1115}));
    CHECK(m && !m->pack_scales);
    const CardEntry* a = db.find(101);
    CHECK(a && a->alias == 100);
    const CardEntry* p = db.find(200);
    CHECK(p && p->level == 4 && p->lscale == 8 && p->rscale == 8);
    const CardEntry* l = db.find(300);
    CHECK(l && l->link_marker == (0x002u | 0x080u) && l->defense == 0);
    // A second source never replaces a card already loaded.
    CardDatabase both;
    both.load_cdb(fixture("mini.cdb"), &err);
    JsonSourceOptions opts;
    both.load_ygoprodeck_json(fixture("mini-cards.json"), opts, &err);
    CHECK(both.find(100) && !both.find(100)->pack_scales);
    CHECK(both.find(600) != nullptr);
}

TEST(message_names) {
    CHECK_EQ(std::string(message_name(MSG_MOVE)), std::string("move"));
    CHECK_EQ(std::string(message_name(MSG_NEW_TURN)), std::string("newTurn"));
    CHECK_EQ(message_of_name("chainSolving"), static_cast<uint8_t>(MSG_CHAIN_SOLVING));
    CHECK_EQ(message_of_name("nope"), static_cast<uint8_t>(0));
    CHECK_EQ(std::string(message_name(250)), std::string("unknown"));
}

TEST(decode_events) {
    W move;
    move.u8(MSG_MOVE).u32(89631139).loc_info(0, LOCATION_HAND, 2, POS_FACEDOWN).loc_info(0, LOCATION_MZONE, 1, POS_FACEUP_ATTACK).u32(0x40);
    json m = decode_message(move.b.data(), move.b.size());
    CHECK_EQ(m["name"].get<std::string>(), std::string("move"));
    CHECK_EQ(m["code"].get<uint32_t>(), 89631139u);
    CHECK_EQ(m["from"]["loc"].get<int>(), LOCATION_HAND);
    CHECK_EQ(m["to"]["seq"].get<int>(), 1);
    CHECK_EQ(m["reason"].get<uint32_t>(), 0x40u);
    W dmg;
    dmg.u8(MSG_DAMAGE).u8(1).u32(1500);
    m = EngineMessage{ MSG_DAMAGE, dmg.b.data(), dmg.b.size() }.decoded();
    CHECK_EQ(m["player"].get<int>(), 1);
    CHECK_EQ(m["amount"].get<int>(), 1500);
    W direct;
    direct.u8(MSG_ATTACK).loc_info(0, LOCATION_MZONE, 0, POS_FACEUP_ATTACK).loc_info(0, 0, 0, 0);
    m = decode_message(direct.b.data(), direct.b.size());
    CHECK(m["target"].is_null());
    CHECK_EQ(m["attacker"]["loc"].get<int>(), LOCATION_MZONE);
    // Too short for its layout: the name only.
    W cut;
    cut.u8(MSG_DAMAGE).u8(1);
    m = decode_message(cut.b.data(), cut.b.size());
    CHECK(!m.contains("amount"));
    CHECK_EQ(m["name"].get<std::string>(), std::string("damage"));
    // A message without a decoding.
    W other;
    other.u8(MSG_SUMMONED);
    m = decode_message(other.b.data(), other.b.size());
    CHECK_EQ(m.size(), static_cast<size_t>(2));
}

TEST(in_effect_kinds) {
    const auto& k = in_effect_prompt_msgs();
    CHECK(k.count(MSG_SELECT_CARD) && k.count(MSG_ANNOUNCE_CARD) && k.count(MSG_ROCK_PAPER_SCISSORS));
    CHECK(!k.count(MSG_SELECT_IDLECMD) && !k.count(MSG_SELECT_BATTLECMD) && !k.count(MSG_SELECT_CHAIN));
    CHECK_EQ(k.size(), static_cast<size_t>(18));
}

TEST(queries_registry) {
    CardDatabase db;
    std::string err;
    CHECK(db.load_ygoprodeck_json(fixture("mini-cards.json"), {}, &err) > 0);
    Session s(&db, "");
    const auto names = s.query_names();
    CHECK(std::find(names.begin(), names.end(), "card") != names.end());
    CHECK(std::find(names.begin(), names.end(), "fieldCard") != names.end());
    const uint32_t code = db.all().begin()->first;
    json c = s.query("card", {{"code", code}});
    CHECK_EQ(c["code"].get<uint32_t>(), code);
    CHECK(s.query("card", {{"code", 1u}}).is_null());
    bool threw = false;
    try { s.query("card", json::object()); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { s.query("nope", json::object()); } catch (const std::out_of_range&) { threw = true; }
    CHECK(threw);
    // No duel: a field query has nothing to read.
    CHECK(s.query("fieldCard", {{"ctrl", 0}, {"loc", LOCATION_MZONE}, {"seq", 0}}).is_null());
    s.register_query("twice", [](Session&, const json& a) { return json(a.value("n", 0) * 2); });
    CHECK_EQ(s.query("twice", {{"n", 21}}).get<int>(), 42);
    s.unregister_query("twice");
    CHECK(!s.has_query("twice"));
}

TEST(filters_registry) {
    Session s(nullptr, "");
    int seen = 0;
    const int a = s.add_message_filter([&](Session&, const EngineMessage&) { ++seen; return FilterResult::Pass; });
    const int b = s.add_message_filter([&](Session&, const EngineMessage&) { return FilterResult::Drop; });
    CHECK(a != b);
    s.remove_message_filter(a);
    s.remove_message_filter(b);
    s.remove_message_filter(12345); // unknown ids are ignored
    CHECK_EQ(seen, 0);
    CHECK(s.script_dirs() == default_script_dirs());
    s.set_script_dirs({"", "custom"});
    CHECK_EQ(s.script_dirs().size(), static_cast<size_t>(2));
    CHECK(s.preload_scripts() == default_preload_scripts());
}

int main(int argc, char** argv) {
    const char* only = argc > 1 ? argv[1] : nullptr;
    for (auto& [name, fn] : tests()) {
        if (only && std::strcmp(only, name) != 0) continue;
        g_test = name;
        fn();
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
