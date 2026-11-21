// SPDX-License-Identifier: AGPL-3.0-or-later
// Protocol v1 types (docs/protocol.md). Numbers follow ygopro-core:
// LOCATION_* / POS_* / PHASE_* bits, MSG_* message types. u64 values (effect
// descriptions, select hints of the announce prompts) are decimal strings.

/** A card in a select list: controller, LOCATION_* bit, zone / pile index, POS_* bits. */
export interface CardRef {
  code: number;
  ctrl: number;
  loc: number;
  seq: number;
  pos: number;
}

/** An activation list entry: `desc` is the effect's description (u64, decimal string). */
export interface ActivationRef extends CardRef {
  desc: string;
}

export interface ZoneRef {
  loc: number;
  seq: number;
}

interface PromptBase {
  /** MSG_* type of the prompt. */
  msg: number;
  /** The player asked (for "rps": the round's step byte). */
  player: number;
  /** The HINT_SELECTMSG the prompt came with (0 = none). */
  hint: number;
  /** The engine message, hex. */
  raw: string;
}

export interface IdlePrompt extends PromptBase {
  kind: 'idle';
  summonable: CardRef[];
  spSummon: CardRef[];
  reposition: CardRef[];
  mset: CardRef[];
  sset: CardRef[];
  activatable: ActivationRef[];
  /** Spell speed of each activatable entry. */
  activatableSpeed: number[];
  toBattle: boolean;
  toEnd: boolean;
  canShuffle: boolean;
}

export interface BattlePrompt extends PromptBase {
  kind: 'battle';
  attackable: CardRef[];
  activatable: ActivationRef[];
  toMain2: boolean;
  toEnd: boolean;
}

export interface ChainPrompt extends PromptBase {
  kind: 'chain';
  activatable: ActivationRef[];
  forced: boolean;
  /** ygopro-core's spe_count: candidates that answer what just happened. */
  speCount: number;
  responding: boolean;
  /** Per activatable entry: an optional trigger effect. */
  triggers: boolean[];
  /** TIMING_* hint of the window, for its player and the opponent. */
  timing: number;
  timingOpponent: number;
}

export interface CardPrompt extends PromptBase {
  kind: 'card' | 'tribute' | 'unselectCard';
  candidates: Array<CardRef & { release?: number; selected?: boolean }>;
  min: number;
  max: number;
  cancelable: boolean;
  finishable?: boolean;
}

export interface EffectYesNoPrompt extends PromptBase {
  kind: 'effectYesNo';
  code: number;
  card: CardRef;
  description: string;
}

export interface YesNoPrompt extends PromptBase {
  kind: 'yesNo';
  description: string;
}

export interface OptionPrompt extends PromptBase {
  kind: 'option';
  options: string[];
}

export interface PositionPrompt extends PromptBase {
  kind: 'position';
  code: number;
  positions: number;
}

export interface PlacePrompt extends PromptBase {
  kind: 'place' | 'disfield';
  /** Unavailable zones (bit set): own Monster Zones 0-6, Spell & Trap Zones 8-15; the opponent's +16. */
  flag: number;
  count: number;
}

export interface SumPrompt extends PromptBase {
  kind: 'sum';
  candidates: Array<CardRef & { sum: number; sumHigh: number }>;
  must: Array<CardRef & { sum: number; sumHigh: number }>;
  acc: number;
  min: number;
  max: number;
  greater: boolean;
}

export interface CounterPrompt extends PromptBase {
  kind: 'counter';
  counterType: number;
  count: number;
  candidates: Array<CardRef & { counters: number }>;
}

export interface SortPrompt extends PromptBase {
  kind: 'sortCard' | 'sortChain';
  candidates: Array<{ code: number; ctrl: number; loc: number; seq: number }>;
}

export interface AnnounceBitsPrompt extends PromptBase {
  kind: 'announceRace' | 'announceAttribute';
  count: number;
  available: string;
}

export interface AnnounceCardPrompt extends PromptBase {
  kind: 'announceCard';
  opcodes: string[];
}

export interface AnnounceNumberPrompt extends PromptBase {
  kind: 'announceNumber';
  options: string[];
}

export interface RpsPrompt extends PromptBase {
  kind: 'rps';
}

export type Prompt =
  | IdlePrompt | BattlePrompt | ChainPrompt | CardPrompt | EffectYesNoPrompt | YesNoPrompt | OptionPrompt
  | PositionPrompt | PlacePrompt | SumPrompt | CounterPrompt | SortPrompt | AnnounceBitsPrompt
  | AnnounceCardPrompt | AnnounceNumberPrompt | RpsPrompt;

export type PromptKind = Prompt['kind'];

export interface ChainLink {
  code: number;
  ctrl: number;
  loc: number;
  seq: number;
}

export interface DuelState {
  duel: 'active' | 'none';
  ended?: boolean;
  winner?: number;
  reason?: number;
  /** The last prompt the engine sent (it stays set once answered, until the next one). */
  pending?: Prompt | null;
  /** An answer is waiting to be read by the engine. */
  responseReady?: boolean;
  turn?: number;
  turnPlayer?: number;
  phase?: number;
  chain?: ChainLink[];
}

/** Answers (docs/protocol.md "Answers"); `raw` / `i32` are passed through unchecked. */
export type Answer =
  | { raw: string }
  | { i32: number }
  | { action: 'summon' | 'spsummon' | 'repos' | 'mset' | 'sset' | 'activate' | 'attack'; index: number }
  | { action: 'battle' | 'end' | 'shuffle' | 'main2' }
  | { index: number }
  | { indices: number[] }
  | { yes: boolean | 0 | 1 }
  | { option: number }
  | { position: number }
  | { player: number; loc: number; seq: number }
  | { places: Array<{ player: number; loc: number; seq: number }> }
  | { counts: number[] }
  | { order: number[] | null }
  | { value: number | string }
  | { code: number }
  | { hand: 1 | 2 | 3 };

