# Porting an engine integration to protocol v1

Many programs embed ygopro-core directly: a duel server, a replay checker, an AI trainer, a puzzle
tool. They all read the engine's messages, answer its prompts and keep some rules of their own on
top. This document lists what such an integration typically needs, and the protocol v1 primitives
that reproduce it exactly, so that it can move onto ocgcore-bridge without changing its behaviour:
the same engine responses, the same default answers, the same snapshots, the same forks.

The rule of thumb: **the engine's side is in the bridge, your rules stay with you.** The bridge
decodes, answers in-effect prompts with plain defaults, and reports what happened. What an action
means in your application, who may take it, which prompts a person or a program should answer,
and what you count, stay in your code, built on the primitives below and on the extension points
([extending.md](extending.md)).

The mapping was checked by re-implementing an existing direct integration on these primitives
only and comparing every output line of the two over that integration's own test suite: there
were no differences.

## Starting a duel

| Need | Protocol v1 |
|---|---|
| two decks, Main Deck in a fixed order | `init.players[i].main` (index 0 is drawn first), `shuffleDecks: false` (the default) |
| decks shuffled from a seed | `shuffleDecks: true`, `seed` |
| shuffles during the duel leave the Deck order alone (replays, tests) | `pseudoShuffle: true` |
| the engine's random numbers from your seed | `seed`: one 64-bit integer (SplitMix64-expanded) or the four words |
| LP, opening hand, draws per turn | `players[i].lp`, `startingHand`, `drawPerTurn` |
| rules presets or raw duel flags | `rules` (`mr1`..`mr5`, `goat`, `speed`, `rush`), `flags` |
| a card's script replaced for one duel (a format's errata) | `scriptOverrides: { "<code>": "<file under the scripts dir>" }` |
| a board set up before the duel, Xyz materials included | `players[i].board` |
| "win this turn" puzzles | `startingHand: 0` for both, `attackFirstTurn: true`, the boards; or an EDOPro puzzle script, `puzzleScript` |
| your own scripts layout | `--scripts`, `--script-dirs` / `init.scriptDirs`, `init.preload` |
| card data | `--cdb` (BabelCDB / EDOPro `.cdb`), or `--cards-json` + `--setcodes` + `--archetypes` (YGOPRODeck-style JSON); `loadCards` at run time |

An `init` error leaves the session with no duel (`no_duel` for the requests that need one). If
your integration treated a failed start in a particular way, do the same on the `init` error.

## Reading the duel

| Need | Protocol v1 |
|---|---|
| the prompt the engine waits on, decoded | `state.pending` (`kind`, its fields; every reply carries `state`) |
| the exact engine message (to compare two prompts byte for byte) | `state.pending.raw` |
| whose prompt it is, the select hint that came with it | `pending.player`, `pending.hint` |
| whose turn the engine is in, the turn count, the phase | `state.turnPlayer`, `state.turn`, `state.phase` |
| the chain being built (where each link was activated) | `state.chain` |
| the chain with each card where it is now | `query chain` |
| each idle activation's spell speed; which chain candidates are optional triggers | `pending.activatableSpeed`, `pending.triggers` |
| both fields with real codes (face-down cards included) and current ATK / DEF | `snapshot` |
| what one player may see, for a prompt (the effect asking, the boards) | `context { player }` |
| one card's current stats | `query fieldCard` |
| what happened since you last looked | `take` (activations, targets, negations, revealed cards, every message with `recordMessages`) |
| a live feed of the engine's messages | `init.events.subscribe` (`event` lines) |
| the end of the duel | `state.ended`, `winner`, `reason` |

## Answering

| Need | Protocol v1 |
|---|---|
| answer a decision point (idle, battle, chain) | `respond { answer }` (typed) |
| answer with response bytes you computed yourself | `respond { answer: { raw: "<hex>" } }` or `{ i32 }` |
| check an answer without sending it | `encode` |
| run the engine once after an answer, nothing auto-answered | `respond { ..., then: "process" }` |
| answer, then let the in-effect prompts be answered | `respond { ..., then: "advance", max, hints }` (the default) |
| answer the pending prompt automatically, without running the engine | `autoAnswer { hints }` |
| run the engine and answer in-effect prompts, up to N times | `advance { max: N, hints }`; `answerFirst: true` answers before the first run |
| send an answer and run nothing yet | `respond { ..., then: "none" }` |

