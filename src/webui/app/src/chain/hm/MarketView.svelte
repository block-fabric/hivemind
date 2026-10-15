<script lang="ts">
  // One market: what it is about, the chance of each of its outcomes now and over time, the decisions
  // it waits for, the positions of the wallet with what they made, who holds its shares; and the panel
  // to trade in it.
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import { go } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { wallets } from '../../lib/state.svelte';
  import Countdown from './Countdown.svelte';
  import OutcomeName from './OutcomeName.svelte';
  import PriceChart from './PriceChart.svelte';
  import TradePanel from './TradePanel.svelte';
  import { barWidth } from './actions';
  import { categoryOf, commonQuestion, formatChance, formatUnits, marketKind, outcomesOf, periodEnd, pnlOf, scalarEstimate, statePayout, titleWithoutCategory, volume, type Market } from './model';
  import { decisionMap, hm, priceSeries, tradesOf } from './store.svelte';

  let { id, back }: { id: string; back: () => void } = $props();

  let full = $state<Market | null>(null);
  let error = $state('');
  $effect(() => {
    void hm.version;
    const want = id;
    rpc
      .raw<Market>('getmarket', [want])
      .then((m) => {
        if (want === id) {
          full = m;
          error = '';
        }
      })
      .catch((e) => (error = e instanceof Error ? e.message : String(e)));
  });

  const market = $derived(full ?? hm.markets.find((m) => m.id === id) ?? null);
  const decisions = $derived(decisionMap());
  const outcomes = $derived(market ? outcomesOf(market, decisions) : []);
  const kind = $derived(market ? marketKind(market, decisions) : 'binary');
  const about = $derived(market ? market.decisions.map((d) => decisions.get(d)).filter((d) => d !== undefined) : []);
  const question = $derived(kind === 'categorical' ? commonQuestion(about.map((d) => d.text)) : '');
  const estimate = $derived(market && kind === 'scalar' && about[0] ? scalarEstimate(market, about[0]) : null);
  const category = $derived(market ? categoryOf(market.text) : null);
  const held = $derived(hm.positions.filter((p) => p.market === id));
  const prices = $derived(market ? market.states.map((s) => s.price ?? 0) : []);
  /** The outcomes in order: those a categorical market is about first, then the other combinations. */
  const shown = $derived([...outcomes].sort((a, b) => Number(b.exclusive) - Number(a.exclusive) || (market?.states[b.state]?.price ?? 0) - (market?.states[a.state]?.price ?? 0)));
  let showAll = $state(false);
  const visible = $derived(showAll || kind !== 'categorical' ? shown : shown.filter((o) => o.exclusive));
  let chosen = $state(-1);
  let side = $state<'buy' | 'sell'>('buy');
  $effect(() => {
    if (chosen < 0 && shown.length) chosen = shown[0].state;
  });

  /** What a settled market paid for a share of each state: the outcomes of its decisions. */
  const payouts = $derived.by(() => {
    if (!market || market.status !== 'settled') return null;
    const outs = about.map((d) => d.outcome ?? 0);
    if (outs.length !== market.decisions.length) return null;
    return market.states.map((s) => statePayout(outs, s.state));
  });

  const rows = $derived(
    held.map((p) => {
      const price = market?.states[p.state]?.price ?? 0;
      return { ...p, outcome: outcomes.find((o) => o.state === p.state), price, pnl: pnlOf(p.shares, price, tradesOf(wallets.current, id, p.state)) };
    }),
  );
  const holders = $derived(market?.positions ? [...market.positions].sort((a, b) => b.shares - a.shares).slice(0, 12) : []);
  const mineSet = $derived(new Set(held.map((p) => p.account)));
</script>

