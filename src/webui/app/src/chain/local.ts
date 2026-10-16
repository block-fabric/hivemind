// The pages of this chain of its own, beside those of the kit: the one file of the interface (with
// LocalPages.svelte, which shows them, and chain.json) that a chain made from the template changes, so
// that merges of the template stay clean. See doc/webui.md, "Pages of a chain of its own".
//
// Hivemind: the markets, the portfolio of the wallet, the voting center, the decisions and the calendar
// of the periods, the wizard that creates them, votecoin and the payouts; their components in
// LocalPages.svelte (chain/pages, chain/hm), their RPC methods in src/webui/local.cpp, the cards of the
// home in LocalCards.svelte, their icons in icons.ts.
import type { IconName } from '../components/Icon.svelte';

export interface LocalPage {
  /** Stable: the page id (go('names')), the command page.<id>. */
  id: string;
  title: string;
  icon: IconName;
  /** What the page is for, in a line (tooltips, the palette). */
  summary: string;
  /**
   * The section of the sidebars and the area of the workspace it goes in. What concerns the chain comes
   * before the tools, whatever the section (the chainBeforeTools tests).
   */
  section: 'wallet' | 'chain' | 'tools';
  /** The page needs a loaded wallet. */
  wallet?: boolean;
  shortcut?: string;
  /** More words to find it by in the palette. */
  keywords?: string;
  /** A quick action of the dashboard's home that opens it (after Send and Receive, with the chain's). */
  quick?: { label: string; icon?: IconName; tab?: string };
  /** Only in the navigation of Advanced mode (lib/uimode.svelte.ts): Simple mode reaches it by the palette and links. */
  advanced?: boolean;
}

export const LOCAL_PAGES = [
  { id: 'markets', title: 'Markets', icon: 'market', section: 'chain', summary: 'Browse the prediction markets and trade in them', keywords: 'hivemind predict trade buy sell shares odds', quick: { label: 'Trade', icon: 'market' } },
  { id: 'portfolio', title: 'Portfolio', icon: 'portfolio', section: 'chain', summary: 'Your shares, what they are worth and what they made', wallet: true, keywords: 'positions profit loss pnl shares' },
  { id: 'voting', title: 'Voting center', icon: 'ballot', section: 'chain', summary: 'Seal and reveal your votes on the decisions', wallet: true, keywords: 'vote seal reveal ballot reputation', quick: { label: 'Vote', icon: 'ballot' }, advanced: true },
  { id: 'decisions', title: 'Decisions', icon: 'decision', section: 'chain', summary: 'The questions the markets are about, and the calendar of the periods', keywords: 'questions periods calendar', advanced: true },
  { id: 'create', title: 'Create', icon: 'create', section: 'chain', summary: 'List a decision and open a market on it', wallet: true, keywords: 'new market decision list open wizard', quick: { label: 'Create a market', icon: 'create', tab: 'market' }, advanced: true },
  { id: 'votecoin', title: 'Votecoin', icon: 'votecoin', section: 'chain', summary: 'The reputation that votes: balance, history, transfers', wallet: true, keywords: 'reputation votecoin transfer holders', advanced: true },
  { id: 'payouts', title: 'Payouts', icon: 'payout', section: 'chain', summary: 'What the markets paid: settlements, fees of authors and voters', wallet: true, keywords: 'settlement payout settled paid', advanced: true },
] as const satisfies readonly LocalPage[];

export type LocalPageId = (typeof LOCAL_PAGES)[number]['id'];

/** Items of the menus (the Menus button and the palette) of this chain: in the group 'Chains' (before the tools). */
export interface LocalCommand {
  id: string;
  label: string;
  page: LocalPageId;
  tab?: string;
  tip?: string;
  keywords?: string;
}

export const LOCAL_COMMANDS = [
  { id: 'hm.create-market', label: 'Create a market…', page: 'create', tab: 'market', tip: 'List the decisions of a market and open it', keywords: 'new market' },
  { id: 'hm.list-decision', label: 'List a decision…', page: 'create', tab: 'decision', tip: 'List a question for the markets and the voters', keywords: 'new decision question' },
  { id: 'hm.periods', label: 'Periods calendar', page: 'decisions', tab: 'calendar', tip: 'The period now, its phase, and the periods ahead', keywords: 'period phase seal reveal' },
  { id: 'hm.transfer', label: 'Move votecoin…', page: 'votecoin', tip: 'Move reputation to another account', keywords: 'transfer reputation' },
] as const satisfies readonly LocalCommand[];
