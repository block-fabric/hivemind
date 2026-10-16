// The bets of Simple mode (pages/SimpleMarkets.svelte, hm/SimpleMarket.svelte, pages/SimpleBets.svelte):
// a bet is a purchase of the shares of one outcome for an amount of coins, a cash-out the sale of all the
// shares of an outcome the wallet holds. They are placed exactly as the trade panel of Advanced places
// them (TradePanel.svelte): the node quotes the trade (quotetrade), then buyshares carries the most it pays
// and sellshares the least it takes, with 1 % of room for the price to move (the default slippage there).
import { t } from '../../lib/i18n.svelte';
import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
import { shortHash } from '../../lib/format';
import { rpc } from '../../lib/rpc';
import { toast } from '../../lib/state.svelte';
import { CHAIN } from '../config';
import { LIMITS, maxCost, minProceeds, previewTrade, sharesForBudget, type Market } from './model';
import { noteTrade, refreshHivemind } from './store.svelte';

/** The room for the price to move, as the trade panel's default. */
export const SIMPLE_SLIPPAGE = 0.01;

/** An amount of coins with the ticker of the chain, 4 decimals at most. */
export function coins(x: number): string {
  const v = Math.abs(x) < 1e-12 ? 0 : x;
  return `${v.toFixed(4).replace(/\.?0+$/, '')} ${CHAIN.coin.ticker}`;
}

/** What a bet of `budget` coins on a state buys: its shares, what it costs (fee included), what it pays if right. */
export function betPreview(market: Market, state: number, budget: number): { shares: number; cost: number; pays: number } | null {
  if (!(budget > 0)) return null;
  const shares = sharesForBudget(market, state, budget);
  const p = shares > 0 ? previewTrade(market, state, shares) : null;
  return p ? { shares, cost: p.total, pays: shares } : null;
}

/** What selling these shares of a state gets now, account after account (each sale moves the price), fee taken. */
export function cashOutPreview(market: Market, state: number, accounts: readonly { shares: number }[]): number {
  let m = market;
  let total = 0;
  for (const a of accounts) {
    const p = previewTrade(m, state, -a.shares);
    if (!p) continue;
    total += p.total;
    m = { ...m, states: m.states.map((s) => (s.state === state ? { ...s, shares: s.shares - a.shares } : s)) };
  }
  return total;
}

/** Why a bet cannot be placed ('' when it can). */
export function betProblem(market: Market, budget: number, holding: number, preview: ReturnType<typeof betPreview>): string {
  if (market.status !== 'trading') return t('Betting on this question is closed.');
  if (!(budget > 0)) return '';
  if (!preview) return t('That buys nothing.');
  if (holding <= 0 && preview.cost < LIMITS.minPosition) return t('A first bet on an answer is at least %1.', coins(LIMITS.minPosition));
  return '';
}

/** Place a bet: the confirmation, then buyshares as the trade panel places it. @return whether it was placed */
export async function placeBet(wallet: string, market: Market, state: number, answer: string, budget: number): Promise<boolean> {
  const shares = sharesForBudget(market, state, budget);
  if (!(shares > 0)) return false;
  const sharesText = shares.toFixed(8);
  let quote: number;
  try {
    quote = await rpc.raw<number>('quotetrade', [market.id, state, sharesText]);
  } catch (e) {
    await inform(t('Place a bet'), errorText(e), 'error');
    return false;
  }
  const bound = maxCost(quote, SIMPLE_SLIPPAGE);
  const r = await ask({
    title: t('Place a bet'),
    message: t('Bet %1 on “%2” to the question “%3”?', coins(quote), answer, market.text),
    informative: t('If “%1” is the answer you get %2; if not, you lose what you bet. If the price moves before the bet is in a block, it costs at most %3, or nothing happens and the coins stay yours.', answer, coins(shares), coins(bound)),
    level: 'question',
    choices: [
      { id: 'no', label: 'Cancel' },
      { id: 'yes', label: 'Bet', kind: 'primary' },
    ],
  });
  if (r.choice !== 'yes') return false;
  try {
    const done = await walletRpc<{ txid: string; account: string; quote: number; maxcost: number }>(wallet, 'buyshares', [market.id, state, sharesText, bound.toFixed(8)]);
    if (!done) return false;
    noteTrade({ wallet, market: market.id, state, shares, coins: done.quote, time: Math.floor(Date.now() / 1000), txid: done.txid, account: done.account });
    toast('success', t('Bet placed'), t('It shows in My bets with the next block (transaction %1).', shortHash(done.txid, 10)));
    void refreshHivemind();
    return true;
  } catch (e) {
    await inform(t('Place a bet'), errorText(e), 'error');
    return false;
  }
}

/** Cash out: sell every share of a state the wallet holds, account by account, as the trade panel sells. @return how many sales went through */
export async function cashOut(wallet: string, market: Market, state: number, answer: string, accounts: readonly { account: string; shares: number }[]): Promise<number> {
  const about = cashOutPreview(market, state, accounts);
  const r = await ask({
    title: t('Cash out'),
    message: t('Sell your bet on “%1” to the question “%2” now, for about %3?', answer, market.text, coins(about)),
    informative: t('The price moves with every bet: each sale takes at most 1% less than the price now, or nothing happens.'),
    level: 'question',
    choices: [
      { id: 'no', label: 'Cancel' },
      { id: 'yes', label: 'Cash out', kind: 'primary' },
    ],
  });
  if (r.choice !== 'yes') return 0;
  let sold = 0;
  for (const a of accounts) {
    const sharesText = a.shares.toFixed(8);
    try {
      const quote = -(await rpc.raw<number>('quotetrade', [market.id, state, `-${sharesText}`]));
      const bound = minProceeds(quote, SIMPLE_SLIPPAGE);
      const done = await walletRpc<{ txid: string; quote: number; proceeds: number }>(wallet, 'sellshares', [market.id, state, sharesText, a.account, bound.toFixed(8)]);
      if (!done) break;
      noteTrade({ wallet, market: market.id, state, shares: -a.shares, coins: done.proceeds, time: Math.floor(Date.now() / 1000), txid: done.txid, account: a.account });
      sold++;
    } catch (e) {
      await inform(t('Cash out'), errorText(e), 'error');
      break;
    }
  }
  if (sold) {
    toast('success', t('Cashed out'), t('The coins come with the next block.'));
    void refreshHivemind();
  }
  return sold;
}
