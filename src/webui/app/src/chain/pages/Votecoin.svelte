<script lang="ts">
  // Votecoin: the reputation that votes on the decisions. What this wallet holds, account by account, and
  // its share of all; how it went over time (as this app saw it: resolutions move reputation from those
  // who vote against the outcome to those who vote with it); moving some to another account; and who
  // holds the most. The Reputation tab of the Qt window of Hivemind.
  import { t } from '../../lib/i18n.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import AddressInput from '../../components/AddressInput.svelte';
  import NoWallet from '../../components/NoWallet.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { formatTime, shortHash } from '../../lib/format';
  import { PAGES } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets, walletsEnabled } from '../../lib/state.svelte';
  import '../hm/hm.css';
  import { barWidth } from '../hm/actions';
  import { formatChance, formatUnits } from '../hm/model';
  import { hm, refreshHivemind, startHivemind } from '../hm/store.svelte';

  $effect(() => startHivemind());

  const noWallet = $derived(!walletsEnabled() || wallets.current === null);
  const info = $derived(hm.info);
  const supply = $derived(info?.reputation ?? 0);
  const mine = $derived(hm.reputation.slice().sort((a, b) => b.amount - a.amount));
  const total = $derived(mine.reduce((n, a) => n + a.amount, 0));
  const mineSet = $derived(new Set(mine.map((a) => a.account)));
  const history = $derived(wallets.current !== null ? (hm.journal.rep[wallets.current] ?? []) : []);
  const changes = $derived(history.map((p, i) => ({ ...p, delta: i ? p.amount - history[i - 1].amount : p.amount })).reverse());

  // The chart of the reputation over time.
  let width = $state(520);
  const tall = 150;
  const path = $derived.by(() => {
    if (history.length < 2) return '';
    const h0 = history[0].h;
    const h1 = Math.max(history[history.length - 1].h, h0 + 1);
    const top = Math.max(...history.map((p) => p.amount)) * 1.1 || 1;
    const x = (h: number) => 6 + ((h - h0) / (h1 - h0)) * (width - 12);
    const y = (a: number) => 8 + (1 - a / top) * (tall - 16);
    return history.map((p, i) => (i ? `H${x(p.h).toFixed(1)}V${y(p.amount).toFixed(1)}` : `M${x(p.h).toFixed(1)} ${y(p.amount).toFixed(1)}`)).join('');
  });

  // --- A transfer ---
  let from = $state('');
  $effect(() => {
    if (!mine.some((a) => a.account === from)) from = mine[0]?.account ?? '';
  });
  let to = $state('');
  let toValid = $state(false);
  let amountText = $state('');
  let formKey = $state(0);
  let busy = $state(false);
  const held = $derived(mine.find((a) => a.account === from)?.amount ?? 0);
  const amount = $derived(Number(amountText.replace(',', '.')));
  const toHolds = $derived(hm.holders.find((h) => h.account === to.trim())?.amount ?? 0);
  const problem = $derived.by(() => {
    if (!info || amountText.trim() === '') return '';
    if (!(amount > 0)) return t('Enter an amount above zero.');
    if (amount > held + 1e-12) return t('The account holds %1.', formatUnits(held));
    const left = held - amount;
    if (left > 1e-12 && left < info.minreputation) return t('A transfer leaves the account with none or with at least %1: move all of it, or less.', formatUnits(info.minreputation));
    if (toValid && toHolds + amount < info.minreputation) return t('An account holds none or at least %1: move more.', formatUnits(info.minreputation));
    if (to.trim() === from) return t('Move it to another account.');
    return '';
  });

  async function newAccount() {
    if (wallets.current === null) return;
    try {
      to = await rpc.call('getnewaddress', ['votecoin', 'bech32'], { wallet: wallets.current });
      formKey++;
    } catch (e) {
      await inform(t('Votecoin'), errorText(e), 'warning');
    }
  }

  async function transfer() {
    const wallet = wallets.current;
    if (wallet === null || !from || !toValid || problem || !(amount > 0)) return;
    const r = await ask({
      title: t('Move votecoin'),
      message: t('Move %1 votecoin from %2 to this account?', formatUnits(amount), shortHash(from, 10)),
      code: to.trim(),
      informative: t('Votecoin is the weight of a vote: whoever holds it votes with it. It moves with the next block.'),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Move', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const done = await walletRpc<{ txid: string }>(wallet, 'transfervotecoin', [from, to.trim(), amount.toFixed(8)]);
      if (!done) return;
      toast('success', t('Votecoin on its way'), t('In the transaction %1.', shortHash(done.txid, 10)));
      amountText = '';
      to = '';
      formKey++;
      void refreshHivemind();
    } catch (e) {
      await inform(t('Move votecoin'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
</script>

<div class="page votecoin">
  <header class="page-head"><h1>Votecoin</h1></header>

  {#if noWallet}
    <NoWallet page={PAGES.votecoin} />
  {:else}
    <section class="hm-figures">
      <div class="panel hm-figure"><span class="muted">This wallet holds</span><strong class="num">{formatUnits(total, 4)}</strong><span class="faint small">votecoin, in {mine.length} account(s)</span></div>
      <div class="panel hm-figure"><span class="muted">Share of all</span><strong class="num">{supply ? formatChance(total / supply) : '–'}</strong><span class="faint small">of {formatUnits(supply)}</span></div>
      <div class="panel hm-figure"><span class="muted">Voters</span><strong class="num">{info?.voters ?? '–'}</strong><span class="faint small">accounts that hold votecoin</span></div>
      <div class="panel hm-figure"><span class="muted">Least holding</span><strong class="num">{formatUnits(info?.minreputation ?? 0)}</strong><span class="faint small">to seal a vote</span></div>
    </section>

    <div class="cols">
      <section class="panel">
        <div class="panel-head"><h2>Over time</h2><span class="muted small">as this app saw it</span></div>
        <div class="panel-body">
          {#if history.length < 2}
            <p class="hm-help">The chart starts with the first change: a resolution moves votecoin from those who vote against the outcome, or not at all, to those who vote with it.</p>
          {:else}
            <div class="chart" bind:clientWidth={width}>
              <svg {width} height={tall} viewBox="0 0 {width} {tall}" role="img" aria-label="The votecoin of this wallet over time"><path class="rep" d={path} /></svg>
            </div>
          {/if}
          {#if changes.length}
            <div class="hm-wrap">
              <table class="hm-table" aria-label="Changes">
                <thead><tr><th>Time</th><th class="end">Block</th><th class="end">Change</th><th class="end">Holding</th></tr></thead>
                <tbody>
                  {#each changes.slice(0, 12) as c (c.h + ':' + c.t)}
                    <tr><td>{formatTime(c.t, true)}</td><td class="end num">{c.h}</td><td class="end num" class:hm-up={c.delta > 0} class:hm-down={c.delta < 0}>{c.delta > 0 ? '+' : ''}{formatUnits(c.delta, 4)}</td><td class="end num">{formatUnits(c.amount, 4)}</td></tr>
                  {/each}
                </tbody>
              </table>
            </div>
          {/if}
        </div>
      </section>

      <section class="panel">
        <div class="panel-head"><h2>Move votecoin</h2></div>
        {#if mine.length === 0}
          <p class="hm-empty">This wallet holds no votecoin.</p>
        {:else}
          <form class="panel-body form" onsubmit={(e) => { e.preventDefault(); transfer(); }}>
            <label class="field">
              <span>From</span>
              <select bind:value={from} aria-label="From">
                {#each mine as a (a.account)}<option value={a.account} translate="no">{a.account} · {formatUnits(a.amount, 4)}</option>{/each}
              </select>
            </label>
            {#key formKey}<AddressInput id="votecoin-to" label="To" bind:value={to} bind:valid={toValid} />{/key}
            <div><button type="button" class="btn small ghost" onclick={newAccount}><Icon name="plus" size={14} /> A new account of this wallet</button></div>
            <label class="field">
              <span>Amount</span>
              <span class="inline"><input id="votecoin-amount" type="text" inputmode="decimal" placeholder="0.00" bind:value={amountText} /><button type="button" class="btn small ghost" onclick={() => (amountText = String(held))}>All</button></span>
            </label>
            {#if problem}<p class="hm-problems" role="alert"><SafeText text={problem} /></p>{/if}
            <p class="hm-help">An account holds none or at least {formatUnits(info?.minreputation ?? 0)}. An account that sealed votes keeps at least that much until they close.</p>
            <div class="row"><button type="submit" class="btn primary" id="votecoin-send" disabled={busy || !!problem || !toValid || !(amount > 0)}><Icon name="send" size={15} /> Move</button></div>
          </form>
        {/if}
      </section>
    </div>

    <section class="panel">
      <div class="panel-head"><h2>Accounts of this wallet</h2></div>
      {#if mine.length === 0}
        <p class="hm-empty">None holds votecoin.</p>
      {:else}
        <div class="hm-wrap">
          <table class="hm-table" aria-label="Accounts of this wallet">
            <thead><tr><th>Account</th><th class="end">Votecoin</th><th>Share of all</th></tr></thead>
            <tbody>
              {#each mine as a (a.account)}
                <tr>
                  <td class="mono"><span translate="no">{a.account}</span> <CopyButton text={a.account} label="Copy the account" /></td>
                  <td class="end num">{formatUnits(a.amount, 8)}</td>
                  <td class="share"><span class="hm-bar hm-c0"><span use:barWidth={supply ? a.amount / supply : 0}></span></span><span class="num small">{supply ? formatChance(a.amount / supply) : '–'}</span></td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </section>

    <section class="panel">
      <div class="panel-head"><h2>Largest holders</h2><span class="muted small">{hm.holders.length} shown</span></div>
      <div class="hm-wrap">
        <table class="hm-table" aria-label="Largest holders">
          <thead><tr><th>Account</th><th class="end">Votecoin</th><th>Share of all</th></tr></thead>
          <tbody>
            {#each hm.holders.slice(0, 50) as h (h.account)}
              <tr>
                <td class="mono cut"><span translate="no">{h.account}</span>{#if mineSet.has(h.account)} <span class="badge accent">yours</span>{/if}</td>
                <td class="end num">{formatUnits(h.amount, 4)}</td>
                <td class="share"><span class="hm-bar hm-c3"><span use:barWidth={supply ? h.amount / supply : 0}></span></span><span class="num small">{supply ? formatChance(h.amount / supply) : '–'}</span></td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    </section>
  {/if}
</div>

<style>
  .votecoin {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .cols {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(22rem, 100%), 1fr));
    gap: 1rem;
    align-items: start;
  }
  .chart svg {
    display: block;
    width: 100%;
  }
  .rep {
    fill: none;
    stroke: var(--accent);
    stroke-width: 2.2;
  }
  .form {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
  }
  .field {
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
  }
  .field > span:first-child {
    font-size: 0.88rem;
    color: var(--text-muted);
  }
  .inline {
    display: flex;
    gap: 0.4rem;
    align-items: center;
  }
  .inline input {
    flex: 1;
  }
  .share {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    min-width: 10rem;
  }
  .share .hm-bar {
    flex: 1;
  }
  .small {
    font-size: 0.82rem;
  }
</style>
