// SPDX-License-Identifier: AGPL-3.0-or-later
// A puzzle, from either format:
//   examples/puzzles/lethal.json   init options with boards (docs/protocol.md, init)
//   examples/puzzles/lethal.lua    an EDOPro-style puzzle script (init.puzzleScript)
// "Win this turn": go to battle and attack with everything.
//   node examples/puzzle.mjs [examples/puzzles/lethal.lua]
import fs from 'node:fs';
import path from 'node:path';
import { Bridge, bridgeOptions, root } from './common.mjs';

const file = process.argv[2] ?? path.join(root, 'examples/puzzles/lethal.json');
const bridge = await Bridge.start(bridgeOptions());
const { description, _comment, ...board } = file.endsWith('.lua')
  ? { puzzleScript: path.resolve(file) }
  : JSON.parse(fs.readFileSync(file, 'utf8'));
if (description) console.log(description);
let state = await bridge.init(board);
const [me] = await bridge.snapshot();
console.log('Xyz materials:', me.mzone[2]?.overlay);
for (let i = 0; i < 100 && !state.ended; i++) {
  const p = state.pending;
  let answer;
  if (p.kind === 'idle') answer = p.toBattle ? { action: 'battle' } : { action: 'end' };
  else if (p.kind === 'battle') answer = p.attackable.length ? { action: 'attack', index: 0 } : { action: 'end' };
  else if (p.kind === 'chain') answer = { index: p.forced ? 0 : -1 };
  state = answer ? (await bridge.respond(answer)).state : (await bridge.advance({ answerFirst: true })).state;
}
console.log(state.ended ? `solved: player ${state.winner} wins` : 'not solved');
bridge.close();
if (!state.ended || state.winner !== 0) process.exitCode = 1;
