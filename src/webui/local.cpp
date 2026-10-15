// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The host side of the pages of Hivemind (src/webui/app/src/chain/local.ts): the RPC methods of the
// markets they call (src/rpc/hivemind.cpp, src/wallet/rpc/hivemind.cpp). No host method of its own: the
// pages need nothing the node's methods and the kit's do not give.

#include <webui/local.h>

namespace webui::local {

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
    (void)bridge;
    (void)node;
}

} // namespace webui::local
