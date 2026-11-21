// SPDX-License-Identifier: AGPL-3.0-or-later
// The client against the real binary. Needs OCGCORE_BRIDGE_BIN,
// OCGCORE_BRIDGE_SCRIPTS (CardScripts) and OCGCORE_BRIDGE_CDB (cards.cdb);
// skipped without them.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { Bridge, BridgeError, IdlePrompt, InitOptions, AskRequest, EngineEvent } from '../src/index';

const BIN = process.env.OCGCORE_BRIDGE_BIN;
const SCRIPTS = process.env.OCGCORE_BRIDGE_SCRIPTS;
const CDB = process.env.OCGCORE_BRIDGE_CDB;
const skip = !BIN || !SCRIPTS || !CDB || !fs.existsSync(BIN) ? 'set OCGCORE_BRIDGE_BIN, OCGCORE_BRIDGE_SCRIPTS and OCGCORE_BRIDGE_CDB' : false;

// The example .ydk deck lists.
const ydk = (name: string) => {
  const deck = { main: [] as number[], extra: [] as number[] };
  let part: 'main' | 'extra' | null = null;
  for (const line of fs.readFileSync(path.join(__dirname, `../../../examples/decks/${name}.ydk`), 'utf8').split(/\r?\n/)) {
    if (line === '#main' || line === '#extra') part = line.slice(1) as 'main' | 'extra';
    else if (line === '!side') part = null;
    else if (/^\d+$/.test(line) && part) deck[part].push(Number(line));
  }
  return deck;
};
const decks = { classic: ydk('classic'), spellcasters: ydk('spellcasters') };
const duel = (extra: Partial<InitOptions> = {}): InitOptions => ({
  seed: 5,
  players: [
    { main: decks.classic.main, extra: decks.classic.extra },
    { main: decks.spellcasters.main, extra: decks.spellcasters.extra },
  ],
  ...extra,
});
const start = (opts: Partial<Parameters<typeof Bridge.start>[0]> = {}) => Bridge.start({ binary: BIN, scripts: SCRIPTS, cdb: [CDB!], ...opts });

test('hello and init', { skip }, async () => {
  const b = await start();
  try {
    const hello = await b.hello();
    assert.equal(hello.protocol, 1);
    assert.ok(hello.cards > 10000);
    const st = await b.init(duel());
    assert.equal(st.duel, 'active');
    assert.equal(st.pending?.kind, 'idle');
    assert.equal(st.pending?.player, 0);
    assert.equal(st.turn, 1);
    const snap = await b.snapshot();
    assert.equal(snap[0].hand.length, 5);
    assert.equal(snap[1].deckCount, 35);
  } finally {
    b.close();
  }
});

test('respond, advance and the auto-responder', { skip }, async () => {
  const b = await start();
  try {
    let st = await b.init(duel());
    const idle = st.pending as IdlePrompt;
    assert.ok(idle.summonable.length > 0);
    const r = await b.respond({ action: 'summon', index: 0 });
    // The summon's zone prompt was answered by the auto-responder.
    assert.ok(r.answered.some((a) => a.kind === 'place'));
    st = r.state;
    assert.ok(st.pending);
    const snap = await b.snapshot();
    assert.equal(snap[0].mzone.filter(Boolean).length, 1);
    const q = await b.take(['activations', 'confirmed']);
    assert.ok(Array.isArray(q.activations));
    assert.equal(q.targets, undefined);
  } finally {
    b.close();
  }
});

test('illegal answers are rejected with a code', { skip }, async () => {
  const b = await start();
  try {
    await b.init(duel());
    await assert.rejects(b.respond({ action: 'battle' }), (e: unknown) => e instanceof BridgeError && e.code === 'illegal_answer');
    await assert.rejects(b.request('nope'), (e: unknown) => e instanceof BridgeError && e.code === 'unknown_request');
    assert.equal(await b.encode({ action: 'end' }), '07000000');
  } finally {
    b.close();
  }
});

