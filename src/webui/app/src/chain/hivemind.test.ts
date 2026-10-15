// The pages of Hivemind: what model.ts works out (the market maker, the names of the states, the clock
// of the periods, the checks of the forms, the profit and loss), the journal kept in the files of the host,
// and the pages driven against a pretend node: the markets and a market traded in, the wizard that lists
// decisions and opens a market, the voting center sealing and revealing, votecoin moved, the payouts,
// the cards of the home, and where the pages are in the menus.
import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest';
import { CHAIN } from './config';
import { LOCAL_COMMANDS, LOCAL_PAGES, type LocalPage } from './local';
import LocalCards from './LocalCards.svelte';
import DialogHost from '../components/DialogHost.svelte';
import Create from './pages/Create.svelte';
import Decisions from './pages/Decisions.svelte';
import Markets from './pages/Markets.svelte';
import Payouts from './pages/Payouts.svelte';
import Portfolio from './pages/Portfolio.svelte';
import Votecoin from './pages/Votecoin.svelte';
import Voting from './pages/Voting.svelte';
import { creation } from './hm/creation.svelte';
import {
  categoryOf,
  clockOf,
  commonQuestion,
  decisionProblems,
  lmsrCost,
  lmsrFunding,
  lmsrPrices,
  marketKind,
  marketProblems,
  maxCost,
  minProceeds,
  optionTexts,
  outcomesOf,
  parseAnswer,
  pnlOf,
  previewTrade,
  revealStart,
  periodEnd,
  sharesForBudget,
  statePayout,
  titleWithoutCategory,
  voteStage,
  type Decision,
  type HivemindInfo,
  type Market,
} from './hm/model';
import { hm, notePrices, resetHivemind, sanitizeJournal } from './hm/store.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { buildCommands, menuSections } from '../lib/commands.svelte';
import { dialogs } from '../lib/dialogs.svelte';
import { AREAS, chainBeforeTools, HOME_QUICK_ACTIONS, NAV_SECTIONS } from '../lib/layout';
import { go, nav } from '../lib/nav.svelte';
import { app, node, prefs, wallets } from '../lib/state.svelte';
import { resetWalletData } from '../lib/walletdata.svelte';
import { encode, program } from '../test/bech32';

const HRP = CHAIN.networks.regtest.hrp;
const ME = encode(HRP, [0, ...program(20)], false);
// Another P2WPKH address: 32 groups of 5 bits, 20 bytes.
const OTHER = encode(HRP, [0, ...Array.from({ length: 32 }, (_, i) => (i * 7 + 3) % 32)], false);
const id = (c: string) => c.repeat(64);

const INFO: HivemindInfo = { period: 3, phase: 'seal', periodblocks: 6, decisions: 6, markets: 3, voters: 2, reputation: 100, minreputation: 0.01, voterpot: 0.0042, listingfee: 0.001, maxvoteperiods: 90, maxperiodsahead: 3650 };

function decision(c: string, extra: Partial<Decision>): Decision {
  return { id: id(c), text: `Question ${c}?`, author: OTHER, scaled: false, eventperiod: 4, voteperiods: 1, closeperiod: 5, rolls: 0, height: 5, status: 'open', ...extra };
}

const DECISIONS: Decision[] = [
  decision('1', { text: 'Will it rain in Paris on 1 May?' }),
  decision('2', { text: 'Was the launch on time?', eventperiod: 2, closeperiod: 3, status: 'voting' }),
  decision('3', { text: 'How many goals were scored?', scaled: true, min: 0, max: 10, eventperiod: 2, closeperiod: 3, status: 'voting' }),
  decision('4', { text: 'Did the bridge open?', eventperiod: 0, closeperiod: 1, status: 'resolved', outcome: 1, answer: 'yes' }),
  decision('5', { text: 'Who wins the cup? — Brazil' }),
  decision('6', { text: 'Who wins the cup? — France' }),
];

function market(c: string, extra: Partial<Market>): Market {
  const shares = extra.states ? extra.states.map((s) => s.shares) : [0, 0];
  const prices = lmsrPrices(shares, extra.liquidity ?? 1);
  return {
    id: id(c),
    text: `Market ${c}`,
    author: OTHER,
    decisions: [id('1')],
    liquidity: 1,
    feebps: 50,
    height: 6,
    status: 'trading',
    tradinguntil: 4,
    treasury: 0.7,
    fees: 0.01,
    states: shares.map((q, s) => ({ state: s, answers: s ? 'Y' : 'N', shares: q, price: prices[s] })),
    ...extra,
  };
}

function marketsNow(): Market[] {
  const rain = market('a', { text: '[Weather] Rain in Paris', states: [{ state: 0, answers: 'N', shares: 0 }, { state: 1, answers: 'Y', shares: 2 }] as Market['states'] });
  rain.states = rain.states.map((s, i) => ({ ...s, price: lmsrPrices([0, 2], 1)[i] }));
  const cup = market('b', { text: 'The cup', decisions: [id('5'), id('6')], states: [0, 1, 2, 3].map((s) => ({ state: s, answers: '', shares: s === 1 ? 1 : 0 })) });
  cup.states = cup.states.map((s, i) => ({ ...s, price: lmsrPrices([0, 1, 0, 0], 1)[i] }));
  const bridge = market('c', { text: 'The bridge', decisions: [id('4')], status: 'settled', tradinguntil: undefined, states: [0, 1].map((s) => ({ state: s, answers: s ? 'Y' : 'N', shares: 1 })) });
  return [rain, cup, bridge];
}

