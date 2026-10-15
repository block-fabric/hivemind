<script lang="ts">
  // Payouts and settlements: what the markets paid this wallet. A market pays when the last of its
  // decisions is resolved: every share gets what its outcome was worth, in the coinbase of the block
  // that settles it, to the account that holds it; the author of a market gets what its market maker has
  // left; the authors of decisions and the voters get their share of the fees. Shown here: the payouts
  // the wallet received (outputs of a coinbase to its accounts of the markets), the markets settled
  // with what a share of each outcome paid, and the positions waiting for a vote, with what they would
  // pay at the chances now.
  import { t } from '../../lib/i18n.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import Amount from '../../components/Amount.svelte';
  import NoWallet from '../../components/NoWallet.svelte';
  import { formatTime, shortHash } from '../../lib/format';
  import { go, PAGES } from '../../lib/nav.svelte';
  import { node, wallets, walletsEnabled } from '../../lib/state.svelte';
  import { current, refreshRecords } from '../../lib/walletdata.svelte';
  import '../hm/hm.css';
  import Countdown from '../hm/Countdown.svelte';
  import OutcomeName from '../hm/OutcomeName.svelte';
  import { formatChance, formatUnits, outcomesOf, periodEnd, statePayout, titleWithoutCategory } from '../hm/model';
  import { decisionMap, hm, startHivemind } from '../hm/store.svelte';

  $effect(() => startHivemind());
  $effect(() => {
    void wallets.txVersion;
    void node.tip?.hash;
    if (wallets.current !== null) void refreshRecords();
  });

  const noWallet = $derived(!walletsEnabled() || wallets.current === null);
  const decisions = $derived(decisionMap());

  /** The labels the wallet gives the accounts of the markets (src/wallet/rpc/hivemind.cpp). */
  const KINDS: Partial<Record<string, 'shares' | 'market' | 'decision'>> = { 'market shares': 'shares', 'market author': 'market', 'decision author': 'decision' };
  const voters = $derived(new Set(hm.reputation.map((r) => r.account)));
  const received = $derived(
    current.records
      .filter((r) => r.coinbase && r.amount > 0n && (KINDS[r.label] !== undefined || voters.has(r.address)))
      .map((r) => ({ r, kind: KINDS[r.label] ?? ('voter' as const) }))
      .sort((a, b) => b.r.time - a.r.time),
  );
  const totals = $derived({
    shares: received.filter((x) => x.kind === 'shares').reduce((n, x) => n + x.r.amount, 0n),
    authors: received.filter((x) => x.kind === 'market' || x.kind === 'decision').reduce((n, x) => n + x.r.amount, 0n),
    voters: received.filter((x) => x.kind === 'voter').reduce((n, x) => n + x.r.amount, 0n),
  });

  /** Markets settled that this wallet traded in (by the trades of this app) or opened. */
  const settled = $derived.by(() => {
    const traded = new Set(hm.journal.trades.filter((x) => x.wallet === wallets.current).map((x) => x.market));
    const opened = new Set(wallets.current !== null ? (hm.journal.mine[wallets.current] ?? []) : []);
    return hm.markets
      .filter((m) => m.status === 'settled')
      .map((m) => {
        const outs = m.decisions.map((d) => decisions.get(d)?.outcome ?? 0);
        const net = new Map<number, number>();
        for (const x of hm.journal.trades) if (x.wallet === wallets.current && x.market === m.id) net.set(x.state, (net.get(x.state) ?? 0) + x.shares);
        const paid = [...net].reduce((n, [s, q]) => n + Math.max(0, q) * statePayout(outs, s), 0);
        return { m, outs, net, paid, yours: traded.has(m.id) || opened.has(m.id) };
      })
      .sort((a, b) => Number(b.yours) - Number(a.yours) || b.m.height - a.m.height);
  });

  /** Positions waiting for the vote: what they pay at the chances now. */
  const pending = $derived(
    hm.positions
      .map((p) => ({ p, m: hm.markets.find((x) => x.id === p.market) }))
      .filter((x) => x.m && x.m.status === 'closed')
      .map((x) => ({ ...x, expected: x.p.shares * (x.m!.states[x.p.state]?.price ?? 0), close: Math.max(...x.m!.decisions.map((d) => decisions.get(d)?.closeperiod ?? 0)) })),
  );
  const KIND_WORDS = { shares: 'Shares', market: 'Market author', decision: 'Decision author', voter: 'Voter' } as const;
</script>

