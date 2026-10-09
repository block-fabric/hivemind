#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that the miner fills a block around the bound on the payouts of its transactions.

The transactions of a block give rise to MAX_TX_PAYOUTS_PER_BLOCK (250) payouts at most. Transactions
that pay well for many payouts each (16 purchases in a market that does not exist, each refunded) must
not leave the rest of the block empty: the miner leaves out those beyond the bound and fills the block
with the others, rather than dropping them after it was filled.

The mempool, for its part, takes a transaction only if it pays the minimum relay fee of 750 vbytes for
each payout it would give rise to (PAYOUT_POLICY_VSIZE), refunds included: a purchase made at 1 sat/vB
is taken, 16 refunded purchases that pay less are not.
"""

from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SLOT, SidechainTest
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxOut, tx_from_hex
from test_framework.script import CScript, OP_RETURN
from test_framework.util import assert_equal, assert_raises_rpc_error

TAG = bytes([0xDB, 0x48, 0x56, 0x4D])
BUY_SHARES = 3
MAX_TX_PAYOUTS_PER_BLOCK = 250
ACTIONS = 16
# Relay policy: the minimum relay fee (sat/kvB) of this many vbytes for each payout.
PAYOUT_POLICY_VSIZE = 750
MIN_RELAY_FEE = 100
HEAVY = 24
LIGHT = 20
# Room for 16 of the heavy transactions (of about 5400 weight each); 15 of them make 240 payouts, and the 16th would go beyond.
BLOCK_MAX_WEIGHT = 100000


def buy_script(n):
    """A purchase in a market that does not exist: it is refunded, which is a payout."""
    market = bytes([0xEE]) * 30 + n.to_bytes(2, "little")
    account = bytes([0x11]) * 20
    return CScript([OP_RETURN, TAG + bytes([BUY_SHARES]) + market + account + bytes([1]) + (100000000).to_bytes(8, "little")])


class HivemindMinerTest(SidechainTest):
    def set_test_params(self):
        super().set_test_params()
        self.extra_args[0] = self.extra_args[0] + [f"-blockmaxweight={BLOCK_MAX_WEIGHT}"]

    def run_test(self):
        super().run_test()

    def spend(self, utxo, outputs, fee):
        """A transaction of the wallet of node 0 that spends `utxo` alone."""
        return self.nodes[0].sendrawtransaction(self.signed(utxo, outputs, fee), 0.1, 1)

    def signed(self, utxo, outputs, fee):
        """The hex of a transaction of the wallet of node 0 that spends `utxo` alone, signed."""
        node = self.nodes[0]
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(utxo["txid"], 16), utxo["vout"]))]
        tx.vout = list(outputs)
        change = int(utxo["amount"] * COIN) - sum(o.nValue for o in outputs) - fee
        tx.vout.append(CTxOut(change, bytes.fromhex(node.getaddressinfo(node.getnewaddress(address_type="bech32"))["scriptPubKey"])))
        signed = node.signrawtransactionwithwallet(tx.serialize().hex())
        assert signed["complete"]
        return signed["hex"]

    def run_sidechain_test(self):
        main = self.main
        side = self.nodes[0]
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()

        self.log.info("Set up the sidechain and fund the wallet with coins of their own")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Hivemind", "Prediction markets")
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, side.getnewaddress(address_type="bech32"), 20)
        self.mine_main()
        self.bmm()
        # One coin for every transaction, so that none depends on another.
        side.sendmany("", {side.getnewaddress(address_type="bech32"): Decimal("0.2") for _ in range(HEAVY + LIGHT)})
        self.bmm()
        self.sync_mempools()
        utxos = [u for u in side.listunspent() if u["amount"] == Decimal("0.2")]
        assert_equal(len(utxos), HEAVY + LIGHT)

        self.log.info("Transactions that pay well for 16 payouts each, and plain ones that pay little")
        heavy = set()
        for i in range(HEAVY):
            outputs = [CTxOut(100000, buy_script(i * ACTIONS + n)) for n in range(ACTIONS)]
            heavy.add(self.spend(utxos[i], outputs, fee=100000))
        light = set()
        for utxo in utxos[HEAVY:]:
            light.add(self.spend(utxo, [CTxOut(1000000, bytes.fromhex(side.getaddressinfo(side.getnewaddress(address_type="bech32"))["scriptPubKey"]))], fee=500))
        assert_equal(len(side.getrawmempool()), HEAVY + LIGHT)
        heavy_weight = sum(tx_from_hex(side.getrawtransaction(t)).get_weight() for t in heavy)
        assert heavy_weight > BLOCK_MAX_WEIGHT, "the heavy transactions alone would fill the block"

        self.log.info("The block takes as many of the heavy ones as the bound allows, and fills the rest with the others")
        block = side.getblock(self.bmm(), 2)
        mined = set(t["txid"] for t in block["tx"][1:])
        taken = MAX_TX_PAYOUTS_PER_BLOCK // ACTIONS
        assert_equal(len(mined & heavy), taken)
        assert_equal(mined & light, light)
        # 240 refunds, each paid by the coinbase (the queue had nothing else to pay).
        coinbase = block["tx"][0]
        assert_equal(sum(1 for o in coinbase["vout"] if o["value"] == Decimal("0.00099")), taken * ACTIONS)

        self.log.info("The rest goes in the next block")
        block = side.getblock(self.bmm(), 1)
        assert_equal(set(block["tx"][1:]), heavy - mined)
        assert_equal(side.getrawmempool(), [])

        self.payout_policy_test()

    def payout_policy_test(self):
        """Relay policy: a transaction pays the minimum relay fee of PAYOUT_POLICY_VSIZE for each payout."""
        side = self.nodes[0]
        side.sendmany("", {side.getnewaddress(address_type="bech32"): Decimal("0.3") for _ in range(3)})
        self.bmm()
        utxos = [u for u in side.listunspent() if u["amount"] == Decimal("0.3")]
        assert_equal(len(utxos), 3)
        # 16 refunds at the minimum relay fee (0.1 sat/vB) of 750 vbytes each.
        needed = ACTIONS * PAYOUT_POLICY_VSIZE * MIN_RELAY_FEE // 1000

        self.log.info("A transaction of 16 refunded purchases that pays well above the minimum relay fee, but not for its payouts, is refused")
        dense = [CTxOut(100000, buy_script(10000 + n)) for n in range(ACTIONS)]
        tx = self.signed(utxos[0], dense, fee=needed - 1)
        vsize = tx_from_hex(tx).get_vsize()
        assert needed - 1 > 5 * vsize * MIN_RELAY_FEE // 1000, "five times the minimum relay fee of its size"
        result = side.testmempoolaccept([tx], 0.1)[0]
        assert_equal(result["allowed"], False)
        assert_equal(result["reject-reason"], "hm-payouts-too-dense")
        assert_raises_rpc_error(-26, "hm-payouts-too-dense", side.sendrawtransaction, tx, 0.1, 1)

        self.log.info("The same, paying for its payouts, is taken")
        side.sendrawtransaction(self.signed(utxos[0], dense, fee=needed), 0.1, 1)

        self.log.info("A plain purchase at the wallet's least fee rate (1 sat/vB) is taken")
        tx = self.signed(utxos[1], [CTxOut(100000, buy_script(20000))], fee=200)
        assert tx_from_hex(tx).get_vsize() >= 180
        txid = side.sendrawtransaction(tx, 0.1, 1)
        assert txid in side.getrawmempool()

        self.log.info("Outputs that pay out nothing are not counted")
        tx = self.signed(utxos[2], [CTxOut(1000000, bytes.fromhex(side.getaddressinfo(side.getnewaddress(address_type="bech32"))["scriptPubKey"]))], fee=200)
        side.sendrawtransaction(tx)

        self.log.info("They go into a block")
        block = side.getblock(self.bmm(), 1)
        assert_equal(len(block["tx"]), 4)
        assert_equal(side.getrawmempool(), [])


if __name__ == '__main__':
    HivemindMinerTest(__file__).main()
