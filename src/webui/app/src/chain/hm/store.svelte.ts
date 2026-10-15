// The state of the markets, kept up to date while the app runs: what the node says (gethivemindinfo, the
// decisions, the markets, the holders of reputation) and what the wallet holds (its shares, its
// reputation), asked again with every block of this chain, one refresh at a time.
//
// And a journal of what the chain does not keep, in files of the data directory of the network (host
// methods hivemind.store.*, src/webui/local.cpp): the answers of the votes this app sealed (the wallet needs
// the answer again to reveal it, as chains-qt kept it in QSettings), in a file of their own, written at once;
// and the votes revealed, the trades it made (for the profit and loss), the prices of the markets as it saw
// them (the charts), the reputation of each wallet over time, and the reminders it gave. (In the preferences
// of the app, capped at 256 kB, a full journal could no longer be saved and the seals were lost: audit port-1
// #1. A journal left there by an older version is moved to the files.)
import { request } from '../../lib/bridge';
import { errorText, inform } from '../../lib/dialogs.svelte';
import { t } from '../../lib/i18n.svelte';
import { every } from '../../lib/poll';
import { host, rpc } from '../../lib/rpc';
import { node, toast, wallets } from '../../lib/state.svelte';
import { voteStage, type Decision, type Holding, type HivemindInfo, type Market, type TradeRecord, type WalletPosition } from './model';

export interface SealRecord {
  wallet: string;
  account: string;
  decision: string;
  /** The answer sealed: "yes", "no", "unknown" or a number of the scale. */
  answer: string;
  /** The period in whose reveal phase it is revealed. */
  close: number;
  time: number;
  txid: string;
  /** Sealed in a ballot (sealballot): revealed with the others of the ballot. */
  ballot?: boolean;
}

export interface VoteRecord {
  wallet: string;
  account: string;
  decision: string;
  /** The question, kept: a resolved decision leaves the chain's state after a while. Text of the chain. */
  text: string;
  answer: string;
  time: number;
  txid: string;
  /** Once resolved: the answer of the chain, and whether it agreed. */
  outcome?: string;
  agreed?: boolean;
}

export interface PricePoint {
  /** The height of this chain. */
  h: number;
  /** Seconds. */
  t: number;
  /** The price of each state, in ten-thousandths. */
  p: number[];
}

export interface RepPoint {
  h: number;
  t: number;
  amount: number;
}

export interface Journal {
  sealed: Record<string, SealRecord>;
  votes: VoteRecord[];
  trades: TradeRecord[];
  prices: Record<string, PricePoint[]>;
  rep: Record<string, RepPoint[]>;
  /** Desktop notifications when a vote is due. */
  reminders: boolean;
  /** The reminders given, so that each is given once. */
  notified: string[];
  /** The decisions this wallet listed and the markets it opened (ids), by wallet. */
  mine: Record<string, string[]>;
}

/** Sizes the journal is kept to (a few hundred kB at most; the seals are not capped). */
const MAX_VOTES = 200;
const MAX_TRADES = 400;
const MAX_POINTS = 60;
const MAX_PRICED_MARKETS = 40;
const MAX_REP_POINTS = 120;
const MAX_NOTIFIED = 300;
const MAX_MINE = 200;

export function emptyJournal(): Journal {
  return { sealed: {}, votes: [], trades: [], prices: {}, rep: {}, reminders: true, notified: [], mine: {} };
}

export const hm = $state({
  info: null as HivemindInfo | null,
  decisions: [] as Decision[],
  markets: [] as Market[],
  /** Of the wallet loaded. */
  positions: [] as WalletPosition[],
  reputation: [] as Holding[],
  /** Every holder of reputation, largest first. */
  holders: [] as Holding[],
  /** getdecision of the decisions being voted on: their votes. */
  details: {} as Record<string, Decision>,
  /** The wallet the positions and reputation are of. */
  wallet: null as string | null,
  /** The height the above are of. */
  height: -1,
  /** Why the node could not be asked (not running as a sidechain, starting). */
  error: '',
  loaded: false,
  journal: emptyJournal(),
  /**
   * The decisions and markets whose author is an address of the wallet loaded (getaddressinfo: ismine), as chains-qt
   * told them: also those listed from the console, from another machine, or before the journal was lost.
   */
  authored: [] as string[],
  /** Counts the refreshes. */
  version: 0,
});

