<script lang="ts">
  import { selectable, stacked } from '../../lib/actions';
  // The shares of the wallet: every position, what it is worth at the price now, what it pays if its
  // outcome comes true, what it cost and what it made (of the trades this app placed: the chain keeps
  // no history of trades); and those trades. The "My positions" tab of the Qt window, with the profit
  // and loss it did not have.
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import NoWallet from '../../components/NoWallet.svelte';
  import { formatTime, shortHash } from '../../lib/format';
  import { go, PAGES } from '../../lib/nav.svelte';
  import { wallets, walletsEnabled } from '../../lib/state.svelte';
  import '../hm/hm.css';
  import OutcomeName from '../hm/OutcomeName.svelte';
  import { formatChance, formatUnits, heldByOutcome, outcomesOf, pnlOf, titleWithoutCategory } from '../hm/model';
  import { decisionMap, hm, openTrade as intend, startHivemind, tradesOf } from '../hm/store.svelte';
  import SimpleBets from './SimpleBets.svelte';
  import { simpleMode } from '../../lib/uimode.svelte';

  /** Open the market of a position on a side of its outcome. */
  function openTrade(market: string, state: number, side: 'buy' | 'sell') {
    intend(market, state, side);
    go('markets', `m-${market}`);
  }

  $effect(() => startHivemind());

  const noWallet = $derived(!walletsEnabled() || wallets.current === null);
  const decisions = $derived(decisionMap());
  // One row an outcome of a market: the chain keeps the shares by account, and each purchase goes to a new one.
  const rows = $derived(
    heldByOutcome(hm.positions)
      .map((p) => {
        const m = hm.markets.find((x) => x.id === p.market);
        const price = m?.states[p.state]?.price ?? 0;
        const outcome = m ? outcomesOf(m, decisions).find((o) => o.state === p.state) : undefined;
        return { ...p, m, price, outcome, pnl: pnlOf(p.shares, price, tradesOf(wallets.current, p.market, p.state)) };
      })
      .sort((a, b) => b.pnl.worth - a.pnl.worth),
  );
  const worth = $derived(rows.reduce((n, r) => n + r.pnl.worth, 0));
  const paysIfRight = $derived(rows.reduce((n, r) => n + r.shares, 0));
  const costed = $derived(rows.filter((r) => r.pnl.pnl !== null));
  const pnl = $derived(costed.reduce((n, r) => n + (r.pnl.pnl ?? 0), 0));
  const trades = $derived(hm.journal.trades.filter((x) => x.wallet === wallets.current).slice().reverse());
  const marketText = (id: string) => hm.markets.find((m) => m.id === id)?.text ?? '';
</script>

