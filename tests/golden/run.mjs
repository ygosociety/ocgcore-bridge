#!/usr/bin/env node
// SPDX-License-Identifier: AGPL-3.0-or-later
// Golden protocol tests: fixed-seed scenarios driven over protocol v1, each
// written as a transcript (one JSON line per step) and compared with the one
// in tests/golden/transcripts/. The same seed and inputs give the same duel.
//
//   node tests/golden/run.mjs --bin build/ocgcore-bridge --scripts <CardScripts> --cdb <cards.cdb>
//        [--extended-bin build/ocgcore-bridge-extended] [--update] [scenario...]
//
// Pinned data: CardScripts and BabelCDB at the commits in tests/golden/pins.json.
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const opt = (name) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : undefined; };
const BIN = opt('--bin');
const SCRIPTS = opt('--scripts') ?? process.env.OCGCORE_BRIDGE_SCRIPTS;
const CDB = opt('--cdb') ?? process.env.OCGCORE_BRIDGE_CDB;
const EXTENDED_BIN = opt('--extended-bin');
const UPDATE = args.includes('--update');
const only = args.filter((a, i) => !a.startsWith('--') && !['--bin', '--scripts', '--cdb', '--extended-bin'].includes(args[i - 1]));
if (!BIN || !SCRIPTS || !CDB) {
  console.error('usage: run.mjs --bin <ocgcore-bridge> --scripts <CardScripts dir> --cdb <cards.cdb> [--update] [scenario...]');
  process.exit(2);
}
if (!fs.existsSync(SCRIPTS) || !fs.existsSync(CDB)) {
  console.log(`golden: skipped (no card data at ${SCRIPTS} / ${CDB}; see tests/golden/fetch-data.sh)`);
  process.exit(77); // ctest SKIP_RETURN_CODE
}
// The example deck lists (.ydk: codes under #main / #extra / !side).
const readYdk = (file) => {
  const deck = { main: [], extra: [] };
  let part = null;
  for (const line of fs.readFileSync(file, 'utf8').split(/\r?\n/).map((l) => l.trim())) {
    if (line === '#main' || line === '#extra') part = line.slice(1);
    else if (line === '!side') part = null;
    else if (/^\d+$/.test(line) && part) deck[part].push(Number(line));
  }
  return deck;
};
const decks = {
  spellcasters: readYdk(path.join(here, '../../examples/decks/spellcasters.ydk')),
  classic: readYdk(path.join(here, '../../examples/decks/classic.ydk')),
};

// ── a minimal protocol client (no dependencies) ──
function startBridge(onAsk, { onEvent, bin = BIN } = {}) {
  const proc = spawn(bin, ['--scripts', SCRIPTS, '--cdb', CDB], { stdio: ['pipe', 'pipe', 'ignore'] });
  const waiting = new Map();
  let nextId = 1;
  let ready;
  const readyP = new Promise((r) => { ready = r; });
  createInterface({ input: proc.stdout }).on('line', (line) => {
    const msg = JSON.parse(line);
    if (msg.type === 'ready') return ready(msg);
    if (msg.type === 'event') return onEvent?.(msg.event, msg.session);
    if (msg.type === 'ask') {
      const answer = onAsk ? onAsk(msg) : null;
      proc.stdin.write(JSON.stringify({ type: 'answer', ask: msg.ask, answer: answer ?? null }) + '\n');
      return;
    }
    const w = waiting.get(msg.id);
    if (!w) return;
    waiting.delete(msg.id);
    msg.ok ? w.resolve(msg.result) : w.reject(Object.assign(new Error(msg.error.message), { code: msg.error.code }));
  });
  const call = (type, fields = {}, session) => new Promise((resolve, reject) => {
    const id = nextId++;
    waiting.set(id, { resolve, reject });
    proc.stdin.write(JSON.stringify({ ...fields, id, type, ...(session ? { session } : {}) }) + '\n');
  });
  return { readyP, call, close: () => { proc.stdin.end(); } };
}

