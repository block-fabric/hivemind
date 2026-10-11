// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/marketsdialog.h>

#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/itemviews.h>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QAction>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QHideEvent>
#include <QShowEvent>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
using NodeRpc::Args;
using NodeRpc::Item;
using NodeRpc::Text;

// The colours of the stages of a market and of what a trade does, the same under every theme.
const char* const GREEN{"#10b981"};
const char* const ORANGE{"#f59e0b"};
const char* const BLUE{"#6495ed"};
const char* const RED{"#ef4444"};
//! The largest holders of reputation shown: the list is for a look, and all of it can be long.
constexpr int SHOWN_HOLDERS{200};

QString ColumnsKey(const QTableWidget* table) { return QStringLiteral("Markets/columns/%1").arg(table->objectName()); }

/**
 * A table whose columns the user can resize and move. What the user makes of them is kept: from
 * one refresh to the next, and from one session to the next.
 */
QTableWidget* Table(const QStringList& headers, QWidget* parent, const char* name)
{
    auto* table{new ItemViews::Table(0, headers.size(), parent)};
    table->setObjectName(name);
    table->setHorizontalHeaderLabels(headers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    table->setWordWrap(true);
    table->setTextElideMode(Qt::ElideMiddle);
    table->verticalHeader()->hide();
    QHeaderView* header{table->horizontalHeader()};
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionsMovable(true);
    header->setStretchLastSection(true);
    header->setMinimumSectionSize(40);
    const QByteArray saved{QSettings().value(ColumnsKey(table)).toByteArray()};
    if (!saved.isEmpty() && header->restoreState(saved)) table->setProperty("userSized", true);
    const auto save{[table] {
        table->setProperty("userSized", true);
        QSettings().setValue(ColumnsKey(table), table->horizontalHeader()->saveState());
    }};
    QObject::connect(header, &QHeaderView::sectionResized, table, [table, save](int, int, int) {
        if (!table->property("fitting").toBool()) save();
    });
    QObject::connect(header, &QHeaderView::sectionMoved, table, [save](int, int, int) { save(); });
    // The context menu of the headings puts the columns back as they were at first.
    header->setContextMenuPolicy(Qt::ActionsContextMenu);
    auto* reset{new QAction(QObject::tr("Fit all columns to what is in them"), header)};
    header->addAction(reset);
    QObject::connect(reset, &QAction::triggered, table, [table] {
        QSettings().remove(ColumnsKey(table));
        table->setProperty("fitting", true);
        for (int i{0}; i < table->columnCount(); ++i) table->horizontalHeader()->moveSection(table->horizontalHeader()->visualIndex(i), i);
        table->setProperty("fitting", false);
        table->setProperty("userSized", false);
        table->setProperty("fitted", false);
    });
    return table;
}

/**
 * Columns as wide as what is in them, their headings included, which are in bold: once, the first
 * time the table has rows. After that, and once the user has sized a column, widths are left alone.
 */
void FitColumns(QTableWidget* table)
{
    if (table->property("userSized").toBool() || table->property("fitted").toBool() || table->rowCount() == 0) {
        table->resizeRowsToContents();
        return;
    }
    table->setProperty("fitting", true);
    table->setProperty("fitted", true);
    table->resizeColumnsToContents();
    QFont heading{table->horizontalHeader()->font()};
    heading.setBold(true);
    const QFontMetrics metrics{heading};
    for (int column{0}; column < table->columnCount(); ++column) {
        const QTableWidgetItem* item{table->horizontalHeaderItem(column)};
        const int needed{metrics.horizontalAdvance(item ? item->text() : QString{}) + 28};
        if (table->columnWidth(column) < needed) table->setColumnWidth(column, needed);
        // A column of long text does not take the whole table: it wraps, or can be widened by hand.
        if (table->columnWidth(column) > 480) table->setColumnWidth(column, 480);
    }
    table->resizeRowsToContents();
    table->setProperty("fitting", false);
}

/** A splitter whose position is remembered. */
QSplitter* Splitter(Qt::Orientation orientation, QWidget* parent, const QString& name)
{
    auto* splitter{new QSplitter(orientation, parent)};
    splitter->setObjectName(name);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(8);
    const QString key{QStringLiteral("Markets/splitter/%1").arg(name)};
    QTimer::singleShot(0, splitter, [splitter, key] {
        const QByteArray saved{QSettings().value(key).toByteArray()};
        if (!saved.isEmpty()) splitter->restoreState(saved);
    });
    QObject::connect(splitter, &QSplitter::splitterMoved, splitter, [splitter, key](int, int) { QSettings().setValue(key, splitter->saveState()); });
    return splitter;
}

/** A widget in a scroll area, so that a narrow window or section scrolls rather than squeezes it. */
QScrollArea* Scrolled(QWidget* inner, QWidget* parent)
{
    auto* scroll{new QScrollArea(parent)};
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(inner);
    return scroll;
}

/** An amount of coins or of shares: as many decimals as it has, two at least. */
QString Coins(double value)
{
    QString text{QString::number(value, 'f', 8)};
    while (text.endsWith('0') && text.at(text.size() - 4) != '.' && text.at(text.size() - 3) != '.') text.chop(1);
    return text;
}
/** The amount as the commands take it. */
std::string Exact(double value) { return QString::number(value, 'f', 8).toStdString(); }
QString Percent(double share) { return QString::number(share * 100, 'f', 1) + QStringLiteral(" %"); }

QString Duration(qint64 seconds)
{
    if (seconds <= 0) return QObject::tr("a moment");
    if (seconds < 90) return QObject::tr("%1 s").arg(seconds);
    if (seconds < 90 * 60) return QObject::tr("%1 min").arg((seconds + 30) / 60);
    if (seconds < 36 * 3600) return QObject::tr("%1 h %2 min").arg(seconds / 3600).arg(seconds % 3600 / 60);
    if (seconds < 14 * 86400) return QObject::tr("%1 d %2 h").arg(seconds / 86400).arg(seconds % 86400 / 3600);
    if (seconds < 70 * 86400) return QObject::tr("%1 weeks").arg(QString::number(seconds / (7.0 * 86400), 'f', 1));
    if (seconds < 730 * 86400) return QObject::tr("%1 months").arg(QString::number(seconds / (30.44 * 86400), 'f', 1));
    return QObject::tr("%1 years").arg(QString::number(seconds / (365.25 * 86400), 'f', 1));
}

/** What the market maker has been paid for the shares it sold: b ln(sum of e^(q/b)). */
double LmsrCost(const std::vector<double>& shares, double liquidity)
{
    const double top{*std::max_element(shares.begin(), shares.end())};
    double sum{0};
    for (const double q : shares) sum += std::exp((q - top) / liquidity);
    return top + liquidity * std::log(sum);
}

std::vector<double> LmsrPrices(const std::vector<double>& shares, double liquidity)
{
    const double top{*std::max_element(shares.begin(), shares.end())};
    std::vector<double> prices;
    double sum{0};
    for (const double q : shares) sum += prices.emplace_back(std::exp((q - top) / liquidity));
    for (double& price : prices) price /= sum;
    return prices;
}

/** A text in a rounded box of a colour. */
QLabel* Pill(const QString& text, const char* colour, QWidget* parent)
{
    auto* pill{new QLabel(text, parent)};
    pill->setStyleSheet(QStringLiteral("QLabel { background-color: %1; color: #ffffff; border-radius: 8px; padding: 2px 8px; font-weight: bold; }").arg(QLatin1String(colour)));
    pill->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
    return pill;
}

QLabel* Dim(const QString& text, QWidget* parent)
{
    auto* label{new QLabel(text, parent)};
    label->setEnabled(false);
    label->setWordWrap(true);
    return label;
}

/** A frame that can be clicked, such as the card of a market or the row of an outcome. */
class ClickFrame : public QFrame
{
public:
    ClickFrame(QWidget* parent, std::function<void()> clicked) : QFrame(parent), m_clicked{std::move(clicked)}
    {
        setCursor(Qt::PointingHandCursor);
        setFrameShape(QFrame::StyledPanel);
    }

protected:
    // Taken, so that the release comes here too.
    void mousePressEvent(QMouseEvent* event) override { event->accept(); }
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) m_clicked();
    }

private:
    std::function<void()> m_clicked;
};

/** Lays its items out in rows, as many to a row as fit: for the cards. */
class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget* parent) : QLayout(parent) { setContentsMargins(0, 0, 0, 0); }
    ~FlowLayout() override
    {
        while (QLayoutItem* item{takeAt(0)}) delete item;
    }
    void addItem(QLayoutItem* item) override { m_items.push_back(item); }
    int count() const override { return m_items.size(); }
    QLayoutItem* itemAt(int index) const override { return m_items.value(index); }
    QLayoutItem* takeAt(int index) override { return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr; }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), /*move=*/false); }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override
    {
        QSize size;
        for (const QLayoutItem* item : m_items) size = size.expandedTo(item->minimumSize());
        return size;
    }
    void setGeometry(const QRect& rect) override
    {
        QLayout::setGeometry(rect);
        arrange(rect, /*move=*/true);
    }

private:
    int arrange(const QRect& rect, bool move) const
    {
        const int gap{10};
        int x{rect.x()}, y{rect.y()}, row_height{0};
        for (QLayoutItem* item : m_items) {
            const QSize size{item->sizeHint()};
            if (x + size.width() > rect.right() + 1 && row_height > 0) {
                x = rect.x();
                y += row_height + gap;
                row_height = 0;
            }
            if (move) item->setGeometry(QRect(QPoint(x, y), size));
            x += size.width() + gap;
            row_height = std::max(row_height, size.height());
        }
        return y + row_height - rect.y();
    }

    QList<QLayoutItem*> m_items;
};

/** Remove what a layout holds. */
void Clear(QLayout* layout)
{
    while (QLayoutItem* item{layout->takeAt(0)}) {
        if (QWidget* widget{item->widget()}) widget->deleteLater();
        if (QLayout* inner{item->layout()}) Clear(inner);
        delete item;
    }
}

QString SealedKey(const QString& decision, const QString& voter) { return QStringLiteral("Markets/sealed/%1/%2").arg(decision, voter); }
} // namespace

MarketsDialog::MarketsDialog(NodeRpc::WalletNameFn wallet_name, QWidget* parent)
    : QDialog(parent, GUIUtil::dialog_flags | Qt::WindowMinMaxButtonsHint), m_wallet_name{std::move(wallet_name)}
{
    setWindowTitle(tr("Markets"));
    // The cards and the rows to pick from have a border that shows which one the pointer is on, or which is picked.
    setStyleSheet(QStringLiteral(
        "QFrame#marketCard, QFrame#outcomeRow, QFrame#ballotCard, QFrame#panel { border: 1px solid palette(mid); border-radius: 8px; }"
        "QFrame#marketCard:hover, QFrame#outcomeRow:hover { border: 1px solid palette(highlight); }"
        "QFrame#outcomeRow[picked=\"true\"] { border: 2px solid palette(highlight); }"
        "QFrame#marketCard QLabel, QFrame#outcomeRow QLabel, QFrame#ballotCard QLabel, QFrame#panel QLabel { border: none; }"
        "QLabel#big { font-size: 18pt; font-weight: bold; }"
        "QLabel#heading { font-size: 13pt; font-weight: bold; }"));

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
    m_tabs->addTab(createPositionsTab(), tr("My &positions"));
    m_tabs->addTab(createCreateTab(), tr("&Create"));
    m_tabs->addTab(createVoteTab(), tr("&Vote"));
    m_tabs->addTab(createDecisionsTab(), tr("&Decisions"));
    m_tabs->addTab(createPeriodsTab(), tr("P&eriods"));
    layout->addWidget(m_tabs, 1);

    m_status = new QLabel(this);
    m_status->setObjectName("marketsStatus");
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);

    connect(refresh_button, &QPushButton::clicked, this, &MarketsDialog::refresh);
    auto* timer{new QTimer(this)};
    connect(timer, &QTimer::timeout, this, &MarketsDialog::tick);
    timer->start(1000);
    resize(1120, 760);
    if (const QByteArray geometry{QSettings().value(QStringLiteral("Markets/geometry")).toByteArray()}; !geometry.isEmpty()) restoreGeometry(geometry);
    setSizeGripEnabled(true);
    GUIUtil::handleCloseWindowShortcut(this);
}

