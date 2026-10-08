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
| Least purchase that opens a position | 0.001 | `MIN_POSITION` |
| Kept of a refund, for the voters | 0.00001 | `REFUND_FEE` |
| Kept of the proceeds of a sale and of change, for the voters | 0.00001 | `PAYOUT_FEE` |
| Payouts the transactions of a block may give rise to | 250 | `MAX_TX_PAYOUTS_PER_BLOCK` |
| Least a sale leaves of a position, if anything | worth 0.001 | `MIN_POSITION` |
| Least shares of a position, opened or left by a sale | 0.001 for each state of the market | `MinPositionShares` |
| Least payout of a settlement, a sale, change or a refund (less goes to the voters) | 0.00001 | `MIN_PAYOUT` |
| Reputation that seals a vote, and least holding a transfer may leave | `reputation_supply / MAX_VOTERS` (0.01) | `MAX_VOTERS` |
| Decisions in a ballot | 1 to 100, all closing in the same period | `MAX_DECISIONS_PER_PERIOD` |
| Actions per transaction | 16 at most | `MAX_ACTIONS_PER_TX` |
| Decisions per market | 1 to 4 | `max_market_decisions` |
| Reputation supply | 100, to one account at the start | `reputation_supply`, `genesis_account` |

All in `Consensus::HivemindParams` (`src/consensus/params.h`), set per network
in `src/kernel/chainparams.cpp`. **The main network has no genesis account
yet**; one has to be set before launch, and whoever holds its key decides
every outcome until the reputation is spread.

The rules of the audits start at heights of their own: `audit_height` and
`audit2_height` (from the start on the main network, signet and regtest; on
testnet, which ran without them, both at the same height, set when the
release is deployed: between the two, ballots, which are of the second
audit's rules, would have been open to accounts without reputation). The
rules of the third audit are part of `audit2_height`'s. On regtest,
`-hivemindaudit2height=<n>` starts both at height `n`, to test the change.

When the tip reaches the height before `audit2_height` (or goes back below
it), the transactions of the mempool whose requests the next block's rules
no longer take -- signed in the old form, say -- are removed from it: they
could never be mined.

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
  everyone else's. Sales, transfers, sealed votes and ballots are signed by
  their account; the signature names the first input of the transaction, so it
  cannot be replayed, and (from `audit2_height`) the chain -- the hash of its
  genesis block, `genesis_hash` -- and the kind of action, so that it means
  nothing on another chain made of this code that shares an output with this
  one; and the output of the action and the outputs of the transaction, so
  that whoever spends the first input can neither repeat a signed action in
  other outputs (a transfer of reputation sixteen times over) nor move it, nor
  change what the transaction pays. The signed message is
  `"Hivemind " + hex(H("Hivemind action", genesis_hash, kind, n, outputs,
  first input, action script))`, where `n` is the index of the output and
  `outputs` is `H("Hivemind outputs", the outputs, each action without its
  signature)` (`ActionOutputsHash`): the wallet makes the transaction with
  room for the signatures and signs it last (so a fee bump, which changes the
  change output, would need new signatures). Before the height it was
  `"Hivemind " + hex(H(first input, action script))`, which is still what is
  checked below it.
- **The markets are kept on the store of the sidechain state**, one entry per
  key (decisions, markets, positions, votes, reputation), with indexes so that
  no rule reads a table whole: open and resolved decisions by the period their
  vote closes in, markets by each of their decisions, settled markets by the
  period they settled in. A block's changes, the earlier value of each entry
  it touched, are its undo data, as for the rest of the sidechain state.
- **The state does not grow for free** (from `audit_height`): a purchase costs
  at least `MIN_PURCHASE`; a vote is sealed only by an account holding the
  reputation that counts as a voter of its own in the resolution; a settlement
  payout below `MIN_PAYOUT` goes to the voters' pot rather than taking room in
  the coinbase.
- **Payouts do not hold back deposits** (from `audit2_height`). What a
  settlement pays waits in the queue of payouts of transactions, with refunds,
  change and sale proceeds, not in that of the deposits from the mainchain.
  Positions are open to anyone who names an account (a purchase need not be
  signed), so the number of positions in a market, each a payout when it
  settles, is bounded by money instead: a purchase that opens a position pays
  at least `MIN_POSITION` (0.001), which stays in the market until it settles.
  100 000 positions hold 100 coins. A sale leaves no shares of the position or
  shares worth `MIN_POSITION` (what selling them would bring right after), so
  that a purchase and a sale of all but a unit, in one transaction, do not make
  a position for next to nothing. A sale that would leave less sells the whole
  position (the wallet refuses to ask for one, as the chain stands): the price
  can move between the signing and the block, and a sale refused for that would
  let anyone who trades first keep a holder from leaving. `min_proceeds`, of
  what was asked for, still bounds what is paid. And a position holds at least `MIN_POSITION`
  shares for each state of its market (`MinPositionShares`), when it is
  opened and when a sale leaves some: a share of a state pays 1/states of a
  coin when the market settles at even odds, so every position then pays at
  least `MIN_POSITION`, and the payouts of a market are bounded by its
  treasury over `MIN_POSITION`. Otherwise positions bought for `MIN_POSITION`
  at a price near one (pushed up by a purchase of the same author, sold back
  after) would hold a sixteenth of that per state in a market of 16 states, and
  make sixteen times as many payouts for the coins locked. Requiring purchases to be signed by their
  account was considered and not done: keys are free, so it would bound nothing,
  and it would cost 65 bytes and a signature check per purchase. Dust (below
  `MIN_PAYOUT`) of sales, change and refunds goes to the voters' pot.
- **Payouts of transactions are scarce** (from `audit2_height`). Each sale,
  and the change of a purchase or of the opening of a market, is a payout of
  the coinbase: they pay `PAYOUT_FEE` to the voters' pot, as a refund pays
  `REFUND_FEE`. And the transactions of a block give rise to at most
  `MAX_TX_PAYOUTS_PER_BLOCK` (250) payouts, refunds of withdrawals included: a
  transaction whose requests or refunds would take the payouts of the block's
  transactions so far beyond is not valid in that block, and the miner leaves
  it for the next (and fills the block with what still fits). That is half of the 500 the queue of payouts of
  transactions is sure to get of a block when deposits wait, so transactions
  alone cannot make the queue grow, and what settlements put in it, which is
  bounded by the coins held in positions, still drains. The bound is on the
  block rather than on the transaction (which has 16 actions at most anyway):
  it is what makes room scarce, and miners give it to the transactions that
  pay the most for it. The shared queue code of the template is unchanged.
- **Reputation is not split ever finer** (from `audit2_height`). Every
  resolution reads every holding, and writes those that change. A transfer
  leaves each of its accounts with none or with at least
  `reputation_supply / MAX_VOTERS`, so at most about `MAX_VOTERS` (10 000)
  accounts are made at that size; holdings that shrink below it by resolution
  can only be emptied, so splitting costs the splitter the reputation it loses
  by not voting. A transfer that would leave less than that behind moves the
  whole holding (a resolution can change a holding between the signing and
  the block, and a transfer refused for it could be kept from ever emptying
  it; the wallet refuses to ask for one, as the chain stands), and a whole
  holding moves wherever it goes, even into an account that holds nothing, as
  it makes no new holding. Such holdings are not left to add up either: one that is
  below that size before a resolution and after it is folded into the holdings
  of those who voted (if nobody did, of the holders of at least that size), by
  what they come out with. A holding that falls below has a period to be moved
  whole into another account or made up again; holdings made below the size
  before `audit2_height` go at the first resolution from then on. So the
  holdings are about `MAX_VOTERS` at most, plus those made since the last
  resolution. Holdings that a resolution does not change are not rewritten,
  nor noted in the undo data. `gethivemindinfo` counts the holders without
  copying them, `listreputation count` gives the largest holders, and the
  wallet's own holdings come from `getwalletreputation` in one call (the
  Markets tab shows the 200 largest and asks the wallet once per refresh, not
  about every account).
