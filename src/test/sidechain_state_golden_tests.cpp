// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Golden values of the sidechain rules: a fixed sequence of blocks, connected one after another to a
// state of their own, and the hash of the state and of what each coinbase pays out after each block.
// The values were recorded with the hooks of sidechain/hooks.h off, as they are unless a sidechain
// installs its own: any change in what a block does to the state, or in the order of what its coinbase
// pays, fails here. A change meant to make one has to say so, and record the values again.

#include <addresstype.h>
#include <arith_uint256.h>
#include <common/signmessage.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <hash.h>
#include <hivemind/math.h>
#include <hivemind/state.h>
#include <key.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sidechain/hooks.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

/** A sidechain state, and the record of its mainchain, that blocks are connected to one after another. */
struct GoldenChain {
    Consensus::SidechainParams params;
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    sidechain::Mainchain mainchain;
    uint256 prev{uint256{0xab}};
    int height{0};
    //! What the coinbase of the last block connected paid out, after its first output.
    std::vector<CTxOut> paid;

    GoldenChain() { Main(); }

    /** Append a block to the record of the mainchain. */
    void Main(const std::function<void(sidechain::MainBlock&)>& fill = {})
    {
        sidechain::MainBlock block;
        const int h{mainchain.Height() + 1};
        block.hash = ArithToUint256(arith_uint256{static_cast<uint64_t>(0x10000 + h)} << 128);
        block.prev_hash = mainchain.TipHash();
        block.time = 1'700'000'000 + h;
        if (fill) fill(block);
        BOOST_REQUIRE(mainchain.Append(block));
    }

    /**
     * A block of these transactions on the last one, after a mainchain block with these events, its
     * coinbase paying what the rules want (worked out on an overlay that is dropped, as a miner does);
     * then a mainchain block that commits to it. Connected if valid; why not, if not.
     */
    std::string Connect(const std::vector<CTransactionRef>& txs, const std::function<void(sidechain::MainBlock&)>& events = {})
    {
        Main(events);
        CBlock block;
        block.nVersion = 0x20000000;
        block.hashPrevBlock = prev;
        block.nTime = 1'700'000'000 + height + 1;
        block.nBits = 0x207fffff;
        CMutableTransaction coinbase;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (height + 1) << OP_0;
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        block.vtx.push_back(MakeTransactionRef(coinbase));
        block.vtx.insert(block.vtx.end(), txs.begin(), txs.end());
        {
            sidechain::StoreOverlay scratch{store, /*journal=*/false};
            sidechain::State dry{scratch};
            std::vector<CTxOut> payouts, peg_owed, tx_payouts;
            std::string reason;
            // The sidechain's own peg actions, if it has any (none unless a test installs them).
            const sidechain::PegStep step{.main_height = mainchain.Height(), .nonce = prev, .max_withdrawal_height = std::nullopt, .peg_height = std::nullopt};
            if (sidechain::Hooks().BeforePegStep(dry, block, height + 1, step, params, peg_owed, reason) &&
                dry.ApplyMainEvents(mainchain.Height(), mainchain, height + 1, params, payouts, reason) &&
                dry.ApplyTxs(block, height + 1, params, dry.MainPending(mainchain, params), /*peg_step=*/true, tx_payouts, reason)) {
                peg_owed.insert(peg_owed.end(), tx_payouts.begin(), tx_payouts.end());
                for (const CTxOut& out : dry.TakePayouts(std::move(payouts), std::move(peg_owed))) coinbase.vout.push_back(out);
            }
        }
        block.vtx[0] = MakeTransactionRef(coinbase);
        block.hashMerkleRoot = BlockMerkleRoot(block);
        Main([&](sidechain::MainBlock& b) { b.bmm = block.GetHash(); });
        sidechain::StoreOverlay block_store{store, /*journal=*/false};
        sidechain::State block_state{block_store};
        CAmount minted{0};
        std::string reason;
        if (!block_state.ConnectBlock(block, height + 1, params, mainchain, minted, reason)) return reason;
        block_store.MergeInto(store);
        store.TakeUndo();
        ++height;
        prev = block.GetHash();
        paid.assign(coinbase.vout.begin() + 1, coinbase.vout.end());
        return {};
    }

