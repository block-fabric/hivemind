// Hivemind in the app: what the node says of the markets (src/rpc/hivemind.cpp, src/wallet/rpc/hivemind.cpp)
// as types, and what the pages work out of it without asking the node: the market maker (LMSR) for the
// previews and charts, the names of the states of a market, the clock of the voting periods, the checks
// of the forms. Pure functions: test/hivemind.test.ts tries them.
import { t, tn } from '../../lib/i18n.svelte';
import { niceTimeOffset } from '../../lib/format';

// --- What the node says ---

/** gethivemindinfo. */
export interface HivemindInfo {
  period: number;
  phase: 'seal' | 'reveal';
  /** Periods counted in mainchain time (the main and test networks)... */
  periodseconds?: number;
  starttime?: number;
  /** ...or in blocks of this chain (regtest). */
  periodblocks?: number;
  decisions: number;
  markets: number;
  voters: number;
  reputation: number;
  minreputation: number;
  voterpot: number;
  listingfee: number;
  maxvoteperiods: number;
  maxperiodsahead: number;
}

export type DecisionStatus = 'open' | 'voting' | 'resolved';

/** listdecisions, getdecision. */
export interface Decision {
  id: string;
  text: string;
  author: string;
  scaled: boolean;
  min?: number;
  max?: number;
  eventperiod: number;
  voteperiods: number;
  closeperiod: number;
  rolls: number;
  height: number;
  status: DecisionStatus;
  outcome?: number;
  answer?: string | number;
  votes?: { voter: string; revealed: boolean; answer?: string | number }[];
}

export type MarketStatus = 'trading' | 'closed' | 'settled';

export interface MarketState {
  state: number;
  /** Y or N for each decision of the market, the first decision first. */
  answers: string;
  shares: number;
  price?: number;
}

/** listmarkets, getmarket. */
export interface Market {
  id: string;
  text: string;
  author: string;
  decisions: string[];
  liquidity: number;
  feebps: number;
  height: number;
  status: MarketStatus;
  tradinguntil?: number;
  treasury: number;
  fees: number;
  states: MarketState[];
  positions?: { account: string; state: number; shares: number }[];
}

/** getwalletpositions. */
export interface WalletPosition {
  market: string;
  account: string;
  state: number;
  shares: number;
}

/** getwalletreputation, listreputation. */
export interface Holding {
  account: string;
  amount: number;
}

// --- Limits of the chain (consensus parameters gethivemindinfo does not give) ---

export const LIMITS = {
  /** The longest question or title, in bytes (max_text_size). */
  maxText: 400,
  /** The highest trading fee, in hundredths of a percent (max_fee_bps). */
  maxFeeBps: 1000,
  /** The least liquidity of a market maker, in coins (min_liquidity). */
  minLiquidity: 0.01,
  /** The most decisions a market is about (max_market_decisions): 2^4 = 16 states. */
  maxMarketDecisions: 4,
  /** The least a purchase that opens a position pays (MIN_POSITION). */
  minPosition: 0.001,
  /** What a payout keeps for the voters (PAYOUT_FEE). */
  payoutFee: 0.00001,
} as const;

/** The fee a market takes by default (createmarket), in hundredths of a percent. */
export const DEFAULT_FEE_BPS = 50;

// --- The market maker: C(q) = b ln(sum exp(q_i / b)) ---

/** What the shares sold so far are worth to the maker, in coins (log-sum-exp, kept stable). */
export function lmsrCost(shares: readonly number[], b: number): number {
  if (b <= 0 || shares.length === 0) return 0;
  const top = Math.max(...shares.map((q) => q / b));
  return b * (top + Math.log(shares.reduce((n, q) => n + Math.exp(q / b - top), 0)));
}

/** The price of each state: the chance the market gives it. */
export function lmsrPrices(shares: readonly number[], b: number): number[] {
  if (b <= 0 || shares.length === 0) return shares.map(() => 0);
  const top = Math.max(...shares.map((q) => q / b));
  const e = shares.map((q) => Math.exp(q / b - top));
  const sum = e.reduce((a, x) => a + x, 0);
  return e.map((x) => x / sum);
}

/** The most the maker can lose, which the author funds: b ln(states). */
export function lmsrFunding(b: number, states: number): number {
  return b * Math.log(Math.max(1, states));
}

