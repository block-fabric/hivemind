<script lang="ts">
  // The decisions: the questions the markets are about and the holders of reputation vote on. Every
  // decision, to search and filter (open, being voted on, resolved, listed by this wallet), a page at a
  // time; one opened shows its periods, its votes and the markets on it. And the calendar: the period
  // now, its phase, and the periods ahead with the decisions that come up in each. The Decisions and
  // Periods tabs of the Qt window of Hivemind.
  import { t } from '../../lib/i18n.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import { go, nav } from '../../lib/nav.svelte';
  import '../hm/hm.css';
  import { barWidth } from '../hm/actions';
  import Countdown from '../hm/Countdown.svelte';
  import { clockOf, periodEnd, revealStart, titleWithoutCategory, type Decision, type DecisionStatus } from '../hm/model';
  import { hm, startHivemind } from '../hm/store.svelte';
  import { node, wallets } from '../../lib/state.svelte';

  $effect(() => startHivemind());

  const TABS = [
    { id: 'list', label: 'Decisions' },
    { id: 'calendar', label: 'Calendar' },
  ];
  const tab = $derived(nav.tab === 'calendar' ? 'calendar' : 'list');

  let filter = $state<DecisionStatus | 'all'>('all');
  let onlyMine = $state(false);
  let search = $state('');
  let pageSize = $state(25);
  let pageNo = $state(0);
  let openId = $state<string | null>(null);

  const mine = $derived(new Set(wallets.current !== null ? (hm.journal.mine[wallets.current] ?? []) : []));
  const shown = $derived.by(() => {
    const q = search.trim().toLowerCase();
    return hm.decisions
      .filter((d) => (filter === 'all' || d.status === filter) && (!onlyMine || mine.has(d.id)) && (!q || d.text.toLowerCase().includes(q) || d.id.startsWith(q)))
      .sort((a, b) => b.height - a.height);
  });
  const pages = $derived(Math.max(1, Math.ceil(shown.length / pageSize)));
  $effect(() => {
    if (pageNo >= pages) pageNo = pages - 1;
  });
  const slice = $derived(shown.slice(pageNo * pageSize, pageNo * pageSize + pageSize));
  const counts = $derived({
    open: hm.decisions.filter((d) => d.status === 'open').length,
    voting: hm.decisions.filter((d) => d.status === 'voting').length,
    resolved: hm.decisions.filter((d) => d.status === 'resolved').length,
  });
  const marketsOn = (id: string) => hm.markets.filter((m) => m.decisions.includes(id));

  let now = $state(Math.floor(Date.now() / 1000));
  $effect(() => {
    const timer = setInterval(() => (now = Math.floor(Date.now() / 1000)), 1000);
    return () => clearInterval(timer);
  });
  const clock = $derived(hm.info ? clockOf(hm.info, now, node.tip?.height ?? 0) : null);
  /** The periods ahead with what comes up in them: answers known, votes that end. */
  const calendar = $derived.by(() => {
    if (!hm.info) return [];
    const p = hm.info.period;
    const out: { period: number; known: Decision[]; closing: Decision[] }[] = [];
    for (let n = p; n < p + 12; n++) {
      out.push({ period: n, known: hm.decisions.filter((d) => d.status !== 'resolved' && d.eventperiod === n), closing: hm.decisions.filter((d) => d.status !== 'resolved' && d.closeperiod === n) });
    }
    return out;
  });
</script>

