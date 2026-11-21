// SPDX-License-Identifier: AGPL-3.0-or-later
// A duel between two deck lists, both players choosing at random among their
// legal actions; every prompt inside an effect gets the auto-responder's answer.
//   node examples/random-duel.mjs [seed]
import { Bridge, bridgeOptions, choices, decks, rng } from './common.mjs';

const seed = Number(process.argv[2] ?? 1);
const rand = rng(seed);
const bridge = await Bridge.start(bridgeOptions());
let state = await bridge.init({
  seed,
  shuffleDecks: true,
  players: [
    { main: decks.spellcasters.main, extra: decks.spellcasters.extra },
    { main: decks.classic.main, extra: decks.classic.extra },
  ],
});
let steps = 0;
while (!state.ended && state.turn <= 40 && steps < 4000) {
  const options = choices(state.pending);
  if (options.length === 0) {
    // Not a decision point: let the auto-responder answer it.
    state = (await bridge.advance({ answerFirst: true })).state;
    continue;
  }
  // Going to battle (and attacking) often, and ending the turn now and then, keeps random play moving.
  const battle = options.find((o) => o.action === 'battle' || o.action === 'attack');
  const end = options.find((o) => o.action === 'end');
  const answer = battle && rand() < 0.6 ? battle : end && rand() < 0.2 ? end : options[Math.floor(rand() * options.length)];
  state = (await bridge.respond(answer)).state;
  steps++;
}
const [p0, p1] = await bridge.snapshot();
console.log(state.ended ? `player ${state.winner} won on turn ${state.turn}` : `stopped on turn ${state.turn}`);
console.log(`LP ${p0.lp} / ${p1.lp} after ${steps} decisions`);
bridge.close();