QWidget* MarketsDialog::createMarketsTab()
{
    m_market_pages = new QStackedWidget(this);

    // The markets, as cards.
    auto* browse{new QWidget(m_market_pages)};
    auto* browse_layout{new QVBoxLayout(browse)};
    auto* bar{new QHBoxLayout};
    m_search = new QLineEdit(browse);
    m_search->setObjectName("marketSearch");
    m_search->setPlaceholderText(tr("Search markets…"));
    m_search->setClearButtonEnabled(true);
    m_show_resolved = new QCheckBox(tr("Show closed and resolved"), browse);
    m_show_resolved->setObjectName("showResolved");
    m_sort = new QComboBox(browse);
    m_sort->setObjectName("marketSort");
    m_sort->addItem(tr("Newest"));
    m_sort->addItem(tr("Most traded"));
    m_sort->addItem(tr("Deepest"));
    m_sort->addItem(tr("Closing soonest"));
    bar->addWidget(m_search, 1);
    bar->addWidget(m_show_resolved);
    bar->addWidget(new QLabel(tr("Sort:"), browse));
    bar->addWidget(m_sort);
    browse_layout->addLayout(bar);
    m_count = Dim({}, browse);
    browse_layout->addWidget(m_count);
    auto* scroll{new QScrollArea(browse)};
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_cards = new QWidget(scroll);
    m_cards->setObjectName("marketCards");
    new FlowLayout(m_cards);
    scroll->setWidget(m_cards);
    browse_layout->addWidget(scroll, 1);
    m_market_pages->addWidget(browse);
    connect(m_search, &QLineEdit::textChanged, this, &MarketsDialog::fillBrowse);
    connect(m_show_resolved, &QCheckBox::toggled, this, &MarketsDialog::fillBrowse);
    connect(m_sort, qOverload<int>(&QComboBox::currentIndexChanged), this, &MarketsDialog::fillBrowse);

    // One market: what it is and its outcomes on the left, trading on the right.
    auto* detail{new QWidget(m_market_pages)};
    auto* detail_outer{new QHBoxLayout(detail)};
    auto* detail_split{Splitter(Qt::Horizontal, detail, QStringLiteral("marketDetail"))};
    detail_outer->addWidget(detail_split);
    auto* left_scroll{new QScrollArea(detail)};
    left_scroll->setWidgetResizable(true);
    left_scroll->setFrameShape(QFrame::NoFrame);
    auto* left{new QWidget(left_scroll)};
    auto* left_layout{new QVBoxLayout(left)};
    auto* back{new QPushButton(tr("← Back to the markets"), left)};
    back->setObjectName("backButton");
    back->setFlat(true);
    left_layout->addWidget(back, 0, Qt::AlignLeft);
    m_detail_title = new QLabel(left);
    m_detail_title->setObjectName("heading");
    m_detail_title->setWordWrap(true);
    left_layout->addWidget(m_detail_title);
    m_detail_stage = new QLabel(left);
    m_detail_stage->setObjectName("detailStage");
    m_detail_stage->setWordWrap(true);
    m_detail_stage->setTextFormat(Qt::RichText);
    left_layout->addWidget(m_detail_stage);
    m_detail_facts = new QLabel(left);
    m_detail_facts->setObjectName("detailFacts");
    m_detail_facts->setWordWrap(true);
    m_detail_facts->setTextFormat(Qt::RichText);
    m_detail_facts->setTextInteractionFlags(Qt::TextSelectableByMouse);
    left_layout->addWidget(m_detail_facts);
    m_detail_decisions = new QLabel(left);
    m_detail_decisions->setWordWrap(true);
    m_detail_decisions->setTextFormat(Qt::RichText);
    left_layout->addWidget(m_detail_decisions);
    auto* outcomes_heading{new QLabel(tr("Outcomes"), left)};
    outcomes_heading->setObjectName("heading");
    left_layout->addWidget(outcomes_heading);
    m_outcomes = new QVBoxLayout;
    left_layout->addLayout(m_outcomes);
    left_layout->addStretch();
    left_scroll->setWidget(left);
    detail_split->addWidget(left_scroll);

    auto* panel{new QFrame(detail)};
    panel->setObjectName("panel");
    panel->setMinimumWidth(300);
    auto* panel_layout{new QVBoxLayout(panel)};
    auto* sides{new QHBoxLayout};
    m_buy_side = new QPushButton(tr("Buy"), panel);
    m_buy_side->setObjectName("buySide");
    m_sell_side = new QPushButton(tr("Sell"), panel);
    m_sell_side->setObjectName("sellSide");
    auto* side_group{new QButtonGroup(panel)};
    for (QPushButton* side : {m_buy_side, m_sell_side}) {
        side->setCheckable(true);
        side_group->addButton(side);
        sides->addWidget(side);
    }
    m_buy_side->setChecked(true);
    panel_layout->addLayout(sides);
    m_selected_outcome = new QLabel(panel);
    m_selected_outcome->setObjectName("selectedOutcome");
    m_selected_outcome->setWordWrap(true);
    panel_layout->addWidget(m_selected_outcome);
    m_input_mode = new QComboBox(panel);
    m_input_mode->setObjectName("inputMode");
    m_input_mode->addItem(tr("Coins to spend"));
    m_input_mode->addItem(tr("Shares to buy"));
    panel_layout->addWidget(m_input_mode);
    m_amount = new QLineEdit(panel);
    m_amount->setObjectName("tradeAmount");
    panel_layout->addWidget(m_amount);
    m_chips = new QWidget(panel);
    auto* chips_layout{new QHBoxLayout(m_chips)};
    chips_layout->setContentsMargins(0, 0, 0, 0);
    chips_layout->setSpacing(4);
    for (const char* amount : {"0.1", "0.5", "1", "5", "10", "50"}) {
        auto* chip{new QPushButton(QLatin1String(amount), m_chips)};
        chip->setFlat(true);
        chips_layout->addWidget(chip);
        connect(chip, &QPushButton::clicked, this, [this, amount] { m_amount->setText(QLatin1String(amount)); });
    }
    panel_layout->addWidget(m_chips);
    auto* holding{new QHBoxLayout};
    m_holding = new QLabel(panel);
    m_holding->setObjectName("holding");
    m_holding->setWordWrap(true);
    m_max = new QPushButton(tr("Max"), panel);
    m_max->setObjectName("maxButton");
    holding->addWidget(m_holding, 1);
    holding->addWidget(m_max);
    panel_layout->addLayout(holding);
    m_preview = new QLabel(panel);
    m_preview->setObjectName("tradePreview");
    m_preview->setWordWrap(true);
    m_preview->setTextFormat(Qt::RichText);
    panel_layout->addWidget(m_preview);
    m_order = new QPushButton(panel);
    m_order->setObjectName("orderButton");
    panel_layout->addWidget(m_order);
    m_order_note = Dim({}, panel);
    m_order_note->setObjectName("orderNote");
    panel_layout->addWidget(m_order_note);
    panel_layout->addStretch();
    detail_split->addWidget(Scrolled(panel, detail_split));
    detail_split->setStretchFactor(0, 3);
    detail_split->setStretchFactor(1, 2);
    m_market_pages->addWidget(detail);

    connect(back, &QPushButton::clicked, this, &MarketsDialog::showBrowse);
    connect(side_group, &QButtonGroup::buttonToggled, this, [this](QAbstractButton*, bool checked) {
        if (!checked) return;
        m_amount->clear();
        updatePreview();
    });
    connect(m_input_mode, qOverload<int>(&QComboBox::currentIndexChanged), this, &MarketsDialog::updatePreview);
    connect(m_amount, &QLineEdit::textChanged, this, &MarketsDialog::updatePreview);
    // A sale is from one account: the most that can be sold at once is what the account that holds most has.
    connect(m_max, &QPushButton::clicked, this, [this] {
        QString account;
        m_amount->setText(Coins(myShares(m_market, m_state, &account)));
    });
    connect(m_order, &QPushButton::clicked, this, &MarketsDialog::placeOrder);
    return m_market_pages;
}

QWidget* MarketsDialog::createPositionsTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};
    m_positions_total = new QLabel(tab);
    m_positions_total->setObjectName("positionsTotal");
    layout->addWidget(m_positions_total);
    m_positions = Table({tr("Market"), tr("Outcome"), tr("Shares"), tr("Chance"), tr("Worth now"), tr("Pays if right"), tr("Stage"), QString{}}, tab, "myPositions");
    layout->addWidget(m_positions, 1);
    layout->addWidget(Dim(tr("Worth now is what the shares would bring if they were sold at the chance the market gives the outcome; selling many moves the chance. "
                             "A share pays one coin if its outcome comes true."), tab));
    return tab;
}

