<script lang="ts">
  import { stacked } from '../../lib/actions';
  // The voting center: what the reputation of this wallet votes on. The decisions to vote on now, with
  // the time left to seal a vote; the votes to reveal, with the time left to reveal them (a vote not
  // revealed does not count); the decisions that come up next; those waiting to be resolved; and the
  // votes of the wallet, with how they fared. A vote is sealed first: nobody sees it; the app keeps the
  // answer sealed, to reveal it in the reveal phase of the last period of the vote. Several decisions
  // that close together can be sealed in one ballot. Reminders, as desktop notifications, when a vote
  // is due. The Vote tab of the Qt window of Hivemind.
  import { t } from '../../lib/i18n.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import NoWallet from '../../components/NoWallet.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { rpc } from '../../lib/rpc';
  import { formatTime, shortHash } from '../../lib/format';
  import { go, PAGES } from '../../lib/nav.svelte';
  import { toast, wallets, walletsEnabled } from '../../lib/state.svelte';
  import '../hm/hm.css';
  import Countdown from '../hm/Countdown.svelte';
  import { formatChance, formatUnits, parseAnswer, periodEnd, revealStart, voteDeadline, voteStage, type Decision } from '../hm/model';
  import { hm, noteRevealed, noteSealed, refreshHivemind, sealKey, setReminders, startHivemind, type SealRecord } from '../hm/store.svelte';

  $effect(() => startHivemind());

  const noWallet = $derived(!walletsEnabled() || wallets.current === null);
  const info = $derived(hm.info);
  const accounts = $derived(hm.reputation.slice().sort((a, b) => b.amount - a.amount));
  let voter = $state('');
  $effect(() => {
    if (!accounts.some((a) => a.account === voter)) voter = accounts[0]?.account ?? '';
  });
  const power = $derived(accounts.reduce((n, a) => n + a.amount, 0));
  const voterAmount = $derived(accounts.find((a) => a.account === voter)?.amount ?? 0);
  const tooSmall = $derived(!!info && voterAmount > 0 && voterAmount < info.minreputation);

  const staged = $derived(info ? hm.decisions.map((d) => ({ d, stage: voteStage(d, info) })) : []);
  const toSeal = $derived(staged.filter((x) => x.stage === 'seal').map((x) => x.d).sort((a, b) => a.closeperiod - b.closeperiod));
  const toReveal = $derived(staged.filter((x) => x.stage === 'reveal').map((x) => x.d));
  const upcoming = $derived(staged.filter((x) => x.stage === 'upcoming').map((x) => x.d).sort((a, b) => a.eventperiod - b.eventperiod).slice(0, 20));
  const waiting = $derived(staged.filter((x) => x.stage === 'waiting').map((x) => x.d));

  /** What this app sealed for the voter chosen, by decision. */
  const sealed = $derived(new Map(Object.values(hm.journal.sealed).filter((s) => s.wallet === wallets.current && s.account === voter).map((s) => [s.decision, s])));
  /** What the chain has of the voter's votes: sealed or revealed. */
  function chainVote(d: Decision) {
    return hm.details[d.id]?.votes?.find((v) => v.voter === voter);
  }
  const revealDue = $derived(toReveal.filter((d) => sealed.has(d.id) || (chainVote(d) && !chainVote(d)?.revealed)));
  const past = $derived(hm.journal.votes.filter((v) => v.wallet === wallets.current));

  /** The answers being written, by decision. */
  let answers = $state<Record<string, string>>({});
  let busy = $state<string | null>(null);

  function answerOf(d: Decision): string {
    return answers[d.id] ?? sealed.get(d.id)?.answer ?? '';
  }
  function shownAnswer(a: string): string {
    return a === 'yes' ? t('Yes') : a === 'no' ? t('No') : a === 'unknown' ? t('Not known yet') : a;
  }

  async function seal(d: Decision) {
    const wallet = wallets.current;
    if (wallet === null || !voter) return;
    const parsed = parseAnswer(answerOf(d), d);
    if (!parsed.ok) {
      await inform(t('Seal vote'), parsed.error, 'warning');
      return;
    }
    busy = d.id;
    try {
      const done = await walletRpc<{ txid: string }>(wallet, 'sealvote', [voter, d.id, parsed.value]);
      if (!done) return;
      // Kept at once, in a file of its own: the answer is needed to reveal the vote.
      if (await noteSealed({ wallet, account: voter, decision: d.id, answer: parsed.value, close: d.closeperiod, time: Math.floor(Date.now() / 1000), txid: done.txid })) {
        toast('success', t('Vote sealed'), t('Reveal it in the last third of period %1. The app keeps your answer until then.', String(d.closeperiod)));
      }
      void refreshHivemind();
    } catch (e) {
      await inform(t('Seal vote'), errorText(e), 'error');
    } finally {
      busy = null;
    }
  }

  /** The decisions to seal in one ballot: those with an answer, closing in the same period as the first. */
  const ballot = $derived.by(() => {
    const ready = toSeal.filter((d) => parseAnswer(answerOf(d), d).ok);
    if (ready.length < 2) return [];
    const close = ready[0].closeperiod;
    return ready.filter((d) => d.closeperiod === close);
  });

  async function sealBallot() {
    const wallet = wallets.current;
    if (wallet === null || !voter || ballot.length < 2) return;
    const votes: Record<string, string> = {};
    for (const d of ballot) {
      const p = parseAnswer(answerOf(d), d);
      if (p.ok) votes[d.id] = p.value;
    }
    const r = await ask({
      title: t('Seal a ballot'),
      message: t('Seal your answers to %1 decisions in one transaction? They are revealed together in the last third of period %2.', String(ballot.length), String(ballot[0].closeperiod)),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Seal', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = 'ballot';
    try {
      const done = await walletRpc<{ txid: string; closeperiod: number }>(wallet, 'sealballot', [voter, votes]);
      if (!done) return;
      const time = Math.floor(Date.now() / 1000);
      const kept = await noteSealed(...Object.entries(votes).map(([id, answer]) => ({ wallet, account: voter, decision: id, answer, close: done.closeperiod, time, txid: done.txid, ballot: true })));
      if (kept) toast('success', t('Ballot sealed'), t('%1 votes, revealed together in period %2.', String(ballot.length), String(done.closeperiod)));
      void refreshHivemind();
    } catch (e) {
      await inform(t('Seal a ballot'), errorText(e), 'error');
    } finally {
      busy = null;
    }
  }

  /**
   * The ballots of the voter the chain has, by the period they close in (getballot): a vote sealed in a ballot is
   * revealed with the others of the ballot, also when this app has no record of the answers (they are then given
   * by hand).
   */
  let ballots = $state<Record<string, string[] | null>>({});
  const ballotKey = (account: string, close: number) => `${account}|${close}`;
  $effect(() => {
    const account = voter;
    const periods = [...new Set(revealDue.filter((d) => !sealed.has(d.id)).map((d) => d.closeperiod))];
    for (const close of periods) {
      const key = ballotKey(account, close);
      if (key in ballots) continue;
      ballots[key] = null;
      rpc
        .raw<{ decisions: string[] } | null>('getballot', [account, close])
        .then((b) => (ballots = { ...ballots, [key]: b?.decisions ?? [] }))
        .catch(() => (ballots = { ...ballots, [key]: [] }));
    }
  });
  /** The decisions of the ballot a decision was sealed in, when the chain has one and this app has no record of it. */
  function chainBallot(d: Decision): string[] {
    const list = ballots[ballotKey(voter, d.closeperiod)];
    return list && list.includes(d.id) ? list : [];
  }

  async function reveal(d: Decision) {
    const wallet = wallets.current;
    if (wallet === null || !voter) return;
    const rec = sealed.get(d.id);
    const inBallot = !rec ? chainBallot(d) : [];
    if (inBallot.length) {
      // A ballot this app has no record of: revealed whole, with the answers given by hand for each of its decisions.
      const votes: Record<string, string> = {};
      for (const id of inBallot) {
        const other = hm.decisions.find((x) => x.id === id);
        const p = other ? parseAnswer(answers[id] ?? '', other) : null;
        if (!p || !p.ok) {
          await inform(t('Reveal vote'), t('This vote was sealed in a ballot of %1 decisions: give the answer you sealed for each of them, then reveal them together.', String(inBallot.length)), 'warning');
          return;
        }
        votes[id] = p.value;
      }
      busy = d.id;
      try {
        const done = await walletRpc<{ txid: string }>(wallet, 'revealballot', [voter, votes]);
        if (!done) return;
        for (const [id, answer] of Object.entries(votes)) noteRevealed(wallet, voter, id, answer, done.txid);
        toast('success', t('Ballot revealed'), t('%1 votes, in the transaction %2.', String(inBallot.length), shortHash(done.txid, 10)));
        void refreshHivemind();
      } catch (e) {
        await inform(t('Reveal vote'), errorText(e), 'error');
      } finally {
        busy = null;
      }
      return;
    }
    const answer = rec?.answer ?? parseAnswer(answerOf(d), d);
    const value = typeof answer === 'string' ? answer : answer.ok ? answer.value : null;
    if (value === null) {
      await inform(t('Reveal vote'), t('Choose the answer you sealed: this app did not seal it, so it does not know it.'), 'warning');
      return;
    }
    busy = d.id;
    try {
      if (rec?.ballot) {
        // A ballot is revealed whole, with the answers it was sealed with.
        const parts = [...sealed.values()].filter((s: SealRecord) => s.ballot && s.txid === rec.txid);
        const votes = Object.fromEntries(parts.map((s) => [s.decision, s.answer]));
        const done = await walletRpc<{ txid: string }>(wallet, 'revealballot', [voter, votes]);
        if (!done) return;
        for (const s of parts) noteRevealed(wallet, voter, s.decision, s.answer, done.txid);
        toast('success', t('Ballot revealed'), t('%1 votes, in the transaction %2.', String(parts.length), shortHash(done.txid, 10)));
      } else {
        const done = await walletRpc<{ txid: string }>(wallet, 'revealvote', [voter, d.id, value]);
        if (!done) return;
        noteRevealed(wallet, voter, d.id, value, done.txid);
        toast('success', t('Vote revealed'), t('It counts when the vote ends, in the transaction %1.', shortHash(done.txid, 10)));
      }
      void refreshHivemind();
    } catch (e) {
      await inform(t('Reveal vote'), errorText(e), 'error');
    } finally {
      busy = null;
    }
  }

  async function revealAll() {
    for (const d of revealDue) if (sealed.has(d.id)) await reveal(d);
  }

  const marketsOn = (id: string) => hm.markets.filter((m) => m.decisions.includes(id));
  /** The market's chance of yes (of high), for a decision with one market on it alone: what the traders think. */
  function marketView(d: Decision): number | null {
    const m = marketsOn(d.id).find((x) => x.decisions.length === 1);
    return m?.states.find((s) => s.state === 1)?.price ?? null;
  }
</script>

<div class="page voting">
  <header class="page-head head">
    <div>
      <h1>Voting center</h1>
      {#if info}
        <p class="muted sub">
          Period {info.period} ·
          {#if info.phase === 'reveal'}<span class="hm-stage vote">Reveal phase</span> <Countdown {info} target={periodEnd(info, info.period)} urgent={info.periodblocks ? 1 : 900} phrase="ends" />{:else}<span class="hm-stage live">Seal phase</span> · reveals <Countdown {info} target={revealStart(info, info.period)} phrase="start" />{/if}
        </p>
      {/if}
    </div>
    <label class="check remind"><input type="checkbox" checked={hm.journal.reminders} onchange={(e) => setReminders((e.currentTarget as HTMLInputElement).checked)} /> <span><Icon name="bell" size={14} /> Remind me when a vote is due</span></label>
  </header>

  {#if noWallet}
    <NoWallet page={PAGES.voting} />
  {:else}
    <section class="hm-figures">
      <div class="panel hm-figure"><span class="muted">Your votecoin</span><strong class="num">{formatUnits(power, 4)}</strong><span class="faint small">{info && info.reputation ? `${formatChance(power / info.reputation)} of all` : ''}</span></div>
      <div class="panel hm-figure"><span class="muted">To vote on</span><strong class="num">{toSeal.length}</strong><span class="faint small">{toSeal.filter((d) => sealed.has(d.id)).length} sealed</span></div>
      <div class="panel hm-figure"><span class="muted">To reveal now</span><strong class="num" class:hm-down={revealDue.length > 0}>{revealDue.length}</strong><span class="faint small">a vote not revealed does not count</span></div>
      <div class="panel hm-figure"><span class="muted">For the voters</span><strong class="num">{formatUnits(info?.voterpot ?? 0, 4)}</strong><span class="faint small">fees, shared by the next vote</span></div>
    </section>

    {#if accounts.length === 0}
      <p class="side-note side-warn">This wallet holds no votecoin, so it cannot vote. Votecoin comes from another account (a transfer) or from voting with the others.</p>
    {:else}
      <div class="row as">
        <label class="pick">
          <span class="muted">Vote as</span>
          <select class="pill" bind:value={voter} aria-label="Vote as">
            {#each accounts as a (a.account)}<option value={a.account} translate="no">{a.account.slice(0, 18)}… · {formatUnits(a.amount, 4)}</option>{/each}
          </select>
        </label>
        {#if tooSmall && info}<span class="hm-down small">This account holds less than {formatUnits(info.minreputation)}, the least that seals a vote.</span>{/if}
      </div>
    {/if}

    {#if revealDue.length}
      <section class="panel due">
        <div class="panel-head">
          <h2><Icon name="alert" size={16} /> Reveal now</h2>
          {#if [...sealed.values()].length > 1}<button type="button" class="btn small primary" disabled={busy !== null} onclick={revealAll}>Reveal all</button>{/if}
        </div>
        <ul class="list">
          {#each revealDue as d (d.id)}
            {@const rec = sealed.get(d.id)}
            <li>
              <div class="q"><SafeText text={d.text} /></div>
              <div class="meta">
                {#if rec}<span>You sealed <strong>{shownAnswer(rec.answer)}</strong>{rec.ballot ? ' (in a ballot)' : ''}</span>{:else}
                  {#if chainBallot(d).length}<span class="muted small">Sealed in a ballot of {chainBallot(d).length} decisions: give each answer, they are revealed together</span>{/if}
                  <span class="answer">
                    {#if d.scaled}<input type="text" inputmode="numeric" placeholder="The number you sealed" bind:value={answers[d.id]} aria-label="The answer you sealed" />{:else}
                      <select class="pill" bind:value={answers[d.id]} aria-label="The answer you sealed"><option value="">The answer you sealed</option><option value="yes">Yes</option><option value="no">No</option><option value="unknown">Not known yet</option></select>
                    {/if}
                  </span>
                {/if}
                <span class="muted"><Countdown info={info!} target={periodEnd(info!, d.closeperiod)} urgent={info?.periodblocks ? 1 : 900} phrase="ends" /></span>
                <button type="button" class="btn small primary" id="reveal-{d.id.slice(0, 8)}" disabled={busy !== null} onclick={() => reveal(d)}>Reveal vote</button>
              </div>
            </li>
          {/each}
        </ul>
      </section>
    {/if}

    <section class="panel">
      <div class="panel-head">
        <h2>To vote on</h2>
        {#if ballot.length >= 2}<button type="button" class="btn small" disabled={busy !== null || !voter} onclick={sealBallot}>Seal {ballot.length} in one ballot</button>{/if}
      </div>
      {#if toSeal.length === 0}
        <p class="hm-empty">No decision is being voted on. A decision comes up for the vote in the period after the one its answer becomes known in.</p>
      {:else}
        <div class="hm-grid pad">
          {#each toSeal as d (d.id)}
            {@const rec = sealed.get(d.id)}
            {@const onChain = chainVote(d)}
            {@const view = marketView(d)}
            <article class="panel ballot" aria-label="A decision to vote on">
              <span class="top">
                <span class="badge">{d.scaled ? `${d.min} to ${d.max}` : 'Yes or no'}</span>
                {#if rec}<span class="hm-stage live">Sealed: {shownAnswer(rec.answer)}</span>{:else if onChain}<span class="hm-stage live">Sealed</span>{:else}<span class="hm-stage vote">Not voted</span>{/if}
              </span>
              <strong class="q"><SafeText text={d.text} /></strong>
              <span class="muted small">Seal before the reveal phase of period {d.closeperiod}: <Countdown info={info!} target={voteDeadline(d, info!, 'seal') ?? 0} urgent={info?.periodblocks ? 2 : 3600} /> · {hm.details[d.id]?.votes?.length ?? 0} vote(s) so far{#if view !== null}{' · '}<span>the market says {formatChance(view)}</span>{/if}</span>
              <div class="answers" role="radiogroup" aria-label="Your answer">
                {#if d.scaled}
                  <input type="text" inputmode="numeric" placeholder="{d.min} to {d.max}" value={answerOf(d) === 'unknown' ? '' : answerOf(d)} oninput={(e) => (answers[d.id] = (e.currentTarget as HTMLInputElement).value)} aria-label="The answer" />
                {:else}
                  <button type="button" role="radio" class="hm-chip" aria-checked={answerOf(d) === 'yes'} onclick={() => (answers[d.id] = 'yes')}>Yes</button>
                  <button type="button" role="radio" class="hm-chip" aria-checked={answerOf(d) === 'no'} onclick={() => (answers[d.id] = 'no')}>No</button>
                {/if}
                <button type="button" role="radio" class="hm-chip" aria-checked={answerOf(d) === 'unknown'} title="If most of the reputation says so, the decision is voted on again later" onclick={() => (answers[d.id] = 'unknown')}>Not known yet</button>
              </div>
              <div class="row">
                <button type="button" class="btn small primary" id="seal-{d.id.slice(0, 8)}" disabled={busy !== null || !voter || tooSmall || !parseAnswer(answerOf(d), d).ok} onclick={() => seal(d)}><Icon name="lock" size={14} /> {rec || onChain ? 'Seal another answer' : 'Seal vote'}</button>
                {#each marketsOn(d.id).slice(0, 1) as m (m.id)}<button type="button" class="btn small ghost" onclick={() => go('markets', `m-${m.id}`)}>Market</button>{/each}
              </div>
            </article>
          {/each}
        </div>
      {/if}
    </section>

    <div class="cols">
      <section class="panel">
        <div class="panel-head"><h2>Coming up</h2></div>
        {#if upcoming.length === 0}
          <p class="hm-empty">Nothing comes up yet.</p>
        {:else}
          <ul class="list">
            {#each upcoming as d (d.id)}
              <li><div class="q"><SafeText text={d.text} /></div><span class="muted small">voting <Countdown info={info!} target={periodEnd(info!, d.eventperiod)} phrase="starts" /> (period {d.eventperiod + 1})</span></li>
            {/each}
          </ul>
        {/if}
      </section>
      <section class="panel">
        <div class="panel-head"><h2>Waiting to be resolved</h2></div>
        {#if waiting.length === 0}
          <p class="hm-empty">None.</p>
        {:else}
          <ul class="list">
            {#each waiting as d (d.id)}<li><div class="q"><SafeText text={d.text} /></div><span class="muted small">the next block resolves it</span></li>{/each}
          </ul>
        {/if}
      </section>
    </div>

    <section class="panel">
      <div class="panel-head"><h2>Your votes</h2><span class="muted small">revealed in this app</span></div>
      {#if past.length === 0}
        <p class="hm-empty">No vote yet.</p>
      {:else}
        <div class="hm-wrap">
          <table class="hm-table" use:stacked aria-label="Your votes">
            <thead><tr><th>Revealed</th><th>Decision</th><th>Your answer</th><th>Outcome</th><th></th></tr></thead>
            <tbody>
              {#each past as v (v.decision + v.account)}
                <tr>
                  <td>{formatTime(v.time, true)}</td>
                  <td class="cut"><SafeText text={v.text || v.decision} /></td>
                  <td>{shownAnswer(v.answer)}</td>
                  <td>{#if v.outcome !== undefined}<SafeText text={v.outcome} />{:else}<span class="faint">not resolved yet</span>{/if}</td>
                  <td>{#if v.agreed !== undefined}<span class="status {v.agreed ? 'ok' : 'bad'}">{v.agreed ? 'With the others' : 'Against the outcome'}</span>{/if}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </section>
  {/if}
</div>

<style>
  .voting {
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
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem;
    margin: 0.25rem 0 0;
  }
  .remind span {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
  }
  .as {
    align-items: center;
    flex-wrap: wrap;
  }
  .pick {
    display: flex;
    align-items: center;
    gap: 0.45rem;
  }
  .due {
    border-color: var(--warning);
  }
  .list {
    list-style: none;
    margin: 0;
    padding: 0 1rem 1rem;
    display: flex;
    flex-direction: column;
    gap: 0.7rem;
  }
  .list li {
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
  }
  .meta {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.6rem;
  }
  .q {
    overflow-wrap: anywhere;
  }
  .pad {
    padding: 0 1rem 1rem;
  }
  .ballot {
    display: flex;
    flex-direction: column;
    gap: 0.55rem;
    padding: 0.9rem;
    background: var(--surface-2);
  }
  .top {
    display: flex;
    flex-wrap: wrap;
    gap: 0.35rem;
  }
  .answers {
    display: flex;
    flex-wrap: wrap;
    gap: 0.35rem;
  }
  .answers input {
    width: 9rem;
  }
  .cols {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(20rem, 100%), 1fr));
    gap: 1rem;
  }
  .small {
    font-size: 0.83rem;
  }
</style>
