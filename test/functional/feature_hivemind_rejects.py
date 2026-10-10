#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that what the markets refuse is refused by the mempool and in blocks, with its reason.

Every way a request of the markets can be malformed (hivemind::CheckTx) is tried twice: as a transaction
sent to the mempool (testmempoolaccept and sendrawtransaction), and in a block committed to by the
mainchain and submitted (submitblock). Each is refused with its own reason. Then a block whose
transactions would give rise to more payouts than a block may pay is refused (hm-block-payouts-full),
and a request in a coinbase is shown to do nothing.
"""

from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SLOT, SidechainTest
from test_framework.blocktools import add_witness_commitment, create_block, create_coinbase
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxOut, hash256, ser_compact_size, ser_string
from test_framework.script import CScript, OP_RETURN, OP_TRUE
from test_framework.util import assert_equal, assert_raises_rpc_error

TAG = bytes([0xDB, 0x48, 0x56, 0x4D])
LISTING_FEE = 100000
MIN_LIQUIDITY = 1000000
MAX_FEE_BPS = 1000
MAX_TEXT_SIZE = 400
MAX_VOTE_PERIODS = 90
MIN_PURCHASE = 10000
MAX_ACTIONS_PER_TX = 16
MAX_DECISIONS_PER_PERIOD = 100
MAX_TX_PAYOUTS_PER_BLOCK = 250
MAX_MONEY = 21000000 * COIN
REPUTATION_SUPPLY = 100 * COIN
# A fee that pays for what the transactions are, so that they are refused for what they ask of the markets.
FEE = 10000
# What a purchase in a market that does not exist is refunded: its payment, less what is kept for the voters.
REFUND = 100000 - 1000


def i64(v):
    return v.to_bytes(8, "little", signed=True)


def u32(v):
    return v.to_bytes(4, "little")


def u16(v):
    return v.to_bytes(2, "little")


def ids(values):
    return ser_compact_size(len(values)) + b"".join(values)


ACCOUNT = bytes([0x11]) * 20
OTHER = bytes([0x22]) * 20
DECISION = bytes([0x33]) * 32
MARKET = bytes([0x44]) * 32
SIGNATURE = bytes([0x1f]) + bytes([0x55]) * 64


def list_decision(text="Will it rain?", event=1000, vote_periods=1, scaled=False, low=0, high=0, author=ACCOUNT):
    return bytes([1]) + author + u32(event) + u16(vote_periods) + bytes([scaled]) + i64(low) + i64(high) + ser_string(text.encode())


def open_market(decisions=(DECISION,), liquidity=MIN_LIQUIDITY, fee_bps=50, text="Rain", author=ACCOUNT):
    return bytes([2]) + author + ids(list(decisions)) + i64(liquidity) + u16(fee_bps) + ser_string(text.encode())


def buy_shares(shares=COIN, state=1, market=MARKET, account=ACCOUNT):
    return bytes([3]) + market + account + bytes([state]) + i64(shares)


def sell_shares(shares=COIN, state=1, min_proceeds=0, signature=SIGNATURE):
    return bytes([4]) + MARKET + ACCOUNT + bytes([state]) + i64(shares) + i64(min_proceeds) + ser_string(signature)


def transfer(amount=COIN, to=OTHER, signature=SIGNATURE):
    return bytes([5]) + ACCOUNT + to + i64(amount) + ser_string(signature)


def seal_vote(signature=SIGNATURE):
    return bytes([6]) + ACCOUNT + DECISION + bytes(32) + ser_string(signature)


def reveal_vote():
    return bytes([7]) + ACCOUNT + DECISION + i64(1) + bytes(32)


def seal_ballot(decisions=(DECISION,), signature=SIGNATURE):
    return bytes([8]) + ACCOUNT + u32(5) + ids(list(decisions)) + bytes(32) + ser_string(signature)


def reveal_ballot(answers=(1,)):
    return bytes([9]) + ACCOUNT + u32(5) + ser_compact_size(len(answers)) + b"".join(i64(a) for a in answers) + bytes(32)


def action(payload, value=0):
    return CTxOut(value, CScript([OP_RETURN, TAG + payload]))


def make_id(txid, n):
    """The id of the decision or market made by output n of a transaction (hivemind::MakeId)."""
    return hash256(bytes.fromhex(txid)[::-1] + u32(n))[::-1].hex()


# Each way a request can be malformed: its name, the outputs that ask for it, and the reason it is refused with.
CASES = [
    ("the tag alone", [action(b"", LISTING_FEE)], "bad-hm-format"),
    ("an unknown kind", [action(bytes([10]) + list_decision()[1:], LISTING_FEE)], "bad-hm-format"),
    ("a byte too many", [action(list_decision() + b"\x00", LISTING_FEE)], "bad-hm-format"),
    ("a byte too few", [action(buy_shares()[:-1], MIN_PURCHASE)], "bad-hm-format"),
    ("one action too many", [action(reveal_vote()) for _ in range(MAX_ACTIONS_PER_TX + 1)], "bad-hm-too-many-actions"),
    ("a listing without text", [action(list_decision(text=""), LISTING_FEE)], "bad-hm-text"),
    ("a listing of a text too long", [action(list_decision(text="x" * (MAX_TEXT_SIZE + 1)), LISTING_FEE)], "bad-hm-text"),
    ("a vote of no period", [action(list_decision(vote_periods=0), LISTING_FEE)], "bad-hm-vote-periods"),
    ("a vote too long", [action(list_decision(vote_periods=MAX_VOTE_PERIODS + 1), LISTING_FEE)], "bad-hm-vote-periods"),
    ("a scale upside down", [action(list_decision(scaled=True, low=5, high=1), LISTING_FEE)], "bad-hm-scale"),
    ("yes or no with an end", [action(list_decision(high=1), LISTING_FEE)], "bad-hm-scale"),
    ("a scale from the unknown answer", [action(list_decision(scaled=True, low=-2**63, high=1), LISTING_FEE)], "bad-hm-scale"),
    ("a listing fee short", [action(list_decision(), LISTING_FEE - 1)], "bad-hm-listing-fee"),
    ("a market without text", [action(open_market(text=""), COIN)], "bad-hm-text"),
    ("a market on no decision", [action(open_market(decisions=()), COIN)], "bad-hm-market-decisions"),
    ("a market on five decisions", [action(open_market(decisions=[bytes([d]) * 32 for d in range(5)]), COIN)], "bad-hm-market-decisions"),
    ("a market on a decision twice", [action(open_market(decisions=(DECISION, DECISION)), COIN)], "bad-hm-market-decisions"),
    ("a liquidity short", [action(open_market(liquidity=MIN_LIQUIDITY - 1), COIN)], "bad-hm-liquidity"),
    ("a liquidity beyond money", [action(open_market(liquidity=MAX_MONEY + 1), COIN)], "bad-hm-liquidity"),
    ("a fee too high", [action(open_market(fee_bps=MAX_FEE_BPS + 1), COIN)], "bad-hm-fee"),
    ("an unfunded market", [action(open_market(), 0)], "bad-hm-funding"),
    ("a purchase of no shares", [action(buy_shares(shares=0), MIN_PURCHASE)], "bad-hm-shares"),
    ("a purchase of a state no market has", [action(buy_shares(state=16), MIN_PURCHASE)], "bad-hm-shares"),
    ("a purchase that pays too little", [action(buy_shares(), MIN_PURCHASE - 1)], "bad-hm-payment"),
    ("a sale of no shares", [action(sell_shares(shares=0))], "bad-hm-shares"),
    ("a sale for less than nothing", [action(sell_shares(min_proceeds=-1))], "bad-hm-proceeds"),
    ("a sale that burns coins", [action(sell_shares(), 1)], "bad-hm-proceeds"),
    ("a sale with a signature of nobody", [action(sell_shares())], "bad-hm-signature"),
    ("a sale unsigned", [action(sell_shares(signature=b""))], "bad-hm-signature"),
    ("a transfer of nothing", [action(transfer(amount=0))], "bad-hm-transfer"),
    ("a transfer of more than there is", [action(transfer(amount=REPUTATION_SUPPLY + 1))], "bad-hm-transfer"),
    ("a transfer to itself", [action(transfer(to=ACCOUNT))], "bad-hm-transfer"),
    ("a transfer with a signature of nobody", [action(transfer())], "bad-hm-signature"),
    ("a seal that burns coins", [action(seal_vote(), 1)], "bad-hm-vote"),
    ("a seal with a signature of nobody", [action(seal_vote())], "bad-hm-signature"),
    ("a reveal that burns coins", [action(reveal_vote(), 1)], "bad-hm-vote"),
    ("a ballot on no decision", [action(seal_ballot(decisions=()))], "bad-hm-vote"),
    ("a ballot on too many decisions", [action(seal_ballot(decisions=[d.to_bytes(32, "little") for d in range(MAX_DECISIONS_PER_PERIOD + 1)]))], "bad-hm-vote"),
    ("a ballot on a decision twice", [action(seal_ballot(decisions=(DECISION, DECISION)))], "bad-hm-vote"),
    ("a ballot with a signature of nobody", [action(seal_ballot())], "bad-hm-signature"),
    ("a ballot reveal of no answer", [action(reveal_ballot(answers=()))], "bad-hm-vote"),
    ("a ballot reveal that burns coins", [action(reveal_ballot(), 1)], "bad-hm-vote"),
]


class HivemindRejectsTest(SidechainTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        super().run_test()

    def signed(self, utxo, outputs, fee=FEE):
        """A transaction of the wallet of node 0 that spends `utxo` alone, with change, signed."""
        node = self.nodes[0]
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(utxo["txid"], 16), utxo["vout"]))]
        tx.vout = list(outputs)
        change = int(utxo["amount"] * COIN) - sum(o.nValue for o in outputs) - fee
        assert change > 0
        tx.vout.append(CTxOut(change, bytes.fromhex(node.getaddressinfo(node.getnewaddress(address_type="bech32"))["scriptPubKey"])))
        signed = node.signrawtransactionwithwallet(tx.serialize().hex())
        assert signed["complete"]
        return signed["hex"]

    def block_with(self, txs, coinbase_outputs=()):
        """A block on the tip of node 0 with these transactions (hex), solved, which the mainchain commits to."""
        node = self.nodes[0]
        tip = node.getblock(node.getbestblockhash())
        coinbase = create_coinbase(tip["height"] + 1, script_pubkey=CScript([OP_TRUE]), nValue=0)
        coinbase.vout.extend(coinbase_outputs)
        block = create_block(int(tip["hash"], 16), coinbase, ntime=tip["time"] + 1, txlist=txs)
        block.nVersion = tip["version"]
        block.nBits = int(tip["bits"], 16)
        add_witness_commitment(block)
        block.solve()
        self.main.createbmmrequest(SLOT, block.hash_hex, Decimal("0.001"))
        self.mine_main()
        return block

    def run_sidechain_test(self):
        main = self.main
        side = self.nodes[0]
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()

        self.log.info("Set up the sidechain, and coins to spend")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Hivemind", "Prediction markets")
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, side.getnewaddress(address_type="bech32"), 50)
        self.mine_main()
        self.bmm()
        side.sendmany("", {side.getnewaddress(address_type="bech32"): Decimal("2") for _ in range(20)})
        self.bmm()
        self.sync_mempools()
        utxos = [u for u in side.listunspent() if u["amount"] == Decimal("2")]
        assert_equal(len(utxos), 20)
        # Blocks with nothing to pay, so that a block built here pays nothing in its coinbase.
        self.bmm()
        self.bmm()

        self.log.info("A well formed request is taken, as a check of how these transactions are made")
        listing = self.signed(utxos[0], [action(list_decision(text="Is this well formed?"), LISTING_FEE)])
        assert_equal(side.testmempoolaccept([listing])[0]["allowed"], True)

        self.log.info("Each malformed request is refused by the mempool, with its reason")
        reasons = set()
        for name, outputs, reason in CASES:
            self.log.debug(f"Mempool: {name}")
            tx = self.signed(utxos[1], outputs)
            result = side.testmempoolaccept([tx])[0]
            assert_equal((name, result["allowed"], result["reject-reason"]), (name, False, reason))
            assert_raises_rpc_error(-26, reason, side.sendrawtransaction, tx, 0.1, 1)
            reasons.add(reason)
        # Every reason the rules have but the one for coinbases, which the mempool never takes.
        assert_equal(len(reasons), 16)
        assert_equal(side.getrawmempool(), [])

        self.log.info("Each malformed request makes a block, committed to by the mainchain, invalid with its reason")
        tip = side.getbestblockhash()
        for name, outputs, reason in CASES:
            self.log.debug(f"Block: {name}")
            block = self.block_with([self.signed(utxos[1], outputs)])
            assert_equal((name, side.submitblock(block.serialize().hex())), (name, reason))
            assert_equal(side.getbestblockhash(), tip)
            assert_equal(side.getblockheader(block.hash_hex)["confirmations"], -1)
        assert_equal(side.listdecisions(), [])

        self.log.info("A block whose transactions would give rise to more payouts than a block may pay is invalid")
        # Purchases in a market that does not exist: each is refunded, which is a payout. Sixteen of them
        # in each of sixteen transactions: 256 payouts.
        refunded = [action(buy_shares(market=bytes([0xEE]) * 30 + n.to_bytes(2, "little")), REFUND + 1000) for n in range(MAX_ACTIONS_PER_TX)]
        txs = [self.signed(utxos[2 + i], refunded) for i in range(16)]
        assert all(side.testmempoolaccept([tx])[0]["allowed"] for tx in txs)
        assert 16 * MAX_ACTIONS_PER_TX > MAX_TX_PAYOUTS_PER_BLOCK
        block = self.block_with(txs)
        assert_equal(side.submitblock(block.serialize().hex()), "hm-block-payouts-full")
        assert_equal(side.getbestblockhash(), tip)
        # Fifteen of them fit; the block has to pay the refunds of their 240 purchases, and does.
        block = self.block_with(txs[:15])
        assert_equal(side.submitblock(block.serialize().hex()), "bad-sc-payouts-missing")
        refunds = [CTxOut(REFUND, CScript([0, ACCOUNT])) for _ in range(15 * MAX_ACTIONS_PER_TX)]
        block = self.block_with(txs[:15], coinbase_outputs=refunds)
        assert_equal(side.submitblock(block.serialize().hex()), None)
        assert_equal(side.getbestblockhash(), block.hash_hex)
        assert_equal(side.getblock(block.hash_hex)["nTx"], 16)
        self.sync_blocks()

        self.log.info("A request in a coinbase does nothing: the coinbase is not a transaction of the markets")
        block = self.block_with([listing], coinbase_outputs=[action(list_decision(text="In a coinbase"), 0)])
        assert_equal(side.submitblock(block.serialize().hex()), None)
        assert_equal(side.getbestblockhash(), block.hash_hex)
        self.sync_blocks()
        decisions = side.listdecisions()
        coinbase_txid = block.vtx[0].txid_hex
        assert_equal([d["text"] for d in decisions], ["Is this well formed?"])
        assert_equal(decisions[0]["id"], make_id(block.vtx[1].txid_hex, 0))
        assert make_id(coinbase_txid, 1) not in [d["id"] for d in decisions]

        self.log.info("The chain goes on, the nodes in step")
        self.bmm()
        self.check_in_sync()
        assert_equal(self.nodes[1].listdecisions(), decisions)


if __name__ == '__main__':
    HivemindRejectsTest(__file__).main()