{#if !market}
  <div class="page">
    <button type="button" class="btn ghost" onclick={back}><Icon name="receive" size={15} /> All markets</button>
    <p class="hm-empty">{error ? error : 'Asking the node…'}</p>
  </div>
{:else}
  <div class="page market-view">
    <nav class="crumbs">
      <button type="button" class="btn ghost small" onclick={back}><Icon name="receive" size={15} /> All markets</button>
      {#if category}<span class="badge"><SafeText text={category} /></span>{/if}
      <span class="badge">{kind === 'binary' ? 'Yes or no' : kind === 'scalar' ? 'Scale' : 'Several outcomes'}</span>
    </nav>

    <header class="head">
      <div class="title">
        <h1><SafeText text={titleWithoutCategory(market.text)} /></h1>
        {#if question}<p class="muted"><SafeText text={question} /></p>{/if}
        <p class="sub">
          <span class="hm-stage" class:live={market.status === 'trading'} class:vote={market.status === 'closed'} class:done={market.status === 'settled'}>{market.status === 'trading' ? 'Trading' : market.status === 'closed' ? 'Being voted on' : 'Paid out'}</span>
          {#if market.status === 'trading' && market.tradinguntil !== undefined && hm.info}
            <span class="muted">Trading closes in <Countdown info={hm.info} target={periodEnd(hm.info, market.tradinguntil)} /> (end of period {market.tradinguntil})</span>
          {:else if market.status === 'closed'}
            <button type="button" class="btn ghost small" onclick={() => go('voting')}>To the voting center</button>
          {/if}
        </p>
      </div>
      <div class="id"><span class="mono faint" translate="no">{market.id.slice(0, 16)}…</span><CopyButton text={market.id} label="Copy the id of the market" /></div>
    </header>

    <section class="hm-figures">
      <div class="panel hm-figure"><span class="muted">Shares sold</span><strong class="num">{formatUnits(volume(market), 2)}</strong></div>
      <div class="panel hm-figure"><span class="muted">Liquidity</span><strong class="num">{formatUnits(market.liquidity, 4)}</strong></div>
      <div class="panel hm-figure"><span class="muted">The market holds</span><strong class="num">{formatUnits(market.treasury, 4)}</strong></div>
      <div class="panel hm-figure"><span class="muted">Fee · fees taken</span><strong class="num">{(market.feebps / 100).toFixed(2)}% · {formatUnits(market.fees, 4)}</strong></div>
    </section>

    <div class="cols">
      <div class="main">
        <section class="panel">
          <div class="panel-head"><h2>Chances</h2>{#if estimate !== null && about[0]}<span class="muted">The market puts it at <strong class="num">{estimate.toFixed(2)}</strong> on the scale from {about[0].min} to {about[0].max}</span>{/if}</div>
          <div class="panel-body">
            {#if market.status !== 'settled'}
              <PriceChart points={priceSeries(market)} now={prices} height={hm.height} {chosen} label="The chance of each outcome over time" />
              <p class="hm-help">As this app saw the prices move, block by block; a market opens at even odds.</p>
            {/if}
            <ul class="outcomes">
              {#each visible as o (o.state)}
                {@const st = market.states[o.state]}
                <li>
                  <button type="button" class="outcome" class:chosen={chosen === o.state} onclick={() => (chosen = o.state)}>
                    <span class="hm-dot hm-c{o.state % 8}"></span>
                    <span class="name"><OutcomeName outcome={o} /></span>
                    {#if payouts}
                      <span class="num strong">{formatUnits(payouts[o.state], 4)} per share</span>
                    {:else}
                      <span class="num strong">{formatChance(st?.price)}</span>
                    {/if}
                    <span class="hm-bar hm-c{o.state % 8}"><span use:barWidth={payouts ? payouts[o.state] : (st?.price ?? 0)}></span></span>
                    <span class="faint small num">{formatUnits(st?.shares ?? 0, 2)} shares sold</span>
                  </button>
                  {#if market.status === 'trading'}
                    <span class="acts">
                      <button type="button" class="btn small" onclick={() => { chosen = o.state; side = 'buy'; }}>Buy</button>
                      {#if held.some((p) => p.state === o.state)}<button type="button" class="btn small ghost" onclick={() => { chosen = o.state; side = 'sell'; }}>Sell</button>{/if}
                    </span>
                  {/if}
                </li>
              {/each}
            </ul>
            {#if kind === 'categorical' && shown.length !== visible.length}
              <button type="button" class="btn ghost small" onclick={() => (showAll = true)}>Show the other {shown.length - visible.length} combinations</button>
            {:else if kind === 'categorical' && showAll}
              <button type="button" class="btn ghost small" onclick={() => (showAll = false)}>Only the outcomes</button>
            {/if}
          </div>
        </section>

        {#if rows.length}
          <section class="panel">
            <div class="panel-head"><h2>Your positions</h2><button type="button" class="btn ghost small" onclick={() => go('portfolio')}>Portfolio</button></div>
            <div class="hm-wrap">
              <table class="hm-table" aria-label="Your positions in this market">
                <thead><tr><th>Outcome</th><th class="end">Shares</th><th class="end">Chance</th><th class="end">Worth now</th><th class="end">Paid</th><th class="end">Profit or loss</th><th></th></tr></thead>
                <tbody>
                  {#each rows as r (r.account + r.state)}
                    <tr>
                      <td>{#if r.outcome}<OutcomeName outcome={r.outcome} />{/if}</td>
                      <td class="end num">{formatUnits(r.shares)}</td>
                      <td class="end num">{formatChance(r.price)}</td>
                      <td class="end num">{formatUnits(r.pnl.worth, 4)}</td>
                      <td class="end num">{r.pnl.cost === null ? '–' : formatUnits(r.pnl.cost, 4)}</td>
                      <td class="end num" class:hm-up={(r.pnl.pnl ?? 0) > 0} class:hm-down={(r.pnl.pnl ?? 0) < 0}>{r.pnl.pnl === null ? '–' : `${r.pnl.pnl >= 0 ? '+' : ''}${formatUnits(r.pnl.pnl, 4)}`}{#if r.pnl.cost !== null && !r.pnl.complete}<span class="faint" title="Some of the shares were traded outside this app"> *</span>{/if}</td>
                      <td class="end">{#if market.status === 'trading'}<button type="button" class="btn small ghost" onclick={() => { chosen = r.state; side = 'sell'; }}>Sell</button>{/if}</td>
                    </tr>
                  {/each}
                </tbody>
              </table>
            </div>
          </section>
        {/if}

        <section class="panel">
          <div class="panel-head"><h2>Decisions</h2><span class="muted small">The market pays out once they are all resolved.</span></div>
          <ul class="decisions">
            {#each about as d (d.id)}
              <li>
                <span class="hm-stage" class:live={d.status === 'open'} class:vote={d.status === 'voting'} class:done={d.status === 'resolved'}>{d.status === 'open' ? 'Open' : d.status === 'voting' ? 'Voting' : 'Resolved'}</span>
                <span class="q"><SafeText text={d.text} /></span>
                <span class="faint small">
                  {d.scaled ? `A number from ${d.min} to ${d.max}` : 'Yes or no'} · {d.status === 'resolved' ? 'answer: ' : `known in period ${d.eventperiod}, voted on until period ${d.closeperiod}`}{#if d.status === 'resolved'}<SafeText text={String(d.answer ?? '')} />{/if}
                </span>
              </li>
            {/each}
          </ul>
        </section>

        {#if holders.length}
          <section class="panel">
            <div class="panel-head"><h2>Who holds shares</h2><span class="muted small">{market.positions?.length ?? 0} position(s)</span></div>
            <div class="hm-wrap">
              <table class="hm-table" aria-label="Who holds shares">
                <thead><tr><th>Account</th><th>Outcome</th><th class="end">Shares</th></tr></thead>
                <tbody>
                  {#each holders as h (h.account + h.state)}
                    {@const o = outcomes.find((x) => x.state === h.state)}
                    <tr>
                      <td class="cut mono"><span translate="no">{h.account}</span>{#if mineSet.has(h.account)} <span class="badge accent">yours</span>{/if}</td>
                      <td>{#if o}<OutcomeName outcome={o} />{/if}</td>
                      <td class="end num">{formatUnits(h.shares)}</td>
                    </tr>
                  {/each}
                </tbody>
              </table>
            </div>
          </section>
        {/if}
      </div>

      <aside class="side">
        {#if chosen >= 0}<TradePanel {market} outcomes={visible} bind:chosen bind:side {held} />{/if}
      </aside>
    </div>
  </div>
{/if}

<style>
  .market-view {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    container-type: inline-size;
  }
  .crumbs {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
  }
  .head {
    display: flex;
    flex-wrap: wrap;
    align-items: flex-start;
    justify-content: space-between;
    gap: 0.6rem 1rem;
  }
  .title {
    min-width: 0;
    flex: 1 1 24rem;
  }
  .title h1 {
    font-size: 1.55rem;
    line-height: 1.25;
    overflow-wrap: anywhere;
  }
  .sub {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.6rem;
    margin: 0.5rem 0 0;
  }
  .id {
    display: flex;
    align-items: center;
    gap: 0.3rem;
  }
  .cols {
    display: grid;
    grid-template-columns: minmax(0, 1fr) minmax(18rem, 22rem);
    gap: 1rem;
    align-items: start;
  }
  .main {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    min-width: 0;
  }
  .side {
    position: sticky;
    top: 0;
    min-width: 0;
  }
  .outcomes,
  .decisions {
    list-style: none;
    margin: 0.8rem 0 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
  }
  .outcomes li {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }
  .outcome {
    flex: 1;
    display: grid;
    grid-template-columns: auto minmax(0, 1fr) auto;
    grid-template-areas: 'dot name pct' 'bar bar bar' 'sold sold sold';
    align-items: center;
    gap: 0.25rem 0.55rem;
    padding: 0.55rem 0.7rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    background: var(--surface-2);
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
    min-width: 0;
  }
  .outcome.chosen {
    border-color: var(--accent);
  }
  .outcome .hm-dot {
    grid-area: dot;
  }
  .outcome .name {
    grid-area: name;
    min-width: 0;
  }
  .outcome .strong {
    grid-area: pct;
    font-weight: 700;
  }
  .outcome .hm-bar {
    grid-area: bar;
  }
  .outcome .small {
    grid-area: sold;
  }
  .acts {
    display: flex;
    flex-direction: column;
    gap: 0.25rem;
  }
  .decisions {
    padding: 0 1rem 1rem;
  }
  .decisions li {
    display: grid;
    grid-template-columns: auto minmax(0, 1fr);
    gap: 0.2rem 0.6rem;
    align-items: center;
  }
  .decisions .q {
    overflow-wrap: anywhere;
  }
  .decisions .small {
    grid-column: 2;
  }
  .small {
    font-size: 0.82rem;
  }
  @container (max-width: 820px) {
    .cols {
      grid-template-columns: minmax(0, 1fr);
    }
    .side {
      position: static;
    }
  }
</style>
