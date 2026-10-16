<script lang="ts">
  // One question, in Simple mode: the question in words, the chance of each answer as a bar, then big
  // buttons for the answers (Yes and No; Higher and Lower on a scale; the options of a question with
  // several), an amount, and what comes of the bet in plain words; the bets of the wallet on it with
  // "Cash out now". Trades go through hm/simpletrade.ts, as the trade panel of Advanced places them.
  import { t } from '../../lib/i18n.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import Term from '../../components/Term.svelte';
  import { rpc } from '../../lib/rpc';
  import { CHAIN } from '../config';
  import { wallets } from '../../lib/state.svelte';
  import { setUiMode } from '../../lib/uimode.svelte';
  import Countdown from './Countdown.svelte';
  import { barWidth } from './actions';
  import { commonQuestion, describeOutcome, heldByOutcome, marketKind, optionTexts, outcomesOf, periodEnd, titleWithoutCategory, type Market, type Outcome } from './model';
  import { decisionMap, hm } from './store.svelte';
  import { betPreview, betProblem, cashOut, cashOutPreview, coins, placeBet } from './simpletrade';

  let { id, back }: { id: string; back: () => void } = $props();

  let full = $state<Market | null>(null);
  $effect(() => {
    void hm.version;
    const want = id;
    rpc
      .raw<Market>('getmarket', [want])
      .then((m) => {
        if (want === id) full = m;
      })
      .catch(() => undefined);
  });
  const market = $derived(full ?? hm.markets.find((m) => m.id === id) ?? null);
  const decisions = $derived(decisionMap());
  const kind = $derived(market ? marketKind(market, decisions) : 'binary');
  const about = $derived(market ? market.decisions.map((d) => decisions.get(d)).filter((d) => d !== undefined) : []);
  /** The answers to pick from: Yes then No, Higher then Lower, or the options of a question with several. */
  const answers = $derived.by((): Outcome[] => {
    if (!market) return [];
    const outs = outcomesOf(market, decisions);
    if (kind === 'categorical') return outs.filter((o) => o.exclusive).sort((a, b) => (market.states[b.state]?.price ?? 0) - (market.states[a.state]?.price ?? 0));
    return [...outs].sort((a, b) => b.state - a.state);
  });
  const question = $derived.by(() => {
    if (!market) return '';
    if (kind === 'categorical') return commonQuestion(about.map((d) => d.text)) || titleWithoutCategory(market.text);
    return about[0]?.text ?? titleWithoutCategory(market.text);
  });
  /** A question with several answers: the chance of the other combinations (none of them, or more than one). */
  const other = $derived(market && kind === 'categorical' ? outcomesOf(market, decisions).filter((o) => !o.exclusive).reduce((n, o) => n + (market.states[o.state]?.price ?? 0), 0) : 0);
  const options = $derived(kind === 'categorical' ? optionTexts(about.map((d) => d.text)) : []);
  const scale = $derived(kind === 'scalar' && about[0] ? { min: about[0].min ?? 0, max: about[0].max ?? 1 } : null);

  function name(o: Outcome): string {
    if (o.kind === 'high') return t('Higher');
    if (o.kind === 'low') return t('Lower');
    return describeOutcome(o);
  }

  let chosen = $state<number | null>(null);
  let amountText = $state('');
  let busy = $state(false);
  const amount = $derived(Number(amountText.replace(',', '.')));
  const pick = $derived(answers.find((o) => o.state === chosen) ?? null);
  const held = $derived(heldByOutcome(hm.positions.filter((p) => p.market === id)));
  const holding = $derived(held.find((h) => h.state === chosen)?.shares ?? 0);
  const preview = $derived(market && pick && amount > 0 ? betPreview(market, pick.state, amount) : null);
  const problem = $derived(market ? betProblem(market, amount, holding, preview) : '');

  async function bet() {
    const wallet = wallets.current;
    if (!market || !pick || wallet === null || busy) return;
    busy = true;
    try {
      if (await placeBet(wallet, market, pick.state, name(pick), amount)) amountText = '';
    } finally {
      busy = false;
    }
  }
  async function sell(state: number) {
    const wallet = wallets.current;
    const h = held.find((x) => x.state === state);
    const o = answers.find((x) => x.state === state) ?? (market ? outcomesOf(market, decisions).find((x) => x.state === state) : undefined);
    if (!market || !h || !o || wallet === null || busy) return;
    busy = true;
    try {
      await cashOut(wallet, market, state, name(o), h.accounts);
    } finally {
      busy = false;
    }
  }
