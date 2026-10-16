// The three first steps of Hivemind, shown on Home in Simple mode until dismissed
// (components/FirstRunTips.svelte; lib/firststeps.ts). English here: the card translates them.
import type { FirstStep } from '../lib/firststeps';
import { CHAIN } from './config';

const main = CHAIN.mainchain?.name ?? 'the mainchain';

export const FIRST_STEPS: readonly FirstStep[] = [
  { title: 'Bring coins here', text: `Bets are paid in coins moved from ${main}. Drivechains gives a deposit address: paste it in the ${main} app (Sidechains, Move coins). They arrive a block or two later.`, icon: 'down', action: { label: 'Get a deposit address', page: 'peg', tab: 'deposit' } },
  { title: 'Pick a question and bet', text: 'Questions shows what people bet on, with the chance the bets give each answer. Choose Yes or No and an amount: you see what you get if you are right.', icon: 'market', action: { label: 'See the questions', page: 'markets' } },
  { title: 'Cash out, or wait for the answer', text: 'My bets shows what each bet is worth now. Cash out any time while the question is open, or keep it: a right bet pays once the answer is known.', icon: 'portfolio', action: { label: 'My bets', page: 'portfolio' } },
];