    /** The hash of what the last block paid out, in order. */
    uint256 PaidHash() const
    {
        HashWriter hasher{};
        hasher << paid;
        return hasher.GetHash();
    }

    /** The state and what the last block paid out are those recorded. */
    void Check(const std::string& state_hash, const std::string& paid_hash, size_t paid_count)
    {
        BOOST_CHECK_EQUAL(state.Hash().GetHex(), state_hash);
        BOOST_CHECK_EQUAL(PaidHash().GetHex(), paid_hash);
        BOOST_CHECK_EQUAL(paid.size(), paid_count);
    }

    hivemind::State Markets() const { return hivemind::State{static_cast<const sidechain::StoreView&>(store)}; }
};

/** The key of the tests: always the same, so that its signatures are too. */
CKey FixedKey(uint8_t n)
{
    std::vector<unsigned char> secret(32, 0);
    secret[0] = 0x5e;
    secret[31] = n;
    CKey key;
    key.Set(secret.begin(), secret.end(), /*fCompressedIn=*/true);
    return key;
}

/** A transaction with these outputs, spending a coin of its own so that every one is different. */
CTransactionRef Tx(std::vector<CTxOut> outputs, uint32_t& nonce)
{
    CMutableTransaction tx;
    ++nonce;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(ArithToUint256(arith_uint256{nonce} << 64)), nonce});
    tx.vout = std::move(outputs);
    return MakeTransactionRef(std::move(tx));
}

CTxOut RefundOut(const CKey& key, const COutPoint& withdrawal)
{
    sidechain::RefundRequest request;
    request.withdrawal = withdrawal;
    BOOST_REQUIRE(key.SignCompact(MessageHash(sidechain::RefundMessage(withdrawal)), request.signature));
    return CTxOut{0, sidechain::RefundScript(request)};
}

hivemind::Account TestAccount(size_t n)
{
    hivemind::Account account;
    account.begin()[0] = 0xa0;
    account.begin()[1] = static_cast<uint8_t>(n & 0xff);
    account.begin()[2] = static_cast<uint8_t>(n >> 8);
    return account;
}

/** Installs hooks for the life of the object; the template's again after. */
struct InstallHooks {
    explicit InstallHooks(std::unique_ptr<sidechain::ChainHooks> hooks) { sidechain::SetHooks(std::move(hooks)); }
    ~InstallHooks() { sidechain::SetHooks(nullptr); }
};