/** A trade to show when a market opens (Portfolio: "Sell…" opens the market on the Sell side of the outcome held). */
export const tradeIntent = $state({ market: '', state: -1, side: 'buy' as 'buy' | 'sell' });

/** Open a market on a side of an outcome. */
export function openTrade(market: string, state: number, side: 'buy' | 'sell'): void {
  tradeIntent.market = market;
  tradeIntent.state = state;
  tradeIntent.side = side;
}

/** Whether an address is the wallet's, by wallet and address (asked once each). */
const ownAddress = new Map<string, boolean>();

/** The ids of the decisions and markets whose author is an address of the wallet (audit port-1 #7). */
async function authoredBy(wallet: string, items: readonly { id: string; author?: string }[]): Promise<string[]> {
  const unknown = [...new Set(items.map((x) => x.author).filter((a): a is string => !!a && !ownAddress.has(`${wallet}|${a}`)))];
  // A few at a time: a chain of many authors asks the wallet once for each, then never again.
  for (let i = 0; i < unknown.length; i += 8) {
    await Promise.all(
      unknown.slice(i, i + 8).map((a) =>
        rpc
          .raw<{ ismine?: boolean }>('getaddressinfo', [a], { wallet })
          .then((r) => ownAddress.set(`${wallet}|${a}`, !!r?.ismine))
          .catch(() => undefined),
      ),
    );
  }
  return items.filter((x) => !!x.author && ownAddress.get(`${wallet}|${x.author}`) === true).map((x) => x.id);
}

/** What the wallet loaded listed or opened: by its author on the chain, and what this app recorded. */
export function ownItems(wallet: string | null): Set<string> {
  if (wallet === null) return new Set();
  return new Set([...(hm.wallet === wallet ? hm.authored : []), ...(hm.journal.mine[wallet] ?? [])]);
}

/** The decisions by id. */
export function decisionMap(): Map<string, Decision> {
  return new Map(hm.decisions.map((d) => [d.id, d]));
}

let running = false;
let again = false;
const afterRefresh: (() => unknown)[] = [];

/** Run `fn` after every refresh (the jobs of the Create wizard that wait for a block). */
export function onRefresh(fn: () => unknown): void {
  if (!afterRefresh.includes(fn)) afterRefresh.push(fn);
}
let journalLoaded = false;
let saveTimer: ReturnType<typeof setTimeout> | null = null;
/** Whether the user was told that the journal cannot be saved (once). */
let journalWarned = false;

/** The stores of the host: the seals, the rest of the journal (null when none). */
interface Stores {
  seals: { sealed?: unknown } | null;
  journal: unknown;
}

/** Read the journal from the files of the host (once); one an older version left in the preferences is moved there. */
export async function loadJournal(): Promise<void> {
  if (journalLoaded) return;
  journalLoaded = true;
  let stores: Stores | null = null;
  try {
    stores = await request<Stores>('host', 'hivemind.store.load', {});
  } catch {
    stores = null;
  }
  let fromPrefs: unknown = null;
  if (!stores || (stores.journal === null && stores.seals === null)) {
    try {
      fromPrefs = (await host.prefs()).hivemind ?? null;
    } catch {
      fromPrefs = null;
    }
  }
  if (fromPrefs) {
    hm.journal = sanitizeJournal(fromPrefs);
    if (stores) {
      // Moved: the seals and the journal to their files, then out of the preferences.
      const moved = (await saveSeals()) && (await writeJournal());
      if (moved) host.setPrefs({ hivemind: null }).catch(() => undefined);
    }
    return;
  }
  const journal = sanitizeJournal(stores?.journal ?? null);
  journal.sealed = sanitizeJournal({ sealed: stores?.seals?.sealed ?? {} }).sealed;
  hm.journal = journal;
}

