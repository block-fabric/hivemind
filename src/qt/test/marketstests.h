// Copyright (c) 2026 The Hivemind developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_MARKETSTESTS_H
#define BITCOIN_QT_TEST_MARKETSTESTS_H

#include <QObject>
#include <QTest>

namespace interfaces {
class Node;
} // namespace interfaces

class MarketsTests : public QObject
{
public:
    explicit MarketsTests(interfaces::Node& node) : m_node(node) {}
    interfaces::Node& m_node;

    Q_OBJECT

private Q_SLOTS:
    void marketsTests();
    void timedMarketsTests();
    void noMarketsTests();
    void nodeRpcTests();
};

#endif // BITCOIN_QT_TEST_MARKETSTESTS_H
