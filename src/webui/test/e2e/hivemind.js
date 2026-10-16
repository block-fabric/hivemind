// Hivemind against a live mainchain (run_sidechain.py --script hivemind.js): the markets driven through
// the pages as a user would. The wallet gets the reputation of the regtest chain (the key of
// feature_hivemind.py, imported in the console) and coins (a deposit by the helper of the mainchain
// wallet, merged mining on). Then, with periods of 45 seconds of mainchain time: the Create wizard lists
// a decision and opens a market on it; a purchase of Yes in the trade panel, a sale of part of it; the
// voting center seals Yes, then reveals it in the reveal phase; the decision resolved, the market pays
// out, and the payout shows. Pictures of every page of Hivemind on the way, in the layouts and looks.
(async () => {
  const { host, rpc, sleep, log, T, waitFor, check, snapshot, text, finish } = E2E;
  const PERIOD = window.__E2E_PERIOD;
  const GENESIS_VOTECOIN_KEY = 'cPfsxwfwtqceBD1MsHSt2q9r6bquHNTixtVGUKubYmYySGcSEynR';
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  const openDialog = () => $$('dialog[open]').pop();
  const button = (label, root = document) => $$('button', root).find((b) => b.textContent.trim().replace(/\s+/g, ' ').startsWith(label));
  async function click(label, root) {
    const b = await waitFor(`the button "${label}"`, () => {
      const el = button(label, typeof root === 'function' ? root() : root);
      return el && !el.disabled ? el : null;
    });
    b.click();
    await sleep(120);
  }
  async function answerDialog(title, label) {
    const d = await waitFor(`the dialog "${title}"`, () => {
      const el = openDialog();
      return el && $('h2', el) && $('h2', el).textContent.trim() === title ? el : null;
    });
    await click(label, d);
    await waitFor(`the dialog "${title}" to go`, () => openDialog() !== d || !d.open);
  }
  async function go(page, tab = '') {
    T().go(page, tab);
    await sleep(300);
  }
  async function shot(name) {
    const overlay = $$('dialog[open]').find((d) => /Synchronizing/.test(d.textContent));
    if (overlay) {
      const hide = button('Hide', overlay);
      if (hide) hide.click();
      await sleep(300);
    }
    await snapshot(name);
  }
  const now = () => Date.now() / 1000;
  /** Seconds left in the period now, and before its reveal phase (periods of mainchain time from 0). */
  const clock = () => {
    const t = now();
    const n = Math.floor(t / PERIOD);
    return { period: n, left: (n + 1) * PERIOD - t, revealIn: n * PERIOD + (PERIOD * 2) / 3 - t };
  };
  async function until(what, fn, ms = 300000) {
    return waitFor(what, fn, ms);
  }

  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await waitFor('the node to reach the mainchain', async () => (await rpc('getmainchaininfo')).connected, 60000);
    await check('Hivemind, the dashboard and Midnight by default', T().app.info.name === 'Hivemind' && T().prefs.layout === 'dashboard' && (T().prefs.theme ?? 'midnight') === 'midnight', `${T().app.info.name} ${T().prefs.layout}`);

    // --- A wallet with the reputation of the chain, and coins ---
    await rpc('createwallet', ['hm']);
    await waitFor('the wallet in the page', () => T().wallets.current === 'hm');
    const desc = await rpc('getdescriptorinfo', [`wpkh(${GENESIS_VOTECOIN_KEY})`]);
    // A command of the console that does more than read waits for the desktop (audit port-1 #3): accepted here.
    await host('test.confirmAnswer', { yes: true });
    const imported = await host('console.run', { line: `importdescriptors '[{"desc": "wpkh(${GENESIS_VOTECOIN_KEY})#${desc.checksum}", "timestamp": "now"}]'`, wallet: 'hm' });
    await check('the key of the reputation imported in the console', imported.ok && /"success": true/.test(imported.result), JSON.stringify(imported));
    const deposit = await rpc('getdepositaddress', [], 'hm');
    // The wallet of the mainchain node pays: on the desktop's word (audit port-1 #4), accepted here.
    await host('test.confirmAnswer', { yes: true });
    await host('peg.deposit', { destination: deposit.depositaddress, amount: '20' });
    await rpc('setbmm', [true, await rpc('getnewaddress', [], 'hm'), true]);
    await waitFor('the deposit credited', async () => (await rpc('getbalances', [], 'hm')).mine.trusted >= 20, 240000);
    await check('coins and votecoin in the wallet', (await rpc('getwalletreputation', [], 'hm')).length === 1);

    await go('home');
    await waitFor('the cards of Hivemind on the home', () => text('.home').includes('Votes due') && text('.home').includes('Votecoin'), 30000);
    await sleep(1500);
    await shot('hm-home-empty');
    await go('markets');
    await sleep(1200);
    await shot('hm-markets-empty');

    // --- Create: a decision whose answer is known in the next period, and a market on it ---
    // From the start of a period: the decision, then the market, are mined while it lasts.
    await until('the start of a period', () => clock().left > PERIOD - 12, PERIOD * 2000);
    const before = (await rpc('gethivemindinfo')).period;
    await go('create', 'market');
    await click('Yes or no');
    $('#create-next').click();
    await sleep(200);
    set($('#create-question'), 'Will the end-to-end test pass?');
    await sleep(200);
    $('#create-next').click();
    await sleep(200);
    set($('#create-event'), '1');
    set($('#create-votes'), '1');
    await sleep(300);
    await shot('hm-create-timing');
    $('#create-next').click();
    await sleep(200);
    set($('select[aria-label="Category"]'), 'Technology');
    set($('#create-liquidity'), '1');
    await sleep(200);
    $('#create-next').click();
    await sleep(400);
    await shot('hm-create-review');
    $('#create-submit').click();
    await answerDialog('Open the market', 'Open');
    await waitFor('the decision listed', async () => (await rpc('listdecisions')).length === 1, 60000);
    const market = await waitFor('the market opened', async () => (await rpc('listmarkets'))[0], 90000);
    const decision = (await rpc('listdecisions'))[0];
    await check('the wizard listed the decision, then opened the market on it', market.decisions[0] === decision.id && market.text === '[Technology] Will the end-to-end test pass?' && decision.eventperiod === before + 1, JSON.stringify({ market, decision }));
    await shot('hm-create-done');

    // --- Trade: buy Yes, then sell part of it ---
    await go('markets', `m-${market.id}`);
    await waitFor('the trade panel', () => $('#trade-amount'));
    await click('Yes', () => $('section[aria-label="Trade"]'));
    set($('#trade-amount'), '0.5');
    await waitFor('the quote of the node', () => text('section[aria-label="Trade"]').includes('Quoted by the node just now.'), 20000);
    await shot('hm-market-buy-preview');
    $('#trade-place').click();
    await answerDialog('Buy shares', 'Buy');
    const position = await waitFor('the shares held', async () => (await rpc('getwalletpositions', [], 'hm')).find((p) => p.market === market.id && p.state === 1), 60000);
    await check('bought shares of Yes', position.shares > 0.5, JSON.stringify(position));
    await waitFor('the chance of Yes up', async () => (await rpc('getmarket', [market.id])).states[1].price > 0.5, 30000);
    await sleep(1500);
    await shot('hm-market-after-buy');
    // A second purchase of Yes goes to a new account of the wallet (buyshares): Your positions keeps one row
    // for Yes, the shares of both added up.
    set($('#trade-amount'), '0.3');
    await waitFor('the quote of the second purchase', () => text('section[aria-label="Trade"]').includes('Quoted by the node just now.'), 20000);
    $('#trade-place').click();
    await answerDialog('Buy shares', 'Buy');
    const yesHeld = async () => (await rpc('getwalletpositions', [], 'hm')).filter((p) => p.market === market.id && p.state === 1);
    const both = await waitFor('the second purchase held', async () => ((await yesHeld()).length >= 2 ? await yesHeld() : null), 60000);
    const total = both.reduce((n, p) => n + p.shares, 0);
    const yesRows = () => $$('table[aria-label="Your positions in this market"] tbody tr');
    await waitFor('one row for Yes', () => yesRows().length === 1 && yesRows()[0].textContent.includes(`in ${both.length} accounts`), 30000);
    const shown = Number(yesRows()[0].querySelectorAll('td')[1].textContent.replace(/[^\d.]/g, ''));
    await check('Your positions: one row for Yes, the shares of both purchases added up, an average price', yesRows().length === 1 && Math.abs(shown - total) < 1e-6 && /\d/.test(yesRows()[0].querySelectorAll('td')[2].textContent), JSON.stringify({ both, shown, row: yesRows()[0].textContent }));
    await sleep(800);
    await shot('hm-market-positions');
    const toSell = (Math.floor(position.shares * 0.25 * 1e8) / 1e8).toFixed(8);
    $$('section[aria-label="Trade"] [role="tab"]').find((b) => b.textContent.trim() === 'Sell').click();
    await sleep(200);
    set($('#trade-amount'), toSell);
    await waitFor('the quote of the sale', () => text('section[aria-label="Trade"]').includes('Quoted by the node just now.'), 20000);
    $('#trade-place').click();
    await answerDialog('Sell shares', 'Sell');
    await waitFor('the shares sold', async () => (await yesHeld()).reduce((n, p) => n + p.shares, 0) < total - 1e-9, 60000);
    await check('sold part of the shares', true);
    await go('portfolio');
    await sleep(1200);
    await shot('hm-portfolio');
    await go('markets');
    await sleep(1200);
    await shot('hm-markets');

    // --- Vote: seal Yes when the decision is up for the vote, reveal it in the reveal phase ---
    await until('the decision up for the vote', async () => (await rpc('listdecisions'))[0].status === 'voting' && clock().revealIn > 12, PERIOD * 4000);
    await check('trading closed when the vote started', (await rpc('getmarket', [market.id])).status === 'closed');
    await go('voting');
    const card = await waitFor('the decision in the voting center', () => $$('article[aria-label="A decision to vote on"]')[0], 30000);
    await sleep(500);
    await shot('hm-voting-seal');
    $$('[role="radio"]', card).find((b) => b.textContent.trim() === 'Yes').click();
    await sleep(150);
    $(`#seal-${decision.id.slice(0, 8)}`).click();
    await waitFor('the vote sealed on the chain', async () => (await rpc('getdecision', [decision.id])).votes.length === 1, 60000);
    await waitFor('the vote shown sealed', () => text('.voting').includes('Sealed: Yes'), 30000);
    const stored = await host('hivemind.store.load');
    const kept = Object.values((stored.seals && stored.seals.sealed) || {}).find((r) => r.decision === decision.id);
    await check('the vote sealed, its answer kept by the app in a file of its own (audit port-1 #1)', !!kept && kept.answer === 'yes', JSON.stringify(stored.seals));
    await sleep(1000);
    await shot('hm-voting-sealed');
    await until('the reveal phase', () => clock().revealIn < -1 && clock().left > 3, PERIOD * 3000);
    await waitFor('the vote to reveal', () => $(`#reveal-${decision.id.slice(0, 8)}`), 30000);
    await shot('hm-voting-reveal');
    $(`#reveal-${decision.id.slice(0, 8)}`).click();
    await waitFor('the vote revealed on the chain', async () => (await rpc('getdecision', [decision.id])).votes.some((v) => v.revealed), 60000);
    await check('the vote revealed with the answer sealed', (await rpc('getdecision', [decision.id])).votes[0].answer === 1);

    // --- Resolved, settled, paid ---
    const resolved = await until('the decision resolved', async () => {
      const d = (await rpc('listdecisions'))[0];
      return d.status === 'resolved' ? d : null;
    }, PERIOD * 3000);
    await check('the decision resolved: yes', resolved.answer === 'yes', JSON.stringify(resolved));
    await waitFor('the market paid out', async () => (await rpc('getmarket', [market.id])).status === 'settled', 60000);
    await go('payouts');
    await waitFor('the payout of the shares in the wallet', () => $$('table[aria-label="Payouts received"] tbody tr').some((r) => r.textContent.includes('Shares')), 120000);
    await check('the payout shows', true);
    await sleep(800);
    await shot('hm-payouts');
    await go('voting');
    await waitFor('the vote with the others', () => text('.voting').includes('With the others'), 30000);
    await check('the vote fared with the outcome', true);
    await shot('hm-voting-past');

    // --- Votecoin: move some to a new account of the wallet ---
    await go('votecoin');
    await click('A new account of this wallet');
    await waitFor('the account', () => $('#votecoin-to') && $('#votecoin-to').value);
    set($('#votecoin-amount'), '10');
    await waitFor('the transfer ready', () => !$('#votecoin-send').disabled, 10000);
    $('#votecoin-send').click();
    await answerDialog('Move votecoin', 'Move');
    await waitFor('the votecoin moved', async () => (await rpc('getwalletreputation', [], 'hm')).length === 2, 90000);
    await check('votecoin moved to another account of the wallet', true);
    await sleep(1000);
    await shot('hm-votecoin');

    // --- Every page of Hivemind, in the layouts and looks ---
    await go('decisions');
    await sleep(800);
    await shot('hm-decisions');
    await go('decisions', 'calendar');
    await sleep(800);
    await shot('hm-calendar');
    await go('markets', `m-${market.id}`);
    await sleep(1200);
    await shot('hm-market-settled');
    await go('home');
    await sleep(1500);
    await shot('hm-home-midnight');
    T().savePrefs({ theme: 'light' });
    for (const page of ['home', 'markets', 'voting', 'votecoin']) {
      await go(page);
      await sleep(900);
      await shot(`hm-${page}-light`);
    }
    T().savePrefs({ theme: 'midnight', layout: 'workspace' });
    await go('markets');
    await sleep(900);
    await shot('hm-workspace-markets');
    // Home in every layout (the Compact layout is gone): the workspace's Home area.
    await go('home');
    await sleep(1200);
    await shot('hm-workspace-home');
    T().savePrefs({ layout: 'dashboard', theme: 'ocean' });
    await go('create');
    await sleep(900);
    await shot('hm-create-ocean');
    T().savePrefs({ theme: 'midnight' });

    await rpc('setbmm', [false]);
    await log(`MARKET ${market.id}`);
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
