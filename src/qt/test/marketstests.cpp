// Copyright (c) 2026 The Hivemind developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/marketstests.h>

#include <addresstype.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <interfaces/chain.h>
#include <interfaces/mining.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <node/context.h>
#include <node/miner.h>
#include <pow.h>
#include <qt/clientmodel.h>
#include <qt/marketsdialog.h>
#include <qt/noderpc.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <rpc/server.h>
#include <script/descriptor.h>
#include <script/script.h>
#include <sidechain/mainchain.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>
#include <wallet/context.h>
#include <wallet/receive.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <QLabel>
#include <QTableWidget>

#include <memory>

using wallet::AddWallet;
using wallet::CreateMockableWalletDatabase;
using wallet::CWallet;
using wallet::RemoveWallet;
using wallet::WalletContext;
using wallet::WALLET_FLAG_DESCRIPTORS;

namespace {

/**
 * A regtest sidechain in slot 9 whose mainchain is a record the test writes: each block is
 * committed to by a mainchain block of its own, which may also carry deposits.
 */
struct SideChain {
    TestingSetup& test;
    int main_blocks{0};

    /** Add a mainchain block that commits to `bmm` (if given) and pays `deposits`. */
    bool AppendMain(std::optional<uint256> bmm, std::vector<sidechain::MainDeposit> deposits = {})
    {
        sidechain::Mainchain& record{*Assert(test.m_node.chainman->m_mainchain)};
        sidechain::MainBlock block;
        block.prev_hash = record.TipHash();
        block.hash = uint256{static_cast<uint8_t>(++main_blocks)};
        block.hash.data()[31] = 0x4d;
        block.time = 1'700'000'000 + main_blocks * 600;
        block.bmm = bmm;
        block.deposits = std::move(deposits);
        return record.Append(block);
    }

    /** Mine a block with the mempool, which the next mainchain block commits to. */
    bool Mine(const CScript& script)
    {
        auto mining{interfaces::MakeMining(test.m_node)};
        auto block_template{mining->createNewBlock({.use_mempool = true, .coinbase_output_script = script}, /*cooldown=*/false)};
        if (!block_template) return false;
        CBlock block{block_template->getBlock()};
        block.hashMerkleRoot = BlockMerkleRoot(block);
        while (!CheckProofOfWork(block.GetHash(), block.nBits, test.m_node.chainman->GetConsensus())) ++block.nNonce;
        if (!AppendMain(block.GetHash())) return false;
        bool new_block{false};
        if (!test.m_node.chainman->ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/true, /*min_pow_checked=*/true, &new_block)) return false;
        test.m_node.validation_signals->SyncWithValidationInterfaceQueue();
        return WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Tip()->GetBlockHash()) == block.GetHash();
    }
};

