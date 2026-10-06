# Hivemind: prediction markets

Hivemind is the [sidechain template](sidechain.md) with prediction markets, after
Paul Sztorc's Truthcoin paper. The markets are rules of the chain: every node
checks every trade, every vote and every payout.

## How it works

- A **decision** is a question whose answer will be known: yes or no, or a
  whole number on a scale. Anyone can list one, for a fee, naming the voting
  period in which its event falls and for how many periods it is voted on.
- A **market** is about one to four decisions and has two states per
  decision. Its author funds a market maker (LMSR) with the most it can lose,
  `liquidity * ln(states)`. The market maker sells and buys shares of the
  states at prices that follow demand. A share of a state pays one coin if the
  state comes true. Trading stops when the vote on the first of its decisions
  starts.
- **Reputation** (votecoin) is a fixed supply of 100, held by accounts. An
  account is a key hash, shown as its P2WPKH address. Reputation can be moved.
- After the event, holders of reputation **vote**. A vote is sealed first
  (a commitment), and revealed in the last third of the last period of the
  vote. A vote that is not revealed, or that does not open its seal, does not
  count. A holder can also answer that the outcome is *not known yet*.
- The first block of the next period **resolves** all decisions whose vote
  ended. Voters are scored against each other across the whole batch: the
  votes form a matrix, its first principal component is the line along which
  voters differ most, and those at the end the outcomes do not follow lose a
  share of their reputation to the others. Who skipped votes loses to those
  who cast them. Outcomes are the reputation-weighted mean (yes or no, with a
  tie band around a half) or median (scales) of the votes.
- If those who answer *not known yet* outweigh those who give an answer, or
  less than a fifth of the reputation answers (the quorum), the decision is
  voted again for as long as before; after too many times it is settled as a
  tie. In the last round, *not known yet* counts for nothing, and the decision
  is settled on the answers given.
- When all decisions of a market are resolved, it **pays**: each share gets
  the product, over the decisions, of how far each went its way. What the
  market maker has left goes back to the author of the market.
- **Fees**: listing fees, and the fees for opening markets, go to those who
  vote on the decisions they were paid for. Trading fees go half to the
  voters, a quarter to the authors of the decisions, a quarter to the author of
  the market. Voters are paid in proportion to the reputation they come out
  with.

## Parameters

| | Value | Where |
|---|---|---|
| Voting period | one day of mainchain time (6 blocks on regtest) | `period_seconds`, `period_blocks` |
| Reputation at stake per vote (alpha) | 1/10 | `alpha_num`, `alpha_den` |
| Tie band for yes or no | 0.5 ± 0.1 | `tolerance` |
| Vote length of a decision | 1 to 90 periods | `max_vote_periods` |
| Times a decision can be put off | 3 (1 on regtest) | `max_rolls` |
| Listing fee | 0.001 | `listing_fee` |
| Quorum | a fifth of the reputation must answer | `QUORUM_PERCENT` |
| Least purchase of shares | 0.0001 | `MIN_PURCHASE` |
| Kept of a refund, for the voters | 0.00001 | `REFUND_FEE` |
| Actions per transaction | 16 at most | `MAX_ACTIONS_PER_TX` |
| Decisions per market | 1 to 4 | `max_market_decisions` |
| Reputation supply | 100, to one account at the start | `reputation_supply`, `genesis_account` |

All in `Consensus::HivemindParams` (`src/consensus/params.h`), set per network
in `src/kernel/chainparams.cpp`. **The main network has no genesis account
yet**; one has to be set before launch, and whoever holds its key decides
every outcome until the reputation is spread.

## Design choices

- **No floating point.** The market maker and the vote are integer arithmetic
  on fixed point numbers (`src/hivemind/math.cpp`): exp and ln by series with
  60 fractional bits, the principal component by a fixed number of
  multiplications, every division with its rounding stated. Trades are rounded
  against the trader. On top of that, a trade is refused if it would leave the
  market holding less than its fullest state can pay, so a market can never
  owe more than it holds.
- **The direction of the vote** has two ends. The one taken as wrong is the
  one that changes the outcomes least; if both give the same outcomes, the
  side with less reputation; if the sides are even, nobody's reputation
  changes. The result does not depend on any sign convention.
- **Skipping votes is penalised by reputation, not by head**, so splitting
  reputation over many accounts gains nothing. (In the reference code of the
  paper it is by head.) **What the silent lose is capped** by what the voters
  themselves put at stake, and nothing is lost without a quorum: otherwise one
  small holder, alone in a batch with a decision of their own, would take a
  tenth of everyone else's reputation every period.
