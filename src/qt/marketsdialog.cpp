// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/marketsdialog.h>

#include <qt/clientmodel.h>
#include <qt/guiutil.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>

#include <map>
#include <set>

namespace {
using NodeRpc::Args;
using NodeRpc::Item;
using NodeRpc::Text;

QTableWidget* Table(const QStringList& headers, QWidget* parent, const char* name)
{
    auto* table{new QTableWidget(0, headers.size(), parent)};
    table->setObjectName(name);
    table->setHorizontalHeaderLabels(headers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    return table;
}

QString Amount(const UniValue& value) { return QString::number(value.get_real(), 'f', 8); }
QString Percent(const UniValue& value) { return QString::number(value.get_real() * 100, 'f', 1) + QStringLiteral(" %"); }

/** Select the row that has `text` in `column` again after the table was filled anew. */
void Reselect(QTableWidget* table, int column, const QString& text)
{
    for (int row{0}; row < table->rowCount(); ++row) {
        if (table->item(row, column)->text() == text) {
            table->selectRow(row);
            return;
        }
    }
}
} // namespace

MarketsDialog::MarketsDialog(NodeRpc::WalletNameFn wallet_name, QWidget* parent)
    : QDialog(parent, GUIUtil::dialog_flags | Qt::WindowMinMaxButtonsHint), m_wallet_name{std::move(wallet_name)}
{
    setWindowTitle(tr("Markets"));
    auto* layout{new QVBoxLayout(this)};
    auto* top{new QHBoxLayout};
    m_summary = new QLabel(this);
    m_summary->setObjectName("marketsSummary");
    m_summary->setWordWrap(true);
    auto* refresh_button{new QPushButton(tr("Refresh"), this)};
    top->addWidget(m_summary, 1);
    top->addWidget(refresh_button);
    layout->addLayout(top);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName("marketsTabs");
    m_tabs->addTab(createMarketsTab(), tr("&Markets"));
    m_tabs->addTab(createDecisionsTab(), tr("&Decisions"));
    m_tabs->addTab(createVoteTab(), tr("&Vote"));
    m_tabs->addTab(createReputationTab(), tr("&Reputation"));
    layout->addWidget(m_tabs, 1);

    connect(refresh_button, &QPushButton::clicked, this, &MarketsDialog::refresh);
    resize(1040, 720);
    GUIUtil::handleCloseWindowShortcut(this);
}

QWidget* MarketsDialog::createMarketsTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};
    auto* splitter{new QSplitter(Qt::Vertical, tab)};

    m_markets = Table({tr("Market"), tr("Status"), tr("Chances"), tr("Liquidity"), tr("Fee"), tr("Id")}, splitter, "markets");
    splitter->addWidget(m_markets);

    auto* lower{new QWidget(splitter)};
    auto* lower_layout{new QHBoxLayout(lower)};
    lower_layout->setContentsMargins(0, 0, 0, 0);

    auto* trade{new QGroupBox(tr("Trade in the selected market"), lower)};
    auto* trade_layout{new QVBoxLayout(trade)};
    m_states = Table({tr("State"), tr("Answers"), tr("Chance"), tr("Shares sold")}, trade, "marketStates");
    trade_layout->addWidget(m_states, 1);
    auto* row{new QHBoxLayout};
    m_shares = new QLineEdit(trade);
    m_shares->setObjectName("tradeShares");
    m_shares->setPlaceholderText(tr("Number of shares"));
    auto* quote_button{new QPushButton(tr("Quote"), trade)};
    auto* buy_button{new QPushButton(tr("Buy"), trade)};
    buy_button->setObjectName("buyButton");
    row->addWidget(m_shares, 1);
    row->addWidget(quote_button);
    row->addWidget(buy_button);
    trade_layout->addLayout(row);
    m_quote = new QLabel(tr("A share of a state pays one coin if the state comes true."), trade);
    m_quote->setObjectName("tradeQuote");
    m_quote->setWordWrap(true);
    trade_layout->addWidget(m_quote);
    lower_layout->addWidget(trade, 3);

    auto* mine{new QGroupBox(tr("Shares of this wallet"), lower)};
    auto* mine_layout{new QVBoxLayout(mine)};
    m_positions = Table({tr("Market"), tr("State"), tr("Shares"), tr("Account"), tr("Market id")}, mine, "myPositions");
    mine_layout->addWidget(m_positions, 1);
    auto* sell_button{new QPushButton(tr("Sell the selected shares"), mine)};
    sell_button->setObjectName("sellButton");
    mine_layout->addWidget(sell_button, 0, Qt::AlignLeft);
    lower_layout->addWidget(mine, 2);
    splitter->addWidget(lower);
    layout->addWidget(splitter, 1);

    auto* open{new QGroupBox(tr("Open a market on the decisions ticked on the Decisions tab"), tab)};
    auto* open_layout{new QHBoxLayout(open)};
    m_market_text = new QLineEdit(open);
    m_market_text->setObjectName("marketText");
    m_market_text->setPlaceholderText(tr("What the market is about"));
    m_market_liquidity = new QLineEdit(QStringLiteral("10"), open);
    m_market_liquidity->setObjectName("marketLiquidity");
    m_market_liquidity->setToolTip(tr("The larger, the less a trade moves the price. You fund the market maker with liquidity × ln(states) and get back what it has left."));
    m_market_liquidity->setMaximumWidth(110);
    m_market_fee = new QSpinBox(open);
    m_market_fee->setRange(0, 1000);
    m_market_fee->setValue(50);
    m_market_fee->setSuffix(tr(" bps fee"));
    auto* open_button{new QPushButton(tr("Open market"), open)};
    open_button->setObjectName("openMarketButton");
    open_layout->addWidget(m_market_text, 1);
    open_layout->addWidget(new QLabel(tr("Liquidity:"), open));
    open_layout->addWidget(m_market_liquidity);
    open_layout->addWidget(m_market_fee);
    open_layout->addWidget(open_button);
    layout->addWidget(open);

    connect(m_markets, &QTableWidget::itemSelectionChanged, this, [this] {
        const QString id{selected(m_markets, 5)};
        m_states->setRowCount(0);
        if (id.isEmpty()) return;
        const auto market{call("getmarket", Args({id.toStdString()}), false, true)};
        if (!market) return;
        const auto& states{(*market)["states"].getValues()};
        m_states->setRowCount(states.size());
        for (size_t row{0}; row < states.size(); ++row) {
            m_states->setItem(row, 0, Item(Text(states[row]["state"])));
            m_states->setItem(row, 1, Item(Text(states[row]["answers"])));
            m_states->setItem(row, 2, Item(states[row].exists("price") ? Percent(states[row]["price"]) : QStringLiteral("-")));
            m_states->setItem(row, 3, Item(Amount(states[row]["shares"])));
        }
        m_states->resizeColumnsToContents();
    });
    connect(quote_button, &QPushButton::clicked, this, &MarketsDialog::quote);
    connect(buy_button, &QPushButton::clicked, this, &MarketsDialog::buy);
    connect(sell_button, &QPushButton::clicked, this, &MarketsDialog::sell);
    connect(open_button, &QPushButton::clicked, this, &MarketsDialog::createMarket);
    return tab;
}

