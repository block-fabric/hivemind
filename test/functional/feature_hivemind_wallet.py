#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test what the wallet and the node refuse when asked for the markets, and the state across reorgs and restarts.

- Accounts: a P2PKH account is taken where a key hash is, but not for shares; a key that is not
  compressed cannot sign; an address that is not of a single key is no account.
- A transaction the mempool refuses is abandoned, and its coins can be spent again.
- Answers, ballots, decisions that are not open, markets on decisions being voted on, trades of no shares.
- A vote sealed with the salt of the round before, mined after the decision was put off, is not revealed.
- A reorganisation that takes back the actions of a block, and mines them again.
- The state of the markets across a restart, -reindex and -reindex-chainstate.
- A node that is no sidechain has no markets; periods of time are at least 30 seconds long.
"""

from decimal import Decimal

from feature_hivemind import GENESIS_VOTECOIN_KEY, PERIOD_BLOCKS, SEAL_BLOCKS
from feature_sidechain import ACTIVATION_PERIOD, SLOT, SidechainTest
from test_framework.address import key_to_p2pkh, keyhash_to_p2pkh, script_to_p2sh
from test_framework.descriptors import descsum_create
from test_framework.script import CScript, OP_TRUE
from test_framework.test_node import ErrorMatch, TestNode
from test_framework.util import assert_equal, assert_raises_rpc_error, get_datadir_path, initialize_datadir
from test_framework.wallet_util import generate_keypair

NOT_ACCEPTED = "The transaction was not accepted into the mempool"


class HivemindWalletTest(SidechainTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        super().run_test()

    def next_clock(self):
        height = self.nodes[0].getblockcount() + 1
        return height // PERIOD_BLOCKS, "reveal" if height % PERIOD_BLOCKS >= SEAL_BLOCKS else "seal"

    def advance(self, period, phase="seal"):
        while self.next_clock() != (period, phase):
            assert self.next_clock()[0] <= period, "already past"
            self.bmm()
        self.sync_mempools()

    def mine_txs(self):
        self.sync_mempools()
        return self.nodes[0].getblock(self.bmm(), 2)

    def reputation(self):
        return {r["account"]: r["amount"] for r in self.nodes[0].listreputation()}

    def snapshot(self, node):
        info = node.gethivemindinfo()
        return (node.getbestblockhash(), node.listdecisions(), node.listmarkets(), node.listreputation(), info, node.getsidechainstate()["hash"])

    def run_sidechain_test(self):
        main = self.main
        side, other = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()

        self.log.info("Set up the sidechain, fund both wallets, and give the reputation to a key of the first")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Hivemind", "Prediction markets")
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, side.getnewaddress(), 50)
        main.createsidechaindeposit(SLOT, other.getnewaddress(), 20)
        self.mine_main()
        self.bmm()
        side.sendmany("", {side.getnewaddress(): 2 for _ in range(10)})
        self.mine_txs()
        assert_equal(side.importdescriptors([{"desc": descsum_create(f"wpkh({GENESIS_VOTECOIN_KEY})"), "timestamp": "now"}])[0]["success"], True)
        alice = side.listreputation()[0]["account"]
        bob = other.getnewaddress(address_type="bech32")
        keyhash = bytes.fromhex(side.getaddressinfo(alice)["witness_program"])

        self.log.info("An account is a key hash: its P2PKH address names it as well as its P2WPKH one")
        alice_pkh = keyhash_to_p2pkh(keyhash)
        assert_equal(side.listpositions(alice_pkh), [])
        period, phase = self.next_clock()
        self.advance(period + 1 if phase == "reveal" else period)
        side.transfervotecoin(alice_pkh, bob, 30)
        self.mine_txs()
        assert_equal(self.reputation(), {alice: 70, bob: 30})
        # An address of a script is no account, for the node or for the wallet.
        p2sh = script_to_p2sh(CScript([OP_TRUE]))
        assert_raises_rpc_error(-5, "An account is the address of a single key", side.listpositions, p2sh)
        assert_raises_rpc_error(-5, "An account is the address of a single key", side.transfervotecoin, p2sh, bob, 1)
        assert_raises_rpc_error(-5, "An account is the address of a single key", side.transfervotecoin, alice, side.getnewaddress(address_type="bech32m"), 1)

        self.log.info("A key that is not compressed cannot sign for the markets")
        privkey, pubkey = generate_keypair(compressed=False, wif=True)
        assert_equal(side.importdescriptors([{"desc": descsum_create(f"pkh({privkey})"), "timestamp": "now"}])[0]["success"], True)
        uncompressed = key_to_p2pkh(pubkey)
        assert_equal(side.getaddressinfo(uncompressed)["ismine"], True)
        assert_raises_rpc_error(-4, "is uncompressed: it cannot sign Hivemind actions", side.transfervotecoin, uncompressed, bob, 1)

        self.log.info("A transaction the mempool refuses is abandoned: its coins can be spent again")
        balance = side.getbalances()["mine"]["trusted"]
        period = self.next_clock()[0]
        info = side.gethivemindinfo()
        assert_raises_rpc_error(-4, NOT_ACCEPTED, side.createdecision, "x" * 401, period + 1)
        assert_raises_rpc_error(-4, NOT_ACCEPTED, side.createdecision, "Too long a vote?", period + 1, info["maxvoteperiods"] + 1)
        abandoned = {t["txid"] for t in side.listtransactions("*", 100) if t.get("abandoned")}
        assert_equal(len(abandoned), 2)
        assert_equal(side.getbalances()["mine"]["trusted"], balance)
        assert_equal(side.getrawmempool(), [])

        self.log.info("Decisions to vote on, and a market")
        # From the start of a period, so that all of what follows in it fits.
        self.advance(self.next_clock()[0] + 1)
        period = self.next_clock()[0]
        one = side.createdecision("Voted on for one period?", period)["id"]
        two = side.createdecision("Voted on for two periods?", period, 2)["id"]
        rolled = side.createdecision("Put off?", period)["id"]
        self.mine_txs()
        market = side.createmarket("One", [one], 1)["id"]
        # A fee beyond what the chain takes: the wallet sends it, the mempool refuses it.
        assert_raises_rpc_error(-4, NOT_ACCEPTED, side.createmarket, "Dear", [one], 1, 1001)
        self.mine_txs()
        assert_raises_rpc_error(-8, "The number of shares must not be zero", side.quotetrade, market, 1, 0)
        assert_raises_rpc_error(-5, "Shares are held by a P2WPKH address", side.buyshares, market, 1, 1, None, alice_pkh)

        self.log.info("A reorganisation takes back a block of actions; they go back to the mempool, and are mined again")
        bought = other.buyshares(market, 1, 1)
        listed = side.createdecision("Listed in the block taken back?", period + 3)
        block = self.mine_txs()
        assert bought["txid"] in [t["txid"] for t in block["tx"]]
        after = self.snapshot(side)
        side.invalidateblock(block["hash"])
        assert_equal(side.listpositions(bought["account"]), [])
        assert listed["id"] not in [d["id"] for d in side.listdecisions()]
        assert_equal(set(side.getrawmempool()), {bought["txid"], listed["txid"]})
        # Another block of the same height, with one more transaction: built now, it would be the same block.
        extra = side.sendtoaddress(side.getnewaddress(), 1)
        again = side.getblock(self.bmm(expect=False), 1)
        assert_equal(side.getbestblockhash(), again["hash"])
        assert_equal(set(again["tx"][1:]), {bought["txid"], listed["txid"], extra})
        self.bmm()
        self.check_in_sync()
        assert_equal(side.listpositions(bought["account"]), [{"market": market, "state": 1, "shares": 1}])
        assert_equal(side.listdecisions(), after[1])
        assert_equal(side.listmarkets(), after[2])
        assert_equal(other.listmarkets(), after[2])
        side.reconsiderblock(block["hash"])
        assert_equal(side.getbestblockhash(), other.getbestblockhash())

        self.log.info("The wallet refuses what the chain would ignore, and says why")
        self.advance(period + 1)
        # The first block of the period: the decisions are being voted on.
        self.bmm()
        assert_equal(side.getdecision(one)["status"], "voting")
        assert_raises_rpc_error(-8, "its answer is being voted on", side.createmarket, "Too late", [one], 1)
        assert_raises_rpc_error(-8, "The answer is \"yes\", \"no\", \"unknown\", or a whole number", side.sealvote, alice, one, "maybe")
        assert_raises_rpc_error(-8, "A ballot has 1 to 100 votes", side.sealballot, alice, {})
        assert_raises_rpc_error(-8, "A decision is named twice", side.sealballot, alice, {one: "yes", one.upper(): "no"})
        assert_raises_rpc_error(-8, "The votes on the decisions of a ballot close in the same period", side.sealballot, alice, {one: "yes", two: "yes"})
        assert_raises_rpc_error(-8, "No open decision with this hash", side.revealvote, alice, "ab" * 32, "yes")

        self.log.info("A vote sealed with the salt of a round, mined after the decision was put off, is not revealed")
        sealed = side.sealvote(alice, rolled, "yes")["txid"]
        # Kept out of blocks until the decision is put off: nobody reveals a vote on it in this round.
        side.prioritisetransaction(sealed, 0, -1000000)
        self.advance(period + 2)
        self.bmm()
        assert sealed in side.getrawmempool()
        decision = side.getdecision(rolled)
        assert_equal((decision["status"], decision["rolls"], decision["closeperiod"]), ("voting", 1, period + 2))
        side.prioritisetransaction(sealed, 0, 1000000)
        assert sealed in [t["txid"] for t in self.mine_txs()["tx"]]
        assert_equal([v["voter"] for v in side.getdecision(rolled)["votes"]], [alice])
        self.advance(period + 2, "reveal")
        assert_raises_rpc_error(-1, "This vote was sealed with the salt of the previous round", side.revealvote, alice, rolled, "yes")
        # Of another answer, it is no seal of this account at all.
        assert_raises_rpc_error(-8, "That is not the answer that was sealed", side.revealvote, alice, rolled, "no")
        self.mine_txs()
        self.check_in_sync()

        self.log.info("The state of the markets survives a restart, and is rebuilt by -reindex and -reindex-chainstate")
        expected = self.snapshot(side)
        assert_equal(self.snapshot(other), expected)
        self.restart_node(0)
        assert_equal(self.snapshot(side), expected)
        for rebuild in ["-reindex", "-reindex-chainstate"]:
            self.restart_node(0, extra_args=self.extra_args[0] + [rebuild])
            self.wait_until(lambda: side.getbestblockhash() == expected[0])
            assert_equal(self.snapshot(side), expected)
        self.connect_nodes(0, 1)
        self.bmm()
        self.check_in_sync()

        self.log.info("A node that is no sidechain has no markets")
        # A node of its own, on ports no other uses: those after the mainchain node's.
        initialize_datadir(self.options.tmpdir, 3, self.chain)
        plain = TestNode(3, get_datadir_path(self.options.tmpdir, 3), chain=self.chain, rpchost=None, timewait=self.rpc_timeout,
                         timeout_factor=self.options.timeout_factor, binaries=self.get_binaries(), coverage_dir=self.options.coveragedir,
                         cwd=self.options.tmpdir, extra_conf=["bind=127.0.0.1"], extra_args=[], use_cli=self.options.usecli)
        plain.start()
        plain.wait_for_rpc_connection()
        assert_raises_rpc_error(-1, "This chain is not running as a sidechain, so it has no markets", plain.gethivemindinfo)
        assert_raises_rpc_error(-1, "This chain is not running as a sidechain, so it has no markets", plain.listdecisions)
        plain.stop_node()
        # Periods of time are at least 30 seconds long.
        plain.assert_start_raises_init_error(["-hivemindperiodseconds=29"], "Invalid -hivemindperiodseconds: a period is at least 30 seconds", match=ErrorMatch.PARTIAL_REGEX)

if __name__ == '__main__':
    HivemindWalletTest(__file__).main()
