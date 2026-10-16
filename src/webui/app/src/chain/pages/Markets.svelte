<script lang="ts">
  // The markets: every market of the chain as a card, to search, filter (status, kind, category, the
  // wallet's) and sort; a card opens the market, its chances over time and the panel to trade in it
  // (hm/MarketView.svelte). The Markets tab of the Qt window of Hivemind, redone.
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { go, nav } from '../../lib/nav.svelte';
  import { wallets } from '../../lib/state.svelte';
  import '../hm/hm.css';
  import { barWidth } from '../hm/actions';
  import Countdown from '../hm/Countdown.svelte';
  import MarketView from '../hm/MarketView.svelte';
  import OutcomeName from '../hm/OutcomeName.svelte';
  import PriceChart from '../hm/PriceChart.svelte';
  import { categoryOf, formatChance, formatUnits, marketKind, outcomesOf, periodEnd, titleWithoutCategory, volume, type Market, type MarketKind } from '../hm/model';
  import { decisionMap, hm, priceSeries, startHivemind, ownItems } from '../hm/store.svelte';
  import SimpleMarket from '../hm/SimpleMarket.svelte';
  import SimpleMarkets from './SimpleMarkets.svelte';
  import { simpleMode } from '../../lib/uimode.svelte';

  $effect(() => startHivemind());

  const openId = $derived(nav.tab.startsWith('m-') ? nav.tab.slice(2) : '');
  const open = (id: string) => go('markets', `m-${id}`);

  type StatusFilter = 'trading' | 'closed' | 'settled' | 'all';
  /** null until chosen: the markets that trade, or all of them when none does. */
  let chosenStatus = $state<StatusFilter | null>(null);
  let kind = $state<MarketKind | 'all'>('all');
  let category = $state('');
  let onlyMine = $state(false);
  let search = $state('');
  let sort = $state<'newest' | 'volume' | 'liquidity' | 'closing' | 'uncertain'>('newest');

  const decisions = $derived(decisionMap());
  const mineIds = $derived(new Set([...hm.positions.map((p) => p.market), ...ownItems(wallets.current)]));
  const counts = $derived({
    trading: hm.markets.filter((m) => m.status === 'trading').length,
    closed: hm.markets.filter((m) => m.status === 'closed').length,
    settled: hm.markets.filter((m) => m.status === 'settled').length,
    all: hm.markets.length,
  });
  const status = $derived<StatusFilter>(chosenStatus ?? (counts.trading || !counts.all ? 'trading' : 'all'));
  const categories = $derived([...new Set(hm.markets.map((m) => categoryOf(m.text)).filter((c): c is string => !!c))].sort());

  function uncertainty(m: Market): number {
    const ps = m.states.map((s) => s.price ?? 0);
    return -ps.reduce((n, p) => n + (p > 0 ? p * Math.log(p) : 0), 0) / Math.log(Math.max(2, ps.length));
  }

  const shown = $derived.by(() => {
    const q = search.trim().toLowerCase();
    const list = hm.markets.filter((m) => {
      if (status !== 'all' && m.status !== status) return false;
      if (kind !== 'all' && marketKind(m, decisions) !== kind) return false;
      if (category && categoryOf(m.text) !== category) return false;
      if (onlyMine && !mineIds.has(m.id)) return false;
      if (q && !m.text.toLowerCase().includes(q) && !m.id.startsWith(q) && !m.decisions.some((d) => (decisions.get(d)?.text ?? '').toLowerCase().includes(q))) return false;
      return true;
    });
    const by: Record<typeof sort, (a: Market, b: Market) => number> = {
      newest: (a, b) => b.height - a.height,
      volume: (a, b) => volume(b) - volume(a),
      liquidity: (a, b) => b.liquidity - a.liquidity,
      closing: (a, b) => (a.tradinguntil ?? Infinity) - (b.tradinguntil ?? Infinity),
      uncertain: (a, b) => uncertainty(b) - uncertainty(a),
    };
    return list.sort(by[sort]);
  });

  const totalVolume = $derived(hm.markets.reduce((n, m) => n + volume(m), 0));
  const worth = $derived(hm.positions.reduce((n, p) => n + p.shares * (hm.markets.find((m) => m.id === p.market)?.states[p.state]?.price ?? 0), 0));
</script>

