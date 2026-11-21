# Extending ocgcore-bridge

The bridge answers the engine's prompts with plain defaults and reports what happens, and it
leaves every rule of your own application to you. You add that logic at four extension points
instead of forking the bridge:

| Extension point | Over the protocol (any language) | In C++ (`libocgbridge`, `ocgbridge_server`) |
|---|---|---|
| [Prompt hooks](#1-prompt-hooks): answer or inspect any prompt | `init.responder.ask` + `ask` / `answer` | `Session::set_ask_hook` |
| [The auto-responder](#2-the-auto-responder): which prompts are answered automatically, and how | `init.responder.manual`, `init.responder.defaults`, `holdKinds` | `ResponderConfig`, `Session::set_default_answer` |
| [Message filters](#3-message-filters): subscribe to or drop engine messages | `init.events.subscribe`, `init.events.drop` | `Session::add_message_filter` |
| [Custom queries](#4-custom-queries): named questions about the duel | `query` | `Session::register_query` |

The protocol side works with the stock `ocgcore-bridge` binary. The C++ side is for a server of
your own: link `ocgbridge_server`, register your extensions, and call `server::run()`; you get the
whole protocol plus what you registered ([5](#5-a-server-of-your-own)).

## How one prompt is answered

When the engine waits on a prompt inside an action (where to place a card, which cards to pick,
yes or no...), `advance`, `autoAnswer` and `respond` (with `then: "advance"`) answer it, trying in
this order:

1. **manual** kinds are never answered: the prompt is left pending (`status: "none"`), as a
   decision point is;
2. the caller's **hints** for this action (`place`, `tributes`, `materials`, `attackTarget`);
3. the **ask hook**, for the kinds in `responder.ask` of that player;
4. a **hold**, for the kinds in `holdKinds` of the players in `hints.hold` (`status: "held"`);
5. a **replacement default**: a fixed answer from `responder.defaults`, then a C++
   `DefaultAnswer`;
6. the **built-in default** ([protocol.md](protocol.md#the-auto-responder)).

Decision points (`idle`, `battle`, `chain`) have no built-in default and are not asked or held:
they come back to the caller, unless a replacement default answers them. Every answer given this
way is listed in the reply's `answered`, with `directed`, `asked` and `replaced` flags.

## 1. Prompt hooks

The ask hook puts a prompt to you while the engine waits, with the prompt decoded and a view of the
duel (`context`). Return an answer, or nothing for the default. The hook sees every prompt kind
but the decision points; `"*"` selects all of them.

TypeScript:

```ts
import { Bridge } from 'ocgcore-bridge';

const bridge = await Bridge.start({
  scripts: 'CardScripts', cdb: ['cards.cdb'],
  onAsk: ({ prompt, context }) => {
    console.log('player', prompt.player, 'is asked', prompt.kind, 'by', context.effect?.code);
    if (prompt.kind === 'yesNo') return { yes: true };   // answer this one
    return null;                                         // inspect only: the next step answers it
  },
});
await bridge.init({ players: [deckA, deckB], responder: { ask: [['*'], null] } }); // player 0's prompts
```

Over the protocol the same is an `ask` line and your `answer` line
([protocol.md](protocol.md#the-ask-hook)). In C++:

```cpp
session.responder().ask_msgs[0] = in_effect_prompt_msgs();
session.set_ask_hook([](Session& s, const json& prompt, std::vector<uint8_t>& out) {
    if (prompt["kind"] != "yesNo") return false;   // the next step answers it
    std::string why;
    return encode_answer(s.pending_buf(), {{"yes", true}}, out, why);
});
```

A hold is the asynchronous form: the request returns with `status: "held"` and the prompt in
`state.pending`, and you answer it later with `respond` (for example after a human chose).

## 2. The auto-responder

`responder.manual` takes kinds away from the auto-responder; `responder.defaults` replaces their
default answer with a fixed one. A fixed answer is used only when it is legal for the prompt at
hand; otherwise the built-in default applies. A default for a decision point lets the engine run
through it, which turns `advance` into "play on until something else is needed":

```json
{"id":1,"type":"init","players":[…],"responder":{
  "manual":["card","sum"],
  "defaults":{"position":{"position":4},"yesNo":{"yes":true},"chain":{"index":-1}}}}
```

Here card and sum picks always come back to the caller, monsters are placed in face-up Defense
Position when it is offered, yes / no questions get yes, and every chain window is passed.

A default that depends on the prompt is a function, registered in C++ (here: the last offered
option instead of the first):

```cpp
session.set_default_answer(MSG_SELECT_OPTION, [](Session& s, const json& prompt, std::vector<uint8_t>& out) {
    const auto n = prompt["options"].size();
    if (n == 0) return false;                                  // the built-in default
    std::string why;
    return encode_answer(s.pending_buf(), {{"option", n - 1}}, out, why);
});
```

`ResponderConfig` also holds `hold_msgs`, `hold_trivial`, the sum solver and the yes / no loop
guard (`effectyn_repeat_limit`); the protocol names are in
[protocol.md](protocol.md#init).

## 3. Message filters

Every message the engine sends passes through the filters, in order, before the session handles
it. A filter can look at it (decoded: `{ msg, name, ...fields }`, see
[protocol.md](protocol.md#events)) and pass it on or drop it. A dropped message is kept from
everything the caller sees: the event queues (`take`), the message record and the events sent
out; the session itself still tracks it (the pending prompt, turn, phase, chain, the end of the
duel).

Over the protocol, subscribed messages arrive as `event` lines while the request that caused them
runs, before its reply:

```ts
const bridge = await Bridge.start({
  scripts: 'CardScripts', cdb: ['cards.cdb'],
  onEvent: (e, session) => { if (e.name === 'damage') console.log(`player ${e.player} took ${e.amount}`); },
});
await bridge.init({ players: [deckA, deckB], events: { subscribe: ['damage', 'move', 'chaining'], drop: ['hint'] } });
```

In C++ (a filter may read the session, not drive it):

```cpp
std::array<uint64_t, 2> damage{};
session.add_message_filter([&](Session&, const EngineMessage& m) {
    if (m.type == MSG_HINT) return FilterResult::Drop;
    if (m.type == MSG_DAMAGE) { json e = m.decoded(); damage[e["player"].get<int>() & 1] += e["amount"].get<uint64_t>(); }
    return FilterResult::Pass;
});
```

## 4. Custom queries

A query is a named function from JSON arguments to a JSON value, answered by the `query` request.
The built-in ones are `card` (a card's data), `fieldCard` (the card at a place, with its current
stats) and `chain` (the chain's cards where they are now); `hello` lists all of them.

```ts
const pearl = await bridge.query('fieldCard', { ctrl: 0, loc: 4, seq: 2 }); // { code, atk, overlay, ... }
```

Register your own in C++; it can read anything the session or the engine knows:

```cpp
session.register_query("handSize", [](Session& s, const json& args) -> json {
    const int player = args.value("player", 0);
    return s.snapshot()[player]["hand"].size();
});
// {"id":7,"type":"query","name":"handSize","args":{"player":1}}  ->  {"id":7,"ok":true,"result":{"value":5}}
```

Throw `std::invalid_argument` for bad arguments (the reply is a `bad_request` error); an unknown
name is an `unknown_query` error.

## 5. A server of your own

[`examples/extended-server/main.cpp`](../examples/extended-server/main.cpp) is a complete server: the
stock protocol plus a message filter, a query and a replacement default, in about 40 lines.

```cpp
#include "server.h"
using namespace ocgbridge;

int main(int argc, char** argv) {
    server::Extensions ext;
    ext.setup = [](Session& s) {                 // once, before the first request
        s.register_query("handSize", /* ... */);
        s.add_message_filter(/* ... */);
        s.set_default_answer(MSG_SELECT_POSITION, /* ... */);
    };
    ext.on_init = [](Session& s, const json& init) { /* per duel; init has your own fields too */ };
    return server::run(argc, argv, ext);
}
```

```cmake
add_subdirectory(ocgcore-bridge)                 # or FetchContent
add_executable(my-duel-server main.cpp)
target_link_libraries(my-duel-server PRIVATE ocgbridge_server)
```

What `setup` registers stays for every duel of the process and is copied into its forks. Clients
(the TypeScript one included) talk to it exactly as to `ocgcore-bridge`; pass its path as
`binary`.

## Keeping it in your own process instead

Everything above also works without the protocol: `libocgbridge` alone is a C++ library (`Session`,
`encode_answer`, `decode_message`). Link `ocgbridge`, create a `CardDatabase` and a `Session`, and
drive the duel with `start`, `submit`, `process` and `advance`.
