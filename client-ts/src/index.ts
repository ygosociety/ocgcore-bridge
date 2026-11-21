// SPDX-License-Identifier: AGPL-3.0-or-later
// ocgcore-bridge client: spawns the ocgcore-bridge binary and speaks protocol
// v1 (newline-delimited JSON over stdin / stdout) with a Promise API.
import { spawn, ChildProcess } from 'child_process';
import { createInterface } from 'readline';
import type {
  Answer, AskRequest, DuelState, EngineEvent, Hello, Hints, InitOptions, PlayerSnapshot, Prompt, PromptContext, QueueName, Queues,
  StepResult,
} from './types';

export * from './types';

export const PROTOCOL_VERSION = 1;

export class BridgeError extends Error {
  constructor(public readonly code: string, message: string) {
    super(message);
    this.name = 'BridgeError';
  }
}

export interface BridgeOptions {
  /** The ocgcore-bridge binary (default: $OCGCORE_BRIDGE_BIN, else "ocgcore-bridge" on PATH). */
  binary?: string;
  /** The card scripts directory (CardScripts). */
  scripts?: string;
  /** BabelCDB / EDOPro .cdb card databases. */
  cdb?: string[];
  /** A YGOPRODeck-style JSON card list, with its setcodes supplement and archetype constants. */
  cardsJson?: string;
  setcodes?: string;
  archetypes?: string;
  /** Extra command-line arguments. */
  args?: string[];
  env?: NodeJS.ProcessEnv;
  /** Per request (default 30000 ms). A request that times out rejects; the process keeps running. */
  timeoutMs?: number;
  /**
   * The ask hook (init responder.ask): return an Answer, or null for the
   * default answer. Without it every ask gets the default.
   */
  onAsk?: (ask: AskRequest) => Answer | null | undefined | Promise<Answer | null | undefined>;
  /**
   * Engine messages subscribed with init `events.subscribe`, as they happen
   * (before the reply of the request that caused them). `session` is the
   * fork's, undefined for the main duel.
   */
  onEvent?: (event: EngineEvent, session?: string) => void;
  /** Where the binary's log (stderr) goes: inherited, ignored, or a line callback (default: ignored). */
  stderr?: 'inherit' | 'ignore' | ((line: string) => void);
}

interface Pending {
  resolve: (v: unknown) => void;
  reject: (e: Error) => void;
  timer?: NodeJS.Timeout;
}

export interface RespondOptions {
  /** What runs after the answer: "advance" (default), "process" or "none". */
  then?: 'advance' | 'process' | 'none';
  max?: number;
  hints?: Hints;
}

export interface AdvanceOptions {
  max?: number;
  answerFirst?: boolean;
  hints?: Hints;
}

/** One request of a batch: a request's type and fields, without `id` or `session`. */
export interface BatchRequest {
  type: string;
  /** Runs only when the request before it ran and replied with this status (or one of these). */
  if?: StepResult['status'] | Array<StepResult['status']>;
  [field: string]: unknown;
}

/** One request's outcome in a batch: its result (without `state`), its error, or skipped. */
export type BatchEntry =
  | { ok: true; result: Record<string, any>; skipped?: undefined }
  | { ok: true; skipped: true; result?: undefined }
  | { ok: false; error: { code: string; message: string } };

export interface BatchResult {
  /** One entry per request, up to and including the first that failed. */
  results: BatchEntry[];
  /** The duel's state after the batch. */
  state: DuelState;
}

/**
 * One duel: the main session of a bridge process, or a fork of it (a copy of
 * the process at the moment of the fork, driven independently).
 */
export class Duel {
  constructor(protected readonly bridge: Bridge, readonly session?: string) {}

  protected call<T>(type: string, fields: object = {}): Promise<T> {
    return this.bridge.request<T>(type, fields, this.session);
  }