const calls: BridgeRequest[] = [];
let info: HivemindInfo = { ...INFO };
let decisions: Decision[] = [];
let markets: Market[] = [];
let prefsStore: Record<string, unknown> = {};
/** The stores of the host (hivemind.store.*: files of the data directory), and whether writing the seals fails. */
let stores: { seals: unknown; journal: unknown } = { seals: null, journal: null };
let sealsFail = false;
let ballotOnChain: { decisions: string[] } | null = null;

async function fakeNode(r: BridgeRequest): Promise<unknown> {
  calls.push(r);
  const p = (r.params ?? []) as unknown[];
  switch (r.method) {
    case 'gethivemindinfo':
      return info;
    case 'listdecisions':
      return decisions;
    case 'getdecision': {
      const d = decisions.find((x) => x.id === p[0]);
      return { ...d, votes: d?.id === id('2') ? [{ voter: OTHER, revealed: false }] : [] };
    }
    case 'listmarkets':
      return markets;
    case 'getmarket': {
      const m = markets.find((x) => x.id === p[0]);
      return { ...m, positions: [{ account: ME, state: 1, shares: 2 }, { account: OTHER, state: 0, shares: 1 }] };
    }
    case 'listreputation':
      return [{ account: ME, amount: 70 }, { account: OTHER, amount: 30 }];
    case 'getwalletreputation':
      return [{ account: ME, amount: 70 }];
    case 'getwalletpositions':
      return [{ market: id('a'), account: ME, state: 1, shares: 2 }];
    case 'quotetrade': {
      const m = markets.find((x) => x.id === p[0])!;
      const q = Number(p[2]);
      return previewTrade(m, p[1] as number, q)!.cost + previewTrade(m, p[1] as number, q)!.fee;
    }
    case 'buyshares':
      return { txid: id('e'), account: ME, quote: 0.5, maxcost: Number(p[3]) };
    case 'sellshares':
      return { txid: id('f'), quote: 0.8, proceeds: 0.79999 };
    case 'createdecision':
      return { txid: `${p[0]}`.length.toString(16).padStart(64, '0'), id: id(String(calls.filter((c) => c.method === 'createdecision').length + 6)), author: ME };
    case 'createmarket':
      return { txid: id('9'), id: id('8'), funding: 0.69, fee: 0.0001, author: ME };
    case 'sealvote':
    case 'revealvote':
    case 'revealballot':
    case 'transfervotecoin':
      return { txid: id('d') };
    case 'sealballot':
      return { txid: id('c'), closeperiod: 3 };
    case 'getnewaddress':
      return OTHER;
    case 'hivemind.store.load':
      return structuredClone(stores);
    case 'hivemind.store.seals':
      if (sealsFail) throw Object.assign(new Error('The disk is full'), { code: -32000 });
      stores.seals = structuredClone((r.params as { value: unknown }).value);
      return true;
    case 'hivemind.store.journal':
      stores.journal = structuredClone((r.params as { value: unknown }).value);
      return true;
    case 'getballot':
      return ballotOnChain;
    case 'prefs.get':
      return prefsStore;
    case 'prefs.set':
      prefsStore = { ...prefsStore, ...(r.params as Record<string, unknown>) };
      return prefsStore;
    case 'app.notify':
      return true;
    case 'wallet.transactions':
      return {
        height: 10,
        hash: 'h',
        maturity: 0,
        records: [
          // A settlement to an account of the shares, a fee to an author, a payment that is no payout.
          [id('7'), 0, 'mined', ME, 'market shares', 1_700_000_000, 150_000_000, 0, 'confirmed', 1, 0, true, '1', 9, 'c', true, false, true],
          [id('6'), 0, 'mined', OTHER, 'market author', 1_700_000_100, 20_000_000, 0, 'confirmed', 1, 0, true, '2', 9, 'c', true, false, true],
          [id('5'), 0, 'recv', OTHER, '', 1_700_000_200, 5_000_000, 0, 'confirmed', 1, 0, true, '3', 9, 'c', false, false, true],
        ],
      };
    default:
      return null;
  }
}

beforeAll(() => {
  const proto = HTMLDialogElement.prototype as unknown as Record<string, unknown>;
  proto.showModal ??= function (this: HTMLDialogElement) {
    this.setAttribute('open', '');
  };
  proto.close ??= function (this: HTMLDialogElement) {
    this.removeAttribute('open');
  };
  // jsdom has no ResizeObserver (bind:clientWidth of the charts).
  (globalThis as Record<string, unknown>).ResizeObserver ??= class {
    observe() {}
    unobserve() {}
    disconnect() {}
  };
  window.matchMedia ??= ((query: string) => ({ matches: false, media: query, addEventListener() {}, removeEventListener() {} })) as unknown as typeof window.matchMedia;
});

beforeEach(() => {
  app.info = { name: CHAIN.name, version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 60 };
  prefs.unit = 'CHN';
  prefs.mask = false;
  node.tip = { height: 20, time: 1_700_000_000, hash: 'ab'.repeat(32), progress: 1, state: 'synced', ibd: false, loadingBlocks: false };
  wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
  wallets.current = 'w';
  info = { ...INFO };
  decisions = DECISIONS.map((d) => ({ ...d }));
  markets = marketsNow();
  prefsStore = {};
  stores = { seals: null, journal: null };
  sealsFail = false;
  ballotOnChain = null;
  calls.length = 0;
  resetHivemind();
  creation.jobs = [];
  setTransport(fakeNode);
});