QWidget* MarketsDialog::createDecisionsTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};
    m_decisions = Table({tr("Use"), tr("Question"), tr("Status"), tr("Answer"), tr("Event period"), tr("Vote ends"), tr("Scale"), tr("Id")}, tab, "decisions");
    layout->addWidget(m_decisions, 1);

    auto* list{new QGroupBox(tr("List a decision"), tab)};
    auto* form{new QFormLayout(list)};
    m_decision_text = new QLineEdit(list);
    m_decision_text->setObjectName("decisionText");
    m_decision_text->setPlaceholderText(tr("A question whose answer will be beyond dispute once known"));
    form->addRow(tr("Question:"), m_decision_text);
    auto* row{new QHBoxLayout};
    m_decision_event = new QSpinBox(list);
    m_decision_event->setObjectName("decisionEvent");
    m_decision_event->setRange(0, 100000000);
    m_decision_event->setToolTip(tr("The voting period in which the answer becomes known. The vote starts in the period after."));
    m_decision_periods = new QSpinBox(list);
    m_decision_periods->setRange(1, 90);
    m_decision_periods->setToolTip(tr("For how many periods the decision is voted on. Longer for answers that take time to establish."));
    m_decision_scaled = new QCheckBox(tr("on a scale from"), list);
    m_decision_min = new QLineEdit(QStringLiteral("0"), list);
    m_decision_max = new QLineEdit(QStringLiteral("100"), list);
    m_decision_min->setMaximumWidth(90);
    m_decision_max->setMaximumWidth(90);
    row->addWidget(new QLabel(tr("Event in period:"), list));
    row->addWidget(m_decision_event);
    row->addWidget(new QLabel(tr("Vote for periods:"), list));
    row->addWidget(m_decision_periods);
    row->addWidget(m_decision_scaled);
    row->addWidget(m_decision_min);
    row->addWidget(new QLabel(tr("to"), list));
    row->addWidget(m_decision_max);
    row->addStretch();
    auto* list_button{new QPushButton(tr("List decision"), list)};
    list_button->setObjectName("listDecisionButton");
    row->addWidget(list_button);
    form->addRow(row);
    layout->addWidget(list);

    connect(list_button, &QPushButton::clicked, this, &MarketsDialog::createDecision);
    return tab;
}