/** A journal read back, with what is not of the expected shape left out. */
export function sanitizeJournal(raw: unknown): Journal {
  const j = emptyJournal();
  if (!raw || typeof raw !== 'object') return j;
  const r = raw as Partial<Journal>;
  const str = (v: unknown) => typeof v === 'string';
  if (r.sealed && typeof r.sealed === 'object') {
    for (const [k, v] of Object.entries(r.sealed)) {
      if (v && str(v.wallet) && str(v.account) && str(v.decision) && str(v.answer) && typeof v.close === 'number') j.sealed[k] = v;
    }
  }
  if (Array.isArray(r.votes)) j.votes = r.votes.filter((v) => v && str(v.decision) && str(v.answer) && str(v.wallet)).slice(0, MAX_VOTES);
  if (Array.isArray(r.trades)) j.trades = r.trades.filter((x) => x && str(x.market) && typeof x.shares === 'number' && typeof x.coins === 'number' && str(x.wallet)).slice(-MAX_TRADES);
  if (r.prices && typeof r.prices === 'object') {
    for (const [k, v] of Object.entries(r.prices)) if (Array.isArray(v)) j.prices[k] = v.filter((p) => p && typeof p.h === 'number' && Array.isArray(p.p)).slice(-MAX_POINTS);
  }
  if (r.rep && typeof r.rep === 'object') {
    for (const [k, v] of Object.entries(r.rep)) if (Array.isArray(v)) j.rep[k] = v.filter((p) => p && typeof p.amount === 'number').slice(-MAX_REP_POINTS);
  }
  if (typeof r.reminders === 'boolean') j.reminders = r.reminders;
  if (Array.isArray(r.notified)) j.notified = r.notified.filter(str).slice(-MAX_NOTIFIED);
  if (r.mine && typeof r.mine === 'object') {
    for (const [k, v] of Object.entries(r.mine)) if (Array.isArray(v)) j.mine[k] = v.filter(str).slice(-MAX_MINE);
  }
  return j;
}

/** Write the journal but the seals; false (and the user told, once) when it cannot be. */
async function writeJournal(): Promise<boolean> {
  const { sealed: _sealed, ...rest } = $state.snapshot(hm.journal);
  void _sealed;
  try {
    await request('host', 'hivemind.store.journal', { value: rest });
    journalWarned = false;
    return true;
  } catch (e) {
    if (!journalWarned) {
      journalWarned = true;
      toast('warning', t('The journal of Hivemind could not be saved'), t('Your trades and the charts of the prices may be missing after a restart: %1', errorText(e)), 15000);
    }
    return false;
  }
}

/** Write the journal back, a moment after the last change. */
export function saveJournal(): void {
  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    saveTimer = null;
    void writeJournal();
  }, 400);
}

/**
 * Write the answers of the sealed votes now, in their own file. False when they cannot be: the caller says
 * so, since a vote whose answer is lost cannot be revealed and does not count.
 */
export async function saveSeals(): Promise<boolean> {
  try {
    await request('host', 'hivemind.store.seals', { value: { sealed: $state.snapshot(hm.journal.sealed) } });
    return true;
  } catch {
    return false;
  }
}

export function sealKey(wallet: string, account: string, decision: string): string {
  return `${wallet}|${account}|${decision}`;
}

/**
 * Remember sealed answers, to reveal them later: written at once. When they cannot be, the user is told, with
 * the answers, which are needed to reveal the votes (the voting center can reveal with answers given by hand).
 */