afterEach(() => {
  resetHivemind();
  setTransport(null);
  dialogs.queue.splice(0);
  wallets.list = [];
  wallets.current = null;
  resetWalletData();
  nav.tab = '';
});

async function answer(choice: string) {
  await waitFor(() => expect(document.querySelector(`button[data-choice="${choice}"]`)).not.toBeNull());
  await fireEvent.click(document.querySelector(`button[data-choice="${choice}"]`)!);
}
const called = (method: string) => calls.filter((c) => c.method === method);
const loaded = () => waitFor(() => expect(hm.loaded && hm.markets.length > 0).toBe(true));

describe('the market maker, as the chain works it out', () => {
  it('prices the states by their shares, and costs a trade by the difference', () => {
    expect(lmsrPrices([0, 0], 1)).toEqual([0.5, 0.5]);
    const p = lmsrPrices([0, 2], 1);
    expect(p[0] + p[1]).toBeCloseTo(1, 12);
    expect(p[1]).toBeCloseTo(Math.exp(2) / (1 + Math.exp(2)), 12);
    // Stable with large shares.
    expect(lmsrPrices([1000, 0], 1)[0]).toBeCloseTo(1, 12);
    expect(lmsrCost([0, 0], 1)).toBeCloseTo(Math.log(2), 12);
    expect(lmsrFunding(1, 2)).toBeCloseTo(Math.log(2), 12);
    expect(lmsrFunding(2, 16)).toBeCloseTo(2 * Math.log(16), 12);
  });

  it('previews a trade: cost, fee, average, the price after; and the shares a budget buys', () => {
    const m = market('a', {});
    const buy = previewTrade(m, 1, 1)!;
    expect(buy.cost).toBeCloseTo(Math.log((1 + Math.E) / 2), 10);
    expect(buy.fee).toBeCloseTo(buy.cost * 0.005, 12);
    expect(buy.total).toBeCloseTo(buy.cost * 1.005, 12);
    expect(buy.priceBefore).toBe(0.5);
    expect(buy.priceAfter).toBeCloseTo(Math.E / (1 + Math.E), 10);
    expect(buy.pricesAfter.reduce((a, b) => a + b, 0)).toBeCloseTo(1, 12);
    // Selling what was bought gives back the cost, less the fee.
    const after = { ...m, states: m.states.map((s) => ({ ...s, shares: s.state === 1 ? 1 : 0 })) };
    const sell = previewTrade(after, 1, -1)!;
    expect(sell.total).toBeCloseTo(buy.cost * 0.995, 10);
    // More shares than there are cannot be sold back to the maker.
    expect(previewTrade(m, 1, -1)).toBeNull();
    expect(previewTrade(m, 2, 1)).toBeNull();
    const shares = sharesForBudget(m, 1, 0.5);
    expect(previewTrade(m, 1, shares)!.total).toBeLessThanOrEqual(0.5);
    expect(previewTrade(m, 1, shares + 1e-6)!.total).toBeGreaterThan(0.5 - 1e-6);
    expect(maxCost(1, 0.01)).toBe(1.01);
    expect(minProceeds(1, 0.02)).toBe(0.98);
  });

  it('names the states: yes and no, low and high, the options of several decisions', () => {
    const map = new Map(DECISIONS.map((d) => [d.id, d]));
    const [rain, cup] = marketsNow();
    expect(marketKind(rain, map)).toBe('binary');
    expect(marketKind(cup, map)).toBe('categorical');
    expect(marketKind({ decisions: [id('3')] }, map)).toBe('scalar');
    expect(outcomesOf(rain, map).map((o) => o.kind)).toEqual(['no', 'yes']);
    expect(outcomesOf({ decisions: [id('3')], states: rain.states }, map).map((o) => o.kind)).toEqual(['low', 'high']);
    const outs = outcomesOf(cup, map);
    expect(outs.map((o) => [o.kind, o.options, o.exclusive])).toEqual([
      ['none', [], false],
      ['combo', ['Brazil'], true],
      ['combo', ['France'], true],
      ['combo', ['Brazil', 'France'], false],
    ]);
    expect(optionTexts(['Who wins? — A', 'Who wins? — B'])).toEqual(['A', 'B']);
    expect(optionTexts(['Rain?', 'Snow?'])).toEqual(['Rain?', 'Snow?']);
    expect(commonQuestion(['Who wins the cup? — Brazil', 'Who wins the cup? — France'])).toBe('Who wins the cup?');
    expect(categoryOf('[Sports] Who wins')).toBe('Sports');
    expect(categoryOf('Rain #weather tomorrow')).toBe('weather');
    expect(categoryOf('No category')).toBeNull();
    expect(titleWithoutCategory('[Sports] Who wins')).toBe('Who wins');
    expect(statePayout([1, 0], 1)).toBe(1);
    expect(statePayout([1, 0], 3)).toBe(0);
    expect(statePayout([0.25], 1)).toBe(0.25);
  });
});