  hello(): Promise<Hello> { return this.call('hello'); }
  /** Starts a duel (any previous one in this session is destroyed). */
  async init(opts: InitOptions): Promise<DuelState> { return (await this.call<{ state: DuelState }>('init', opts)).state; }
  async state(): Promise<DuelState> { return (await this.call<{ state: DuelState }>('state')).state; }
  /** The pending prompt, or null. */
  async pending(): Promise<Prompt | null> { return (await this.state()).pending ?? null; }
  respond(answer: Answer, opts: RespondOptions = {}): Promise<StepResult> {
    return this.call('respond', { answer, ...opts });
  }
  async process(): Promise<DuelState> { return (await this.call<{ state: DuelState }>('process')).state; }
  advance(opts: AdvanceOptions = {}): Promise<StepResult> { return this.call('advance', opts); }
  autoAnswer(hints?: Hints): Promise<StepResult> { return this.call('autoAnswer', hints ? { hints } : {}); }
  /** The response bytes (hex) an answer encodes to, checked against the pending prompt. */
  async encode(answer: Answer): Promise<string> { return (await this.call<{ raw: string }>('encode', { answer })).raw; }
  async snapshot(): Promise<[PlayerSnapshot, PlayerSnapshot]> {
    return (await this.call<{ players: [PlayerSnapshot, PlayerSnapshot] }>('snapshot')).players;
  }
  context(player?: number): Promise<PromptContext> { return this.call('context', player === undefined ? {} : { player }); }
  take(queues?: QueueName[]): Promise<Queues> { return this.call('take', queues ? { queues } : {}); }
  /** A named query (built in: "card", "fieldCard", "chain"; a server of your own adds more). */
  async query<T = unknown>(name: string, args: object = {}): Promise<T> {
    return (await this.call<{ value: T }>('query', { name, args })).value;
  }
  /**
   * A copy of this duel in a new process (asks are off in forks: their prompts take the defaults).
   * `detach`: the copy is a session of the bridge process itself, not of this fork: its requests
   * go to it directly and it lives on when this fork is destroyed.
   */
  async fork(opts: { chainStartSnapshots?: boolean; detach?: boolean } = {}): Promise<Duel> {
    const { session, detached } = await this.call<{ session: string; detached?: boolean }>('fork', opts);
    if (detached) return new Duel(this.bridge, session);
    return new Duel(this.bridge, this.session ? `${this.session}/${session.split('/').pop()}` : session);
  }
  /**
   * Several requests in one round trip, run in order as if sent one by one
   * (fork, batch and exit cannot be batched).
   */
  batch(requests: BatchRequest[]): Promise<BatchResult> { return this.call('batch', { requests }); }
  /** Ends this session's duel; a fork's process exits. */
  async destroy(): Promise<void> { await this.call('destroy'); }
}

export class Bridge extends Duel {
  private proc: ChildProcess;
  private waiting = new Map<number, Pending>();
  private nextId = 1;
  private closed = false;
  private readyPromise: Promise<{ protocol: number; version: string }>;
  private readonly timeoutMs: number;