export interface TradePreview {
  /** Shares: positive to buy, negative to sell. */
  shares: number;
  /** C(after) - C(before): positive when it costs, negative when it pays. */
  cost: number;
  fee: number;
  /** What the trader pays (buying) or gets (selling), fee included: always positive. */
  total: number;
  /** Coins per share, fee included. */
  average: number;
  priceBefore: number;
  priceAfter: number;
  /** How far the average is from the price before, as a fraction of it. */
  impact: number;
  /** All the prices after the trade. */
  pricesAfter: number[];
}

/** A trade worked out as the chain does (State::Quote), in floating point: a preview, the node quotes the exact cost. */
export function previewTrade(market: Pick<Market, 'states' | 'liquidity' | 'feebps'>, state: number, shares: number): TradePreview | null {
  const q = market.states.map((s) => s.shares);
  if (state < 0 || state >= q.length || shares === 0 || !Number.isFinite(shares)) return null;
  const after = q.slice();
  after[state] += shares;
  if (after[state] < 0) return null;
  const b = market.liquidity;
  const cost = lmsrCost(after, b) - lmsrCost(q, b);
  const fee = (Math.abs(cost) * market.feebps) / 10000;
  const total = shares > 0 ? cost + fee : -cost - fee;
  const before = lmsrPrices(q, b);
  const pricesAfter = lmsrPrices(after, b);
  const average = total / Math.abs(shares);
  const priceBefore = before[state];
  return { shares, cost, fee, total, average, priceBefore, priceAfter: pricesAfter[state], impact: priceBefore > 0 ? Math.abs(average - priceBefore) / priceBefore : 0, pricesAfter };
}

/** How many shares a budget buys, fee included (the inverse of previewTrade, by bisection). */
export function sharesForBudget(market: Pick<Market, 'states' | 'liquidity' | 'feebps'>, state: number, budget: number): number {
  if (!(budget > 0)) return 0;
  let lo = 0;
  // Each share costs less than a coin and the fee: the budget bounds the shares from below, 1000 times it from above.
  let hi = Math.max(budget * 1000, 1);
  for (let i = 0; i < 80; i++) {
    const mid = (lo + hi) / 2;
    const p = previewTrade(market, state, mid);
    if (p && p.total <= budget) lo = mid;
    else hi = mid;
  }
  return Math.floor(lo * 1e8) / 1e8;
}

/** The most to pay for a purchase quoted at `quote`, with `slippage` (a fraction) of room for the price to move. */
export function maxCost(quote: number, slippage: number): number {
  return Math.ceil(quote * (1 + Math.max(0, slippage)) * 1e8) / 1e8;
}

/** The least to get for a sale quoted at `proceeds`, with `slippage` of room. */
export function minProceeds(proceeds: number, slippage: number): number {
  return Math.max(0, Math.floor(proceeds * (1 - Math.max(0, slippage)) * 1e8) / 1e8);
}

// --- What a market is about ---

export type MarketKind = 'binary' | 'scalar' | 'categorical';

/** A market on one yes or no question, on a scale, or on several questions (a state for each combination of answers). */
export function marketKind(market: Pick<Market, 'decisions'>, decisions: ReadonlyMap<string, Decision>): MarketKind {
  if (market.decisions.length > 1) return 'categorical';
  return decisions.get(market.decisions[0])?.scaled ? 'scalar' : 'binary';
}

/** The answer a state stands for, decision by decision: true for yes (or high). */
export function stateAnswers(state: number, count: number): boolean[] {
  return Array.from({ length: count }, (_, d) => ((state >> d) & 1) === 1);
}

/**
 * What the decisions of a categorical market have in common, left out of their options: "Who wins the
 * cup? — Brazil", "Who wins the cup? — France" are the options "Brazil" and "France". Text of the chain.
 */
export function optionTexts(texts: readonly string[]): string[] {
  if (texts.length < 2) return texts.slice();
  let prefix = texts[0];
  for (const x of texts) {
    let i = 0;
    while (i < prefix.length && i < x.length && prefix[i] === x[i]) i++;
    prefix = prefix.slice(0, i);
  }
  // Cut at the last separator of the common part, so that a word is never split.
  const cut = Math.max(prefix.lastIndexOf(' — '), prefix.lastIndexOf(': '), prefix.lastIndexOf(' - '));
  if (cut <= 0) return texts.slice();
  const sep = prefix.slice(cut).startsWith(' — ') || prefix.slice(cut).startsWith(' - ') ? 3 : 2;
  const options = texts.map((x) => x.slice(cut + sep).trim());
  return options.every((o) => o.length > 0) ? options : texts.slice();
}

