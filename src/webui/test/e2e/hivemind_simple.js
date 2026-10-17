// Hivemind in Simple mode against a live mainchain (run_sidechain.py --script hivemind_simple.js --js-var
// SIMPLE=true): coins brought in (a deposit by the helper of the mainchain wallet, merged mining on), two
// questions opened through RPC (a yes-or-no one, one with two options), then in Midnight and Light: the
// questions as cards, one opened with Yes and No and an amount, a bet placed, the bet in the question and in
// My bets, the cards of the home; at last a cash-out. Pictures of each.
(async () => {
  const { host, rpc, sleep, T, waitFor, check, snapshot, text, finish } = E2E;
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
  };
  const openDialog = () => $$('dialog[open]').pop();
  const button = (label, root = document) => $$('button', root).find((b) => b.textContent.trim().replace(/\s+/g, ' ').startsWith(label));
  async function answerDialog(title, label) {
    const d = await waitFor(`the dialog "${title}"`, () => {
      const el = openDialog();
      return el && $('h2', el) && $('h2', el).textContent.trim() === title ? el : null;
    });
    const b = await waitFor(`the button "${label}"`, () => button(label, d));
    b.click();
    await waitFor(`the dialog "${title}" to go`, () => openDialog() !== d || !d.open);
  }
  async function go(page, tab = '') {
    T().go(page, tab);
    await sleep(400);
  }
  async function shot(name) {
    const overlay = $$('dialog[open]').find((d) => /Synchronizing/.test(d.textContent));
    if (overlay) {
      const hide = button('Hide', overlay);
      if (hide) hide.click();
    }
    await sleep(700);
    await snapshot(name);
  }

  try {
    // What pays or signs is confirmed in the dialog of the host (audit 7 #1): every question accepted here, checked at the end.
    await host('test.confirmAnswer', { always: true });
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await check('Advanced on a first start (user decision), Simple once chosen', window.__E2E_FIRST_MODE === 'advanced' && T().prefs.uiMode === 'simple', `${window.__E2E_FIRST_MODE} ${T().prefs.uiMode}`);
    await waitFor('the node to reach the mainchain', async () => (await rpc('getmainchaininfo')).connected, 60000);
    await check('the voting center, decisions, create, votecoin and payouts out of the sidebar', !/Voting center|Decisions|Votecoin|Payouts|Create/.test(text('nav.sidebar .scroll')) && /Questions/.test(text('nav.sidebar .scroll')) && /My bets/.test(text('nav.sidebar .scroll')), text('nav.sidebar .scroll'));

    // Coins.
    await rpc('createwallet', ['hm']);
    await waitFor('the wallet in the page', () => T().wallets.current === 'hm');
    const deposit = await rpc('getdepositaddress', [], 'hm');
    await host('test.confirmAnswer', { yes: true });
    await host('peg.deposit', { destination: deposit.depositaddress, amount: '20' });
    await rpc('setbmm', [true, await rpc('getnewaddress', [], 'hm'), true]);
    await waitFor('the deposit credited', async () => (await rpc('getbalances', [], 'hm')).mine.trusted >= 20, 240000);

    // Two questions, opened through RPC (Create is Advanced's).
    const period = (await rpc('gethivemindinfo')).period;
    const rain = await rpc('createdecision', ['Will it rain in Paris on 1 May?', period + 4, 1], 'hm');
    const brazil = await rpc('createdecision', ['Who wins the cup? — Brazil', period + 4, 1], 'hm');
    const france = await rpc('createdecision', ['Who wins the cup? — France', period + 4, 1], 'hm');
    await waitFor('the decisions listed', async () => (await rpc('listdecisions')).length === 3, 90000);
    await rpc('createmarket', ['[Weather] Rain in Paris on 1 May', [rain.id], '1.00000000', 50], 'hm');
    await rpc('createmarket', ['[Sport] The cup', [brazil.id, france.id], '1.00000000', 50], 'hm');
    const markets = await waitFor('the markets open', async () => {
      const list = await rpc('listmarkets');
      return list.length === 2 ? list : null;
    }, 90000);
    const rainMarket = markets.find((m) => m.decisions[0] === rain.id);
    const cupMarket = markets.find((m) => m.decisions.length === 2);

    T().savePrefs({ layout: 'dashboard', tipsDone: true });
    for (const theme of ['midnight', 'light']) {
      T().savePrefs({ theme });
      await go('markets');
      await waitFor('the question cards', () => text('ul[aria-label="Questions"]').includes('Will it rain in Paris on 1 May?') && text('ul[aria-label="Questions"]').includes('Who wins the cup?'), 30000);
      await shot(`hm-bets-questions-${theme}`);

      await go('markets', `m-${rainMarket.id}`);
      const yes = await waitFor('Yes', () => $$('.simple-market [role="radio"]').find((b) => b.textContent.trim() === 'Yes'));
      await check(`${theme}: Yes and No`, $$('.simple-market [role="radio"]').map((b) => b.textContent.trim()).join() === 'Yes,No');
      yes.click();
      set($('#bet-amount'), '1');
      const outcome = await waitFor('the outcome in words', () => text('[data-testid="bet-outcome"]'));
      await check(`${theme}: the outcome in words`, /If “Yes” happens you get ≈ [\d.]+ \S+; if not, you lose 1 \S+\./.test(outcome), outcome);
      await shot(`hm-bets-question-${theme}`);
      if (theme === 'midnight') {
        $('#bet-place').click();
        await answerDialog('Place a bet', 'Bet');
        const pos = await waitFor('the bet held', async () => (await rpc('getwalletpositions', [], 'hm')).find((p) => p.market === rainMarket.id && p.state === 1), 90000);
        await check('the bet bought shares of Yes', pos.shares > 1, JSON.stringify(pos));
      }
      await waitFor('the bet on the question', () => $('section[aria-label="Your bets on this question"]'), 60000);
      await shot(`hm-bets-bet-placed-${theme}`);

      await go('markets', `m-${cupMarket.id}`);
      await waitFor('the options', () => $$('.simple-market [role="radio"]').length === 2);
      await shot(`hm-bets-cup-${theme}`);

      await go('portfolio');
      await waitFor('My bets', () => text('ul[aria-label="My bets"]').includes('Will it rain in Paris on 1 May?'), 30000);
      await shot(`hm-bets-mybets-${theme}`);

      await go('home');
      await waitFor('the cards of Hivemind', () => text('.home').includes('My bets') && text('.home').includes('Closing soon'), 30000);
      await check(`${theme}: no votes nor votecoin on the home`, !text('.home').includes('Votes due') && !text('.home').includes('Votecoin'));
      await shot(`hm-bets-home-${theme}`);
    }

    // Cash out, from My bets.
    await go('portfolio');
    const cash = await waitFor('Cash out', () => button('Cash out now', $('ul[aria-label="My bets"]')));
    cash.click();
    await answerDialog('Cash out', 'Cash out');
    await waitFor('the bet sold', async () => !(await rpc('getwalletpositions', [], 'hm')).some((p) => p.market === rainMarket.id && p.shares > 0), 90000);
    await check('cashed out: no shares left', true);
    await waitFor('My bets empty', () => text('.simple-bets').includes('No bet yet'), 30000);
    await shot('hm-bets-cashed-out-light');
    {
      // The confirmations of the desktop: what the chain's pages paid or signed, each with its facts.
      const titles = ["Buy shares", "Create a decision", "Create a market", "Reveal a ballot", "Reveal a vote", "Seal a ballot", "Seal a vote", "Sell shares", "Send votecoin"];
      const asked = (await host('test.confirmed', {})).filter((q) => titles.includes(q.title));
      await check(`the desktop confirmed what the pages paid or signed (${asked.length})`, asked.length > 0 && asked.every((q) => q.rows.length > 1), JSON.stringify(asked.map((q) => q.title)));
    }
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