QWidget* MarketsDialog::createCreateTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};
    // The two forms side by side; the line between them can be moved.
    auto* forms{Splitter(Qt::Horizontal, tab, QStringLiteral("create"))};

    auto* list{new QGroupBox(tr("1. List a decision"), tab)};
    auto* list_form{new QFormLayout(list)};
    list_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    list_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_decision_text = new QLineEdit(list);
    m_decision_text->setObjectName("decisionText");
    m_decision_text->setPlaceholderText(tr("A question whose answer will be beyond dispute once known"));
    list_form->addRow(tr("Question:"), m_decision_text);
    auto* kind{new QHBoxLayout};
    m_decision_binary = new QRadioButton(tr("Yes or no"), list);
    m_decision_binary->setChecked(true);
    m_decision_scaled = new QRadioButton(tr("A number from"), list);
    m_decision_scaled->setObjectName("decisionScaled");
    m_decision_min = new QLineEdit(QStringLiteral("0"), list);
    m_decision_max = new QLineEdit(QStringLiteral("100"), list);
    m_decision_min->setMaximumWidth(90);
    m_decision_max->setMaximumWidth(90);
    kind->addWidget(m_decision_binary);
    kind->addWidget(m_decision_scaled);
    kind->addWidget(m_decision_min);
    kind->addWidget(new QLabel(tr("to"), list));
    kind->addWidget(m_decision_max);
    kind->addStretch();
    list_form->addRow(tr("Answer:"), kind);
    // When the answer becomes known: so long from now, or on a date. Either sets the other.
    auto* when{new QHBoxLayout};
    m_event_amount = new QSpinBox(list);
    m_event_amount->setObjectName("eventAmount");
    m_event_amount->setRange(0, 100000);
    m_event_unit = new QComboBox(list);
    m_event_unit->setObjectName("eventUnit");
    m_event_date = new QDateTimeEdit(list);
    m_event_date->setObjectName("eventDate");
    m_event_date->setCalendarPopup(true);
    m_event_date->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
    m_event_date_label = new QLabel(tr("that is, on"), list);
    when->addWidget(new QLabel(tr("in"), list));
    when->addWidget(m_event_amount);
    when->addWidget(m_event_unit);
    when->addWidget(m_event_date_label);
    when->addWidget(m_event_date, 1);
    list_form->addRow(tr("Answer known:"), when);
    auto* lasts{new QHBoxLayout};
    m_decision_periods = new QSpinBox(list);
    m_decision_periods->setObjectName("decisionPeriods");
    m_decision_periods->setRange(1, 100000);
    m_decision_periods->setToolTip(tr("For how long the decision is voted on, after its answer is known. Votes are revealed in the last third of the last period of the vote. Longer for answers that take time to establish."));
    m_vote_unit = new QComboBox(list);
    m_vote_unit->setObjectName("voteUnit");
    lasts->addWidget(m_decision_periods);
    lasts->addWidget(m_vote_unit);
    lasts->addStretch();
    list_form->addRow(tr("Vote lasts:"), lasts);
    m_decision_note = Dim({}, list);
    list_form->addRow(m_decision_note);
    auto* list_button{new QPushButton(tr("List decision"), list)};
    list_button->setObjectName("listDecisionButton");
    list_form->addRow(list_button);
    forms->addWidget(Scrolled(list, forms));

    auto* open{new QGroupBox(tr("2. Open a market"), tab)};
    auto* open_layout{new QVBoxLayout(open)};
    auto* open_form{new QFormLayout};
    open_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    open_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    open_layout->addLayout(open_form);
    m_market_text = new QLineEdit(open);
    m_market_text->setObjectName("marketText");
    m_market_text->setPlaceholderText(tr("What the market is about"));
    open_form->addRow(tr("Title:"), m_market_text);
    // The decisions to pick from take the height there is: there can be many, with long questions.
    auto* about{new QHBoxLayout};
    about->addWidget(new QLabel(tr("About:"), open));
    auto* decision_filter{new QLineEdit(open)};
    decision_filter->setObjectName("marketDecisionFilter");
    decision_filter->setPlaceholderText(tr("Find a decision…"));
    decision_filter->setClearButtonEnabled(true);
    about->addWidget(decision_filter, 1);
    auto* only_ticked{new QCheckBox(tr("Only ticked"), open)};
    about->addWidget(only_ticked);
    open_layout->addLayout(about);
    m_market_decisions = new ItemViews::List(open);
    m_market_decisions->setObjectName("marketDecisions");
    m_market_decisions->setMinimumHeight(140);
    m_market_decisions->setWordWrap(true);
    m_market_decisions->setTextElideMode(Qt::ElideNone);
    m_market_decisions->setAlternatingRowColors(true);
    m_market_decisions->setToolTip(tr("The decisions that are still open. A market on one decision has two outcomes; every further decision doubles them."));
    open_layout->addWidget(m_market_decisions, 1);
    const auto filter_decisions{[this, decision_filter, only_ticked] {
        const QString words{decision_filter->text().trimmed()};
        for (int row{0}; row < m_market_decisions->count(); ++row) {
            QListWidgetItem* item{m_market_decisions->item(row)};
            const bool match{words.isEmpty() || item->text().contains(words, Qt::CaseInsensitive) || item->data(Qt::UserRole).toString().startsWith(words)};
            item->setHidden(!match || (only_ticked->isChecked() && item->checkState() != Qt::Checked));
        }
    }};
    connect(decision_filter, &QLineEdit::textChanged, this, filter_decisions);
    connect(only_ticked, &QCheckBox::toggled, this, filter_decisions);
    connect(m_market_decisions->model(), &QAbstractItemModel::rowsInserted, this, [filter_decisions] { QTimer::singleShot(0, filter_decisions); });
    auto* open_rest{new QFormLayout};
    open_rest->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    open_rest->setRowWrapPolicy(QFormLayout::WrapLongRows);
    open_layout->addLayout(open_rest);
    m_market_liquidity = new QLineEdit(QStringLiteral("10"), open);
    m_market_liquidity->setObjectName("marketLiquidity");
    m_market_liquidity->setToolTip(tr("How deep the market is: the larger, the less a trade moves the chances. It is no limit to what can be bet."));
    open_rest->addRow(tr("Depth:"), m_market_liquidity);
    m_market_fee = new QDoubleSpinBox(open);
    m_market_fee->setObjectName("marketFee");
    m_market_fee->setRange(0, 10);
    m_market_fee->setDecimals(2);
    m_market_fee->setSingleStep(0.25);
    m_market_fee->setValue(0.5);
    m_market_fee->setSuffix(QStringLiteral(" %"));
    m_market_fee->setToolTip(tr("Taken from every trade: half goes to the voters, a quarter to those who listed the decisions, a quarter to you."));
    open_rest->addRow(tr("Trading fee:"), m_market_fee);
    m_market_note = Dim({}, open);
    m_market_note->setObjectName("marketNote");
    open_rest->addRow(m_market_note);
    auto* open_button{new QPushButton(tr("Open market"), open)};
    open_button->setObjectName("openMarketButton");
    open_rest->addRow(open_button);
    forms->addWidget(open);
    forms->setStretchFactor(0, 1);
    forms->setStretchFactor(1, 1);
    layout->addWidget(forms, 1);

    const auto note{[this] {
        int ticked{0};
        for (int row{0}; row < m_market_decisions->count(); ++row) ticked += m_market_decisions->item(row)->checkState() == Qt::Checked;
        const double liquidity{m_market_liquidity->text().toDouble()};
        if (ticked == 0) {
            m_market_note->setText(tr("Tick the decisions the market is about."));
        } else {
            const int outcomes{1 << ticked};
            m_market_note->setText(tr("%1 outcomes. You fund the market with %2 coins: that is the most you can lose, however much is bet. "
                                      "You get back what the market has left when it pays out.")
                                       .arg(outcomes).arg(Coins(liquidity * std::log(double(outcomes)))));
        }
    }};
    connect(m_market_decisions, &QListWidget::itemChanged, this, note);
    connect(m_market_liquidity, &QLineEdit::textChanged, this, note);
    const auto from_amount{[this] {
        // The date that is so long from now.
        if (m_period_seconds > 0) {
            const QSignalBlocker blocker{m_event_date};
            m_event_date->setDateTime(QDateTime::currentDateTime().addSecs(qint64(m_event_amount->value()) * m_event_unit->currentData().toLongLong()));
        }
        noteDecision();
    }};
    connect(m_event_amount, qOverload<int>(&QSpinBox::valueChanged), this, from_amount);
    connect(m_event_unit, qOverload<int>(&QComboBox::currentIndexChanged), this, from_amount);
    connect(m_event_date, &QDateTimeEdit::dateTimeChanged, this, [this](const QDateTime& date) {
        // So long from now, in the unit chosen.
        const QSignalBlocker blocker{m_event_amount};
        const qint64 unit{std::max<qint64>(1, m_event_unit->currentData().toLongLong())};
        m_event_amount->setValue(int(std::max<qint64>(0, QDateTime::currentDateTime().secsTo(date) + unit / 2) / unit));
        noteDecision();
    });
    connect(m_decision_periods, qOverload<int>(&QSpinBox::valueChanged), this, [this] { noteDecision(); });
    connect(m_vote_unit, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { noteDecision(); });
    connect(list_button, &QPushButton::clicked, this, &MarketsDialog::createDecision);
    connect(open_button, &QPushButton::clicked, this, &MarketsDialog::createMarket);
    return tab;
}

QWidget* MarketsDialog::createDecisionsTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};
    auto* bar{new QHBoxLayout};
    m_decision_search = new QLineEdit(tab);
    m_decision_search->setObjectName("decisionSearch");
    m_decision_search->setPlaceholderText(tr("Search decisions: words of the question, or an id…"));
    m_decision_search->setClearButtonEnabled(true);
    m_decision_filter = new QComboBox(tab);
    m_decision_filter->setObjectName("decisionFilter");
    m_decision_filter->addItem(tr("All"), QString{});
    m_decision_filter->addItem(tr("Open"), QStringLiteral("open"));
    m_decision_filter->addItem(tr("Being voted on"), QStringLiteral("voting"));
    m_decision_filter->addItem(tr("Resolved"), QStringLiteral("resolved"));
    m_decision_mine = new QCheckBox(tr("Only those I listed"), tab);
    m_decision_mine->setObjectName("decisionMine");
    bar->addWidget(m_decision_search, 1);
    bar->addWidget(new QLabel(tr("Show:"), tab));
    bar->addWidget(m_decision_filter);
    bar->addWidget(m_decision_mine);
    layout->addLayout(bar);

    m_decisions = Table({tr("Question"), tr("Status"), tr("Answer"), tr("Answer on"), tr("Known in period"), tr("Vote ends in period"), tr("Mine"), tr("Id")}, tab, "decisions");
    m_decisions->setSortingEnabled(false);
    layout->addWidget(m_decisions, 1);

    auto* pager{new QHBoxLayout};
    m_decision_count = Dim({}, tab);
    m_decision_count->setWordWrap(false);
    m_decision_prev = new QPushButton(tr("← Previous"), tab);
    m_decision_prev->setObjectName("decisionPrev");
    m_decision_next = new QPushButton(tr("Next →"), tab);
    m_decision_next->setObjectName("decisionNext");
    m_decision_page_label = new QLabel(tab);
    m_decision_page_size = new QComboBox(tab);
    m_decision_page_size->setObjectName("decisionPageSize");
    for (const int size : {10, 25, 50, 100}) m_decision_page_size->addItem(tr("%1 per page").arg(size), size);
    m_decision_page_size->setCurrentIndex(1);
    auto* use{new QPushButton(tr("Open a market on it…"), tab)};
    use->setObjectName("decisionUse");
    use->setToolTip(tr("Go to the Create tab with the selected decision ticked. Only for a decision that is still open."));
    pager->addWidget(m_decision_count, 1);
    pager->addWidget(use);
    pager->addWidget(m_decision_page_size);
    pager->addWidget(m_decision_prev);
    pager->addWidget(m_decision_page_label);
    pager->addWidget(m_decision_next);
    layout->addLayout(pager);

    const auto first_page{[this] {
        m_decision_page = 0;
        fillDecisions();
    }};
    connect(m_decision_search, &QLineEdit::textChanged, this, first_page);
    connect(m_decision_filter, qOverload<int>(&QComboBox::currentIndexChanged), this, first_page);
    connect(m_decision_mine, &QCheckBox::toggled, this, first_page);
    connect(m_decision_page_size, qOverload<int>(&QComboBox::currentIndexChanged), this, first_page);
    connect(m_decision_prev, &QPushButton::clicked, this, [this] {
        --m_decision_page;
        fillDecisions();
    });
    connect(m_decision_next, &QPushButton::clicked, this, [this] {
        ++m_decision_page;
        fillDecisions();
    });
    const auto use_selected{[this] {
        const int row{m_decisions->currentRow()};
        if (row < 0) {
            say(tr("Select a decision first."), /*error=*/true);
            return;
        }
        const QString id{m_decisions->item(row, 7)->text()};
        const Decision* d{decision(id)};
        if (!d || d->status != "open") {
            say(tr("A market can only be opened on a decision that is still open."), /*error=*/true);
            return;
        }
        for (int i{0}; i < m_market_decisions->count(); ++i) {
            if (m_market_decisions->item(i)->data(Qt::UserRole).toString() == id) m_market_decisions->item(i)->setCheckState(Qt::Checked);
        }
        if (m_market_text->text().isEmpty()) m_market_text->setText(d->text);
        m_tabs->setCurrentIndex(2);
        m_market_text->setFocus();
    }};
    connect(use, &QPushButton::clicked, this, use_selected);
    connect(m_decisions, &QTableWidget::cellDoubleClicked, this, use_selected);
    return tab;
}