`respond` with `then: "advance"` is one engine run plus up to `max` automatic answers; "answer,
then run, then N follow-ups" is `advance { answerFirst: true, max: N + 1 }` or the same steps one
request at a time. The engine never runs on a stale response: `process` does nothing while a
prompt waits unanswered.

## In-effect prompts

| Need | Protocol v1 |
|---|---|
| the zone, tributes, materials or attack target a move named | `hints.place`, `tributes`, `materials`, `attackTarget` (a zone hint `{ loc, seq }` matches the first candidate there, any controller) |
| leave a card pick for the next request | `hints.deferCardSelect` |
| a person answers some prompts (asynchronously) | `hints.hold: [players]` with `responder.holdKinds`: the reply has `status: "held"`, answer later with `respond` |
| "is the held prompt still the one I showed?" | compare `state.pending.raw` with the one you kept |
| a program answers some prompts (synchronously) | `responder.ask: [kinds of player 0, kinds of player 1]`, the `ask` / `answer` lines; `answer: null` takes the default |
| no ask for this request (e.g. while replaying a recorded move) | `hints.ask: false` |
| nothing answered automatically for this request | `hints.autorespond: false` |
| some kinds never answered automatically | `responder.manual` |
| other default answers | `responder.defaults`, or a C++ `DefaultAnswer` |
| the sum prompts solved as the engine checks both modes | `responder.sum: "checked"` (`"exact"`: exact sums only) |
| what was answered for you, and how | every reply's `answered`: `{ msg, kind, player, raw, directed, asked, replaced }` |

The `answered` lists are enough to count prompts any way you like (per player and kind, choices
with more than one legal answer, repeats of the same `raw`, answers that came from a hint or an
ask). Put the sum prompt in `responder.ask` only together with `responder.sum: "checked"` if your
integration never asked it otherwise.

## Search

| Need | Protocol v1 |
|---|---|
| try a line of play without moving the real duel | `fork`, then the same requests with `session`, then `destroy` |
| a fork of a fork | `fork { session }` (ids `"<parent>/<child>"`) |
| field snapshots at the start of chains, inside a fork | `fork { chainStartSnapshots: true }` |

A fork copies the whole process at that moment (the duel, the scripts' Lua states, the queues, the
loop guards), copy-on-write. Forks never ask: their ask-kind prompts get the defaults. Each request
to a fork is relayed by its parent process, so a line of play costs one round trip per request.

Measured on one machine (Apple M-series, one duel position, 50 forks each):

| | per fork |
|---|---|
| `fork` + one request + `destroy` | 1.29 ms |
| `fork` + six requests + `destroy` | 1.84 ms |

## Exactness notes

- Replies are JSON objects with their keys sorted. Where your integration stores, logs or compares
  objects it builds from them, sort their keys too.
- 64-bit values (effect descriptions, option strings, announce filters) are decimal strings; the
  select `hint` is a number.
- The reasons in `illegal_answer` errors ("missing indices", "too few cards", ...) are stable;
  they come from the same checks the engine makes.
- A chain window's `forced` flag and candidates are in both `pending.raw` (byte 3 and the list)
  and the decoded fields.
- `rps`'s `player` is the message's second byte (the round's step), as the engine writes it.
- A zone hint for a place outside the Monster and Spell & Trap Zones is treated as a Monster Zone
  hint with that index.
- A `disfield` answer from a zone hint is not reported as `directed`.

## What stays with you

- **Your action vocabulary and its validation**: which of your actions maps to which idle, battle
  or chain entry, what state each one needs, which are always allowed, and the messages players
  see. Compare against `state.pending`; check answers with `encode`.
- **Moves made elsewhere**: if your own client or another engine instance decides moves, which
  open chain windows a move implicitly passed and which prompts it already answered.
- **People and programs answering prompts**: who answers what, which chain windows to offer, when
  a hold ends. The bridge offers the mechanisms (holds, the ask hook, manual kinds) and reports.
- **Counting and policies**: what you count as a choice, and how a program decides.
- **Search scripts**: what to try in a fork and how to score it.