export async function noteSealed(...recs: SealRecord[]): Promise<boolean> {
  for (const rec of recs) hm.journal.sealed[sealKey(rec.wallet, rec.account, rec.decision)] = rec;
  if (await saveSeals()) return true;
  const answers = recs.map((r) => `${r.decision.slice(0, 12)}…: ${r.answer}`).join('\n');
  await inform(t('The answer of your vote could not be saved'), t('Write it down: it is needed to reveal the vote in period %1, or the vote does not count.', String(recs[0]?.close ?? '')), 'error', answers);
  return false;
}

/** A vote revealed: from the sealed ones to the votes of the wallet. */
export function noteRevealed(wallet: string, account: string, decision: string, answer: string, txid: string): void {
  const key = sealKey(wallet, account, decision);
  if (key in hm.journal.sealed) {
    delete hm.journal.sealed[key];
    void saveSeals();
  }
  const text = hm.decisions.find((d) => d.id === decision)?.text ?? '';
  hm.journal.votes = [{ wallet, account, decision, text, answer, time: Math.floor(Date.now() / 1000), txid }, ...hm.journal.votes.filter((v) => !(v.wallet === wallet && v.account === account && v.decision === decision))].slice(0, MAX_VOTES);
  saveJournal();
}

export function noteTrade(rec: TradeRecord): void {
  hm.journal.trades = [...hm.journal.trades, rec].slice(-MAX_TRADES);
  saveJournal();
}

/** A decision listed or a market opened by this wallet. */
export function noteMine(wallet: string, id: string): void {
  const list = hm.journal.mine[wallet] ?? [];
  if (!list.includes(id)) hm.journal.mine[wallet] = [...list, id].slice(-MAX_MINE);
  saveJournal();
}

export function setReminders(on: boolean): void {
  hm.journal.reminders = on;
  saveJournal();
}

/** The trades recorded for a position of the wallet. */
export function tradesOf(wallet: string | null, market: string, state: number): TradeRecord[] {
  return hm.journal.trades.filter((x) => x.wallet === wallet && x.market === market && x.state === state);
}

/** The prices of a market as this app saw them, oldest first, ending with now. */
export function priceSeries(market: Market): PricePoint[] {
  const points = hm.journal.prices[market.id] ?? [];
  return points;
}

/** Note the prices of the markets that trade, when they moved (the charts). */
export function notePrices(markets: readonly Market[], height: number, now: number): boolean {
  let changed = false;
  for (const m of markets) {
    if (m.status !== 'trading' || m.states.some((s) => s.price === undefined)) continue;
    const p = m.states.map((s) => Math.round((s.price ?? 0) * 10000));
    const list = hm.journal.prices[m.id] ?? [];
    const last = list[list.length - 1];
    if (last && last.h >= height) continue;
    if (last && last.p.length === p.length && last.p.every((x, i) => x === p[i])) continue;
    // The start: a market opens at even odds.
    const start = list.length === 0 && m.height < height ? [{ h: m.height, t: now, p: p.map(() => Math.round(10000 / p.length)) }] : [];
    hm.journal.prices[m.id] = [...list, ...start, { h: height, t: now, p }].slice(-MAX_POINTS);
    changed = true;
  }
  // Keep the markets still on the chain, the latest first.
  const ids = Object.keys(hm.journal.prices);
  if (ids.length > MAX_PRICED_MARKETS) {
    const live = new Set(markets.map((m) => m.id));
    const keep = ids
      .sort((a, b) => Number(live.has(b)) - Number(live.has(a)) || (hm.journal.prices[b].at(-1)?.h ?? 0) - (hm.journal.prices[a].at(-1)?.h ?? 0))
      .slice(0, MAX_PRICED_MARKETS);
    for (const id of ids) if (!keep.includes(id)) delete hm.journal.prices[id];
    changed = true;
  }
  return changed;
}