describe('the clock of the periods', () => {
  it('counts periods in blocks on a test chain: the seal phase, then the last third to reveal', () => {
    const blocks: HivemindInfo = { ...INFO, period: 3, periodblocks: 6 };
    expect(periodEnd(blocks, 3)).toBe(24);
    expect(revealStart(blocks, 3)).toBe(22);
    // At height 19 the next block (20) is in period 3: 4 blocks to its end, 2 to its reveal phase.
    expect(clockOf(blocks, 0, 19)).toMatchObject({ period: 3, left: 4, revealIn: 2, unit: 'blocks', length: 6 });
  });

  it('counts periods in mainchain time on the main network', () => {
    const time: HivemindInfo = { ...INFO, periodblocks: undefined, periodseconds: 3600, starttime: 1000, period: 2 };
    expect(periodEnd(time, 2)).toBe(1000 + 3 * 3600);
    expect(revealStart(time, 2)).toBe(1000 + 2 * 3600 + 2400);
    expect(clockOf(time, 1000 + 2 * 3600 + 600, 0)).toMatchObject({ left: 3000, revealIn: 1800, unit: 'seconds' });
  });

  it('tells the stage of the vote on a decision', () => {
    const at = (period: number, phase: 'seal' | 'reveal') => ({ period, phase });
    const d = { status: 'voting' as const, eventperiod: 2, closeperiod: 4 };
    expect(voteStage({ ...d, status: 'open' }, at(2, 'seal'))).toBe('upcoming');
    expect(voteStage(d, at(3, 'reveal'))).toBe('seal');
    expect(voteStage(d, at(4, 'seal'))).toBe('seal');
    expect(voteStage(d, at(4, 'reveal'))).toBe('reveal');
    expect(voteStage(d, at(5, 'seal'))).toBe('waiting');
    expect(voteStage({ ...d, status: 'resolved' }, at(5, 'seal'))).toBe('resolved');
  });
});

describe('the forms', () => {
  it('checks an answer, a decision, a market', () => {
    expect(parseAnswer('Yes', { scaled: false })).toEqual({ ok: true, value: 'yes' });
    expect(parseAnswer('maybe', { scaled: false }).ok).toBe(false);
    expect(parseAnswer('7', { scaled: true, min: 0, max: 10 })).toEqual({ ok: true, value: '7' });
    expect(parseAnswer('11', { scaled: true, min: 0, max: 10 }).ok).toBe(false);
    expect(parseAnswer('unknown', { scaled: true, min: 0, max: 10 })).toEqual({ ok: true, value: 'unknown' });
    const ok = { text: 'Will it rain?', scaled: false, min: 0, max: 0, eventperiod: 4, voteperiods: 1 };
    expect(decisionProblems(ok, INFO)).toEqual([]);
    expect(decisionProblems({ ...ok, text: '' }, INFO)).toEqual(['Enter the question.']);
    expect(decisionProblems({ ...ok, text: 'é'.repeat(201) }, INFO)[0]).toMatch(/too long/);
    expect(decisionProblems({ ...ok, eventperiod: 2 }, INFO)[0]).toMatch(/period 3 at the earliest/);
    expect(decisionProblems({ ...ok, voteperiods: 91 }, INFO)[0]).toMatch(/1 to 90 periods/);
    expect(decisionProblems({ ...ok, scaled: true, min: 5, max: 5 }, INFO)[0]).toMatch(/above the low end/);
    const map = new Map(DECISIONS.map((d) => [d.id, d]));
    const m = { text: 'Rain', decisions: [id('1')], liquidity: 1, feebps: 50 };
    expect(marketProblems(m, map)).toEqual([]);
    expect(marketProblems({ ...m, decisions: [id('2')] }, map)[0]).toMatch(/still open/);
    expect(marketProblems({ ...m, liquidity: 0.001 }, map)[0]).toMatch(/at least 0.01/);
    expect(marketProblems({ ...m, feebps: 1001 }, map)[0]).toMatch(/0% to 10%/);
    expect(marketProblems({ ...m, decisions: [id('1'), id('3')] }, map)).toContain('A market on several decisions takes yes or no decisions only.');
    expect(marketProblems({ ...m, decisions: ['a', 'b', 'c', 'd', 'e'] }, map)).toContain('A market is about 4 decisions at most.');
  });

  it('works out the profit and loss of a position from the trades recorded', () => {
    expect(pnlOf(2, 0.6, [])).toMatchObject({ worth: 1.2, cost: null, pnl: null, complete: false });
    const t = (shares: number, coins: number) => ({ wallet: 'w', market: 'm', state: 1, shares, coins, time: 0, txid: 'x' });
    const p = pnlOf(2, 0.6, [t(3, 1.5), t(-1, 0.7)]);
    expect(p.cost).toBe(1.5);
    expect(p.proceeds).toBe(0.7);
    expect(p.pnl).toBeCloseTo(1.2 + 0.7 - 1.5, 12);
    expect(p.complete).toBe(true);
    expect(pnlOf(5, 0.6, [t(3, 1.5)]).complete).toBe(false);
  });
});