- **Ballots** (from `audit2_height`). `SealBallot` seals a voter's votes on up
  to 100 decisions that close in the same period, with one commitment
  (`BallotCommitment`: voter, period, decisions, answers, salt) and one
  signature; `RevealBallot` gives the answers and the salt, without repeating
  the decisions, which the chain keeps with the ballot. A ballot is the voter's
  for its period: a new one replaces it, together with the seals that were
  still its own; a single `sealvote` replaces it for its decision. Ballots of
  periods that are over leave the state. Room: a reveal is about 100 bytes
  plus 8 per decision, ~900 bytes at 100 decisions, so 10 000 voters
  revealing 100 decisions take ~11 MB with transaction overhead, against
  ~45 MB of weight-limited data space in the 8-hour reveal phase of a day
  period at a block every 10 minutes (single reveals took ~100 MB and did not
  fit, so censoring them, which costs voters reputation, was cheap). The seals
  take ~3.3 KB per voter (~35 MB) in the two-thirds of the period meant for
  sealing. The decisions of a ballot must close together: its reveal shows all
  of its answers at once.
- **Sealed votes name their voter**: the commitment covers the voter, so a
  sealed vote cannot be copied by someone who has not formed an opinion. The
  wallet makes a new salt for every round of voting on a decision and period
  it closes in (a decision moved by a stall is voted on again in the same
  round, and a reveal sent in the reveal phase no block was mined in may have
  made the salt of the old period public; a vote sealed for the old period is
  still revealed with its own salt), and one per
  chain, period and set of decisions for ballots (a signature of the voter's
  key on a message naming `genesis_hash`, the period and the decisions: a key
  signs the same message on every chain made of this code, so a salt revealed
  on one would otherwise open the same key's ballots on another). A vote that was sealed with the salt of the round before
  (mined after the decision was put off) is not revealed by the wallet: that
  salt is public, so the wallet asks for the vote to be sealed again.
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
  Skipped periods are resolved one at a time, those in which a decision
  closed, and only those: a clock that jumps far (on a regtest chain whose
  periods count from time 0, its first block is billions of seconds in) goes
  through no periods with nothing in them. Holders of less than a
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
  it in the next round, with a salt the reveal has made public. Nor before the
  vote opens (the period after the event): the chain would ignore that seal.
- **No reveal before the reveal phase.** The chain ignores a reveal made
  before the reveal phase of the decision's last period, and the vote would be
  public from then on: the wallet refuses it, as of the next block's clock.
- **Shares are held by P2WPKH accounts.** What a market pays is paid to the
  P2WPKH output of the account; buying and selling take P2WPKH addresses only,
  since the key of a P2PKH address may be uncompressed, which P2WPKH cannot
  pay.

## Not in this version

- **Audits and the miner veto** of the paper: a disputed outcome stands.
- Several decisions on one axis of a market, and categorical decisions.
- A retargeted listing fee: the fee only rises with how full a period is.

## Commands

Node: `gethivemindinfo`, `listdecisions`, `getdecision`, `listmarkets`,
`getmarket`, `quotetrade`, `listreputation`, `listpositions`.

Wallet: `createdecision`, `createmarket`, `buyshares`, `sellshares`,
`transfervotecoin`, `sealvote`, `revealvote`, `sealballot`, `revealballot`,
`getwalletreputation`.

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
