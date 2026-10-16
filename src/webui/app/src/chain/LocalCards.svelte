<script lang="ts">
  // The cards of Hivemind on the home of the dashboard: the positions of the wallet, the markets that
  // close soonest, the votes due (to seal, to reveal) with their deadlines, and the votecoin of the
  // wallet. Each opens its page.
  import SafeText from '../components/SafeText.svelte';
  import Icon from '../components/Icon.svelte';
  import Card from '../views/home/Card.svelte';
  import { go } from '../lib/nav.svelte';
  import { wallets } from '../lib/state.svelte';
  import './hm/hm.css';
  import Countdown from './hm/Countdown.svelte';
  import { formatChance, formatUnits, heldByOutcome, leader, periodEnd, pnlOf, revealStart, titleWithoutCategory, voteStage } from './hm/model';
  import { hm, startHivemind, tradesOf } from './hm/store.svelte';
  import { simpleMode } from '../lib/uimode.svelte';

  // Simple mode (lib/uimode.svelte.ts): my bets and the questions closing soon, in plain words; the votes and
  // votecoin are Advanced's.
  const simple = $derived(simpleMode());

  $effect(() => startHivemind());

  const info = $derived(hm.info);
  const positions = $derived(
    heldByOutcome(hm.positions).map((p) => {
      const m = hm.markets.find((x) => x.id === p.market);
      const price = m?.states[p.state]?.price ?? 0;
      return { p, m, pnl: pnlOf(p.shares, price, tradesOf(wallets.current, p.market, p.state)) };
    }),
  );
  const worth = $derived(positions.reduce((n, x) => n + x.pnl.worth, 0));
  const costed = $derived(positions.filter((x) => x.pnl.pnl !== null));
  const pnl = $derived(costed.reduce((n, x) => n + (x.pnl.pnl ?? 0), 0));
  const closing = $derived(
    hm.markets
      .filter((m) => m.status === 'trading' && m.tradinguntil !== undefined)
      .sort((a, b) => (a.tradinguntil ?? 0) - (b.tradinguntil ?? 0) || b.liquidity - a.liquidity)
      .slice(0, 3),
  );
  const sealedHere = $derived(new Set(Object.values(hm.journal.sealed).filter((s) => s.wallet === wallets.current).map((s) => s.decision)));
  const due = $derived.by(() => {
    if (!info) return { seal: [], reveal: [] };
    const staged = hm.decisions.map((d) => ({ d, stage: voteStage(d, info) }));
    return {
      seal: staged.filter((x) => x.stage === 'seal' && !sealedHere.has(x.d.id)).map((x) => x.d),
      reveal: staged.filter((x) => x.stage === 'reveal' && sealedHere.has(x.d.id)).map((x) => x.d),
    };
  });
  const nextSeal = $derived(due.seal.length ? Math.min(...due.seal.map((d) => d.closeperiod)) : null);
  const votecoin = $derived(hm.reputation.reduce((n, r) => n + r.amount, 0));
  const lastChange = $derived.by(() => {
    const list = wallets.current !== null ? (hm.journal.rep[wallets.current] ?? []) : [];
    return list.length > 1 ? list[list.length - 1].amount - list[list.length - 2].amount : 0;
  });
</script>

