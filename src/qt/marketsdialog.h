// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MARKETSDIALOG_H
#define BITCOIN_QT_MARKETSDIALOG_H

#include <qt/noderpc.h>

#include <QDialog>
#include <QMap>
#include <QStringList>
#include <QVector>

class ClientModel;

QT_BEGIN_NAMESPACE
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QTabWidget;
class QVBoxLayout;
QT_END_NAMESPACE

/**
 * Window for the prediction markets: browsing the markets and trading in
 * them, the shares of the wallet, listing decisions and opening markets on
 * them, voting on the decisions, and the periods all of this goes by.
 *
 * The layout follows the Truthcoin wallet of LayerTwo Labs: markets as cards,
 * a market with its outcomes as bars to pick from and a panel to trade in,
 * with a preview of every trade. The window works through the RPC commands
 * of the node and of the wallet.
 */
class MarketsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MarketsDialog(NodeRpc::WalletNameFn wallet_name, QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model);

    struct Decision {
        QString id, text, status, answer;
        bool scaled{false};
        qint64 min{0}, max{0};
        int event_period{0}, close_period{0}, height{0};
        //! Whether the wallet listed it.
        bool mine{false};
    };
    struct State {
        int index{0};
        QString answers;
        double shares{0};
        double price{0};
    };
    struct Market {
        QString id, text, status, author;
        QStringList decisions;
        double liquidity{0}, treasury{0}, fees{0};
        int fee_bps{0}, height{0}, trading_until{-1};
        QVector<State> states;
    };
    struct Position {
        QString market, account;
        int state{0};
        double shares{0};
    };

public Q_SLOTS:
    void refresh();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private Q_SLOTS:
    void showMarket(const QString& id);
    void showBrowse();
    void updatePreview();
    void placeOrder();
    void createMarket();
    void createDecision();
    void transfer();
    void tick();

