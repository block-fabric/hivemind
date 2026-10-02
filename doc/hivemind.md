# Hivemind on the sidechain template: state and plan

This tree is the sidechain template with the identity of Hivemind (slot 9 of
the Chains mainchain). It works as a sidechain: merged mining, deposits,
withdrawals. **The prediction markets are not in it yet.** This file records
what the old Hivemind code does, what is wrong with it, and what porting the
markets means, so that the work can be planned.

## What the old code does

The old code (LayerTwo-Labs/hivemind, Bitcoin Core 0.16) has:

- the objects of the markets, carried in transaction outputs: branch,
  decision, market, trade, sealed vote, reveal vote, steal vote, outcome
  (`src/primitives/market.{h,cpp}`);
- an index of those objects in LevelDB (`CMarketTreeDB`);
- 24 wallet RPC commands that create and list them, and a markets page in
  the GUI;
- the market maker (LMSR) and the vote resolution (a singular value
  decomposition, `src/linalg`), both in floating point.

## What is wrong with it

1. **The markets are not consensus.** Validation only *indexes* the market
   objects it finds in the outputs of a block. It does not check that a
   trade pays what the market maker asks, that a vote is cast by someone
   entitled to, or that an outcome is the one the votes give. The wallet does
   that arithmetic for its own transactions; nothing stops another wallet
   from doing it differently. As it is, the chain is a bulletin board for
   market objects.
2. **The index is not reverted** when a block is disconnected, so a
   reorganisation leaves objects of dropped blocks in it.
3. **Floating point.** The price of a trade and the resolution of the votes
   are computed with `double`, `log` and `exp`. Different processors,
   compilers and maths libraries can give results that differ in the last
   bits, which in consensus code splits the chain. This has to become integer
   or fixed point arithmetic before any of it is a rule.
4. **The coinbase amount.** The block reward check allows fees, deposits and
   refunds. Payouts of resolved markets, which the miner of the outcome block
   puts in the coinbase, are not counted, so such a block is rejected as
   `bad-cb-amount`. This is what stopped the Hivemind chain of Skydoge at
   block 496.
5. **Votecoin**, the reputation that entitles to vote, is tracked through
   special addresses and is only loosely tied to the rest.

## What porting means

The objects, the RPC commands and the GUI page can be ported as they are;
that is mechanical. Making the markets *rules of the chain* is new work,
which the old code never did:

1. a market state next to `sidechain::State`, updated and reverted block by
   block: branches, decisions, markets with their shares, the capital held,
   votes, reputation;
2. the market maker in fixed point, with tests that pin its results;
3. rules for every object: who may create it, what it costs, what it pays;
4. vote resolution in fixed point, or a simpler deterministic rule;
5. payouts created by the coinbase, counted like deposits and refunds are.

## Decisions to take first

- Whether trades and outcomes are enforced by every node (the list above),
  or the chain stays a bulletin board as in the old code, with the known
  weakness.
- Whether the resolution keeps the SVD scheme of the Hivemind paper, in
  fixed point, or uses a simpler rule (for example the weighted median of
  the votes).
- Whether reputation (votecoin) is a second coin of the chain, or is derived
  from locked coins.
