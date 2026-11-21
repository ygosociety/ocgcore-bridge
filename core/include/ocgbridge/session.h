// SPDX-License-Identifier: AGPL-3.0-or-later
// libocgbridge: one duel over edo9300/ygopro-core.
//
//   Session s(&cards, "/path/to/CardScripts");
//   s.start(setup);              // decks or a puzzle board, seed, LP, rules
//   while (!s.game_over()) {
//       nlohmann::json p = s.prompt_json();   // what the engine waits on
//       s.submit(encode_answer(...));         // answer it (answers.h)
//       s.advance(20, hints);                 // run on, auto-answering follow-ups
//   }
//
// The engine runs only when it has a fresh answer: process() is a no-op while
// a prompt waits unanswered, so a stale response buffer is never re-read for a
// different prompt.
//
// Extension points (docs/extending.md): the ask hook (answer or inspect any
// prompt), replacement default answers and manual prompt kinds (the
// auto-responder), message filters (observe or drop decoded engine messages)
// and named queries. All of them are kept across start() / destroy().
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ocgbridge/cards.h"
#include "ocgbridge/events.h"
#include "ocgbridge/messages.h"

namespace ocgbridge {

using json = nlohmann::json;

// A card placed on the board before the duel starts (a puzzle), in ocgcore
// terms: LOCATION_* (one bit), the zone index, POS_* bits, and for a Monster
// Zone card its Xyz materials (codes, bottom first).
struct BoardCard {
    uint32_t code{0};
    uint32_t loc{0};
    uint32_t seq{0};
    uint32_t pos{0};
    std::vector<uint32_t> materials;
};

struct PlayerSetup {
    // Main Deck in draw order: index 0 is the top card (drawn first).
    std::vector<uint32_t> main;
    std::vector<uint32_t> extra;
    std::vector<BoardCard> board;
    uint32_t lp{8000};
    uint32_t starting_hand{5};
    uint32_t draw_per_turn{1};
};

struct DuelSetup {
    std::array<uint64_t, 4> seed{0xDEADBEEFULL, 0xCAFEBABEULL, 0x12345678ULL, 0x9ABCDEF0ULL};
    // Duel flags (DUEL_MODE_* and friends); rules_flags() names the presets.
    uint64_t flags{0};
    bool flags_set{false};
    // DUEL_PSEUDO_SHUFFLE: shuffles during the duel leave the Deck's order as
    // it is (the engine never shuffles the Deck when the duel starts).
    bool pseudo_shuffle{false};
    // Shuffle each Main Deck before the duel (Fisher-Yates driven by the seed).
    bool shuffle_decks{false};
    bool attack_first_turn{false};
    PlayerSetup players[2];
    // A card's script replaced by another file of the scripts tree, loaded
    // under the card's own name ("c<code>.lua").
    std::map<uint32_t, std::string> script_overrides;
    // An EDOPro-style puzzle: a Lua file run after the decks and boards above
    // are added and before the duel starts (Debug.ReloadFieldBegin,
    // Debug.SetPlayerInfo, Debug.AddCard, Debug.ReloadFieldEnd...). A path,
    // or a file under the scripts directory.
    std::string puzzle_script;
};

// Where card scripts are looked up, relative to the scripts directory: "" is
// the directory itself. The default is the CardScripts layout (the root, then
// official/, pre-release/, pre-errata/, goat/, rush/, skill/, unofficial/).
const std::vector<std::string>& default_script_dirs();
// Scripts loaded right after a duel is created, before any card's script.
const std::vector<std::string>& default_preload_scripts();

// "mr1".."mr5", "goat", "speed", "rush" -> DUEL_MODE_* flags. nullopt for an unknown name.
std::optional<uint64_t> rules_flags(const std::string& name);
// A 64-bit seed expanded to the engine's four words with SplitMix64.
std::array<uint64_t, 4> expand_seed(uint64_t seed);

// ── Auto-responder ──────────────────────────────────────────────────────────
// Answers the prompts inside an action (where to place, which position, which
// cards...) with fixed defaults, unless the caller directs, asks for, or holds
// them. Decision points (idle, battle, chain windows) are never auto-answered.
enum class SumSolver {
    Exact,   // exact sums only (a "total N or more" prompt with no exact sum gets an empty answer)
    Checked, // both modes, as the engine checks them
};

struct ResponderConfig {
    SumSolver sum_solver{SumSolver::Exact};
    // Per player: prompt messages the ask hook is asked for (any prompt but
    // the decision points).
    std::set<uint8_t> ask_msgs[2];
    // Prompt messages that can be held for the caller.
    std::set<uint8_t> hold_msgs;
    // Hold prompts with a single possible answer too (one option, one position).
    bool hold_trivial{false};
    // The same MSG_SELECT_EFFECTYN (byte for byte) answered yes this many times
    // in one turn is answered no from then on (a script loop guard).
    int effectyn_repeat_limit{8};
    // Prompt messages never answered automatically (left to the caller, as a
    // decision point is).
    std::set<uint8_t> manual_msgs;
    // Replacement defaults as fixed answers (answers.h), per prompt message:
    // used when the answer is legal for the prompt, else the next default.
    std::map<uint8_t, json> default_answers;
    ResponderConfig();
};

// Per call: what the caller already knows about the prompts it may meet.
struct Hints {
    // SELECT_PLACE / SELECT_DISFIELD: the zone to take when it is free.
    std::optional<ZoneHint> place;
    // SELECT_TRIBUTE: the candidates at these (loc, seq) are the tributes.
    std::vector<ZoneHint> tributes;
    // SELECT_CARD / SELECT_SUM: these (loc, seq) are the cards to pick; used only
    // when every one of them is a candidate.
    std::vector<ZoneHint> materials;
    // SELECT_CARD: the candidate at this (loc, seq) (an attack target).
    std::optional<ZoneHint> attack_target;
    // SELECT_CARD: leave it unanswered (after the hints above).
    bool defer_card_select{false};
    // The ask hook may be asked.
    bool ask{true};
    // Bit p set: player p's prompts of the hold kinds are held (left unanswered
    // and reported) instead of auto-answered.
    uint8_t hold_players{0};
    // false: nothing is answered automatically (processing still happens).
    bool autorespond{true};
};

// One prompt the auto-responder (or the ask hook) answered.
struct Answered {
    uint8_t msg{0};
    int player{-1};
    std::vector<uint8_t> raw;
    bool directed{false}; // from a hint or the ask hook
    bool asked{false};    // by the ask hook
    bool replaced{false}; // by a replacement default
};

enum class AnswerStatus { None, Answered, Held };

class Session;
// Asked for a prompt of an ask kind; returns true with the engine response, or
// false for the default answer. `prompt` is prompt_json().
using AskHook = std::function<bool(Session&, const json& prompt, std::vector<uint8_t>& response)>;
// A replacement default answer for one prompt message: true with the engine
// response, or false for the next default.
using DefaultAnswer = std::function<bool(Session&, const json& prompt, std::vector<uint8_t>& response)>;

// A message filter sees every engine message, in order, before the session
// handles it. Drop keeps the message from what the caller sees (the event
// queues, the message record, later filters); the session's own tracking
// (the pending prompt, turn, phase, chain, the end of the duel) still sees it.
// A filter may read the session but must not submit, process or start.
enum class FilterResult { Pass, Drop };
using MessageFilter = std::function<FilterResult(Session&, const EngineMessage&)>;

// A named query: arguments in, a JSON value out. Throw std::invalid_argument
// for bad arguments.
using QueryHandler = std::function<json(Session&, const json& args)>;

class Session {
public:
    Session(const CardDatabase* cards, std::string scripts_path);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Creates and starts the duel (any previous one is destroyed first), then
    // runs it to its first prompt. Throws std::runtime_error.
    void start(const DuelSetup& setup);
    void destroy();
    bool active() const { return duel_ != nullptr; }