  private constructor(private readonly opts: BridgeOptions) {
    super(undefined as unknown as Bridge);
    (this as unknown as { bridge: Bridge }).bridge = this;
    this.timeoutMs = opts.timeoutMs ?? 30_000;
    const args: string[] = [];
    if (opts.scripts) args.push('--scripts', opts.scripts);
    for (const c of opts.cdb ?? []) args.push('--cdb', c);
    if (opts.cardsJson) args.push('--cards-json', opts.cardsJson);
    if (opts.setcodes) args.push('--setcodes', opts.setcodes);
    if (opts.archetypes) args.push('--archetypes', opts.archetypes);
    args.push(...(opts.args ?? []));
    const binary = opts.binary ?? process.env.OCGCORE_BRIDGE_BIN ?? 'ocgcore-bridge';
    const stderr = opts.stderr ?? 'ignore';
    this.proc = spawn(binary, args, {
      stdio: ['pipe', 'pipe', stderr === 'inherit' ? 'inherit' : typeof stderr === 'function' ? 'pipe' : 'ignore'],
      env: opts.env ?? process.env,
    });
    if (typeof stderr === 'function' && this.proc.stderr) {
      createInterface({ input: this.proc.stderr }).on('line', stderr);
    }
    let onReady!: (v: { protocol: number; version: string }) => void;
    let onFail!: (e: Error) => void;
    this.readyPromise = new Promise((resolve, reject) => { onReady = resolve; onFail = reject; });
    createInterface({ input: this.proc.stdout! }).on('line', (line) => {
      let msg: any;
      try { msg = JSON.parse(line); } catch { return; }
      if (msg.type === 'ready') { onReady({ protocol: msg.protocol, version: msg.version }); return; }
      if (msg.type === 'ask') { void this.answerAsk(msg); return; }
      if (msg.type === 'event') {
        try { this.opts.onEvent?.(msg.event, msg.session); } catch { /* a throwing handler does not stop the client */ }
        return;
      }
      const p = typeof msg.id === 'number' ? this.waiting.get(msg.id) : undefined;
      if (!p) return;
      this.waiting.delete(msg.id);
      if (p.timer) clearTimeout(p.timer);
      if (msg.ok) p.resolve(msg.result);
      else p.reject(new BridgeError(msg.error?.code ?? 'error', msg.error?.message ?? 'request failed'));
    });
    const fail = (err: Error) => {
      this.closed = true;
      onFail(err);
      for (const p of this.waiting.values()) { if (p.timer) clearTimeout(p.timer); p.reject(err); }
      this.waiting.clear();
    };
    this.proc.on('error', (err) => fail(err));
    // A write to a process that is gone (EPIPE) fails the bridge instead of
    // throwing out of the event loop.
    this.proc.stdin?.on('error', (err) => fail(err));
    this.proc.on('exit', (code, signal) => fail(new BridgeError('exited', `ocgcore-bridge exited (code ${code}, signal ${signal})`)));
  }

  /** Spawns the binary and waits for it to be ready. */
  static async start(opts: BridgeOptions = {}): Promise<Bridge> {
    const b = new Bridge(opts);
    const ready = await b.readyPromise;
    if (ready.protocol !== PROTOCOL_VERSION) {
      b.close();
      throw new BridgeError('protocol', `ocgcore-bridge speaks protocol ${ready.protocol}, this client ${PROTOCOL_VERSION}`);
    }
    return b;
  }

  /** Loads more card data (a .cdb, or a YGOPRODeck JSON list). */
  loadCards(source: { cdb?: string | string[]; json?: string; setcodes?: string; archetypes?: string; scripts?: string }): Promise<{ added: number; cards: number }> {
    return this.request('loadCards', source);
  }

  request<T>(type: string, fields: object = {}, session?: string): Promise<T> {
    if (this.closed) return Promise.reject(new BridgeError('closed', 'the bridge process is gone'));
    const id = this.nextId++;
    return new Promise<T>((resolve, reject) => {
      const p: Pending = { resolve: resolve as (v: unknown) => void, reject };
      p.timer = setTimeout(() => {
        if (!this.waiting.delete(id)) return;
        reject(new BridgeError('timeout', `${type} got no reply in ${this.timeoutMs} ms`));
      }, this.timeoutMs);
      p.timer.unref?.();
      this.waiting.set(id, p);
      const msg = { ...fields, id, type, ...(session ? { session } : {}) };
      if (!this.write(msg)) {
        this.waiting.delete(id);
        clearTimeout(p.timer);
        reject(new BridgeError('closed', 'the bridge process is gone'));
      }
    });
  }

  private write(msg: object): boolean {
    if (this.closed || !this.proc.stdin?.writable) return false;
    try {
      this.proc.stdin.write(JSON.stringify(msg) + '\n');
      return true;
    } catch {
      return false;
    }
  }

  private async answerAsk(msg: AskRequest): Promise<void> {
    let answer: Answer | null | undefined = null;
    if (this.opts.onAsk) {
      try { answer = await this.opts.onAsk(msg); } catch { answer = null; }
    }
    this.write({ type: 'answer', ask: msg.ask, answer: answer ?? null });
  }

  /** Ends the process (forks end with it). */
  close(): void {
    if (this.closed) return;
    this.write({ type: 'exit', id: 0 });
    this.proc.stdin?.end();
    this.closed = true;
    const t = setTimeout(() => this.proc.kill(), 2000);
    t.unref?.();
  }
}