void TestMarketsPage(interfaces::Node& node)
{
    TestingSetup test{ChainType::REGTEST, {.extra_args = {"-sidechainslot=9"}}};
    QVERIFY(test.m_node.chainman->GetConsensus().sidechain.enabled);
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    node.setContext(&test.m_node);
    // The page works through RPC, including the wallet commands.
    wallet_loader->registerRpcs();
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();

    // An empty wallet, which follows the chain.
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(node.context()->chain.get(), "", CreateMockableWalletDatabase());
    {
        LOCK(wallet->cs_wallet);
        wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        wallet->SetupDescriptorScriptPubKeyMans();
        const CBlockIndex* tip{WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Tip())};
        wallet->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
    }
    wallet->m_chain_notifications_handler = wallet->chain().handleNotifications(wallet);
    wallet->SetBroadcastTransactions(true);
    // There is no fee estimate on the test chain.
    wallet->m_min_fee = CFeeRate{10000};
    wallet->m_fallback_fee = CFeeRate{10000};
    WalletContext& context = *node.walletLoader().context();
    AddWallet(context, wallet);
    // Whatever check fails, the wallet goes before the node.
    struct Unload {
        WalletContext& context;
        std::shared_ptr<CWallet>& wallet;
        ~Unload()
        {
            wallet->m_chain_notifications_handler.reset();
            RemoveWallet(context, wallet, /*load_on_start=*/std::nullopt);
        }
    } unload{context, wallet};

    // Coins come to a sidechain by deposits: one to the wallet, paid by the block after the mainchain made it.
    SideChain chain{test};
    const CScript miner{CScript() << OP_TRUE};
    QVERIFY(chain.AppendMain(std::nullopt));
    sidechain::MainDeposit deposit;
    deposit.destination = wallet::getnewaddress(*wallet);
    deposit.amount = 50 * COIN;
    deposit.txid = uint256{0xde};
    QVERIFY(chain.AppendMain(std::nullopt, {deposit}));
    QVERIFY(chain.Mine(miner));
    QCOMPARE(wallet::GetBalance(*wallet).m_mine_trusted, 50 * COIN);

    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    OptionsModel options_model(node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    ClientModel client_model(node, &options_model);
    WalletModel wallet_model(interfaces::MakeWallet(context, wallet), client_model, platform_style.get());

    // A decision, a market on it, and shares of the wallet in the market, each mined.
    const auto rpc{[&](const std::string& method, const UniValue& params, bool of_wallet) {
        QString rpc_error;
        const auto result{NodeRpc::Call(&client_model, method, params, rpc_error, of_wallet ? std::optional<QString>{QString{}} : std::nullopt)};
        if (!result) qWarning("%s: %s", method.c_str(), qPrintable(rpc_error));
        return result;
    }};
    const auto info{rpc("gethivemindinfo", NodeRpc::Args({}), false)};
    QVERIFY(info);
    const int period{(*info)["period"].getInt<int>()};
    const auto decision{rpc("createdecision", NodeRpc::Args({"Will the page show this market?", period + 2}), true)};
    QVERIFY(decision);
    QVERIFY(chain.Mine(miner));
    UniValue decisions{UniValue::VARR};
    decisions.push_back((*decision)["id"]);
    const auto market{rpc("createmarket", NodeRpc::Args({"Shown on the page", decisions, 10}), true)};
    QVERIFY(market);
    QVERIFY(chain.Mine(miner));
    const auto bought{rpc("buyshares", NodeRpc::Args({(*market)["id"], 1, 3}), true)};
    QVERIFY(bought);
    QVERIFY(chain.Mine(miner));
    const auto positions{rpc("getwalletpositions", NodeRpc::Args({}), true)};
    QVERIFY(positions);
    QCOMPARE(positions->size(), size_t{1});

    // The page, offscreen, refreshed: what it reads comes from another thread.
    const NodeRpc::WalletNameFn wallet_name{[] { return std::optional<QString>{QString{}}; }};
    // An application of the tests before shut down, which stopped the thread of the refresh.
    NodeRpc::Restart();
    // And stopped again before the node goes, whatever check fails: no call runs on a node that is gone.
    struct StopRpc {
        ~StopRpc() { NodeRpc::Stop(); }
    } stop_rpc;
    MarketsDialog page(wallet_name);
    page.resize(1100, 750);
    page.setClientModel(&client_model);
    page.refresh();
    auto* summary{page.findChild<QLabel*>("marketsSummary")};
    QTRY_VERIFY2_WITH_TIMEOUT(summary->text().contains("1 markets, 1 decisions"), qPrintable(summary->text()), 10000);
    QVERIFY(page.findChild<QTabWidget*>("marketsTabs")->isEnabled());

    // The market, as a card.
    QList<QWidget*> cards;
    for (QWidget* card : page.findChildren<QWidget*>("marketCard")) {
        if (card->parent() == page.findChild<QWidget*>("marketCards")) cards << card;
    }
    QCOMPARE(cards.size(), 1);
    bool titled{false};
    for (const QLabel* label : cards.at(0)->findChildren<QLabel*>()) titled |= label->text() == "Shown on the page";
    QVERIFY(titled);

    // The shares of the wallet.
    auto* table{page.findChild<QTableWidget*>("myPositions")};
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 0)->text(), QString{"Shown on the page"});
    QCOMPARE(table->item(0, 2)->text().toDouble(), 3.0);
    QVERIFY2(page.findChild<QLabel*>("positionsTotal")->text().contains("in 1 position"), qPrintable(page.findChild<QLabel*>("positionsTotal")->text()));

    // A block the wallet trades in again: the next refresh shows it.
    QVERIFY(rpc("buyshares", NodeRpc::Args({(*market)["id"], 0, 2}), true));
    QVERIFY(chain.Mine(miner));
    page.refresh();
    QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 2, 10000);

}

} // namespace

void MarketsTests::marketsTests()
{
    TestMarketsPage(m_node);
}