<div class="page decisions">
  <header class="page-head head">
    <h1>Decisions</h1>
    <button type="button" class="btn primary" onclick={() => go('create', 'decision')}><Icon name="plus" size={15} /> List a decision</button>
  </header>

  {#if hm.error}<p class="side-note side-bad" role="alert"><SafeText text={hm.error} /></p>{/if}

  <div class="tabs" role="tablist" aria-label="Decisions">
    {#each TABS as tb (tb.id)}
      <button type="button" role="tab" aria-selected={tab === tb.id} onclick={() => (nav.tab = tb.id)}>{t(tb.label)}</button>
    {/each}
  </div>

  {#if tab === 'list'}
    <section class="panel filters">
      <div class="searchbox"><Icon name="search" size={16} /><input type="search" placeholder="Search decisions: words of the question, or an id" bind:value={search} aria-label="Search decisions" /></div>
      <div class="hm-chips" role="group" aria-label="Status">
        <button type="button" class="hm-chip" aria-pressed={filter === 'all'} onclick={() => (filter = 'all')}>All <span class="count">{hm.decisions.length}</span></button>
        <button type="button" class="hm-chip" aria-pressed={filter === 'open'} onclick={() => (filter = 'open')}>Open <span class="count">{counts.open}</span></button>
        <button type="button" class="hm-chip" aria-pressed={filter === 'voting'} onclick={() => (filter = 'voting')}>Being voted on <span class="count">{counts.voting}</span></button>
        <button type="button" class="hm-chip" aria-pressed={filter === 'resolved'} onclick={() => (filter = 'resolved')}>Resolved <span class="count">{counts.resolved}</span></button>
        <button type="button" class="hm-chip" aria-pressed={onlyMine} onclick={() => (onlyMine = !onlyMine)}>Listed by this wallet</button>
      </div>
    </section>

    <section class="panel">
      {#if shown.length === 0}
        <p class="hm-empty">{hm.decisions.length === 0 ? 'No decision yet. List one: a question whose answer will be beyond dispute once known.' : 'No decision matches.'}</p>
      {:else}
        <div class="hm-wrap">
          <table class="hm-table" aria-label="Decisions">
            <thead><tr><th>Question</th><th>Status</th><th>Answer</th><th class="end">Known in period</th><th class="end">Vote ends in period</th><th class="end">Markets</th></tr></thead>
            <tbody>
              {#each slice as d (d.id)}
                <tr class="click" onclick={() => (openId = openId === d.id ? null : d.id)} aria-expanded={openId === d.id}>
                  <td class="cut q"><SafeText text={d.text} />{#if mine.has(d.id)} <span class="badge accent">yours</span>{/if}</td>
                  <td><span class="hm-stage" class:live={d.status === 'open'} class:vote={d.status === 'voting'} class:done={d.status === 'resolved'}>{d.status === 'open' ? 'Open' : d.status === 'voting' ? 'Voting' : 'Resolved'}</span></td>
                  <td>{#if d.status === 'resolved'}<strong><SafeText text={String(d.answer ?? '')} /></strong>{:else}<span class="muted">{d.scaled ? `${d.min} to ${d.max}` : 'yes or no'}</span>{/if}</td>
                  <td class="end num">{d.eventperiod}</td>
                  <td class="end num">{d.closeperiod}</td>
                  <td class="end num">{marketsOn(d.id).length}</td>
                </tr>
                {#if openId === d.id}
                  {@const full = hm.details[d.id] ?? d}
                  <tr class="detail">
                    <td colspan="6">
                      <dl class="facts">
                        <dt>Id</dt><dd class="mono"><span translate="no">{d.id}</span> <CopyButton text={d.id} label="Copy the id of the decision" /></dd>
                        <dt>Author</dt><dd class="mono"><span translate="no">{d.author}</span></dd>
                        <dt>Listed in block</dt><dd class="num">{d.height}</dd>
                        <dt>Put off</dt><dd class="num">{d.rolls} time(s)</dd>
                        {#if full.votes}<dt>Votes</dt><dd class="num">{full.votes.length}, {full.votes.filter((v) => v.revealed).length} revealed</dd>{/if}
                      </dl>
                      <div class="row">
                        {#if d.status === 'open'}<button type="button" class="btn small primary" onclick={() => go('create', `market:${d.id}`)}><Icon name="plus" size={14} /> Open a market on it</button>{/if}
                        {#if d.status === 'voting'}<button type="button" class="btn small" onclick={() => go('voting')}>Vote on it</button>{/if}
                        {#each marketsOn(d.id) as m (m.id)}<button type="button" class="btn small ghost" onclick={() => go('markets', `m-${m.id}`)}><SafeText text={titleWithoutCategory(m.text)} /></button>{/each}
                      </div>
                    </td>
                  </tr>
                {/if}
              {/each}
            </tbody>
          </table>
        </div>
        <div class="pager">
          <span class="muted small">{pageNo * pageSize + 1} to {Math.min(shown.length, (pageNo + 1) * pageSize)} of {shown.length}</span>
          <div class="row">
            <button type="button" class="btn small" disabled={pageNo === 0} onclick={() => pageNo--}>Previous</button>
            <span class="small">Page {pageNo + 1} of {pages}</span>
            <button type="button" class="btn small" disabled={pageNo >= pages - 1} onclick={() => pageNo++}>Next</button>
            <select class="pill" bind:value={pageSize} aria-label="Per page">
              {#each [10, 25, 50, 100] as n (n)}<option value={n}>{n} per page</option>{/each}
            </select>
          </div>
        </div>
      {/if}
    </section>
  {:else if hm.info && clock}
    <section class="panel now">
      <div class="panel-body">
        <div class="period">
          <span class="caps">Now</span>
          <strong class="big">Period {hm.info.period}</strong>
          <span class="muted">{hm.info.periodblocks ? `${hm.info.periodblocks} blocks of this chain each (a test chain)` : 'counted in the time of the mainchain'}</span>
        </div>
        <div class="phases" aria-label="The phases of the period">
          <div class="phase-bar"><span class="seal"></span><span class="reveal"></span><span class="mark" use:barWidth={clock.length ? 1 - clock.left / clock.length : 0}></span></div>
          <div class="phase-words"><span>Votes are sealed</span><span>Revealed</span></div>
        </div>
        <p class="state">
          {#if hm.info.phase === 'reveal'}
            <span class="hm-stage vote">Reveal phase</span> The votes that end in this period are revealed now. The period ends in <Countdown info={hm.info} target={periodEnd(hm.info, hm.info.period)} urgent={hm.info.periodblocks ? 1 : 600} />.
          {:else}
            <span class="hm-stage live">Seal phase</span> Votes are sealed: nobody sees them. The reveal phase starts in <Countdown info={hm.info} target={revealStart(hm.info, hm.info.period)} />.
          {/if}
        </p>
        <p class="hm-help">A period is for trading until it ends. In the periods after, the decisions whose answer became known in it are voted on: votes are sealed, then revealed in the last third of the last period of the vote; the first block after it resolves them and pays the markets.</p>
      </div>
    </section>
    <section class="panel">
      <div class="panel-head"><h2>Periods ahead</h2></div>
      <div class="hm-wrap">
        <table class="hm-table" aria-label="Periods ahead">
          <thead><tr><th>Period</th><th>Ends</th><th>Answers known</th><th>Votes end</th></tr></thead>
          <tbody>
            {#each calendar as c (c.period)}
              <tr class:current={c.period === hm.info.period}>
                <td class="num">{c.period}{#if c.period === hm.info.period} <span class="badge accent">now</span>{/if}</td>
                <td><Countdown info={hm.info} target={periodEnd(hm.info, c.period)} /></td>
                <td>{#each c.known as d (d.id)}<div class="cut"><SafeText text={d.text} /></div>{:else}<span class="faint">–</span>{/each}</td>
                <td>{#each c.closing as d (d.id)}<div class="cut"><SafeText text={d.text} /></div>{:else}<span class="faint">–</span>{/each}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    </section>
  {:else}
    <p class="hm-empty">Asking the node…</p>
  {/if}
</div>

<style>
  .decisions {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .head {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 0.8rem;
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
  .q {
    max-width: 30rem;
  }
  .detail td {
    background: var(--surface-2);
  }
  .detail .row {
    flex-wrap: wrap;
    margin-top: 0.6rem;
  }
  .pager {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 0.6rem;
    padding: 0.7rem 1rem;
    border-top: 1px solid var(--line);
  }
  .pager .row {
    align-items: center;
  }
  .now .panel-body {
    display: flex;
    flex-direction: column;
    gap: 0.9rem;
  }
  .period {
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: 0.4rem 0.8rem;
  }
  .big {
    font-size: 1.6rem;
    font-weight: 700;
  }
  .phase-bar {
    position: relative;
    display: grid;
    grid-template-columns: 2fr 1fr;
    gap: 3px;
    height: 0.7rem;
  }
  .phase-bar .seal {
    border-radius: 999px 0 0 999px;
    background: var(--accent-soft);
  }
  .phase-bar .reveal {
    border-radius: 0 999px 999px 0;
    background: var(--warning-soft);
  }
  .phase-bar .mark {
    position: absolute;
    inset: 0 auto 0 0;
    border-right: 3px solid var(--text);
    border-radius: 999px 0 0 999px;
    background: transparent;
  }
  .phase-words {
    display: grid;
    grid-template-columns: 2fr 1fr;
    margin-top: 0.3rem;
    font-size: 0.8rem;
    color: var(--text-muted);
  }
  .state {
    margin: 0;
    line-height: 1.8;
  }
  .state .hm-stage {
    margin-right: 0.35rem;
  }
  tr.current td {
    background: var(--accent-soft);
  }
  .cut {
    max-width: 24rem;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .small {
    font-size: 0.85rem;
  }
</style>