describe('the journal of the app', () => {
  it('keeps the prices it saw, and reads back only what has its shape', async () => {
    const [rain] = marketsNow();
    expect(notePrices([rain], 20, 1000)).toBe(true);
    // The start at even odds, then the price now.
    expect(hm.journal.prices[rain.id].map((p) => p.p)).toEqual([[5000, 5000], rain.states.map((s) => Math.round(s.price! * 10000))]);
    expect(notePrices([rain], 21, 1001)).toBe(false);
    const j = sanitizeJournal({ sealed: { a: { wallet: 'w', account: ME, decision: 'd', answer: 'yes', close: 3 }, b: { wallet: 1 } }, votes: 'x', trades: [{ wallet: 'w', market: 'm', shares: 1, coins: 1 }, {}], reminders: false });
    expect(Object.keys(j.sealed)).toEqual(['a']);
    expect(j.votes).toEqual([]);
    expect(j.trades.length).toBe(1);
    expect(j.reminders).toBe(false);
    expect(sanitizeJournal(null).reminders).toBe(true);
  });
});

describe('the journal in the files of the host (audit port-1 #1)', () => {
  it('keeps the seals in a file of their own, written at once, whatever the size of the journal', async () => {
    const { loadJournal, noteSealed, saveJournal } = await import('./hm/store.svelte');
    await loadJournal();
    // A journal past 256 kB (the cap of the preferences): written all the same.
    const pts = Array.from({ length: 60 }, (_, i) => ({ h: i, t: i, p: Array.from({ length: 16 }, () => 625) }));
    for (let i = 0; i < 40; i++) hm.journal.prices['m' + i + 'x'.repeat(60)] = pts;
    hm.journal.votes = Array.from({ length: 200 }, () => ({ wallet: 'w', account: 'a'.repeat(42), decision: 'd'.repeat(64), text: 'Q'.repeat(400), answer: 'yes', time: 1, txid: 'f'.repeat(64) }));
    expect(await noteSealed({ wallet: 'w', account: ME, decision: id('2'), answer: 'yes', close: 3, time: 1, txid: id('d') })).toBe(true);
    expect((stores.seals as { sealed: Record<string, { answer: string }> }).sealed[`w|${ME}|${id('2')}`].answer).toBe('yes');
    saveJournal();
    await waitFor(() => expect(stores.journal).not.toBeNull(), { timeout: 2000 });
    expect(JSON.stringify(stores.journal).length).toBeGreaterThan(256 * 1024);
    // The seals are not in the journal file, and come back from their own after a restart.
    expect((stores.journal as Record<string, unknown>).sealed).toBeUndefined();
    resetHivemind();
    await loadJournal();
    expect(Object.keys(hm.journal.sealed)).toEqual([`w|${ME}|${id('2')}`]);
    expect(Object.keys(hm.journal.prices).length).toBe(40);
  });

  it('says so, with the answer, when the seal cannot be written', async () => {
    const { loadJournal, noteSealed } = await import('./hm/store.svelte');
    render(DialogHost);
    await loadJournal();
    sealsFail = true;
    const kept = noteSealed({ wallet: 'w', account: ME, decision: id('2'), answer: 'no', close: 3, time: 1, txid: id('d') });
    await waitFor(() => expect(dialogs.queue[0]?.title).toBe('The answer of your vote could not be saved'));
    expect(dialogs.queue[0]?.informative).toContain(': no');
    await answer('ok');
    expect(await kept).toBe(false);
  });

  it('moves a journal an older version left in the preferences to the files', async () => {
    const { loadJournal } = await import('./hm/store.svelte');
    prefsStore = { hivemind: { sealed: { k: { wallet: 'w', account: ME, decision: id('2'), answer: 'yes', close: 3, time: 1, txid: 'x' } }, trades: [{ wallet: 'w', market: 'm', state: 1, shares: 1, coins: 1, time: 1 }] } };
    await loadJournal();
    expect(Object.keys(hm.journal.sealed)).toEqual(['k']);
    expect((stores.seals as { sealed: Record<string, unknown> }).sealed.k).toBeTruthy();
    expect((stores.journal as { trades: unknown[] }).trades.length).toBe(1);
    await waitFor(() => expect(prefsStore.hivemind).toBeNull());
  });
});