    ResponderConfig& responder() { return responder_; }
    void set_ask_hook(AskHook hook) { ask_hook_ = std::move(hook); }
    bool has_ask_hook() const { return static_cast<bool>(ask_hook_); }
    // The first activation of a chain that starts after another one resolved
    // carries the field as that chain begins (activations' "field").
    void set_chain_start_snapshots(bool on) { chain_start_snapshots_ = on; }
    // Every engine message is kept for take_messages().
    void set_record_messages(bool on) { record_messages_ = on; }

    // ── Extension points ──
    // A replacement default for prompt message `msg` (nullptr removes it).
    void set_default_answer(uint8_t msg, DefaultAnswer fn);
    void clear_default_answers() { default_fns_.clear(); }
    // Returns an id for remove_message_filter. Filters run in the order added.
    int add_message_filter(MessageFilter filter);
    void remove_message_filter(int id);
    // Replaces a query of the same name. Built in: "card", "fieldCard", "chain".
    void register_query(const std::string& name, QueryHandler handler);
    void unregister_query(const std::string& name) { queries_.erase(name); }
    bool has_query(const std::string& name) const { return queries_.count(name) > 0; }
    std::vector<std::string> query_names() const;
    // Throws std::out_of_range for an unknown name, std::invalid_argument for
    // bad arguments (from the handler).
    json query(const std::string& name, const json& args);

