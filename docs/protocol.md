# ocgcore-bridge protocol, version 1

`ocgcore-bridge` runs one duel per process and talks newline-delimited JSON
(NDJSON) on stdin and stdout: one JSON object per line, in both directions.
Logs go to stderr and are not part of the protocol.

```
ocgcore-bridge [--scripts DIR] [--script-dirs SUB,SUB...] [--cdb FILE]...
               [--cards-json FILE [--setcodes FILE] [--archetypes FILE]]
```

- `--scripts DIR`: a [CardScripts](https://github.com/ProjectIgnis/CardScripts) checkout (the
  directory with `constant.lua`, `utility.lua` and `official/`).
- `--script-dirs`: where card scripts are looked up under `DIR`, in order (an empty entry is `DIR`
  itself). Default: the CardScripts layout, `,official,pre-release,pre-errata,goat,rush,skill,unofficial`.
  `init.scriptDirs` overrides it for one duel.
- `--cdb FILE` (repeatable): a [BabelCDB](https://github.com/ProjectIgnis/BabelCDB) / EDOPro card
  database (`cards.cdb`, `release-*.cdb`...).
- `--cards-json FILE`: a YGOPRODeck-style card list (`{ "data": [ { "id", "type", "race", ... } ] }`),
  with `--setcodes` (a setcodes supplement, `data/setcodes.json`) and `--archetypes` (the scripts'
  `archetype_setcode_constants.lua`, default: the one in `--scripts`).

All of these can also be given over the protocol (`loadCards`, `init.scripts`, `init.scriptDirs`).
Nothing else is read from the file system: every path comes from the command line or a request.

Numbers are ygopro-core's own: `LOCATION_*`, `POS_*`, `PHASE_*`, `TIMING_*`, `HINTMSG_*` and
`MSG_*` values from `ocgapi_constants.h`. A card's place is `{ ctrl, loc, seq }` (controller,
one `LOCATION_*` bit, zone or pile index). 64-bit values that do not fit a JavaScript number
(effect descriptions, option strings, announce filters) are decimal strings.

## Messages

### From the server

