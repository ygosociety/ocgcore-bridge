// SPDX-License-Identifier: AGPL-3.0-or-later
// Decoding of ygopro-core's MSG_SELECT_* / MSG_ANNOUNCE_* / MSG_SORT_* prompt
// messages, and the solvers the auto-responder uses for the prompts that need
// a search (card sums, counters).
//
// Every layout below is ygopro-core's own (playerop.cpp field::process(...)).
// A "buffer" is one engine message: [type:u8][player:u8][payload...] — the
// u32 length prefix OCG_DuelGetMessage puts in front of each message is not
// part of it.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ocgbridge {

// A card in a select list. seq and pos are truncated to 8 bits, as every
// list entry the engine sends fits them.
struct CardRef {
    uint32_t code{0};
    uint8_t  ctrl{0};
    uint8_t  loc{0};
    uint8_t  seq{0};
    uint8_t  pos{0};
    // Activation lists only (idle / battle activatable, chain candidates):
    // the effect's description (aux.Stringid), which tells two effects of one
    // card apart. 0 elsewhere, and for an effect without a description.
    uint64_t desc{0};
};

// MSG_SELECT_IDLECMD:
//   [summonable:u32 + n*(code:u32 ctrl:u8 loc:u8 seq:u32)]   10 B entries
//   [spsummon  :u32 + n*10 B] [repos:u32 + n*(code:u32 ctrl:u8 loc:u8 seq:u8)] 7 B
//   [mset:u32 + n*10 B] [sset:u32 + n*10 B]
//   [activate:u32 + n*(code:u32 ctrl:u8 loc:u8 seq:u32 desc:u64 client_mode:u8)] 19 B
//   [to_bp:u8][to_ep:u8][can_shuffle:u8]
struct IdleData {
    std::vector<CardRef> summonable;
    std::vector<CardRef> sp_summon;
    std::vector<CardRef> reposition;
    std::vector<CardRef> mset;
    std::vector<CardRef> sset;
    std::vector<CardRef> activatable;
    bool to_bp{false};
    bool to_ep{false};
    bool can_shuffle{false};
};

// MSG_SELECT_BATTLECMD:
//   [activate:u32 + n*19 B] [attack:u32 + n*(code:u32 ctrl:u8 loc:u8 seq:u8 direct:u8)] 8 B
//   [to_m2:u8][to_ep:u8]
struct BattleData {
    std::vector<CardRef> activatable;
    std::vector<CardRef> attackable;
    bool to_m2{false};
    bool to_ep{false};
};

// MSG_SELECT_CHAIN:
//   [spe_count:u8][forced:u8][hint_timing:u32][hint_timing_opp:u32]
//   [count:u32] + count*(code:u32 loc_info{ctrl:u8 loc:u8 seq:u32 pos:u32} desc:u64 client_mode:u8) 23 B
// The response is an i32 index into `candidates`, or -1 to pass (not when forced).
struct ChainData {
    bool forced{false};
    // spe_count: candidates that answer what just happened (optional triggers,
    // and activate / quick effects whose hint timing matches the event). 0 = an
    // open game state. 0x7f = choosing which of several triggers to chain.
    uint8_t spe_count{0};
    uint32_t hint_timing{0};
    uint32_t hint_timing_opp{0};
    std::vector<CardRef> candidates;
};

// MSG_SELECT_CARD: [cancelable:u8][min:u32][max:u32][count:u32] + count*(code:u32 + loc_info) 14 B
struct SelectCardData {
    uint32_t min{1};
    uint32_t max{1};
    std::vector<CardRef> candidates;
};

// MSG_SELECT_SUM:
//   [hasNoMax:u8][acc:u32][min:u32][max:u32]
//   [mustCount:u32] + mustCount*(code:u32 + loc_info(10B) + sum_param:u32)
//   [selectCount:u32] + selectCount*(code:u32 + loc_info(10B) + sum_param:u32)
// `sum_param` packs up to two values per card (low 16 bits / high 16 bits);
// the engine accepts either per card. must_select cards are always counted and
// never part of the response: response indices are into `select`.
struct SumCardEntry {
    CardRef ref;
    uint32_t sum_param{0};
};
struct SelectSumData {
    uint32_t acc{0};
    uint32_t min{0};
    uint32_t max{0}; // 0: no explicit max
    // hasNoMax (SelectWithSumGreater, "total Level N or more"): the engine then
    // accepts any selection whose largest total reaches acc with no card to spare.
    bool greater{false};
    std::vector<SumCardEntry> must_select;
    std::vector<SumCardEntry> select;
};

// MSG_SELECT_COUNTER: [countertype:u16][count:u16][size:u32]
//   + size*(code:u32 ctrl:u8 loc:u8 seq:u8 current_count:u16) 9 B
// The response is `size` int16 values: counters to remove from each card.
struct CounterCandidate {
    CardRef ref;
    uint16_t current_count{0};
};
struct SelectCounterData {
    uint16_t countertype{0};
    uint16_t count{0};
    std::vector<CounterCandidate> candidates;
};

