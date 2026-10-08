#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the change of the rules of the markets at audit2_height.

A signed action is signed for the rules of a height: one signed before audit2_height and still in the
mempool when the tip reaches it would never be mined. The mempool lets it go as the tip crosses the height.
"""

from feature_hivemind import GENESIS_VOTECOIN_KEY
from feature_sidechain import ACTIVATION_PERIOD, SLOT, SidechainTest
from test_framework.descriptors import descsum_create
from test_framework.util import assert_equal

AUDIT2_HEIGHT = 12


class HivemindUpgradeTest(SidechainTest):
    def set_test_params(self):
        super().set_test_params()
        self.extra_args = [args + [f"-hivemindaudit2height={AUDIT2_HEIGHT}"] for args in self.extra_args]

    def run_test(self):
        super().run_test()

    def run_sidechain_test(self):
        main = self.main
        side, other = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()

        self.log.info("Set up the sidechain and fund the wallet")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Hivemind", "Prediction markets")
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, side.getnewaddress(), 10)
        self.mine_main()
        self.bmm()
        assert_equal(side.importdescriptors([{"desc": descsum_create(f"wpkh({GENESIS_VOTECOIN_KEY})"), "timestamp": "now"}])[0]["success"], True)
        alice = side.listreputation()[0]["account"]
        bob = other.getnewaddress(address_type="bech32")
        # Never in a reveal phase (the last two blocks of six): a transfer mined then is ignored.
        assert AUDIT2_HEIGHT % 6 < 4

        self.log.info("A transfer signed under the old rules waits in the mempool")
        while side.getblockcount() < AUDIT2_HEIGHT - 3:
            self.bmm()
        txid = side.transfervotecoin(alice, bob, 10)["txid"]
        # Kept out of the blocks, as one whose fee is too low might be.
        side.prioritisetransaction(txid, 0, -100000000)
        self.bmm()
        assert_equal(side.getblockcount(), AUDIT2_HEIGHT - 2)
        assert txid in side.getrawmempool()

        self.log.info("The tip reaches the height before audit2_height: it can no longer be mined, and leaves")
        self.bmm()
        assert_equal(side.getblockcount(), AUDIT2_HEIGHT - 1)
        assert txid not in side.getrawmempool()
        assert_equal({r["account"]: r["amount"] for r in side.listreputation()}, {alice: 100})

        # Out of the mempool, it can be abandoned, and its coins spent again.
        side.abandontransaction(txid)

        self.log.info("Signed under the new rules, a transfer is mined")
        side.transfervotecoin(alice, bob, 10)
        self.sync_mempools()
        self.bmm()
        assert_equal(side.getblockcount(), AUDIT2_HEIGHT)
        assert_equal({r["account"]: r["amount"] for r in side.listreputation()}, {alice: 90, bob: 10})


if __name__ == '__main__':
    HivemindUpgradeTest(__file__).main()