test('a batch runs its requests in order, in one round trip', { skip }, async () => {
  const b = await start();
  try {
    await b.init(duel());
    const one = await b.fork();
    const two = await b.fork();
    await one.respond({ action: 'summon', index: 0 });
    await one.autoAnswer();
    const r = await two.batch([{ type: 'respond', answer: { action: 'summon', index: 0 } }, { type: 'autoAnswer' }, { type: 'advance', if: 'answered' }]);
    assert.equal(r.results.length, 3);
    assert.ok(r.results[0].ok && r.results[0].result && !('state' in r.results[0].result));
    assert.ok(r.results[2].ok && r.results[2].skipped);
    assert.deepEqual(r.state, await one.state());
    assert.deepEqual(await two.snapshot(), await one.snapshot());
    const bad = await two.batch([{ type: 'encode', answer: { action: 'nothing' } }, { type: 'process' }]);
    assert.equal(bad.results.length, 1);
    assert.ok(!bad.results[0].ok && bad.results[0].error.code === 'illegal_answer');
    await one.destroy();
    await two.destroy();
  } finally {
    b.close();
  }
});

test('forks are independent copies', { skip }, async () => {
  const b = await start();
  try {
    await b.init(duel());
    const before = await b.snapshot();
    const f = await b.fork();
    assert.ok(f.session);
    await f.respond({ action: 'summon', index: 0 });
    const forked = await f.snapshot();
    assert.equal(forked[0].mzone.filter(Boolean).length, 1);
    assert.deepEqual(await b.snapshot(), before);
    const nested = await f.fork();
    assert.equal(nested.session?.split('/').length, 2);
    assert.deepEqual(await nested.snapshot(), forked);
    await nested.destroy();
    await f.destroy();
    await assert.rejects(f.state(), (e: unknown) => e instanceof BridgeError && e.code === 'no_session');
  } finally {
    b.close();
  }
});

test('a detached fork outlives the fork it was made from', { skip }, async () => {
  const b = await start();
  try {
    await b.init(duel());
    const f = await b.fork();
    await f.respond({ action: 'summon', index: 0 });
    const d = await f.fork({ detach: true });
    assert.ok(d.session && !d.session.includes('/'));
    assert.deepEqual(await d.snapshot(), await f.snapshot());
    assert.notEqual((await d.hello()).pid, (await f.hello()).pid);
    await f.destroy();
    assert.equal((await d.state()).duel, 'active');
    await d.destroy();
    await assert.rejects(d.state(), (e: unknown) => e instanceof BridgeError && e.code === 'no_session');
  } finally {
    b.close();
  }
});

test('asks go to onAsk', { skip }, async () => {
  const asks: AskRequest[] = [];
  const b = await start({
    onAsk: (a) => {
      asks.push(a);
      if (a.prompt.kind === 'place') {
        for (let s = 6; s >= 0; s--) if (!(a.prompt.flag & (1 << s))) return { player: a.prompt.player, loc: 4, seq: s };
      }
      return null;
    },
  });
  try {
    await b.init(duel({ responder: { ask: [['place'], null] } }));
    await b.respond({ action: 'summon', index: 0 });
    assert.equal(asks.length, 1);
    assert.equal(asks[0].prompt.kind, 'place');
    assert.equal(asks[0].context.turnPlayer, 0);
    const snap = await b.snapshot();
    // The ask put the monster in the rightmost free Main Monster Zone.
    assert.ok(snap[0].mzone[4]);
  } finally {
    b.close();
  }
});

test('holds leave the prompt to the caller', { skip }, async () => {
  const b = await start();
  try {
    await b.init(duel());
    const r = await b.respond({ action: 'summon', index: 0 }, { hints: { hold: [0] } });
    assert.equal(r.status, 'held');
    assert.equal(r.state.pending?.kind, 'place');
    const r2 = await b.respond({ player: 0, loc: 4, seq: 1 });
    assert.equal(r2.status, 'none');
    assert.ok((await b.snapshot())[0].mzone[1]);
  } finally {
    b.close();
  }
});