QWidget* MarketsDialog::createVoteTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};
    auto* intro{new QLabel(tr("Holders of reputation vote on the decisions whose event has passed. A vote is sealed first, so that nobody can "
                              "copy it, and shown in the last third of the last period of the vote. A vote that is not shown does not count. "
                              "Voting with the others earns reputation; voting against them, or not voting, loses it."), tab)};
    intro->setWordWrap(true);
    layout->addWidget(intro);
    m_voting = Table({tr("Question"), tr("Vote ends in period"), tr("Scale"), tr("Votes"), tr("Id")}, tab, "votingDecisions");
    layout->addWidget(m_voting, 1);
    auto* row{new QHBoxLayout};
    m_voter = new QComboBox(tab);
    m_voter->setObjectName("voter");
    m_voter->setToolTip(tr("The accounts of this wallet that hold reputation."));
    m_answer = new QLineEdit(tab);
    m_answer->setObjectName("voteAnswer");
    m_answer->setPlaceholderText(tr("yes, no, a number on the scale, or unknown"));
    auto* seal_button{new QPushButton(tr("Seal vote"), tab)};
    seal_button->setObjectName("sealButton");
    auto* reveal_button{new QPushButton(tr("Reveal vote"), tab)};
    reveal_button->setObjectName("revealButton");
    row->addWidget(new QLabel(tr("Vote as:"), tab));
    row->addWidget(m_voter, 1);
    row->addWidget(m_answer, 1);
    row->addWidget(seal_button);
    row->addWidget(reveal_button);
    layout->addLayout(row);

    connect(seal_button, &QPushButton::clicked, this, &MarketsDialog::sealVote);
    connect(reveal_button, &QPushButton::clicked, this, &MarketsDialog::revealVote);
    return tab;
}