/** The question of a categorical market: what its decisions have in common, or '' if nothing. */
export function commonQuestion(texts: readonly string[]): string {
  const options = optionTexts(texts);
  if (texts.length < 2 || options[0] === texts[0]) return '';
  return texts[0].slice(0, texts[0].length - options[0].length).replace(/\s*(—|-|:)\s*$/, '').trim();
}

/**
 * The question a market is about as the chain has it: the text of its decision, or what its decisions have in
 * common (their texts when nothing). The title of a market is free text of whoever opened it and may say the
 * opposite of its decision; a confirmation names this (audit 7 E-HM3). Text of the chain.
 */
export function chainQuestion(market: Pick<Market, 'decisions'>, decisions: ReadonlyMap<string, Decision>): string {
  const texts = market.decisions.map((id) => decisions.get(id)?.text ?? `${id.slice(0, 12)}…`);
  if (texts.length === 1) return texts[0];
  return commonQuestion(texts) || texts.join(' / ');
}

/**
 * A state of a market, in parts the page draws: for one decision, yes or no (low or high for a scale);
 * for several, the options answered yes. `exclusive` marks the states of a categorical market with
 * exactly one yes: the outcomes it is about; the others are combinations that should not come true.
 */
export interface Outcome {
  state: number;
  kind: 'yes' | 'no' | 'high' | 'low' | 'combo' | 'none';
  /** The options answered yes (text of the chain), for a combination. */
  options: string[];
  exclusive: boolean;
}

export function outcomesOf(market: Pick<Market, 'decisions' | 'states'>, decisions: ReadonlyMap<string, Decision>): Outcome[] {
  const n = market.decisions.length;
  const texts = market.decisions.map((id) => decisions.get(id)?.text ?? id.slice(0, 12));
  const options = optionTexts(texts);
  return market.states.map(({ state }) => {
    const yes = stateAnswers(state, n);
    if (n === 1) {
      const scaled = !!decisions.get(market.decisions[0])?.scaled;
      return { state, kind: scaled ? (yes[0] ? 'high' : 'low') : yes[0] ? 'yes' : 'no', options: [], exclusive: true };
    }
    const chosen = options.filter((_, d) => yes[d]);
    return { state, kind: chosen.length ? 'combo' : 'none', options: chosen, exclusive: chosen.length === 1 };
  });
}

/** A state in plain words, for a message: Yes, No, Low, High, or its options joined (text of the chain). */
export function describeOutcome(o: Outcome): string {
  switch (o.kind) {
    case 'yes':
      return t('Yes');
    case 'no':
      return t('No');
    case 'high':
      return t('High');
    case 'low':
      return t('Low');
    case 'none':
      return t('None of them');
    default:
      return o.options.join(' + ');
  }
}

/** Where a market on a scale puts the answer: min + (max - min) * the price of high. */
export function scalarEstimate(market: Pick<Market, 'states'>, decision: Pick<Decision, 'min' | 'max'>): number | null {
  const high = market.states.find((s) => s.state === 1)?.price;
  if (high === undefined || decision.min === undefined || decision.max === undefined) return null;
  return decision.min + (decision.max - decision.min) * high;
}

/** The state the market favours, and its chance. */
export function leader(market: Pick<Market, 'states'>): { state: number; price: number } | null {
  let best: { state: number; price: number } | null = null;
  for (const s of market.states) if (s.price !== undefined && (!best || s.price > best.price)) best = { state: s.state, price: s.price };
  return best;
}

/** Shares sold, all states together: how much a market traded. */
export function volume(market: Pick<Market, 'states'>): number {
  return market.states.reduce((n, s) => n + s.shares, 0);
}

/**
 * The category of a market: a word in brackets at the start of its title ("[Sports] Who wins…"), as
 * the wizard writes it, or a hashtag; null if it names none. Text of the chain.
 */
export function categoryOf(title: string): string | null {
  const bracket = /^\s*\[([^\]]{1,24})\]/.exec(title);
  if (bracket) return bracket[1].trim();
  const tag = /(?:^|\s)#([\p{L}\p{N}_-]{2,24})/u.exec(title);
  return tag ? tag[1] : null;
}

/** The title without its category in brackets. */
export function titleWithoutCategory(title: string): string {
  return title.replace(/^\s*\[[^\]]{1,24}\]\s*/, '');
}

/** What a share of each state paid, once the market settled: the outcomes of its decisions (StatePayout). */
export function statePayout(outcomes: readonly number[], state: number): number {
  let pay = 1;
  outcomes.forEach((o, d) => (pay *= ((state >> d) & 1) === 1 ? o : 1 - o));
  return pay;
}