<!-- Simple mode (lib/uimode.svelte.ts): My bets, in plain words (SimpleBets). -->
{#if simpleMode()}
<SimpleBets />
{:else}
<div class="page portfolio">
  <header class="page-head"><h1>Portfolio</h1></header>

  {#if noWallet}
    <NoWallet page={PAGES.portfolio} />
  {:else}
    <section class="hm-figures">
      <div class="panel hm-figure"><span class="muted">Worth now</span><strong class="num">{formatUnits(worth, 4)}</strong><span class="faint small">at the chances of the markets</span></div>
      <div class="panel hm-figure"><span class="muted">Positions</span><strong class="num">{rows.length}</strong><span class="faint small">in {new Set(rows.map((r) => r.market)).size} market(s)</span></div>
      <div class="panel hm-figure"><span class="muted">Profit or loss</span><strong class="num" class:hm-up={pnl > 0} class:hm-down={pnl < 0}>{costed.length ? `${pnl >= 0 ? '+' : ''}${formatUnits(pnl, 4)}` : '–'}</strong><span class="faint small">of the trades placed in this app</span></div>
      <div class="panel hm-figure"><span class="muted">At most it pays</span><strong class="num">{formatUnits(paysIfRight, 4)}</strong><span class="faint small">if every outcome held comes true</span></div>
    </section>

    <section class="panel">
      <div class="panel-head"><h2>Positions</h2><button type="button" class="btn ghost small" onclick={() => go('markets')}><Icon name="search" size={14} /> Markets</button></div>
      {#if rows.length === 0}
        <p class="hm-empty">This wallet holds no shares. Buy some in a market.</p>
      {:else}
        <div class="hm-wrap">
          <table class="hm-table" use:stacked aria-label="Positions">
            <thead><tr><th>Market</th><th>Outcome</th><th class="end">Shares</th><th class="end" title="What a share bought cost on average, fee included">Avg. price</th><th class="end">Chance</th><th class="end">Worth now</th><th class="end">Profit or loss</th><th>Stage</th><th><span class="sr-only">Trade</span></th></tr></thead>
            <tbody>
              {#each rows as r (`${r.market}:${r.state}`)}
                <tr class="click" onclick={() => openTrade(r.market, r.state, 'buy')} use:selectable={() => openTrade(r.market, r.state, 'buy')}>
                  <td class="cut"><SafeText text={titleWithoutCategory(r.m?.text ?? r.market)} /></td>
                  <td>{#if r.outcome}<OutcomeName outcome={r.outcome} />{/if}</td>
                  <td class="end num">{formatUnits(r.shares)}</td>
                  <td class="end num" title={r.pnl.basis === null ? undefined : `Paid ${formatUnits(r.pnl.basis, 4)} for the shares held, fee included`}>{r.pnl.avgPrice === null ? '–' : formatUnits(r.pnl.avgPrice, 4)}</td>
                  <td class="end num">{formatChance(r.price)}</td>
                  <td class="end num">{formatUnits(r.pnl.worth, 4)}</td>
                  <td class="end num" class:hm-up={(r.pnl.pnl ?? 0) > 0} class:hm-down={(r.pnl.pnl ?? 0) < 0}>{r.pnl.pnl === null ? '–' : `${r.pnl.pnl >= 0 ? '+' : ''}${formatUnits(r.pnl.pnl, 4)}`}{#if r.pnl.cost !== null && !r.pnl.complete}<span class="faint" title="Some of the shares were traded outside this app"> *</span>{/if}</td>
                  <td><span class="hm-stage" class:live={r.m?.status === 'trading'} class:vote={r.m?.status === 'closed'}>{r.m?.status === 'trading' ? 'Trading' : 'Being voted on'}</span></td>
                  <td class="end">{#if r.m?.status === 'trading'}<button type="button" class="btn small ghost" onclick={(e) => { e.stopPropagation(); openTrade(r.market, r.state, 'sell'); }}>Sell…</button>{/if}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
        <p class="hm-help pad">Worth now is what the shares would bring at the chance the market gives their outcome; selling many moves the chance. The average price is what a share bought cost, fee included; the profit or loss counts the trades placed in this app at that average; – where it placed none.</p>
      {/if}
    </section>

    <section class="panel">
      <div class="panel-head"><h2>Trades</h2><span class="muted small">placed in this app</span></div>
      {#if trades.length === 0}
        <p class="hm-empty">No trade yet.</p>
      {:else}
        <div class="hm-wrap">
          <table class="hm-table" use:stacked aria-label="Trades">
            <thead><tr><th>Time</th><th>Market</th><th>Order</th><th class="end">Shares</th><th class="end">Coins</th><th class="end">Average</th><th>Transaction</th></tr></thead>
            <tbody>
              {#each trades as x (x.txid)}
                {@const m = hm.markets.find((y) => y.id === x.market)}
                {@const o = m ? outcomesOf(m, decisions).find((y) => y.state === x.state) : undefined}
                <tr class="click" onclick={() => go('markets', `m-${x.market}`)} use:selectable={() => go('markets', `m-${x.market}`)}>
                  <td>{formatTime(x.time, true)}</td>
                  <td class="cut"><SafeText text={titleWithoutCategory(marketText(x.market) || x.market)} /></td>
                  <td><span class="status {x.shares > 0 ? 'ok' : 'bad'}">{x.shares > 0 ? 'Buy' : 'Sell'}</span> {#if o}<OutcomeName outcome={o} />{/if}</td>
                  <td class="end num">{formatUnits(Math.abs(x.shares))}</td>
                  <td class="end num">{x.shares > 0 ? '−' : '+'}{formatUnits(x.coins, 4)}</td>
                  <td class="end num">{formatUnits(x.coins / Math.abs(x.shares), 4)}</td>
                  <td class="mono"><span translate="no">{shortHash(x.txid, 8)}</span></td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </section>
  {/if}
</div>
{/if}

<style>
  .portfolio {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .pad {
    padding: 0.6rem 1rem 0.9rem;
  }
  .small {
    font-size: 0.82rem;
  }
</style>