export interface Hints {
  place?: ZoneRef;
  tributes?: ZoneRef[];
  materials?: ZoneRef[];
  attackTarget?: ZoneRef;
  deferCardSelect?: boolean;
  ask?: boolean;
  hold?: number[];
  autorespond?: boolean;
}

export interface AnsweredPrompt {
  msg: number;
  kind: PromptKind | 'other';
  player: number;
  raw: string;
  directed: boolean;
  asked: boolean;
  /** Answered by a replacement default (responder.defaults, or one registered in C++). */
  replaced: boolean;
}

export interface StepResult {
  status: 'none' | 'answered' | 'held';
  answered: AnsweredPrompt[];
  state: DuelState;
}

export interface BoardCard {
  code: number;
  loc: number;
  seq: number;
  pos: number;
  materials?: number[];
}

export interface PlayerSetup {
  /** Main Deck in draw order: index 0 is drawn first. */
  main?: number[];
  extra?: number[];
  board?: BoardCard[];
  lp?: number;
  startingHand?: number;
  drawPerTurn?: number;
}

/** A prompt kind, or "*" for every in-effect kind (all but idle, battle and chain). */
export type PromptKindOrAll = PromptKind | '*';

export interface ResponderOptions {
  sum?: 'exact' | 'checked';
  /** Per player: prompt kinds put to the caller (onAsk) before the default. */
  ask?: [PromptKindOrAll[] | null, PromptKindOrAll[] | null];
  holdKinds?: PromptKindOrAll[];
  holdTrivial?: boolean;
  effectYesNoRepeatLimit?: number;
  /** Prompt kinds never answered automatically. */
  manual?: PromptKindOrAll[];
  /** Replacement defaults: a fixed answer per kind, used when it is legal for the prompt. */
  defaults?: Partial<Record<PromptKind, Answer>>;
}

/** An engine message: name or MSG_* number; "*" (subscribe only) is every message. */
export type MessageSelector = string | number;

export interface EventOptions {
  /** Messages sent as events (onEvent) while a request runs. */
  subscribe?: MessageSelector[];
  /** Messages kept from everything the caller sees (events, take queues, recordMessages). */
  drop?: MessageSelector[];
}

/** A decoded engine message: { msg, name, ...fields, raw } (docs/protocol.md "Events"). */
export interface EngineEvent {
  msg: number;
  name: string;
  raw: string;
  [field: string]: unknown;
}

export interface InitOptions {
  players: [PlayerSetup, PlayerSetup];
  /** A 64-bit seed (expanded with SplitMix64) or the engine's 4 words. */
  seed?: number | [number, number, number, number];
  rules?: 'mr1' | 'mr2' | 'mr3' | 'mr4' | 'mr5' | 'goat' | 'speed' | 'rush';
  flags?: number;
  pseudoShuffle?: boolean;
  shuffleDecks?: boolean;
  attackFirstTurn?: boolean;
  scriptOverrides?: Record<string, string>;
  responder?: ResponderOptions;
  chainStartSnapshots?: boolean;
  recordMessages?: boolean;
  scripts?: string;
  /** Script lookup directories under `scripts` ("" = the directory itself). */
  scriptDirs?: string[];
  /** Scripts loaded when the duel is created (default constant.lua, utility.lua). */
  preload?: string[];
  events?: EventOptions;
}

export interface SnapshotCard {
  code: number;
  position: number;
  overlay?: number[];
  atk?: number;
  def?: number;
  destroyed?: boolean;
}

export interface PlayerSnapshot {
  mzone: Array<SnapshotCard | null>;
  szone: Array<SnapshotCard | null>;
  hand: Array<SnapshotCard | null>;
  grave: Array<SnapshotCard | null>;
  banished: Array<SnapshotCard | null>;
  extra: Array<SnapshotCard | null>;
  deckCount: number;
  lp: number;
}

export interface VisibleCard {
  code: number;
  pos: number;
  atk?: number;
  def?: number;
  level?: number;
  type?: number;
}

export interface PromptContext {
  hint: number;
  chainLength: number;
  effect?: { code: number; ctrl: number; category: number };
  chain?: ChainLink[];
  turnPlayer?: number;
  players?: Array<{
    lp: number;
    mzone: Array<VisibleCard | null>;
    szone: Array<VisibleCard | null>;
    grave: number[];
    handCount: number;
    deckCount: number;
    hand?: number[];
  }>;
}

export interface Activation extends ChainLink {
  turn: number;
  phase: number;
  field?: [PlayerSnapshot, PlayerSnapshot];
}

export interface Queues {
  activations?: Activation[];
  targets?: Array<{ by: ChainLink; player: number; targets: ChainLink[] }>;
  negations?: Array<ChainLink & { link: number; kind: 'activation' | 'effect' }>;
  confirmed?: Array<ChainLink & { to: number }>;
  messages?: Array<{ msg: number; raw: string }>;
}

export type QueueName = keyof Queues;

export interface AskRequest {
  ask: number;
  prompt: Prompt;
  context: PromptContext;
}

export interface Hello {
  protocol: number;
  version: string;
  cdb: boolean;
  cards: number;
  fork: boolean;
  /** The process id of the session's process. */
  pid: number;
  /** Named queries the server answers (query). */
  queries: string[];
}
