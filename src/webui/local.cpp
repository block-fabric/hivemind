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
        // What the wallet does: transactions it signs (the keys, below).
        {"createdecision", Bridge::Thread::WORKER},
        {"createmarket", Bridge::Thread::WORKER},
        {"buyshares", Bridge::Thread::WORKER},
        {"sellshares", Bridge::Thread::WORKER},
        {"transfervotecoin", Bridge::Thread::WORKER},
        {"sealvote", Bridge::Thread::WORKER},
        {"revealvote", Bridge::Thread::WORKER},
        {"sealballot", Bridge::Thread::WORKER},
        {"revealballot", Bridge::Thread::WORKER},
    };
}

std::set<std::string> KeyedRpcMethods()
{
    // Each signs a transaction, and those of a market or a vote sign a message with an account's key too.
    return {"createdecision", "createmarket", "buyshares", "sellshares", "transfervotecoin", "sealvote", "revealvote", "sealballot", "revealballot"};
}

void RegisterHost(Bridge& bridge, NodeFn node)
{
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
}

} // namespace webui::local