void MarketsDialog::fillDecisions()
{
    const QString search{m_decision_search->text().trimmed()};
    const QString status{m_decision_filter->currentData().toString()};
    QVector<const Decision*> shown;
    // The newest first.
    for (auto it{m_decision_list.crbegin()}; it != m_decision_list.crend(); ++it) {
        const Decision& d{*it};
        if (!status.isEmpty() && d.status != status) continue;
        if (m_decision_mine->isChecked() && !d.mine) continue;
        if (!search.isEmpty() && !d.text.contains(search, Qt::CaseInsensitive) && !d.id.startsWith(search, Qt::CaseInsensitive)) continue;
        shown.push_back(&d);
    }
    std::stable_sort(shown.begin(), shown.end(), [](const Decision* a, const Decision* b) { return a->height > b->height; });
    const int per_page{m_decision_page_size->currentData().toInt()};
    const int pages{std::max<int>(1, (shown.size() + per_page - 1) / per_page)};
    m_decision_page = std::clamp(m_decision_page, 0, pages - 1);
    const int first{m_decision_page * per_page};
    const int count{std::min<int>(per_page, shown.size() - first)};

    const int keep_row{m_decisions->currentRow()};
    const QString keep{keep_row >= 0 && m_decisions->item(keep_row, 7) ? m_decisions->item(keep_row, 7)->text() : QString{}};
    m_decisions->setRowCount(std::max(0, count));
    for (int row{0}; row < count; ++row) {
        const Decision& d{*shown[first + row]};
        const QString known{untilEndOf(d.event_period)};
        m_decisions->setItem(row, 0, Item(d.text));
        m_decisions->setItem(row, 1, Item(d.status == "open" ? tr("Open") : d.status == "voting" ? tr("Being voted on") : tr("Resolved")));
        m_decisions->setItem(row, 2, Item(d.answer));
        m_decisions->setItem(row, 3, Item(d.scaled ? tr("%1 to %2").arg(d.min).arg(d.max) : tr("yes or no")));
        m_decisions->setItem(row, 4, Item(d.status == "open" && !known.isEmpty() ? tr("%1 (in %2)").arg(d.event_period).arg(known) : QString::number(d.event_period)));
        m_decisions->setItem(row, 5, Item(QString::number(d.close_period)));
        m_decisions->setItem(row, 6, Item(d.mine ? tr("yes") : QString{}));
        m_decisions->setItem(row, 7, Item(d.id));
        if (d.id == keep) m_decisions->selectRow(row);
    }
    FitColumns(m_decisions);
    m_decision_count->setText(shown.isEmpty() ? (m_decision_list.isEmpty() ? tr("No decisions yet. List one on the Create tab.") : tr("No decision matches."))
                                              : tr("%1 to %2 of %n decision(s)", "", shown.size()).arg(first + 1).arg(first + count));
    m_decision_page_label->setText(tr("Page %1 of %2").arg(m_decision_page + 1).arg(pages));
    m_decision_prev->setEnabled(m_decision_page > 0);
    m_decision_next->setEnabled(m_decision_page + 1 < pages);
}

QWidget* MarketsDialog::createVoteTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};
    m_voter_stats = new QLabel(tab);
    m_voter_stats->setObjectName("voterStats");
    m_voter_stats->setTextFormat(Qt::RichText);
    m_voter_stats->setWordWrap(true);
    layout->addWidget(m_voter_stats);
    auto* as{new QHBoxLayout};
    as->addWidget(new QLabel(tr("Vote as:"), tab));
    m_voter = new QComboBox(tab);
    m_voter->setObjectName("voter");
    m_voter->setToolTip(tr("The accounts of this wallet that hold reputation."));
    as->addWidget(m_voter, 1);
    layout->addLayout(as);

    auto* inner{new QTabWidget(tab)};
    auto* ballot_scroll{new QScrollArea(inner)};
    ballot_scroll->setWidgetResizable(true);
    ballot_scroll->setFrameShape(QFrame::NoFrame);
    auto* ballot{new QWidget(ballot_scroll)};
    ballot->setObjectName("ballot");
    m_ballot = new QVBoxLayout(ballot);
    ballot_scroll->setWidget(ballot);
    inner->addTab(ballot_scroll, tr("Ballot"));

    auto* reputation{new QWidget(inner)};
    auto* reputation_layout{new QVBoxLayout(reputation)};
    m_reputation = Table({tr("Account"), tr("Reputation"), tr("Share"), tr("Of this wallet")}, reputation, "reputation");
    reputation_layout->addWidget(m_reputation, 1);
    auto* row{new QHBoxLayout};
    m_transfer_to = new QLineEdit(reputation);
    m_transfer_to->setObjectName("transferTo");
    m_transfer_to->setPlaceholderText(tr("Address of the account to move reputation to"));
    m_transfer_amount = new QLineEdit(reputation);
    m_transfer_amount->setObjectName("transferAmount");
    m_transfer_amount->setPlaceholderText(tr("Amount"));
    m_transfer_amount->setMaximumWidth(140);
    auto* transfer_button{new QPushButton(tr("Move from the selected account"), reputation)};
    transfer_button->setObjectName("transferButton");
    row->addWidget(m_transfer_to, 1);
    row->addWidget(m_transfer_amount);
    row->addWidget(transfer_button);
    reputation_layout->addLayout(row);
    inner->addTab(reputation, tr("Reputation"));
    layout->addWidget(inner, 1);

    connect(m_voter, qOverload<int>(&QComboBox::activated), this, [this] { fillBallot(); });
    connect(transfer_button, &QPushButton::clicked, this, &MarketsDialog::transfer);
    return tab;
}

QWidget* MarketsDialog::createPeriodsTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};
    m_period_now = new QLabel(tab);
    m_period_now->setObjectName("big");
    layout->addWidget(m_period_now);
    m_period_facts = new QLabel(tab);
    m_period_facts->setObjectName("periodFacts");
    m_period_facts->setWordWrap(true);
    m_period_facts->setTextFormat(Qt::RichText);
    layout->addWidget(m_period_facts);
    m_countdown = new QLabel(tab);
    m_countdown->setObjectName("heading");
    layout->addWidget(m_countdown);
    m_period_progress = new QProgressBar(tab);
    m_period_progress->setRange(0, 1000);
    m_period_progress->setTextVisible(false);
    m_period_progress->setMaximumWidth(420);
    layout->addWidget(m_period_progress);
    layout->addWidget(Dim(tr("A period is for trading until it ends. In the periods after, the decisions whose answer became known in it are voted on: votes are "
                             "sealed in the first two thirds of a period, and those of the decisions whose vote ends in it are revealed in the last third. "
                             "The clock is the time of the blocks of the mainchain, and a period turns with the first block of this chain after it."), tab));
    auto* heading{new QLabel(tr("Being voted on"), tab)};
    heading->setObjectName("heading");
    layout->addWidget(heading);
    m_voting = Table({tr("Question"), tr("Answer on"), tr("Known in period"), tr("Vote ends in period"), tr("Vote ends in"), tr("Votes")}, tab, "votingDecisions");
    layout->addWidget(m_voting, 1);
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

void MarketsDialog::hideEvent(QHideEvent* event)
{
    QSettings().setValue(QStringLiteral("Markets/geometry"), saveGeometry());
    QDialog::hideEvent(event);
}

std::optional<UniValue> MarketsDialog::call(const std::string& method, const UniValue& params, bool wallet, bool quiet)
{
    QString error;
    std::optional<QString> wallet_name;
    if (wallet) {
        wallet_name = m_wallet_name();
        if (!wallet_name) {
            if (!quiet) say(tr("Open or create a wallet first."), /*error=*/true);
            return std::nullopt;
        }
    }
    auto result{NodeRpc::Call(m_client_model, method, params, error, wallet_name)};
    if (!result && !quiet) say(error, /*error=*/true);
    return result;
}

bool MarketsDialog::isMine(const std::optional<QString>& wallet, const QString& address) const
{
    return wallet && m_mine.value(*wallet + QLatin1Char('/') + address, false);
}