    // Script lookup (default_script_dirs(), default_preload_scripts()).
    void set_script_dirs(std::vector<std::string> dirs) { script_dirs_ = std::move(dirs); }
    const std::vector<std::string>& script_dirs() const { return script_dirs_; }
    void set_preload_scripts(std::vector<std::string> files) { preload_scripts_ = std::move(files); }
    const std::vector<std::string>& preload_scripts() const { return preload_scripts_; }

    // ── Stepping ──
    // Runs the engine until it waits on a prompt or the duel ends; a no-op
    // while a prompt waits without a fresh answer.
    void process();
    // The answer to the pending prompt (the engine reads it on the next process()).
    void submit(const std::vector<uint8_t>& response);
    bool response_ready() const { return response_ready_; }
    // Answers the pending prompt with the auto-responder (no processing).
    AnswerStatus auto_answer(const Hints& hints, std::vector<Answered>* log = nullptr);
    // Up to `max_iter` times: process(), then auto-answer; stops at the first
    // prompt not answered. With `answer_first`, auto-answers before the first
    // process() (and stops when that fails). Returns the last answer's status.
    AnswerStatus advance(int max_iter, const Hints& hints, bool answer_first = false, std::vector<Answered>* log = nullptr);

    // ── State ──
    // The last prompt message the engine sent (0 before the first): it stays
    // set after it is answered, until the next one.
    uint8_t pending_msg() const { return pending_msg_; }
    int pending_player() const { return pending_player_; }
    const std::vector<uint8_t>& pending_buf() const { return pending_buf_; }
    // The HINT_SELECTMSG the pending prompt came with (0 = none).
    uint64_t prompt_hint() const { return prompt_hint_; }
    bool game_over() const { return game_over_; }
    int winner() const { return win_player_; }
    int win_reason() const { return win_reason_; }
    // From MSG_NEW_TURN / MSG_NEW_PHASE: whose turn (-1 before the first),
    // turns started (from 1), the phase (PHASE_*, 0 before the first).
    int turn_player() const { return engine_turn_player_; }
    int turn_count() const { return turn_count_; }
    int phase() const { return phase_; }
    int chain_length() const;
    // The chain being built, link 1 first: [{code, ctrl, loc, seq}] where each
    // link's card was when it was activated.
    json chain_links() const;
    // The chain's links with each card where it is now.
    json chain_cards_now() const;
    // While waiting at MSG_SELECT_IDLECMD: each activation's spell speed (the
    // message's order). At MSG_SELECT_CHAIN: per candidate, whether it is an
    // optional trigger effect.
    json select_chain_speeds() const;
    json select_chain_trigger_flags() const;
    bool select_chain_has_optional_trigger() const;

    // ── Decoding ──
    // The pending prompt: { msg, kind, player, hint, raw, ...kind fields }; null when none.
    json prompt_json() const;
    // Both players' zones: real codes, positions, overlay units, ATK/DEF of
    // monsters, "destroyed" on GY / Extra Deck cards destroyed to get there.
    json snapshot() const;
    // What a prompt is for, as `viewer` may see the duel: the select hint, the
    // effect asking, the chain, whose turn it is, both boards (the viewer's own
    // hand codes only; the opponent's face-down cards hidden). viewer < 0: the
    // pending prompt's player.
    json context(int viewer = -1) const;
    json state_json() const;

