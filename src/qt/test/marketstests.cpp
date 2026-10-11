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

#include <functional>
#include <utility>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>

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
    //! The time of the first mainchain block; each one after is ten minutes later.
    int64_t start_time{1'700'000'000};

    /** Add a mainchain block that commits to `bmm` (if given) and pays `deposits`. */
    bool AppendMain(std::optional<uint256> bmm, std::vector<sidechain::MainDeposit> deposits = {})
    {
        sidechain::Mainchain& record{*Assert(test.m_node.chainman->m_mainchain)};
        sidechain::MainBlock block;
        block.prev_hash = record.TipHash();
        block.hash = uint256{static_cast<uint8_t>(++main_blocks)};
        block.hash.data()[31] = 0x4d;
        block.time = start_time + main_blocks * 600;
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


/** The key that holds all the reputation when the regtest chain starts. */
constexpr const char* GENESIS_VOTECOIN_KEY{"cPfsxwfwtqceBD1MsHSt2q9r6bquHNTixtVGUKubYmYySGcSEynR"};

/** Click Yes on the message box that the next action shows, once it is shown. */
void ConfirmNext()
{
    QTimer::singleShot(0, [] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* box{qobject_cast<QMessageBox*>(widget)}; box && box->isVisible()) box->button(QMessageBox::Yes)->click();
        }
    });
}

/** Once the message box that the next action shows is shown: do `meanwhile`, as a block coming then would, and click Yes. */
void ConfirmNextAfter(std::function<void()> meanwhile)
{
    QTimer::singleShot(0, [meanwhile = std::move(meanwhile)] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* box{qobject_cast<QMessageBox*>(widget)}; box && box->isVisible()) {
                meanwhile();
                box->button(QMessageBox::Yes)->click();
            }
        }
    });
}

