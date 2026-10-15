#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the prediction markets: decisions, markets, trades, votes, resolution, payouts."""

from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SLOT, SidechainTest
from test_framework.descriptors import descsum_create
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

# The key that holds all the reputation when the regtest chain starts.
GENESIS_VOTECOIN_KEY = "cPfsxwfwtqceBD1MsHSt2q9r6bquHNTixtVGUKubYmYySGcSEynR"
PERIOD_BLOCKS = 6
# Blocks of a period in which votes are sealed; they are revealed in the rest.
SEAL_BLOCKS = 4
LISTING_FEE = Decimal("0.001")
REFUND_FEE = Decimal("0.00001")
# Kept for the voters of the proceeds of a sale and of change.
PAYOUT_FEE = Decimal("0.00001")


class HivemindTest(SidechainTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        super().run_test()

    def info(self):
        return self.nodes[0].gethivemindinfo()

    def next_clock(self):
        """Period and phase of the next block, which is the one that transactions sent now go in."""
        height = self.nodes[0].getblockcount() + 1
        return height // PERIOD_BLOCKS, "reveal" if height % PERIOD_BLOCKS >= SEAL_BLOCKS else "seal"

    def advance(self, period, phase="seal"):
        """Mine until the next block is in the given period and phase."""
        while self.next_clock() != (period, phase):
            assert self.next_clock()[0] <= period, "already past"
            self.bmm()
        self.sync_mempools()

    def mine_txs(self):
        self.sync_mempools()
        return self.nodes[0].getblock(self.bmm(), 2)

    def reputation(self):
        return {r["account"]: r["amount"] for r in self.nodes[0].listreputation()}

    def run_sidechain_test(self):
        main = self.main
        side, other = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()

        self.log.info("Set up the sidechain and fund both wallets")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Hivemind", "Prediction markets")
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, side.getnewaddress(), 60)
        main.createsidechaindeposit(SLOT, other.getnewaddress(), 40)
        self.mine_main()
        # A few blocks, so that the periods of the markets have a past.
        for _ in range(3):
            self.bmm()
        # Several coins each, so that a wallet can make several transactions in a block.
        side.sendmany("", {side.getnewaddress(): 5 for _ in range(10)})
        other.sendmany("", {other.getnewaddress(): 3 for _ in range(10)})
        self.mine_txs()
        assert_equal(side.gettxoutsetinfo()["total_amount"], 100)

        self.log.info("All the reputation starts with one key")
        assert_equal(side.importdescriptors([{"desc": descsum_create(f"wpkh({GENESIS_VOTECOIN_KEY})"), "timestamp": "now"}])[0]["success"], True)
        alice = side.listreputation()[0]["account"]
        assert_equal(self.reputation(), {alice: 100})
        assert_equal(side.getaddressinfo(alice)["ismine"], True)
        bob = other.getnewaddress(address_type="bech32")
        carol = other.getnewaddress(address_type="bech32")
        assert_raises_rpc_error(-4, "cannot sign for the account", other.transfervotecoin, alice, bob, 30)
        # Reputation does not move while votes are revealed: a transfer mined then is ignored.
        period, phase = self.next_clock()
        self.advance(period + 1 if phase == "reveal" else period)
        side.transfervotecoin(alice, bob, 30)
        side.transfervotecoin(alice, carol, 30)
        # More than the account holds: mined, and nothing happens.
        self.mine_txs()
        side.transfervotecoin(alice, bob, 41)
        self.mine_txs()
        assert_equal(self.reputation(), {alice: 40, bob: 30, carol: 30})
        # A transfer leaves each account with none, or with at least what seals a vote: the holdings,
        # which every resolution reads, cannot be split ever finer.
        assert_equal(self.info()["minreputation"], Decimal("0.01"))
        assert_equal(self.info()["reputation"], 100)
        assert_raises_rpc_error(-8, "leaves each account with no reputation or with at least 0.01", side.transfervotecoin, alice, bob, Decimal("39.995"))
        assert_raises_rpc_error(-8, "leaves each account with no reputation or with at least 0.01", side.transfervotecoin, alice, side.getnewaddress(), Decimal("0.001"))
        # The largest holders, and the holdings of a wallet, each in one call.
        assert_equal(side.listreputation(1), [{"account": alice, "amount": 40}])
        assert_equal(side.getwalletreputation(), [{"account": alice, "amount": 40}])
        assert_equal(sorted(r["account"] for r in other.getwalletreputation()), sorted([bob, carol]))
        period, phase = self.next_clock()
        self.advance(period, "reveal")
        side.transfervotecoin(alice, bob, 1)
        self.mine_txs()
        assert_equal(self.reputation(), {alice: 40, bob: 30, carol: 30})

        self.log.info("List decisions")
        # From the start of a period, so that what follows does not depend on how many blocks came before.
        self.advance(self.next_clock()[0] + 1)
        period = self.next_clock()[0]
        event = period + 1
        assert_equal(self.info()["listingfee"], LISTING_FEE)
        rain = side.createdecision("Will it rain?", event)
        degrees = side.createdecision("How many degrees?", event, 1, True, 0, 100)
        late = side.createdecision("Is it known?", event)
        # A decision whose event is in the past is not listed, and the fee comes back.
        past = side.createdecision("Did it rain?", period - 1)
        block = self.mine_txs()
        # Less what is kept for the voters.
        refunds = [out for out in block["tx"][0]["vout"] if out["value"] == LISTING_FEE - REFUND_FEE]
        assert_equal(len(refunds), 1)
        decisions = {d["id"]: d for d in side.listdecisions()}
        assert_equal(set(decisions), {rain["id"], degrees["id"], late["id"]})
        assert past["id"] not in decisions
        assert_equal(decisions[rain["id"]]["status"], "open")
        assert_equal(decisions[rain["id"]]["text"], "Will it rain?")
        assert_equal(decisions[rain["id"]]["closeperiod"], event + 1)
        assert_equal(decisions[degrees["id"]]["scaled"], True)
        assert_equal(decisions[degrees["id"]]["max"], 100)
        assert_equal(side.listdecisions("voting"), [])

        self.log.info("Open markets and trade")
        market = side.createmarket("Rain", [rain["id"]], 10, 100)
        assert_equal(market["funding"], Decimal("6.93147181"))
        both = side.createmarket("Rain and degrees", [rain["id"], degrees["id"]], 5)
        # The wallet says at once why a market would not be opened, unless it is told not to look.
        assert_raises_rpc_error(-8, "There is no decision " + "ab" * 32, side.createmarket, "About nothing", ["ab" * 32], 5)
        nothing = side.createmarket("About nothing", ["ab" * 32], 5, 50, True)
        block = self.mine_txs()
        # The market on a decision that does not exist was not opened, and its funding came back.
        assert_equal(set(m["id"] for m in side.listmarkets()), {market["id"], both["id"]})
        assert nothing["id"] not in [m["id"] for m in side.listmarkets()]
        assert_equal(len([out for out in block["tx"][0]["vout"] if out["value"] == nothing["funding"] + nothing["fee"] - REFUND_FEE]), 1)
        rain_market = side.getmarket(market["id"])
        assert_equal(rain_market["status"], "trading")
        assert_equal(rain_market["treasury"], market["funding"])
        assert_equal([s["price"] for s in rain_market["states"]], [Decimal("0.5"), Decimal("0.5")])
        assert_equal([s["answers"] for s in rain_market["states"]], ["N", "Y"])
        assert_equal(len(side.getmarket(both["id"])["states"]), 4)

        # A position is opened with at least 0.001: positions are payouts at settlement.
        assert_raises_rpc_error(-8, "A purchase that opens a position costs at least 0.001", other.buyshares, market["id"], 1, Decimal("0.0001"))
        # Three shares of yes: 10 * ln((e^0.3 + 1) / 2) = 1.61208..., and one percent of fee.
        quote = other.quotetrade(market["id"], 1, 3)
        assert_equal(quote, Decimal("1.61208064") + Decimal("0.01612081"))
        balance = other.getbalance()
        buy = other.buyshares(market["id"], 1, 3)
        assert_equal(buy["quote"], quote)
        fee = -other.gettransaction(buy["txid"])["fee"]
        self.mine_txs()
        dave = buy["account"]
        assert_equal(other.listpositions(dave), [{"market": market["id"], "state": 1, "shares": 3}])
        # The shares of a wallet, in one call (as the Markets tab reads them).
        assert_equal(other.getwalletpositions(), [{"market": market["id"], "account": dave, "state": 1, "shares": 3}])
        assert_equal(side.getwalletpositions(), [])
        # A position holds at least 0.001 shares for each state of its market, whatever they cost.
        assert_greater_than(other.quotetrade(market["id"], 1, Decimal("0.0019")), Decimal("0.001"))
        assert_raises_rpc_error(-8, "A position in this market holds at least 0.002 shares", other.buyshares, market["id"], 1, Decimal("0.0019"))
        rain_market = side.getmarket(market["id"])
        assert_equal(rain_market["treasury"], market["funding"] + Decimal("1.61208064"))
        assert_equal(rain_market["fees"], Decimal("0.01612081"))
        assert_greater_than(rain_market["states"][1]["price"], Decimal("0.57"))
        assert_equal(sum(s["price"] for s in rain_market["states"]), 1)
        # What was put up beyond the cost comes back with the coinbase, which takes a while to mature, less
        # what a payout keeps for the voters.
        for _ in range(2):
            self.bmm()
        assert_equal(other.getbalance(), balance - quote - fee - PAYOUT_FEE)

        # A purchase that the price has moved away from does not happen.
        cheap = other.buyshares(market["id"], 1, 5, Decimal("0.1"), dave)
        block = self.mine_txs()
        assert_equal(len([out for out in block["tx"][0]["vout"] if out["value"] == Decimal("0.1") - REFUND_FEE]), 1)
        assert_equal(other.listpositions(dave)[0]["shares"], 3)
        del cheap

        # Selling one share back.
        assert_raises_rpc_error(-4, "cannot sign for the account", side.sellshares, market["id"], 1, 1, dave)
        assert_raises_rpc_error(-8, "has not sold as many", other.sellshares, market["id"], 0, 1, dave)
        # A sale leaves no shares or shares worth what opens a position: no dust positions for next to nothing.
        assert_raises_rpc_error(-8, "A sale leaves no shares, or at least 0.002 shares worth at least 0.001", other.sellshares, market["id"], 1, Decimal("2.99999"), dave)
        proceeds = -other.quotetrade(market["id"], 1, -1)
        sell = other.sellshares(market["id"], 1, 1, dave)
        assert_equal(sell["quote"], proceeds)
        assert_equal(sell["proceeds"], proceeds - PAYOUT_FEE)
        block = self.mine_txs()
        assert_equal(len([out for out in block["tx"][0]["vout"] if out["value"] == proceeds - PAYOUT_FEE and out["scriptPubKey"].get("address") == dave]), 1)
        assert_equal(other.listpositions(dave)[0]["shares"], 2)
        # Shares of the market on two decisions: rain and many degrees.
        hot = other.buyshares(both["id"], 3, 2)
        self.mine_txs()
        erin = hot["account"]
        # No market owes more than it holds.
        for m in side.listmarkets():
            assert_greater_than(m["treasury"] + Decimal("0.00000001"), max(s["shares"] for s in m["states"]))

        self.log.info("Trading stops when the vote starts")
        self.advance(event + 1)
        closed = other.buyshares(market["id"], 0, 1) if other.quotetrade(market["id"], 0, 1) else None
        self.mine_txs()
        assert_equal(side.getmarket(market["id"])["status"], "closed")
        assert_equal(other.listpositions(dave), [{"market": market["id"], "state": 1, "shares": 2}])
        assert_raises_rpc_error(-8, "Trading in the market \"Rain\" is closed", other.quotetrade, market["id"], 0, 1)
        del closed
        assert_equal(side.getdecision(rain["id"])["status"], "voting")

        self.log.info("Vote: sealed first")
        side.sealvote(alice, rain["id"], "yes")
        other.sealvote(bob, rain["id"], "yes")
        other.sealvote(carol, rain["id"], "yes")
        side.sealvote(alice, degrees["id"], "40")
        other.sealvote(bob, degrees["id"], "42")
        other.sealvote(carol, degrees["id"], "99")
        for voter, node in ((alice, side), (bob, other), (carol, other)):
            node.sealvote(voter, late["id"], "unknown")
        self.mine_txs()
        # Carol thinks again: a later seal replaces the earlier one.
        other.sealvote(carol, rain["id"], "no")
        self.mine_txs()
        votes = side.getdecision(rain["id"])["votes"]
        assert_equal(sorted(v["voter"] for v in votes), sorted([alice, bob, carol]))
        assert all(not v["revealed"] and "answer" not in v for v in votes)
        # A holding that sealed votes is not emptied until they are revealed: moved on, it would seal
        # the votes of another account too.
        assert_raises_rpc_error(-8, "The account sealed votes that close in period", side.transfervotecoin, alice, side.getnewaddress(address_type="bech32"), 40)
        # It is too early to reveal: the wallet will not make public a vote that would not count.
        assert_raises_rpc_error(-1, "Votes on this decision are revealed in the reveal phase of its last period", other.revealvote, carol, rain["id"], "no")
        assert all(not v["revealed"] for v in side.getdecision(rain["id"])["votes"])

        self.log.info("Vote: then revealed")
        self.advance(event + 1, "reveal")
        assert_equal(self.next_clock(), (event + 1, "reveal"))
        side.revealvote(alice, rain["id"], "yes")
        other.revealvote(bob, rain["id"], "yes")
        # Only the vote that was sealed can be revealed: the wallet says so before the chain ignores it.
        assert_raises_rpc_error(-8, "That is not the answer that was sealed", other.revealvote, carol, rain["id"], "yes")
        side.revealvote(alice, degrees["id"], "40")
        other.revealvote(bob, degrees["id"], "42")
        other.revealvote(carol, degrees["id"], "99")
        for voter, node in ((alice, side), (bob, other), (carol, other)):
            node.revealvote(voter, late["id"], "unknown")
        # It is too late to seal: the wallet says so (the chain would ignore the seal).
        assert_raises_rpc_error(-1, "a vote can no longer be sealed in this period", other.sealvote, carol, degrees["id"], "41")
        self.mine_txs()
        assert_equal(self.info()["phase"], "reveal")
        votes = {v["voter"]: v for v in side.getdecision(rain["id"])["votes"]}
        assert_equal(votes[alice], {"voter": alice, "revealed": True, "answer": 1})
        assert_equal(votes[carol]["revealed"], False)
        other.revealvote(carol, rain["id"], "no")
        self.mine_txs()
        assert_equal({v["voter"]: v.get("answer") for v in side.getdecision(rain["id"])["votes"]}, {alice: 1, bob: 1, carol: 0})
        assert_equal({v["voter"]: v.get("answer") for v in side.getdecision(degrees["id"])["votes"]}, {alice: 40, bob: 42, carol: 99})
        assert_equal({v["voter"]: v.get("answer") for v in side.getdecision(late["id"])["votes"]}, {alice: "unknown", bob: "unknown", carol: "unknown"})

        self.log.info("The first block of the next period resolves the votes and pays the markets")
        self.advance(event + 2)
        total_before = side.gettxoutsetinfo()["total_amount"]
        before = side.getbestblockhash()
        block = self.mine_txs()
        resolved = side.getdecision(rain["id"])
        assert_equal(resolved["status"], "resolved")
        assert_equal(resolved["answer"], "yes")
        assert_equal(resolved["outcome"], 1)
        assert_equal(resolved["votes"], [])
        assert_raises_rpc_error(-8, "is resolved already", side.createmarket, "Too late", [rain["id"]], 5)
        scale = side.getdecision(degrees["id"])
        assert_equal(scale["answer"], 42)
        assert_equal(scale["outcome"], Decimal("0.42"))
        # The answer to the third is not known: it is voted again.
        again = side.getdecision(late["id"])
        assert_equal(again["status"], "voting")
        assert_equal(again["rolls"], 1)
        assert_equal(again["closeperiod"], event + 2)

        # Carol stood alone on rain and far off on degrees: she loses a tenth of her reputation to the others.
        reputation = self.reputation()
        assert_equal(reputation[carol], 27)
        assert_equal(sum(reputation.values()), 100)
        assert_greater_than(reputation[alice], 41)
        assert_greater_than(reputation[bob], 31)

        # The markets paid. Two shares of yes pay two coins; two shares of "rain and many
        # degrees" pay 0.42 each.
        payouts = {}
        for out in block["tx"][0]["vout"][1:]:
            if "address" in out["scriptPubKey"]:
                payouts[out["scriptPubKey"]["address"]] = payouts.get(out["scriptPubKey"]["address"], 0) + out["value"]
        assert_equal(payouts[dave], 2)
        assert_equal(payouts[erin], Decimal("0.84"))
        for m in side.listmarkets():
            assert_equal(m["status"], "settled")
            assert_equal(m["treasury"], 0)
            assert_equal(m["fees"], 0)
        assert_equal(other.listpositions(dave), [])
        # The voters got the listing fees of the two decisions and half the trading fees.
        assert_greater_than(payouts[alice], 0)
        assert_greater_than(payouts[bob], 0)
        assert_greater_than(payouts[carol], 0)
        assert_greater_than(payouts[alice], payouts[bob])
        # Nothing is created and nothing is lost: what the markets held is back in circulation,
        # but for the fee of the decision still open and what rounding left with the voters' fees.
        total = side.gettxoutsetinfo()["total_amount"]
        assert_greater_than(total, total_before)
        assert_greater_than(Decimal("100.00000001"), total + LISTING_FEE + self.info()["voterpot"])
        assert_greater_than(total + LISTING_FEE + self.info()["voterpot"], Decimal("99.99999"))
        self.check_in_sync()
        for node in self.nodes:
            assert_equal(node.listreputation(), side.listreputation())
            assert_equal(node.listmarkets(), side.listmarkets())
            assert_equal(node.listdecisions(), side.listdecisions())

        self.log.info("A reorganisation takes the resolution back, and brings it again")
        after = side.getbestblockhash()
        snapshot = (side.listreputation(), side.listmarkets(), side.listdecisions(), total)
        side.invalidateblock(after)
        assert_equal(side.getbestblockhash(), before)
        assert_equal(self.reputation(), {alice: 40, bob: 30, carol: 30})
        assert_equal(side.getdecision(rain["id"])["status"], "voting")
        assert_equal(len(side.getdecision(rain["id"])["votes"]), 3)
        assert_equal(side.getmarket(market["id"])["status"], "closed")
        assert_equal(side.listpositions(dave), [{"market": market["id"], "state": 1, "shares": 2}])
        assert_equal(side.gettxoutsetinfo()["total_amount"], total_before)
        side.reconsiderblock(after)
        assert_equal(side.getbestblockhash(), after)
        assert_equal((side.listreputation(), side.listmarkets(), side.listdecisions(), side.gettxoutsetinfo()["total_amount"]), snapshot)

        self.log.info("The state survives a restart")
        self.restart_node(0)
        assert_equal((side.listreputation(), side.listmarkets(), side.listdecisions(), side.gettxoutsetinfo()["total_amount"]), snapshot)
        self.connect_nodes(0, 1)

        self.log.info("A decision that keeps being unknown is settled as a tie, and costs nobody reputation")
        reputation = self.reputation()
        side.sealvote(alice, late["id"], "unknown")
        self.mine_txs()
        self.advance(event + 2, "reveal")
        side.revealvote(alice, late["id"], "unknown")
        self.mine_txs()
        self.advance(event + 3)
        self.mine_txs()
        tie = side.getdecision(late["id"])
        assert_equal(tie["status"], "resolved")
        assert_equal(tie["answer"], "tie")
        assert_equal(tie["outcome"], Decimal("0.5"))
        # A decision that nobody could answer is no vote to have skipped, nor one to earn by: otherwise
        # anyone could list decisions nobody can answer and take reputation from those who do not vote on them.
        assert_equal(self.reputation(), reputation)
        self.check_in_sync()
        assert_equal(other.listreputation(), side.listreputation())

        self.log.info("Ballots: the votes on many decisions in one seal and one reveal")
        period = self.next_clock()[0]
        first = side.createdecision("Ballot one?", period)
        second = side.createdecision("Ballot two?", period, 1, True, 0, 10)
        self.mine_txs()
        self.advance(period + 1)
        votes = {first["id"]: "yes", second["id"]: 7}
        assert_equal(side.sealballot(alice, votes)["closeperiod"], period + 1)
        other.sealballot(bob, votes)
        self.mine_txs()
        # The ballot as the chain keeps it: its decisions, in order, until it is revealed (getballot), so that an app
        # that lost the record of the answers can still reveal it, with the answers alone.
        ballot = side.getballot(alice, period + 1)
        assert_equal(ballot["closeperiod"], period + 1)
        assert_equal(sorted(ballot["decisions"]), sorted([first["id"], second["id"]]))
        assert_equal(side.getballot(alice, period + 2), None)
        assert_raises_rpc_error(-8, "No such period", side.getballot, alice, -1)
        assert_equal(len(side.getdecision(first["id"])["votes"]), 2)
        assert all(not v["revealed"] for v in side.getdecision(second["id"])["votes"])
        assert_raises_rpc_error(-1, "revealed in the reveal phase", side.revealballot, alice, votes)
        self.advance(period + 1, "reveal")
        assert_raises_rpc_error(-8, "not the answers that were sealed", side.revealballot, alice, {first["id"]: "no", second["id"]: 7})
        assert_raises_rpc_error(-8, "give its answer", side.revealballot, alice, {first["id"]: "yes"})
        side.revealballot(alice, votes)
        other.revealballot(bob, votes)
        self.mine_txs()
        assert_equal(side.getballot(alice, period + 1), None)
        assert_equal({v["voter"]: v.get("answer") for v in side.getdecision(first["id"])["votes"]}, {alice: 1, bob: 1})
        assert_equal({v["voter"]: v.get("answer") for v in side.getdecision(second["id"])["votes"]}, {alice: 7, bob: 7})
        self.advance(period + 2)
        self.mine_txs()
        assert_equal(side.getdecision(first["id"])["answer"], "yes")
        assert_equal(side.getdecision(second["id"])["answer"], 7)
        self.check_in_sync()
        assert_equal(other.listreputation(), side.listreputation())


if __name__ == '__main__':
    HivemindTest(__file__).main()
