// What the Create wizard sends, and what waits for a block: a market opens on decisions that are in the
// chain, so one on new decisions lists them first, then opens once they are mined. The jobs that wait are
// kept in the journal of Hivemind (store.svelte.ts, `pending`), so that a restart of the app resumes them:
// they were in memory only, and a market whose decisions were listed was never opened when the app closed
// before the next block (audit 7 E-HM1). The wizard and Decisions show the jobs under way.
import { t } from '../../lib/i18n.svelte';
import { errorText, walletRpc } from '../../lib/dialogs.svelte';
import { toast } from '../../lib/state.svelte';
import type { DecisionDraft } from './model';
import { hm, journalReady, noteMine, notePending, onRefresh, refreshHivemind, type PendingCreation } from './store.svelte';

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
  /** How many decisions the market is about (those listed already and the new ones). */
  expected: number;
  /** Resumed from the journal at a start of the app. */
  resumed?: boolean;
}

export const creation = $state({ jobs: [] as CreationJob[] });
let nextId = 1;
/** Whether the jobs the journal keeps were taken up (once, after the journal is read). */
let resumed = false;

export interface CreationRequest {
  wallet: string;
  drafts: DecisionDraft[];
  /** Decisions listed already. */
  existing: string[];
  market: { text: string; liquidity: number; feebps: number } | null;
}

/** What the journal keeps of a job waiting for a block. */
function pendingOf(job: CreationJob): PendingCreation | null {
  if (!job.market || !job.txids.length) return null;
  return { key: job.txids[0], wallet: job.wallet, title: job.title, decisions: [...job.decisions], expected: job.expected, market: { ...job.market }, txids: [...job.txids], time: Math.floor(Date.now() / 1000) };
}

/** List the new decisions; open the market at once on listed ones, or once the new ones are mined. */
export async function startCreation(req: CreationRequest): Promise<CreationJob> {
  const job: CreationJob = {
    id: nextId++,
    wallet: req.wallet,
    title: req.market?.text ?? req.drafts[0]?.text ?? '',
    state: 'sending',
    decisions: [...req.existing],
    expected: req.existing.length + req.drafts.length,
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
      // Kept from the first decision on: a market whose decisions are partly listed is still opened after a restart.
      if (req.market) {
        const rec = pendingOf(live());
        if (rec && !(await notePending(rec.key, rec))) {
          toast('warning', t('The market waiting to open could not be saved'), t('Keep the app open until the decisions are in a block: the market opens then.'), 15000);
        }
      }
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
    if (live().txids.length) void notePending(live().txids[0], null);
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
  const key = job.txids[0];
  job.txids.push(done.txid);
  job.marketId = done.id;
  job.state = 'done';
  noteMine(job.wallet, done.id);
  if (key) await notePending(key, null);
}

/** A market on exactly these decisions, with this title, on the chain already (opened before the app closed). */
function openedAlready(job: CreationJob): string | null {
  if (!job.market) return null;
  const want = [...job.decisions].sort().join(',');
  const m = hm.markets.find((x) => x.text === job.market!.text && [...x.decisions].sort().join(',') === want);
  return m ? m.id : null;
}

/** The jobs the journal keeps, taken up once at a start of the app. */
function resumeCreation(): void {
  if (resumed || !journalReady()) return;
  resumed = true;
  const known = new Set(creation.jobs.map((j) => j.txids[0]).filter(Boolean));
  const back: CreationJob[] = hm.journal.pending
    .filter((p) => !known.has(p.key))
    .map((p) => ({ id: nextId++, wallet: p.wallet, title: p.title, state: 'waiting', decisions: [...p.decisions], expected: p.expected, market: { ...p.market }, txids: [...p.txids], resumed: true }));
  // The app closed in the middle of listing the decisions: a market on a part of them is not what was asked.
  for (const job of back) {
    if (job.decisions.length >= job.expected) continue;
    job.state = 'failed';
    job.error = t('The app closed before all the decisions of the market were listed (%1 of %2). Open the market in Create on the decisions listed.', String(job.decisions.length), String(job.expected));
    void notePending(job.txids[0], null);
  }
  if (!back.length) return;
  creation.jobs = [...creation.jobs, ...back].slice(0, 12);
  const waiting = back.filter((j) => j.state === 'waiting');
  if (!waiting.length) return;
  toast('info', t('A market waits to open'), waiting.length === 1 ? t('“%1” opens once its decisions are in a block. Keep the app open until then.', waiting[0].title) : t('%1 markets open once their decisions are in a block. Keep the app open until then.', String(waiting.length)), 10000);
}

/** Forget a market waiting to open (its decisions may never be mined): it is not opened. */
export async function forgetCreation(id: number): Promise<void> {
  const job = creation.jobs.find((j) => j.id === id);
  if (!job || job.state !== 'waiting') return;
  creation.jobs = creation.jobs.filter((j) => j.id !== id);
  if (job.txids[0]) await notePending(job.txids[0], null);
}

/** With every refresh: open the markets whose decisions are now in the chain. */
export async function advanceCreation(): Promise<void> {
  resumeCreation();
  const listed = new Set(hm.decisions.map((d) => d.id));
  for (const job of creation.jobs) {
    if (job.state !== 'waiting' || !job.decisions.every((d) => listed.has(d))) continue;
    const already = openedAlready(job);
    if (already) {
      job.marketId = already;
      job.state = 'done';
      if (job.txids[0]) void notePending(job.txids[0], null);
      continue;
    }
    try {
      await openMarket(job);
      toast('success', t('Market opened'), t('“%1” trades once its transaction is in a block.', job.title));
    } catch (e) {
      job.state = 'failed';
      job.error = errorText(e);
      if (job.txids[0]) void notePending(job.txids[0], null);
      toast('error', t('The market was not opened'), errorText(e));
    }
  }
}

/** The jobs waiting for a block (shown in Create and Decisions). */
export function waitingJobs(): CreationJob[] {
  return creation.jobs.filter((j) => j.state === 'waiting');
}

/** For the tests: no job, and the journal's taken up again with the next refresh (as at a start of the app). */
export function resetCreation(): void {
  creation.jobs = [];
  resumed = false;
}

onRefresh(advanceCreation);