/** Refresh the page and wait until it showed what it read, and what a refresh asked for meanwhile read. */
void Settle(MarketsDialog& page)
{
    page.refresh();
    for (int i{0}; i < 2; ++i) {
        // Calls run one after the other: this one runs once those queued before it are done and shown.
        bool done{false};
        NodeRpc::RunAsync(&page, [&done] { return std::function<void()>{[&done] { done = true; }}; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

/** The widgets named `name` that are not about to be deleted. */
template <typename T>
QList<T*> Live(QWidget& page, const char* name)
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return page.findChildren<T*>(name);
}

/** The cards of the markets shown. */
QList<QWidget*> Cards(MarketsDialog& page)
{
    QList<QWidget*> cards;
    for (QWidget* card : Live<QWidget>(page, "marketCard")) {
        if (card->parent() == page.findChild<QWidget*>("marketCards")) cards << card;
    }
    return cards;
}

/** The first of the widgets named `name` that has a label of this text. */
template <typename T = QWidget>
T* WithLabel(MarketsDialog& page, const char* name, const QString& text)
{
    for (T* widget : Live<T>(page, name)) {
        for (const QLabel* label : widget->template findChildren<QLabel*>()) {
            if (label->text() == text) return widget;
        }
    }
    return nullptr;
}

/** The button of a ballot card that answers so. */
QAbstractButton* Answer(QWidget* card, const QString& text)
{
    for (QRadioButton* button : card->findChildren<QRadioButton*>()) {
        if (button->text() == text) return button;
    }
    return nullptr;
}

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

    // From here on the page is shown, as the user sees it: it refreshes itself on every block.
    page.show();
    Settle(page);
    auto* status{page.findChild<QLabel*>("marketsStatus")};
    auto* tabs{page.findChild<QTabWidget*>("marketsTabs")};
    const auto tip_height{[&] { return WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Height()); }};
    // Mine until the next block is the one `next_is` wants, as the clock of the regtest markets goes: periods
    // of 6 blocks, the last 2 to reveal votes.
    const auto mine_until{[&](const std::function<bool(int)>& next_is) {
        for (int i{0}; i < 30 && !next_is(tip_height() + 1); ++i) QVERIFY(chain.Mine(miner));
        QVERIFY(next_is(tip_height() + 1));
    }};

    // Looking for markets.
    tabs->setCurrentIndex(0);
    auto* search{page.findChild<QLineEdit*>("marketSearch")};
    search->setText("no such market");
    QCOMPARE(Cards(page).size(), 0);
    search->clear();
    QCOMPARE(Cards(page).size(), 1);
    auto* sort{page.findChild<QComboBox*>("marketSort")};
    for (int i{1}; i < sort->count(); ++i) {
        sort->setCurrentIndex(i);
        QCOMPARE(Cards(page).size(), 1);
    }
    sort->setCurrentIndex(0);
    page.findChild<QCheckBox*>("showResolved")->setChecked(true);
    QCOMPARE(Cards(page).size(), 1);

    // One market: its card opens it.
    QTest::mouseClick(Cards(page).at(0), Qt::LeftButton);
    auto* back{page.findChild<QPushButton*>("backButton")};
    QVERIFY(back->isVisible());
    QVERIFY(page.findChild<QLabel*>("detailStage")->text().contains("LIVE"));
    QVERIFY(page.findChild<QLabel*>("detailFacts")->text().contains(QString::fromStdString((*market)["id"].get_str())));
    auto* order{page.findChild<QPushButton*>("orderButton")};
    auto* amount{page.findChild<QLineEdit*>("tradeAmount")};
    auto* preview{page.findChild<QLabel*>("tradePreview")};
    auto* order_note{page.findChild<QLabel*>("orderNote")};
    auto* input_mode{page.findChild<QComboBox*>("inputMode")};
    QVERIFY(!amount->isEnabled());
    QVERIFY(page.findChild<QLabel*>("selectedOutcome")->text().startsWith("Pick an outcome"));

    // Buying: the outcome, then how much; the node quotes it, and the order is placed once confirmed.
    QWidget* yes{WithLabel(page, "outcomeRow", "Yes")};
    QVERIFY(yes);
    QTest::mouseClick(yes, Qt::LeftButton);
    QVERIFY(amount->isEnabled());
    QVERIFY(page.findChild<QLabel*>("holding")->text().contains("3.00"));
    amount->setText("abc");
    QCOMPARE(order_note->text(), QString{"Enter an amount above zero."});
    QVERIFY(!order->isEnabled());
    input_mode->setCurrentIndex(1);
    amount->setText("1");
    QVERIFY2(preview->text().contains("You pay:"), qPrintable(preview->text()));
    input_mode->setCurrentIndex(0);
    amount->setText("0.5");
    QVERIFY2(preview->text().contains("You pay:"), qPrintable(preview->text()));
    QVERIFY(order->isEnabled() && order->text().startsWith("Buy for"));
    ConfirmNext();
    QTest::mouseClick(order, Qt::LeftButton);
    QVERIFY2(status->text().startsWith("Bought"), qPrintable(status->text()));
    QVERIFY(amount->text().isEmpty());
    QVERIFY(chain.Mine(miner));
    Settle(page);
    QCOMPARE(table->rowCount(), 3);

    // Selling: of one account, at most what it holds.
    auto* sell_side{page.findChild<QPushButton*>("sellSide")};
    sell_side->click();
    QVERIFY(sell_side->isChecked());
    amount->setText("100");
    QVERIFY2(order_note->text().contains("in one account"), qPrintable(order_note->text()));
    auto* max{page.findChild<QPushButton*>("maxButton")};
    QVERIFY(max->isVisible() && max->isEnabled());
    max->click();
    QCOMPARE(amount->text().toDouble(), 3.0);
    QVERIFY2(preview->text().contains("You get:"), qPrintable(preview->text()));
    QVERIFY(order->text().startsWith("Sell for"));
    ConfirmNext();
    order->click();
    QVERIFY2(status->text().startsWith("Sold"), qPrintable(status->text()));
    QVERIFY(chain.Mine(miner));
    Settle(page);
    QCOMPARE(table->rowCount(), 2);
    back->click();
    QVERIFY(!back->isVisible());

    // The shares, on the tab of the positions: a sale starts there too.
    tabs->setCurrentIndex(1);
    QPushButton* sell_button{nullptr};
    for (int row{0}; row < table->rowCount(); ++row) {
        if (auto* button{qobject_cast<QPushButton*>(table->cellWidget(row, 7))}) sell_button = button;
    }
    QVERIFY(sell_button);
    sell_button->click();
    QCOMPARE(tabs->currentIndex(), 0);
    QVERIFY(back->isVisible() && sell_side->isChecked());
    page.findChild<QPushButton*>("buySide")->click();
    back->click();

    // The columns of a table: sized by hand, kept; put back, fitted again.
    QHeaderView* header{table->horizontalHeader()};
    header->resizeSection(0, 321);
    QVERIFY(table->property("userSized").toBool());
    QVERIFY(QSettings().contains("Markets/columns/myPositions"));
    QVERIFY(!header->actions().isEmpty());
    header->actions().at(0)->trigger();
    QVERIFY(!table->property("userSized").toBool());
    QVERIFY(!QSettings().contains("Markets/columns/myPositions"));

    // Listing decisions.
    tabs->setCurrentIndex(2);
    auto* list_button{page.findChild<QPushButton*>("listDecisionButton")};
    auto* question{page.findChild<QLineEdit*>("decisionText")};
    auto* event_amount{page.findChild<QSpinBox*>("eventAmount")};
    auto* vote_periods{page.findChild<QSpinBox*>("decisionPeriods")};
    list_button->click();
    QCOMPARE(status->text(), QString{"Enter the question."});
    question->setText("Listed from the page?");
    event_amount->setValue(5000);
    list_button->click();
    QVERIFY2(status->text().contains("too far ahead"), qPrintable(status->text()));
    event_amount->setValue(3);
    vote_periods->setValue(100);
    list_button->click();
    QVERIFY2(status->text().contains("too long a vote"), qPrintable(status->text()));
    vote_periods->setValue(1);
    list_button->click();
    QVERIFY2(status->text().startsWith("Listed \"Listed from the page?\""), qPrintable(status->text()));
    QVERIFY(question->text().isEmpty());
    question->setText("How many from the page?");
    page.findChild<QRadioButton*>("decisionScaled")->setChecked(true);
    list_button->click();
    QVERIFY2(status->text().startsWith("Listed"), qPrintable(status->text()));
    QVERIFY(chain.Mine(miner));
    Settle(page);
    QVERIFY2(summary->text().contains("1 markets, 3 decisions"), qPrintable(summary->text()));

    // Opening a market on one of them.
    auto* open_button{page.findChild<QPushButton*>("openMarketButton")};
    open_button->click();
    QCOMPARE(status->text(), QString{"Give the market a title and tick the decisions it is about."});
    auto* market_decisions{page.findChild<QListWidget*>("marketDecisions")};
    QCOMPARE(market_decisions->count(), 3);
    const auto item_of{[&](const QString& text) -> QListWidgetItem* {
        for (int row{0}; row < market_decisions->count(); ++row) {
            if (market_decisions->item(row)->text().startsWith(text)) return market_decisions->item(row);
        }
        return nullptr;
    }};
    auto* decision_filter{page.findChild<QLineEdit*>("marketDecisionFilter")};
    decision_filter->setText("How many");
    QVERIFY(item_of("Listed from the page?")->isHidden() && !item_of("How many from the page?")->isHidden());
    decision_filter->clear();
    QVERIFY(!item_of("Listed from the page?")->isHidden());
    item_of("Listed from the page?")->setCheckState(Qt::Checked);
    QVERIFY2(page.findChild<QLabel*>("marketNote")->text().startsWith("2 outcomes"), qPrintable(page.findChild<QLabel*>("marketNote")->text()));
    page.findChild<QLineEdit*>("marketText")->setText("Opened from the page");
    page.findChild<QLineEdit*>("marketLiquidity")->setText("1");
    open_button->click();
    QVERIFY2(status->text().startsWith("Opened \"Opened from the page\""), qPrintable(status->text()));
    QVERIFY(chain.Mine(miner));
    Settle(page);
    QVERIFY2(summary->text().contains("2 markets, 3 decisions"), qPrintable(summary->text()));

    // The decisions, page by page.
    // Each in a period of its own: the listing fee of a period that fills up is more, from the tenth on.
    for (int i{0}; i < 11; ++i) QVERIFY(rpc("createdecision", NodeRpc::Args({strprintf("Decision %d of many", i), period + 20 + i}), true));
    QVERIFY(chain.Mine(miner));
    Settle(page);
    tabs->setCurrentIndex(4);
    auto* decisions_table{page.findChild<QTableWidget*>("decisions")};
    QCOMPARE(decisions_table->rowCount(), 14);
    auto* page_size{page.findChild<QComboBox*>("decisionPageSize")};
    auto* next_page{page.findChild<QPushButton*>("decisionNext")};
    auto* previous_page{page.findChild<QPushButton*>("decisionPrev")};
    page_size->setCurrentIndex(0);
    QCOMPARE(decisions_table->rowCount(), 10);
    QVERIFY(next_page->isEnabled() && !previous_page->isEnabled());
    next_page->click();
    QCOMPARE(decisions_table->rowCount(), 4);
    QVERIFY(!next_page->isEnabled() && previous_page->isEnabled());
    previous_page->click();
    QCOMPARE(decisions_table->rowCount(), 10);
    page_size->setCurrentIndex(1);
    auto* decision_search{page.findChild<QLineEdit*>("decisionSearch")};
    auto* decision_status{page.findChild<QComboBox*>("decisionFilter")};
    decision_status->setCurrentIndex(2);
    QCOMPARE(decisions_table->rowCount(), 0);
    decision_status->setCurrentIndex(1);
    QCOMPARE(decisions_table->rowCount(), 14);
    decision_status->setCurrentIndex(0);
    page.findChild<QCheckBox*>("decisionMine")->setChecked(true);
    QCOMPARE(decisions_table->rowCount(), 14);
    page.findChild<QCheckBox*>("decisionMine")->setChecked(false);
    decision_search->setText("How many");
    QCOMPARE(decisions_table->rowCount(), 1);
    // A market on the decision selected, from here.
    auto* use{page.findChild<QPushButton*>("decisionUse")};
    decisions_table->setCurrentCell(-1, -1);
    use->click();
    QCOMPARE(status->text(), QString{"Select a decision first."});
    decisions_table->setCurrentCell(0, 0);
    use->click();
    QCOMPARE(tabs->currentIndex(), 2);
    QCOMPARE(item_of("How many from the page?")->checkState(), Qt::Checked);
    QCOMPARE(page.findChild<QLineEdit*>("marketText")->text(), QString{"How many from the page?"});
    item_of("How many from the page?")->setCheckState(Qt::Unchecked);
    item_of("Listed from the page?")->setCheckState(Qt::Unchecked);

    // Reputation: the key that holds all of it on regtest, imported into the wallet.
    const std::string descriptor{strprintf("wpkh(%s)", GENESIS_VOTECOIN_KEY)};
    const auto descriptor_info{rpc("getdescriptorinfo", NodeRpc::Args({descriptor}), false)};
    QVERIFY(descriptor_info);
    UniValue import{UniValue::VOBJ};
    import.pushKV("desc", descriptor + "#" + (*descriptor_info)["checksum"].get_str());
    import.pushKV("timestamp", "now");
    UniValue imports{UniValue::VARR};
    imports.push_back(import);
    const auto imported{rpc("importdescriptors", NodeRpc::Args({imports}), true)};
    QVERIFY(imported && (*imported)[0]["success"].get_bool());
    tabs->setCurrentIndex(3);
    Settle(page);
    auto* reputation{page.findChild<QTableWidget*>("reputation")};
    auto* voter{page.findChild<QComboBox*>("voter")};
    QCOMPARE(reputation->rowCount(), 1);
    QCOMPARE(reputation->item(0, 3)->text(), QString{"yes"});
    QCOMPARE(voter->count(), 1);
    QVERIFY2(page.findChild<QLabel*>("voterStats")->text().contains("this wallet can vote"), qPrintable(page.findChild<QLabel*>("voterStats")->text()));

    // Moving some of it to another account of the wallet; not while votes are revealed, when it would not move.
    mine_until([](int height) { return height % 6 < 4; });
    Settle(page);
    auto* transfer_button{page.findChild<QPushButton*>("transferButton")};
    reputation->setCurrentCell(-1, -1);
    transfer_button->click();
    QVERIFY2(status->text().startsWith("Select an account of this wallet"), qPrintable(status->text()));
    reputation->setCurrentCell(0, 0);
    page.findChild<QLineEdit*>("transferTo")->setText(QString::fromStdString(wallet::getnewaddress(*wallet)));
    page.findChild<QLineEdit*>("transferAmount")->setText("10");
    transfer_button->click();
    QVERIFY2(status->text().startsWith("The reputation is on its way."), qPrintable(status->text()));
    QVERIFY(chain.Mine(miner));
    Settle(page);
    QCOMPARE(reputation->rowCount(), 2);
    QCOMPARE(voter->count(), 2);

    // Voting on the first decision, once its answer is known: sealed, then revealed.
    const int event{period + 2};
    // The first block of the period after the event shows it as being voted on; the next can seal votes.
    mine_until([&](int height) { return height / 6 == event + 1 && height % 6 >= 1 && height % 6 < 4; });
    Settle(page);
    const QString first_question{"Will the page show this market?"};
    QWidget* card{WithLabel(page, "ballotCard", first_question)};
    QVERIFY(card);
    // The account with the most: the one that keeps 90.
    const QString holder{reputation->item(0, 0)->text()};
    voter->setCurrentIndex(voter->findText(holder));
    Q_EMIT voter->activated(voter->currentIndex());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    card = WithLabel(page, "ballotCard", first_question);
    QVERIFY(card);
    auto* seal{card->findChild<QPushButton*>("sealButton")};
    auto* reveal{card->findChild<QPushButton*>("revealButton")};
    QVERIFY(seal->isEnabled() && !reveal->isEnabled());
    seal->click();
    QCOMPARE(status->text(), QString{"Choose an answer first."});
    Answer(card, "Yes")->click();
    seal->click();
    QVERIFY2(status->text().startsWith("Sealed the vote"), qPrintable(status->text()));
    QVERIFY(chain.Mine(miner));
    Settle(page);
    card = WithLabel(page, "ballotCard", first_question);
    QCOMPARE(card->findChild<QPushButton*>("sealButton")->text(), QString{"Seal another answer"});
    QVERIFY(!card->findChild<QPushButton*>("revealButton")->isEnabled());
    // The periods: one decision being voted on.
    tabs->setCurrentIndex(5);
    QVERIFY(page.findChild<QLabel*>("periodFacts")->text().contains("Counted in blocks"));
    QCOMPARE(page.findChild<QTableWidget*>("votingDecisions")->rowCount(), 1);
    tabs->setCurrentIndex(3);
    // The reveal phase of the last period of the vote: the answer sealed is filled in.
    mine_until([&](int height) { return height / 6 == event + 1 && height % 6 == 5; });
    Settle(page);
    card = WithLabel(page, "ballotCard", first_question);
    reveal = card->findChild<QPushButton*>("revealButton");
    QVERIFY(reveal->isEnabled());
    QVERIFY(Answer(card, "Yes")->isChecked());
    reveal->click();
    QVERIFY2(status->text().startsWith("Revealed the vote"), qPrintable(status->text()));
    QVERIFY(chain.Mine(miner));
    Settle(page);
    QVERIFY(WithLabel(page, "ballotCard", first_question)->findChildren<QPushButton*>().isEmpty());
    const auto votes{rpc("getdecision", NodeRpc::Args({(*decision)["id"]}), false)};
    QVERIFY(votes);
    QCOMPARE((*votes)["votes"].size(), size_t{1});
    QVERIFY((*votes)["votes"][0]["revealed"].get_bool());

    // The next period resolves it, and the market on it pays: it is no longer traded, and no market can be opened on it.
    QVERIFY(chain.Mine(miner));
    Settle(page);
    QVERIFY(rpc("getdecision", NodeRpc::Args({(*decision)["id"]}), false).value()["status"].get_str() == "resolved");
    tabs->setCurrentIndex(4);
    decision_search->setText("Will the page");
    QCOMPARE(decisions_table->rowCount(), 1);
    decisions_table->setCurrentCell(0, 0);
    use->click();
    QCOMPARE(status->text(), QString{"A market can only be opened on a decision that is still open."});
    tabs->setCurrentIndex(0);
    page.findChild<QCheckBox*>("showResolved")->setChecked(false);
    QCOMPARE(Cards(page).size(), 1);
    page.findChild<QCheckBox*>("showResolved")->setChecked(true);
    QWidget* settled{nullptr};
    for (QWidget* c : Cards(page)) {
        for (const QLabel* label : c->findChildren<QLabel*>()) {
            if (label->text() == "Shown on the page") settled = c;
        }
    }
    QVERIFY(settled);
    QTest::mouseClick(settled, Qt::LeftButton);
    QVERIFY(page.findChild<QLabel*>("detailStage")->text().contains("RESOLVED"));
    QVERIFY(page.findChild<QLabel*>("selectedOutcome")->text().contains("paid out"));
    back->click();
    page.hide();
}

/** Run a call without a node, or on a node, on the thread of the calls. */
void TestNodeRpc(interfaces::Node& node)
{
    QString error;
    QVERIFY(!NodeRpc::Call(nullptr, "getblockcount", NodeRpc::Args({}), error));
    QCOMPARE(error, QString{"The node is not available."});
    bool done{false};
    QObject receiver;
    NodeRpc::CallAsync(&receiver, nullptr, "getblockcount", NodeRpc::Args({}), [&](std::optional<UniValue> result, const QString& message) {
        done = !result && message == "The node is not available.";
    });
    QVERIFY(done);

    TestingSetup test{ChainType::REGTEST};
    node.setContext(&test.m_node);
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    OptionsModel options_model(node);
    bilingual_str init_error;
    QVERIFY(options_model.Init(init_error));
    ClientModel client_model(node, &options_model);
    NodeRpc::Restart();
    struct StopRpc {
        ~StopRpc() { NodeRpc::Stop(); }
    } stop_rpc;

    std::optional<UniValue> result;
    done = false;
    NodeRpc::CallAsync(&receiver, &client_model, "getblockcount", NodeRpc::Args({}), [&](std::optional<UniValue> r, const QString& message) {
        result = r;
        error = message;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QVERIFY(result);
    QCOMPARE(result->getInt<int>(), 0);
    // The error of a call, as its message.
    done = false;
    NodeRpc::CallAsync(&receiver, &client_model, "getblockhash", NodeRpc::Args({1000}), [&](std::optional<UniValue> r, const QString& message) {
        result = r;
        error = message;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QVERIFY(!result);
    QCOMPARE(error, QString{"Block height out of range"});
    // A call with the wrong number of arguments: its help, as the message.
    QVERIFY(!NodeRpc::Call(&client_model, "getblockcount", NodeRpc::Args({1}), error));
    QVERIFY2(error.startsWith("getblockcount\n"), qPrintable(error));
    // A receiver gone before the call is done hears nothing of it.
    bool heard{false};
    auto* gone{new QObject};
    NodeRpc::CallAsync(gone, &client_model, "getblockcount", NodeRpc::Args({}), [&](std::optional<UniValue>, const QString&) { heard = true; });
    delete gone;
    done = false;
    NodeRpc::RunAsync(&receiver, [&done] { return std::function<void()>{[&done] { done = true; }}; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QVERIFY(!heard);
    // Once stopped, nothing is run.
    NodeRpc::Stop();
    bool ran{false};
    NodeRpc::RunAsync(&receiver, [&ran] { ran = true; return std::function<void()>{}; });
    QTest::qWait(200);
    QVERIFY(!ran);
    NodeRpc::Restart();
    NodeRpc::RunAsync(&receiver, [&ran] { ran = true; return std::function<void()>{}; });
    QTRY_VERIFY_WITH_TIMEOUT(ran, 10000);

    // What the pages show.
    QCOMPARE(NodeRpc::HashRate(999), QString{"999 H/s"});
    QCOMPARE(NodeRpc::HashRate(1234567), QString{"1.23 MH/s"});
    QCOMPARE(NodeRpc::HashRate(2.5e21), QString{"2500.00 EH/s"});
    QCOMPARE(NodeRpc::Text(UniValue{"text"}), QString{"text"});
    QCOMPARE(NodeRpc::Text(UniValue{42}), QString{"42"});
    std::unique_ptr<QTableWidgetItem> item{NodeRpc::Item("cell")};
    QVERIFY(!(item->flags() & Qt::ItemIsEditable));
}

/**
 * Markets on a scale and on two decisions, a vote on a scale, and what the page says when an order
 * cannot be quoted, a wallet without reputation votes, or the market shown goes with a reorg.
 */
void TestScaledMarketsPage(interfaces::Node& node)
{
    TestingSetup test{ChainType::REGTEST, {.extra_args = {"-sidechainslot=9"}}};
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    node.setContext(&test.m_node);
    wallet_loader->registerRpcs();
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();

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
    wallet->m_min_fee = CFeeRate{10000};
    wallet->m_fallback_fee = CFeeRate{10000};
    WalletContext& context = *node.walletLoader().context();
    AddWallet(context, wallet);
    struct Unload {
        WalletContext& context;
        std::shared_ptr<CWallet>& wallet;
        ~Unload()
        {
            wallet->m_chain_notifications_handler.reset();
            RemoveWallet(context, wallet, /*load_on_start=*/std::nullopt);
        }
    } unload{context, wallet};

    SideChain chain{test};
    const CScript miner{CScript() << OP_TRUE};
    QVERIFY(chain.AppendMain(std::nullopt));
    sidechain::MainDeposit deposit;
    deposit.destination = wallet::getnewaddress(*wallet);
    deposit.amount = 50 * COIN;
    deposit.txid = uint256{0xdf};
    QVERIFY(chain.AppendMain(std::nullopt, {deposit}));
    QVERIFY(chain.Mine(miner));

    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    OptionsModel options_model(node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    ClientModel client_model(node, &options_model);
    WalletModel wallet_model(interfaces::MakeWallet(context, wallet), client_model, platform_style.get());
    const auto rpc{[&](const std::string& method, const UniValue& params, bool of_wallet) {
        QString rpc_error;
        const auto result{NodeRpc::Call(&client_model, method, params, rpc_error, of_wallet ? std::optional<QString>{QString{}} : std::nullopt)};
        if (!result) qWarning("%s: %s", method.c_str(), qPrintable(rpc_error));
        return result;
    }};
    const auto tip_height{[&] { return WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Height()); }};
    const auto mine_until{[&](const std::function<bool(int)>& next_is) {
        for (int i{0}; i < 30 && !next_is(tip_height() + 1); ++i) QVERIFY(chain.Mine(miner));
        QVERIFY(next_is(tip_height() + 1));
    }};

    // A decision on a scale from 0 to 100, one of yes or no, a market on the first and one on both.
    const int period{rpc("gethivemindinfo", NodeRpc::Args({}), false).value()["period"].getInt<int>()};
    const int event{period + 2};
    const QString scaled_question{"How many on the scale?"};
    const auto scaled{rpc("createdecision", NodeRpc::Args({scaled_question.toStdString(), event, 1, true, 0, 100}), true)};
    QVERIFY(scaled);
    const auto binary{rpc("createdecision", NodeRpc::Args({"Yes or no?", event}), true)};
    QVERIFY(binary);
    QVERIFY(chain.Mine(miner));
    UniValue one{UniValue::VARR};
    one.push_back((*scaled)["id"]);
    UniValue both{one};
    both.push_back((*binary)["id"]);
    const auto on_scale{rpc("createmarket", NodeRpc::Args({"On a scale", one, 10}), true)};
    QVERIFY(on_scale);
    const std::string on_scale_id{(*on_scale)["id"].get_str()};
    QVERIFY(rpc("createmarket", NodeRpc::Args({"On two decisions", both, 10}), true));
    QVERIFY(chain.Mine(miner));

    NodeRpc::Restart();
    struct StopRpc {
        ~StopRpc() { NodeRpc::Stop(); }
    } stop_rpc;
    MarketsDialog page([] { return std::optional<QString>{QString{}}; });
    page.resize(1100, 750);
    page.setClientModel(&client_model);
    page.show();
    Settle(page);
    auto* summary{page.findChild<QLabel*>("marketsSummary")};
    QVERIFY2(summary->text().contains("2 markets, 2 decisions"), qPrintable(summary->text()));
    auto* status{page.findChild<QLabel*>("marketsStatus")};
    auto* tabs{page.findChild<QTabWidget*>("marketsTabs")};
    auto* back{page.findChild<QPushButton*>("backButton")};
    auto* amount{page.findChild<QLineEdit*>("tradeAmount")};
    auto* order{page.findChild<QPushButton*>("orderButton")};
    auto* order_note{page.findChild<QLabel*>("orderNote")};
    auto* input_mode{page.findChild<QComboBox*>("inputMode")};
    tabs->setCurrentIndex(0);

    // A market on one number: where on the scale it puts it, and its outcomes the two ends of the scale.
    QWidget* card{WithLabel(page, "marketCard", "On a scale")};
    QVERIFY(card);
    QTest::mouseClick(card, Qt::LeftButton);
    QVERIFY(back->isVisible());
    bool gauge{false};
    for (const QLabel* label : Live<QLabel>(page, nullptr)) gauge |= label->text().startsWith("The market puts it at about <b style=\"font-size:16pt\">50.00</b> on the scale from 0 to 100.");
    QVERIFY(gauge);
    QVERIFY(WithLabel(page, "outcomeRow", "Lower (toward 0)"));
    QWidget* higher{WithLabel(page, "outcomeRow", "Higher (toward 100)")};
    QVERIFY(higher);

    // Orders the page cannot place: coins that buy no share, and more shares than there are coins.
    QTest::mouseClick(higher, Qt::LeftButton);
    QVERIFY(amount->isEnabled());
    input_mode->setCurrentIndex(0);
    amount->setText("0.0000000001");
    QCOMPARE(order_note->text(), QString{"That buys no shares."});
    QVERIFY(!order->isEnabled());
    input_mode->setCurrentIndex(1);
    amount->setText("30000000");
    QCOMPARE(order_note->text(), QString{"Amount out of range"});
    QVERIFY(!order->isEnabled());
    amount->clear();
    input_mode->setCurrentIndex(0);
    back->click();

    // A market on two decisions: each outcome names an answer to each.
    card = WithLabel(page, "marketCard", "On two decisions");
    QVERIFY(card);
    QTest::mouseClick(card, Qt::LeftButton);
    QVERIFY(WithLabel(page, "outcomeRow", QStringLiteral("%1: Lower (toward 0)  ·  Yes or no?: No").arg(scaled_question)));
    QVERIFY(WithLabel(page, "outcomeRow", QStringLiteral("%1: Higher (toward 100)  ·  Yes or no?: Yes").arg(scaled_question)));
    back->click();

    // An order confirmed while blocks come: it goes to the market confirmed, and only while it is as it was.
    // The page fills its list of markets again with each block; one opened meanwhile can come before it.
    const auto index_of{[&](const std::string& id) {
        const auto markets{rpc("listmarkets", NodeRpc::Args({}), false)};
        for (size_t i{0}; markets && i < markets->size(); ++i) {
            if ((*markets)[i]["id"].get_str() == id) return int(i);
        }
        return -1;
    }};
    const auto shares_in{[&](const std::string& id) {
        double shares{0};
        int others{0};
        const auto positions{rpc("getwalletpositions", NodeRpc::Args({}), true)};
        if (!positions) return std::make_pair(-1.0, -1);
        for (const UniValue& p : positions->getValues()) {
            if (p["market"].get_str() == id) shares += p["shares"].get_real();
            else ++others;
        }
        return std::make_pair(shares, others);
    }};
    const auto order_on_scale{[&] {
        card = WithLabel(page, "marketCard", "On a scale");
        QVERIFY(card);
        QTest::mouseClick(card, Qt::LeftButton);
        QWidget* up{WithLabel(page, "outcomeRow", "Higher (toward 100)")};
        QVERIFY(up);
        QTest::mouseClick(up, Qt::LeftButton);
        input_mode->setCurrentIndex(1);
        amount->setText("2");
        QVERIFY(order->isEnabled() && order->text().startsWith("Buy for"));
    }};
    order_on_scale();
    const int was_at{index_of(on_scale_id)};
    QVERIFY(was_at >= 0);
    int added{0};
    ConfirmNextAfter([&] {
        // Markets opened until one comes before it in the list, each in a block of its own; the page shows them.
        for (; added < 8 && index_of(on_scale_id) == was_at; ++added) {
            QVERIFY(rpc("createmarket", NodeRpc::Args({strprintf("Opened meanwhile %d", added), one, 1}), true));
            QVERIFY(chain.Mine(miner));
        }
        Settle(page);
    });
    QTest::mouseClick(order, Qt::LeftButton);
    QVERIFY(added > 0);
    QVERIFY2(status->text().startsWith("Bought 2"), qPrintable(status->text()));
    QVERIFY(chain.Mine(miner));
    Settle(page);
    QCOMPARE(shares_in(on_scale_id), std::make_pair(2.0, 0));
    back->click();

    // One that stops trading meanwhile: the order is not placed.
    order_on_scale();
    ConfirmNextAfter([&] {
        mine_until([&](int height) { return height / 6 == event + 1 && height % 6 >= 1 && height % 6 < 4; });
        Settle(page);
    });
    QTest::mouseClick(order, Qt::LeftButton);
    QVERIFY2(status->text().startsWith("The market changed while the order was being confirmed"), qPrintable(status->text()));
    QVERIFY(chain.Mine(miner));
    Settle(page);
    QCOMPARE(shares_in(on_scale_id), std::make_pair(2.0, 0));
    back->click();

    // The vote: a wallet without reputation cannot seal one; on a scale, the answer is a number.
    mine_until([&](int height) { return height / 6 == event + 1 && height % 6 >= 1 && height % 6 < 4; });
    Settle(page);
    tabs->setCurrentIndex(3);
    QWidget* ballot{WithLabel(page, "ballotCard", scaled_question)};
    QVERIFY(ballot);
    QAbstractButton* is_number{Answer(ballot, "It is")};
    QVERIFY(is_number);
    auto* number{ballot->findChild<QSpinBox*>()};
    QVERIFY(number);
    QCOMPARE(number->minimum(), 0);
    QCOMPARE(number->maximum(), 100);
    is_number->click();
    number->setValue(42);
    ballot->findChild<QPushButton*>("sealButton")->click();
    QCOMPARE(status->text(), QString{"This wallet holds no reputation, so it cannot vote."});

    const std::string descriptor{strprintf("wpkh(%s)", GENESIS_VOTECOIN_KEY)};
    const auto descriptor_info{rpc("getdescriptorinfo", NodeRpc::Args({descriptor}), false)};
    QVERIFY(descriptor_info);
    UniValue import{UniValue::VOBJ};
    import.pushKV("desc", descriptor + "#" + (*descriptor_info)["checksum"].get_str());
    import.pushKV("timestamp", "now");
    UniValue imports{UniValue::VARR};
    imports.push_back(import);
    const auto imported{rpc("importdescriptors", NodeRpc::Args({imports}), true)};
    QVERIFY(imported && (*imported)[0]["success"].get_bool());
    Settle(page);
    ballot = WithLabel(page, "ballotCard", scaled_question);
    QVERIFY(ballot);
    Answer(ballot, "It is")->click();
    ballot->findChild<QSpinBox*>()->setValue(42);
    ballot->findChild<QPushButton*>("sealButton")->click();
    QVERIFY2(status->text().startsWith(QStringLiteral("Sealed the vote on \"%1\"").arg(scaled_question)), qPrintable(status->text()));
    QVERIFY(chain.Mine(miner));
    Settle(page);
    // The number sealed is kept, and filled in again.
    ballot = WithLabel(page, "ballotCard", scaled_question);
    QVERIFY(ballot);
    QVERIFY(Answer(ballot, "It is")->isChecked());
    QCOMPARE(ballot->findChild<QSpinBox*>()->value(), 42);

    // The market shown goes with the blocks that a reorg takes back: the page goes back to the list.
    tabs->setCurrentIndex(0);
    // Trading in it is closed by now: shown with the markets that are no longer traded.
    page.findChild<QCheckBox*>("showResolved")->setChecked(true);
    card = WithLabel(page, "marketCard", "On a scale");
    QVERIFY(card);
    QTest::mouseClick(card, Qt::LeftButton);
    QVERIFY(back->isVisible());
    const std::string opened{WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain()[2]->GetBlockHash().GetHex())};
    QVERIFY(rpc("invalidateblock", NodeRpc::Args({opened}), false));
    Settle(page);
    QVERIFY(!back->isVisible());
    QVERIFY2(summary->text().contains("0 markets, 0 decisions"), qPrintable(summary->text()));
    page.hide();
}

/** The page where periods are counted in mainchain time, without a wallet. */
void TestTimedMarketsPage(interfaces::Node& node)
{
    const qint64 day{86400};
    TestingSetup test{ChainType::REGTEST, {.extra_args = {"-sidechainslot=9", "-hivemindperiodseconds=86400"}}};
    node.setContext(&test.m_node);
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    // A mainchain whose last block is of now, so that the periods are those of today.
    SideChain chain{test};
    chain.start_time = QDateTime::currentSecsSinceEpoch() - 1200;
    QVERIFY(chain.AppendMain(std::nullopt));
    QVERIFY(chain.Mine(CScript() << OP_TRUE));

    OptionsModel options_model(node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    ClientModel client_model(node, &options_model);
    NodeRpc::Restart();
    struct StopRpc {
        ~StopRpc() { NodeRpc::Stop(); }
    } stop_rpc;
    MarketsDialog page([] { return std::optional<QString>{}; });
    page.resize(1100, 750);
    page.setClientModel(&client_model);
    page.show();
    Settle(page);
    const int period{int((chain.start_time + 2 * 600) / day)};
    auto* summary{page.findChild<QLabel*>("marketsSummary")};
    QVERIFY2(summary->text().startsWith(QStringLiteral("Period %1, about ").arg(period)), qPrintable(summary->text()));
    QVERIFY2(summary->text().contains("0 markets, 0 decisions"), qPrintable(summary->text()));
    auto* tabs{page.findChild<QTabWidget*>("marketsTabs")};

    // The periods, and how long is left of this one.
    tabs->setCurrentIndex(5);
    QVERIFY2(page.findChild<QLabel*>("periodFacts")->text().contains("Counted in the time of the mainchain"), qPrintable(page.findChild<QLabel*>("periodFacts")->text()));
    QVERIFY(page.findChild<QLabel*>("periodFacts")->text().contains("a period lasts 24 h 0 min"));
    QTest::qWait(1100);
    bool counting{false};
    for (const QLabel* label : page.findChildren<QLabel*>("heading")) counting |= label->text().contains(QStringLiteral("left in period %1").arg(period));
    QVERIFY(counting);

    // When the answer of a decision becomes known: so long from now, or on a date, in units of the calendar.
    tabs->setCurrentIndex(2);
    auto* unit{page.findChild<QComboBox*>("eventUnit")};
    auto* amount{page.findChild<QSpinBox*>("eventAmount")};
    auto* date{page.findChild<QDateTimeEdit*>("eventDate")};
    auto* vote_unit{page.findChild<QComboBox*>("voteUnit")};
    auto* vote_periods{page.findChild<QSpinBox*>("decisionPeriods")};
    QCOMPARE(unit->count(), 6);
    QVERIFY(date->isVisible());
    // Three periods ahead to start with: in days.
    QCOMPARE(unit->currentText(), QString{"days"});
    QCOMPARE(amount->value(), 3);
    QCOMPARE(vote_unit->currentText(), QString{"days"});
    QCOMPARE(vote_periods->value(), 1);
    QLabel* note{nullptr};
    for (QLabel* label : page.findChildren<QLabel*>()) {
        if (label->text().startsWith("The answer is known in period")) note = label;
    }
    QVERIFY(note);
    QVERIFY2(note->text().contains(QStringLiteral("period %1").arg(period + 3)), qPrintable(note->text()));
    // A date sets the amount.
    date->setDateTime(QDateTime::currentDateTime().addSecs(30 * day));
    QCOMPARE(amount->value(), 30);
    QVERIFY2(note->text().contains("weeks"), qPrintable(note->text()));
    // Too far: at most so many periods, in years.
    unit->setCurrentIndex(5);
    amount->setValue(20);
    QVERIFY2(note->text().startsWith("That is too far ahead: the answer can be at most 3650 periods away, which is about 10.0 years"), qPrintable(note->text()));
    amount->setValue(1);
    // Too long a vote: at most 90 periods, three months.
    vote_unit->setCurrentIndex(2);
    vote_periods->setValue(100);
    QVERIFY2(note->text().startsWith("That is too long a vote: it can last at most 90 periods, which is about 3.0 months"), qPrintable(note->text()));
    vote_unit->setCurrentIndex(1);
    vote_periods->setValue(30);
    QVERIFY2(note->text().contains("The vote then takes 2 period"), qPrintable(note->text()));
    vote_periods->setValue(1);
    // Without a wallet, nothing can be listed.
    page.findChild<QLineEdit*>("decisionText")->setText("Listed without a wallet?");
    page.findChild<QPushButton*>("listDecisionButton")->click();
    QCOMPARE(page.findChild<QLabel*>("marketsStatus")->text(), QString{"Open or create a wallet first."});

    // Nothing to show yet.
    tabs->setCurrentIndex(0);
    QCOMPARE(Cards(page).size(), 0);
    tabs->setCurrentIndex(3);
    QVERIFY(page.findChild<QLabel*>("voterStats")->text().contains("this wallet holds no reputation"));
    page.hide();
}

/** The page on a chain that is no sidechain. */
void TestNoMarketsPage(interfaces::Node& node)
{
    TestingSetup test{ChainType::REGTEST};
    node.setContext(&test.m_node);
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    OptionsModel options_model(node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    ClientModel client_model(node, &options_model);
    NodeRpc::Restart();
    struct StopRpc {
        ~StopRpc() { NodeRpc::Stop(); }
    } stop_rpc;
    MarketsDialog page([] { return std::optional<QString>{}; });
    page.setClientModel(&client_model);
    Settle(page);
    QCOMPARE(page.findChild<QLabel*>("marketsSummary")->text(), QString{"This chain is not running as a sidechain, so it has no markets."});
    QVERIFY(!page.findChild<QTabWidget*>("marketsTabs")->isEnabled());
}

} // namespace

void MarketsTests::marketsTests()
{
    TestMarketsPage(m_node);
}

void MarketsTests::scaledMarketsTests()
{
    TestScaledMarketsPage(m_node);
}

void MarketsTests::timedMarketsTests()
{
    TestTimedMarketsPage(m_node);
}

void MarketsTests::noMarketsTests()
{
    TestNoMarketsPage(m_node);
}

void MarketsTests::nodeRpcTests()
{
    TestNodeRpc(m_node);
}