- **Shares and reputation are balances of accounts**, not coins. Coins paid to
  a market are burned by the transaction that pays them; what a market pays is
  created by the coinbase of the block in which it does, like deposits.
- **A transaction that is well formed is valid.** If what it asks cannot be
  done when its block is connected (the price moved, the market closed, the
  account lacks the shares), nothing happens and the coins burned for it are
  paid back, less 0.00001 for the voters: payouts take room in blocks, and
  refunds that cost nothing could be made by the thousand to hold back
  everyone else's. Sales, transfers and sealed votes are signed by their account;
  the signature names the first input of the transaction, so it cannot be
  replayed.
- **Reverting a block** restores the entries of the market state that the
  block changed, kept with its undo data.
- **Sealed votes name their voter**: the commitment covers the voter, so a
  sealed vote cannot be copied by someone who has not formed an opinion. The
  wallet makes a new salt for every round of voting on a decision.
- **Reputation does not move while votes are revealed** (the last third of a
  period): a transfer mined then is ignored. Otherwise a holder could put the
  reputation behind whichever revealed vote turned out best.
- **The phase never goes back** within a period: once a block is in the reveal
  phase, a mainchain time that steps back does not reopen sealing.
- **Only decisions that get an answer count.** Skipping a decision that is
  voted again later costs nothing, and saying that the answer to a decision
  that gets one cannot be known is no vote: it can never be wrong, so it earns
  nothing. Otherwise listing decisions nobody can answer would strip
  reputation from those who do not vote on them.
- **Bounded work, priced space.** At most 100 decisions close in a period (a
  listing beyond is refused and its fee given back; a decision voted again
  moves to the first period with room). Listing costs the listing fee doubled
  for every 10 decisions closing in the same period already, and opening a
  market costs, on top of its funding, the listing fee doubled for every 4
  unsettled markets on the one of its decisions that has the most (in any
  place); both fees go to the voters of those decisions.
  So filling a period, or crowding a decision with markets, costs more the
  further it goes, instead of being cheap and keeping everyone else out.
  Skipped periods are resolved one at a time; holders of less than a
  10 000th of the reputation, and those who did not vote, are scored together
  as one. Resolved decisions and settled markets leave the state 30 periods
  later. The arithmetic of the vote scales its numbers so that nothing can
  exceed 128 bits.
- **A stall does not use up a decision.** A decision whose votes could never
  be revealed -- its last period went by without a block of this chain in the
  reveal phase, or the clock reached it in its reveal phase -- is voted on in
  the first period that still has a time for sealing, rather than put off. A
  decision that is put off is voted on again from such a period too, not in
  periods the chain skipped.
- **The clock is the median time of the mainchain** (of a block and the ten
  before it), which no single miner can set ahead; a block's own time can be
  two hours ahead.
- **No seal in the reveal phase.** The wallet refuses to seal a vote on a
  decision whose votes are being revealed: put off, the decision would count
  it in the next round, with a salt the reveal has made public.

## Not in this version

- **Audits and the miner veto** of the paper: a disputed outcome stands.
- Several decisions on one axis of a market, and categorical decisions.
- A retargeted listing fee: the fee only rises with how full a period is.

## Commands

Node: `gethivemindinfo`, `listdecisions`, `getdecision`, `listmarkets`,
`getmarket`, `quotetrade`, `listreputation`, `listpositions`.

Wallet: `createdecision`, `createmarket`, `buyshares`, `sellshares`,
`transfervotecoin`, `sealvote`, `revealvote`.

In `hivemind-qt`: Markets, in the navigation bar.

## Code

| What | Where |
|---|---|
| Market maker and vote resolution | `src/hivemind/math.{h,cpp}` |
| Objects, script formats, rules | `src/hivemind/state.{h,cpp}` |
| Hook into the sidechain rules | `sidechain::State` (`ApplyMainEvents`, `ApplyTx`) |
| Commands | `src/rpc/hivemind.cpp`, `src/wallet/rpc/hivemind.cpp` |
| GUI | `src/qt/marketsdialog.{h,cpp}` |
| Tests | `src/test/hivemind_tests.cpp`, `test/functional/feature_hivemind.py` |

## What the references do differently

The paper and its R code, the LayerTwo-Labs implementation in Rust and the
Skydoge build were read for this. In short: all of them compute in floating point; the Rust
implementation has no sealed votes and keeps periods of a day; Skydoge fixed
the payout bound, the seal, and charging the quoted price, which this
implementation has from the start.
