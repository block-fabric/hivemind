<script lang="ts">
  // The Markets page in Simple mode: "Questions". Every question open for bets as a card (the question in
  // words, the chance of the leading answers as bars, when betting closes), a search, and those being
  // decided or answered on demand; a card opens the question (hm/SimpleMarket.svelte).
  import { t } from '../../lib/i18n.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import Term from '../../components/Term.svelte';
  import { go } from '../../lib/nav.svelte';
  import { barWidth } from '../hm/actions';
  import Countdown from '../hm/Countdown.svelte';
  import { commonQuestion, describeOutcome, marketKind, outcomesOf, periodEnd, scalarEstimate, titleWithoutCategory, type Market, type Outcome } from '../hm/model';
  import { decisionMap, hm } from '../hm/store.svelte';

  let { open }: { open: (id: string) => void } = $props();

  let show = $state<'open' | 'other'>('open');
  let search = $state('');
  const decisions = $derived(decisionMap());

  function question(m: Market): string {
    const about = m.decisions.map((d) => decisions.get(d)).filter((d) => d !== undefined);
    if (m.decisions.length > 1) return commonQuestion(about.map((d) => d.text)) || titleWithoutCategory(m.text);
    return about[0]?.text ?? titleWithoutCategory(m.text);
  }
  function name(o: Outcome): string {
    if (o.kind === 'high') return t('Higher');
    if (o.kind === 'low') return t('Lower');
    return describeOutcome(o);
  }
  /** The answers a card shows: Yes for a yes-or-no question, Higher on a scale, the two leading options of several. */
  function shownAnswers(m: Market): Outcome[] {
    const outs = outcomesOf(m, decisions);
    const k = marketKind(m, decisions);
    if (k === 'categorical') return outs.filter((o) => o.exclusive).sort((a, b) => (m.states[b.state]?.price ?? 0) - (m.states[a.state]?.price ?? 0)).slice(0, 3);
    return outs.filter((o) => o.state === 1);
  }

  const shown = $derived.by(() => {
    const q = search.trim().toLowerCase();
    return hm.markets
      .filter((m) => (show === 'open' ? m.status === 'trading' : m.status !== 'trading'))
      .filter((m) => !q || m.text.toLowerCase().includes(q) || question(m).toLowerCase().includes(q))
      .sort((a, b) => (show === 'open' ? (a.tradinguntil ?? Infinity) - (b.tradinguntil ?? Infinity) : b.height - a.height));
  });
  const openCount = $derived(hm.markets.filter((m) => m.status === 'trading').length);
</script>

<div class="page simple-markets">
  <header class="page-head head">
    <div>
      <h1>Questions</h1>
      <p class="muted sub">Bet on what will happen. Each <Term id="market">question</Term> shows the chance the bets give each answer; a right bet pays 1 coin a share.</p>
    </div>
    <button type="button" class="btn" onclick={() => go('portfolio')}><Icon name="portfolio" size={15} /> My bets</button>
  </header>

  <div class="tools">
    <div class="hm-chips" role="group" aria-label="Which questions">
      <button type="button" class="hm-chip" aria-pressed={show === 'open'} onclick={() => (show = 'open')}>Open for bets <span class="count">{openCount}</span></button>
      <button type="button" class="hm-chip" aria-pressed={show === 'other'} onclick={() => (show = 'other')}>Being decided or answered <span class="count">{hm.markets.length - openCount}</span></button>
    </div>
    <div class="searchbox">
      <Icon name="search" size={16} />
      <input type="search" placeholder="Search the questions" bind:value={search} aria-label="Search the questions" />
    </div>
  </div>

  {#if hm.error}<p class="side-note side-bad" role="alert"><SafeText text={hm.error} /></p>{/if}

  {#if !hm.loaded}
    <p class="hm-empty">Asking the node…</p>
  {:else if shown.length === 0}
    <p class="hm-empty panel">{hm.markets.length === 0 ? 'No question yet. Questions are opened in Advanced mode (Create).' : show === 'open' ? 'No question is open for bets right now.' : 'No question matches.'}</p>
  {:else}
    <ul class="cards" aria-label="Questions">
      {#each shown as m (m.id)}
        {@const k = marketKind(m, decisions)}
        {@const about = decisions.get(m.decisions[0])}
        <li>
          <button type="button" class="panel card" onclick={() => open(m.id)}>
            <strong class="q"><SafeText text={question(m)} /></strong>
            {#if k === 'scalar' && about}
              {@const est = scalarEstimate(m, about)}
              <span class="estimate">Expected: <strong class="num">{est === null ? '–' : est.toFixed(2)}</strong> <span class="faint">(from {about.min} to {about.max})</span></span>
            {:else}
              {#each shownAnswers(m) as o (o.state)}
                {@const p = m.states[o.state]?.price ?? 0}
                <span class="line hm-c{o.state % 8}">
                  <span class="name">{name(o)}</span>
                  <span class="hm-bar"><span use:barWidth={p}></span></span>
                  <span class="num pct">{Math.round(p * 100)}%</span>
                </span>
              {/each}
              {#if k === 'categorical'}
                {@const other = outcomesOf(m, decisions).filter((o) => !o.exclusive).reduce((n, o) => n + (m.states[o.state]?.price ?? 0), 0)}
                {#if other >= 0.005}<span class="faint small">{t('None of them, or several: %1%', String(Math.round(other * 100)))}</span>{/if}
              {/if}
            {/if}
            <span class="foot faint small">
              {#if m.status === 'trading'}
                {#if m.tradinguntil !== undefined && hm.info}Betting <Countdown info={hm.info} target={periodEnd(hm.info, m.tradinguntil)} phrase="closes" />{:else}Open for bets{/if}
              {:else if m.status === 'closed'}Being decided{:else}Answered{/if}
            </span>
          </button>
        </li>
      {/each}
    </ul>
  {/if}
</div>

<style>
  .simple-markets {
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
    max-width: 44rem;
  }
  .tools {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 0.6rem;
  }
  .searchbox {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    flex: 0 1 22rem;
    padding: 0 0.7rem;
    border: 1px solid var(--line);
    border-radius: 999px;
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
  .cards {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(min(19rem, 100%), 1fr));
    gap: 0.9rem;
    margin: 0;
    padding: 0;
    list-style: none;
  }
  .card {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    width: 100%;
    height: 100%;
    padding: 1rem 1.1rem;
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .card:hover {
    border-color: var(--accent);
  }
  .q {
    font-size: 1.08rem;
    line-height: 1.3;
  }
  .line {
    display: grid;
    grid-template-columns: minmax(3rem, 7rem) 1fr 3rem;
    align-items: center;
    gap: 0.6rem;
  }
  .line .name {
    font-weight: 600;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .line .hm-bar {
    height: 0.6rem;
  }
  .pct {
    text-align: right;
    font-weight: 700;
  }
  .foot {
    margin-top: auto;
  }
</style>
