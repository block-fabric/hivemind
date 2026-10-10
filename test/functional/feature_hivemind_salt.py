#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the salts of sealed votes across a decision moved by a stall.

A decision whose last period goes by without a block of this chain in its reveal phase is voted on again
later, in the same round. A reveal sent in that phase -- which the next block's clock allowed, but which no
block mined in it -- made its salt public: a vote sealed again for the new period has to have another salt.
And a vote sealed before the decision was moved can still be revealed.

With periods of mainchain time, a vote can also be revealed as soon as the mainchain is in the reveal phase
of its last period, before a block of this chain is.
"""

from decimal import Decimal

from feature_hivemind import GENESIS_VOTECOIN_KEY
from feature_sidechain import ACTIVATION_PERIOD, SLOT, SidechainTest
from test_framework.descriptors import descsum_create
from test_framework.util import assert_equal

PERIOD_SECONDS = 3600
# Into a period, in seconds: votes are sealed until two thirds of it.
SEAL_AT = 60
REVEAL_AT = 2500


class HivemindSaltTest(SidechainTest):
    def set_test_params(self):
        super().set_test_params()
        self.extra_args = [args + [f"-hivemindperiodseconds={PERIOD_SECONDS}"] for args in self.extra_args]

    def run_test(self):
        super().run_test()

    def go_to(self, period, at, mine=True):
        """Move the mainchain's median time to `at` seconds into `period`, and mine a block of this chain then."""
        when = period * PERIOD_SECONDS + at
        self.main.setmocktime(when)
        self.mine_main(11)
        assert_equal(self.main.getblockchaininfo()["mediantime"], when)
        if mine:
            self.sync_mempools()
            return self.bmm()

    def reveal_salt(self, txid):
        """The salt of the reveal in a transaction: the last 32 bytes of its action."""
        node = self.nodes[0]
        for out in node.decoderawtransaction(node.gettransaction(txid)["hex"])["vout"]:
            script = out["scriptPubKey"]["hex"]
            if script.startswith("6a"):
                return script[-64:]
        raise AssertionError("no reveal")

    def run_sidechain_test(self):
        main = self.main
        side, other = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()

        self.log.info("Set up the sidechain, fund the wallets and give two voters reputation")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Hivemind", "Prediction markets")
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, side.getnewaddress(), 10)
        main.createsidechaindeposit(SLOT, other.getnewaddress(), 10)
        self.mine_main()
        self.bmm()
        assert_equal(side.importdescriptors([{"desc": descsum_create(f"wpkh({GENESIS_VOTECOIN_KEY})"), "timestamp": "now"}])[0]["success"], True)
        alice = side.listreputation()[0]["account"]
        bob = other.getnewaddress(address_type="bech32")
        start = main.getblockchaininfo()["mediantime"] // PERIOD_SECONDS + 1
        self.go_to(start, SEAL_AT)
        side.transfervotecoin(alice, bob, 40)
        self.sync_mempools()
        self.bmm()
        assert_equal({r["account"]: r["amount"] for r in side.listreputation()}, {alice: 60, bob: 40})

        self.log.info("A decision on this period, voted on in the next")
        decision = side.createdecision("Will it rain?", start)["id"]
        self.sync_mempools()
        self.bmm()
        close = start + 1
        assert_equal(side.getdecision(decision)["closeperiod"], close)
        self.go_to(close, SEAL_AT)
        side.sealvote(alice, decision, "yes")
        other.sealvote(bob, decision, "yes")
        self.sync_mempools()
        self.bmm()

        self.log.info("Its reveal phase starts on the mainchain; a reveal is sent, and no block of this chain comes until two periods later")
        self.go_to(close, REVEAL_AT, mine=False)
        first = side.revealvote(alice, decision, "yes")["txid"]
        self.go_to(close + 2, SEAL_AT, mine=False)
        block = side.getblock(self.bmm(), 1)
        # Mined, and ignored: the decision is voted on again in the same round, and the salt is public.
        assert first in block["tx"]
        moved = side.getdecision(decision)
        assert_equal((moved["status"], moved["rolls"], moved["closeperiod"]), ("voting", 0, close + 2))

        self.log.info("Sealed again, the vote has another salt; sealed before, it can still be revealed")
        side.sealvote(alice, decision, "no")
        self.sync_mempools()
        self.bmm()
        self.go_to(close + 2, REVEAL_AT)
        second = side.revealvote(alice, decision, "no")["txid"]
        assert self.reveal_salt(first) != self.reveal_salt(second)
        other.revealvote(bob, decision, "yes")
        self.sync_mempools()
        self.bmm()
        votes = {v["voter"]: v.get("answer") for v in side.getdecision(decision)["votes"]}
        assert_equal(votes, {alice: 0, bob: 1})
        assert_equal(side.getmempoolinfo()["size"], 0)
        assert_equal(Decimal(sum(r["amount"] for r in side.listreputation())), 100)

        self.log.info("Periods are counted in mainchain time, from the beginning of time")
        info = side.gethivemindinfo()
        assert_equal((info["periodseconds"], info["starttime"]), (PERIOD_SECONDS, 0))
        assert "periodblocks" not in info

        self.log.info("A vote can be revealed once the mainchain is in the reveal phase of its period, before this chain is")
        period = close + 3
        self.go_to(period, SEAL_AT)
        early = side.createdecision("Revealed before a block of its reveal phase?", period, 2)["id"]
        self.sync_mempools()
        self.bmm()
        self.go_to(period + 1, SEAL_AT)
        side.sealvote(alice, early, "yes")
        self.sync_mempools()
        self.bmm()
        # The last block of this chain is of the period before; the next one is in the reveal phase of this one.
        self.go_to(period + 2, REVEAL_AT, mine=False)
        assert_equal((side.gethivemindinfo()["period"], side.gethivemindinfo()["phase"]), (period + 1, "seal"))
        side.revealvote(alice, early, "yes")
        self.sync_mempools()
        self.bmm()
        assert_equal(side.getdecision(early)["closeperiod"], period + 2)
        assert_equal(side.getdecision(early)["votes"], [{"voter": alice, "revealed": True, "answer": 1}])


if __name__ == '__main__':
    HivemindSaltTest(__file__).main()