<!-- Simple mode (lib/uimode.svelte.ts): the questions as cards, one opened with Yes and No (SimpleMarkets, SimpleMarket). -->
{#if openId && simpleMode()}
  {#key openId}<SimpleMarket id={openId} back={() => go('markets')} />{/key}
{:else if openId}
  {#key openId}<MarketView id={openId} back={() => go('markets')} />{/key}
{:else if simpleMode()}
  <SimpleMarkets {open} />
{:else}
  <div class="page markets">
    <header class="page-head head">
      <div>
        <h1>Markets</h1>
        {#if hm.info}
          <p class="muted sub">
            Period {hm.info.period} · {hm.info.phase === 'reveal' ? 'votes are being revealed' : 'votes are being sealed'} · it <Countdown info={hm.info} target={periodEnd(hm.info, hm.info.period)} phrase="ends" />
          </p>
        {/if}
      </div>
      <div class="row">
        <button type="button" class="btn" onclick={() => go('portfolio')}><Icon name="wallet" size={15} /> Portfolio</button>
        <button type="button" class="btn primary" onclick={() => go('create', 'market')}><Icon name="plus" size={15} /> Create a market</button>
      </div>
    </header>

    {#if hm.error}<p class="side-note side-bad" role="alert"><SafeText text={hm.error} /></p>{/if}

    <section class="hm-figures">
      <div class="panel hm-figure"><span class="muted">Trading</span><strong class="num">{counts.trading}</strong><span class="faint small">of {counts.all} markets</span></div>
      <div class="panel hm-figure"><span class="muted">Shares sold</span><strong class="num">{formatUnits(totalVolume, 2)}</strong><span class="faint small">all markets</span></div>
      <div class="panel hm-figure"><span class="muted">For the voters</span><strong class="num">{formatUnits(hm.info?.voterpot ?? 0, 4)}</strong><span class="faint small">fees of the next vote</span></div>
      <div class="panel hm-figure"><span class="muted">Your shares are worth</span><strong class="num">{formatUnits(worth, 4)}</strong><span class="faint small">{hm.positions.length} position(s)</span></div>
    </section>

    <section class="panel filters" aria-label="Find a market">
      <div class="searchbox">
        <Icon name="search" size={16} />
        <input type="search" placeholder="Search markets: words of the title or of a decision, an id" bind:value={search} aria-label="Search markets" />
      </div>
      <div class="hm-chips" role="group" aria-label="Status">
        <button type="button" class="hm-chip" aria-pressed={status === 'trading'} onclick={() => (chosenStatus = 'trading')}>Trading <span class="count">{counts.trading}</span></button>
        <button type="button" class="hm-chip" aria-pressed={status === 'closed'} onclick={() => (chosenStatus = 'closed')}>Being voted on <span class="count">{counts.closed}</span></button>
        <button type="button" class="hm-chip" aria-pressed={status === 'settled'} onclick={() => (chosenStatus = 'settled')}>Paid out <span class="count">{counts.settled}</span></button>
        <button type="button" class="hm-chip" aria-pressed={status === 'all'} onclick={() => (chosenStatus = 'all')}>All <span class="count">{counts.all}</span></button>
      </div>
      <div class="hm-chips" role="group" aria-label="Kind">
        <button type="button" class="hm-chip" aria-pressed={kind === 'all'} onclick={() => (kind = 'all')}>Every kind</button>
        <button type="button" class="hm-chip" aria-pressed={kind === 'binary'} onclick={() => (kind = 'binary')}>Yes or no</button>
        <button type="button" class="hm-chip" aria-pressed={kind === 'scalar'} onclick={() => (kind = 'scalar')}>Scale</button>
        <button type="button" class="hm-chip" aria-pressed={kind === 'categorical'} onclick={() => (kind = 'categorical')}>Several outcomes</button>
        <button type="button" class="hm-chip" aria-pressed={onlyMine} onclick={() => (onlyMine = !onlyMine)}><Icon name="wallet" size={13} /> Mine</button>
      </div>
      <div class="selects">
        <label class="pick">
          <span class="muted">Category</span>
          <select class="pill" bind:value={category}>
            <option value="">All</option>
            {#each categories as c (c)}<option value={c} translate="no">{c}</option>{/each}
          </select>
        </label>
        <label class="pick">
          <span class="muted">Sort</span>
          <select class="pill" bind:value={sort}>
            <option value="newest">Newest</option>
            <option value="volume">Most traded</option>
            <option value="liquidity">Deepest</option>
            <option value="closing">Closing soonest</option>
            <option value="uncertain">Most uncertain</option>
          </select>
        </label>
      </div>
    </section>

    {#if !hm.loaded}
      <p class="hm-empty">Asking the node…</p>
    {:else if shown.length === 0}
      <div class="panel none">
        <p class="hm-empty">{hm.markets.length === 0 ? 'No market yet. Open the first one: list a decision, then a market on it.' : 'No market matches.'}</p>
        {#if hm.markets.length === 0}<button type="button" class="btn primary" onclick={() => go('create')}><Icon name="plus" size={15} /> Create</button>{/if}
      </div>
    {:else}
      <p class="muted small count-line">{shown.length} market(s)</p>
      <ul class="hm-grid cards" aria-label="Markets">
        {#each shown as m (m.id)}
          {@const outs = outcomesOf(m, decisions)}
          {@const top = [...outs].filter((o) => o.exclusive).sort((a, b) => (m.states[b.state]?.price ?? 0) - (m.states[a.state]?.price ?? 0)).slice(0, 3)}
          {@const cat = categoryOf(m.text)}
          {@const k = marketKind(m, decisions)}
          <li><button type="button" class="panel card" onclick={() => open(m.id)}>
            <span class="top">
              <span class="hm-stage" class:live={m.status === 'trading'} class:vote={m.status === 'closed'} class:done={m.status === 'settled'}>{m.status === 'trading' ? 'Live' : m.status === 'closed' ? 'Voting' : 'Paid out'}</span>
              <span class="badge">{k === 'binary' ? 'Yes or no' : k === 'scalar' ? 'Scale' : `${outs.filter((o) => o.exclusive).length} outcomes`}</span>
              {#if cat}<span class="badge"><SafeText text={cat} /></span>{/if}
              {#if mineIds.has(m.id)}<span class="badge accent">yours</span>{/if}
            </span>
            <strong class="title"><SafeText text={titleWithoutCategory(m.text)} /></strong>
            {#if m.status !== 'settled'}
              <ul class="odds">
                {#each top as o (o.state)}
                  <li class="hm-c{o.state % 8}">
                    <span class="name"><span class="hm-dot hm-c{o.state % 8}"></span><OutcomeName outcome={o} /></span>
                    <span class="num pct">{formatChance(m.states[o.state]?.price)}</span>
                    <span class="hm-bar"><span use:barWidth={m.states[o.state]?.price ?? 0}></span></span>
                  </li>
                {/each}
              </ul>
              <div class="spark"><PriceChart points={priceSeries(m)} now={m.states.map((s) => s.price ?? 0)} height={hm.height} compact tall={46} label="The chances of the market over time" /></div>
            {/if}
            <span class="foot faint small">
              <span class="num">{formatUnits(volume(m), 2)} shares sold</span>
              <span class="num">liquidity {formatUnits(m.liquidity, 2)}</span>
              {#if m.status === 'trading' && m.tradinguntil !== undefined && hm.info}<span><Countdown info={hm.info} target={periodEnd(hm.info, m.tradinguntil)} phrase="closes" /></span>{/if}
            </span>
          </button></li>
        {/each}
      </ul>
    {/if}
  </div>
{/if}

<style>
  .markets {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .head {
    display: flex;
    flex-wrap: wrap;
    align-items: flex-end;
    justify-content: space-between;
    gap: 0.8rem;
  }
  .sub {
    margin: 0.25rem 0 0;
  }
  .filters {
    display: flex;
    flex-direction: column;
    gap: 0.7rem;
    padding: 0.9rem 1rem;
  }
  .searchbox {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    padding: 0 0.7rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    background: var(--surface-2);
    color: var(--text-muted);
  }
  .searchbox input {
    flex: 1;
    min-width: 0;
    border: 0;
    background: none;
    box-shadow: none;
  }
  .selects {
    display: flex;
    flex-wrap: wrap;
    gap: 0.8rem;
  }
  .pick {
    display: flex;
    align-items: center;
    gap: 0.45rem;
    font-size: 0.88rem;
  }
  .none {
    display: flex;
    flex-direction: column;
    align-items: center;
    padding-bottom: 1rem;
  }
  .none .hm-empty {
    align-self: stretch;
  }
  .count-line {
    margin: 0;
  }
  .cards {
    list-style: none;
    margin: 0;
    padding: 0;
  }
  .cards > li {
    display: flex;
    min-width: 0;
  }
  .card {
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    padding: 1rem;
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
    min-width: 0;
    transition:
      border-color 160ms,
      transform 160ms;
  }
  .card:hover,
  .card:focus-visible {
    border-color: var(--accent);
  }
  .top {
    display: flex;
    flex-wrap: wrap;
    gap: 0.35rem;
    align-items: center;
  }
  .title {
    font-size: 1.02rem;
    line-height: 1.35;
    overflow-wrap: anywhere;
  }
  .odds {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 0.4rem;
  }
  .odds li {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    gap: 0.2rem 0.6rem;
    align-items: center;
  }
  .odds .name {
    display: flex;
    align-items: center;
    gap: 0.4rem;
    min-width: 0;
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
  }
  .odds .pct {
    font-weight: 700;
  }
  .odds .hm-bar {
    grid-column: 1 / -1;
  }
  .spark {
    margin: 0 -0.2rem;
  }
  .foot {
    display: flex;
    flex-wrap: wrap;
    gap: 0.3rem 0.9rem;
    margin-top: auto;
  }
  .small {
    font-size: 0.82rem;
  }
</style>
