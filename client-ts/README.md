# ocgcore-bridge (Node.js client)

Run Yu-Gi-Oh! duels on [edo9300/ygopro-core](https://github.com/edo9300/ygopro-core) from Node.js
18+. This package spawns the `ocgcore-bridge` binary (built from
[the repository](../README.md)) and speaks its protocol v1 with typed Promises.

```ts
import { Bridge } from 'ocgcore-bridge';

const bridge = await Bridge.start({
  binary: '/path/to/ocgcore-bridge',   // or $OCGCORE_BRIDGE_BIN, or on PATH
  scripts: '/path/to/CardScripts',
  cdb: ['/path/to/cards.cdb'],
  onAsk: ({ prompt }) => null,         // init.responder.ask prompts; null = the default answer
  onEvent: (event, session) => {},     // init.events.subscribe messages, as they happen
});
let state = await bridge.init({ seed: 7, players: [{ main: deckA }, { main: deckB }] });
const p = state.pending;               // an IdlePrompt, ChainPrompt, ...
if (p?.kind === 'idle' && p.summonable.length) {
  state = (await bridge.respond({ action: 'summon', index: 0 })).state;
}
const fork = await bridge.fork();      // a copy of the duel, for search
await fork.respond({ action: 'end' });
await fork.destroy();
bridge.close();
```

API (every method returns a Promise; a fork has the same methods):

| Method | Request |
|---|---|
| `Bridge.start(options)` | spawns the binary, waits for `ready` |
| `hello()`, `loadCards(source)` | `hello`, `loadCards` |
| `init(options)`, `state()`, `pending()` | `init`, `state` |
| `respond(answer, { then, max, hints })` | `respond` |
| `process()`, `advance({ max, answerFirst, hints })`, `autoAnswer(hints)` | `process`, `advance`, `autoAnswer` |
| `encode(answer)` | `encode` |
| `snapshot()`, `context(player?)`, `take(queues?)` | `snapshot`, `context`, `take` |
| `query(name, args)` | `query` |
| `fork(options)`, `destroy()` | `fork`, `destroy` |
| `close()` | `exit` |

A rejected request throws a `BridgeError` with the protocol's `code` (`illegal_answer`,
`no_duel`, ...). The types (`Prompt`, `Answer`, `Hints`, `DuelState`, ...) follow
[docs/protocol.md](../docs/protocol.md); the extension points (asks, manual kinds, replacement
defaults, events, queries) are in [docs/extending.md](../docs/extending.md).

Licence: AGPL-3.0-or-later.