// The remaining prompt kinds, decoded into one shape:
// SELECT_CARD:   [cancelable:u8][min:u32][max:u32][count:u32] + count*(code + loc_info)
// SELECT_TRIBUTE:[cancelable:u8][min:u32][max:u32][count:u32]
//                + count*(code:u32 ctrl:u8 loc:u8 seq:u32 release_param:u8) 11 B
//                (min is compared with the SUM of release_param, max with the count)
// SELECT_UNSELECT_CARD: [finishable:u8][cancelable:u8][min:u32][max:u32]
//                [count:u32] + count*(code + loc_info) [count2:u32] + count2*(code + loc_info)
// SELECT_OPTION: [count:u8] + count*u64
// SELECT_YESNO:  [desc:u64]
// SELECT_EFFECTYN: [code:u32][loc_info:10][desc:u64]
// SELECT_POSITION: [code:u32][positions:u8]
// SELECT_PLACE / SELECT_DISFIELD: [count:u8][flag:u32]  (flag bit SET = zone unavailable)
struct PromptData {
    uint8_t  msg{0};
    uint8_t  player{0};
    bool     cancelable{false};
    bool     finishable{false};
    uint32_t min{0};
    uint32_t max{0};
    std::vector<CardRef> cards;        // CARD/TRIBUTE: candidates; UNSELECT: select + unselect
    std::vector<uint8_t> release;      // TRIBUTE: release_param per candidate
    uint32_t select_count{0};          // UNSELECT: how many of `cards` are the select list
    uint32_t code{0};                  // EFFECTYN / POSITION
    CardRef  card;                     // EFFECTYN: the card asking
    uint64_t description{0};           // EFFECTYN / YESNO
    std::vector<uint64_t> options;     // OPTION
    uint8_t  positions{0};             // POSITION
    uint32_t flag{0};                  // PLACE
    uint8_t  count{0};                 // PLACE: zones to pick
};

std::vector<CardRef> parse_card_refs(const uint8_t* p, const uint8_t* end, int count, int entry_bytes = 8);
IdleData          parse_idle_data(const std::vector<uint8_t>& buf);
BattleData        parse_battle_data(const std::vector<uint8_t>& buf);
ChainData         parse_chain_data(const std::vector<uint8_t>& buf);
SelectCardData    parse_select_card_data(const std::vector<uint8_t>& buf);
SelectSumData     parse_select_sum_data(const std::vector<uint8_t>& buf);
SelectCounterData parse_select_counter_data(const std::vector<uint8_t>& buf);
PromptData        parse_prompt(const std::vector<uint8_t>& buf);

// Whether `msg` is one of the prompt messages a duel waits on for an answer.
bool is_prompt_message(uint8_t msg);

// Greedy counter removal: fills candidates in list order up to each one's
// current_count until data.count is reached. found=false only when the total
// available falls short.
std::pair<std::vector<int16_t>, bool> solve_select_counter(const SelectCounterData& data);

// An exact-sum subset of data.select (count within min..max when max is set)
// that, with every must_select card, admits a per-card low/high value choice
// summing to data.acc — ygopro-core's select_sum_check1. Fewest cards first
// (skipping a card is tried before taking it). empty + false when none is found
// within the search budget.
std::pair<std::vector<uint8_t>, bool> solve_select_sum(const SelectSumData& data);

// Whether the engine accepts `idxs` (indices into data.select): the checks of
// field::process(SelectSum&)'s response step. Duplicates are rejected.
bool select_sum_accepts(const SelectSumData& data, const std::vector<uint8_t>& idxs);

// solve_select_sum when it finds an exact sum (legal in both modes), else for a
// "greater" prompt the fewest cards (earliest first) that select_sum_accepts.
std::pair<std::vector<uint8_t>, bool> solve_select_sum_checked(const SelectSumData& data);

// List lookups; -1 on a miss.
int find_by_code(const std::vector<CardRef>& list, uint32_t code);
// First entry at (loc, seq), any controller.
int find_by_loc_seq(const std::vector<CardRef>& list, uint32_t loc, uint32_t seq);
// Exact (code, loc, seq[, ctrl]); ctrl < 0 = any controller.
int find_by_ref(const std::vector<CardRef>& list, uint32_t code, int loc, int seq, int ctrl = -1);

// The lowest `count` set bits of `available` (an ANNOUNCE_ATTRIB / RACE answer).
uint64_t lowest_n_bits(uint64_t available, int count);

// The response to a MSG_SELECT_PLACE / MSG_SELECT_DISFIELD: `count` zones as
// [player:u8, location:u8, sequence:u8] triples, each free in `flag` (bits SET =
// unavailable: own mzone 0-6 / szone 8-15, opponent's mzone 16-22 / szone 24-31)
// and not already picked. `preferred` (when set: own field, loc 0x08 = Spell &
// Trap Zone, anything else = Monster Zone) is tried first for every pick;
// otherwise the first free zone: own mzone, own szone, opponent's mzone, szone.
struct ZoneHint { uint32_t loc{0}; uint32_t seq{0}; };
std::vector<uint8_t> place_response(uint8_t playerid, int count, uint32_t flag, const ZoneHint* preferred);

// Encoders for the response layouts the engine reads.
std::vector<uint8_t> encode_i32(int32_t v);
// [type=2:i32][size:u32][idx:u8...] (parse_response_cards, byte-width indices).
std::vector<uint8_t> encode_card_indices(const std::vector<uint8_t>& indices);
// [type=3:i32][bitmask:u32] (parse_response_cards, one bit per candidate).
std::vector<uint8_t> encode_card_bitmask(uint32_t bitmask);

std::string to_hex(const uint8_t* data, size_t len);
inline std::string to_hex(const std::vector<uint8_t>& v) { return to_hex(v.data(), v.size()); }
bool from_hex(const std::string& hex, std::vector<uint8_t>& out);

} // namespace ocgbridge