const MarketsDialog::Market* MarketsDialog::market(const QString& id) const
{
    for (const Market& m : m_market_list) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

const MarketsDialog::Decision* MarketsDialog::decision(const QString& id) const
{
    for (const Decision& d : m_decision_list) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

QString MarketsDialog::outcomeName(const Market& market, int state, int question_length) const
{
    QStringList parts;
    for (int d{0}; d < market.decisions.size(); ++d) {
        const bool high{((state >> d) & 1) != 0};
        const Decision* about{decision(market.decisions[d])};
        QString answer;
        if (about && about->scaled) {
            answer = high ? tr("Higher (toward %1)").arg(about->max) : tr("Lower (toward %1)").arg(about->min);
        } else {
            answer = high ? tr("Yes") : tr("No");
        }
        if (market.decisions.size() == 1) return answer;
        QString question{about ? about->text : market.decisions[d].left(8)};
        if (question.size() > question_length) question = question.left(question_length - 1) + QChar(0x2026);
        parts << QStringLiteral("%1: %2").arg(question, answer);
    }
    return parts.join(QStringLiteral("  ·  "));
}

QString MarketsDialog::untilEndOf(int period) const
{
    if (m_period_seconds <= 0) return {};
    const qint64 end{m_start_time + (qint64(period) + 1) * m_period_seconds};
    return Duration(end - QDateTime::currentSecsSinceEpoch());
}

QString MarketsDialog::stage(const Market& market) const
{
    if (market.status == "settled") return tr("Resolved: it has paid out");
    if (market.status == "closed") return tr("Trading is closed: the answer is being voted on");
    if (market.trading_until < 0) return tr("Trading");
    const QString left{untilEndOf(market.trading_until)};
    return left.isEmpty() ? tr("Trading until period %1 ends").arg(market.trading_until)
                          : tr("Trading closes in about %1 (when period %2 ends)").arg(left).arg(market.trading_until);
}

double MarketsDialog::myShares(const QString& market, int state, QString* account) const
{
    double total{0}, most{0};
    for (const Position& position : m_position_list) {
        if (position.market != market || position.state != state) continue;
        total += position.shares;
        if (position.shares > most) {
            most = position.shares;
            if (account) *account = position.account;
        }
    }
    // One sale is from one account.
    return account ? most : total;
}

void MarketsDialog::say(const QString& text, bool error)
{
    m_status->setStyleSheet(error ? QStringLiteral("QLabel { color: %1; }").arg(QLatin1String(RED)) : QStringLiteral("QLabel { color: %1; }").arg(QLatin1String(GREEN)));
    m_status->setText(text);
}

void MarketsDialog::sent(const QString& what, const UniValue& result)
{
    say(tr("%1 It is in transaction %2 and takes effect with the next block of this chain.").arg(what, Text(result["txid"])));
    refresh();
}

void MarketsDialog::showMarket(const QString& id)
{
    if (m_market != id) {
        m_state = -1;
        m_amount->clear();
    }
    m_market = id;
    m_market_pages->setCurrentIndex(1);
    m_tabs->setCurrentIndex(0);
    fillDetail();
}

void MarketsDialog::showBrowse()
{
    m_market_pages->setCurrentIndex(0);
}

void MarketsDialog::fillBrowse()
{
    QVector<const Market*> shown;
    const QString search{m_search->text().trimmed()};
    for (const Market& m : m_market_list) {
        if (!m_show_resolved->isChecked() && m.status != "trading") continue;
        if (!search.isEmpty() && !m.text.contains(search, Qt::CaseInsensitive)) continue;
        shown.push_back(&m);
    }
    const auto traded{[](const Market* m) {
        double sum{0};
        for (const State& s : m->states) sum += s.shares;
        return sum;
    }};
    switch (m_sort->currentIndex()) {
    case 0: std::stable_sort(shown.begin(), shown.end(), [](const Market* a, const Market* b) { return a->height > b->height; }); break;
    case 1: std::stable_sort(shown.begin(), shown.end(), [&](const Market* a, const Market* b) { return traded(a) > traded(b); }); break;
    case 2: std::stable_sort(shown.begin(), shown.end(), [](const Market* a, const Market* b) { return a->liquidity > b->liquidity; }); break;
    default: std::stable_sort(shown.begin(), shown.end(), [](const Market* a, const Market* b) { return a->trading_until < b->trading_until; }); break;
    }

    Clear(m_cards->layout());
    for (const Market* m : shown) {
        const QString id{m->id};
        auto* card{new ClickFrame(m_cards, [this, id] { showMarket(id); })};
        card->setObjectName("marketCard");
        card->setFixedSize(310, 190);
        auto* layout{new QVBoxLayout(card)};
        auto* title{new QLabel(m->text, card)};
        title->setWordWrap(true);
        QFont bold{title->font()};
        bold.setBold(true);
        title->setFont(bold);
        layout->addWidget(title);
        auto* line{new QHBoxLayout};
        line->addWidget(Pill(m->status == "trading" ? tr("LIVE") : m->status == "closed" ? tr("VOTING") : tr("RESOLVED"),
                             m->status == "trading" ? GREEN : m->status == "closed" ? ORANGE : BLUE, card));
        QString when;
        if (m->status == "trading" && m->trading_until >= 0) {
            const QString left{untilEndOf(m->trading_until)};
            when = left.isEmpty() ? tr("until period %1").arg(m->trading_until) : tr("closes in %1").arg(left);
        }
        line->addWidget(Dim(when, card), 1);
        layout->addLayout(line);
        // The two likeliest outcomes.
        QVector<State> states{m->states};
        std::stable_sort(states.begin(), states.end(), [](const State& a, const State& b) { return a.price > b.price; });
        for (int i{0}; i < std::min<int>(2, states.size()); ++i) {
            if (m->status == "settled") break;
            QString name{outcomeName(*m, states[i].index, /*question_length=*/11)};
            if (name.size() > 34) name = name.left(33) + QChar(0x2026);
            auto* outcome{new QLabel(QStringLiteral("<b style=\"color:%1\">%2</b>&nbsp;&nbsp;%3").arg(QLatin1String(states[i].price > 0.5 ? GREEN : BLUE), Percent(states[i].price), name.toHtmlEscaped()), card)};
            outcome->setTextFormat(Qt::RichText);
            layout->addWidget(outcome);
        }
        if (states.size() > 2 && m->status != "settled") layout->addWidget(Dim(tr("+%1 more").arg(states.size() - 2), card));
        layout->addStretch();
        layout->addWidget(Dim(tr("%1 traded  ·  depth %2  ·  %3 outcomes").arg(Coins(traded(m)), Coins(m->liquidity)).arg(m->states.size()), card));
        m_cards->layout()->addWidget(card);
    }
    m_count->setText(shown.isEmpty() ? (m_market_list.isEmpty() ? tr("No markets yet. Open one on the Create tab.") : tr("No markets to show. Tick \"Show closed and resolved\" to see those that are no longer trading."))
                                     : tr("%n market(s)", "", shown.size()));
}

void MarketsDialog::fillDetail()
{
    const Market* m{market(m_market)};
    if (!m) {
        showBrowse();
        return;
    }
    m_detail_title->setText(m->text);
    const char* colour{m->status == "trading" ? GREEN : m->status == "closed" ? ORANGE : BLUE};
    m_detail_stage->setText(QStringLiteral("<span style=\"color:%1; font-weight:bold\">%2</span>&nbsp;&nbsp;%3")
                                .arg(QLatin1String(colour), m->status == "trading" ? tr("LIVE") : m->status == "closed" ? tr("VOTING") : tr("RESOLVED"), stage(*m).toHtmlEscaped()));
    double traded{0};
    for (const State& s : m->states) traded += s.shares;
    m_detail_facts->setText(tr("Shares sold: <b>%1</b> &nbsp;·&nbsp; Depth: <b>%2</b> &nbsp;·&nbsp; Fee: <b>%3 %</b> &nbsp;·&nbsp; The market holds: <b>%4</b><br>"
                               "Opened by %5 in block %6 &nbsp;·&nbsp; Id: %7")
                                .arg(Coins(traded), Coins(m->liquidity), QString::number(m->fee_bps / 100.0, 'f', 2), Coins(m->treasury), m->author).arg(m->height).arg(m->id));
    QStringList about;
    for (const QString& id : m->decisions) {
        const Decision* d{decision(id)};
        if (!d) continue;
        const QString state{d->status == "open" ? tr("answer known in period %1").arg(d->event_period)
                            : d->status == "voting" ? tr("being voted on until period %1 ends").arg(d->close_period)
                                                    : tr("resolved: %1").arg(d->answer)};
        about << QStringLiteral("• %1 <span style=\"color:gray\">(%2; %3)</span>").arg(d->text.toHtmlEscaped(), d->scaled ? tr("a number from %1 to %2").arg(d->min).arg(d->max) : tr("yes or no"), state);
    }
    m_detail_decisions->setText(about.join(QStringLiteral("<br>")));

    Clear(m_outcomes);
    if (m->status == "settled") {
        m_outcomes->addWidget(Dim(tr("The market has paid out: every share got what its outcome was worth, and what was left went back to the one who opened it."), this));
    }
    // A market on one number: where the market puts it on the scale.
    if (m->decisions.size() == 1 && m->status != "settled") {
        const Decision* d{decision(m->decisions[0])};
        if (d && d->scaled && m->states.size() == 2) {
            const double estimate{d->min + m->states[1].price * (d->max - d->min)};
            auto* gauge{new QLabel(tr("The market puts it at about <b style=\"font-size:16pt\">%1</b> on the scale from %2 to %3.").arg(QString::number(estimate, 'f', 2)).arg(d->min).arg(d->max), this)};
            gauge->setTextFormat(Qt::RichText);
            m_outcomes->addWidget(gauge);
        }
    }
    QVector<State> states{m->states};
    std::stable_sort(states.begin(), states.end(), [](const State& a, const State& b) { return a.price > b.price; });
    for (const State& s : states) {
        if (m->status == "settled") break;
        const int index{s.index};
        auto* row{new ClickFrame(this, [this, index] {
            m_state = index;
            fillDetail();
        })};
        row->setObjectName("outcomeRow");
        row->setProperty("picked", index == m_state);
        auto* layout{new QHBoxLayout(row)};
        auto* name{new QLabel(outcomeName(*m, index), row)};
        name->setWordWrap(true);
        QFont bold{name->font()};
        bold.setBold(true);
        name->setFont(bold);
        layout->addWidget(name, 1);
        const double mine{myShares(m->id, index)};
        if (mine > 0) layout->addWidget(Dim(tr("you hold %1").arg(Coins(mine)), row));
        auto* chance{new QLabel(Percent(s.price), row)};
        chance->setObjectName("heading");
        chance->setMinimumWidth(80);
        chance->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(chance);
        auto* bar{new QProgressBar(row)};
        bar->setRange(0, 1000);
        bar->setValue(int(s.price * 1000));
        bar->setTextVisible(false);
        bar->setFixedSize(120, 10);
        // Yes, or the high end of a scale, in green; with several decisions the bars are all of one colour.
        const char* tint{m->decisions.size() > 1 ? BLUE : (index & 1) ? GREEN : RED};
        bar->setStyleSheet(QStringLiteral("QProgressBar { border: none; border-radius: 5px; background-color: palette(mid); } QProgressBar::chunk { border-radius: 5px; background-color: %1; }").arg(QLatin1String(tint)));
        layout->addWidget(bar);
        m_outcomes->addWidget(row);
    }
    updatePreview();
}

void MarketsDialog::updatePreview()
{
    const Market* m{market(m_market)};
    m_trade_shares = 0;
    m_trade_total = 0;
    if (!m) return;
    const bool buying{m_buy_side->isChecked()};
    const bool trading{m->status == "trading"};
    m_input_mode->setVisible(buying);
    m_chips->setVisible(buying);
    m_order->setText(buying ? tr("Buy") : tr("Sell"));
    m_order->setEnabled(false);
    m_amount->setEnabled(trading && m_state >= 0);
    m_order_note->clear();
    m_preview->clear();
    m_holding->clear();
    m_max->setVisible(!buying);
    if (!trading) {
        m_selected_outcome->setText(m->status == "closed" ? tr("Trading is closed. Shares are neither sold nor bought back until the market pays out, after the vote.") : tr("The market has paid out."));
        return;
    }
    if (m_state < 0 || m_state >= m->states.size()) {
        m_selected_outcome->setText(tr("Pick an outcome on the left to trade in it."));
        return;
    }
    m_selected_outcome->setText(tr("<b>%1</b><br>now at %2").arg(outcomeName(*m, m_state).toHtmlEscaped(), Percent(m->states[m_state].price)));
    QString account;
    const double held{myShares(m->id, m_state, &account)};
    m_holding->setText(held > 0 ? tr("Your shares in this outcome: %1").arg(Coins(held)) : tr("You have no shares in this outcome."));
    m_max->setEnabled(held > 0);
    m_amount->setPlaceholderText(!buying ? tr("Shares to sell") : m_input_mode->currentIndex() == 0 ? tr("Coins to spend") : tr("Shares to buy"));

    bool ok{false};
    const double entered{m_amount->text().trimmed().toDouble(&ok)};
    if (m_amount->text().trimmed().isEmpty()) {
        m_order_note->setText(buying ? tr("A share pays one coin if the outcome comes true, and nothing if it does not.") : QString{});
        return;
    }
    if (!ok || entered <= 0) {
        m_order_note->setText(tr("Enter an amount above zero."));
        return;
    }
    std::vector<double> shares;
    for (const State& s : m->states) shares.push_back(s.shares);
    const double fee_rate{m->fee_bps / 10000.0};
    const auto cost_of{[&](double n) {
        std::vector<double> after{shares};
        after[m_state] += n;
        return LmsrCost(after, m->liquidity) - LmsrCost(shares, m->liquidity);
    }};
    double trade{entered};
    if (buying && m_input_mode->currentIndex() == 0) {
        // The most shares the coins buy, the fee included.
        double low{0}, high{entered * 1e6};
        for (int i{0}; i < 200; ++i) {
            const double middle{(low + high) / 2};
            (cost_of(middle) * (1 + fee_rate) <= entered ? low : high) = middle;
        }
        trade = std::floor(low * 1e6) / 1e6;
        if (trade <= 0) {
            m_order_note->setText(tr("That buys no shares."));
            return;
        }
    }
    if (!buying) {
        if (trade > held + 1e-9) {
            m_order_note->setText(held > 0 ? tr("You hold %1 shares of this outcome in one account.").arg(Coins(held)) : tr("You have no shares of this outcome to sell."));
            return;
        }
        trade = -trade;
    }
    // The node says what the trade costs to the last unit.
    QString error;
    const auto quote{NodeRpc::Call(m_client_model, "quotetrade", Args({m->id.toStdString(), m_state, Exact(trade)}), error, std::nullopt)};
    if (!quote) {
        m_order_note->setText(error);
        return;
    }
    const double total{quote->get_real()};
    const double base{cost_of(trade)};
    const double fee{std::abs(total - base)};
    std::vector<double> after{shares};
    after[m_state] += trade;
    const double new_price{LmsrPrices(after, m->liquidity)[m_state]};
    const QString row{QStringLiteral("<tr><td>%1&nbsp;&nbsp;</td><td align=\"right\">%2</td></tr>")};
    QString table{QStringLiteral("<table>")};
    table += row.arg(tr("Shares:"), Coins(std::abs(trade)));
    table += row.arg(buying ? tr("Cost:") : tr("Proceeds:"), Coins(std::abs(base)));
    table += row.arg(tr("Fee (%1 %):").arg(QString::number(m->fee_bps / 100.0, 'f', 2)), Coins(fee));
    table += QStringLiteral("<tr><td><b>%1</b>&nbsp;&nbsp;</td><td align=\"right\"><b>%2</b></td></tr>").arg(buying ? tr("You pay:") : tr("You get:"), Coins(std::abs(total)));
    table += row.arg(tr("Chance after:"), QStringLiteral("%1 → %2").arg(Percent(m->states[m_state].price), Percent(new_price)));
    if (buying) {
        table += row.arg(tr("Pays if right:"), Coins(trade));
        table += row.arg(tr("Gain if right:"), QStringLiteral("<span style=\"color:%1\">+%2</span>").arg(QLatin1String(GREEN), Coins(trade - total)));
    }
    table += QStringLiteral("</table>");
    m_preview->setText(table);
    m_trade_shares = trade;
    m_trade_total = total;
    m_order->setEnabled(true);
    m_order->setText(buying ? tr("Buy for %1").arg(Coins(total)) : tr("Sell for %1").arg(Coins(-total)));
    if (buying && new_price > 0.98) m_order_note->setText(tr("This much moves the chance close to certain: further shares cost almost a coin each."));
}

void MarketsDialog::placeOrder()
{
    const Market* m{market(m_market)};
    if (!m || m_trade_shares == 0 || m_state < 0) return;
    // What is confirmed, copied: the message box runs the event loop, and a block that comes meanwhile
    // refreshes the page, which fills the list of markets again and moves or frees what `m` points at.
    const QString id{m->id};
    const QString status{m->status};
    const int state{m_state};
    const double shares{m_trade_shares};
    const double total{m_trade_total};
    const bool buying{shares > 0};
    const QString name{outcomeName(*m, state)};
    const QString question{buying ? tr("Buy %1 shares of \"%2\" in \"%3\" for about %4?").arg(Coins(shares), name, m->text, Coins(total))
                                  : tr("Sell %1 shares of \"%2\" in \"%3\" for about %4?").arg(Coins(-shares), name, m->text, Coins(-total))};
    m = nullptr;
    if (QMessageBox::question(this, windowTitle(), question, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes) != QMessageBox::Yes) return;
    // The market confirmed, as the page knows it now: still there, still shown, as it was.
    const Market* now{market(id)};
    if (!now || now->status != status || m_market != id || m_state != state) {
        say(tr("The market changed while the order was being confirmed, so it was not placed. Check it and place it again."), /*error=*/true);
        return;
    }
    if (buying) {
        if (const auto result{call("buyshares", Args({id.toStdString(), state, Exact(shares)}), /*wallet=*/true)}) {
            m_amount->clear();
            sent(tr("Bought %1 shares of \"%2\".").arg(Coins(shares), name), *result);
        }
    } else {
        QString account;
        myShares(id, state, &account);
        if (const auto result{call("sellshares", Args({id.toStdString(), state, Exact(-shares), account.toStdString()}), /*wallet=*/true)}) {
            m_amount->clear();
            sent(tr("Sold %1 shares of \"%2\".").arg(Coins(-shares), name), *result);
        }
    }
}

void MarketsDialog::fillPositions()
{
    m_positions->setRowCount(0);
    double worth{0};
    for (const Position& position : m_position_list) {
        const Market* m{market(position.market)};
        if (!m || position.state >= m->states.size()) continue;
        const int row{m_positions->rowCount()};
        m_positions->insertRow(row);
        const double price{m->states[position.state].price};
        worth += position.shares * price;
        m_positions->setItem(row, 0, Item(m->text));
        m_positions->setItem(row, 1, Item(outcomeName(*m, position.state, /*question_length=*/18)));
        m_positions->setItem(row, 2, Item(Coins(position.shares)));
        m_positions->setItem(row, 3, Item(Percent(price)));
        m_positions->setItem(row, 4, Item(Coins(position.shares * price)));
        m_positions->setItem(row, 5, Item(Coins(position.shares)));
        m_positions->setItem(row, 6, Item(m->status == "trading" ? tr("Trading") : tr("Waiting for the vote")));
        if (m->status == "trading") {
            auto* sell{new QPushButton(tr("Sell…"), m_positions)};
            const QString id{m->id};
            const int state{position.state};
            connect(sell, &QPushButton::clicked, this, [this, id, state] {
                showMarket(id);
                m_state = state;
                m_sell_side->setChecked(true);
                fillDetail();
            });
            m_positions->setCellWidget(row, 7, sell);
        }
    }
    FitColumns(m_positions);
    m_positions_total->setText(m_position_list.isEmpty() ? tr("This wallet holds no shares. Buy some in a market on the Markets tab.")
                                                         : tr("<b>Worth now: %1</b> in %n position(s)", "", m_positions->rowCount()).arg(Coins(worth)));
}

void MarketsDialog::fillCreate()
{
    // The units time is told in: those of the calendar where periods are counted in time, periods on a test chain.
    const bool timed{m_period_seconds > 0};
    if (m_event_unit->count() == 0 || (m_event_unit->itemData(0).toLongLong() > 0) != timed) {
        const QSignalBlocker block_unit{m_event_unit}, block_vote{m_vote_unit}, block_amount{m_event_amount}, block_periods{m_decision_periods}, block_date{m_event_date};
        m_event_unit->clear();
        m_vote_unit->clear();
        if (timed) {
            const std::pair<QString, qint64> units[]{{tr("minutes"), 60}, {tr("hours"), 3600}, {tr("days"), 86400}, {tr("weeks"), 7 * 86400}, {tr("months"), 2629800}, {tr("years"), 31557600}};
            for (const auto& [name, seconds] : units) {
                m_event_unit->addItem(name, seconds);
                m_vote_unit->addItem(name, seconds);
            }
            // To start with: a few periods from now, and a vote of one period.
            const qint64 ahead{3 * m_period_seconds};
            const int unit{ahead < 3 * 3600 ? 0 : ahead < 3 * 86400 ? 1 : 2};
            m_event_unit->setCurrentIndex(unit);
            m_event_amount->setValue(int(ahead / m_event_unit->currentData().toLongLong()));
            m_event_date->setDateTime(QDateTime::currentDateTime().addSecs(ahead));
            const int vote_unit{m_period_seconds < 3600 ? 0 : m_period_seconds < 86400 ? 1 : 2};
            m_vote_unit->setCurrentIndex(vote_unit);
            m_decision_periods->setValue(int(std::max<qint64>(1, m_period_seconds / m_vote_unit->currentData().toLongLong())));
        } else {
            m_event_unit->addItem(tr("periods"), 0);
            m_vote_unit->addItem(tr("periods"), 0);
            m_event_amount->setValue(2);
            m_decision_periods->setValue(1);
        }
        m_event_date->setVisible(timed);
        m_event_date_label->setVisible(timed);
    }
    if (timed) m_event_date->setMinimumDateTime(QDateTime::currentDateTime());
    noteDecision();

    QStringList ticked;
    for (int row{0}; row < m_market_decisions->count(); ++row) {
        if (m_market_decisions->item(row)->checkState() == Qt::Checked) ticked << m_market_decisions->item(row)->data(Qt::UserRole).toString();
    }
    {
        const QSignalBlocker blocker{m_market_decisions};
        m_market_decisions->clear();
        for (const Decision& d : m_decision_list) {
            if (d.status != "open") continue;
            const QString left{untilEndOf(d.event_period)};
            auto* item{new QListWidgetItem(left.isEmpty() ? tr("%1  (until period %2)").arg(d.text).arg(d.event_period) : tr("%1  (trades for about %2)").arg(d.text, left), m_market_decisions)};
            item->setData(Qt::UserRole, d.id);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(ticked.contains(d.id) ? Qt::Checked : Qt::Unchecked);
        }
    }
    Q_EMIT m_market_liquidity->textChanged(m_market_liquidity->text());

}

int MarketsDialog::eventPeriod() const
{
    if (m_period_seconds <= 0) return m_period + m_event_amount->value();
    const qint64 at{m_event_date->dateTime().toSecsSinceEpoch()};
    return std::max<int>(m_period, int((at - m_start_time) / m_period_seconds));
}

int MarketsDialog::votePeriods() const
{
    if (m_period_seconds <= 0) return std::max(1, m_decision_periods->value());
    const qint64 seconds{qint64(m_decision_periods->value()) * m_vote_unit->currentData().toLongLong()};
    return int(std::max<qint64>(1, (seconds + m_period_seconds - 1) / m_period_seconds));
}

bool MarketsDialog::noteDecision()
{
    const int event{eventPeriod()};
    const int periods{votePeriods()};
    QString problem;
    if (event - m_period > m_max_periods_ahead) {
        problem = m_period_seconds > 0 ? tr("That is too far ahead: the answer can be at most %1 periods away, which is about %2.").arg(m_max_periods_ahead).arg(Duration(qint64(m_max_periods_ahead) * m_period_seconds))
                                       : tr("That is too far ahead: the answer can be at most %1 periods away.").arg(m_max_periods_ahead);
    } else if (periods > m_max_vote_periods) {
        problem = m_period_seconds > 0 ? tr("That is too long a vote: it can last at most %1 periods, which is about %2.").arg(m_max_vote_periods).arg(Duration(qint64(m_max_vote_periods) * m_period_seconds))
                                       : tr("That is too long a vote: it can last at most %1 periods.").arg(m_max_vote_periods);
    }
    if (!problem.isEmpty()) {
        m_decision_note->setEnabled(true);
        m_decision_note->setStyleSheet(QStringLiteral("QLabel { color: %1; }").arg(QLatin1String(RED)));
        m_decision_note->setText(problem);
        return false;
    }
    m_decision_note->setEnabled(false);
    m_decision_note->setStyleSheet({});
    QString text;
    if (m_period_seconds > 0) {
        const auto end_of{[this](int period) { return QDateTime::fromSecsSinceEpoch(m_start_time + (qint64(period) + 1) * m_period_seconds); }};
        const QString format{QStringLiteral("yyyy-MM-dd HH:mm")};
        text = tr("The answer is known in period %1. Markets on it trade until that period ends, %2 (in about %3). "
                  "The vote then takes %n period(s) and is over %4.", "", periods)
                   .arg(event).arg(end_of(event).toString(format), untilEndOf(event), end_of(event + periods).toString(format));
    } else {
        text = tr("Markets on it trade until period %1 ends; it is voted on until period %2 ends.").arg(event).arg(event + periods);
    }
    m_decision_note->setText(text + QLatin1Char(' ') + tr("Listing costs %1, which goes to the voters.").arg(Coins(m_listing_fee)));
    return true;
}

void MarketsDialog::createDecision()
{
    if (m_decision_text->text().trimmed().isEmpty()) {
        say(tr("Enter the question."), /*error=*/true);
        return;
    }
    if (!noteDecision()) {
        say(m_decision_note->text(), /*error=*/true);
        return;
    }
    UniValue params{Args({m_decision_text->text().trimmed().toStdString(), eventPeriod(), votePeriods(), m_decision_scaled->isChecked()})};
    if (m_decision_scaled->isChecked()) {
        params.push_back(static_cast<int64_t>(m_decision_min->text().toLongLong()));
        params.push_back(static_cast<int64_t>(m_decision_max->text().toLongLong()));
    }
    if (const auto result{call("createdecision", params, /*wallet=*/true)}) {
        const QString text{m_decision_text->text().trimmed()};
        m_decision_text->clear();
        sent(tr("Listed \"%1\". Once it is mined, tick it on the right to open a market on it.").arg(text), *result);
    }
}

void MarketsDialog::createMarket()
{
    UniValue decisions(UniValue::VARR);
    for (int row{0}; row < m_market_decisions->count(); ++row) {
        if (m_market_decisions->item(row)->checkState() == Qt::Checked) decisions.push_back(m_market_decisions->item(row)->data(Qt::UserRole).toString().toStdString());
    }
    if (decisions.empty() || m_market_text->text().trimmed().isEmpty()) {
        say(tr("Give the market a title and tick the decisions it is about."), /*error=*/true);
        return;
    }
    const int fee_bps{static_cast<int>(std::lround(m_market_fee->value() * 100))};
    if (const auto result{call("createmarket", Args({m_market_text->text().trimmed().toStdString(), decisions, m_market_liquidity->text().trimmed().toStdString(), fee_bps}), /*wallet=*/true)}) {
        const QString text{m_market_text->text().trimmed()};
        m_market_text->clear();
        sent(tr("Opened \"%1\", funded with %2.").arg(text, Coins((*result)["funding"].get_real())), *result);
    }
}

void MarketsDialog::vote(const Decision& decision, const QString& answer, bool reveal)
{
    const QString voter{m_voter->currentText()};
    if (voter.isEmpty()) {
        say(tr("This wallet holds no reputation, so it cannot vote."), /*error=*/true);
        return;
    }
    if (answer.isEmpty()) {
        say(tr("Choose an answer first."), /*error=*/true);
        return;
    }
    if (const auto result{call(reveal ? "revealvote" : "sealvote", Args({voter.toStdString(), decision.id.toStdString(), answer.toStdString()}), /*wallet=*/true)}) {
        // The answer is kept on this computer, to reveal the vote with later: the chain only has its seal.
        if (!reveal) QSettings().setValue(SealedKey(decision.id, voter), answer);
        sent(reveal ? tr("Revealed the vote on \"%1\": %2.").arg(decision.text, answer) : tr("Sealed the vote on \"%1\". It has to be revealed in the last third of period %2.").arg(decision.text).arg(decision.close_period), *result);
    }
}

void MarketsDialog::fillBallot()
{
    Clear(m_ballot);
    const QString voter{m_voter->currentText()};
    int open{0};
    for (const Decision& d : m_decision_list) {
        if (d.status != "voting") continue;
        ++open;
        const Decision about{d};
        // What the account chosen did so far.
        bool sealed{false}, revealed{false};
        QString shown;
        int votes{0};
        if (const auto detail{m_decision_details.constFind(d.id)}; detail != m_decision_details.constEnd()) {
            for (const UniValue& v : (*detail)["votes"].getValues()) {
                ++votes;
                if (Text(v["voter"]) != voter) continue;
                sealed = true;
                revealed = v["revealed"].get_bool();
                if (revealed && v.exists("answer")) {
                    shown = Text(v["answer"]);
                    if (!d.scaled) shown = shown == "1" ? tr("Yes") : shown == "0" ? tr("No") : shown;
                    if (shown == "unknown") shown = tr("Not known yet");
                }
            }
        }
        const QString remembered{QSettings().value(SealedKey(d.id, voter)).toString()};
        const bool reveal_now{m_reveal_phase && m_period == d.close_period};

        auto* card{new QFrame(this)};
        card->setObjectName("ballotCard");
        card->setFrameShape(QFrame::StyledPanel);
        auto* layout{new QVBoxLayout(card)};
        auto* head{new QHBoxLayout};
        auto* question{new QLabel(d.text, card)};
        question->setWordWrap(true);
        QFont bold{question->font()};
        bold.setBold(true);
        question->setFont(bold);
        head->addWidget(question, 1);
        head->addWidget(Pill(d.scaled ? tr("Number %1 to %2").arg(d.min).arg(d.max) : tr("Yes or no"), d.scaled ? BLUE : GREEN, card));
        layout->addLayout(head);
        const QString left{untilEndOf(d.close_period)};
        layout->addWidget(Dim((left.isEmpty() ? tr("The vote ends with period %1.").arg(d.close_period) : tr("The vote ends in about %1 (with period %2).").arg(left).arg(d.close_period)) +
                              QLatin1Char(' ') + tr("Votes so far: %1.").arg(votes), card));

        if (revealed) {
            layout->addWidget(new QLabel(tr("You voted: <b>%1</b>. It counts when the vote ends.").arg(shown.toHtmlEscaped()), card));
            m_ballot->addWidget(card);
            continue;
        }
        // The answer: yes, no or not known yet; or a number.
        auto* row{new QHBoxLayout};
        auto* group{new QButtonGroup(card)};
        QSpinBox* number{nullptr};
        const auto radio{[&](const QString& text, const QString& value) {
            auto* button{new QRadioButton(text, card)};
            button->setProperty("answer", value);
            button->setChecked(remembered == value);
            group->addButton(button);
            row->addWidget(button);
            return button;
        }};
        if (d.scaled) {
            QRadioButton* is_number{radio(tr("It is"), QStringLiteral("number"))};
            number = new QSpinBox(card);
            number->setRange(int(std::max<qint64>(d.min, std::numeric_limits<int>::min())), int(std::min<qint64>(d.max, std::numeric_limits<int>::max())));
            bool is_int{false};
            const int kept{remembered.toInt(&is_int)};
            if (is_int) {
                number->setValue(kept);
                is_number->setChecked(true);
            }
            row->addWidget(number);
        } else {
            radio(tr("Yes"), QStringLiteral("yes"));
            radio(tr("No"), QStringLiteral("no"));
        }
        radio(tr("Not known yet"), QStringLiteral("unknown"))->setToolTip(tr("If most of the reputation says so, the decision is voted on again later."));
        row->addStretch();
        const auto answer{[group, number] {
            const QAbstractButton* chosen{group->checkedButton()};
            if (!chosen) return QString{};
            const QString value{chosen->property("answer").toString()};
            return value == "number" && number ? QString::number(number->value()) : value;
        }};
        auto* seal{new QPushButton(sealed ? tr("Seal another answer") : tr("Seal vote"), card)};
        seal->setObjectName("sealButton");
        auto* reveal{new QPushButton(tr("Reveal vote"), card)};
        reveal->setObjectName("revealButton");
        reveal->setEnabled(sealed && reveal_now);
        row->addWidget(seal);
        row->addWidget(reveal);
        layout->addLayout(row);
        QString state;
        if (!sealed) {
            state = reveal_now ? tr("No vote is sealed, and this is the time to reveal: a vote sealed now can still be revealed before the period ends.")
                               : tr("Seal your answer now; nobody sees it. You reveal it in the last third of period %1.").arg(d.close_period);
        } else if (reveal_now) {
            state = remembered.isEmpty() ? tr("Your vote is sealed. Reveal it now: choose the answer you sealed.") : tr("Your vote is sealed. Reveal it now; the answer you sealed is filled in.");
        } else {
            state = tr("Your vote is sealed. It can be revealed in the last third of period %1; one that is not revealed does not count.").arg(d.close_period);
        }
        auto* note{new QLabel(state, card)};
        note->setWordWrap(true);
        if (sealed && reveal_now) note->setStyleSheet(QStringLiteral("QLabel { color: %1; font-weight: bold; }").arg(QLatin1String(ORANGE)));
        layout->addWidget(note);
        connect(seal, &QPushButton::clicked, this, [this, about, answer] { vote(about, answer(), /*reveal=*/false); });
        connect(reveal, &QPushButton::clicked, this, [this, about, answer] { vote(about, answer(), /*reveal=*/true); });
        m_ballot->addWidget(card);
    }
    if (open == 0) m_ballot->addWidget(Dim(tr("No decision is being voted on. A decision comes up for the vote in the period after the one its answer becomes known in."), this));
    m_ballot->addStretch();
}

void MarketsDialog::fillPeriods()
{
    m_period_now->setText(tr("Period %1").arg(m_period));
    m_period_facts->setText(tr("%1 &nbsp;·&nbsp; a period lasts %2 &nbsp;·&nbsp; %3")
                                .arg(m_period_seconds > 0 ? tr("Counted in the time of the mainchain") : tr("Counted in blocks of this chain (a test chain)"),
                                     m_period_seconds > 0 ? Duration(m_period_seconds) : tr("%1 blocks").arg(m_period_blocks),
                                     m_reveal_phase ? tr("<b style=\"color:%1\">Now: votes that end this period are revealed</b>").arg(QLatin1String(ORANGE)) : tr("Now: votes are sealed")));
    m_voting->setRowCount(0);
    for (const Decision& d : m_decision_list) {
        if (d.status != "voting") continue;
        const int row{m_voting->rowCount()};
        m_voting->insertRow(row);
        const auto detail{m_decision_details.constFind(d.id)};
        m_voting->setItem(row, 0, Item(d.text));
        m_voting->setItem(row, 1, Item(d.scaled ? tr("%1 to %2").arg(d.min).arg(d.max) : tr("yes or no")));
        m_voting->setItem(row, 2, Item(QString::number(d.event_period)));
        m_voting->setItem(row, 3, Item(QString::number(d.close_period)));
        m_voting->setItem(row, 4, Item(untilEndOf(d.close_period)));
        m_voting->setItem(row, 5, Item(detail != m_decision_details.constEnd() ? QString::number((*detail)["votes"].size()) : QString{}));
    }
    FitColumns(m_voting);
    tick();
}

void MarketsDialog::tick()
{
    if (!isVisible()) return;
    if (m_period_seconds <= 0) {
        m_countdown->setText(tr("The period turns as blocks are mined."));
        m_period_progress->setVisible(false);
        return;
    }
    const qint64 start{m_start_time + qint64(m_period) * m_period_seconds};
    const qint64 elapsed{std::clamp<qint64>(QDateTime::currentSecsSinceEpoch() - start, 0, m_period_seconds)};
    const qint64 left{m_period_seconds - elapsed};
    m_period_progress->setVisible(true);
    m_period_progress->setValue(int(elapsed * 1000 / m_period_seconds));
    const qint64 reveal_in{m_period_seconds * 2 / 3 - elapsed};
    m_countdown->setText(left <= 0 ? tr("Period %1 is over: it turns with the next block of this chain.").arg(m_period)
                         : reveal_in > 0 ? tr("%1 left in period %2  ·  votes are revealed in %3").arg(Duration(left)).arg(m_period).arg(Duration(reveal_in))
                                         : tr("%1 left in period %2  ·  time to reveal votes").arg(Duration(left)).arg(m_period));
}

void MarketsDialog::transfer()
{
    const int row{m_reputation->currentRow()};
    const QString from{row < 0 ? QString{} : m_reputation->item(row, 0)->text()};
    if (from.isEmpty() || m_transfer_to->text().trimmed().isEmpty() || m_transfer_amount->text().trimmed().isEmpty()) {
        say(tr("Select an account of this wallet, and enter where to move reputation and how much."), /*error=*/true);
        return;
    }
    if (const auto result{call("transfervotecoin", Args({from.toStdString(), m_transfer_to->text().trimmed().toStdString(), m_transfer_amount->text().trimmed().toStdString()}), /*wallet=*/true)}) {
        m_transfer_amount->clear();
        sent(tr("The reputation is on its way."), *result);
    }
}

void MarketsDialog::refresh()
{
    // Off the GUI thread, one at a time: what the node is asked takes cs_main, and blocks can come
    // faster than it answers. A refresh asked for meanwhile runs once this one is shown.
    if (m_refreshing) {
        m_refresh_again = true;
        return;
    }
    m_refreshing = true;
    ClientModel* const model{m_client_model};
    const std::optional<QString> wallet{m_wallet_name()};
    const QMap<QString, bool> known{m_mine};
    NodeRpc::RunAsync(this, [this, model, wallet, known]() -> std::function<void()> {
        Fetched fetched;
        fetched.wallet = wallet;
        QString error;
        const auto get{[&](const std::string& method, const UniValue& params, bool of_wallet) -> std::optional<UniValue> {
            if (of_wallet && !wallet) return std::nullopt;
            return NodeRpc::Call(model, method, params, error, of_wallet ? wallet : std::nullopt);
        }};
        fetched.info = get("gethivemindinfo", Args({}), false);
        if (fetched.info) {
            fetched.decisions = get("listdecisions", Args({}), false);
            fetched.markets = get("listmarkets", Args({}), false);
            // The shares of this wallet, in one call: not every position of every market.
            fetched.positions = get("getwalletpositions", Args({}), true);
            // The largest holders, and the holdings of this wallet in one call: not a question to the
            // wallet about every account there is, on every block.
            fetched.reputation = get("listreputation", Args({SHOWN_HOLDERS}), false);
            fetched.own = get("getwalletreputation", Args({}), true);
            // The votes on the decisions being voted on, for the ballot and the periods.
            if (fetched.decisions) {
                for (const UniValue& d : fetched.decisions->getValues()) {
                    if (Text(d["status"]) != "voting") continue;
                    if (auto detail{get("getdecision", Args({d["id"]}), false)}) fetched.details.insert(Text(d["id"]), std::move(*detail));
                }
            }
            // Which authors of decisions are the wallet's: each asked once, then known.
            if (wallet && fetched.decisions) {
                for (const UniValue& d : fetched.decisions->getValues()) {
                    const QString address{Text(d["author"])};
                    const QString key{*wallet + QLatin1Char('/') + address};
                    if (known.contains(key) || fetched.mine.contains(key)) continue;
                    if (const auto info{get("getaddressinfo", Args({address.toStdString()}), true)}) fetched.mine.insert(key, (*info)["ismine"].get_bool());
                }
            }
        }
        return [this, fetched = std::move(fetched)]() mutable {
            apply(std::move(fetched));
            m_refreshing = false;
            if (m_refresh_again) {
                m_refresh_again = false;
                refresh();
            }
        };
    });
}

void MarketsDialog::apply(Fetched fetched)
{
    for (auto it{fetched.mine.cbegin()}; it != fetched.mine.cend(); ++it) m_mine.insert(it.key(), it.value());
    m_decision_details = std::move(fetched.details);
    const auto& wallet{fetched.wallet};
    const auto info{std::move(fetched.info)};
    if (!info) {
        m_summary->setText(tr("This chain is not running as a sidechain, so it has no markets."));
        m_tabs->setEnabled(false);
        return;
    }
    m_tabs->setEnabled(true);
    m_period = (*info)["period"].getInt<int>();
    m_reveal_phase = Text((*info)["phase"]) == "reveal";
    m_period_blocks = info->exists("periodblocks") ? (*info)["periodblocks"].getInt<int>() : 0;
    m_period_seconds = info->exists("periodseconds") ? (*info)["periodseconds"].getInt<int64_t>() : 0;
    m_start_time = info->exists("starttime") ? (*info)["starttime"].getInt<int64_t>() : 0;
    m_listing_fee = (*info)["listingfee"].get_real();
    m_max_vote_periods = (*info)["maxvoteperiods"].getInt<int>();
    if (info->exists("maxperiodsahead")) m_max_periods_ahead = (*info)["maxperiodsahead"].getInt<int>();
    const QString left{untilEndOf(m_period)};
    m_summary->setText(tr("Period %1%2  ·  %3  ·  %4 markets, %5 decisions")
                           .arg(m_period)
                           .arg(left.isEmpty() ? QString{} : tr(", about %1 left").arg(left))
                           .arg(m_reveal_phase ? tr("votes are being revealed") : tr("votes are being sealed"))
                           .arg((*info)["markets"].getInt<int>())
                           .arg((*info)["decisions"].getInt<int>()));

    m_decision_list.clear();
    if (const auto& decisions{fetched.decisions}) {
        for (const UniValue& d : decisions->getValues()) {
            Decision decision;
            decision.id = Text(d["id"]);
            decision.text = Text(d["text"]);
            decision.status = Text(d["status"]);
            decision.scaled = d["scaled"].get_bool();
            if (decision.scaled) {
                decision.min = d["min"].getInt<int64_t>();
                decision.max = d["max"].getInt<int64_t>();
            }
            decision.event_period = d["eventperiod"].getInt<int>();
            decision.close_period = d["closeperiod"].getInt<int>();
            if (d.exists("answer")) decision.answer = Text(d["answer"]);
            decision.height = d["height"].getInt<int>();
            decision.mine = isMine(wallet, Text(d["author"]));
            m_decision_list.push_back(std::move(decision));
        }
    }

    m_market_list.clear();
    m_position_list.clear();
    if (const auto& markets{fetched.markets}) {
        for (const UniValue& m : markets->getValues()) {
            Market market;
            market.id = Text(m["id"]);
            market.text = Text(m["text"]);
            market.status = Text(m["status"]);
            market.author = Text(m["author"]);
            for (const UniValue& id : m["decisions"].getValues()) market.decisions << Text(id);
            market.liquidity = m["liquidity"].get_real();
            market.treasury = m["treasury"].get_real();
            market.fees = m["fees"].get_real();
            market.fee_bps = m["feebps"].getInt<int>();
            market.height = m["height"].getInt<int>();
            if (m.exists("tradinguntil")) market.trading_until = m["tradinguntil"].getInt<int>();
            for (const UniValue& s : m["states"].getValues()) {
                State state;
                state.index = s["state"].getInt<int>();
                state.answers = Text(s["answers"]);
                state.shares = s["shares"].get_real();
                if (s.exists("price")) state.price = s["price"].get_real();
                market.states.push_back(std::move(state));
            }
            m_market_list.push_back(std::move(market));
        }
    }
    // The shares of this wallet, in the markets that are not settled.
    if (const auto& positions{fetched.positions}) {
        for (const UniValue& p : positions->getValues()) {
            const QString id{Text(p["market"])};
            if (const Market* in{market(id)}; !in || in->status == "settled") continue;
            m_position_list.push_back({id, Text(p["account"]), p["state"].getInt<int>(), p["shares"].get_real()});
        }
    }

    m_reputation_list.clear();
    // The largest holders, and the holdings of this wallet in one call: not a question to the
    // wallet about every account there is, on every block.
    if (const auto& reputation{fetched.reputation}) {
        const QString voter{m_voter->currentText()};
        const double total{(*info)["reputation"].get_real()};
        double mine_total{0};
        QSet<QString> mine;
        m_voter->clear();
        if (wallet) {
            if (const auto& own{fetched.own}) {
                for (const UniValue& holder : own->getValues()) {
                    const QString account{Text(holder["account"])};
                    const double amount{holder["amount"].get_real()};
                    mine.insert(account);
                    m_reputation_list.insert(account, amount);
                    m_voter->addItem(account);
                    mine_total += amount;
                }
            }
        }
        m_reputation->setRowCount(reputation->size());
        for (size_t row{0}; row < reputation->size(); ++row) {
            const UniValue& holder{(*reputation)[row]};
            const QString account{Text(holder["account"])};
            const double amount{holder["amount"].get_real()};
            m_reputation->setItem(row, 0, Item(account));
            m_reputation->setItem(row, 1, Item(Coins(amount)));
            m_reputation->setItem(row, 2, Item(total > 0 ? Percent(amount / total) : QString{}));
            m_reputation->setItem(row, 3, Item(mine.contains(account) ? tr("yes") : QString{}));
        }
        FitColumns(m_reputation);
        if (!voter.isEmpty()) m_voter->setCurrentIndex(std::max(0, m_voter->findText(voter)));
        int to_vote{0};
        for (const Decision& d : m_decision_list) to_vote += d.status == "voting";
        m_voter_stats->setText(tr("Reputation of this wallet: <b style=\"color:%1\">%2</b> (%3 of all) &nbsp;·&nbsp; Period <b>%4</b> &nbsp;·&nbsp; Decisions to vote on: <b>%5</b> &nbsp;·&nbsp; %6")
                                   .arg(QLatin1String(mine_total > 0 ? GREEN : RED), Coins(mine_total), total > 0 ? Percent(mine_total / total) : QStringLiteral("-"))
                                   .arg(m_period).arg(to_vote)
                                   .arg(mine_total > 0 ? tr("this wallet can vote") : tr("this wallet holds no reputation, so it cannot vote")));
    }

    fillBrowse();
    if (m_market_pages->currentIndex() == 1) fillDetail();
    fillPositions();
    fillCreate();
    fillDecisions();
    fillBallot();
    fillPeriods();
}