QWidget* MarketsDialog::createReputationTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};
    m_reputation = Table({tr("Account"), tr("Reputation"), tr("Share"), tr("Of this wallet")}, tab, "reputation");
    layout->addWidget(m_reputation, 1);
    auto* row{new QHBoxLayout};
    m_transfer_to = new QLineEdit(tab);
    m_transfer_to->setPlaceholderText(tr("Address of the account to move reputation to"));
    m_transfer_amount = new QLineEdit(tab);
    m_transfer_amount->setPlaceholderText(tr("Amount"));
    m_transfer_amount->setMaximumWidth(140);
    auto* transfer_button{new QPushButton(tr("Move from the selected account"), tab)};
    row->addWidget(m_transfer_to, 1);
    row->addWidget(m_transfer_amount);
    row->addWidget(transfer_button);
    layout->addLayout(row);
    connect(transfer_button, &QPushButton::clicked, this, &MarketsDialog::transfer);
    return tab;
}

void MarketsDialog::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
    if (client_model) {
        connect(client_model, &ClientModel::numBlocksChanged, this, [this] { if (isVisible()) refresh(); });
    }
}

void MarketsDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    refresh();
}

std::optional<UniValue> MarketsDialog::call(const std::string& method, const UniValue& params, bool wallet, bool quiet)
{
    QString error;
    std::optional<QString> wallet_name;
    if (wallet) {
        wallet_name = m_wallet_name();
        if (!wallet_name) {
            if (!quiet) QMessageBox::information(this, windowTitle(), tr("Open or create a wallet first."));
            return std::nullopt;
        }
    }
    auto result{NodeRpc::Call(m_client_model, method, params, error, wallet_name)};
    if (!result && !quiet) QMessageBox::warning(this, windowTitle(), error);
    return result;
}

QString MarketsDialog::selected(const QTableWidget* table, int column)
{
    const int row{table->currentRow()};
    return row < 0 || !table->item(row, column) ? QString{} : table->item(row, column)->text();
}

void MarketsDialog::done(const QString& what, const UniValue& result)
{
    QMessageBox::information(this, windowTitle(), tr("%1 It is in transaction %2 and takes effect when that is mined.").arg(what, Text(result["txid"])));
    refresh();
}

void MarketsDialog::quote()
{
    const QString market{selected(m_markets, 5)};
    const QString state{selected(m_states, 0)};
    if (market.isEmpty() || state.isEmpty() || m_shares->text().trimmed().isEmpty()) {
        m_quote->setText(tr("Select a market and a state, and enter a number of shares."));
        return;
    }
    const auto cost{call("quotetrade", Args({market.toStdString(), state.toInt(), m_shares->text().trimmed().toStdString()}))};
    if (cost) m_quote->setText(tr("%1 shares of state %2 cost %3 now, fee included, and pay %1 if the state comes true.").arg(m_shares->text().trimmed(), state, Amount(*cost)));
}

void MarketsDialog::buy()
{
    const QString market{selected(m_markets, 5)};
    const QString state{selected(m_states, 0)};
    const QString shares{m_shares->text().trimmed()};
    if (market.isEmpty() || state.isEmpty() || shares.isEmpty()) {
        QMessageBox::information(this, windowTitle(), tr("Select a market and a state, and enter a number of shares."));
        return;
    }
    const auto cost{call("quotetrade", Args({market.toStdString(), state.toInt(), shares.toStdString()}))};
    if (!cost) return;
    if (QMessageBox::question(this, windowTitle(), tr("Buy %1 shares of state %2 for about %3?").arg(shares, state, Amount(*cost))) != QMessageBox::Yes) return;
    if (const auto result{call("buyshares", Args({market.toStdString(), state.toInt(), shares.toStdString()}), /*wallet=*/true)}) {
        done(tr("The shares will be held by the account %1.").arg(Text((*result)["account"])), *result);
    }
}

void MarketsDialog::sell()
{
    const int row{m_positions->currentRow()};
    if (row < 0) {
        QMessageBox::information(this, windowTitle(), tr("Select shares of this wallet first."));
        return;
    }
    const QString market{m_positions->item(row, 4)->text()};
    const int state{m_positions->item(row, 1)->text().toInt()};
    const QString shares{m_positions->item(row, 2)->text()};
    const QString account{m_positions->item(row, 3)->text()};
    if (const auto result{call("sellshares", Args({market.toStdString(), state, shares.toStdString(), account.toStdString()}), /*wallet=*/true)}) {
        done(tr("Sold for about %1.").arg(Amount((*result)["quote"])), *result);
    }
}