<div class="page payouts">
  <header class="page-head"><h1>Payouts</h1></header>

  {#if noWallet}
    <NoWallet page={PAGES.payouts} />
  {:else}
    <section class="hm-figures">
      <div class="panel hm-figure"><span class="muted">Shares paid</span><strong><Amount sats={totals.shares} /></strong><span class="faint small">settlements and sales</span></div>
      <div class="panel hm-figure"><span class="muted">As an author</span><strong><Amount sats={totals.authors} /></strong><span class="faint small">what markets had left, fees</span></div>
      <div class="panel hm-figure"><span class="muted">As a voter</span><strong><Amount sats={totals.voters} /></strong><span class="faint small">fees shared by the votes</span></div>
      <div class="panel hm-figure"><span class="muted">Waiting for a vote</span><strong class="num">{formatUnits(pending.reduce((n, x) => n + x.expected, 0), 4)}</strong><span class="faint small">at the chances now</span></div>
    </section>

    {#if pending.length}
      <section class="panel">
        <div class="panel-head"><h2>Waiting for the vote</h2></div>
        <div class="hm-wrap">
          <table class="hm-table" aria-label="Waiting for the vote">
            <thead><tr><th>Market</th><th>Outcome</th><th class="end">Shares</th><th class="end">Chance</th><th class="end">Expected</th><th>Vote ends</th></tr></thead>
            <tbody>
              {#each pending as x (x.p.market + x.p.account + x.p.state)}
                {@const o = outcomesOf(x.m!, decisions).find((y) => y.state === x.p.state)}
                <tr class="click" onclick={() => go('markets', `m-${x.p.market}`)}>
                  <td class="cut"><SafeText text={titleWithoutCategory(x.m!.text)} /></td>
                  <td>{#if o}<OutcomeName outcome={o} />{/if}</td>
                  <td class="end num">{formatUnits(x.p.shares)}</td>
                  <td class="end num">{formatChance(x.m!.states[x.p.state]?.price)}</td>
                  <td class="end num">{formatUnits(x.expected, 4)}</td>
                  <td>{#if hm.info}<Countdown info={hm.info} target={periodEnd(hm.info, x.close)} />{/if}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      </section>
    {/if}

    <section class="panel">
      <div class="panel-head"><h2>Received</h2><span class="muted small">outputs of a coinbase to the accounts of the markets</span></div>
      {#if received.length === 0}
        <p class="hm-empty">{current.recordsLoaded ? 'No payout yet. A market pays when its decisions are resolved.' : 'Loading the history…'}</p>
      {:else}
        <div class="hm-wrap">
          <table class="hm-table" aria-label="Payouts received">
            <thead><tr><th>Time</th><th>As</th><th class="end">Block</th><th class="end">Amount</th><th>Account</th><th>Transaction</th></tr></thead>
            <tbody>
              {#each received as x (x.r.key)}
                <tr>
                  <td>{formatTime(x.r.time, true)}</td>
                  <td><span class="status ok">{t(KIND_WORDS[x.kind])}</span></td>
                  <td class="end num">{x.r.blockHeight}</td>
                  <td class="end"><Amount sats={x.r.amount} /></td>
                  <td class="mono cut"><span translate="no">{x.r.address}</span></td>
                  <td class="mono"><span translate="no">{shortHash(x.r.txid, 8)}</span></td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </section>

    <section class="panel">
      <div class="panel-head"><h2>Settled markets</h2><span class="muted small">kept by the chain for 30 periods</span></div>
      {#if settled.length === 0}
        <p class="hm-empty">No market has settled yet.</p>
      {:else}
        <ul class="settled">
          {#each settled as s (s.m.id)}
            {@const outs = outcomesOf(s.m, decisions)}
            <li class="panel">
              <button type="button" class="link" onclick={() => go('markets', `m-${s.m.id}`)}><strong><SafeText text={titleWithoutCategory(s.m.text)} /></strong></button>
              {#if s.yours}<span class="badge accent">yours</span>{/if}
              <div class="pays">
                {#each outs.filter((o) => statePayout(s.outs, o.state) > 0) as o (o.state)}
                  <span class="pay"><span class="hm-dot hm-c{o.state % 8}"></span><OutcomeName outcome={o} /> <span class="num">{formatUnits(statePayout(s.outs, o.state), 4)} per share</span></span>
                {/each}
              </div>
              {#if s.net.size}<span class="muted small">Your recorded shares paid about <strong class="num">{formatUnits(s.paid, 4)}</strong></span>{/if}
            </li>
          {/each}
        </ul>
      {/if}
    </section>
  {/if}
</div>

<style>
  .payouts {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .settled {
    list-style: none;
    margin: 0;
    padding: 0 1rem 1rem;
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
  }
  .settled li {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem 0.8rem;
    padding: 0.8rem 1rem;
    background: var(--surface-2);
  }
  .link {
    border: 0;
    background: none;
    padding: 0;
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .link:hover strong {
    text-decoration: underline;
  }
  .pays {
    display: flex;
    flex-wrap: wrap;
    gap: 0.4rem 1rem;
    width: 100%;
  }
  .pay {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
  }
  .small {
    font-size: 0.82rem;
  }
</style>