// --- The voting periods ---

/** Where the clock of the markets is: from gethivemindinfo and the time (or the height) now. */
export interface Clock {
  period: number;
  phase: 'seal' | 'reveal';
  /** Seconds (time) or blocks (test chains) left in the period, and before its reveal phase. */
  left: number;
  revealIn: number;
  unit: 'seconds' | 'blocks';
  /** The length of a period, in its unit. */
  length: number;
}

/** The end of period `n`: a time (seconds) or a height. */
export function periodEnd(info: HivemindInfo, n: number): number {
  if (info.periodblocks) return (n + 1) * info.periodblocks;
  return (info.starttime ?? 0) + (n + 1) * (info.periodseconds ?? 0);
}

/** When the reveal phase of period `n` starts: the last third of it. */
export function revealStart(info: HivemindInfo, n: number): number {
  if (info.periodblocks) return n * info.periodblocks + Math.ceil((info.periodblocks * 2) / 3);
  const ps = info.periodseconds ?? 0;
  return (info.starttime ?? 0) + n * ps + Math.ceil((ps * 2) / 3);
}

/**
 * The period a moment falls in (seconds): on a chain whose periods are counted in time, the period of that time; on
 * a test chain counted in blocks, the period of the height the chain reaches by then at `spacing` seconds a block.
 * Never one that is over.
 */
export function periodAt(info: HivemindInfo, when: number, now: number, height: number, spacing: number): number {
  let n: number;
  if (info.periodblocks) {
    const h = height + 1 + Math.max(0, Math.ceil((when - now) / Math.max(1, spacing)));
    n = Math.floor(h / info.periodblocks);
  } else {
    n = Math.floor((when - (info.starttime ?? 0)) / Math.max(1, info.periodseconds ?? 1));
  }
  return Math.max(info.period, n);
}

/** The clock now: `now` in seconds of the time, `height` of the tip (blocks are counted for the next block). */
export function clockOf(info: HivemindInfo, now: number, height: number): Clock {
  const blocks = !!info.periodblocks;
  const at = blocks ? height + 1 : now;
  const end = periodEnd(info, info.period);
  const reveal = revealStart(info, info.period);
  return {
    period: info.period,
    phase: info.phase,
    left: Math.max(0, end - at),
    revealIn: Math.max(0, reveal - at),
    unit: blocks ? 'blocks' : 'seconds',
    length: blocks ? info.periodblocks! : info.periodseconds ?? 0,
  };
}

/** How far it is to a time or height, in words: "3 hours", "4 blocks". */
export function untilIn(info: HivemindInfo, target: number, now: number, height: number): string {
  if (info.periodblocks) {
    const n = Math.max(0, target - (height + 1));
    return tn('%n block(s)', n);
  }
  return niceTimeOffset(Math.max(0, target - now));
}

/** Where a decision is in its life, for the voting center. */
export type VoteStage = 'upcoming' | 'seal' | 'reveal' | 'waiting' | 'resolved';

/**
 * The stage of a decision now: before its vote; while votes are sealed (from the period after its event
 * until the reveal phase of its last period); while they are revealed (the reveal phase of its last
 * period); after the vote, until a block resolves it.
 */
export function voteStage(decision: Pick<Decision, 'status' | 'eventperiod' | 'closeperiod'>, info: Pick<HivemindInfo, 'period' | 'phase'>): VoteStage {
  if (decision.status === 'resolved') return 'resolved';
  if (info.period <= decision.eventperiod) return 'upcoming';
  if (info.period > decision.closeperiod) return 'waiting';
  if (info.period === decision.closeperiod && info.phase === 'reveal') return 'reveal';
  return 'seal';
}

/** The deadline of the stage a decision is in: the end of sealing (the reveal phase of its last period), or of revealing. */
export function voteDeadline(decision: Pick<Decision, 'closeperiod' | 'eventperiod'>, info: HivemindInfo, stage: VoteStage): number | null {
  if (stage === 'seal') return revealStart(info, decision.closeperiod);
  if (stage === 'reveal') return periodEnd(info, decision.closeperiod);
  if (stage === 'upcoming') return periodEnd(info, decision.eventperiod);
  return null;
}

// --- Answers and amounts ---

