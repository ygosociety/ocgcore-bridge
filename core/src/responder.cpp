// SPDX-License-Identifier: AGPL-3.0-or-later
// The auto-responder: default answers for the prompts inside an action, the
// caller's hints, the ask hook and holds.
#include "internal.h"

#include <cstring>

namespace ocgbridge {

// A prompt with exactly one possible answer (one option, one position) is no
// choice: it is never held unless hold_trivial says so.
bool Session::has_single_answer() const {
    if (pending_msg_ != MSG_SELECT_OPTION && pending_msg_ != MSG_SELECT_POSITION) return false;
    PromptData d = parse_prompt(pending_buf_);
    if (pending_msg_ == MSG_SELECT_OPTION) return d.options.size() == 1;
    return d.positions != 0 && (d.positions & (d.positions - 1)) == 0;
}

bool Session::ask(std::vector<uint8_t>& resp) {
    return ask_hook_ && ask_hook_(*this, prompt_json(), resp);
}

// The ask hook, for the pending player's prompt kinds it was configured for:
// answered in place, so the loop that reached the prompt carries on exactly as
// with a default answer. false: not asked, or no answer (the default follows).
bool Session::answer_by_ask(const Hints& hints) {
    if (!ask_hook_ || !hints.ask) return false;
    if (pending_player_ != 0 && pending_player_ != 1) return false;
    if (!responder_.ask_msgs[pending_player_].count(pending_msg_)) return false;
    std::vector<uint8_t> resp;
    if (!ask(resp)) return false;
    submit(resp);
    answer_directed_ = true;
    answer_asked_ = true;
    return true;
}

// A replacement default: the configured fixed answer when it is legal for this
// prompt, then the registered function.
bool Session::answer_by_replacement() {
    auto fixed = responder_.default_answers.find(pending_msg_);
    if (fixed != responder_.default_answers.end()) {
        std::vector<uint8_t> resp;
        std::string reason;
        if (encode_answer(pending_buf_, fixed->second, resp, reason)) {
            submit(resp);
            answer_replaced_ = true;
            return true;
        }
    }
    auto fn = default_fns_.find(pending_msg_);
    if (fn != default_fns_.end() && fn->second) {
        std::vector<uint8_t> resp;
        if (fn->second(*this, prompt_json(), resp)) {
            submit(resp);
            answer_replaced_ = true;
            return true;
        }
    }
    return false;
}

// What comes before a kind's built-in default once no hint applied: the ask
// hook, a hold, a replacement default.
Session::Pre Session::before_default(const Hints& hints) {
    if (answer_by_ask(hints)) return Pre::Answered;
    if (hold(hints)) return Pre::Held;
    if (answer_by_replacement()) return Pre::Answered;
    return Pre::Continue;
}

bool Session::hold(const Hints& hints) {
    if (pending_player_ != 0 && pending_player_ != 1) return false;
    if (!(hints.hold_players & (1u << pending_player_))) return false;
    if (!responder_.hold_msgs.count(pending_msg_)) return false;
    if (!responder_.hold_trivial && has_single_answer()) return false;
    return true;
}

void Session::set_default_answer(uint8_t msg, DefaultAnswer fn) {
    if (fn) default_fns_[msg] = std::move(fn);
    else default_fns_.erase(msg);
}

const std::set<uint8_t>& in_effect_prompt_msgs() {
    static const std::set<uint8_t> MSGS = {
        MSG_SELECT_CARD, MSG_SELECT_TRIBUTE, MSG_SELECT_UNSELECT_CARD, MSG_SELECT_EFFECTYN, MSG_SELECT_YESNO,
        MSG_SELECT_OPTION, MSG_SELECT_POSITION, MSG_SELECT_PLACE, MSG_SELECT_DISFIELD, MSG_SELECT_SUM,
        MSG_SELECT_COUNTER, MSG_SORT_CARD, MSG_SORT_CHAIN, MSG_ANNOUNCE_RACE, MSG_ANNOUNCE_ATTRIB,
        MSG_ANNOUNCE_CARD, MSG_ANNOUNCE_NUMBER, MSG_ROCK_PAPER_SCISSORS,
    };
    return MSGS;
}

static bool zone_free(uint32_t flag, const ZoneHint& z) {
    const uint8_t seq = static_cast<uint8_t>(z.seq);
    uint32_t bit = (z.loc == LOCATION_SZONE) ? (1u << (seq + 8)) : (1u << seq);
    return !(bit & flag);
}

// Runs before_default() and leaves answer_pending() when it answered or held.
#define BEFORE_DEFAULT()                                         \
    switch (before_default(hints)) {                             \
        case Pre::Answered: return true;                         \
        case Pre::Held: held = true; return false;               \
        case Pre::Continue: break;                               \
    }

bool Session::answer_pending(const Hints& hints, bool& held) {
    held = false;
    if (!hints.autorespond) return false;
    if (responder_.manual_msgs.count(pending_msg_)) return false;
    const auto& buf = pending_buf_;
    switch (pending_msg_) {
        // Decision points: only a replacement default answers them.
        case MSG_SELECT_IDLECMD:
        case MSG_SELECT_BATTLECMD:
        case MSG_SELECT_CHAIN:
            return answer_by_replacement();
        case 0:
            return false;

        // [type][player][count][disabled_mask:u32]; bits SET = unavailable.
        // Response: count * [player:u8, location:u8, sequence:u8].
        // MSG_SELECT_DISFIELD has the same payload and response.
        case MSG_SELECT_PLACE:
        case MSG_SELECT_DISFIELD: {
            uint8_t playerid = (buf.size() > 1) ? buf[1] : 0;
            int     count    = (buf.size() > 2) ? (int)buf[2] : 1;
            uint32_t flag    = 0;
            if (buf.size() >= 7) std::memcpy(&flag, buf.data() + 3, 4);
            const bool dest_ok = count == 1 && hints.place && zone_free(flag, *hints.place);
            if (!dest_ok) { BEFORE_DEFAULT(); }
            // A disfield pick is not reported as directed, even with a hint.
            answer_directed_ = dest_ok && pending_msg_ == MSG_SELECT_PLACE;
            submit(place_response(playerid, count, flag, hints.place ? &*hints.place : nullptr));
            return true;
        }

        // [type][player][code:u32][positions:u8]: the lowest offered position.
        case MSG_SELECT_POSITION: {
            BEFORE_DEFAULT();
            uint8_t mask = (buf.size() >= 7) ? buf[6] : 0x01u;
            submit(encode_i32(static_cast<int32_t>(mask & (~mask + 1))));
            return true;
        }

        // The hinted tributes, else the first candidates until their release
        // values reach min. Response: a candidate bitmask.
        case MSG_SELECT_TRIBUTE: {
            PromptData td = parse_prompt(buf);
            uint32_t bitmask = 0;
            for (const auto& tz : hints.tributes) {
                int i = find_by_loc_seq(td.cards, tz.loc, tz.seq);
                if (i >= 0 && i < 32) bitmask |= (1u << i);
            }
            if (bitmask == 0) { BEFORE_DEFAULT(); }
            answer_directed_ = bitmask != 0;
            if (bitmask == 0) {
                uint32_t sum = 0;
                for (size_t i = 0; i < td.cards.size() && i < 32 && sum < td.min; ++i) {
                    bitmask |= (1u << i);
                    sum += td.release[i] ? td.release[i] : 1;
                }
                if (bitmask == 0) bitmask = 1u;
            }
            submit(encode_card_bitmask(bitmask));
            return true;
        }

        // Yes, except the very same prompt answered yes effectyn_repeat_limit
        // times this turn (a script loop: the effect can do nothing and asks again).
        case MSG_SELECT_EFFECTYN: {
            BEFORE_DEFAULT();
            int& yeses = effectyn_yeses_[buf];
            const int limit = responder_.effectyn_repeat_limit;
            submit(encode_i32(yeses < limit ? 1 : 0));
            if (yeses < limit) yeses++;
            return true;
        }

        case MSG_SELECT_YESNO:
            BEFORE_DEFAULT();
            submit(encode_i32(0));
            return true;

        case MSG_SELECT_OPTION:
            BEFORE_DEFAULT();
            submit(encode_i32(0));
            return true;

        // The hinted attack target or materials, else (unless deferred) the
        // first `min` candidates.
        case MSG_SELECT_CARD: {
            if (hints.attack_target) {
                SelectCardData sd = parse_select_card_data(buf);
                int i = find_by_loc_seq(sd.candidates, hints.attack_target->loc, hints.attack_target->seq);
                if (i >= 0) {
                    submit(encode_card_indices({ static_cast<uint8_t>(i) }));
                    answer_directed_ = true;
                    return true;
                }
            }
            if (!hints.materials.empty()) {
                SelectCardData sd = parse_select_card_data(buf);
                std::vector<uint8_t> idxs;
                for (const auto& mz : hints.materials) {
                    int i = find_by_loc_seq(sd.candidates, mz.loc, mz.seq);
                    if (i >= 0) idxs.push_back(static_cast<uint8_t>(i));
                }
                // Only when every named material is a candidate.
                if (idxs.size() == hints.materials.size() && idxs.size() >= sd.min) {
                    submit(encode_card_indices(idxs));
                    answer_directed_ = true;
                    return true;
                }
            }
            if (hints.defer_card_select) return false;
            BEFORE_DEFAULT();
            SelectCardData sd = parse_select_card_data(buf);
            std::vector<uint8_t> idxs;
            for (uint32_t k = 0; k < sd.min; ++k) idxs.push_back(static_cast<uint8_t>(k));
            submit(encode_card_indices(idxs));
            return true;
        }

        // The hinted materials, else a subset reaching the sum (sum_solver).
        case MSG_SELECT_SUM: {
            SelectSumData sd = parse_select_sum_data(buf);
            if (!hints.materials.empty()) {
                std::vector<CardRef> refs;
                for (const auto& e : sd.select) refs.push_back(e.ref);
                std::vector<uint8_t> idxs;
                for (const auto& mz : hints.materials) {
                    int i = find_by_loc_seq(refs, mz.loc, mz.seq);
                    if (i >= 0) idxs.push_back(static_cast<uint8_t>(i));
                }
                if (idxs.size() == hints.materials.size()) {
                    submit(encode_card_indices(idxs));
                    answer_directed_ = true;
                    return true;
                }
            }
            BEFORE_DEFAULT();
            if (responder_.sum_solver == SumSolver::Checked)
                submit(encode_card_indices(solve_select_sum_checked(sd).first));
            else
                submit(encode_card_indices(solve_select_sum(sd).first)); // none found: {}
            return true;
        }

        // [finishable][cancelable][min:u32]...: finish (-1) when nothing more is
        // required, else pick the first candidate. Response: -1, or [1:i32][index:i32].
        case MSG_SELECT_UNSELECT_CARD: {
            BEFORE_DEFAULT();
            uint8_t finishable = buf.size() > 2 ? buf[2] : 0;
            uint8_t cancelable = buf.size() > 3 ? buf[3] : 1;
            uint32_t min_req = 0;
            if (buf.size() >= 8) std::memcpy(&min_req, buf.data() + 4, 4);
            if (min_req == 0 && (finishable || cancelable)) {
                submit(encode_i32(-1));
            } else {
                std::vector<uint8_t> resp(8);
                int32_t v[2] = {1, 0};
                std::memcpy(resp.data(), v, 8);
                submit(resp);
            }
            return true;
        }

        // [count:u8][available:u32]: the lowest `count` attributes.
        case MSG_ANNOUNCE_ATTRIB: {
            BEFORE_DEFAULT();
            int count = (buf.size() > 2) ? (int)buf[2] : 1;
            uint32_t available = 0;
            if (buf.size() >= 7) std::memcpy(&available, buf.data() + 3, 4);
            uint32_t resp = static_cast<uint32_t>(lowest_n_bits(available, count));
            std::vector<uint8_t> out(4);
            std::memcpy(out.data(), &resp, 4);
            submit(out);
            return true;
        }

        // [count:u8][available:u64]: the lowest `count` races (u64 response).
        case MSG_ANNOUNCE_RACE: {
            BEFORE_DEFAULT();
            int count = (buf.size() > 2) ? (int)buf[2] : 1;
            uint64_t available = 0;
            if (buf.size() >= 11) std::memcpy(&available, buf.data() + 3, 8);
            uint64_t resp = lowest_n_bits(available, count);
            std::vector<uint8_t> out(8);
            std::memcpy(out.data(), &resp, 8);
            submit(out);
            return true;
        }

        // The first offered number (the response is an index).
        case MSG_ANNOUNCE_NUMBER:
            BEFORE_DEFAULT();
            submit(encode_i32(0));
            return true;

        // The engine's own order (a first byte of -1).
        case MSG_SORT_CARD:
        case MSG_SORT_CHAIN:
            BEFORE_DEFAULT();
            submit({ 0xFF });
            return true;

        // Counters removed greedily in list order (an int16 per candidate).
        case MSG_SELECT_COUNTER: {
            BEFORE_DEFAULT();
            SelectCounterData scd = parse_select_counter_data(buf);
            auto [resp, found] = solve_select_counter(scd);
            if (!found) resp.assign(scd.candidates.size(), 0);
            std::vector<uint8_t> out(resp.size() * sizeof(int16_t));
            if (!resp.empty()) std::memcpy(out.data(), resp.data(), out.size());
            submit(out);
            return true;
        }

        // A card the filter program accepts (announce.cpp).
        case MSG_ANNOUNCE_CARD:
            BEFORE_DEFAULT();
            submit(encode_i32(static_cast<int32_t>(pick_declarable_code(buf))));
            return true;

        // Rock (a tie repeats the prompt).
        case MSG_ROCK_PAPER_SCISSORS:
            BEFORE_DEFAULT();
            submit(encode_i32(1));
            return true;

        default:
            return false;
    }
}

#undef BEFORE_DEFAULT

AnswerStatus Session::auto_answer(const Hints& hints, std::vector<Answered>* log) {
    answer_directed_ = false;
    answer_asked_ = false;
    answer_replaced_ = false;
    bool held = false;
    if (!answer_pending(hints, held)) return held ? AnswerStatus::Held : AnswerStatus::None;
    // Already submitted; pending_buf_ is still this prompt.
    if (log) log->push_back({ pending_msg_, pending_player_, pending_buf_, answer_directed_, answer_asked_, answer_replaced_ });
    return AnswerStatus::Answered;
}

AnswerStatus Session::advance(int max_iter, const Hints& hints, bool answer_first, std::vector<Answered>* log) {
    AnswerStatus last = AnswerStatus::None;
    if (answer_first) {
        last = auto_answer(hints, log);
        if (last != AnswerStatus::Answered) return last;
    }
    for (int i = 0; i < max_iter; ++i) {
        process();
        last = auto_answer(hints, log);
        if (last != AnswerStatus::Answered) return last;
    }
    return last;
}

} // namespace ocgbridge