describe.runIf(CHAIN.name === 'Hivemind')('the pages of Hivemind', () => {
  it('are in the Chain section and the menus, before the tools, with their quick actions', () => {
    const own = (LOCAL_PAGES as readonly LocalPage[]).map((p) => p.id);
    expect(own).toEqual(['markets', 'portfolio', 'voting', 'decisions', 'create', 'votecoin', 'payouts']);
    expect(NAV_SECTIONS.find((s) => s.title === 'Chain')!.pages).toEqual(['peg', 'bmm', ...own, 'explorer']);
    expect(AREAS.find((a) => a.id === 'chain')!.tabs.map((t) => t.page)).toEqual(['peg', 'bmm', ...own, 'explorer']);
    expect(chainBeforeTools(HOME_QUICK_ACTIONS.map((q) => q.page), (p) => p)).toBeNull();
    expect(HOME_QUICK_ACTIONS.map((q) => q.label)).toEqual(expect.arrayContaining(['Trade', 'Vote', 'Create a market']));
    const chain = menuSections(buildCommands()).find((s) => s.group === 'Chains')!;
    for (const c of LOCAL_COMMANDS) expect(chain.items.map((i) => i.label)).toContain(c.label);
    chain.items.find((c) => c.label === 'Periods calendar')!.run();
    expect([nav.page, nav.tab]).toEqual(['decisions', 'calendar']);
    expect(CHAIN.uriScheme).toBe('hivemind');
  });

  it('Markets: the cards, filtered by status, kind, category and words', async () => {
    render(Markets);
    await loaded();
    await waitFor(() => expect(screen.getAllByRole('button', { name: /Rain in Paris|The cup/ }).length).toBe(2));
    // The settled market shows with "Paid out".
    expect(screen.queryByText('The bridge')).toBeNull();
    await fireEvent.click(screen.getByRole('button', { name: /^Paid out/ }));
    await waitFor(() => expect(screen.getByText('The bridge')).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: /^All/ }));
    await fireEvent.click(screen.getByRole('button', { name: 'Several outcomes' }));
    await waitFor(() => expect(screen.queryByText('Rain in Paris')).toBeNull());
    expect(screen.getByText('The cup')).toBeTruthy();
    await fireEvent.click(screen.getByRole('button', { name: 'Every kind' }));
    await fireEvent.change(screen.getByRole('combobox', { name: 'Category' }), { target: { value: 'Weather' } });
    await waitFor(() => expect(screen.queryByText('The cup')).toBeNull());
    expect(screen.getByText('Rain in Paris')).toBeTruthy();
    await fireEvent.change(screen.getByRole('combobox', { name: 'Category' }), { target: { value: '' } });
    await fireEvent.input(screen.getByRole('searchbox', { name: 'Search markets' }), { target: { value: 'france' } });
    await waitFor(() => expect(screen.queryByText('Rain in Paris')).toBeNull());
    expect(screen.getByText('The cup')).toBeTruthy();
  });

  it('a market: its chances, the positions of the wallet, a purchase with its preview and slippage, a sale', async () => {
    go('markets', `m-${id('a')}`);
    render(Markets);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(screen.getByRole('heading', { name: 'Rain in Paris' })).toBeTruthy());
    await waitFor(() => expect(called('getmarket').length).toBeGreaterThan(0));
    expect(screen.getByRole('img', { name: 'The chance of each outcome over time' })).toBeTruthy();
    expect(screen.getByRole('table', { name: 'Your positions in this market' })).toBeTruthy();
    expect(within(screen.getByRole('table', { name: 'Who holds shares' })).getAllByText('yours').length).toBe(1);
    // Buy Yes for 0.5 coins: the preview, then the node's quote.
    const trade = screen.getByRole('region', { name: 'Trade' });
    await fireEvent.click(within(trade).getByRole('radio', { name: /Yes/ }));
    await fireEvent.input(trade.querySelector('#trade-amount')!, { target: { value: '0.5' } });
    await waitFor(() => expect(within(trade).getByLabelText('Preview of the trade')).toBeTruthy());
    await waitFor(() => expect(called('quotetrade').length).toBeGreaterThan(0));
    await waitFor(() => expect(within(trade).getByText('Quoted by the node just now.')).toBeTruthy());
    await fireEvent.click(within(trade).getByRole('button', { name: '2%' }));
    await fireEvent.click(trade.querySelector('#trade-place')!);
    await answer('yes');
    await waitFor(() => expect(called('buyshares').length).toBe(1));
    const [mkt, state, shares, max] = called('buyshares')[0].params as [string, number, string, string];
    expect([mkt, state]).toEqual([id('a'), 1]);
    const quote = previewTrade(markets[0], 1, Number(shares))!.total;
    expect(quote).toBeLessThanOrEqual(0.5 + 1e-8);
    expect(Number(max)).toBeCloseTo(maxCost(quote, 0.02), 6);
    expect(hm.journal.trades).toMatchObject([{ wallet: 'w', market: id('a'), state: 1, coins: 0.5 }]);
    // Sell all of the shares of the account.
    await fireEvent.click(within(trade).getByRole('tab', { name: 'Sell' }));
    await fireEvent.click(within(trade).getByRole('button', { name: 'All' }));
    await waitFor(() => expect(within(trade).getByLabelText('Preview of the trade')).toBeTruthy());
    await fireEvent.click(trade.querySelector('#trade-place')!);
    await answer('yes');
    await waitFor(() => expect(called('sellshares').length).toBe(1));
    expect(called('sellshares')[0].params).toEqual([id('a'), 1, '2.00000000', ME, expect.any(String)]);
    expect(hm.journal.trades.length).toBe(2);
  });

  it('Create: a market on a new yes or no decision, listed first, opened once it is in a block', async () => {
    go('create', 'market');
    render(Create);
    render(DialogHost);
    await loaded();
    await fireEvent.click(screen.getByRole('radio', { name: /Yes or no/ }));
    await fireEvent.click(document.querySelector('#create-next')!);
    await fireEvent.input(document.querySelector('#create-question')!, { target: { value: 'Will the vote pass?' } });
    await fireEvent.click(document.querySelector('#create-next')!);
    // Timing: the answer in 2 periods, voted on for 2.
    await fireEvent.input(document.querySelector('#create-event')!, { target: { value: '2' } });
    await fireEvent.input(document.querySelector('#create-votes')!, { target: { value: '2' } });
    expect(screen.getByRole('list', { name: 'Timeline' }).textContent).toContain('until period 5 ends');
    await fireEvent.click(document.querySelector('#create-next')!);
    await fireEvent.change(screen.getByRole('combobox', { name: 'Category' }), { target: { value: 'Politics' } });
    await fireEvent.input(document.querySelector('#create-liquidity')!, { target: { value: '0.001' } });
    await waitFor(() => expect(screen.getByRole('alert').textContent).toMatch(/liquidity is at least/));
    await fireEvent.input(document.querySelector('#create-liquidity')!, { target: { value: '2' } });
    await fireEvent.click(document.querySelector('#create-next')!);
    expect(screen.getByLabelText('Preview of the market').textContent).toContain('Will the vote pass?');
    await fireEvent.click(document.querySelector('#create-submit')!);
    await answer('yes');
    await waitFor(() => expect(called('createdecision').length).toBe(1));
    expect(called('createdecision')[0].params).toEqual(['Will the vote pass?', 5, 2]);
    expect(called('createmarket').length).toBe(0);
    await waitFor(() => expect(creation.jobs[0]?.state).toBe('waiting'));
    // The decision mined: the next refresh opens the market.
    const listed = called('createdecision').length + 6;
    decisions = [...decisions, decision(String(listed), { text: 'Will the vote pass?', eventperiod: 5, closeperiod: 7 })];
    node.tip = { ...node.tip!, height: 21, hash: 'cd'.repeat(32) };
    const { refreshHivemind } = await import('./hm/store.svelte');
    await refreshHivemind();
    await waitFor(() => expect(called('createmarket').length).toBe(1));
    expect(called('createmarket')[0].params).toEqual(['[Politics] Will the vote pass?', [id(String(listed))], '2.00000000', 50]);
    await waitFor(() => expect(creation.jobs[0].state).toBe('done'));
  });

  it('Create: one of several options lists a decision for each; a decision alone opens no market', async () => {
    go('create', 'market');
    render(Create);
    render(DialogHost);
    await loaded();
    await fireEvent.click(screen.getByRole('radio', { name: /One of several/ }));
    await fireEvent.click(document.querySelector('#create-next')!);
    await fireEvent.input(document.querySelector('#create-question')!, { target: { value: 'Who wins?' } });
    await fireEvent.input(screen.getByRole('textbox', { name: 'Option 1' }), { target: { value: 'Ann' } });
    await fireEvent.input(screen.getByRole('textbox', { name: 'Option 2' }), { target: { value: 'Ann' } });
    await waitFor(() => expect(screen.getByRole('alert').textContent).toMatch(/same name/));
    await fireEvent.input(screen.getByRole('textbox', { name: 'Option 2' }), { target: { value: 'Bob' } });
    await fireEvent.click(screen.getByRole('button', { name: /Option$/ }));
    await fireEvent.input(screen.getByRole('textbox', { name: 'Option 3' }), { target: { value: 'Cy' } });
    await fireEvent.click(document.querySelector('#create-next')!);
    await fireEvent.click(document.querySelector('#create-next')!);
    await fireEvent.click(document.querySelector('#create-next')!);
    await fireEvent.click(document.querySelector('#create-submit')!);
    await answer('yes');
    await waitFor(() => expect(called('createdecision').length).toBe(3));
    expect(called('createdecision').map((c) => (c.params as unknown[])[0])).toEqual(['Who wins? — Ann', 'Who wins? — Bob', 'Who wins? — Cy']);
    calls.length = 0;
    creation.jobs = [];
    go('create', 'decision');
    await waitFor(() => expect(screen.getByRole('button', { name: 'A decision alone' }).getAttribute('aria-pressed')).toBe('true'));
  });

  it('Voting center: seals an answer, keeps it, reveals it in the reveal phase; a ballot for several', async () => {
    render(Voting);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(screen.getAllByRole('article', { name: 'A decision to vote on' }).length).toBe(2));
    const [launch, goals] = screen.getAllByRole('article', { name: 'A decision to vote on' });
    await fireEvent.click(within(launch).getByRole('radio', { name: 'Yes' }));
    await fireEvent.click(within(launch).getByRole('button', { name: /Seal vote/ }));
    await waitFor(() => expect(called('sealvote').length).toBe(1));
    expect(called('sealvote')[0].params).toEqual([ME, id('2'), 'yes']);
    expect(Object.values(hm.journal.sealed)).toMatchObject([{ wallet: 'w', account: ME, decision: id('2'), answer: 'yes', close: 3 }]);
    // A ballot: both answered, closing in the same period.
    await fireEvent.input(within(goals).getByRole('textbox', { name: 'The answer' }), { target: { value: '4' } });
    await fireEvent.click(screen.getByRole('button', { name: /in one ballot/ }));
    await answer('yes');
    await waitFor(() => expect(called('sealballot').length).toBe(1));
    expect(called('sealballot')[0].params).toEqual([ME, { [id('2')]: 'yes', [id('3')]: '4' }]);
    // The reveal phase of period 3: both to reveal, the ballot whole.
    info = { ...INFO, phase: 'reveal' };
    node.tip = { ...node.tip!, height: 22, hash: 'ef'.repeat(32) };
    const { refreshHivemind } = await import('./hm/store.svelte');
    await refreshHivemind();
    await waitFor(() => expect(screen.getByRole('heading', { name: /Reveal now/ })).toBeTruthy());
    await fireEvent.click(document.querySelector(`#reveal-${id('2').slice(0, 8)}`)!);
    await waitFor(() => expect(called('revealballot').length).toBe(1));
    expect(called('revealballot')[0].params).toEqual([ME, { [id('2')]: 'yes', [id('3')]: '4' }]);
    expect(Object.keys(hm.journal.sealed)).toEqual([]);
    expect(hm.journal.votes.map((v) => v.answer).sort()).toEqual(['4', 'yes']);
    // And a reminder was given while votes were due.
    expect(called('app.notify').length).toBeGreaterThan(0);
  });

  it('Voting center: a ballot this app has no record of is revealed with the answers given by hand (getballot)', async () => {
    ballotOnChain = { decisions: [id('2'), id('3')] };
    info = { ...INFO, phase: 'reveal' };
    // The chain has the voter's sealed votes; this app has no record of them.
    const before = decisions;
    decisions = before.map((d) => d);
    render(Voting);
    render(DialogHost);
    await loaded();
    hm.details = { [id('2')]: { ...decisions[1], votes: [{ voter: ME, revealed: false }] }, [id('3')]: { ...decisions[2], votes: [{ voter: ME, revealed: false }] } };
    await waitFor(() => expect(screen.getByRole('heading', { name: /Reveal now/ })).toBeTruthy());
    await waitFor(() => expect(called('getballot').length).toBeGreaterThan(0));
    expect(called('getballot')[0].params).toEqual([ME, 3]);
    await waitFor(() => expect(screen.getAllByText(/Sealed in a ballot of 2 decisions/).length).toBe(2));
    // Only one answer: the ballot needs both.
    const selects = screen.getAllByRole('combobox', { name: 'The answer you sealed' });
    await fireEvent.change(selects[0], { target: { value: 'yes' } });
    await fireEvent.click(document.querySelector(`#reveal-${id('2').slice(0, 8)}`)!);
    await waitFor(() => expect(dialogs.queue[0]?.message).toMatch(/ballot of 2 decisions/));
    await answer('ok');
    expect(called('revealballot').length).toBe(0);
    await fireEvent.input(screen.getByRole('textbox', { name: 'The answer you sealed' }), { target: { value: '4' } });
    await fireEvent.click(document.querySelector(`#reveal-${id('2').slice(0, 8)}`)!);
    await waitFor(() => expect(called('revealballot').length).toBe(1));
    expect(called('revealballot')[0].params).toEqual([ME, { [id('2')]: 'yes', [id('3')]: '4' }]);
    expect(called('revealvote').length).toBe(0);
  });

  it('Votecoin: the holdings, a transfer checked against the least holding, then sent', async () => {
    render(Votecoin);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(screen.getByRole('table', { name: 'Accounts of this wallet' })).toBeTruthy());
    expect(within(screen.getByRole('table', { name: 'Largest holders' })).getAllByRole('row').length).toBe(3);
    await fireEvent.click(screen.getByRole('button', { name: /A new account of this wallet/ }));
    await waitFor(() => expect(called('getnewaddress')[0]?.params).toEqual(['votecoin', 'bech32']));
    await fireEvent.input(document.querySelector('#votecoin-amount')!, { target: { value: '69.995' } });
    await waitFor(() => expect(screen.getByRole('alert').textContent).toMatch(/none or with at least/));
    await fireEvent.input(document.querySelector('#votecoin-amount')!, { target: { value: '20' } });
    await waitFor(() => expect(screen.queryByRole('alert')).toBeNull());
    await waitFor(() => expect((document.querySelector('#votecoin-send') as HTMLButtonElement).disabled).toBe(false));
    await fireEvent.click(document.querySelector('#votecoin-send')!);
    await answer('yes');
    await waitFor(() => expect(called('transfervotecoin').length).toBe(1));
    expect(called('transfervotecoin')[0].params).toEqual([ME, OTHER, '20.00000000']);
  });

  it('Portfolio, Payouts, Decisions and the cards of the home', async () => {
    const portfolio = render(Portfolio);
    await loaded();
    await waitFor(() => expect(screen.getByRole('table', { name: 'Positions' })).toBeTruthy());
    expect(within(screen.getByRole('table', { name: 'Positions' })).getByText('Rain in Paris')).toBeTruthy();
    portfolio.unmount();

    const payouts = render(Payouts);
    await waitFor(() => expect(screen.getByRole('table', { name: 'Payouts received' })).toBeTruthy());
    // The settlement and the author's fee; not the plain payment.
    expect(within(screen.getByRole('table', { name: 'Payouts received' })).getAllByRole('row').length).toBe(3);
    expect(screen.getByText('The bridge')).toBeTruthy();
    payouts.unmount();

    const list = render(Decisions);
    await waitFor(() => expect(screen.getByRole('table', { name: 'Decisions' })).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: /^Being voted on/ }));
    await waitFor(() => expect(within(screen.getByRole('table', { name: 'Decisions' })).getAllByRole('row').length).toBe(3));
    nav.tab = 'calendar';
    await waitFor(() => expect(screen.getByRole('table', { name: 'Periods ahead' })).toBeTruthy());
    list.unmount();

    render(LocalCards);
    await waitFor(() => expect(screen.getByRole('heading', { name: 'Votes due' })).toBeTruthy());
    expect(screen.getByRole('heading', { name: 'My positions' })).toBeTruthy();
    expect(screen.getByRole('heading', { name: 'Closing soon' })).toBeTruthy();
    expect(screen.getByRole('heading', { name: 'Votecoin' })).toBeTruthy();
    expect(screen.getByText('Rain in Paris')).toBeTruthy();
  });
});