{#if hm.loaded && !hm.error}
  <div class="hm-cards">
    <Card title={simple ? 'My bets' : 'My positions'} icon="portfolio" open={() => go('portfolio')} openLabel="Open the portfolio" stat>
      <span class="value num">{formatUnits(worth, 4)}</span>
      {#if simple}
        <dl class="pairs">
          <dt>Bets</dt><dd class="num">{positions.length}</dd>
          <dt>Waiting for an answer</dt><dd class="num">{positions.filter((x) => x.m?.status === 'closed').length}</dd>
        </dl>
      {:else}
      <dl class="pairs">
        <dt>Positions</dt><dd class="num">{positions.length}</dd>
        <dt>Profit or loss</dt><dd class="num" class:hm-up={pnl > 0} class:hm-down={pnl < 0}>{costed.length ? `${pnl >= 0 ? '+' : ''}${formatUnits(pnl, 4)}` : '–'}</dd>
        <dt>Waiting for a vote</dt><dd class="num">{positions.filter((x) => x.m?.status === 'closed').length}</dd>
      </dl>
      {/if}
      {#snippet actions()}
        <button type="button" class="btn grow" onclick={() => go('markets')}><Icon name="market" size={15} /> {simple ? 'Bet' : 'Trade'}</button>
        {#if !simple}<button type="button" class="btn ghost" onclick={() => go('payouts')}>Payouts</button>{/if}
      {/snippet}
    </Card>

    <Card title="Closing soon" icon="clock" open={() => go('markets')} openLabel="Open the markets" stat>
      {#if closing.length === 0}
        <p class="muted">No market is trading.</p>
      {:else}
        <ul class="closing">
          {#each closing as m (m.id)}
            {@const top = leader(m)}
            <li>
              <button type="button" class="link" onclick={() => go('markets', `m-${m.id}`)}><SafeText text={titleWithoutCategory(m.text)} /></button>
              <span class="faint small">{#if top}{formatChance(top.price)} · {/if}{#if info && m.tradinguntil !== undefined}<Countdown {info} target={periodEnd(info, m.tradinguntil)} />{/if}</span>
            </li>
          {/each}
        </ul>
      {/if}
      {#snippet actions()}
        {#if simple}
          <button type="button" class="btn grow" onclick={() => go('markets')}><Icon name="market" size={15} /> See the questions</button>
        {:else}
          <button type="button" class="btn grow" onclick={() => go('create', 'market')}><Icon name="create" size={15} /> Create a market</button>
        {/if}
      {/snippet}
    </Card>

    {#if !simple}
    <Card title="Votes due" icon="ballot" open={() => go('voting')} openLabel="Open the voting center" stat>
      <span class="value num" class:hm-down={due.reveal.length > 0}>{due.reveal.length + due.seal.length}</span>
      <dl class="pairs">
        <dt>To reveal now</dt><dd class="num">{due.reveal.length}</dd>
        <dt>To seal</dt><dd class="num">{due.seal.length}</dd>
        <dt>Next deadline</dt><dd>{#if info && due.reveal.length}<Countdown {info} target={periodEnd(info, info.period)} urgent={info.periodblocks ? 1 : 900} />{:else if info && nextSeal !== null}<Countdown {info} target={revealStart(info, nextSeal)} />{:else}–{/if}</dd>
      </dl>
      {#snippet actions()}
        <button type="button" class="btn grow" class:primary={due.reveal.length > 0} onclick={() => go('voting')}><Icon name="ballot" size={15} /> {due.reveal.length ? 'Reveal' : 'Vote'}</button>
      {/snippet}
    </Card>

    <Card title="Votecoin" icon="votecoin" open={() => go('votecoin')} openLabel="Open votecoin" stat>
      <span class="value num">{formatUnits(votecoin, 4)}</span>
      <dl class="pairs">
        <dt>Share of all</dt><dd class="num">{info?.reputation ? formatChance(votecoin / info.reputation) : '–'}</dd>
        <dt>Last change</dt><dd class="num" class:hm-up={lastChange > 0} class:hm-down={lastChange < 0}>{lastChange ? `${lastChange > 0 ? '+' : ''}${formatUnits(lastChange, 4)}` : '–'}</dd>
        <dt>Period</dt><dd class="num">{info ? `${info.period} · ${info.phase === 'reveal' ? 'reveal' : 'seal'}` : '–'}</dd>
      </dl>
      {#snippet actions()}
        <button type="button" class="btn grow" onclick={() => go('votecoin')}><Icon name="send" size={15} /> Move</button>
        <button type="button" class="btn ghost" onclick={() => go('decisions', 'calendar')}>Calendar</button>
      {/snippet}
    </Card>
    {/if}
  </div>
{/if}

<style>
  .hm-cards {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(14.5rem, 1fr));
    gap: 1rem;
  }
  .value {
    display: block;
    margin: 0.1rem 0 0.55rem;
    font-size: 1.55rem;
    font-weight: 650;
    letter-spacing: -0.02em;
  }
  .pairs {
    display: grid;
    /* The labels keep their longest word; the values wrap when the card is narrow (as the kit's cards). */
    grid-template-columns: minmax(min-content, 1fr) minmax(0, auto);
    gap: 0.35rem 1rem;
    margin: 0;
    padding-top: 0.6rem;
    border-top: 1px solid var(--line);
  }
  .pairs dt {
    color: var(--text-muted);
    font-size: 0.86rem;
    overflow-wrap: break-word;
  }
  .pairs dd {
    min-width: 0;
    overflow-wrap: anywhere;
    margin: 0;
    text-align: right;
    font-weight: 600;
    font-size: 0.92rem;
  }
  .closing {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 0.55rem;
  }
  .closing li {
    display: flex;
    flex-direction: column;
    gap: 0.1rem;
    min-width: 0;
  }
  .link {
    border: 0;
    padding: 0;
    background: none;
    color: var(--text);
    font: inherit;
    font-weight: 600;
    text-align: left;
    cursor: pointer;
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
  }
  .link:hover {
    text-decoration: underline;
  }
  .small {
    font-size: 0.82rem;
  }
  @container (max-width: 520px) {
    .hm-cards {
      grid-template-columns: minmax(0, 1fr);
    }
  }
</style>
