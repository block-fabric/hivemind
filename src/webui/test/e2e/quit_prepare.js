// Before run_quit.py's runs (local.cmake, webui_e2e_quit_hivemind): a market of the Create wizard waits to open in the
// journal of Hivemind (audit 7 E-HM1), as when the app closed before its decision was mined. The next starts resume it
// and keep it waiting (its decision is not in the chain): the page asks the host to confirm a quit.
(async () => {
  const { host, T, waitFor, finish } = E2E;
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready', 300000);
    const key = 'ab'.repeat(32);
    await host('hivemind.store.journal', {
      value: { pending: [{ key, wallet: 'w', title: '[Weather] Snow in Paris', decisions: ['cd'.repeat(32)], expected: 1, market: { text: '[Weather] Snow in Paris', liquidity: 1, feebps: 50 }, txids: [key], time: 1 }] },
    });
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
