<script lang="ts">
  // Trading in a market: buy or sell shares of one of its states. The cost is worked out here as the
  // market maker of the chain works it out (LMSR, model.ts) as the amount is typed, and quoted by the
  // node (quotetrade) before the order is placed; the order carries the most it pays (or the least it
  // takes) with the slippage chosen, so that a price that moved meanwhile cannot cost more than that.
  import { t } from '../../lib/i18n.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import { shortHash } from '../../lib/format';
  import OutcomeName from './OutcomeName.svelte';
  import { chainQuestion, describeOutcome, formatChance, formatUnits, LIMITS, maxCost, minProceeds, previewTrade, sharesForBudget, type Market, type Outcome, type WalletPosition } from './model';
  import { decisionMap, noteTrade, refreshHivemind } from './store.svelte';

  let {
    market,
    outcomes,
    chosen = $bindable(0),
    side = $bindable<'buy' | 'sell'>('buy'),
    held,
  }: { market: Market; outcomes: Outcome[]; chosen?: number; side?: 'buy' | 'sell'; held: WalletPosition[] } = $props();

  const SLIPPAGES = [0.005, 0.01, 0.02, 0.05];
  let slippage = $state(0.01);
  let mode = $state<'coins' | 'shares'>('coins');
  let amountText = $state('');
  let busy = $state(false);
  let quote = $state<{ key: string; value: number | null; error: string } | null>(null);

  const trading = $derived(market.status === 'trading');
  const amount = $derived(Number(amountText.replace(',', '.')));
  const valid = $derived(amountText.trim() !== '' && Number.isFinite(amount) && amount > 0);
  /** The shares of the chosen state the wallet holds, and in which account (the one that holds most). */
  const mine = $derived(held.filter((p) => p.state === chosen).sort((a, b) => b.shares - a.shares));
  const holding = $derived(mine.reduce((n, p) => n + p.shares, 0));
  let account = $state('');
  $effect(() => {
    if (!mine.some((p) => p.account === account)) account = mine[0]?.account ?? '';
  });
  const inAccount = $derived(mine.find((p) => p.account === account)?.shares ?? 0);

  /** The shares of the order. */
  const shares = $derived.by(() => {
    if (!valid) return 0;
    if (side === 'sell') return Math.min(amount, inAccount);
    return mode === 'shares' ? amount : sharesForBudget(market, chosen, amount);
  });
  const preview = $derived(shares > 0 ? previewTrade(market, chosen, side === 'buy' ? shares : -shares) : null);
  const sharesText = $derived(shares.toFixed(8));

  // The node's quote, a moment after the last keystroke.
  $effect(() => {
    const key = `${market.id}|${chosen}|${side}|${sharesText}`;
    if (!trading || shares <= 0) {
      quote = null;
      return;
    }
    const timer = setTimeout(() => {
      rpc
        .raw<number>('quotetrade', [market.id, chosen, side === 'buy' ? sharesText : `-${sharesText}`])
        .then((v) => (quote = { key, value: side === 'buy' ? v : -v, error: '' }))
        .catch((e) => (quote = { key, value: null, error: errorText(e) }));
    }, 250);
    return () => clearTimeout(timer);
  });
  const quoted = $derived(quote && quote.key === `${market.id}|${chosen}|${side}|${sharesText}` ? quote : null);
  const total = $derived(quoted?.value ?? preview?.total ?? 0);
  const bound = $derived(side === 'buy' ? maxCost(total, slippage) : minProceeds(total, slippage));

  const problem = $derived.by(() => {
    if (!trading) return '';
    if (amountText.trim() !== '' && !valid) return t('Enter an amount above zero.');
    if (!valid) return '';
    if (side === 'sell' && holding <= 0) return t('This wallet holds no shares of this outcome to sell.');
    if (side === 'sell' && amount > inAccount + 1e-12) return t('The account holds %1 shares of this outcome.', formatUnits(inAccount));
    if (side === 'buy' && shares <= 0) return t('That buys no shares.');
    if (side === 'buy' && holding <= 0 && total < LIMITS.minPosition) return t('A purchase that opens a position costs at least %1.', String(LIMITS.minPosition));
    if (quoted?.error) return quoted.error;
    return '';
  });

  function chips(): { display: string; value: string }[] {
    if (side === 'sell') {
      return [0.25, 0.5, 1].map((f) => ({ display: f === 1 ? t('All') : `${f * 100}%`, value: (Math.floor(inAccount * f * 1e8) / 1e8).toString() }));
    }
    return (mode === 'coins' ? [0.01, 0.1, 1, 10] : [1, 10, 100, 1000]).map((v) => ({ display: String(v), value: String(v) }));
  }

  async function place() {
    const wallet = wallets.current;
    if (wallet === null || !preview || problem) return;
    const o = outcomes.find((x) => x.state === chosen);
    const name = o ? describeOutcome(o) : String(chosen);
    // The question of the chain next to the title: the title is free text, the decision is what pays.
    const asked = chainQuestion(market, decisionMap());
    const before = inAccount;
    const r = await ask({
      title: side === 'buy' ? t('Buy shares') : t('Sell shares'),
      message:
        side === 'buy'
          ? t('Buy %1 shares of “%2” on the question “%3” (market “%4”) for about %5? The order pays at most %6.', formatUnits(shares), name, asked, market.text, formatUnits(total), formatUnits(bound))
          : t('Sell %1 shares of “%2” on the question “%3” (market “%4”) for about %5? The order takes at least %6.', formatUnits(shares), name, asked, market.text, formatUnits(total), formatUnits(bound)),
      informative: t('The question is the decision on the chain: its answer decides what a share pays, whatever the title of the market says. Prices move with every trade. If the price moved by more than the slippage when the transaction is mined, nothing is traded and the coins come back.'),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: side === 'buy' ? 'Buy' : 'Sell', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      if (side === 'buy') {
        const done = await walletRpc<{ txid: string; account: string; quote: number; maxcost: number }>(wallet, 'buyshares', [market.id, chosen, sharesText, bound.toFixed(8)]);
        if (!done) return;
        noteTrade({ wallet, market: market.id, state: chosen, shares, coins: done.quote, time: Math.floor(Date.now() / 1000), txid: done.txid, account: done.account });
        toast('success', t('Order placed'), t('Bought %1 shares, in the transaction %2. They show with the next block.', formatUnits(shares), shortHash(done.txid, 10)));
      } else {
        const done = await walletRpc<{ txid: string; quote: number; proceeds: number }>(wallet, 'sellshares', [market.id, chosen, sharesText, account, bound.toFixed(8)]);
        if (!done) return;
        noteTrade({ wallet, market: market.id, state: chosen, shares: -shares, coins: done.proceeds, time: Math.floor(Date.now() / 1000), txid: done.txid, account, before });
        toast('success', t('Order placed'), t('Sold %1 shares, in the transaction %2. The proceeds come with the next block.', formatUnits(shares), shortHash(done.txid, 10)));
      }
      amountText = '';
      void refreshHivemind();
    } catch (e) {
      await inform(side === 'buy' ? t('Buy shares') : t('Sell shares'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
</script>

<section class="panel trade" aria-label="Trade">
  <div class="sides" role="tablist" aria-label="Buy or sell">
    <button type="button" role="tab" aria-selected={side === 'buy'} class="buy" onclick={() => (side = 'buy')}>Buy</button>
    <button type="button" role="tab" aria-selected={side === 'sell'} class="sell" onclick={() => (side = 'sell')}>Sell</button>
  </div>

  {#if !trading}
    <p class="hm-help closed">
      {market.status === 'closed' ? 'Trading is closed: the decisions are being voted on. Shares are neither bought nor sold until the market pays out.' : 'The market has paid out: every share got what its outcome was worth.'}
    </p>
  {:else}
    <div class="body">
      <span class="caps">Outcome</span>
      <div class="pick" role="radiogroup" aria-label="Outcome">
        {#each outcomes as o (o.state)}
          <button type="button" role="radio" aria-checked={chosen === o.state} class="opt" onclick={() => (chosen = o.state)}>
            <span class="hm-dot hm-c{o.state % 8}"></span>
            <span class="name"><OutcomeName outcome={o} /></span>
            <span class="num faint">{formatChance(market.states[o.state]?.price)}</span>
          </button>
        {/each}
      </div>

      {#if side === 'buy'}
        <div class="modes" role="group" aria-label="Amount in">
          <button type="button" class="hm-chip" aria-pressed={mode === 'coins'} onclick={() => (mode = 'coins')}>Coins to spend</button>
          <button type="button" class="hm-chip" aria-pressed={mode === 'shares'} onclick={() => (mode = 'shares')}>Shares to buy</button>
        </div>
      {:else if mine.length > 1}
        <label class="field">
          <span>Account</span>
          <select bind:value={account}>
            {#each mine as p (p.account)}<option value={p.account} translate="no">{p.account.slice(0, 14)}… · {formatUnits(p.shares)}</option>{/each}
          </select>
        </label>
      {/if}

      <label class="field">
        <span>{side === 'sell' ? 'Shares to sell' : mode === 'coins' ? 'Coins to spend' : 'Shares to buy'}</span>
        <input id="trade-amount" type="text" inputmode="decimal" autocomplete="off" placeholder="0.00" bind:value={amountText} />
      </label>
      <div class="hm-chips">
        {#each chips() as c (c.display)}<button type="button" class="hm-chip" onclick={() => (amountText = c.value)}>{c.display}</button>{/each}
      </div>
      <p class="hm-help">{holding > 0 ? `You hold ${formatUnits(holding)} shares of this outcome.` : 'You hold no shares of this outcome.'}</p>

      <div class="slip">
        <span class="caps">Slippage</span>
        <div class="hm-chips" role="group" aria-label="Slippage">
          {#each SLIPPAGES as s (s)}<button type="button" class="hm-chip" aria-pressed={slippage === s} onclick={() => (slippage = s)}>{s * 100}%</button>{/each}
        </div>
      </div>

      {#if preview}
        <dl class="facts preview" aria-label="Preview of the trade">
          <dt>Shares</dt><dd class="num">{formatUnits(Math.abs(preview.shares))}</dd>
          <dt>Average price</dt><dd class="num">{formatUnits(Math.abs(total / preview.shares), 4)}</dd>
          <dt>Price impact</dt><dd class="num" class:hm-down={preview.impact > 0.05}>{formatChance(preview.impact)}</dd>
          <dt>Trading fee ({(market.feebps / 100).toFixed(2)}%)</dt><dd class="num">{formatUnits(preview.fee)}</dd>
          <dt>Chance</dt><dd class="num">{formatChance(preview.priceBefore)} → {formatChance(preview.priceAfter)}</dd>
          <dt class="strong">{side === 'buy' ? 'You pay' : 'You get'}</dt><dd class="num strong">{formatUnits(total)} {quoted?.value != null ? '' : '≈'}</dd>
          <dt>{side === 'buy' ? 'At most, with slippage' : 'At least, with slippage'}</dt><dd class="num">{formatUnits(bound)}</dd>
          {#if side === 'buy'}
            <dt>Pays if right</dt><dd class="num">{formatUnits(preview.shares)}</dd>
            <dt>Gain if right</dt><dd class="num hm-up">+{formatUnits(preview.shares - total)}</dd>
          {/if}
        </dl>
        <p class="hm-help quote">{quoted?.value != null ? 'Quoted by the node just now.' : quoted?.error ? '' : 'Worked out here; the node quotes it next.'}</p>
      {/if}

      {#if problem}<p class="hm-problems" role="alert"><SafeText text={problem} /></p>{/if}
      {#if side === 'buy' && preview && preview.priceAfter > 0.98}<p class="hm-help">This much moves the chance close to certain: further shares cost almost a coin each.</p>{/if}

      <button type="button" id="trade-place" class="btn primary place" disabled={busy || !preview || !!problem || wallets.current === null} onclick={place}>
        <Icon name={side === 'buy' ? 'plus' : 'up'} size={16} />
        {side === 'buy' ? 'Buy' : 'Sell'}{#if preview} · {formatUnits(total)}{/if}
      </button>
      {#if wallets.current === null}<p class="hm-help">Open a wallet to trade.</p>{/if}
      <p class="hm-help">A share pays one coin if its outcome comes true, and nothing if not.</p>
    </div>
  {/if}
</section>

<style>
  .trade {
    display: flex;
    flex-direction: column;
    min-width: 0;
  }
  .sides {
    display: grid;
    grid-template-columns: 1fr 1fr;
    border-bottom: 1px solid var(--line);
  }
  .sides button {
    padding: 0.75rem;
    border: 0;
    border-bottom: 2px solid transparent;
    background: none;
    color: var(--text-muted);
    font: inherit;
    font-weight: 650;
    cursor: pointer;
  }
  .sides button[aria-selected='true'].buy {
    color: var(--text);
    border-bottom-color: var(--positive);
  }
  .sides button[aria-selected='true'].sell {
    color: var(--text);
    border-bottom-color: var(--negative);
  }
  .body {
    display: flex;
    flex-direction: column;
    gap: 0.7rem;
    padding: 1rem;
  }
  .closed {
    padding: 1rem;
  }
  .pick {
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
    max-height: 14rem;
    overflow-y: auto;
  }
  .opt {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    padding: 0.45rem 0.65rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    background: var(--surface-2);
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .opt[aria-checked='true'] {
    border-color: var(--accent);
    background: var(--accent-soft);
  }
  .opt .name {
    flex: 1;
    min-width: 0;
  }
  .modes {
    display: flex;
    gap: 0.35rem;
  }
  .field {
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
  }
  .field > span {
    font-size: 0.85rem;
    color: var(--text-muted);
  }
  .slip {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 0.4rem;
  }
  .preview {
    padding: 0.7rem 0.8rem;
    border-radius: var(--radius-m);
    background: var(--surface-2);
  }
  .preview .strong {
    font-weight: 700;
    color: var(--text);
  }
  .quote {
    margin-top: -0.4rem;
    font-size: 0.78rem;
  }
  .place {
    justify-content: center;
    min-height: 2.6rem;
    font-size: 1rem;
  }
</style>