/** Note the reputation of the wallet, when it changed. */
export function noteReputation(wallet: string, amount: number, height: number, now: number): boolean {
  const list = hm.journal.rep[wallet] ?? [];
  const last = list[list.length - 1];
  if (last && Math.abs(last.amount - amount) < 1e-9) return false;
  hm.journal.rep[wallet] = [...list, { h: height, t: now, amount }].slice(-MAX_REP_POINTS);
  return true;
}

/** The outcome of the votes of the wallet, once their decisions are resolved. */
export function noteOutcomes(decisions: readonly Decision[]): boolean {
  let changed = false;
  const byId = new Map(decisions.map((d) => [d.id, d]));
  hm.journal.votes = hm.journal.votes.map((v) => {
    if (v.outcome !== undefined) return v;
    const d = byId.get(v.decision);
    if (!d || d.status !== 'resolved' || d.answer === undefined) return v;
    changed = true;
    const outcome = String(d.answer);
    const agreed = v.answer === 'unknown' ? false : d.scaled ? Math.abs(Number(v.answer) - Number(d.answer)) <= Math.max(1, ((d.max ?? 0) - (d.min ?? 0)) * 0.05) : v.answer === outcome;
    return { ...v, outcome, agreed };
  });
  // Seals of decisions resolved, or no longer on the chain, are no use any more.
  let sealsChanged = false;
  for (const [k, s] of Object.entries(hm.journal.sealed)) {
    const d = byId.get(s.decision);
    if (!d || d.status === 'resolved') {
      delete hm.journal.sealed[k];
      sealsChanged = true;
    }
  }
  if (sealsChanged) void saveSeals();
  return changed;
}

/** The accounts of the wallet that hold reputation, by address. */
export function walletAccounts(): string[] {
  return hm.reputation.map((r) => r.account);
}

/**
 * Reminders: a vote this wallet can cast, a sealed vote to reveal. Once each. The notification names no
 * text of the chain: a desktop may read markup in the body of a notification.
 */
export function remind(now: number): void {
  if (!hm.journal.reminders || !hm.info || hm.wallet === null || hm.reputation.length === 0) return;
  const info = hm.info;
  const notified = new Set(hm.journal.notified);
  const give = (key: string, title: string, body: string) => {
    if (notified.has(key)) return;
    notified.add(key);
    hm.journal.notified = [...hm.journal.notified, key].slice(-MAX_NOTIFIED);
    host.notify(title, body, 'info').catch(() => undefined);
  };
  for (const d of hm.decisions) {
    const stage = voteStage(d, info);
    const sealed = Object.values(hm.journal.sealed).filter((s) => s.wallet === hm.wallet && s.decision === d.id);
    if (stage === 'reveal' && sealed.length) give(`reveal|${hm.wallet}|${d.id}|${d.closeperiod}`, t('Time to reveal your vote'), t('Reveal your sealed votes in the voting center before period %1 ends, or they do not count.', String(d.closeperiod)));
    else if (stage === 'seal' && !sealed.length) give(`seal|${hm.wallet}|${d.id}|${d.closeperiod}`, t('A decision to vote on'), t('A decision is up for the vote: seal your answer in the voting center before the reveal phase of period %1.', String(d.closeperiod)));
  }
  void now;
  saveJournal();
}

/** Ask the node again: at most one refresh at a time, and one more if asked meanwhile. */
export async function refreshHivemind(): Promise<void> {
  if (running) {
    again = true;
    return;
  }
  running = true;
  try {
    do {
      again = false;
      await refreshOnce();
    } while (again);
  } finally {
    running = false;
  }
}