/** An answer as the wallet takes it: "yes", "no", "unknown", or a whole number of the scale. */
export function parseAnswer(text: string, decision: Pick<Decision, 'scaled' | 'min' | 'max'>): { ok: true; value: string } | { ok: false; error: string } {
  const v = text.trim().toLowerCase();
  if (v === 'unknown') return { ok: true, value: 'unknown' };
  if (!decision.scaled) {
    if (v === 'yes' || v === 'no') return { ok: true, value: v };
    return { ok: false, error: t('Choose yes, no or not known yet.') };
  }
  if (!/^-?\d+$/.test(v)) return { ok: false, error: t('The answer is a whole number of the scale.') };
  const n = Number(v);
  if (n < (decision.min ?? 0) || n > (decision.max ?? 0)) return { ok: false, error: t('The answer is from %1 to %2.', String(decision.min ?? 0), String(decision.max ?? 0)) };
  return { ok: true, value: String(n) };
}

/** A number of shares or of votecoin, with as many decimals as it needs (at most 8). */
export function formatUnits(x: number, decimals = 8): string {
  if (!Number.isFinite(x)) return '–';
  const fixed = x.toFixed(decimals).replace(/\.?0+$/, '');
  const [whole, frac] = fixed.split('.');
  const grouped = whole.replace(/\B(?=(\d{3})+(?!\d))/g, ' ');
  return frac ? `${grouped}.${frac}` : grouped;
}

/** A chance as a percentage: "62.5%". */
export function formatChance(p: number | undefined): string {
  if (p === undefined || !Number.isFinite(p)) return '–';
  const pct = p * 100;
  return `${pct >= 10 || pct === 0 ? pct.toFixed(1) : pct.toFixed(2)}%`;
}

/** Coins of the chain from a number of the RPC, in satoshis. */
export function sats(coins: number): bigint {
  return BigInt(Math.round(coins * 1e8));
}

// --- The forms of the wizard ---

export interface DecisionDraft {
  text: string;
  scaled: boolean;
  min: number;
  max: number;
  eventperiod: number;
  voteperiods: number;
}

/** What is wrong with a decision to list, in words; [] if nothing. */
export function decisionProblems(d: DecisionDraft, info: Pick<HivemindInfo, 'period' | 'maxvoteperiods' | 'maxperiodsahead'>): string[] {
  const out: string[] = [];
  const bytes = new TextEncoder().encode(d.text.trim()).length;
  if (!d.text.trim()) out.push(t('Enter the question.'));
  else if (bytes > LIMITS.maxText) out.push(t('The question is too long: at most %1 bytes.', String(LIMITS.maxText)));
  if (d.scaled) {
    if (!Number.isInteger(d.min) || !Number.isInteger(d.max)) out.push(t('The ends of the scale are whole numbers.'));
    else if (d.max <= d.min) out.push(t('The high end of the scale is above the low end.'));
  }
  if (!Number.isInteger(d.eventperiod) || d.eventperiod < info.period) out.push(t('The answer cannot be known in a period that is over: period %1 at the earliest.', String(info.period)));
  else if (d.eventperiod > info.period + info.maxperiodsahead) out.push(t('That is too far ahead: period %1 at the latest.', String(info.period + info.maxperiodsahead)));
  if (!Number.isInteger(d.voteperiods) || d.voteperiods < 1 || d.voteperiods > info.maxvoteperiods) out.push(t('A vote lasts 1 to %1 periods.', String(info.maxvoteperiods)));
  return out;
}

export interface MarketDraft {
  text: string;
  decisions: string[];
  liquidity: number;
  feebps: number;
}

/** What is wrong with a market to open; [] if nothing. Decisions are checked against those listed. */
export function marketProblems(m: MarketDraft, decisions: ReadonlyMap<string, Decision>): string[] {
  const out: string[] = [];
  const bytes = new TextEncoder().encode(m.text.trim()).length;
  if (!m.text.trim()) out.push(t('Give the market a title.'));
  else if (bytes > LIMITS.maxText) out.push(t('The title is too long: at most %1 bytes.', String(LIMITS.maxText)));
  if (m.decisions.length === 0) out.push(t('Choose the decisions the market is about.'));
  if (m.decisions.length > LIMITS.maxMarketDecisions) out.push(t('A market is about %1 decisions at most.', String(LIMITS.maxMarketDecisions)));
  if (new Set(m.decisions).size !== m.decisions.length) out.push(t('A decision is chosen twice.'));
  for (const id of m.decisions) {
    const d = decisions.get(id);
    if (d && d.status !== 'open') out.push(t('A market can only be opened on decisions that are still open.'));
  }
  if (m.decisions.length > 1 && m.decisions.some((id) => decisions.get(id)?.scaled)) out.push(t('A market on several decisions takes yes or no decisions only.'));
  if (!(m.liquidity >= LIMITS.minLiquidity)) out.push(t('The liquidity is at least %1.', String(LIMITS.minLiquidity)));
  if (!Number.isInteger(m.feebps) || m.feebps < 0 || m.feebps > LIMITS.maxFeeBps) out.push(t('The trading fee is from 0% to %1%.', String(LIMITS.maxFeeBps / 100)));
  return [...new Set(out)];
}