/** Hooks whose peg actions of their own owe `count` outputs in every block that takes the peg actions. */
struct PegOwedHooks : sidechain::ChainHooks {
    size_t count;
    explicit PegOwedHooks(size_t n) : count{n} {}
    bool BeforePegStep(sidechain::State&, const CBlock&, int, const sidechain::PegStep&, const Consensus::SidechainParams&,
                       std::vector<CTxOut>& owed_tx, std::string&) const override
    {
        for (size_t i{0}; i < count; ++i) owed_tx.emplace_back(static_cast<CAmount>(7 + i), CScript() << OP_2);
        return true;
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(sidechain_state_golden_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(settlement_then_refunds_past_the_bound)
{
    // A market of many positions, settled at the start of a block (BeginBlock), whose payouts wait with
    // those of transactions; in the same block, refunds of withdrawals up to the bound of what a
    // block's transactions pay out (hivemind::MAX_TX_PAYOUTS_PER_BLOCK), and deposits, more than half a
    // block of them. The refunds beyond the bound wait for the next block.
    GoldenChain chain;
    chain.params.hivemind.period_blocks = 6;
    chain.params.hivemind.max_rolls = 0;
    uint32_t nonce{0};
    const CKey key{FixedKey(1)};
    const CScript pay{GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey()})};
    const size_t refunds{hivemind::MAX_TX_PAYOUTS_PER_BLOCK + 50}, holders{100};

    // Block 1: withdrawals, refundable by the key, and a decision.
    std::vector<CTxOut> withdrawal_outs;
    for (size_t i{0}; i < refunds; ++i) withdrawal_outs.emplace_back(COIN + static_cast<CAmount>(i), sidechain::WithdrawalScript(1000, key.GetPubKey().GetID(), pay));
    const CTransactionRef withdrawals{Tx(withdrawal_outs, nonce)};
    hivemind::ListDecision list;
    list.event_period = 0;
    list.text = "decision";
    const CTransactionRef listed{Tx({CTxOut{chain.params.hivemind.listing_fee, hivemind::ActionScript(list)}}, nonce)};
    BOOST_REQUIRE_EQUAL(chain.Connect({withdrawals, listed}), "");
    chain.Check("9b723960f005ca147d3be14e2b7b614658d51e55625a271fb3442f972447aebb", "9a538906e6466ebd2617d321f71bc94e56056ce213d366773699e28158e00614", 0);

    // Block 2: a market on it.
    hivemind::OpenMarket open;
    open.decisions = {hivemind::MakeId(listed->GetHash(), 0)};
    open.liquidity = chain.params.hivemind.min_liquidity;
    open.text = "market";
    const CTransactionRef opened{Tx({CTxOut{hivemind::LmsrFunding(open.liquidity, 2) + chain.Markets().MarketFee(open.decisions, chain.params.hivemind), hivemind::ActionScript(open)}}, nonce)};
    BOOST_REQUIRE_EQUAL(chain.Connect({opened}), "");
    chain.Check("012d6ad776bb536b51b053bdd0f9db3d1a87a48ec2ee6fef0a14005246886778", "9a538906e6466ebd2617d321f71bc94e56056ce213d366773699e28158e00614", 0);
    const hivemind::Id market{hivemind::MakeId(opened->GetHash(), 0)};
    BOOST_REQUIRE(chain.Markets().GetMarket(market));

    // Block 3: a purchase by each holder, each with change.
    std::vector<CTransactionRef> buys;
    for (size_t i{0}; i < holders; ++i) {
        hivemind::BuyShares buy;
        buy.market = market;
        buy.account = TestAccount(i);
        buy.state = 1;
        buy.shares = hivemind::ONE;
        buys.push_back(Tx({CTxOut{100 * COIN, hivemind::ActionScript(buy)}}, nonce));
    }
    BOOST_REQUIRE_EQUAL(chain.Connect(buys), "");
    chain.Check("c364ccb8a30f00ddf12177fb397903c5ba42b752094fc163ba97d74ce59b1522", "d228b01807e8bdc371800c677e2db4ff0a318b684d158894807eb395a0b1b8ed", holders);

    // Empty blocks up to the one that settles the market.
    while (chain.height < 11) {
        BOOST_REQUIRE_EQUAL(chain.Connect({}), "");
        BOOST_REQUIRE(!chain.Markets().GetMarket(market)->settled);
    }
    chain.Check("ec227b37fb270bca8401ec03b8a95866c01b3d0a754d68b72a089d492e8d1b04", "9a538906e6466ebd2617d321f71bc94e56056ce213d366773699e28158e00614", 0);

    // The refunds, 50 a transaction.
    std::vector<CTransactionRef> refund_txs;
    for (size_t from{0}; from < refunds; from += 50) {
        std::vector<CTxOut> outs;
        for (size_t i{from}; i < from + 50; ++i) outs.push_back(RefundOut(key, COutPoint{withdrawals->GetHash(), static_cast<uint32_t>(i)}));
        refund_txs.push_back(Tx(outs, nonce));
    }
    const auto deposits{[](sidechain::MainBlock& b) {
        for (int i{0}; i < 700; ++i) {
            sidechain::MainDeposit deposit;
            deposit.destination = "deposit " + std::to_string(i);
            deposit.amount = COIN + i;
            deposit.txid = ArithToUint256(arith_uint256{static_cast<uint64_t>(i + 1)});
            b.deposits.push_back(deposit);
        }
    }};
    // Block 12, beyond the bound: not valid, and nothing changes.
    const uint256 before{chain.state.Hash()};
    BOOST_CHECK_EQUAL(chain.Connect(refund_txs, deposits), "hm-block-payouts-full");
    BOOST_CHECK(chain.state.Hash() == before);
    // Up to it: the settlement, and as many refunds as the bound allows.
    const std::vector<CTransactionRef> fitting{refund_txs.begin(), refund_txs.begin() + hivemind::MAX_TX_PAYOUTS_PER_BLOCK / 50};
    BOOST_REQUIRE_EQUAL(chain.Connect(fitting, deposits), "");
    BOOST_CHECK(chain.Markets().GetMarket(market)->settled);
    chain.Check("550ad8034cc4a15adf458bb79fce56b7b679c9cd57bdba958335214faaa6721d", "5f71fe61f99e3a7ce06966e1b9e44ae00aa315616626f96d0300d1c79172d202", sidechain::MAX_PAYOUTS_PER_BLOCK);
    // The deposits first, half the block and what the settlement and the refunds leave; then those, the refunds last.
    BOOST_CHECK(chain.paid[0].nValue == COIN);
    BOOST_CHECK(chain.paid.back().scriptPubKey == pay);
    BOOST_CHECK_EQUAL(std::count_if(chain.paid.begin(), chain.paid.end(), [&](const CTxOut& o) { return o.scriptPubKey == pay; }), static_cast<long>(hivemind::MAX_TX_PAYOUTS_PER_BLOCK));
    BOOST_CHECK(chain.state.TxQueue().empty());

    // Block 13: the other refunds, and the deposits that waited.
    BOOST_REQUIRE_EQUAL(chain.Connect({refund_txs.begin() + hivemind::MAX_TX_PAYOUTS_PER_BLOCK / 50, refund_txs.end()}), "");
    chain.Check("f43d68c841d429973fdaf92ab8c24c6ceca605bfdb5c6440c8c58f939996b5df", "63ef6348aa05015da55c61221618ae5219383c6a6837f35ba5d77422c77778de", 801);
    BOOST_CHECK(chain.state.Queue().empty());
    BOOST_CHECK(chain.state.TxQueue().empty());
}

BOOST_AUTO_TEST_CASE(own_peg_actions_take_no_room_of_the_transactions)
{
    // Not golden values: what a sidechain's own peg actions owe (ChainHooks::BeforePegStep) is paid as
    // refunds are, but takes none of the room the block's transactions have for their payouts
    // (hivemind::MAX_TX_PAYOUTS_PER_BLOCK); it used to, and a block of as many refunds as a block takes
    // was invalid (hm-block-payouts-full).
    InstallHooks installed{std::make_unique<PegOwedHooks>(7)};
    GoldenChain chain;
    uint32_t nonce{0};
    const CKey key{FixedKey(1)};
    const CScript pay{GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey()})};
    std::vector<CTxOut> withdrawal_outs(hivemind::MAX_TX_PAYOUTS_PER_BLOCK + 1, CTxOut{COIN, sidechain::WithdrawalScript(1000, key.GetPubKey().GetID(), pay)});
    const CTransactionRef withdrawals{Tx(withdrawal_outs, nonce)};
    BOOST_REQUIRE_EQUAL(chain.Connect({withdrawals}), "");
    BOOST_CHECK_EQUAL(chain.paid.size(), 7U);
    std::vector<CTxOut> outs;
    for (size_t i{0}; i < hivemind::MAX_TX_PAYOUTS_PER_BLOCK; ++i) outs.push_back(RefundOut(key, COutPoint{withdrawals->GetHash(), static_cast<uint32_t>(i)}));
    const CTransactionRef refunds{Tx(outs, nonce)};
    const CTransactionRef one_more{Tx({RefundOut(key, COutPoint{withdrawals->GetHash(), static_cast<uint32_t>(hivemind::MAX_TX_PAYOUTS_PER_BLOCK)})}, nonce)};
    // One beyond the bound is still one too many.
    BOOST_CHECK_EQUAL(chain.Connect({refunds, one_more}), "hm-block-payouts-full");
    // As many as the bound: valid, and the coinbase pays what the peg actions owe, then the refunds.
    BOOST_REQUIRE_EQUAL(chain.Connect({refunds}), "");
    BOOST_REQUIRE_EQUAL(chain.paid.size(), 7 + hivemind::MAX_TX_PAYOUTS_PER_BLOCK);
    for (size_t i{0}; i < 7; ++i) BOOST_CHECK(chain.paid[i] == (CTxOut{static_cast<CAmount>(7 + i), CScript() << OP_2}));
    for (size_t i{7}; i < chain.paid.size(); ++i) BOOST_CHECK(chain.paid[i] == (CTxOut{COIN, pay}));
}

BOOST_AUTO_TEST_SUITE_END()
