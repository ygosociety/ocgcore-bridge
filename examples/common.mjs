// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared by the examples: where the binary and the card data are.
//   OCGCORE_BRIDGE_BIN      the ocgcore-bridge binary (default: build/ocgcore-bridge)
//   OCGCORE_BRIDGE_SCRIPTS  a CardScripts checkout (default: tests/data/CardScripts)
//   OCGCORE_BRIDGE_CDB      a cards.cdb (default: tests/data/BabelCDB/cards.cdb)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

export const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const require = createRequire(import.meta.url);
// The client, built with `npm run build` in client-ts/ (or `npm install ocgcore-bridge`).
export const { Bridge } = require(path.join(root, 'client-ts/dist/index.js'));

// A .ydk deck list (the format of YGOPro, EDOPro and most deck builders): card
// codes one per line under #main, #extra and !side; other # lines are comments.
export function readYdk(file) {
  const deck = { main: [], extra: [], side: [] };
  let part = null;
  for (const raw of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
    const line = raw.trim();
    if (line === '#main') part = 'main';
    else if (line === '#extra') part = 'extra';
    else if (line === '!side') part = 'side';
    else if (/^\d+$/.test(line) && part) deck[part].push(Number(line));
  }
  return deck;
}

// examples/decks/<name>.ydk; the Main Deck in the file's order (the duel shuffles it when asked).
export const decks = Object.fromEntries(
  fs.readdirSync(path.join(root, 'examples/decks')).filter((f) => f.endsWith('.ydk')).sort()
    .map((f) => [path.basename(f, '.ydk'), readYdk(path.join(root, 'examples/decks', f))]),
);

export function bridgeOptions(extra = {}) {
  return {
    binary: process.env.OCGCORE_BRIDGE_BIN ?? path.join(root, 'build/ocgcore-bridge'),
    scripts: process.env.OCGCORE_BRIDGE_SCRIPTS ?? path.join(root, 'tests/data/CardScripts'),
    cdb: [process.env.OCGCORE_BRIDGE_CDB ?? path.join(root, 'tests/data/BabelCDB/cards.cdb')],
    ...extra,
  };
}

// A small seeded PRNG (mulberry32), so a run can be repeated.
export function rng(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// Every legal decision at an idle / battle / chain prompt, as answers.
export function choices(p) {
  const out = [];
  if (p.kind === 'idle') {
    for (const [action, list] of [['summon', p.summonable], ['spsummon', p.spSummon], ['mset', p.mset], ['sset', p.sset], ['activate', p.activatable], ['repos', p.reposition]])
      list.forEach((_, index) => out.push({ action, index }));
    if (p.toBattle) out.push({ action: 'battle' });
    if (p.toEnd) out.push({ action: 'end' });
  } else if (p.kind === 'battle') {
    p.attackable.forEach((_, index) => out.push({ action: 'attack', index }));
    p.activatable.forEach((_, index) => out.push({ action: 'activate', index }));
    if (p.toMain2) out.push({ action: 'main2' });
    if (p.toEnd) out.push({ action: 'end' });
  } else if (p.kind === 'chain') {
    if (!p.forced) out.push({ index: -1 });
    p.activatable.forEach((_, index) => out.push({ index }));
  }
  return out;
}
