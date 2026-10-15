# Hivemind

Hivemind is a sidechain of Chains for prediction markets: people trade on what will happen, and
holders of reputation (votecoin) vote on what did. The markets are rules of the chain: every node
checks every trade, every vote and every payout.

| | |
|---|---|
| Slot on the mainchain | 9 |
| Coin | CHN, deposited from [Chains](https://github.com/block-fabric/chains) and withdrawn back to it |
| Addresses | `hm1…` (`thm1…` on the test networks) |
| P2P port | 9855 (testnet 19855, signet 39855, regtest 29855) |
| RPC port | 9854 (testnet 19854, signet 39854, regtest 29854) |

Hivemind is the [sidechain template](doc/sidechain.md) of Chains with the markets added. A Hivemind node needs a
Chains node (`chainsd` or `chains-app`) to follow, reached over its RPC interface.

Its GUI is `hivemind-app`: the node and its wallet with an HTML interface in a native web view
(WebKitGTK on Linux, WebView2 on Windows), with pages for the markets, trading, the voting center,
votecoin and the payouts ([doc/webui.md](doc/webui.md)). There is no Qt GUI.

## Installing

Builds for Linux (x86-64) are at https://blockfab.org/drivechains/. Unpack, check `sha256sum -c SHA256SUMS`, and run what is in `bin/`. The app needs WebKitGTK (on Ubuntu 24.04: `sudo apt install libwebkit2gtk-4.1-0 libsqlite3-0`); the node and the tools need none of it. On Windows, the installer or the zip of `hivemind-app` (the WebView2 Runtime of Windows 10 and 11).

## Building from source

On Debian or Ubuntu (24.04):

```sh
sudo apt install build-essential cmake pkgconf python3 libevent-dev libboost-dev \
    libsqlite3-dev libzmq3-dev libgtk-3-dev libwebkit2gtk-4.1-dev nodejs npm
git clone https://github.com/block-fabric/hivemind-app.git
cd hivemind-app
cmake -B build
cmake --build build -j$(nproc)
```

`-DBUILD_WEBUI=OFF` (and no GTK, WebKitGTK or Node.js) for a node without a window. Windows: cross-built
with the depends system (`make -C depends HOST=x86_64-w64-mingw32 NO_QT=1`), see [doc/webui.md](doc/webui.md).
Other systems and options: [doc/build-unix.md](doc/build-unix.md) and the other `doc/build-*.md`.

The programs, in `build/bin`:

| Program | What it is |
|---|---|
| `hivemind-app` | the wallet, with its window |
| `hivemindd` | the node, without a window |
| `hivemind-cli` | commands for a running node or wallet |
| `hivemind-wallet`, `hivemind-tx`, `hivemind-util` | tools that work without a node |

Tests: `ctest --test-dir build` (unit tests) and `build/test/functional/test_runner.py`. The
sidechain tests need a built Chains tree next to this one (`../chains`), or `MAINCHAIN_BIN_DIR`
set to the folder of its programs.

## Running it

First a Chains node, with RPC on (`server=1` in `chains.conf`, or `-server`), on the same network.

The simplest is to let the Chains wallet run Hivemind: in its **Sidechain Nodes** window, select
Hivemind, press **Locate programs…** and choose the folder with `hivemindd` and `hivemind-app`, then **Start node**
or **Open wallet**.

By hand, on the same computer as the Chains node:

```sh
hivemind-app                # the main network
hivemind-app -testnet       # the test network
```

It finds the Chains node by itself: RPC on 127.0.0.1 at the port of the network, and the cookie
file in the Chains data folder (`~/.chains/.cookie`, `~/.chains/testnet/.cookie`). Options for
anything else:

| Option | What it is |
|---|---|
| `-mainchainrpcconnect=<ip>` | address of the Chains node (default 127.0.0.1) |
| `-mainchainrpcport=<port>` | its RPC port (default 9554, testnet 19554, signet 39554, regtest 29554) |
| `-mainchainrpccookiefile=<file>` | its cookie file |
| `-mainchaindatadir=<dir>` | its data folder, where the cookie file is looked for (default `~/.chains`) |
| `-mainchainrpcuser=<user>`, `-mainchainrpcpassword=<pw>` | credentials, instead of the cookie |
| `-mainchainrpcwallet=<name>` | the Chains wallet that pays for merged mining (default: its only loaded wallet) |

The slot is fixed on the main and test networks; `-sidechainslot=<n>` is for regtest only, where
it makes the chain a sidechain in slot `n` (see [doc/sidechain.md](doc/sidechain.md)).

Data and the configuration file `hivemind.conf` are in `~/.hivemind`; the test network has a subfolder
`testnet`. The networks have no DNS seeds: give the node a peer with `addnode`. A sample
`~/.hivemind/hivemind.conf`:

```ini
server=1
fallbackfee=0.0002

# For the test network instead, uncomment:
# testnet=1

[main]
addnode=<peer>:9855
mainchainrpcwallet=<wallet of the Chains node>

[test]
addnode=<peer>:19855
mainchainrpcwallet=<wallet of the Chains node>
```

On the test network, the `-mainchainrpcport`, `-mainchainrpccookiefile` and `-mainchainrpcwallet`
options are read only from the `[test]` section.

## Coins in and out

- **Deposit.** In Hivemind, get a deposit address (`getdepositaddress`, or the **Mainchain** page):
  `s9_<address>_<checksum>`. In Chains, send to it from the **Sidechains** page, or
  `chains-cli createsidechaindeposit 9 <deposit address> <amount>`. The coins appear with the
  next Hivemind block.
- **Withdraw.** `createwithdrawal <Chains address> <amount> ( <fee for Chains miners> )`, or the
  **Mainchain** page. Withdrawals are paid in bundles that Chains miners vote on (about three
  months on the main network, 600 blocks on the test network). A withdrawal not in the bundle
  being voted on can be taken back: `refundwithdrawal <txid> <vout>`.
- **Merged mining.** Hivemind blocks are mined by Chains miners, for a fee in CHN paid by the Chains
  wallet. `setbmm true <Hivemind address>` has the node ask for a block whenever the fees waiting pay
  for one; it offers the miners 99% and keeps 1% at the address. `requestbmmblock <address>
  <amount>` asks for one block now, for a fee of your choice (for a deposit on a quiet chain).
  `getbmminfo` shows how it goes.

How deposits, withdrawals, bundles and merged mining work: [doc/sidechain.md](doc/sidechain.md).

## Markets

- A **decision** is a question whose answer will be known: yes or no, or a number on a scale.
- A **market** is about one to four decisions, with a market maker (LMSR) that always quotes a price.
- Holders of **votecoin** vote on the decisions, sealed first and revealed after; their votes settle
  the markets, and their reputation follows how well they agree with the others.

The main network has no starting account for the votecoin yet: until one is set, use the test
network or regtest. How it all works: [doc/hivemind.md](doc/hivemind.md).

## Commands

Hivemind, on the node: `gethivemindinfo`, `listdecisions`, `getdecision`, `listmarkets`, `getmarket`,
`quotetrade`, `listpositions`, `listreputation`.

Hivemind, in the wallet: `createdecision`, `createmarket`, `buyshares`, `sellshares`, `sealvote`,
`revealvote`, `transfervotecoin`.

Sidechain, on the node: `getmainchaininfo`, `syncmainchain`, `setbmm`, `getbmminfo`, `requestbmmblock`, `createbmmblock`, `listwithdrawals`, `getwithdrawalbundle`.

Sidechain, in the wallet: `getdepositaddress`, `createwithdrawal`, `refundwithdrawal`.

## Wallet

The pages: Overview, Send, Receive, Transactions, **Mainchain** (deposits, withdrawals, merged
mining), and the **Prediction Markets** window (decisions, markets, trading, voting).

## Credits

Hivemind follows Paul Sztorc's Truthcoin paper; LayerTwo Labs' Hivemind implementation was a reference for it. It is built on the sidechain template of Chains, which is based on
[Bitcoin Core](https://github.com/bitcoin/bitcoin) v32; its README is in
[doc/README-bitcoin-core.md](doc/README-bitcoin-core.md).

## License

MIT: see [COPYING](COPYING).