async function refreshOnce(): Promise<void> {
  await loadJournal();
  const wallet = wallets.current;
  const height = node.tip?.height ?? -1;
  const now = Math.floor(Date.now() / 1000);
  try {
    const [info, decisionsRaw, marketsRaw, holdersRaw] = await Promise.all([
      rpc.raw<HivemindInfo>('gethivemindinfo'),
      rpc.raw<Decision[]>('listdecisions'),
      rpc.raw<Market[]>('listmarkets'),
      rpc.raw<Holding[]>('listreputation', [100]),
    ]);
    // Lists, whatever a node that is not Hivemind's answers.
    const decisions = Array.isArray(decisionsRaw) ? decisionsRaw : [];
    const markets = Array.isArray(marketsRaw) ? marketsRaw : [];
    const holders = Array.isArray(holdersRaw) ? holdersRaw : [];
    if (!info) throw new Error(t('This node does not run Hivemind.'));
    if (!hm.loaded) {
      // The first time: the markets and the decisions at once; what the wallet holds, the votes and the authors
      // follow (the first visit of Markets waited for all of it: audit port-1 #10).
      hm.info = info;
      hm.decisions = decisions;
      hm.markets = markets;
      hm.holders = holders;
      hm.height = height;
      hm.error = '';
      hm.loaded = true;
      hm.version++;
    }
    let positions: WalletPosition[] = [];
    let reputation: Holding[] = [];
    if (wallet !== null) {
      [positions, reputation] = await Promise.all([
        rpc.raw<WalletPosition[]>('getwalletpositions', [], { wallet }).catch(() => [] as WalletPosition[]),
        rpc.raw<Holding[]>('getwalletreputation', [], { wallet }).catch(() => [] as Holding[]),
      ]);
    }
    // The votes on the decisions being voted on.
    const details: Record<string, Decision> = {};
    const voting = decisions.filter((d) => d.status === 'voting').slice(0, 100);
    await Promise.all(
      voting.map((d) =>
        rpc
          .raw<Decision>('getdecision', [d.id])
          .then((full) => (details[d.id] = full))
          .catch(() => undefined),
      ),
    );
    const authored = wallet !== null ? await authoredBy(wallet, [...decisions, ...markets]) : [];
    hm.authored = authored;
    hm.info = info;
    hm.decisions = decisions;
    hm.markets = markets;
    hm.holders = holders;
    hm.positions = positions;
    hm.reputation = reputation;
    hm.details = details;
    hm.wallet = wallet;
    hm.height = height;
    hm.error = '';
    hm.loaded = true;
    let changed = notePrices(markets, height, now);
    if (wallet !== null) changed = noteReputation(wallet, reputation.reduce((n, r) => n + r.amount, 0), height, now) || changed;
    changed = noteOutcomes(decisions) || changed;
    if (changed) saveJournal();
    remind(now);
    for (const fn of afterRefresh) void Promise.resolve(fn()).catch(() => undefined);
  } catch (e) {
    hm.error = e instanceof Error ? e.message : String(e);
    hm.loaded = true;
  }
  hm.version++;
}

let lastTip = '';
let lastWallet: string | null | undefined;
let timer: (() => void) | null = null;

/**
 * Follow the chain: a refresh with every new block and with a change of wallet, and every 15 seconds (a
 * period counted in time turns without a block). The pages and the cards call it; the first starts it.
 */
export function startHivemind(): void {
  const tick = () => {
    const tip = node.tip?.hash ?? '';
    if (tip !== lastTip || wallets.current !== lastWallet) {
      lastTip = tip;
      lastWallet = wallets.current;
      void refreshHivemind();
    }
  };
  tick();
  if (timer) return;
  let ticks = 0;
  // Resting while the window is hidden, as the timers of the pages (lib/poll.ts).
  timer = every(3000, () => {
    tick();
    // Periods counted in time turn without a block: a look every 15 seconds.
    if (++ticks % 5 === 0) void refreshHivemind();
  });
}

/** For the tests. */
export function resetHivemind(): void {
  timer?.();
  timer = null;
  lastTip = '';
  lastWallet = undefined;
  journalLoaded = false;
  journalWarned = false;
  ownAddress.clear();
  Object.assign(hm, { info: null, decisions: [], markets: [], positions: [], reputation: [], holders: [], details: {}, wallet: null, height: -1, error: '', loaded: false, journal: emptyJournal(), authored: [], version: 0 });
}