void MarketsDialog::createMarket()
{
    UniValue decisions(UniValue::VARR);
    for (int row{0}; row < m_decisions->rowCount(); ++row) {
        if (m_decisions->item(row, 0)->checkState() == Qt::Checked) decisions.push_back(m_decisions->item(row, 7)->text().toStdString());
    }
    if (decisions.empty() || m_market_text->text().trimmed().isEmpty()) {
        QMessageBox::information(this, windowTitle(), tr("Tick the decisions the market is about on the Decisions tab, and say what the market is about."));
        return;
    }
    if (const auto result{call("createmarket", Args({m_market_text->text().trimmed().toStdString(), decisions, m_market_liquidity->text().trimmed().toStdString(), m_market_fee->value()}), /*wallet=*/true)}) {
        m_market_text->clear();
        done(tr("The market maker was funded with %1.").arg(Amount((*result)["funding"])), *result);
    }
}

void MarketsDialog::createDecision()
{
    if (m_decision_text->text().trimmed().isEmpty()) {
        QMessageBox::information(this, windowTitle(), tr("Enter the question."));
        return;
    }
    UniValue params{Args({m_decision_text->text().trimmed().toStdString(), m_decision_event->value(), m_decision_periods->value(), m_decision_scaled->isChecked()})};
    if (m_decision_scaled->isChecked()) {
        params.push_back(static_cast<int64_t>(m_decision_min->text().toLongLong()));
        params.push_back(static_cast<int64_t>(m_decision_max->text().toLongLong()));
    }
    if (const auto result{call("createdecision", params, /*wallet=*/true)}) {
        m_decision_text->clear();
        done(tr("The decision is listed."), *result);
    }
}

void MarketsDialog::sealVote()
{
    const QString decision{selected(m_voting, 4)};
    if (decision.isEmpty() || m_voter->currentText().isEmpty() || m_answer->text().trimmed().isEmpty()) {
        QMessageBox::information(this, windowTitle(), tr("Select a decision, an account that holds reputation, and enter the answer."));
        return;
    }
    if (const auto result{call("sealvote", Args({m_voter->currentText().toStdString(), decision.toStdString(), m_answer->text().trimmed().toStdString()}), /*wallet=*/true)}) {
        done(tr("The vote is sealed. Remember the answer: it has to be given again to reveal the vote."), *result);
    }
}

void MarketsDialog::revealVote()
{
    const QString decision{selected(m_voting, 4)};
    if (decision.isEmpty() || m_voter->currentText().isEmpty() || m_answer->text().trimmed().isEmpty()) {
        QMessageBox::information(this, windowTitle(), tr("Select a decision, the account that sealed the vote, and enter the answer that was sealed."));
        return;
    }
    if (const auto result{call("revealvote", Args({m_voter->currentText().toStdString(), decision.toStdString(), m_answer->text().trimmed().toStdString()}), /*wallet=*/true)}) {
        done(tr("The vote is revealed, if this is the answer that was sealed and the time to reveal has come."), *result);
    }
}

void MarketsDialog::transfer()
{
    const QString from{selected(m_reputation, 0)};
    if (from.isEmpty() || m_transfer_to->text().trimmed().isEmpty() || m_transfer_amount->text().trimmed().isEmpty()) {
        QMessageBox::information(this, windowTitle(), tr("Select an account of this wallet, and enter where to move reputation and how much."));
        return;
    }
    if (const auto result{call("transfervotecoin", Args({from.toStdString(), m_transfer_to->text().trimmed().toStdString(), m_transfer_amount->text().trimmed().toStdString()}), /*wallet=*/true)}) {
        m_transfer_amount->clear();
        done(tr("The reputation is on its way."), *result);
    }
}