| Message | When |
|---|---|
| `{ "type": "ready", "protocol": 1, "version": "0.1.1" }` | once, at start (after the command line's card data is loaded) |
| `{ "id": ..., "ok": true, "result": { ... } }` | the reply to a request |
| `{ "id": ..., "ok": false, "error": { "code": "...", "message": "..." } }` | a rejected request |
| `{ "type": "ask", "ask": 1, "prompt": { ... }, "context": { ... } }` | the ask hook (see `init.responder.ask`) |
| `{ "type": "event", "event": { ... }, "session"?: "1" }` | a subscribed engine message (see `init.events`) |

Every request carries an `id` (any JSON value; it is echoed back) and a `type`. Requests are
handled one at a time, in order; the reply to a request comes after any `ask` and `event` it
triggers.

Error codes: `bad_json`, `bad_request`, `unknown_request`, `no_duel` (no `init` yet),
`init` (the engine refused the duel), `illegal_answer`, `cards` (a card source could not be
read), `fork`, `no_session`, `unknown_query`, `internal`.

### To the server

| Request | Does |
|---|---|
| `hello` | versions and capabilities |
| `loadCards` | adds card data |
| `init` | starts a duel |
| `state` | the duel's state and the pending prompt |
| `respond` | answers the pending prompt, then runs the duel on |
| `process` | runs the engine to its next prompt |
| `advance` | runs the engine on, auto-answering in-effect prompts |
| `autoAnswer` | auto-answers the pending prompt (no processing) |
| `encode` | the response bytes an answer gives (checked, not sent) |
| `snapshot` | both players' zones |
| `context` | what a prompt is for, as one player sees the duel |
| `take` | drains the event queues |
| `query` | a named query (built in, or registered by a server of your own) |
| `fork` | copies the duel into a new session |
| `batch` | several requests in one round trip |
| `destroy` | ends the duel (or a fork) |
| `exit` | ends the process |
| `{ "type": "answer", ... }` | answers an `ask` (no reply) |

Any request may carry `"session": "<id>"` to address a fork (see `fork`).

---

### `hello`

```json
{"id":1,"type":"hello"}
{"id":1,"ok":true,"result":{"cards":14759,"cdb":true,"fork":false,"pid":4242,"protocol":1,"queries":["card","chain","fieldCard"],"version":"0.1.1"}}
```

`cdb`: .cdb databases are supported (built with SQLite). `cards`: cards loaded. `fork`: this
process is a fork. `pid`: its process id. `queries`: the names `query` answers.

### `loadCards`

```json
{"id":2,"type":"loadCards","cdb":["/data/BabelCDB/cards.cdb","/data/BabelCDB/release-betb.cdb"]}
{"id":2,"ok":true,"result":{"added":14830,"cards":14830}}

{"id":3,"type":"loadCards","json":"/data/cards.json","setcodes":"/data/setcodes.json","scripts":"/data/CardScripts"}
{"id":3,"ok":true,"result":{"added":13902,"cards":28732}}
```

Sources add up; a card already loaded keeps its first entry. The JSON source reads `id`, `alias`,
`type` (+ `race` for Spells / Traps, `typeline` for monster sub-types), `race`, `attribute`,
`atk`, `def`, `level`, `linkval`, `linkmarkers`, `archetype` and `card_images` (alternate
artworks: each one becomes a card aliased to the original). The supplement adds real setcodes,
aliases and token entries; without setcodes for a card, its `archetype` name is matched against
the scripts' `SET_*` constants.

### `init`

```json
{"id":4,"type":"init","seed":42,"players":[
  {"main":[46986414,89631139,55144522,5318639,12580477],"extra":[1861629]},
  {"main":[83011277,31305911,40640057,26202165,69140098],"extra":[]}]}
{"id":4,"ok":true,"result":{"state":{"chain":[],"duel":"active","ended":false,
  "pending":{"activatable":[{"code":55144522,"ctrl":0,"desc":"57823222300672","loc":2,"pos":0,"seq":2}],
    "activatableSpeed":[1],"canShuffle":true,"hint":0,"kind":"idle","mset":[],"msg":11,"player":0,
    "raw":"0b00…","reposition":[],"spSummon":[],"sset":[…],"summonable":[],"toBattle":false,"toEnd":true},
  "phase":4,"responseReady":false,"turn":1,"turnPlayer":0}}}
```

| Field | Default | Meaning |
|---|---|---|
| `players` | required | two players: `{ main, extra, board, lp, startingHand, drawPerTurn }` |
| `players[].main` | `[]` | Main Deck codes in draw order: index 0 is drawn first |
| `players[].extra` | `[]` | Extra Deck codes |
| `players[].board` | `[]` | cards placed before the duel: `{ code, loc, seq, pos, materials? }` (`materials`: Xyz material codes under a Monster Zone card) |
| `players[].lp` | 8000 | starting LP |
| `players[].startingHand` | 5 | cards drawn at the start |
| `players[].drawPerTurn` | 1 | |
| `seed` | fixed | a 64-bit integer (expanded to the engine's 4 words with SplitMix64), or the 4 words |
| `rules` | `"mr5"` | `"mr1"`..`"mr5"`, `"goat"`, `"speed"`, `"rush"` (the `DUEL_MODE_*` presets) |
| `flags` | | extra duel flags, OR-ed into the rules |
| `pseudoShuffle` | false | `DUEL_PSEUDO_SHUFFLE`: shuffles during the duel keep the Deck's order |
| `shuffleDecks` | false | shuffle each Main Deck first (seeded); otherwise the given order is the Deck's |
| `attackFirstTurn` | false | the first player may attack on turn 1 (puzzles) |
| `scriptOverrides` | `{}` | `{ "<code>": "<file>" }`: another file of the scripts tree for that card (e.g. `"goat/c12345.lua"`), loaded under the card's own name |
| `responder` | | the auto-responder, below |
| `chainStartSnapshots` | true | the first activation of a chain that starts after another resolved carries `field` (a `snapshot`) |
| `recordMessages` | false | every engine message is kept for `take` (`messages`) |
| `scripts` | | the scripts directory (else `--scripts`) |
| `scriptDirs` | `--script-dirs` | where card scripts are looked up under `scripts` (`""` = the directory itself) |
| `preload` | `["constant.lua", "utility.lua"]` | scripts loaded when the duel is created, before any card's |
| `puzzleScript` | | an EDOPro-style puzzle: a Lua file (a path, or one under `scripts`) run after the decks and boards are added and before the duel starts; `players` may then be left out (see "Puzzles") |
| `events` | | `{ subscribe, drop }`: engine messages sent as `event` lines, and messages dropped from everything the caller sees (see "Events") |

`responder`:

| Field | Default | Meaning |
|---|---|---|
| `sum` | `"exact"` | `MSG_SELECT_SUM`: `"exact"` solves exact sums only (a "total N or more" prompt with no exact sum gets an empty answer); `"checked"` solves both modes as the engine checks them |
| `ask` | `[null, null]` | per player: prompt kinds put to the caller with an `ask` before the default (any kind but `idle`, `battle`, `chain`; `"*"` = all of them) |
| `holdKinds` | `card`, `tribute`, `unselectCard`, `effectYesNo`, `yesNo`, `option`, `position`, `place` | prompt kinds a `hold` hint can hold (`"*"` = every in-effect kind) |
| `holdTrivial` | false | also hold prompts with one possible answer (one option, one position) |
| `effectYesNoRepeatLimit` | 8 | the very same `effectYesNo` prompt answered yes this many times in one turn is answered no (a script loop guard) |
| `manual` | `[]` | prompt kinds never answered automatically (left pending, as a decision point is; `"*"` = every in-effect kind) |
| `defaults` | `{}` | replacement defaults: `{ "<kind>": <answer> }`, a fixed answer (below) used when it is legal for the prompt, else the built-in default. Decision points too: `{ "chain": { "index": -1 } }` passes every chain window it can |

Sending `ask` kinds that the default would answer anyway costs one round trip per prompt; a caller
that only wants to see them can use `events` or the `answered` lists instead.

A new `init` destroys the session's previous duel. Card data stays loaded.

### `state`

```json
{"id":5,"type":"state"}
{"id":5,"ok":true,"result":{"state":{"duel":"active","ended":false,"pending":{…},"responseReady":false,
  "turn":3,"turnPlayer":0,"phase":4,"chain":[]}}}
```

| Field | Meaning |
|---|---|
| `duel` | `"active"` or `"none"` (no duel: the other fields are absent) |
| `ended`, `winner`, `reason` | the duel is over (`MSG_WIN`: winner 0 / 1 / 2 = draw, reason) |
| `pending` | the last prompt the engine sent (a Prompt, below); it stays set once answered, until the next one |
| `responseReady` | an answer is waiting for the engine to read it |
| `turn`, `turnPlayer`, `phase` | from `MSG_NEW_TURN` / `MSG_NEW_PHASE` (turns counted from 1, `turnPlayer` -1 and `phase` 0 before the first) |
| `chain` | the chain being built, link 1 first: `{ code, ctrl, loc, seq }` where each link was activated |

### Prompts

Every prompt has `msg` (`MSG_*`), `kind`, `player`, `hint` (the `HINT_SELECTMSG` it came with,
0 = none) and `raw` (the engine message, hex). The kinds and their fields:

| kind | msg | Fields |
|---|---|---|
| `idle` | 11 | `summonable`, `spSummon`, `reposition`, `mset`, `sset` (CardRefs), `activatable` (CardRefs with `desc`), `activatableSpeed` (spell speed per activatable), `toBattle`, `toEnd`, `canShuffle` |
| `battle` | 10 | `attackable`, `activatable` (with `desc`), `toMain2`, `toEnd` |
| `chain` | 16 | `activatable` (with `desc`), `forced`, `speCount`, `responding` (`speCount` > 0), `triggers` (per activatable: an optional trigger effect), `timing`, `timingOpponent` |
| `card` | 15 | `candidates`, `min`, `max`, `cancelable` |
| `tribute` | 20 | `candidates` (with `release`: how many tributes each counts as), `min` (compared with the sum of `release`), `max`, `cancelable` |
| `unselectCard` | 26 | `candidates` (with `selected`), `min`, `max`, `cancelable`, `finishable` |
| `effectYesNo` | 12 | `code`, `card`, `description` |
| `yesNo` | 13 | `description` |
| `option` | 14 | `options` (descriptions) |
| `position` | 19 | `code`, `positions` (`POS_*` bits on offer) |
| `place` / `disfield` | 18 / 24 | `count` zones to pick, `flag` (bit set = unavailable: own Monster Zones bits 0-6, Spell & Trap Zones 8-15; the opponent's +16) |
| `sum` | 23 | `candidates` / `must` (with `sum` and `sumHigh`: the two values a card may count as), `acc`, `min`, `max`, `greater` |
| `counter` | 22 | `counterType`, `count`, `candidates` (with `counters`) |
| `sortCard` / `sortChain` | 25 / 21 | `candidates` |
| `announceRace` / `announceAttribute` | 140 / 141 | `count`, `available` (bits) |
| `announceCard` | 142 | `opcodes` (the filter program) |
| `announceNumber` | 143 | `options` |
| `rps` | 132 | (none; `player` is the round's step byte) |

A CardRef is `{ code, ctrl, loc, seq, pos }`.

### Answers

`respond.answer` and `encode.answer`, checked against the pending prompt:

| kind | Answer |
|---|---|
| any | `{ "raw": "<hex>" }` or `{ "i32": n }`: response bytes as they are (not checked) |
| `idle` | `{ "action": "summon" \| "spsummon" \| "repos" \| "mset" \| "sset" \| "activate", "index": n }`, `{ "action": "battle" \| "end" \| "shuffle" }` |
| `battle` | `{ "action": "activate" \| "attack", "index": n }`, `{ "action": "main2" \| "end" }` |
| `chain` | `{ "index": n }`, `-1` passes (not when `forced`) |
| `card`, `tribute` | `{ "indices": [..] }` (`[]` cancels a cancelable one) |
| `sum` | `{ "indices": [..] }` into `candidates` |
| `unselectCard` | `{ "index": n }`, `-1` finishes / cancels |
| `effectYesNo`, `yesNo` | `{ "yes": true }` (or 0 / 1) |
| `option` | `{ "option": n }` |
| `position` | `{ "position": 1 \| 2 \| 4 \| 8 }` |
| `place`, `disfield` | `{ "player", "loc", "seq" }`, or `{ "places": [ ... ] }` for `count` > 1 |
| `counter` | `{ "counts": [..] }` per candidate, adding up to `count` |
| `sortCard`, `sortChain` | `{ "order": [..] }`, or `{ "order": null }` for the engine's order |
| `announceRace`, `announceAttribute` | `{ "value": bits }` (exactly `count` of `available`) |
| `announceCard` | `{ "code": n }` |
| `announceNumber` | `{ "index": n }` |
| `rps` | `{ "hand": 1 \| 2 \| 3 }` |

### `respond`

```json
{"id":6,"type":"respond","answer":{"action":"summon","index":0}}
{"id":6,"ok":true,"result":{"answered":[{"asked":false,"directed":false,"kind":"place","msg":18,
  "player":0,"raw":"120001e0e0e0e0"}],"state":{…},"status":"none"}}
```

Answers the pending prompt, then `then`: `"advance"` (default: like `advance` with `max`
(20) and `hints`), `"process"` (run to the next prompt, nothing auto-answered) or `"none"`.
An illegal answer is rejected (`illegal_answer`) and nothing is sent to the engine.

### `process`

```json
{"id":7,"type":"process"}
{"id":7,"ok":true,"result":{"state":{…}}}
```

Runs the engine until it waits on a prompt or the duel ends. It does nothing while a prompt waits
without a fresh answer: the engine is never run on a stale response.

### `advance` and `autoAnswer`

```json
{"id":8,"type":"advance","max":20,"answerFirst":false,"hints":{"place":{"loc":4,"seq":2}}}
{"id":8,"ok":true,"result":{"answered":[…],"state":{…},"status":"none"}}

{"id":9,"type":"autoAnswer","hints":{"hold":[0]}}
{"id":9,"ok":true,"result":{"answered":[],"state":{…},"status":"held"}}
```

`advance` does, up to `max` times: `process`, then auto-answer the pending prompt; it stops at the
first prompt not answered (a decision point, a held prompt, a deferred card pick). With
`answerFirst` it auto-answers before the first `process` (and stops if that fails).
`autoAnswer` auto-answers the pending prompt once, without processing.

`status`: `"answered"` (the last auto-answer succeeded: the engine has a response to read),
`"held"` (the pending prompt is held for the caller), `"none"`. `answered` lists every prompt the
auto-responder or the ask hook answered, in order: `{ msg, kind, player, raw, directed, asked, replaced }`
(`directed`: from a hint or an ask; `asked`: by the ask hook; `replaced`: by a replacement default).

#### The auto-responder

Decision points (`idle`, `battle`, `chain`) have no default: they are answered automatically
only with a replacement default (`responder.defaults`). The rest, unless `manual`:

| kind | Default answer |
|---|---|
| `place` | the hinted zone if free (one zone), else the first free: own Monster Zones, own Spell & Trap Zones, the opponent's Monster Zones, then Spell & Trap Zones |
| `disfield` | as `place` (no ask, no hold) |
| `position` | the lowest `POS_*` bit on offer |
| `tribute` | the hinted tributes, else the first candidates until their `release` reaches `min` |
| `effectYesNo` | yes (see `effectYesNoRepeatLimit`) |
| `yesNo` | no |
| `option` | the first |
| `card` | the hinted attack target or materials, else (unless `deferCardSelect`) the first `min` |
| `sum` | the hinted materials, else a solution (`responder.sum`; none found: `[]`) |
| `unselectCard` | finish when `min` is 0 and it may finish, else the first candidate |
| `announceAttribute`, `announceRace` | the lowest `count` bits |
| `announceNumber` | the first |
| `announceCard` | a code the filter accepts: from the opponent's cards first, then the player's own, then any known card |
| `sortCard`, `sortChain` | the engine's order |
| `counter` | counters taken in list order |
| `rps` | rock |

Before the built-in default, in this order: the hints, the ask hook (`ask` kinds, when
`hints.ask`), a hold (`holdKinds` of the `hints.hold` players), a replacement default
(`responder.defaults`, then one registered in C++). `place`, `disfield`, `tribute`, `card` and
`sum` go on to the ask hook only when no hint applied. [extending.md](extending.md) has examples.

#### Hints

| Field | Meaning |
|---|---|
| `place` | `{ loc, seq }`: the zone for a one-zone `place` (`loc` 8 = Spell & Trap Zone, else Monster Zone); also tried first for each zone of a `place` / `disfield` |
| `tributes` | `[{ loc, seq }]`: the `tribute` candidates at these places |
| `materials` | `[{ loc, seq }]`: the `card` / `sum` candidates at these places, used only when every one is a candidate (for `card`: and at least `min`) |
| `attackTarget` | `{ loc, seq }`: the `card` candidate there |
| `deferCardSelect` | leave a `card` prompt unanswered (after `attackTarget` / `materials`) |
| `ask` | (default true) the ask hook may be asked |
| `hold` | `[players]`: their prompts of the hold kinds are held |
| `autorespond` | (default true) false: nothing is answered automatically |

A place hint matches the first candidate at that `loc` / `seq`, whoever controls it.

### The ask hook

With `init.responder.ask`, a prompt of an ask kind is sent to the caller while the server waits:

```json
{"type":"ask","ask":3,"prompt":{"kind":"place","msg":18,"player":0,"count":1,"flag":4294967264,"hint":0,"raw":"…"},
 "context":{"chain":[],"chainLength":0,"hint":0,"players":[…],"turnPlayer":0}}
{"type":"answer","ask":3,"answer":{"player":0,"loc":4,"seq":4}}
```

`answer: null` (or an illegal answer) takes the default: the hook can inspect a prompt without
answering it. Requests sent while an ask waits are handled after the current request. Forks never
ask.

### `encode`

```json
{"id":10,"type":"encode","answer":{"action":"end"}}
{"id":10,"ok":true,"result":{"raw":"07000000"}}
```

### `snapshot`

```json
{"id":11,"type":"snapshot"}
{"id":11,"ok":true,"result":{"players":[
  {"banished":[],"deckCount":35,"extra":[{"code":1861629,"position":8}],"grave":[],
   "hand":[{"code":46986414,"position":10},…],"lp":8000,
   "mzone":[{"atk":2500,"code":71594310,"def":1900,"overlay":[69140098,69140098],"position":1},null,…],
   "szone":[null,…]},
  {…}]}}
```

Per player: `mzone` (7 slots), `szone` (8: 0-4, Field Zone 5, Pendulum Zones 6-7 as the engine
lists them), `hand`, `grave`, `banished`, `extra`, `deckCount`, `lp`. A slot is `null` or
`{ code, position, overlay?, atk?, def?, destroyed? }`: `atk` / `def` with every effect applied
(Monster Zones), `overlay` the Xyz materials, `destroyed` on a GY / Extra Deck card whose last
move was a destruction. Real codes for every card, face-down ones included.

### `context`

```json
{"id":12,"type":"context","player":1}
{"id":12,"ok":true,"result":{"chain":[],"chainLength":0,"effect":{"category":0,"code":26202165,"ctrl":0},
  "hint":506,"players":[{"deckCount":30,"grave":[26202165],"handCount":5,"lp":8000,"mzone":[…],"szone":[…]},
  {"deckCount":33,"grave":[],"hand":[…],"handCount":5,"lp":8000,"mzone":[…],"szone":[…]}],"turnPlayer":0}}
```

What a prompt is for, as `player` (default: the pending prompt's player) sees the duel: `hint`,
the effect asking (`effect`: its card, controller and `CATEGORY_*` bits), the chain with each
link's card where it is now, `turnPlayer`, and both boards: zone slots `{ code, pos, atk?, def?,
level?, type? }` (an opponent's face-down card: code 0, no stats), GY codes, hand and Deck
counts, and the viewer's own hand codes.

### `take`

```json
{"id":13,"type":"take","queues":["activations","targets"]}
{"id":13,"ok":true,"result":{"activations":[{"code":26202165,"ctrl":0,"loc":16,"phase":4,"seq":0,"turn":1}],
  "targets":[{"by":{"code":12580477,"ctrl":0,"loc":8,"seq":2},"player":0,"targets":[{"code":89631139,"ctrl":1,"loc":4,"seq":0}]}]}}
```

Drains the queues named (default: all). Each queue holds what came since it was last taken:

| Queue | From | Entry |
|---|---|---|
| `activations` | `MSG_CHAINING` | `{ code, ctrl, loc, seq, turn, phase, field? }` |
| `targets` | `MSG_BECOME_TARGET` | `{ by, player, targets }`: the activation they belong to, who targets, the cards where they are |
| `negations` | `MSG_CHAIN_NEGATED` / `MSG_CHAIN_DISABLED` | `{ link, kind: "activation" \| "effect", code, ctrl, loc, seq }` |
| `confirmed` | `MSG_CONFIRM_CARDS` | `{ code, ctrl, loc, seq, to }` |
| `messages` | every message (`init.recordMessages`) | `{ msg, raw }` |

### `query`

```json
{"id":14,"type":"query","name":"fieldCard","args":{"ctrl":0,"loc":4,"seq":2}}
{"id":14,"ok":true,"result":{"value":{"alias":0,"atk":2600,"attribute":1,"code":71594310,"ctrl":0,"def":1900,
  "level":0,"link":0,"loc":4,"overlay":[69140098,69140098],"owner":0,"pos":1,"race":"256","rank":4,"seq":2,"type":8388609}}}
{"id":15,"type":"query","name":"card","args":{"code":89631139}}
{"id":15,"ok":true,"result":{"value":{"alias":0,"atk":3000,"attribute":16,"code":89631139,"def":2500,"level":8,
  "linkMarker":0,"lscale":0,"race":"8192","rscale":0,"setcodes":[221],"type":17}}}
```

A named query: `args` in, `value` out. Built in:

| Name | Args | Value |
|---|---|---|
| `card` | `{ code }` | the card's data as loaded (`type`, `atk`, `def`, `level`, `race`, `attribute`, scales, `linkMarker`, `setcodes`, `alias`), or `null` |
| `fieldCard` | `{ ctrl, loc, seq }` | the card at that place now: `code`, `alias`, place, `pos`, `owner`, `type`, and in a Monster Zone its current `atk`, `def`, `level`, `rank`, `link`, `attribute`, `race`, `overlay`; `null` when empty |
| `chain` | | the chain's cards where they are now (`{ code, ctrl, loc, seq }` per link) |

`card` works before `init`. A server built on the server library adds its own
([extending.md](extending.md)); `hello.queries` lists them. Errors: `unknown_query`, `bad_request`.

### Events

```json
{"id":3,"type":"init","seed":1,"players":[…],"events":{"subscribe":["move","chaining"],"drop":["hint"]}}
{"id":3,"ok":true,"result":{…}}
{"id":4,"type":"respond","answer":{"action":"activate","index":0}}
{"type":"event","event":{"code":55144522,"from":{"ctrl":0,"loc":2,"pos":10,"seq":3},"msg":50,"name":"move",
  "raw":"324a7049030002030000000a0000000008000000000500000000040002","reason":33555456,"to":{"ctrl":0,"loc":8,"pos":5,"seq":0}}}
{"type":"event","event":{"card":{"ctrl":0,"loc":8,"pos":5,"seq":0},"code":55144522,"desc":"57823222300672","link":1,
  "msg":70,"name":"chaining","raw":"464a7049…","trigger":{"ctrl":0,"loc":8,"seq":0}}}
{"id":4,"ok":true,"result":{…}}
```

`events.subscribe`: messages, by name or `MSG_*` number (`"*"` = all), sent as `event` lines as
the engine produces them, before the reply of the request that ran the engine; a fork's carry its
`session`. `events.drop`: messages kept from everything the caller sees (events, `take` queues,
`recordMessages`); the duel's own tracking (prompt, turn, phase, chain, the end) still sees them.

An event is `{ msg, name, raw, ...fields }`. Names are the `MSG_*` names in lowerCamelCase
(`move`, `newTurn`, `chainSolving`, `selectIdlecmd`...). A place is `{ ctrl, loc, seq, pos }`.
Fields:

| name | Fields |
|---|---|
| `newTurn` | `player` |
| `newPhase` | `phase` |
| `win` | `player`, `reason` |
| `hint` | `hintType`, `player`, `data` (u64 string) |
| `draw` | `player`, `cards` (`{ code, pos }`) |
| `damage`, `recover`, `payLpcost` | `player`, `amount` |
| `lpupdate` | `player`, `lp` |
| `move` | `code`, `from`, `to`, `reason` |
| `summoning`, `spsummoning`, `flipsummoning`, `set` | `code`, `card` |
| `posChange` | `code`, `ctrl`, `loc`, `seq`, `from`, `to` (positions) |
| `chaining` | `code`, `card`, `trigger` (`{ ctrl, loc, seq }`), `desc` (u64 string), `link` |
| `chained`, `chainSolving`, `chainSolved`, `chainNegated`, `chainDisabled` | `link` |
| `attack` | `attacker`, `target` (`null`: a direct attack) |
| `shuffleDeck`, `shuffleHand`, `shuffleExtra` | `player` |
| `tossCoin`, `tossDice` | `player`, `results` |
| `confirmCards` | `player`, `cards` (`{ code, ctrl, loc, seq }`) |
| `becomeTarget` | `cards` (places) |

Other messages carry `msg`, `name` and `raw` only.

### Puzzles

Two formats:

- **A board in `init`**: `players[].board` places cards before the duel (Xyz materials included);
  `startingHand: 0` and `attackFirstTurn: true` make it a "win this turn" puzzle.
  [`examples/puzzles/lethal.json`](../examples/puzzles/lethal.json) is one.
- **An EDOPro-style puzzle script**: `puzzleScript` runs a Lua file that sets the duel up with
  ygopro-core's `Debug` functions (`Debug.ReloadFieldBegin(flags, rule)`, `Debug.SetPlayerInfo`,
  `Debug.AddCard`, `Debug.ReloadFieldEnd`), as EDOPro's puzzles do; the decks and boards given in
  `init` are added first, and `ReloadFieldBegin` clears them.
  [`examples/puzzles/lethal.lua`](../examples/puzzles/lethal.lua) is the same puzzle.

### `fork`

```json
{"id":14,"type":"fork"}
{"id":14,"ok":true,"result":{"session":"1"}}
{"id":15,"type":"respond","session":"1","answer":{"action":"summon","index":0}}
{"id":15,"ok":true,"result":{…}}
{"id":16,"type":"fork","session":"1"}
{"id":16,"ok":true,"result":{"session":"1/1"}}
{"id":17,"type":"destroy","session":"1/1"}
{"id":17,"ok":true,"result":{}}
```

A fork is a copy of the process at that moment (the duel, the scripts' Lua states, the queues,
the loop guards), copy-on-write, driven with the same requests plus `session`. The duel it was
forked from does not move. Options: `chainStartSnapshots` (default false). Forks never ask (their
ask-kind prompts get the defaults); they keep the duel's `events` settings, and their events carry
their `session`. A fork of a fork is `"<parent>/<child>"`. `destroy` with a
`session` ends that fork's process; so does the parent's exit.

A request for a fork goes through the process it was forked from (and, for a fork of a fork,
through each one above it), which passes the fork's reply line on unchanged.

`detach: true` makes the copy a session of the process that received the request instead:

```json
{"id":18,"type":"fork","session":"1","detach":true}
{"id":18,"ok":true,"result":{"detached":true,"session":"2"}}
```

The copy is made from session `1` (here, after its moves) but is addressed as `"2"`: its requests
go to it directly, not through `1`, and it lives on when `1` (or any fork above it) is destroyed.
This suits a search that keeps a fork per position and forks each next position from it: chains
of such forks don't get longer, and a position no longer needed is destroyed on its own. Detached
forks are reaped by the system; they end with the bridge process like any fork. (The fork's socket
is passed to the bridge process with the reply, so detached forks need a Unix-like system, like
forks.) Without a `session`, `detach` makes an ordinary fork. (Between bridge processes, the
receiver of a passed socket confirms it with a `{"type":"received"}` line, which has no reply.)

### `batch`

```json
{"id":20,"type":"batch","session":"1","requests":[
  {"type":"respond","answer":{"index":-1},"then":"process"},
  {"type":"autoAnswer"},
  {"type":"advance","if":"answered","max":20}]}
{"id":20,"ok":true,"result":{"results":[
  {"ok":true,"result":{"status":"none","answered":[]}},
  {"ok":true,"result":{"status":"answered","answered":[…]}},
  {"ok":true,"result":{"status":"none","answered":[…]}}],"state":{…}}}
```

Runs `requests` in order in one session (the batch's `session`, if any), exactly as if they
were sent one by one, and replies once. Each request is a request object without `id` and
`session`; `fork`, `batch` and `exit` cannot be batched. Each entry of `results` is
`{ ok: true, result }` (the result without its `state`), `{ ok: true, skipped: true }`, or
`{ ok: false, error: { code, message } }`. The first request that fails ends the batch: the
requests after it do not run and have no entry. `state` is the duel's state after the batch.

`if` (a status, or a list of them) runs a request only when the one before it ran and replied
with that `status` (`advance`, `autoAnswer` and `respond` have one); otherwise it is skipped, and
a request whose `if` refers to a skipped one is skipped too. This saves round trips for short
fixed sequences, such as "answer this prompt, process, auto-answer, and advance only if
something was answered", which matters most for forks, whose requests pass through their parent
processes.

### `destroy` and `exit`

```json
{"id":18,"type":"destroy"}
{"id":18,"ok":true,"result":{}}
{"id":19,"type":"exit"}
{"id":19,"ok":true,"result":{}}
```

`destroy` ends the duel (card data stays loaded; `init` starts another). `exit` (or closing stdin)
ends the process and its forks.

## Versioning

The protocol number changes only for changes a client must know about. New requests, new fields
in replies and new optional request fields keep version 1.
