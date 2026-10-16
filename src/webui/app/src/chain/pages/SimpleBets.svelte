<script lang="ts">
  // The Portfolio page in Simple mode: "My bets". One line a bet (an answer of a question the wallet holds
  // shares of, however many purchases): the question, the answer, what it pays if right, what it is worth
  // now and what that makes, its stage in words, and "Cash out" while the question is open (sold as the
  // trade panel of Advanced sells, hm/simpletrade.ts).
  import { t } from '../../lib/i18n.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import NoWallet from '../../components/NoWallet.svelte';
  import { go, PAGES } from '../../lib/nav.svelte';
  import { wallets, walletsEnabled } from '../../lib/state.svelte';
  import { commonQuestion, describeOutcome, heldByOutcome, outcomesOf, pnlOf, titleWithoutCategory, type Market, type Outcome } from '../hm/model';
  import { decisionMap, hm, tradesOf } from '../hm/store.svelte';
  import { cashOut, cashOutPreview, coins } from '../hm/simpletrade';

  const noWallet = $derived(!walletsEnabled() || wallets.current === null);
  const decisions = $derived(decisionMap());
  let busy = $state(false);

  function question(m: Market | undefined, id: string): string {
    if (!m) return id.slice(0, 16);
    const about = m.decisions.map((d) => decisions.get(d)).filter((d) => d !== undefined);
    if (m.decisions.length > 1) return commonQuestion(about.map((d) => d.text)) || titleWithoutCategory(m.text);
    return about[0]?.text ?? titleWithoutCategory(m.text);
  }
  function name(o: Outcome | undefined): string {
    if (!o) return '';
    if (o.kind === 'high') return t('Higher');
    if (o.kind === 'low') return t('Lower');
    return describeOutcome(o);
  }

  const rows = $derived(
    heldByOutcome(hm.positions).map((p) => {
      const m = hm.markets.find((x) => x.id === p.market);
      const price = m?.states[p.state]?.price ?? 0;
      const outcome = m ? outcomesOf(m, decisions).find((o) => o.state === p.state) : undefined;
      return { ...p, m, outcome, pnl: pnlOf(p.shares, price, tradesOf(wallets.current, p.market, p.state)), now: m && m.status === 'trading' ? cashOutPreview(m, p.state, p.accounts) : null };
    }),
  );
  const worth = $derived(rows.reduce((n, r) => n + (r.now ?? r.pnl.worth), 0));

  async function sell(r: (typeof rows)[number]) {
    const wallet = wallets.current;
    if (!r.m || wallet === null || busy) return;
    busy = true;
    try {
      await cashOut(wallet, r.m, r.state, name(r.outcome), r.accounts);
    } finally {
      busy = false;
    }
  }
</script>

<div class="page simple-bets">
  <header class="page-head head">
    <div>
      <h1>My bets</h1>
      {#if !noWallet && rows.length}<p class="muted sub">{t('%1 bet(s), worth about %2 now.', String(rows.length), coins(worth))}</p>{/if}
    </div>
    <button type="button" class="btn" onclick={() => go('markets')}><Icon name="market" size={15} /> Questions</button>
  </header>

  {#if noWallet}
    <NoWallet page={PAGES.portfolio} />
  {:else if rows.length === 0}
    <div class="panel none">
      <p class="hm-empty">No bet yet. Pick a question and bet on an answer.</p>
      <button type="button" class="btn primary" onclick={() => go('markets')}>See the questions</button>
    </div>
  {:else}
    <ul class="bets" aria-label="My bets">
      {#each rows as r (`${r.market}:${r.state}`)}
        <li class="panel bet">
          <button type="button" class="link q" onclick={() => go('markets', `m-${r.market}`)}><SafeText text={question(r.m, r.market)} /></button>
          <span class="answer">Your answer: <strong>{name(r.outcome)}</strong></span>
          <dl>
            <dt>Pays if right</dt><dd class="num">{coins(r.shares)}</dd>
            <dt>Worth now</dt><dd class="num">≈ {coins(r.now ?? r.pnl.worth)}</dd>
            {#if r.pnl.pnl !== null}<dt>So far</dt><dd class="num" class:hm-up={r.pnl.pnl > 0} class:hm-down={r.pnl.pnl < 0}>{r.pnl.pnl >= 0 ? '+' : '−'}{coins(Math.abs(r.pnl.pnl))}</dd>{/if}
          </dl>
          <span class="stage faint small">
            {#if r.m?.status === 'trading'}Open: cash out any time, or wait for the answer.{:else if r.m?.status === 'closed'}Waiting for the answer: the bet pays once it is decided.{:else}Answered.{/if}
          </span>
          {#if r.m?.status === 'trading' && r.now !== null}
            <button type="button" class="btn" disabled={busy} onclick={() => sell(r)}>{t('Cash out now for ≈ %1', coins(r.now))}</button>
          {/if}
        </li>
      {/each}
    </ul>
    <p class="faint small">Every trade, the average prices and the accounts are on this page in Advanced mode.</p>
  {/if}
</div>

<style>
  .simple-bets {
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
  .none {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 0.6rem;
    padding: 1rem;
  }
  .bets {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(min(20rem, 100%), 1fr));
    gap: 0.9rem;
    margin: 0;
    padding: 0;
    list-style: none;
  }
  .bet {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 0.55rem;
    padding: 1rem 1.1rem;
  }
  .q {
    font-size: 1.05rem;
    font-weight: 650;
    text-align: left;
  }
  dl {
    display: grid;
    grid-template-columns: auto auto;
    gap: 0.25rem 1.2rem;
    margin: 0;
  }
  dt {
    color: var(--text-muted);
  }
  dd {
    margin: 0;
    text-align: right;
    font-weight: 600;
  }
</style>