// --- The wallet's trades, and what they made ---

/** A trade this app made, as it recorded it (the chain keeps no history of trades). */
export interface TradeRecord {
  wallet: string;
  market: string;
  state: number;
  /** Positive bought, negative sold. */
  shares: number;
  /** Coins paid (buying) or got (selling), fee included: always positive. */
  coins: number;
  time: number;
  txid: string;
  account?: string;
  /**
   * Pending until the transaction is in a block and the chain shows what it did; refused when the chain did
   * not do it (a purchase given back, a sale skipped). Records of older versions have none: counted as done.
   */
  status?: 'pending' | 'done' | 'refused';
  /** A sale: the shares the account held before it (to tell a sale skipped from one done). */
  before?: number;
}

export interface Pnl {
  /** Shares the wallet holds now (the chain). */
  shares: number;
  /** What they bring at the price now. */
  worth: number;
  /** What was paid in all, of the trades this app recorded; null if it recorded none. */
  cost: number | null;
  /** What a share bought cost on average, fee included (the purchases recorded); null without one. */
  avgPrice: number | null;
  /** What the shares held cost, at that average; null without it. */
  basis: number | null;
  /** What sales brought. */
  proceeds: number;
  /**
   * What the trades made, at the average cost: the sales against what the shares sold cost, and the shares
   * held at the price now against what they cost (worth + proceeds - cost when the trades explain every
   * share); null without a purchase recorded.
   */
  pnl: number | null;
  /** Whether the trades recorded explain every share held (else the cost is that of a part). */
  complete: boolean;
}

/** The profit and loss of a position: the shares held at the price now, against the trades recorded (average cost). */
export function pnlOf(held: number, price: number, allTrades: readonly TradeRecord[]): Pnl {
  // Only the trades the chain did: a pending one may still be given back or skipped (audit 7 E-HM4).
  const trades = allTrades.filter((x) => x.status === undefined || x.status === 'done');
  const bought = trades.filter((x) => x.shares > 0);
  const sold = trades.filter((x) => x.shares < 0);
  const cost = bought.reduce((n, x) => n + x.coins, 0);
  const boughtShares = bought.reduce((n, x) => n + x.shares, 0);
  const soldShares = sold.reduce((n, x) => n - x.shares, 0);
  const proceeds = sold.reduce((n, x) => n + x.coins, 0);
  const net = boughtShares - soldShares;
  const worth = held * price;
  const recorded = trades.length > 0;
  const avgPrice = boughtShares > 0 ? cost / boughtShares : null;
  const basis = avgPrice === null ? null : avgPrice * held;
  return {
    shares: held,
    worth,
    cost: recorded ? cost : null,
    avgPrice,
    basis,
    proceeds,
    pnl: avgPrice === null ? null : worth - basis! + proceeds - avgPrice * soldShares,
    complete: recorded && Math.abs(net - held) < 1e-8,
  };
}

/**
 * The positions of the wallet by market and outcome: the chain keeps them by account, and a purchase goes to
 * a new account of the wallet each time (buyshares), so one outcome is often held in several. Their shares
 * add up; the accounts are kept (a sale is made from one of them). In the order the outcomes come first.
 */
export interface HeldOutcome {
  market: string;
  state: number;
  shares: number;
  /** The accounts that hold some, most first. */
  accounts: { account: string; shares: number }[];
}

export function heldByOutcome(positions: readonly WalletPosition[]): HeldOutcome[] {
  const out = new Map<string, HeldOutcome>();
  for (const p of positions) {
    const key = `${p.market}:${p.state}`;
    let h = out.get(key);
    if (!h) out.set(key, (h = { market: p.market, state: p.state, shares: 0, accounts: [] }));
    h.shares += p.shares;
    h.accounts.push({ account: p.account, shares: p.shares });
  }
  for (const h of out.values()) h.accounts.sort((a, b) => b.shares - a.shares);
  return [...out.values()];
}