test('ask any kind with "*", and replacement defaults', { skip }, async () => {
  const kinds = new Set<string>();
  const b = await start({ onAsk: (a) => { kinds.add(a.prompt.kind); return null; } });
  try {
    await b.init(duel({ responder: { ask: [['*'], null], defaults: { position: { position: 4 } } } }));
    const r = await b.respond({ action: 'summon', index: 0 });
    assert.ok(kinds.has('place'));
    assert.ok(r.answered.every((a) => a.asked === false && a.replaced === false), 'a null answer takes the default');
  } finally {
    b.close();
  }
});

test('manual kinds are left to the caller', { skip }, async () => {
  const b = await start();
  try {
    await b.init(duel({ responder: { manual: ['place'] } }));
    const r = await b.respond({ action: 'summon', index: 0 });
    assert.equal(r.status, 'none');
    assert.equal(r.state.pending?.kind, 'place');
    await b.respond({ player: 0, loc: 4, seq: 3 });
    assert.ok((await b.snapshot())[0].mzone[3]);
  } finally {
    b.close();
  }
});

test('a replacement default answers decision points', { skip }, async () => {
  const b = await start();
  try {
    // Every chain window passes; idle prompts still stop (no default for them).
    await b.init(duel({ responder: { defaults: { chain: { index: -1 } } } }));
    const r = await b.respond({ action: 'summon', index: 0 });
    assert.notEqual(r.state.pending?.kind, 'chain');
  } finally {
    b.close();
  }
});

test('events: subscribe and drop', { skip }, async () => {
  const events: Array<[EngineEvent, string | undefined]> = [];
  const b = await start({ onEvent: (e, s) => events.push([e, s]) });
  try {
    await b.init(duel({ recordMessages: true, events: { subscribe: ['*'], drop: ['hint'] } }));
    await b.respond({ action: 'summon', index: 0 });
    const summon = events.find(([e]) => e.name === 'summoning');
    assert.ok(summon, 'a summoning event');
    assert.equal(summon![1], undefined);
    assert.equal(typeof summon![0].code, 'number');
    assert.ok(!events.some(([e]) => e.name === 'hint'), 'dropped messages are not sent');
    const q = await b.take(['messages']);
    assert.ok(!q.messages!.some((m) => m.msg === 2), 'nor recorded');
    events.length = 0;
    const f = await b.fork();
    const p = (await f.state()).pending!;
    await f.respond(p.kind === 'idle' ? { action: 'end' } : { index: -1 });
    assert.ok(events.length > 0 && events.every(([, s]) => s === f.session), 'fork events carry the session');
    await f.destroy();
  } finally {
    b.close();
  }
});

test('queries', { skip }, async () => {
  const b = await start();
  try {
    const hello = await b.hello();
    assert.deepEqual([...hello.queries].sort(), ['card', 'chain', 'fieldCard']);
    const card = await b.query<{ code: number; type: number }>('card', { code: 89631139 });
    assert.equal(card.code, 89631139);
    await b.init(duel());
    await b.respond({ action: 'summon', index: 0 });
    const snap = await b.snapshot();
    const seq = snap[0].mzone.findIndex(Boolean);
    const fc = await b.query<{ code: number; atk: number }>('fieldCard', { ctrl: 0, loc: 4, seq });
    assert.equal(fc.code, snap[0].mzone[seq]!.code);
    assert.equal(fc.atk, snap[0].mzone[seq]!.atk);
    await assert.rejects(b.query('nope'), (e: unknown) => e instanceof BridgeError && e.code === 'unknown_query');
    await assert.rejects(b.query('fieldCard', {}), (e: unknown) => e instanceof BridgeError && e.code === 'bad_request');
  } finally {
    b.close();
  }
});
