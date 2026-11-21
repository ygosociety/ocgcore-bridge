// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace fs = std::filesystem;

namespace ocgbridge {

std::optional<uint64_t> rules_flags(const std::string& name) {
    if (name == "mr1") return static_cast<uint64_t>(DUEL_MODE_MR1);
    if (name == "mr2") return static_cast<uint64_t>(DUEL_MODE_MR2);
    if (name == "mr3") return static_cast<uint64_t>(DUEL_MODE_MR3);
    if (name == "mr4") return static_cast<uint64_t>(DUEL_MODE_MR4);
    if (name == "mr5") return static_cast<uint64_t>(DUEL_MODE_MR5);
    if (name == "goat") return static_cast<uint64_t>(DUEL_MODE_GOAT);
    if (name == "speed") return static_cast<uint64_t>(DUEL_MODE_SPEED);
    if (name == "rush") return static_cast<uint64_t>(DUEL_MODE_RUSH);
    return std::nullopt;
}

std::array<uint64_t, 4> expand_seed(uint64_t seed) {
    std::array<uint64_t, 4> out{};
    uint64_t x = seed;
    for (auto& w : out) {
        uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        w = z ^ (z >> 31);
    }
    return out;
}

ResponderConfig::ResponderConfig() {
    hold_msgs = { MSG_SELECT_CARD, MSG_SELECT_TRIBUTE, MSG_SELECT_UNSELECT_CARD, MSG_SELECT_EFFECTYN,
                  MSG_SELECT_YESNO, MSG_SELECT_OPTION, MSG_SELECT_POSITION, MSG_SELECT_PLACE };
}

// ── ocgcore callbacks ────────────────────────────────────────────────────────

namespace {

void card_reader_cb(void* payload, uint32_t code, OCG_CardData* out) {
    static_cast<const Session*>(payload)->read_card(code, out);
}

void card_reader_done_cb(void*, OCG_CardData*) {}

int script_reader_cb(void* payload, OCG_Duel duel, const char* name) {
    return static_cast<Session*>(payload)->read_script(duel, name);
}

void log_handler_cb(void*, const char* msg, int) {
    std::fprintf(stderr, "[ocgcore] %s\n", msg);
}

} // namespace

const std::vector<std::string>& default_script_dirs() {
    static const std::vector<std::string> DIRS = {
        "", "official", "pre-release", "pre-errata", "goat", "rush", "skill", "unofficial"
    };
    return DIRS;
}

const std::vector<std::string>& default_preload_scripts() {
    static const std::vector<std::string> FILES = { "constant.lua", "utility.lua" };
    return FILES;
}

void Session::read_card(uint32_t code, void* out_ptr) const {
    auto* out = static_cast<OCG_CardData*>(out_ptr);
    std::memset(out, 0, sizeof(*out));
    out->code = code;
    const CardEntry* e = cards_ ? cards_->find(code) : nullptr;
    if (!e) return;
    out->alias = e->alias;
    // A null-terminated uint16_t array, read by the engine within this call.
    static thread_local uint16_t setcode_buf[5];
    if (!e->setcodes.empty()) {
        size_t n = 0;
        for (uint16_t s : e->setcodes) if (n < 4) setcode_buf[n++] = s;
        setcode_buf[n] = 0;
        out->setcodes = setcode_buf;
    } else {
        out->setcodes = nullptr;
    }
    out->type        = e->type;
    out->level       = e->pack_scales ? (((e->lscale & 0xFF) << 24) | ((e->rscale & 0xFF) << 16) | (e->level & 0xFFFF)) : e->level;
    out->attribute   = e->attribute;
    out->race        = e->race;
    out->attack      = e->attack;
    out->defense     = e->defense;
    out->lscale      = e->lscale;
    out->rscale      = e->rscale;
    out->link_marker = e->link_marker;
}

bool Session::load_script_file(void* duel, const std::string& file, const char* name) {
    const fs::path base(scripts_path_);
    for (auto& candidate : { file, file + ".lua" }) {
        for (const auto& sub : script_dirs_) {
            fs::path p = sub.empty() ? base / candidate : base / sub / candidate;
            std::ifstream f(p, std::ios::binary);
            if (!f.is_open()) continue;
            std::string src((std::istreambuf_iterator<char>(f)), {});
            OCG_LoadScript(static_cast<OCG_Duel>(duel), src.c_str(), static_cast<uint32_t>(src.size()), name);
            return true;
        }
    }
    return false;
}

int Session::read_script(void* duel, const char* name) {
    if (scripts_path_.empty()) return 0;
    if (!script_overrides_.empty()) {
        unsigned code = 0;
        if (std::sscanf(name, "c%u", &code) == 1) {
            auto it = script_overrides_.find(code);
            if (it != script_overrides_.end() && load_script_file(duel, it->second, name)) return 1;
        }
    }
    if (load_script_file(duel, name, name)) return 1;
    // An alternate artwork has no script of its own: its original card's is
    // loaded under the requested name, so the effects bind to this code.
    unsigned code = 0;
    if (cards_ && std::sscanf(name, "c%u", &code) == 1) {
        const CardEntry* e = cards_->find(code);
        if (e && e->alias && e->alias != code) {
            if (load_script_file(duel, "c" + std::to_string(e->alias) + ".lua", name)) return 1;
        }
        // The other way round: a card listed under a reprint code whose script
        // is under one of its artwork codes.
        if (const auto* arts = cards_->art_codes(code)) {
            for (uint32_t art : *arts)
                if (load_script_file(duel, "c" + std::to_string(art) + ".lua", name)) return 1;
        }
    }
    return 0;
}

void Session::preload_script(const char* name) {
    if (scripts_path_.empty() || !duel_) return;
    fs::path p = fs::path(scripts_path_) / name;
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return;
    std::string src((std::istreambuf_iterator<char>(f)), {});
    OCG_LoadScript(static_cast<OCG_Duel>(duel_), src.c_str(), static_cast<uint32_t>(src.size()), name);
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

Session::Session(const CardDatabase* cards, std::string scripts_path)
    : cards_(cards), scripts_path_(std::move(scripts_path)),
      script_dirs_(default_script_dirs()), preload_scripts_(default_preload_scripts()) {
    register_builtin_queries();
}

Session::~Session() { destroy(); }

void Session::reset_state() {
    pending_msg_ = 0;
    response_ready_ = false;
    pending_player_ = -1;
    pending_buf_.clear();
    activations_ = json::array();
    targets_ = json::array();
    negations_ = json::array();
    confirmed_ = json::array();
    messages_ = json::array();
    last_chaining_ = nullptr;
    turn_count_ = 0;
    phase_ = 0;
    chain_ended_ = false;
    game_over_ = false;
    win_player_ = -1;
    win_reason_ = -1;
    script_overrides_.clear();
    effectyn_yeses_.clear();
    select_hint_[0] = select_hint_[1] = 0;
    prompt_hint_ = 0;
    engine_turn_player_ = -1;
    deck_codes_[0].clear();
    deck_codes_[1].clear();
}

void Session::destroy() {
    if (duel_) { OCG_DestroyDuel(static_cast<OCG_Duel>(duel_)); duel_ = nullptr; }
    reset_state();
}

void Session::start(const DuelSetup& setup) {
    destroy();

    // DUEL_PSEUDO_SHUFFLE: shuffles during the duel keep the Deck order.
    constexpr uint64_t PSEUDO_SHUFFLE = 0x10;
    const uint64_t rules = setup.flags_set ? setup.flags : static_cast<uint64_t>(DUEL_MODE_MR5);
    script_overrides_ = setup.script_overrides;

    OCG_DuelOptions opts{};
    for (int i = 0; i < 4; ++i) opts.seed[i] = setup.seed[i];
    opts.flags = (setup.pseudo_shuffle ? PSEUDO_SHUFFLE : 0) | rules |
                 (setup.attack_first_turn ? static_cast<uint64_t>(DUEL_ATTACK_FIRST_TURN) : 0);
    opts.team1 = { setup.players[0].lp, setup.players[0].starting_hand, setup.players[0].draw_per_turn };
    opts.team2 = { setup.players[1].lp, setup.players[1].starting_hand, setup.players[1].draw_per_turn };
    opts.cardReader = card_reader_cb;
    opts.payload1 = this;
    opts.scriptReader = script_reader_cb;
    opts.payload2 = this;
    opts.logHandler = log_handler_cb;
    opts.payload3 = nullptr;
    opts.cardReaderDone = card_reader_done_cb;
    opts.payload4 = nullptr;
    opts.enableUnsafeLibraries = 0;

    OCG_Duel duel = nullptr;
    int status = OCG_CreateDuel(&duel, &opts);
    if (status != OCG_DUEL_CREATION_SUCCESS) {
        script_overrides_.clear();
        throw std::runtime_error("OCG_CreateDuel failed, status=" + std::to_string(status));
    }
    duel_ = duel;

    for (const auto& file : preload_scripts_) preload_script(file.c_str());

    // Deck and Extra Deck cards enter face-down, as EDOPro's clients add them
    // (position 0 is neither face-up nor face-down, and scripts asking
    // IsFacedown() of an Extra Deck card would never pass).
    for (uint8_t team = 0; team < 2; ++team) {
        PlayerSetup p = setup.players[team];
        if (setup.shuffle_decks && p.main.size() > 1) {
            // SplitMix64 over the seed's words, one stream per player.
            uint64_t x = setup.seed[team] ^ setup.seed[2 + team];
            auto next = [&x]() {
                uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
                return z ^ (z >> 31);
            };
            for (size_t i = p.main.size() - 1; i > 0; --i) std::swap(p.main[i], p.main[next() % (i + 1)]);
        }
        for (uint32_t code : p.main) deck_codes_[team].push_back(code);
        for (uint32_t code : p.extra) deck_codes_[team].push_back(code);
        int unknown = 0;
        // The engine draws from the back of the Main Deck list: added in
        // reverse, index 0 is drawn first.
        for (int i = static_cast<int>(p.main.size()) - 1; i >= 0; --i) {
            OCG_NewCardInfo ci{};
            ci.team = team; ci.duelist = 0;
            ci.code = p.main[static_cast<size_t>(i)];
            ci.con = team; ci.loc = LOCATION_DECK; ci.seq = 0; ci.pos = POS_FACEDOWN_DEFENSE;
            if (!cards_ || !cards_->find(ci.code)) ++unknown;
            OCG_DuelNewCard(duel, &ci);
        }
        if (unknown > 0)
            std::fprintf(stderr, "[ocgcore-bridge] player%d: %d Main Deck cards have no card data\n", (int)team, unknown);
        for (uint32_t code : p.extra) {
            OCG_NewCardInfo ci{};
            ci.team = team; ci.duelist = 0;
            ci.code = code;
            ci.con = team; ci.loc = LOCATION_EXTRA; ci.seq = 0; ci.pos = POS_FACEDOWN_DEFENSE;
            OCG_DuelNewCard(duel, &ci);
        }
    }
    add_board(setup.players[0], 0);
    add_board(setup.players[1], 1);
    if (!setup.puzzle_script.empty()) load_puzzle(setup.puzzle_script);

    OCG_StartDuel(duel);
    process();
}

// Cards placed on the board before the duel starts. Xyz materials have no
// OCG_* API: this is what the engine's own Debug.PreAddCard does for
// LOCATION_OVERLAY (a new card owned by the side, attached with xyz_add).
void Session::add_board(const PlayerSetup& p, uint8_t team) {
    auto* pduel = static_cast<duel*>(duel_);
    for (const auto& c : p.board) {
        OCG_NewCardInfo ci{};
        ci.team = team; ci.duelist = 0;
        ci.code = c.code;
        ci.con = team;
        ci.loc = c.loc;
        ci.seq = c.seq;
        ci.pos = c.pos;
        if (!ci.code || !ci.loc) continue;
        deck_codes_[team].push_back(ci.code);
        OCG_DuelNewCard(static_cast<OCG_Duel>(duel_), &ci);
        if (ci.loc != LOCATION_MZONE || c.materials.empty()) continue;
        card* xyz = pduel->game_field->get_field_card(team, LOCATION_MZONE, ci.seq);
        if (!xyz) {
            std::fprintf(stderr, "[ocgcore-bridge] player%d: no card in Monster Zone %u for materials\n", (int)team, ci.seq);
            continue;
        }
        for (uint32_t code : c.materials) {
            if (!code) continue;
            card* mat = pduel->new_card(code);
            mat->owner = team;
            xyz->xyz_add(mat);
            deck_codes_[team].push_back(code);
        }
    }
}

// A puzzle script: the file as given, else under the scripts directory. The
// cards it places replace deck_codes_ (the announce default's preferences).
void Session::load_puzzle(const std::string& file) {
    fs::path p(file);
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open() && !scripts_path_.empty()) { p = fs::path(scripts_path_) / file; f.open(p, std::ios::binary); }
    if (!f.is_open()) {
        destroy();
        throw std::runtime_error("puzzle script not found: " + file);
    }
    std::string src((std::istreambuf_iterator<char>(f)), {});
    const std::string name = p.filename().string();
    if (!OCG_LoadScript(static_cast<OCG_Duel>(duel_), src.c_str(), static_cast<uint32_t>(src.size()), name.c_str())) {
        destroy();
        throw std::runtime_error("puzzle script failed: " + file);
    }
    auto& field = *static_cast<duel*>(duel_)->game_field;
    for (uint8_t team = 0; team < 2; ++team) {
        deck_codes_[team].clear();
        const auto& pl = field.player[team];
        for (const card_vector* list : { &pl.list_main, &pl.list_extra, &pl.list_hand, &pl.list_mzone, &pl.list_szone,
                                         &pl.list_grave, &pl.list_remove })
            for (const card* c : *list) {
                if (!c) continue;
                deck_codes_[team].push_back(c->data.code);
                for (const card* m : c->xyz_materials) if (m) deck_codes_[team].push_back(m->data.code);
            }
    }
}

// ── Processing ───────────────────────────────────────────────────────────────

void Session::submit(const std::vector<uint8_t>& response) {
    if (!duel_) return;
    OCG_DuelSetResponse(static_cast<OCG_Duel>(duel_), response.data(), static_cast<uint32_t>(response.size()));
    response_ready_ = true;
}

void Session::process() {
    if (!duel_) return;
    // A prompt nobody answered: running the engine would make it re-read the
    // previous (stale) response for this prompt.
    if (pending_msg_ != 0 && !response_ready_) return;
    response_ready_ = false;
    for (;;) {
        int status = OCG_DuelProcess(static_cast<OCG_Duel>(duel_));
        uint32_t len = 0;
        const uint8_t* buf = static_cast<const uint8_t*>(OCG_DuelGetMessage(static_cast<OCG_Duel>(duel_), &len));
        if (buf && len >= 5) parse_messages(buf, len);
        if (status == OCG_DUEL_STATUS_END || status == OCG_DUEL_STATUS_AWAITING) break;
    }
}

uint32_t Session::card_code_at(uint8_t ctrl, uint8_t loc, uint32_t seq) const {
    if (!duel_ || ctrl > 1) return 0;
    auto& pl = static_cast<duel*>(duel_)->game_field->player[ctrl];
    const card_vector* list = nullptr;
    switch (loc) {
        case LOCATION_MZONE: list = &pl.list_mzone; break;
        case LOCATION_SZONE: list = &pl.list_szone; break;
        case LOCATION_GRAVE: list = &pl.list_grave; break;
        case LOCATION_HAND: list = &pl.list_hand; break;
        case LOCATION_REMOVED: list = &pl.list_remove; break;
        case LOCATION_EXTRA: list = &pl.list_extra; break;
        case LOCATION_DECK: list = &pl.list_main; break;
        default: return 0;
    }
    if (seq >= list->size() || !(*list)[seq]) return 0;
    return (*list)[seq]->data.code;
}

void Session::parse_messages(const uint8_t* buf, uint32_t len) {
    const uint8_t* p   = buf;
    const uint8_t* end = buf + len;
    while (p + 4 <= end) {
        uint32_t msg_size;
        std::memcpy(&msg_size, p, 4);
        p += 4;
        if (p + msg_size > end) break;
        // Filters run first; a dropped message is kept from the caller's
        // queues, but the tracking below still sees it.
        const bool visible = msg_size < 1 || filters_.empty() || !filter_drops(p, msg_size);
        if (msg_size >= 1 && record_messages_ && visible)
            messages_.push_back({{"msg", p[0]}, {"raw", to_hex(p, msg_size)}});
        if (msg_size >= 2) {
            uint8_t t = p[0];
            if (is_prompt_message(t)) {
                // The hint belongs to the prompt right after it. A select /
                // unselect loop hints once, so its later rounds keep the first
                // round's hint.
                const int pl = p[1] & 1;
                if (select_hint_[pl] || t != MSG_SELECT_UNSELECT_CARD || pending_msg_ != t) prompt_hint_ = select_hint_[pl];
                select_hint_[pl] = 0;
                pending_msg_    = t;
                pending_player_ = static_cast<int>(p[1]);
                pending_buf_.assign(p, p + msg_size);
            } else if (t == MSG_HINT && msg_size >= 11 && p[1] == HINT_SELECTMSG) {
                // [type][hint type u8][player u8][desc u64]
                std::memcpy(&select_hint_[p[2] & 1], p + 3, 8);
            } else if (t == MSG_NEW_TURN) {
                engine_turn_player_ = static_cast<int>(p[1]);
                effectyn_yeses_.clear();
                turn_count_++;
                phase_ = 0;
            } else if (t == MSG_NEW_PHASE && msg_size >= 3) {
                uint16_t phase;
                std::memcpy(&phase, p + 1, 2);
                phase_ = phase;
            } else if (t == MSG_CHAINING && msg_size >= 21) {
                // [type][code u32][card loc_info: ctrl u8, loc u8, seq u32, pos u32]
                // [triggering ctrl u8][triggering loc u8][triggering seq u32]...
                uint32_t code, seq;
                std::memcpy(&code, p + 1, 4);
                std::memcpy(&seq, p + 17, 4);
                json entry = {{"code", code}, {"ctrl", p[15]}, {"loc", p[16]}, {"seq", seq},
                              {"turn", turn_count_}, {"phase", phase_}};
                if (visible && chain_ended_ && chain_start_snapshots_) entry["field"] = snapshot();
                chain_ended_ = false;
                last_chaining_ = {{"code", code}, {"ctrl", p[15]}, {"loc", p[16]}, {"seq", seq}};
                if (visible) activations_.push_back(std::move(entry));
            } else if ((t == MSG_CHAIN_NEGATED || t == MSG_CHAIN_DISABLED) && duel_) {
                // [type][chain count u8]: the link, from 1.
                const uint8_t link = p[1];
                auto& chain = static_cast<duel*>(duel_)->game_field->core.current_chain;
                json entry = {{"link", link}, {"kind", t == MSG_CHAIN_NEGATED ? "activation" : "effect"}};
                if (link >= 1 && link <= chain.size() && chain[link - 1].triggering_effect) {
                    const card* c = chain[link - 1].triggering_effect->get_handler();
                    entry["code"] = c->data.code;
                    entry["ctrl"] = c->current.controler;
                    entry["loc"] = c->current.location;
                    entry["seq"] = c->current.sequence;
                    if (visible) negations_.push_back(std::move(entry));
                }
            } else if (t == MSG_BECOME_TARGET && msg_size >= 5) {
                // [type][count u32] then per card loc_info [ctrl u8][loc u8][seq u32][pos u32]
                uint32_t count;
                std::memcpy(&count, p + 1, 4);
                json list = json::array();
                for (uint32_t i = 0; i < count && 5 + (i + 1) * 10 <= msg_size; i++) {
                    const uint8_t* c = p + 5 + i * 10;
                    uint32_t tseq;
                    std::memcpy(&tseq, c + 2, 4);
                    list.push_back({{"code", card_code_at(c[0], c[1], tseq)}, {"ctrl", c[0]}, {"loc", c[1]}, {"seq", tseq}});
                }
                // A link's targets always follow its own MSG_CHAINING.
                if (visible && !list.empty() && !last_chaining_.is_null())
                    targets_.push_back({{"by", last_chaining_}, {"player", last_chaining_["ctrl"]}, {"targets", std::move(list)}});
            } else if (t == MSG_CHAIN_END) {
                chain_ended_ = true;
            } else if (t == MSG_CONFIRM_CARDS && msg_size >= 6) {
                // [type][player u8][count u32] then per card [code u32][ctrl u8][loc u8][seq u32]
                uint32_t count;
                std::memcpy(&count, p + 2, 4);
                for (uint32_t i = 0; i < count && 6 + (i + 1) * 10 <= msg_size; i++) {
                    const uint8_t* c = p + 6 + i * 10;
                    uint32_t code, seq;
                    std::memcpy(&code, c, 4);
                    std::memcpy(&seq, c + 6, 4);
                    if (visible) confirmed_.push_back({{"code", code}, {"ctrl", c[4]}, {"loc", c[5]}, {"seq", seq}, {"to", p[1]}});
                }
            } else if (t == MSG_WIN && msg_size >= 3) {
                game_over_  = true;
                win_player_ = static_cast<int>(p[1]);
                win_reason_ = static_cast<int>(p[2]);
            }
        }
        p += msg_size;
    }
}

bool Session::filter_drops(const uint8_t* msg, uint32_t size) {
    const EngineMessage m{ msg[0], msg, size };
    // A copy: a filter may add or remove filters.
    const auto filters = filters_;
    for (const auto& [id, f] : filters)
        if (f && f(*this, m) == FilterResult::Drop) return true;
    return false;
}

int Session::add_message_filter(MessageFilter filter) {
    const int id = next_filter_id_++;
    filters_.emplace_back(id, std::move(filter));
    return id;
}

void Session::remove_message_filter(int id) {
    for (auto it = filters_.begin(); it != filters_.end(); ++it)
        if (it->first == id) { filters_.erase(it); return; }
}

static json drain(json& queue) {
    json out = std::move(queue);
    queue = json::array();
    return out;
}

json Session::take_activations() { return drain(activations_); }
json Session::take_targets() { return drain(targets_); }
json Session::take_negations() { return drain(negations_); }
json Session::take_confirmed() { return drain(confirmed_); }
json Session::take_messages() { return drain(messages_); }

} // namespace ocgbridge
