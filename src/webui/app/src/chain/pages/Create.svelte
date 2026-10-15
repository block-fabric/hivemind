<script lang="ts">
  // Create: a wizard that lists the decisions a market is about and opens the market on them, or lists
  // a decision alone. Its kind (yes or no, a number on a scale, one of several options), the question,
  // when its answer is known and how long it is voted on, the market (title, category, liquidity, fee),
  // then a preview of the market as it will show, with what it costs, checked before anything is sent.
  // A market waits for its new decisions to be in a block: the wizard opens it then. The Create tab of
  // the Qt window of Hivemind.
  import { t } from '../../lib/i18n.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import NoWallet from '../../components/NoWallet.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { formatTime, shortHash } from '../../lib/format';
  import { go, nav, PAGES } from '../../lib/nav.svelte';
  import { app, node, toast, wallets, walletsEnabled } from '../../lib/state.svelte';
  import '../hm/hm.css';
  import Countdown from '../hm/Countdown.svelte';
  import { decisionProblems, DEFAULT_FEE_BPS, formatChance, formatUnits, LIMITS, lmsrFunding, marketProblems, periodAt, periodEnd, revealStart, type DecisionDraft, type MarketKind } from '../hm/model';
  import { creation, startCreation } from '../hm/creation.svelte';
  import { hm, startHivemind } from '../hm/store.svelte';

  $effect(() => startHivemind());

  const noWallet = $derived(!walletsEnabled() || wallets.current === null);
  const STEPS = ['Kind', 'Question', 'Timing', 'Market', 'Review'] as const;
  let step = $state(0);

  let kind = $state<MarketKind>('binary');
  let reuse = $state(false);
  let picked = $state<string[]>([]);
  let question = $state('');
  let options = $state<string[]>(['', '']);
  let min = $state(0);
  let max = $state(100);
  let eventIn = $state(1);
  let votePeriods = $state(1);
  /**
   * When the answer is known: a number of periods from now, an amount of time from now, or a date, as chains-qt
   * took them (audit port-1 #8). The time and the date give the period they fall in.
   */
  let when = $state<'periods' | 'time' | 'date'>('periods');
  let timeAmount = $state(7);
  let timeUnit = $state<'hours' | 'days' | 'weeks'>('days');
  let dateText = $state('');
  const UNIT_SECONDS = { hours: 3600, days: 86400, weeks: 604800 } as const;
  /** The moment the answer is known, in seconds, by the time or the date given; null when none is given. */
  const whenSeconds = $derived.by(() => {
    const now = Math.floor(Date.now() / 1000);
    if (when === 'time') return Number.isFinite(timeAmount) && timeAmount > 0 ? now + Math.round(timeAmount * UNIT_SECONDS[timeUnit]) : null;
    if (when === 'date') {
      const ms = dateText ? new Date(`${dateText}T23:59:59`).getTime() : NaN;
      return Number.isFinite(ms) ? Math.floor(ms / 1000) : null;
    }
    return null;
  });
  $effect(() => {
    const at = whenSeconds;
    if (at === null || !hm.info) return;
    const now = Math.floor(Date.now() / 1000);
    eventIn = periodAt(hm.info, at, now, node.tip?.height ?? 0, app.info?.blockSpacing ?? 60) - hm.info.period;
  });
  let withMarket = $state(true);
  let title = $state('');
  let titleTouched = $state(false);
  let category = $state('');
  let liquidity = $state(1);
  let feePct = $state(DEFAULT_FEE_BPS / 100);
  let search = $state('');

  // Opened for a decision (Decisions → "Open a market on it"), or to list a decision alone.
  $effect(() => {
    const tab = nav.tab;
    if (tab.startsWith('market:')) {
      const id = tab.slice(7);
      const d = hm.decisions.find((x) => x.id === id);
      reuse = true;
      picked = [id];
      kind = d?.scaled ? 'scalar' : 'binary';
      withMarket = true;
      step = 3;
    } else if (tab === 'decision') {
      withMarket = false;
      reuse = false;
    } else if (tab === 'market') {
      withMarket = true;
    }
  });

  const info = $derived(hm.info);
  const period = $derived(info?.period ?? 0);
  const eventPeriod = $derived(period + Math.max(0, Math.floor(eventIn)));
  const closePeriod = $derived(eventPeriod + Math.max(1, Math.floor(votePeriods)));
  const openDecisions = $derived(hm.decisions.filter((d) => d.status === 'open' && (kind === 'scalar' ? d.scaled : !d.scaled)));
  const candidates = $derived(openDecisions.filter((d) => !search.trim() || d.text.toLowerCase().includes(search.trim().toLowerCase()) || d.id.startsWith(search.trim())));
  const decisions = $derived(new Map(hm.decisions.map((d) => [d.id, d])));
  const CATEGORIES = ['Crypto', 'Economics', 'Politics', 'Science', 'Sports', 'Technology', 'Weather', 'World'];

  /** The decisions to list: one, or one per option of a categorical market. */
  const drafts = $derived.by((): DecisionDraft[] => {
    if (reuse) return [];
    const q = question.trim();
    if (kind === 'categorical') return options.map((o) => ({ text: `${q} — ${o.trim()}`, scaled: false, min: 0, max: 0, eventperiod: eventPeriod, voteperiods: Math.floor(votePeriods) }));
    return [{ text: q, scaled: kind === 'scalar', min: Math.floor(min), max: Math.floor(max), eventperiod: eventPeriod, voteperiods: Math.floor(votePeriods) }];
  });
  const states = $derived(2 ** (reuse ? picked.length : drafts.length));
  const defaultTitle = $derived(reuse ? (decisions.get(picked[0])?.text ?? '') : question.trim());
  $effect(() => {
    if (!titleTouched) title = defaultTitle;
  });
  const fullTitle = $derived(category ? `[${category}] ${title.trim()}` : title.trim());
  const feeBps = $derived(Math.round(feePct * 100));
  const funding = $derived(lmsrFunding(liquidity, states));
  const listing = $derived((info?.listingfee ?? 0) * drafts.length);

  /** What is wrong with the question (step 2), the timing (step 3), the market (step 4). */
  const questionProblems = $derived.by(() => {
    if (!info) return [t('Asking the node…')];
    const out: string[] = [];
    if (reuse) {
      if (picked.length === 0) out.push(t('Choose the decisions the market is about.'));
      if (kind !== 'categorical' && picked.length > 1) out.push(t('A market of this kind is about one decision.'));
      if (kind === 'categorical' && picked.length < 2) out.push(t('Choose 2 to %1 decisions.', String(LIMITS.maxMarketDecisions)));
      return out;
    }
    if (kind === 'categorical') {
      if (options.length < 2 || options.length > LIMITS.maxMarketDecisions) out.push(t('Give 2 to %1 options.', String(LIMITS.maxMarketDecisions)));
      if (options.some((o) => !o.trim())) out.push(t('Give every option a name.'));
      if (new Set(options.map((o) => o.trim().toLowerCase())).size !== options.length) out.push(t('Two options have the same name.'));
    }
    // The timing is the next step's: checked here with a timing that is right.
    for (const d of drafts) out.push(...decisionProblems({ ...d, eventperiod: info.period, voteperiods: 1 }, info));
    return [...new Set(out)];
  });
  const timingProblems = $derived(!info || reuse ? [] : [...new Set(decisionProblems({ text: '?', scaled: false, min: 0, max: 0, eventperiod: eventPeriod, voteperiods: Math.floor(votePeriods) }, info))]);
  const marketProblemList = $derived(withMarket ? marketProblems({ text: fullTitle, decisions: reuse ? picked : drafts.map((_, i) => `new${i}`), liquidity, feebps: feeBps }, decisions).filter((p) => !reuse || !questionProblems.includes(p)) : []);
  const problems = $derived([...new Set([...questionProblems, ...timingProblems, ...marketProblemList])]);
  /** The problems of the step shown, so that a step is not held up by those of the next. */
  const stepProblems = $derived(step === 1 ? questionProblems : step === 2 ? timingProblems : step === 3 ? marketProblemList : step === 4 ? problems : []);

  function setKind(k: MarketKind) {
    kind = k;
    picked = [];
  }
  function togglePick(id: string) {
    if (picked.includes(id)) picked = picked.filter((x) => x !== id);
    else if (kind === 'categorical') picked = [...picked, id].slice(0, LIMITS.maxMarketDecisions);
    else picked = [id];
  }

  async function submit() {
    const wallet = wallets.current;
    if (wallet === null || problems.length || !info) return;
    const costs = [
      drafts.length ? t('Listing %1 decision(s): at least %2, which goes to the voters.', String(drafts.length), formatUnits(listing)) : '',
      withMarket ? t('Funding the market: %1, the most it can lose; what it has left comes back when it pays out. And an opening fee, for the voters.', formatUnits(funding, 8)) : '',
    ].filter(Boolean);
    const r = await ask({
      title: withMarket ? t('Open the market') : t('List the decision'),
      message: withMarket ? t('Open “%1”?', fullTitle) : t('List “%1”?', drafts[0]?.text ?? ''),
      informative: costs.join('\n'),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: withMarket ? 'Open' : 'List', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    try {
      await startCreation({
        wallet,
        drafts,
        existing: reuse ? picked : [],
        market: withMarket ? { text: fullTitle, liquidity, feebps: feeBps } : null,
      });
      toast('success', withMarket ? t('Creating the market') : t('Decision listed'), withMarket ? t('The market opens once its decisions are in a block.') : t('It shows with the next block.'));
    } catch (e) {
      await inform(withMarket ? t('Open the market') : t('List the decision'), errorText(e), 'error');
    }
  }

  function reset() {
    step = 0;
    question = '';
    options = ['', ''];
    picked = [];
    titleTouched = false;
    category = '';
  }
</script>

<div class="page create">
  <header class="page-head head">
    <h1>Create</h1>
    <div class="hm-chips" role="group" aria-label="What to create">
      <button type="button" class="hm-chip" aria-pressed={withMarket} onclick={() => (withMarket = true)}>A market</button>
      <button type="button" class="hm-chip" aria-pressed={!withMarket} onclick={() => { withMarket = false; reuse = false; }}>A decision alone</button>
    </div>
  </header>

  {#if noWallet}
    <NoWallet page={PAGES.create} />
  {:else}
    {#if creation.jobs.length}
      <section class="panel jobs" aria-label="Under way">
        <div class="panel-head"><h2>Under way</h2></div>
        <ul>
          {#each creation.jobs as j (j.id)}
            <li>
              <span class="hm-stage" class:live={j.state === 'done'} class:vote={j.state === 'waiting' || j.state === 'sending'} class:bad={j.state === 'failed'}>{j.state === 'done' ? 'Done' : j.state === 'failed' ? 'Failed' : j.state === 'waiting' ? 'Waiting for a block' : 'Sending'}</span>
              <span class="what"><SafeText text={j.title} /></span>
              {#if j.error}<span class="hm-down small"><SafeText text={j.error} /></span>{/if}
              {#if j.marketId}<button type="button" class="btn small" onclick={() => go('markets', `m-${j.marketId}`)}>Open the market</button>{/if}
              {#if j.txids.length}<span class="faint small mono" translate="no">{j.txids.map((x) => shortHash(x, 6)).join(' ')}</span>{/if}
            </li>
          {/each}
        </ul>
      </section>
    {/if}

    <ol class="steps" aria-label="Steps">
      {#each STEPS as s, i (s)}
        <li class:on={i === step} class:done={i < step}><button type="button" onclick={() => (step = i)} disabled={i === 3 && !withMarket}><span class="n">{i + 1}</span> {t(s)}</button></li>
      {/each}
    </ol>

    <section class="panel wizard">
      <div class="panel-body">
        {#if step === 0}
          <h2>What kind of {withMarket ? 'market' : 'decision'}?</h2>
          <div class="kinds" role="radiogroup" aria-label="Kind">
            <button type="button" role="radio" aria-checked={kind === 'binary'} class="kind" onclick={() => setKind('binary')}>
              <strong>Yes or no</strong><span class="muted">Will it happen? Two outcomes; a share of the right one pays 1.</span>
            </button>
            <button type="button" role="radio" aria-checked={kind === 'scalar'} class="kind" onclick={() => setKind('scalar')}>
              <strong>A number</strong><span class="muted">How much, how many? An answer on a scale; shares of Low and High pay by where it falls.</span>
            </button>
            {#if withMarket}
              <button type="button" role="radio" aria-checked={kind === 'categorical'} class="kind" onclick={() => setKind('categorical')}>
                <strong>One of several</strong><span class="muted">Who wins? 2 to {LIMITS.maxMarketDecisions} options, a yes or no decision each, in one market.</span>
              </button>
            {/if}
          </div>
          {#if withMarket}
            <label class="check"><input type="checkbox" bind:checked={reuse} /> <span>On decisions already listed</span></label>
          {/if}
        {:else if step === 1}
          {#if reuse}
            <h2>The decisions</h2>
            <div class="searchbox"><Icon name="search" size={16} /><input type="search" placeholder="Find a decision" bind:value={search} aria-label="Find a decision" /></div>
            {#if candidates.length === 0}
              <p class="hm-empty">No open decision of this kind. List one: untick “On decisions already listed”.</p>
            {:else}
              <ul class="picks">
                {#each candidates.slice(0, 60) as d (d.id)}
                  <li>
                    <label class="check">
                      <input type="checkbox" checked={picked.includes(d.id)} onchange={() => togglePick(d.id)} />
                      <span><SafeText text={d.text} /> <span class="faint small">· known in period {d.eventperiod}{d.scaled ? ` · ${d.min} to ${d.max}` : ''}</span></span>
                    </label>
                  </li>
                {/each}
              </ul>
            {/if}
          {:else}
            <h2>The question</h2>
            <label class="field">
              <span>Question</span>
              <input id="create-question" type="text" maxlength="400" placeholder={kind === 'categorical' ? 'Who will win the 2027 cup?' : kind === 'scalar' ? 'How many millimetres of rain fall in Paris in May 2027?' : 'Will it rain in Paris on 1 May 2027?'} bind:value={question} />
            </label>
            <p class="hm-help">Word it so that its answer is beyond dispute once known: the holders of reputation vote on it then, and lose reputation if they vote against the others.</p>
            {#if kind === 'scalar'}
              <div class="pair">
                <label class="field"><span>From</span><input id="create-min" type="number" step="1" bind:value={min} /></label>
                <label class="field"><span>To</span><input id="create-max" type="number" step="1" bind:value={max} /></label>
              </div>
              <p class="hm-help">A whole number between the two is voted for; a share of High pays where the answer falls on the scale, Low the rest.</p>
            {:else if kind === 'categorical'}
              <div class="options">
                {#each options as _, i (i)}
                  <div class="opt">
                    <span class="hm-dot hm-c{i % 8}"></span>
                    <input type="text" placeholder="Option {i + 1}" bind:value={options[i]} aria-label="Option {i + 1}" />
                    <button type="button" class="btn ghost small icon" aria-label="Remove the option" disabled={options.length <= 2} onclick={() => (options = options.filter((__, k) => k !== i))}><Icon name="x" size={14} /></button>
                  </div>
                {/each}
                <button type="button" class="btn small" disabled={options.length >= LIMITS.maxMarketDecisions} onclick={() => (options = [...options, ''])}><Icon name="plus" size={14} /> Option</button>
              </div>
              <p class="hm-help">Each option is listed as a yes or no decision; the market has a state for each combination of answers, those with one yes first.</p>
            {/if}
          {/if}
        {:else if step === 2}
          <h2>Timing</h2>
          {#if reuse}
            <p class="hm-help">The decisions chosen have their periods already: the market trades until the first of them is up for the vote.</p>
          {:else if info}
            <div class="pair">
              <div class="field">
                <span id="create-when-label">The answer is known</span>
                <div class="hm-chips" role="radiogroup" aria-labelledby="create-when-label">
                  <button type="button" class="hm-chip" role="radio" aria-checked={when === 'periods'} onclick={() => (when = 'periods')}>In a number of periods</button>
                  <button type="button" class="hm-chip" role="radio" aria-checked={when === 'time'} onclick={() => (when = 'time')}>After some time</button>
                  <button type="button" class="hm-chip" role="radio" aria-checked={when === 'date'} onclick={() => (when = 'date')}>On a date</button>
                </div>
                {#if when === 'periods'}
                  <span class="inline"><input id="create-event" type="number" min="0" max={info.maxperiodsahead} step="1" aria-label="Periods from now" bind:value={eventIn} /> <span class="muted">periods: period {eventPeriod}</span></span>
                {:else if when === 'time'}
                  <span class="inline">
                    <input id="create-event-time" type="number" min="1" step="1" aria-label="Time from now" bind:value={timeAmount} />
                    <select class="pill" aria-label="Unit of time" bind:value={timeUnit}><option value="hours">hours</option><option value="days">days</option><option value="weeks">weeks</option></select>
                    <span class="muted">from now: period {eventPeriod}</span>
                  </span>
                {:else}
                  <span class="inline"><input id="create-event-date" type="date" aria-label="Date" bind:value={dateText} /> <span class="muted">{dateText ? `period ${eventPeriod}` : 'choose a day'}</span></span>
                {/if}
              </div>
              <label class="field">
                <span>It is voted on for</span>
                <span class="inline"><input id="create-votes" type="number" min="1" max={info.maxvoteperiods} step="1" bind:value={votePeriods} /> <span class="muted">period(s)</span></span>
              </label>
            </div>
            <ol class="timeline" aria-label="Timeline">
              <li><span class="hm-stage live">Trading</span> until period {eventPeriod} ends{#if info.periodseconds}, {formatTime(periodEnd(info, eventPeriod))}{/if} (<Countdown {info} target={periodEnd(info, eventPeriod)} phrase="in" />)</li>
              <li><span class="hm-stage vote">Votes sealed</span> from period {eventPeriod + 1} until the reveal phase of period {closePeriod}{#if info.periodseconds}, {formatTime(revealStart(info, closePeriod))}{/if}</li>
              <li><span class="hm-stage vote">Votes revealed</span> in the last third of period {closePeriod}</li>
              <li><span class="hm-stage done">Resolved</span> by the first block after period {closePeriod}{#if withMarket}; the market pays out{/if}</li>
            </ol>
            <p class="hm-help">Longer votes for answers that take time to establish. The event can be at most {info.maxperiodsahead} periods ahead; a vote lasts at most {info.maxvoteperiods} periods.</p>
          {/if}
        {:else if step === 3}
          <h2>The market</h2>
          <label class="field"><span>Title</span><input id="create-title" type="text" maxlength="400" bind:value={title} oninput={() => (titleTouched = true)} /></label>
          <label class="field">
            <span>Category</span>
            <span class="inline">
              <select class="pill" bind:value={category} aria-label="Category">
                <option value="">None</option>
                {#each CATEGORIES as c (c)}<option value={c}>{t(c)}</option>{/each}
              </select>
              <span class="faint small">Written at the start of the title, for the filters of the market browser.</span>
            </span>
          </label>
          <div class="pair">
            <label class="field">
              <span>Liquidity</span>
              <input id="create-liquidity" type="number" min={LIMITS.minLiquidity} step="0.1" bind:value={liquidity} />
              <span class="hm-help">The larger, the less a trade moves the chances. You fund the market with {formatUnits(funding, 4)}: the most it can lose, whatever is bet.</span>
            </label>
            <label class="field">
              <span>Trading fee, %</span>
              <input id="create-fee" type="number" min="0" max={LIMITS.maxFeeBps / 100} step="0.05" bind:value={feePct} />
              <span class="hm-help">Taken from every trade: half to the voters, a quarter to those who listed the decisions, a quarter to you.</span>
            </label>
          </div>
        {:else}
          <h2>Review</h2>
          <div class="review">
            {#if withMarket}
              <div class="panel preview" aria-label="Preview of the market">
                <span class="top"><span class="hm-stage live">Live</span><span class="badge">{kind === 'binary' ? 'Yes or no' : kind === 'scalar' ? 'Scale' : `${reuse ? picked.length : options.length} outcomes`}</span>{#if category}<span class="badge">{t(category)}</span>{/if}</span>
                <strong class="ptitle"><SafeText text={title} /></strong>
                <ul class="podds">
                  {#if kind === 'categorical'}
                    {#each reuse ? picked.map((id) => decisions.get(id)?.text ?? id) : options as o, i (i)}
                      <li><span class="hm-dot hm-c{(1 << i) % 8}"></span><SafeText text={o} /><span class="num">{formatChance(1 / states)}</span></li>
                    {/each}
                  {:else}
                    <li><span class="hm-dot hm-c1"></span>{kind === 'scalar' ? 'High' : 'Yes'}<span class="num">50.0%</span></li>
                    <li><span class="hm-dot hm-c0"></span>{kind === 'scalar' ? 'Low' : 'No'}<span class="num">50.0%</span></li>
                  {/if}
                </ul>
                <span class="faint small">{states} states · liquidity {formatUnits(liquidity, 2)} · fee {feePct}%</span>
              </div>
            {/if}
            <dl class="facts">
              {#if drafts.length}
                <dt>Decisions to list</dt><dd>{#each drafts as d, i (i)}<div><SafeText text={d.text} /></div>{/each}</dd>
                <dt>Answer known in</dt><dd>period {eventPeriod}</dd>
                <dt>Voted on until</dt><dd>period {closePeriod}</dd>
                <dt>Listing fees</dt><dd class="num">{formatUnits(listing)} or more</dd>
              {:else if reuse}
                <dt>Decisions</dt><dd>{#each picked as id (id)}<div><SafeText text={decisions.get(id)?.text ?? id} /></div>{/each}</dd>
              {/if}
              {#if withMarket}
                <dt>Funding</dt><dd class="num">{formatUnits(funding, 8)}</dd>
                <dt>Opening fee</dt><dd>For the voters: more, the more markets there are on the same first decision</dd>
              {/if}
            </dl>
          </div>
        {/if}

        {#if stepProblems.length && (step === 4 || question || picked.length || step > 1)}
          <ul class="hm-problems" role="alert">{#each stepProblems as p (p)}<li><SafeText text={p} /></li>{/each}</ul>
        {/if}

        <div class="row nav">
          <button type="button" class="btn ghost" disabled={step === 0} onclick={() => (step = step === 4 && !withMarket ? 2 : step - 1)}>Back</button>
          {#if step < 4}
            <button type="button" class="btn primary" id="create-next" disabled={stepProblems.length > 0} onclick={() => (step = step === 2 && !withMarket ? 4 : step + 1)}>Next</button>
          {:else}
            <button type="button" class="btn primary" id="create-submit" disabled={problems.length > 0} onclick={submit}><Icon name="check" size={15} /> {withMarket ? 'Open the market' : 'List the decision'}</button>
            <button type="button" class="btn ghost" onclick={reset}>Start again</button>
          {/if}
        </div>
      </div>
    </section>
  {/if}
</div>

<style>
  .create {
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
  .steps {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-wrap: wrap;
    gap: 0.4rem;
  }
  .steps button {
    display: inline-flex;
    align-items: center;
    gap: 0.45rem;
    padding: 0.35rem 0.8rem 0.35rem 0.4rem;
    border: 1px solid var(--line);
    border-radius: 999px;
    background: var(--surface-2);
    color: var(--text-muted);
    font: inherit;
    font-weight: 600;
    cursor: pointer;
  }
  .steps .n {
    display: inline-grid;
    place-items: center;
    width: 1.5rem;
    height: 1.5rem;
    border-radius: 50%;
    background: var(--raised);
    font-size: 0.8rem;
  }
  .steps .on button {
    border-color: var(--accent);
    color: var(--text);
  }
  .steps .on .n,
  .steps .done .n {
    background: var(--accent);
    color: var(--on-accent);
  }
  .wizard .panel-body {
    padding-top: 1.2rem;
    display: flex;
    flex-direction: column;
    gap: 0.9rem;
    max-width: 52rem;
  }
  .wizard h2 {
    font-size: 1.15rem;
  }
  .kinds {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(14rem, 100%), 1fr));
    gap: 0.7rem;
  }
  .kind {
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
    padding: 1rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-l);
    background: var(--surface-2);
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .kind[aria-checked='true'] {
    border-color: var(--accent);
    background: var(--accent-soft);
  }
  .field {
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
    min-width: 0;
  }
  .field > span:first-child {
    font-size: 0.88rem;
    color: var(--text-muted);
  }
  .inline {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
  }
  .inline input {
    width: 7rem;
  }
  .pair {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(14rem, 100%), 1fr));
    gap: 0.8rem;
  }
  .options {
    display: flex;
    flex-direction: column;
    gap: 0.45rem;
    align-items: flex-start;
  }
  .opt {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    width: 100%;
    max-width: 30rem;
  }
  .opt input {
    flex: 1;
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
    border: 0;
    background: none;
    box-shadow: none;
  }
  .picks {
    list-style: none;
    margin: 0;
    padding: 0;
    max-height: 22rem;
    overflow-y: auto;
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
  }
  .timeline {
    margin: 0;
    padding-left: 1.4rem;
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }
  .timeline li {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem;
  }
  .review {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(18rem, 100%), 1fr));
    gap: 1rem;
    align-items: start;
  }
  .preview {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    padding: 1rem;
  }
  .preview .top {
    display: flex;
    flex-wrap: wrap;
    gap: 0.35rem;
  }
  .ptitle {
    overflow-wrap: anywhere;
  }
  .podds {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
  }
  .podds li {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }
  .podds .num {
    margin-left: auto;
    font-weight: 700;
  }
  .nav {
    justify-content: flex-start;
    gap: 0.5rem;
  }
  .jobs ul {
    list-style: none;
    margin: 0;
    padding: 0 1rem 1rem;
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }
  .jobs li {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
  }
  .what {
    font-weight: 600;
    overflow-wrap: anywhere;
  }
  .small {
    font-size: 0.82rem;
  }
</style>
