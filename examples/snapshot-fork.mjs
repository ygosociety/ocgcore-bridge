// SPDX-License-Identifier: AGPL-3.0-or-later
// Search by forking: from one position, try every legal first action in its
// own fork (a copy of the duel process), compare the boards they lead to, and
// play the best one in the real duel.
//   node examples/snapshot-fork.mjs
import { Bridge, bridgeOptions, choices, decks } from './common.mjs';

const bridge = await Bridge.start(bridgeOptions());
const state = await bridge.init({
  seed: 3,
  players: [
    { main: decks.classic.main, extra: decks.classic.extra },
    { main: decks.spellcasters.main, extra: decks.spellcasters.extra },
  ],
});
const me = state.pending.player;
// Board value: our LP minus theirs, plus 1000 per card we control on the field.
const value = (snap) =>
  snap[me].lp - snap[1 - me].lp + 1000 * (snap[me].mzone.filter(Boolean).length + snap[me].szone.filter(Boolean).length);

let best = null;
for (const answer of choices(state.pending)) {
  const fork = await bridge.fork();
  await fork.respond(answer);
  const v = value(await fork.snapshot());
  console.log(JSON.stringify(answer), '->', v);
  if (!best || v > best.v) best = { answer, v };
  await fork.destroy();
}
console.log('best:', JSON.stringify(best.answer));
await bridge.respond(best.answer);
console.log('real duel now has', (await bridge.snapshot())[me].mzone.filter(Boolean).length, 'monster(s)');
bridge.close();