void MarketsDialog::refresh()
{
    const auto info{call("gethivemindinfo", Args({}), false, true)};
    if (!info) {
        m_summary->setText(tr("This chain is not running as a sidechain, so it has no markets."));
        m_tabs->setEnabled(false);
        return;
    }
    m_tabs->setEnabled(true);
    const QString length{info->exists("periodblocks") ? tr("%1 blocks").arg(Text((*info)["periodblocks"])) : GUIUtil::formatDurationStr(std::chrono::seconds{(*info)["periodseconds"].getInt<int64_t>()})};
    m_summary->setText(tr("Voting period %1 (a period lasts %2). Now: %3. Listing a decision costs %4.")
                           .arg(Text((*info)["period"]), length,
                                Text((*info)["phase"]) == "reveal" ? tr("votes that end this period are revealed") : tr("votes are sealed"),
                                Amount((*info)["listingfee"])));
    if (m_decision_event->value() < (*info)["period"].getInt<int>()) m_decision_event->setValue((*info)["period"].getInt<int>() + 1);
    m_decision_periods->setMaximum((*info)["maxvoteperiods"].getInt<int>());

    // Which accounts are of this wallet.
    const auto wallet{m_wallet_name()};
    const auto is_mine{[&](const QString& address) {
        if (!wallet) return false;
        QString error;
        const auto address_info{NodeRpc::Call(m_client_model, "getaddressinfo", Args({address.toStdString()}), error, wallet)};
        return address_info && (*address_info)["ismine"].get_bool();
    }};

    std::map<QString, QString> market_names;
    if (const auto markets{call("listmarkets", Args({}), false, true)}) {
        const QString keep{selected(m_markets, 5)};
        m_markets->setRowCount(markets->size());
        for (size_t row{0}; row < markets->size(); ++row) {
            const UniValue& market{(*markets)[row]};
            QStringList chances;
            for (const UniValue& state : market["states"].getValues()) {
                if (state.exists("price")) chances << QStringLiteral("%1 %2").arg(Text(state["answers"]), Percent(state["price"]));
            }
            const QString status{Text(market["status"])};
            m_markets->setItem(row, 0, Item(Text(market["text"])));
            m_markets->setItem(row, 1, Item(status == "trading" ? tr("Trading") : status == "closed" ? tr("Waiting for the vote") : tr("Settled")));
            m_markets->setItem(row, 2, Item(chances.join(QStringLiteral("   "))));
            m_markets->setItem(row, 3, Item(Amount(market["liquidity"])));
            m_markets->setItem(row, 4, Item(QString::number(market["feebps"].getInt<int>() / 100.0, 'f', 2) + QStringLiteral(" %")));
            m_markets->setItem(row, 5, Item(Text(market["id"])));
            market_names[Text(market["id"])] = Text(market["text"]);
        }
        m_markets->resizeColumnsToContents();
        Reselect(m_markets, 5, keep);

        // The shares of this wallet, from the positions of each market that is not settled.
        m_positions->setRowCount(0);
        for (const UniValue& market : markets->getValues()) {
            if (Text(market["status"]) == "settled") continue;
            const auto detail{call("getmarket", Args({market["id"]}), false, true)};
            if (!detail) continue;
            for (const UniValue& position : (*detail)["positions"].getValues()) {
                const QString account{Text(position["account"])};
                if (!is_mine(account)) continue;
                const int row{m_positions->rowCount()};
                m_positions->insertRow(row);
                m_positions->setItem(row, 0, Item(Text(market["text"])));
                m_positions->setItem(row, 1, Item(Text(position["state"])));
                m_positions->setItem(row, 2, Item(Amount(position["shares"])));
                m_positions->setItem(row, 3, Item(account));
                m_positions->setItem(row, 4, Item(Text(market["id"])));
            }
        }
        m_positions->resizeColumnsToContents();
    }

    if (const auto decisions{call("listdecisions", Args({}), false, true)}) {
        std::set<QString> ticked;
        for (int row{0}; row < m_decisions->rowCount(); ++row) {
            if (m_decisions->item(row, 0)->checkState() == Qt::Checked) ticked.insert(m_decisions->item(row, 7)->text());
        }
        const QString keep{selected(m_voting, 4)};
        m_decisions->setRowCount(decisions->size());
        m_voting->setRowCount(0);
        for (size_t row{0}; row < decisions->size(); ++row) {
            const UniValue& decision{(*decisions)[row]};
            const QString status{Text(decision["status"])};
            const QString scale{decision["scaled"].get_bool() ? tr("%1 to %2").arg(Text(decision["min"]), Text(decision["max"])) : tr("yes or no")};
            auto* use{Item(QString{})};
            use->setFlags(use->flags() | Qt::ItemIsUserCheckable);
            use->setCheckState(ticked.contains(Text(decision["id"])) ? Qt::Checked : Qt::Unchecked);
            m_decisions->setItem(row, 0, use);
            m_decisions->setItem(row, 1, Item(Text(decision["text"])));
            m_decisions->setItem(row, 2, Item(status == "open" ? tr("Open") : status == "voting" ? tr("Being voted on") : tr("Resolved")));
            m_decisions->setItem(row, 3, Item(decision.exists("answer") ? Text(decision["answer"]) : QString{}));
            m_decisions->setItem(row, 4, Item(Text(decision["eventperiod"])));
            m_decisions->setItem(row, 5, Item(Text(decision["closeperiod"])));
            m_decisions->setItem(row, 6, Item(scale));
            m_decisions->setItem(row, 7, Item(Text(decision["id"])));
            if (status != "voting") continue;
            const auto detail{call("getdecision", Args({decision["id"]}), false, true)};
            const int vote_row{m_voting->rowCount()};
            m_voting->insertRow(vote_row);
            m_voting->setItem(vote_row, 0, Item(Text(decision["text"])));
            m_voting->setItem(vote_row, 1, Item(Text(decision["closeperiod"])));
            m_voting->setItem(vote_row, 2, Item(scale));
            m_voting->setItem(vote_row, 3, Item(detail ? QString::number((*detail)["votes"].size()) : QString{}));
            m_voting->setItem(vote_row, 4, Item(Text(decision["id"])));
        }
        m_decisions->resizeColumnsToContents();
        m_voting->resizeColumnsToContents();
        Reselect(m_voting, 4, keep);
    }

    if (const auto reputation{call("listreputation", Args({}), false, true)}) {
        const QString keep{selected(m_reputation, 0)};
        const QString voter{m_voter->currentText()};
        double total{0};
        for (const UniValue& holder : reputation->getValues()) total += holder["amount"].get_real();
        m_reputation->setRowCount(reputation->size());
        m_voter->clear();
        for (size_t row{0}; row < reputation->size(); ++row) {
            const UniValue& holder{(*reputation)[row]};
            const QString account{Text(holder["account"])};
            const bool mine{is_mine(account)};
            m_reputation->setItem(row, 0, Item(account));
            m_reputation->setItem(row, 1, Item(Amount(holder["amount"])));
            m_reputation->setItem(row, 2, Item(total > 0 ? QString::number(holder["amount"].get_real() * 100 / total, 'f', 2) + QStringLiteral(" %") : QString{}));
            m_reputation->setItem(row, 3, Item(mine ? tr("yes") : QString{}));
            if (mine) m_voter->addItem(account);
        }
        m_reputation->resizeColumnsToContents();
        Reselect(m_reputation, 0, keep);
        if (!voter.isEmpty()) m_voter->setCurrentIndex(std::max(0, m_voter->findText(voter)));
    }
}
