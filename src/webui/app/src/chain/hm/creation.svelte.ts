// What the Create wizard sends, and what waits for a block: a market opens on decisions that are in the
// chain, so one on new decisions lists them first, then opens once they are mined. Kept while the app
// runs (the wizard shows the jobs under way, wherever the user went meanwhile).
import { t } from '../../lib/i18n.svelte';
import { errorText, walletRpc } from '../../lib/dialogs.svelte';
import { toast } from '../../lib/state.svelte';
import type { DecisionDraft } from './model';
import { hm, noteMine, onRefresh, refreshHivemind } from './store.svelte';

export interface CreationJob {
  id: number;
  wallet: string;
  /** The title of the market, or the question of the decision. Text of the user. */
  title: string;
  state: 'sending' | 'waiting' | 'done' | 'failed';
  /** The decisions the market is about, as they will be once mined. */
  decisions: string[];
  market: { text: string; liquidity: number; feebps: number } | null;
  marketId?: string;
  txids: string[];
  error?: string;
}

export const creation = $state({ jobs: [] as CreationJob[] });
let nextId = 1;

export interface CreationRequest {
  wallet: string;
  drafts: DecisionDraft[];
  /** Decisions listed already. */
  existing: string[];
  market: { text: string; liquidity: number; feebps: number } | null;
}

/** List the new decisions; open the market at once on listed ones, or once the new ones are mined. */
export async function startCreation(req: CreationRequest): Promise<CreationJob> {
  const job: CreationJob = {
    id: nextId++,
    wallet: req.wallet,
    title: req.market?.text ?? req.drafts[0]?.text ?? '',
    state: 'sending',
    decisions: [...req.existing],
    market: req.market,
    txids: [],
  };
  creation.jobs = [job, ...creation.jobs].slice(0, 12);
  const live = () => creation.jobs.find((j) => j.id === job.id)!;
  try {
    for (const d of req.drafts) {
      const params: unknown[] = [d.text, d.eventperiod, d.voteperiods];
      if (d.scaled) params.push(true, d.min, d.max);
      const done = await walletRpc<{ txid: string; id: string }>(req.wallet, 'createdecision', params);
      if (!done) throw new Error(t('Cancelled.'));
      live().decisions.push(done.id);
      live().txids.push(done.txid);
      noteMine(req.wallet, done.id);
    }
    if (!req.market) {
      live().state = 'done';
    } else if (req.drafts.length === 0) {
      await openMarket(live());
    } else {
      live().state = 'waiting';
    }
  } catch (e) {
    live().state = 'failed';
    live().error = errorText(e);
    throw e;
  } finally {
    void refreshHivemind();
  }
  return live();
}

async function openMarket(job: CreationJob): Promise<void> {
  if (!job.market) return;
  job.state = 'sending';
  const done = await walletRpc<{ txid: string; id: string; funding: number; fee: number }>(job.wallet, 'createmarket', [job.market.text, job.decisions, job.market.liquidity.toFixed(8), job.market.feebps]);
  if (!done) throw new Error(t('Cancelled.'));
  job.txids.push(done.txid);
  job.marketId = done.id;
  job.state = 'done';
  noteMine(job.wallet, done.id);
}

/** With every refresh: open the markets whose decisions are now in the chain. */
export async function advanceCreation(): Promise<void> {
  const listed = new Set(hm.decisions.map((d) => d.id));
  for (const job of creation.jobs) {
    if (job.state !== 'waiting' || !job.decisions.every((d) => listed.has(d))) continue;
    try {
      await openMarket(job);
      toast('success', t('Market opened'), t('“%1” trades once its transaction is in a block.', job.title));
    } catch (e) {
      job.state = 'failed';
      job.error = errorText(e);
      toast('error', t('The market was not opened'), errorText(e));
    }
  }
}

onRefresh(advanceCreation);
