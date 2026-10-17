// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The host side of the pages of Hivemind (src/webui/app/src/chain/local.ts): the RPC methods of the
// markets they call (src/rpc/hivemind.cpp, src/wallet/rpc/hivemind.cpp), and the store of what the chain
// does not keep (the answers of sealed votes, the journal of the app), in files of the data directory.

#include <webui/local.h>

#include <common/args.h>
#include <interfaces/node.h>
#include <node/context.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/fs_helpers.h>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace webui::local {

namespace {
const UniValue& Param(const UniValue& params, const std::string& key)
{
    return params.isObject() ? params.find_value(key) : NullUniValue;
}

/** Where a store is kept: in the data directory of the network, apart from the settings of the app. */
fs::path StorePath(interfaces::Node& node, const std::string& name)
{
    node::NodeContext* context{node.context()};
    if (!context || !context->args) throw HostError("The node is still starting");
    return context->args->GetDataDirNet() / fs::u8path(name);
}
} // namespace

UniValue ReadStore(const fs::path& path)
{
    std::ostringstream text;
    {
        // Closed before anything else: on Windows an open file cannot be renamed.
        std::ifstream file{path.std_path(), std::ios::binary};
        if (!file) return UniValue{};
        text << file.rdbuf();
    }
    UniValue value;
    if (!value.read(text.str()) || !value.isObject()) {
        // Kept aside rather than lost: what the user may need to reveal a vote by hand.
        std::error_code ec;
        fs::rename(path, fs::PathFromString(fs::PathToString(path) + ".unreadable"), ec);
        return UniValue{};
    }
    return value;
}

void WriteStore(const fs::path& path, const UniValue& value)
{
    if (!value.isObject()) throw HostError(BRIDGE_INVALID_REQUEST, "A store is an object");
    const std::string text{value.write()};
    if (text.size() > MAX_STORE_BYTES) throw HostError(strprintf("The record is too large (%u bytes; at most %u)", text.size(), MAX_STORE_BYTES));
    // Written whole to another file, flushed to the disk, then put in place: a crash leaves the old one or the new one.
    const fs::path temp{fs::PathFromString(fs::PathToString(path) + ".new")};
    FILE* file{fsbridge::fopen(temp, "wb")};
    if (!file) throw HostError(strprintf("%s cannot be written", fs::PathToString(temp)));
    const bool written{std::fwrite(text.data(), 1, text.size(), file) == text.size() && FileCommit(file)};
    if (std::fclose(file) != 0 || !written || !RenameOver(temp, path)) {
        std::error_code ec;
        fs::remove(temp, ec);
        throw HostError(strprintf("%s could not be written", fs::PathToString(path)));
    }
}

std::map<std::string, Bridge::Thread> RpcMethods()
{
    return {
        // What the node says of the markets: read only.
        {"gethivemindinfo", Bridge::Thread::WORKER},
        {"listdecisions", Bridge::Thread::WORKER},
        {"getdecision", Bridge::Thread::WORKER},
        {"listmarkets", Bridge::Thread::WORKER},
        {"getmarket", Bridge::Thread::WORKER},
        {"quotetrade", Bridge::Thread::WORKER},
        {"listreputation", Bridge::Thread::WORKER},
        {"listpositions", Bridge::Thread::WORKER},
        // The ballot of an account, to reveal it when this app has no record of it.
        {"getballot", Bridge::Thread::WORKER},
        // What the wallet holds.
        {"getwalletreputation", Bridge::Thread::WORKER},
        {"getwalletpositions", Bridge::Thread::WORKER},
        // Whether a trade's transaction is in a block (gettransaction: confirmations, blockheight), to settle the trades
        // the Portfolio counts against the chain (audit 7 E-HM4). Read only.
        {"gettransaction", Bridge::Thread::WORKER},
        // What the wallet does, transactions it signs: confirmed on the desktop (ConfirmedRpcMethods, below).
    };
}

std::set<std::string> KeyedRpcMethods()
{
    // None: what signs is confirmed on the desktop (ConfirmedRpcMethods).
    return {};
}

std::map<std::string, confirm::Method> ConfirmedRpcMethods()
{
    // What the pages do that pays or signs: never the page's to call by itself. The host shows each call on the desktop, its
    // destinations whole first, then its amounts, then the rest of its parameters, with the passphrase of a locked wallet
    // typed there, and runs it with the keys unlocked for it alone (confirm.h, audit 7 #1).
    return {
        {"createdecision", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Create a decision", "Create", "The wallet pays for a new decision of Hivemind, which reporters vote on."), /*fee_unknown=*/true}},
        {"createmarket", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Create a market", "Create", "The wallet funds a new market with its liquidity, and pays its fee.", {}, {"liquidity"}), /*fee_unknown=*/true}},
        {"buyshares", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Buy shares", "Buy", "The wallet pays for shares of an outcome of a market, at most the cost shown.", {}, {"shares", "maxcost"}), /*fee_unknown=*/true}},
        {"sellshares", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Sell shares", "Sell", "The wallet sells shares of an outcome of a market, for at least the proceeds shown.", {"account"}, {"shares", "minproceeds"}), /*fee_unknown=*/true}},
        {"transfervotecoin", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Send votecoin", "Send", "The wallet sends votecoin (reputation) of one of its accounts to another.", {"to"}, {"amount"}), /*fee_unknown=*/true}},
        {"sealvote", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Seal a vote", "Seal", "The wallet signs and publishes the sealed vote of an account on a decision."), /*fee_unknown=*/true}},
        {"revealvote", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Reveal a vote", "Reveal", "The wallet signs and publishes the vote of an account on a decision."), /*fee_unknown=*/true}},
        {"sealballot", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Seal a ballot", "Seal", "The wallet signs and publishes the sealed ballot of an account."), /*fee_unknown=*/true}},
        {"revealballot", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Reveal a ballot", "Reveal", "The wallet signs and publishes the ballot of an account."), /*fee_unknown=*/true}},
    };
}

void RegisterHost(Bridge& bridge, NodeFn node, guard::ConfirmFn confirm, confirm::KeysFn keys)
{
    (void)keys;
    // The answers of the votes this app sealed (the wallet needs the answer again to reveal it), and the rest of
    // the journal of the app (trades, prices, votes revealed, reminders): each in a file of its own in the data
    // directory, with no cap but a sanity one (audit port-1 #1: in the preferences of the app, capped at 256 kB,
    // the seals were lost once the journal filled up). The seals are written at once, and said when they cannot be.
    bridge.RegisterHost("hivemind.store.load", [node](const UniValue&) {
        interfaces::Node& n{node()};
        UniValue out{UniValue::VOBJ};
        out.pushKV("seals", ReadStore(StorePath(n, SEALS_FILE)));
        out.pushKV("journal", ReadStore(StorePath(n, JOURNAL_FILE)));
        return out;
    }, Bridge::Thread::LONG);
    for (const auto& [method, file] : {std::pair{"hivemind.store.seals", SEALS_FILE}, std::pair{"hivemind.store.journal", JOURNAL_FILE}}) {
        bridge.RegisterHost(method, [node, file = std::string{file}](const UniValue& params) {
            WriteStore(StorePath(node(), file), Param(params, "value"));
            return UniValue{true};
        }, Bridge::Thread::LONG);
    }
    (void)confirm;
}

std::vector<std::string> CoinbaseRoles(interfaces::Chain& chain, const uint256& block)
{
    // For example (FastBlocks): {"staked", "bmm"} for a coinbase whose first output is the producer's share
    // and whose second is the fee pot paid to a BMM bidder.
    (void)chain;
    (void)block;
    return {};
}

} // namespace webui::local
