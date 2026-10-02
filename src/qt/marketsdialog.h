// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MARKETSDIALOG_H
#define BITCOIN_QT_MARKETSDIALOG_H

#include <qt/noderpc.h>

#include <QDialog>

class ClientModel;

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableWidget;
class QTabWidget;
QT_END_NAMESPACE

/**
 * Window for the prediction markets: the markets and trading in them, the
 * decisions they are about, voting on those, and the reputation that votes.
 *
 * The window works through the RPC commands of the node and of the wallet.
 */
class MarketsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MarketsDialog(NodeRpc::WalletNameFn wallet_name, QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model);

public Q_SLOTS:
    void refresh();

protected:
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void quote();
    void buy();
    void sell();
    void createMarket();
    void createDecision();
    void sealVote();
    void revealVote();
    void transfer();

private:
    QWidget* createMarketsTab();
    QWidget* createDecisionsTab();
    QWidget* createVoteTab();
    QWidget* createReputationTab();
    /** Run a command and show its error, if any. */
    std::optional<UniValue> call(const std::string& method, const UniValue& params, bool wallet = false, bool quiet = false);
    /** Text of a column of the selected row, or an empty string. */
    static QString selected(const QTableWidget* table, int column);
    void done(const QString& what, const UniValue& result);

    ClientModel* m_client_model{nullptr};
    NodeRpc::WalletNameFn m_wallet_name;
    QLabel* m_summary;
    QTabWidget* m_tabs;

    QTableWidget* m_markets;
    QTableWidget* m_states;
    QLineEdit* m_shares;
    QLabel* m_quote;
    QTableWidget* m_positions;
    QLineEdit* m_market_text;
    QLineEdit* m_market_liquidity;
    QSpinBox* m_market_fee;

    QTableWidget* m_decisions;
    QLineEdit* m_decision_text;
    QSpinBox* m_decision_event;
    QSpinBox* m_decision_periods;
    QCheckBox* m_decision_scaled;
    QLineEdit* m_decision_min;
    QLineEdit* m_decision_max;

    QTableWidget* m_voting;
    QComboBox* m_voter;
    QLineEdit* m_answer;

    QTableWidget* m_reputation;
    QLineEdit* m_transfer_to;
    QLineEdit* m_transfer_amount;

    //! The accounts of the open wallet that hold shares or reputation.
    QStringList m_my_accounts;
};

#endif // BITCOIN_QT_MARKETSDIALOG_H