</script>

{#if !market}
  <div class="page">
    <button type="button" class="btn ghost" onclick={back}><Icon name="receive" size={15} /> All questions</button>
    <p class="hm-empty">Asking the node…</p>
  </div>
{:else}
  <div class="page simple-market">
    <button type="button" class="btn ghost small backlink" onclick={back}><Icon name="receive" size={15} /> All questions</button>
    <header>
      <h1><SafeText text={question} /></h1>
      <p class="muted">
        {#if market.status === 'trading'}
          Open for bets{#if market.tradinguntil !== undefined && hm.info}: it <Countdown info={hm.info} target={periodEnd(hm.info, market.tradinguntil)} phrase="closes" />{/if}.
        {:else if market.status === 'closed'}
          Bets are closed: the holders of <Term id="votecoin">Votecoin</Term> are deciding the answer. Bets pay out once they have.
        {:else}
          Answered: the bets were paid out.
        {/if}
      </p>
    </header>

    <section class="panel chances" aria-label="Chances">
      {#each answers as o (o.state)}
        {@const p = market.states[o.state]?.price ?? 0}
        <div class="chance hm-c{o.state % 8}">
          <span class="name">{name(o)}</span>
          <span class="hm-bar"><span use:barWidth={p}></span></span>
          <strong class="num">{Math.round(p * 100)}%</strong>
        </div>
      {/each}
      {#if other >= 0.005}
        <div class="chance hm-c4">
          <span class="name muted">None of them, or several</span>
          <span class="hm-bar"><span use:barWidth={other}></span></span>
          <strong class="num muted">{Math.round(other * 100)}%</strong>
        </div>
      {/if}
      <p class="faint small">The chance is what the bets placed so far say. {#if scale}On a scale from {scale.min} to {scale.max}: Higher wins more the higher the answer.{/if}</p>
    </section>

    {#if market.status === 'trading'}
      <section class="panel bet" aria-label="Place a bet">
        <h2>Your bet</h2>
        <div class="answers" class:many={answers.length > 2} role="radiogroup" aria-label="Answer">
          {#each answers as o (o.state)}
            <button type="button" role="radio" aria-checked={chosen === o.state} class="answer hm-c{o.state % 8}" class:yes={o.kind === 'yes' || o.kind === 'high'} class:no={o.kind === 'no' || o.kind === 'low'} onclick={() => (chosen = o.state)}>{name(o)}</button>
          {/each}
        </div>
        <label class="field">
          <span>Amount to bet ({CHAIN.coin.ticker})</span>
          <input id="bet-amount" type="text" inputmode="decimal" autocomplete="off" placeholder="0.00" bind:value={amountText} />
        </label>
        <div class="hm-chips">
          {#each ['0.1', '1', '5', '10'] as v (v)}<button type="button" class="hm-chip" onclick={() => (amountText = v)}>{v}</button>{/each}
        </div>
        {#if pick && preview}
          <p class="outcome" data-testid="bet-outcome">
            {#if scale}
              {t('If the answer is %1 or more you get ≈ %2; if it is %3 or less, you lose %4. In between, part of it.', String(scale.max), coins(preview.pays), String(scale.min), coins(preview.cost))}
            {:else}
              {t('If “%1” happens you get ≈ %2; if not, you lose %3.', name(pick), coins(preview.pays), coins(preview.cost))}
            {/if}
          </p>
        {:else if !pick}
          <p class="faint small">Choose an answer, then an amount.</p>
        {/if}
        {#if problem}<p class="hm-problems" role="alert">{problem}</p>{/if}
        <button type="button" id="bet-place" class="btn primary place" disabled={busy || !pick || !preview || !!problem || wallets.current === null} onclick={bet}>
          {pick && preview ? t('Bet %1 on %2', coins(preview.cost), name(pick)) : t('Bet')}
        </button>
        {#if wallets.current === null}<p class="faint small">Open a wallet to bet.</p>{/if}
      </section>
    {/if}

    {#if held.length}
      <section class="panel mine" aria-label="Your bets on this question">
        <h2>Your bets on this question</h2>
        <ul>
          {#each held as h (h.state)}
            {@const o = outcomesOf(market, decisions).find((x) => x.state === h.state)}
            <li>
              <span>On <strong>{o ? name(o) : ''}</strong>: pays {coins(h.shares)} if right.</span>
              {#if market.status === 'trading'}
                <button type="button" class="btn small" disabled={busy || wallets.current === null} onclick={() => sell(h.state)}>{t('Cash out now for ≈ %1', coins(cashOutPreview(market, h.state, h.accounts)))}</button>
              {/if}
            </li>
          {/each}
        </ul>
      </section>
    {/if}

    {#if kind === 'categorical' && options.length > 1}
      <p class="faint small">Bets on several answers at once, the price chart and the holders are in <button type="button" class="link" onclick={() => setUiMode('advanced')}>Advanced mode</button>.</p>
    {:else}
      <p class="faint small">The price chart, the fees and the <Term id="slippage">slippage</Term> are in <button type="button" class="link" onclick={() => setUiMode('advanced')}>Advanced mode</button>.</p>
    {/if}
  </div>
{/if}

<style>
  .simple-market {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    max-width: 52rem;
  }
  .backlink {
    align-self: flex-start;
  }
  h1 {
    margin: 0;
    font-size: 1.6rem;
    line-height: 1.25;
  }
  header p {
    margin: 0.4rem 0 0;
  }
  h2 {
    margin: 0 0 0.6rem;
    font-size: 1.05rem;
  }
  .chances {
    display: flex;
    flex-direction: column;
    gap: 0.55rem;
    padding: 1rem 1.1rem;
  }
  .chance {
    display: grid;
    grid-template-columns: minmax(5rem, 12rem) 1fr 3.5rem;
    align-items: center;
    gap: 0.8rem;
  }
  .chance .name {
    font-weight: 600;
    overflow-wrap: anywhere;
  }
  .chance .hm-bar {
    height: 0.7rem;
  }
  .chance strong {
    text-align: right;
    font-size: 1.1rem;
  }
  .chances p {
    margin: 0.2rem 0 0;
  }
  .bet {
    display: flex;
    flex-direction: column;
    gap: 0.7rem;
    padding: 1rem 1.1rem;
  }
  .bet h2 {
    margin: 0;
  }
  .answers {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 0.6rem;
  }
  .answers.many {
    grid-template-columns: repeat(auto-fill, minmax(10rem, 1fr));
  }
  .answer {
    min-height: 3.6rem;
    padding: 0.6rem 0.8rem;
    border: 2px solid var(--line);
    border-radius: var(--radius-l);
    background: var(--surface-2);
    color: var(--text);
    font: inherit;
    font-size: 1.25rem;
    font-weight: 700;
    cursor: pointer;
    overflow-wrap: anywhere;
  }
  .answers.many .answer {
    font-size: 1rem;
  }
  .answer:hover {
    border-color: var(--hm-c);
  }
  .answer[aria-checked='true'] {
    border-color: var(--hm-c);
    background: color-mix(in srgb, var(--hm-c) 18%, var(--surface-2));
  }
  .field {
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
    max-width: 16rem;
  }
  .field > span {
    color: var(--text-muted);
    font-size: 0.88rem;
  }
  .outcome {
    margin: 0;
    padding: 0.7rem 0.85rem;
    border-radius: var(--radius-m);
    background: var(--surface-2);
    font-size: 1.02rem;
    font-weight: 550;
  }
  .place {
    align-self: flex-start;
    min-height: 2.7rem;
    padding: 0 1.4rem;
    font-size: 1.05rem;
  }
  .mine {
    padding: 1rem 1.1rem;
  }
  .mine ul {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
    margin: 0;
    padding: 0;
    list-style: none;
  }
  .mine li {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
  }
</style>
