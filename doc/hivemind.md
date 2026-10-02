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
  nobody answers, the decision is voted again for as long as before; after too
  many times it is settled as a tie.
- When all decisions of a market are resolved, it **pays**: each share gets
  the product, over the decisions, of how far each went its way. What the
  market maker has left goes back to the author of the market.
- **Fees**: listing fees go to the voters. Trading fees go half to the voters,
  a quarter to the authors of the decisions, a quarter to the author of the
  market. Voters are paid in proportion to the reputation they come out with.

## Parameters

| | Value | Where |
|---|---|---|
| Voting period | one day of mainchain time (6 blocks on regtest) | `period_seconds`, `period_blocks` |
| Reputation at stake per vote (alpha) | 1/10 | `alpha_num`, `alpha_den` |
| Tie band for yes or no | 0.5 ± 0.1 | `tolerance` |
| Vote length of a decision | 1 to 90 periods | `max_vote_periods` |
| Times a decision can be put off | 3 (1 on regtest) | `max_rolls` |
| Listing fee | 0.001 | `listing_fee` |
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
  paper it is by head.)
- **Shares and reputation are balances of accounts**, not coins. Coins paid to
  a market are burned by the transaction that pays them; what a market pays is
  created by the coinbase of the block in which it does, like deposits.
- **A transaction that is well formed is valid.** If what it asks cannot be
  done when its block is connected (the price moved, the market closed, the
  account lacks the shares), nothing happens and the coins burned for it are
  paid back. Sales, transfers and sealed votes are signed by their account;
  the signature names the first input of the transaction, so it cannot be
  replayed.
- **Reverting a block** restores a copy of the whole market state, kept with
  the undo data of each block that changed it. Simple; it grows with the
  markets and wants replacing by a delta before the chain is busy.

## Not in this version

- **Audits and the miner veto** of the paper: a disputed outcome stands.
- Several decisions on one axis of a market, and categorical decisions.
- A tiered, retargeted listing fee: the fee is fixed.
- Freezing reputation during the reveal: reputation counts where it is when
  the vote is resolved, and an account that voted and moved its reputation
  away simply has no weight.

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
Skydoge build were read for this; notes are in `refs/` next to the source
trees. In short: all of them compute in floating point; the Rust
implementation has no sealed votes and keeps periods of a day; Skydoge fixed
the payout bound, the seal, and charging the quoted price, which this
implementation has from the start.