    // ── Event queues (drained on read) ──
    json take_activations();  // MSG_CHAINING: {code, ctrl, loc, seq, turn, phase[, field]}
    json take_targets();      // MSG_BECOME_TARGET: {by, player, targets}
    json take_negations();    // MSG_CHAIN_NEGATED / DISABLED: {link, kind, code, ctrl, loc, seq}
    json take_confirmed();    // MSG_CONFIRM_CARDS: {code, ctrl, loc, seq, to}
    json take_messages();     // every engine message: {msg, raw} (set_record_messages)

    const CardDatabase* cards() const { return cards_; }
    const std::string& scripts_path() const { return scripts_path_; }
    void set_scripts_path(std::string path) { scripts_path_ = std::move(path); }

    // ocgcore callbacks (public for the C trampolines).
    void read_card(uint32_t code, void* out) const;
    int read_script(void* duel, const char* name);

private:
    enum class Pre { Answered, Held, Continue };
    bool answer_pending(const Hints& hints, bool& held);
    bool ask(std::vector<uint8_t>& resp);
    bool answer_by_ask(const Hints& hints);
    bool answer_by_replacement();
    Pre before_default(const Hints& hints);
    bool hold(const Hints& hints);
    bool filter_drops(const uint8_t* msg, uint32_t size);
    void register_builtin_queries();
    bool has_single_answer() const;
    void parse_messages(const uint8_t* buf, uint32_t len);
    uint32_t card_code_at(uint8_t ctrl, uint8_t loc, uint32_t seq) const;
    uint32_t pick_declarable_code(const std::vector<uint8_t>& buf) const;
    void add_board(const PlayerSetup& p, uint8_t team);
    void load_puzzle(const std::string& file);
    bool load_script_file(void* duel, const std::string& file, const char* name);
    void preload_script(const char* name);
    void reset_state();

    const CardDatabase* cards_;
    std::string scripts_path_;
    void* duel_{nullptr};
    ResponderConfig responder_;
    AskHook ask_hook_;
    bool chain_start_snapshots_{true};
    bool record_messages_{false};
    std::map<uint8_t, DefaultAnswer> default_fns_;
    std::vector<std::pair<int, MessageFilter>> filters_;
    int next_filter_id_{1};
    std::map<std::string, QueryHandler> queries_;
    std::vector<std::string> script_dirs_;
    std::vector<std::string> preload_scripts_;

    uint8_t pending_msg_{0};
    int pending_player_{-1};
    std::vector<uint8_t> pending_buf_;
    bool response_ready_{false};
    bool game_over_{false};
    int win_player_{-1};
    int win_reason_{-1};
    int engine_turn_player_{-1};
    int turn_count_{0};
    int phase_{0};
    uint64_t select_hint_[2]{0, 0};
    uint64_t prompt_hint_{0};
    std::vector<uint32_t> deck_codes_[2];
    std::map<uint32_t, std::string> script_overrides_;
    std::map<std::vector<uint8_t>, int> effectyn_yeses_;
    bool chain_ended_{false};
    json activations_ = json::array();
    json targets_ = json::array();
    json negations_ = json::array();
    json confirmed_ = json::array();
    json messages_ = json::array();
    json last_chaining_ = nullptr;
    // Set by answer_pending for the Answered log.
    bool answer_directed_{false};
    bool answer_asked_{false};
    bool answer_replaced_{false};
};

// prompt kind name <-> message type ("card" <-> MSG_SELECT_CARD, ...); 0 / "other" when unknown.
uint8_t prompt_msg_of_kind(const std::string& kind);
const char* prompt_kind(uint8_t msg);
// Every prompt message the auto-responder can answer with a built-in default
// (all prompt kinds but idle, battle and chain).
const std::set<uint8_t>& in_effect_prompt_msgs();

} // namespace ocgbridge