// mulberry32: a small seeded PRNG for the scripted players.
function rng(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6D2B79F5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// A random legal decision at an idle / battle / chain prompt.
function decide(p, rand) {
  const pick = (n) => Math.floor(rand() * n);
  if (p.kind === 'idle') {
    const moves = [];
    for (const [action, list] of [['summon', p.summonable], ['spsummon', p.spSummon], ['mset', p.mset], ['sset', p.sset], ['activate', p.activatable], ['repos', p.reposition]])
      list.forEach((_, index) => moves.push({ action, index }));
    if (p.toBattle) moves.push({ action: 'battle' });
    if (p.toEnd && (moves.length === 0 || rand() < 0.3)) return { action: 'end' };
    return moves.length ? moves[pick(moves.length)] : { action: 'end' };
  }
  if (p.kind === 'battle') {
    const moves = [];
    p.attackable.forEach((_, index) => moves.push({ action: 'attack', index }));
    p.activatable.forEach((_, index) => moves.push({ action: 'activate', index }));
    if (moves.length && rand() < 0.7) return moves[pick(moves.length)];
    return p.toMain2 && rand() < 0.5 ? { action: 'main2' } : p.toEnd ? { action: 'end' } : moves[0];
  }
  if (p.kind === 'chain') {
    if (!p.forced && (p.activatable.length === 0 || rand() < 0.6)) return { index: -1 };
    return { index: pick(p.activatable.length) };
  }
  return null;
}

// A legal answer to an in-effect prompt (asks and holds): the first one.
function firstAnswer(p) {
  switch (p.kind) {
    case 'card': return { indices: [...Array(Math.max(p.min, 1)).keys()].slice(0, p.candidates.length) };
    case 'tribute': {
      const indices = [];
      let sum = 0;
      for (let i = 0; i < p.candidates.length && sum < p.min; i++) { indices.push(i); sum += p.candidates[i].release || 1; }
      return { indices };
    }
    case 'unselectCard': return (p.finishable || p.cancelable) && p.min === 0 ? { index: -1 } : { index: 0 };
    case 'effectYesNo': return { yes: true };
    case 'yesNo': return { yes: false };
    case 'option': return { option: p.options.length - 1 };
    case 'position': { for (const b of [8, 4, 2, 1]) if (p.positions & b) return { position: b }; return null; }
    case 'place': {
      for (let s = 6; s >= 0; s--) if (!(p.flag & (1 << s))) return { player: p.player, loc: 4, seq: s };
      for (let s = 7; s >= 0; s--) if (!(p.flag & (1 << (s + 8)))) return { player: p.player, loc: 8, seq: s };
      return null;
    }
    default: return null;
  }
}

const lp = (snap) => snap.map((s) => s.lp);
const counts = (snap) => snap.map((s) => ({ hand: s.hand.length, deck: s.deckCount, grave: s.grave.length, field: s.mzone.filter(Boolean).length + s.szone.filter(Boolean).length }));

// Plays until the duel ends or `maxSteps`, one transcript line per decision.
async function play(b, out, { seed, maxSteps = 400, maxTurn = 12, hints, session, onPrompt }) {
  const rand = rng(seed);
  let st = (await b.call('state', {}, session)).state;
  for (let step = 0; step < maxSteps; step++) {
    if (st.ended) { out.push({ end: true, winner: st.winner, reason: st.reason, turn: st.turn }); return st; }
    if (st.turn > maxTurn) { out.push({ stop: 'turns', turn: st.turn }); return st; }
    const p = st.pending;
    let answer = decide(p, rand);
    if (!answer && onPrompt) answer = onPrompt(p);
    if (!answer) {
      const r = await b.call('advance', { answerFirst: true, hints }, session);
      out.push({ step, auto: p.kind, status: r.status, answered: r.answered.map((a) => a.kind) });
      st = r.state;
      if (r.status !== 'answered' && st.pending?.kind === p.kind && !onPrompt) { out.push({ stuck: p.kind }); return st; }
      continue;
    }
    const r = await b.call('respond', { answer, hints }, session);
    st = r.state;
    out.push({ step, kind: p.kind, player: p.player, answer, status: r.status, answered: r.answered.map((a) => a.kind), turn: st.turn, phase: st.phase });
  }
  out.push({ stop: 'steps' });
  return st;
}

const scenarios = {
  // A full duel between two decks with seeded random players; every in-effect
  // prompt gets the auto-responder's default.
  async duel(out) {
    const b = startBridge();
    await b.readyP;
    const st = (await b.call('init', {
      seed: 7, shuffleDecks: true,
      players: [{ main: decks.spellcasters.main, extra: decks.spellcasters.extra }, { main: decks.classic.main, extra: decks.classic.extra }],
    })).state;
    out.push({ start: st.pending.kind, player: st.pending.player, turn: st.turn });
    await play(b, out, { seed: 1, maxSteps: 500, maxTurn: 14 });
    const snap = (await b.call('snapshot')).players;
    out.push({ lp: lp(snap), counts: counts(snap) });
    const q = await b.call('take');
    out.push({ activations: q.activations.length, targets: q.targets.length, negations: q.negations.length, confirmed: q.confirmed.length });
    b.close();
  },

  // A board set up before the duel: a Monster Zone card, an Xyz with
  // materials, the first player attacking on turn 1.
  async puzzle(out) {
    const b = startBridge();
    await b.readyP;
    const st = (await b.call('init', {
      attackFirstTurn: true,
      players: [
        { startingHand: 0, board: [
          { code: 70781052, loc: 4, seq: 0, pos: 1 },
          { code: 71594310, loc: 4, seq: 2, pos: 1, materials: [69140098, 69140098] },
          { code: 55144522, loc: 2, seq: 0, pos: 10 },
        ], main: [40640057, 40640057, 40640057] },
        { lp: 3000, startingHand: 0, board: [{ code: 44095762, loc: 8, seq: 1, pos: 10 }], main: [40640057, 40640057] },
      ],
    })).state;
    out.push({ start: st.pending.kind, actions: { summonable: st.pending.summonable.length, activatable: st.pending.activatable.map((a) => a.code), toBattle: st.pending.toBattle } });
    const snap = (await b.call('snapshot')).players;
    out.push({ overlay: snap[0].mzone[2]?.overlay ?? null, atk: snap[0].mzone.map((c) => c?.atk ?? null) });
    // Straight to battle; attack directly with everything that can.
    let r = { state: st };
    for (let i = 0; i < 60 && !r.state.ended; i++) {
      const p = r.state.pending;
      let answer = null;
      if (p.kind === 'idle') answer = p.toBattle ? { action: 'battle' } : { action: 'end' };
      else if (p.kind === 'battle') answer = p.attackable.length ? { action: 'attack', index: 0 } : { action: 'end' };
      else if (p.kind === 'chain') answer = { index: p.forced ? 0 : -1 };
      r = answer ? await b.call('respond', { answer }) : await b.call('advance', { answerFirst: true });
      out.push({ step: i, kind: p.kind, answer, status: r.status, answered: r.answered.map((a) => a.kind), lp: lp((await b.call('snapshot')).players) });
    }
    out.push({ ended: !!r.state.ended, winner: r.state.winner ?? null });
    b.close();
  },

  // Forks: two lines from the same position, each in its own copy; the duel
  // itself does not move, and a fork of a fork works.
  async fork(out) {
    const b = startBridge();
    await b.readyP;
    await b.call('init', { seed: 3, players: [{ main: decks.classic.main, extra: decks.classic.extra }, { main: decks.spellcasters.main, extra: decks.spellcasters.extra }] });
    const before = counts((await b.call('snapshot')).players);
    const f1 = (await b.call('fork')).session;
    const f2 = (await b.call('fork')).session;
    const a = []; await play(b, a, { seed: 11, maxSteps: 60, maxTurn: 3, session: f1 });
    const c = []; await play(b, c, { seed: 12, maxSteps: 60, maxTurn: 3, session: f2 });
    out.push({ fork1: a.length, fork1End: counts((await b.call('snapshot', {}, f1)).players) });
    out.push({ fork2: c.length, fork2End: counts((await b.call('snapshot', {}, f2)).players) });
    const nested = (await b.call('fork', {}, f1)).session;
    const n = []; await play(b, n, { seed: 13, maxSteps: 20, maxTurn: 4, session: nested });
    out.push({ nested: nested.split('/').length, nestedSteps: n.length });
    await b.call('destroy', {}, nested);
    await b.call('destroy', {}, f1);
    await b.call('destroy', {}, f2);
    const after = counts((await b.call('snapshot')).players);
    out.push({ unchanged: JSON.stringify(before) === JSON.stringify(after), main: after });
    try { await b.call('state', {}, f1); out.push({ gone: false }); } catch (e) { out.push({ gone: e.code }); }
    b.close();
  },

  // batch: the same requests one by one in one fork and as one batch in
  // another give the same duel; "if" skips, the first error ends a batch, and
  // a batch reaches a fork of a fork.
  async batch(out) {
    const b = startBridge();
    await b.readyP;
    await b.call('init', { seed: 3, players: [{ main: decks.classic.main, extra: decks.classic.extra }, { main: decks.spellcasters.main, extra: decks.spellcasters.extra }] });
    const f1 = (await b.call('fork')).session;
    const f2 = (await b.call('fork')).session;
    const rand = rng(17);
    const requests = [];
    let st = (await b.call('state', {}, f1)).state;
    for (let i = 0; i < 40 && !st.ended && st.turn <= 3; i++) {
      const answer = decide(st.pending, rand);
      const req = answer ? { type: 'respond', answer } : { type: 'advance', answerFirst: true };
      requests.push(req);
      const { type, ...fields } = req;
      st = (await b.call(type, fields, f1)).state;
    }
    const r = await b.call('batch', { requests }, f2);
    const snap1 = (await b.call('snapshot', {}, f1)).players;
    const snap2 = (await b.call('snapshot', {}, f2)).players;
    out.push({ requests: requests.length, results: r.results.length, allOk: r.results.every((x) => x.ok && !('state' in x.result)),
      sameState: JSON.stringify(r.state) === JSON.stringify((await b.call('state', {}, f1)).state), sameSnapshot: JSON.stringify(snap1) === JSON.stringify(snap2), turn: r.state.turn });
    const c = await b.call('batch', { requests: [
      { type: 'autoAnswer' },
      { type: 'advance', if: 'answered' },
      { type: 'state', if: ['none', 'held'] },
      { type: 'encode', answer: { action: 'nothing' } },
      { type: 'process' },
    ] }, f2);
    out.push({ conditional: c.results.map((x) => (x.skipped ? 'skipped' : x.ok ? 'ok' : x.error.code)), status: c.results[0].result.status });
    const nested = (await b.call('fork', {}, f2)).session;
    const n = await b.call('batch', { requests: [{ type: 'snapshot' }, { type: 'fork' }] }, nested);
    out.push({ nested: nested.split('/').length, results: n.results.map((x) => (x.ok ? Object.keys(x.result).join(',') : x.error.code)), sameAsParent: JSON.stringify(n.results[0].result.players) === JSON.stringify(snap2) });
    const empty = await b.call('batch', { requests: [] });
    out.push({ empty: empty.results.length, main: empty.state.duel });
    try { await b.call('batch', {}); out.push({ noRequests: 'ok' }); } catch (e) { out.push({ noRequests: e.code }); }
    b.close();
  },

  // The ask hook: player 0's in-effect prompts are put to the caller.
  // detach: a fork made in a fork that the main process talks to directly. It
  // is the same copy as a nested fork made at the same moment, it outlives
  // the fork it was made from, and no process is left once the bridge exits.
  async detach(out) {
    const b = startBridge();
    await b.readyP;
    await b.call('init', { seed: 3, players: [{ main: decks.classic.main, extra: decks.classic.extra }, { main: decks.spellcasters.main, extra: decks.spellcasters.extra }] });
    const pids = [(await b.call('hello')).pid];
    const f1 = (await b.call('fork')).session;
    await play(b, [], { seed: 11, maxSteps: 20, maxTurn: 3, session: f1 });
    const d1 = await b.call('fork', { detach: true }, f1);
    const n1 = (await b.call('fork', {}, f1)).session;
    const a = []; await play(b, a, { seed: 21, maxSteps: 30, maxTurn: 4, session: d1.session });
    const c = []; await play(b, c, { seed: 21, maxSteps: 30, maxTurn: 4, session: n1 });
    const snap = async (s) => JSON.stringify((await b.call('snapshot', {}, s)).players);
    out.push({ d1: d1.session, detached: d1.detached, nested: n1, sameAsNested: JSON.stringify(a) === JSON.stringify(c) && (await snap(d1.session)) === (await snap(n1)), steps: a.length });
    const d2 = await b.call('fork', { detach: true }, n1);
    const d3 = await b.call('fork', { detach: true });
    out.push({ d2: d2.session, d3: d3.session, d3detached: d3.detached, d2fork: (await b.call('hello', {}, d2.session)).fork });
    for (const s of [f1, n1, d1.session, d2.session, d3.session]) pids.push((await b.call('hello', {}, s)).pid);
    await b.call('destroy', {}, f1);
    let nestedGone;
    try { await b.call('state', {}, n1); nestedGone = false; } catch (e) { nestedGone = e.code; }
    out.push({ afterParentDestroyed: { nested: nestedGone, d1: (await b.call('state', {}, d1.session)).state.duel, d2: (await b.call('state', {}, d2.session)).state.duel } });
    await b.call('destroy', {}, d2.session);
    b.close();
    const alive = () => pids.filter((pid) => { try { process.kill(pid, 0); return true; } catch { return false; } }).length;
    for (let i = 0; i < 100 && alive(); i++) await new Promise((r) => setTimeout(r, 50));
    out.push({ processes: pids.length, distinct: new Set(pids).size, leftAfterExit: alive() });
  },

  async ask(out) {
    const asked = [];
    const b = startBridge((msg) => { asked.push(msg.prompt.kind); return firstAnswer(msg.prompt); });
    await b.readyP;
    await b.call('init', {
      seed: 21, shuffleDecks: true,
      responder: { ask: [['card', 'tribute', 'unselectCard', 'effectYesNo', 'yesNo', 'option', 'position', 'place'], null] },
      players: [{ main: decks.spellcasters.main, extra: decks.spellcasters.extra }, { main: decks.classic.main, extra: decks.classic.extra }],
    });
    const lines = [];
    await play(b, lines, { seed: 5, maxSteps: 200, maxTurn: 8 });
    out.push(...lines);
    out.push({ asked: asked.length, kinds: [...new Set(asked)].sort() });
    b.close();
  },

  // Holds: player 1's in-effect prompts are left for the caller and answered with respond.
  async hold(out) {
    const b = startBridge();
    await b.readyP;
    await b.call('init', { seed: 33, shuffleDecks: true, players: [{ main: decks.classic.main, extra: decks.classic.extra }, { main: decks.spellcasters.main, extra: decks.spellcasters.extra }] });
    let held = 0;
    const lines = [];
    await play(b, lines, {
      seed: 9, maxSteps: 250, maxTurn: 8, hints: { hold: [1] },
      onPrompt: (p) => { held++; return firstAnswer(p); },
    });
    out.push(...lines);
    out.push({ held });
    b.close();
  },
};

// The extension points over the protocol: ask "*", manual kinds, replacement
// defaults, events (subscribe and drop) and the built-in queries.
scenarios.extend = async (out) => {
  const asked = {};
  const events = {};
  const b = startBridge((msg) => { asked[msg.prompt.kind] = (asked[msg.prompt.kind] ?? 0) + 1; return firstAnswer(msg.prompt); },
    { onEvent: (e) => { events[e.name] = (events[e.name] ?? 0) + 1; } });
  await b.readyP;
  const hello = await b.call('hello');
  out.push({ queries: hello.queries });
  out.push({ card: await b.call('query', { name: 'card', args: { code: 89631139 } }) });
  await b.call('init', {
    seed: 17, shuffleDecks: true, recordMessages: true,
    responder: { ask: [['*'], null], manual: ['effectYesNo'], defaults: { position: { position: 4 }, option: { option: 0 } } },
    events: { subscribe: ['chaining', 'damage', 'summoning', 'spsummoning', 'hint', 'win'], drop: ['hint'] },
    players: [{ main: decks.spellcasters.main, extra: decks.spellcasters.extra }, { main: decks.classic.main, extra: decks.classic.extra }],
  });
  let manual = 0;
  const lines = [];
  await play(b, lines, {
    seed: 3, maxSteps: 250, maxTurn: 8,
    onPrompt: (p) => { if (p.kind === 'effectYesNo') { manual++; return { yes: true }; } return null; },
  });
  out.push(...lines);
  const recorded = (await b.call('take', { queues: ['messages'] })).messages;
  const snap = (await b.call('snapshot')).players;
  const seq = snap[1].mzone.findIndex(Boolean);
  const fc = seq >= 0 ? await b.call('query', { name: 'fieldCard', args: { ctrl: 1, loc: 4, seq } }) : null;
  out.push({ asked, manual, events, hintsRecorded: recorded.filter((m) => m.msg === 2).length, fieldCard: fc });
  out.push({ chain: await b.call('query', { name: 'chain' }) });
  try { await b.call('query', { name: 'nope' }); } catch (e) { out.push({ unknownQuery: e.code }); }
  b.close();
};

// An EDOPro-style puzzle script (init.puzzleScript).
scenarios.luaPuzzle = async (out) => {
  const b = startBridge();
  await b.readyP;
  let r = { state: (await b.call('init', { puzzleScript: path.join(here, '../../examples/puzzles/lethal.lua') })).state };
  const snap = (await b.call('snapshot')).players;
  out.push({ start: r.state.pending.kind, lp: lp(snap), overlay: snap[0].mzone[2]?.overlay ?? null, szone: snap[1].szone.map((c) => c?.code ?? null) });
  for (let i = 0; i < 60 && !r.state.ended; i++) {
    const p = r.state.pending;
    let answer = null;
    if (p.kind === 'idle') answer = p.toBattle ? { action: 'battle' } : { action: 'end' };
    else if (p.kind === 'battle') answer = p.attackable.length ? { action: 'attack', index: 0 } : { action: 'end' };
    else if (p.kind === 'chain') answer = { index: p.forced ? 0 : -1 };
    r = answer ? await b.call('respond', { answer }) : await b.call('advance', { answerFirst: true });
    out.push({ step: i, kind: p.kind, answer, status: r.status, lp: lp((await b.call('snapshot')).players) });
  }
  out.push({ ended: !!r.state.ended, winner: r.state.winner ?? null });
  b.close();
};

// A server built on the server library with C++ extensions
// (examples/extended-server): a query, a message filter, a replacement default.
if (EXTENDED_BIN) scenarios.extendedServer = async (out) => {
  const b = startBridge(null, { bin: EXTENDED_BIN });
  await b.readyP;
  out.push({ queries: (await b.call('hello')).queries });
  await b.call('init', {
    seed: 23, shuffleDecks: true, recordMessages: true,
    players: [{ main: decks.classic.main, extra: decks.classic.extra }, { main: decks.spellcasters.main, extra: decks.spellcasters.extra }],
  });
  const lines = [];
  await play(b, lines, { seed: 3, maxSteps: 300, maxTurn: 10 });
  out.push({ steps: lines.length, end: lines[lines.length - 1] });
  const recorded = (await b.call('take', { queues: ['messages'] })).messages;
  out.push({ damageTaken: await b.call('query', { name: 'damageTaken' }), hintsRecorded: recorded.filter((m) => m.msg === 2).length });
  b.close();
};

let failed = 0;
const dir = path.join(here, 'transcripts');
fs.mkdirSync(dir, { recursive: true });
for (const [name, run] of Object.entries(scenarios)) {
  if (only.length && !only.includes(name)) continue;
  const out = [];
  await run(out);
  const text = out.map((o) => JSON.stringify(o)).join('\n') + '\n';
  const file = path.join(dir, `${name}.ndjson`);
  if (UPDATE) {
    fs.writeFileSync(file, text);
    console.log(`golden ${name}: written (${out.length} lines)`);
    continue;
  }
  const want = fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : '';
  if (want === text) { console.log(`golden ${name}: ok (${out.length} lines)`); continue; }
  failed++;
  const a = want.split('\n'), b = text.split('\n');
  const i = a.findIndex((l, k) => l !== b[k]);
  console.log(`golden ${name}: DIFFERS at line ${i + 1}\n  want ${a[i]}\n  got  ${b[i]}`);
}
process.exit(failed ? 1 : 0);
