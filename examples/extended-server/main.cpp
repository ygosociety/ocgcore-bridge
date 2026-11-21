// SPDX-License-Identifier: AGPL-3.0-or-later
// A server of your own: the same protocol v1 as ocgcore-bridge, plus
//  - a query, "damageTaken": the damage each player took this duel, counted by
//  - a message filter on MSG_DAMAGE (it also drops MSG_HINT from what the
//    caller sees, to show a drop);
//  - a replacement default: position prompts take face-up Defense Position when
//    it is offered (the built-in default is the lowest POS_* bit: face-up Attack).
//
//   ocgcore-bridge-extended --scripts DIR --cdb cards.cdb
//   {"id":1,"type":"query","name":"damageTaken"}
#include <array>

#include "ocgapi_constants.h"
#include "ocgbridge/answers.h"
#include "server.h"

using namespace ocgbridge;

int main(int argc, char** argv) {
    static std::array<uint64_t, 2> damage{0, 0};

    server::Extensions ext;
    ext.setup = [](Session& s) {
        s.add_message_filter([](Session&, const EngineMessage& m) {
            if (m.type == MSG_HINT) return FilterResult::Drop;
            if (m.type == MSG_DAMAGE) {
                const json ev = m.decoded(); // { msg, name, player, amount }
                if (ev.contains("player")) damage[ev["player"].get<int>() & 1] += ev["amount"].get<uint64_t>();
            }
            return FilterResult::Pass;
        });
        s.register_query("damageTaken", [](Session&, const json&) { return json{damage[0], damage[1]}; });
        s.set_default_answer(MSG_SELECT_POSITION, [](Session& s, const json& prompt, std::vector<uint8_t>& out) {
            if (!(prompt.value("positions", 0) & POS_FACEUP_DEFENSE)) return false; // the built-in default
            std::string reason;
            return encode_answer(s.pending_buf(), {{"position", POS_FACEUP_DEFENSE}}, out, reason);
        });
    };
    ext.on_init = [](Session&, const json&) { damage = {0, 0}; };
    return server::run(argc, argv, ext);
}
