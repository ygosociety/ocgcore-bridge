# Changelog

All notable changes. Versions follow [semver](https://semver.org); 0.x until protocol v1 is settled.

## 0.1.1

### Added
- `batch`: several requests in one round trip, run in order as if sent one by one, with `if`
  conditions on the previous request's status.
- `fork` with `detach: true`: a copy of a fork that the main process talks to directly (its socket
  is passed up with the reply). It outlives the fork it was made from, and its requests skip the
  forks in between.
- `hello` reports the process id.

### Changed
- A fork's reply lines pass through its parent unparsed.
- A destroyed fork is reaped without waiting for it.

## 0.1.0

The first release.

### Added
- `core/`: `libocgbridge`, a C++17 library over ygopro-core: duel sessions (decks, a board placed
  before the duel, EDOPro-style puzzle scripts, seed, LP, rules presets and flags, script
  overrides, configurable script directories), card data from BabelCDB `.cdb` databases (SQLite)
  and YGOPRODeck-style JSON lists, every prompt decoded to JSON, typed answers with the engine's
  own checks, an auto-responder with caller hints, field snapshots, a player's view of the duel,
  event queues, and a decoding of the common engine messages.
- Extension points ([docs/extending.md](docs/extending.md)): an ask hook for any prompt kind,
  holds, manual prompt kinds and replacement default answers, message filters (observe or drop
  engine messages), named queries.
- `server/`: `ocgcore-bridge`, protocol v1 over stdin/stdout ([docs/protocol.md](docs/protocol.md)),
  with forked sessions (copies of a running duel), nested forks, event subscriptions and queries;
  also a library (`ocgbridge_server`) for servers with C++ extensions of their own.
- `client-ts/`: the `ocgcore-bridge` npm package (Node.js 18+).
- `examples/`: a random duel between two `.ydk` deck lists, a puzzle in both formats, a fork-based
  search, a server with C++ extensions.
- Tests: unit tests (ctest), golden protocol transcripts on pinned CardScripts / BabelCDB commits,
  client tests against the real binary, a check that the tree stays product-neutral; CI on
  ubuntu-latest.
- [docs/compat-mapping.md](docs/compat-mapping.md): porting a direct ygopro-core integration.