private:
    QWidget* createMarketsTab();
    QWidget* createPositionsTab();
    QWidget* createCreateTab();
    QWidget* createDecisionsTab();
    QWidget* createVoteTab();
    QWidget* createPeriodsTab();

    void fillBrowse();
    void fillDetail();
    void fillPositions();
    void fillCreate();
    void fillBallot();
    void fillDecisions();
    void fillPeriods();

    /** Run a command and show its error, if any. */
    std::optional<UniValue> call(const std::string& method, const UniValue& params, bool wallet = false, bool quiet = false);
    /** What a refresh read of the node and of the wallet, off the GUI thread. */
    struct Fetched {
        std::optional<QString> wallet;
        std::optional<UniValue> info, decisions, markets, positions, reputation, own;
        //! Whether addresses that were not known yet are the wallet's, by wallet/address.
        QMap<QString, bool> mine;
        //! getdecision of the decisions being voted on, by id.
        QMap<QString, UniValue> details;
    };
    /** Show what a refresh read: on the GUI thread. */
    void apply(Fetched fetched);
    /** Whether the wallet the refresh was for has the address, as far as the window knows. */
    bool isMine(const std::optional<QString>& wallet, const QString& address) const;
    const Market* market(const QString& id) const;
    const Decision* decision(const QString& id) const;
    /** What a state of a market stands for, in words. */
    QString outcomeName(const Market& market, int state, int question_length = 40) const;
    /** How long until a period ends, in words; empty where periods are counted in blocks. */
    QString untilEndOf(int period) const;
    /** Where a market is in its life, in words. */
    QString stage(const Market& market) const;
    /** The shares of the wallet in a state of a market, and the account that holds most of them. */
    double myShares(const QString& market, int state, QString* account = nullptr) const;
    void say(const QString& text, bool error = false);
    void sent(const QString& what, const UniValue& result);
    void vote(const Decision& decision, const QString& answer, bool reveal);
    /** The period the answer of the decision being listed becomes known in, and for how many periods it is voted on. */
    int eventPeriod() const;
    int votePeriods() const;
    /** Say what the times chosen for the decision amount to. @return false if they cannot be */
    bool noteDecision();

    ClientModel* m_client_model{nullptr};
    NodeRpc::WalletNameFn m_wallet_name;
    QMap<QString, bool> m_mine;
    //! A refresh is under way, and another was asked for meanwhile.
    bool m_refreshing{false};
    bool m_refresh_again{false};

    // What the node last said.
    int m_period{0};
    bool m_reveal_phase{false};
    qint64 m_period_seconds{0}, m_start_time{0};
    int m_period_blocks{0};
    double m_listing_fee{0};
    int m_max_vote_periods{90};
    int m_max_periods_ahead{365};
    QVector<Market> m_market_list;
    QVector<Decision> m_decision_list;
    QVector<Position> m_position_list;
    QMap<QString, double> m_reputation_list;
    //! getdecision of the decisions being voted on, by id.
    QMap<QString, UniValue> m_decision_details;

    QLabel* m_summary{nullptr};
    QLabel* m_status{nullptr};
    QTabWidget* m_tabs{nullptr};

    // Markets: the cards, and one market.
    QStackedWidget* m_market_pages{nullptr};
    QLineEdit* m_search{nullptr};
    QCheckBox* m_show_resolved{nullptr};
    QComboBox* m_sort{nullptr};
    QLabel* m_count{nullptr};
    QWidget* m_cards{nullptr};
    QString m_market;
    int m_state{-1};
    QLabel* m_detail_title{nullptr};
    QLabel* m_detail_stage{nullptr};
    QLabel* m_detail_facts{nullptr};
    QLabel* m_detail_decisions{nullptr};
    QVBoxLayout* m_outcomes{nullptr};
    QPushButton* m_buy_side{nullptr};
    QPushButton* m_sell_side{nullptr};
    QLabel* m_selected_outcome{nullptr};
    QComboBox* m_input_mode{nullptr};
    QLineEdit* m_amount{nullptr};
    QWidget* m_chips{nullptr};
    QLabel* m_holding{nullptr};
    QPushButton* m_max{nullptr};
    QLabel* m_preview{nullptr};
    QPushButton* m_order{nullptr};
    QLabel* m_order_note{nullptr};
    //! The trade the preview is of: shares, negative to sell, and what it costs or brings.
    double m_trade_shares{0};
    double m_trade_total{0};

    // Positions.
    QLabel* m_positions_total{nullptr};
    QTableWidget* m_positions{nullptr};

    // Create.
    QLineEdit* m_decision_text{nullptr};
    QRadioButton* m_decision_binary{nullptr};
    QRadioButton* m_decision_scaled{nullptr};
    QLineEdit* m_decision_min{nullptr};
    QLineEdit* m_decision_max{nullptr};
    QSpinBox* m_event_amount{nullptr};
    QComboBox* m_event_unit{nullptr};
    QDateTimeEdit* m_event_date{nullptr};
    QLabel* m_event_date_label{nullptr};
    QSpinBox* m_decision_periods{nullptr};
    QComboBox* m_vote_unit{nullptr};
    QLabel* m_decision_note{nullptr};
    QLineEdit* m_market_text{nullptr};
    QListWidget* m_market_decisions{nullptr};
    QLineEdit* m_market_liquidity{nullptr};
    QDoubleSpinBox* m_market_fee{nullptr};
    QLabel* m_market_note{nullptr};

    // Decisions.
    QLineEdit* m_decision_search{nullptr};
    QComboBox* m_decision_filter{nullptr};
    QCheckBox* m_decision_mine{nullptr};
    QTableWidget* m_decisions{nullptr};
    QLabel* m_decision_count{nullptr};
    QPushButton* m_decision_prev{nullptr};
    QPushButton* m_decision_next{nullptr};
    QLabel* m_decision_page_label{nullptr};
    QComboBox* m_decision_page_size{nullptr};
    int m_decision_page{0};

    // Vote.
    QLabel* m_voter_stats{nullptr};
    QComboBox* m_voter{nullptr};
    QVBoxLayout* m_ballot{nullptr};
    QTableWidget* m_reputation{nullptr};
    QLineEdit* m_transfer_to{nullptr};
    QLineEdit* m_transfer_amount{nullptr};

    // Periods.
    QLabel* m_period_now{nullptr};
    QLabel* m_period_facts{nullptr};
    QLabel* m_countdown{nullptr};
    QProgressBar* m_period_progress{nullptr};
    QTableWidget* m_voting{nullptr};
};

#endif // BITCOIN_QT_MARKETSDIALOG_H
