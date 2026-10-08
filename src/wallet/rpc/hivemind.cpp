// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <core_io.h>
#include <hash.h>
#include <hivemind/math.h>
#include <hivemind/state.h>
#include <key_io.h>
#include <rpc/util.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <univalue.h>

namespace wallet {
namespace {

using hivemind::Account;
using hivemind::Action;

Account ParseAccount(const UniValue& value)
{
    const CTxDestination dest{DecodeDestination(value.get_str())};
    if (const auto* keyhash{std::get_if<WitnessV0KeyHash>(&dest)}) return Account{*keyhash};
    if (const auto* keyhash{std::get_if<PKHash>(&dest)}) return Account{*keyhash};
    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "An account is the address of a single key (P2WPKH or P2PKH)");
}

/** An account that holds shares: paid as P2WPKH, so given as P2WPKH (the key of a P2PKH address may be uncompressed). */
Account ParseTradeAccount(const UniValue& value)
{
    const CTxDestination dest{DecodeDestination(value.get_str())};
    if (const auto* keyhash{std::get_if<WitnessV0KeyHash>(&dest)}) return Account{*keyhash};
    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Shares are held by a P2WPKH address: what they pay is paid there");
}

/** A new address of the wallet to be an account. */
Account NewAccount(CWallet& wallet, const std::string& label)
{
    const auto dest{wallet.GetNewDestination(OutputType::BECH32, label)};
    if (!dest) throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, util::ErrorString(dest).original);
    return Account{std::get<WitnessV0KeyHash>(*dest)};
}

std::vector<unsigned char> Sign(const CWallet& wallet, const std::string& message, const Account& account)
{
    std::string signature;
    const SigningResult result{wallet.SignMessage(message, PKHash{account}, signature)};
    if (result != SigningResult::OK) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("This wallet cannot sign for the account %s: %s", EncodeDestination(WitnessV0KeyHash{account}), SigningResultString(result)));
    }
    std::vector<unsigned char> bytes{*Assert(DecodeBase64(signature))};
    // The header of a compact signature says whether the key is compressed (31 to 34): the chain only
    // takes those. An uncompressed key, from an old import, would make an action the chain refuses.
    if (bytes.size() != 65 || bytes[0] < 31) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("The key of the account %s is uncompressed: it cannot sign Hivemind actions. Use another account", EncodeDestination(WitnessV0KeyHash{account})));
    }
    return bytes;
}

/**
 * Make, sign and send the transaction that asks for `action` with an output of `value`.
 *
 * @param[in] signer  the account that has to authorise the action, if it needs it. The
 *                    signature names the first input of the transaction, so the
 *                    transaction is made first, with room for the signature.
 * @return the transaction and the index of the output with the action
 */
template <typename T>
std::pair<CTransactionRef, uint32_t> SendAction(CWallet& wallet, T action, CAmount value, const std::optional<Account>& signer = std::nullopt)
{
    const auto script_of{[&] { return hivemind::ActionScript(Action{action}); }};
    if constexpr (requires { action.signature; }) {
        action.signature.assign(CPubKey::COMPACT_SIGNATURE_SIZE, 0);
    }
    const CScript placeholder{script_of()};
    std::vector<CRecipient> recipients{{CNoDestination{placeholder}, value, /*fSubtractFeeFromAmount=*/false}};
    CCoinControl coin_control;
    auto res{CreateTransaction(wallet, recipients, /*change_pos=*/std::nullopt, coin_control, /*sign=*/false)};
    if (!res) throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, util::ErrorString(res).original);

    CMutableTransaction mtx{*res->tx};
    uint32_t index{0};
    while (index < mtx.vout.size() && mtx.vout[index].scriptPubKey != placeholder) ++index;
    CHECK_NONFATAL(index < mtx.vout.size());
    if constexpr (requires { action.signature; }) {
        action.signature.clear();
        // Signed for the next block, which is where the transaction goes: the message names the chain,
        // the output and the outputs of the transaction from audit2_height. The placeholder has the
        // size of the signature, so that the fee is right, and is left out of what is signed, as the
        // signature will be.
        const int next_height{wallet.chain().getHeight().value_or(-1) + 1};
        action.signature = Sign(wallet, hivemind::ActionMessage(Action{action}, mtx.vin[0].prevout, index, hivemind::ActionOutputsHash(mtx.vout),
                                                                wallet.chain().getHivemindParams(), next_height), *Assert(signer));
        mtx.vout[index].scriptPubKey = script_of();
    }
    if (!wallet.SignTransaction(mtx)) throw JSONRPCError(RPC_WALLET_ERROR, "Signing the transaction failed");

    const CTransactionRef tx{MakeTransactionRef(std::move(mtx))};
    wallet.CommitTransaction(tx, /*replaces_txid=*/std::nullopt, /*comment=*/std::nullopt, /*comment_to=*/std::nullopt);
    if (!wallet.chain().isInMempool(tx->GetHash())) {
        wallet.AbandonTransaction(tx->GetHash());
        throw JSONRPCError(RPC_WALLET_ERROR, "The transaction was not accepted into the mempool");
    }
    return {tx, index};
}

/**
 * What opens the seal of a vote. It is a signature by the voter, which the wallet
 * can make again when the vote is revealed, so that there is nothing to store. It
 * is another for each round of voting on the decision: the salt of a round is
 * public once the vote is revealed, and would open the seal of the next.
 */
uint256 VoteSaltOfRound(CWallet& wallet, const Account& voter, const hivemind::Id& decision, unsigned round)
{
    const std::vector<unsigned char> signature{Sign(wallet, strprintf("Hivemind vote salt %s round %u", decision.GetHex(), round), voter)};
    return Hash(signature);
}

uint256 VoteSalt(CWallet& wallet, const Account& voter, const hivemind::Id& decision)
{
    const auto rolls{wallet.chain().getDecisionRolls(decision)};
    if (!rolls) throw JSONRPCError(RPC_INVALID_PARAMETER, "There is no such decision on the chain");
    return VoteSaltOfRound(wallet, voter, decision, *rolls);
}

/**
 * What opens the seal of a ballot: a signature by the voter, as for a single vote. One for each chain,
 * period in which decisions close and set of decisions: it is public once the ballot is revealed, after
 * which no ballot of that period can be sealed any more. The chain (its genesis block) is named, as the
 * message a key signs is the same on every chain made of this code: the salt of a ballot on one would
 * otherwise open the seal of the same key's ballot on another.
 */
uint256 BallotSalt(CWallet& wallet, const Account& voter, uint32_t close_period, const std::vector<hivemind::Id>& decisions)
{
    HashWriter hasher;
    hasher << decisions;
    const std::string message{strprintf("Hivemind ballot salt chain %s period %u decisions %s",
                                        wallet.chain().getHivemindParams().genesis_hash.GetHex(), close_period, hasher.GetHash().GetHex())};
    return Hash(Sign(wallet, message, voter));
}

int64_t ParseAnswer(const UniValue& value)
{
    if (value.isStr()) {
        const std::string text{ToLower(value.get_str())};
        if (text == "unknown") return hivemind::ANSWER_UNKNOWN;
        if (text == "yes") return 1;
        if (text == "no") return 0;
        const auto number{ToIntegral<int64_t>(text)};
        if (number) return *number;
        throw JSONRPCError(RPC_INVALID_PARAMETER, "The answer is \"yes\", \"no\", \"unknown\", or a whole number on the scale of the decision");
    }
    return value.getInt<int64_t>();
}

/** The answers of a ballot, by decision: {"decision id": answer, ...}. */
std::map<hivemind::Id, int64_t> ParseBallotAnswers(const UniValue& votes)
{
    std::map<hivemind::Id, int64_t> answers;
    for (const std::string& key : votes.getKeys()) {
        if (!answers.emplace(ParseHashV(UniValue{key}, "decision"), ParseAnswer(votes[key])).second) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "A decision is named twice");
        }
    }
    if (answers.empty() || answers.size() > hivemind::MAX_DECISIONS_PER_PERIOD) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("A ballot has 1 to %u votes", hivemind::MAX_DECISIONS_PER_PERIOD));
    }
    return answers;
}

std::shared_ptr<CWallet> ReadyWallet(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return nullptr;
    pwallet->BlockUntilSyncedToCurrentChain();
    EnsureWalletIsUnlocked(*pwallet);
    return pwallet;
}

} // namespace

RPCMethod createdecision()
{
    return RPCMethod{
        "createdecision",
        "List a decision: a question whose answer will be known, for markets to be about and for the holders of\n"
        "reputation to vote on. It costs the listing fee (see gethivemindinfo), which goes to those who vote on it." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"text", RPCArg::Type::STR, RPCArg::Optional::NO, "The question, worded so that its answer is beyond dispute once known"},
            {"eventperiod", RPCArg::Type::NUM, RPCArg::Optional::NO, "The voting period in which the answer becomes known; the vote starts in the period after"},
            {"voteperiods", RPCArg::Type::NUM, RPCArg::Default{1}, "The number of periods to vote on it for"},
            {"scaled", RPCArg::Type::BOOL, RPCArg::Default{false}, "Whether the answer is a point on a scale, as opposed to yes or no"},
            {"min", RPCArg::Type::NUM, RPCArg::Default{0}, "The low end of the scale, a whole number"},
            {"max", RPCArg::Type::NUM, RPCArg::Default{0}, "The high end of the scale"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The id of the transaction"},
            {RPCResult::Type::STR_HEX, "id", "The id of the decision, once the transaction is mined"},
            {RPCResult::Type::STR, "author", "The address of this wallet that gets the author's share of fees"},
        }},
        RPCExamples{HelpExampleCli("createdecision", "\"Will it rain in Paris on 1 June 2027?\" 240")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    hivemind::ListDecision list;
    list.text = request.params[0].get_str();
    list.event_period = request.params[1].getInt<uint32_t>();
    list.vote_periods = request.params[2].isNull() ? 1 : request.params[2].getInt<uint16_t>();
    list.scaled = !request.params[3].isNull() && request.params[3].get_bool();
    if (list.scaled) {
        list.min = request.params[4].isNull() ? 0 : request.params[4].getInt<int64_t>();
        list.max = request.params[5].isNull() ? 0 : request.params[5].getInt<int64_t>();
        if (list.min >= list.max) throw JSONRPCError(RPC_INVALID_PARAMETER, "The scale needs a low end below its high end");
    }
    list.author = NewAccount(*pwallet, "decision author");
    // The fee is the listing fee, more if the period is filling up; the voters get it.
    const auto [tx, index]{SendAction(*pwallet, list, pwallet->chain().getDecisionListingFee(list.event_period + list.vote_periods))};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("id", hivemind::MakeId(tx->GetHash(), index).GetHex());
    result.pushKV("author", EncodeDestination(WitnessV0KeyHash{list.author}));
    return result;
},
    };
}

RPCMethod createmarket()
{
    return RPCMethod{
        "createmarket",
        "Open a market on one or more decisions. This wallet funds the market maker with the most it can lose,\n"
        "liquidity * ln(number of states); it gets back what is left when the market is settled, and a quarter of the trading fees." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"text", RPCArg::Type::STR, RPCArg::Optional::NO, "What the market is about"},
            {"decisions", RPCArg::Type::ARR, RPCArg::Optional::NO, "The decisions; the market has two states for each",
                {{"id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "The id of a decision"}}},
            {"liquidity", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The liquidity of the market maker in " + CURRENCY_UNIT + ": the larger, the less a trade moves the price"},
            {"feebps", RPCArg::Type::NUM, RPCArg::Default{50}, "The trading fee in hundredths of a percent"},
            {"unchecked", RPCArg::Type::BOOL, RPCArg::Default{false}, "Send the transaction even if the market would not be opened as the chain stands, in which case the funding comes back"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The id of the transaction"},
            {RPCResult::Type::STR_HEX, "id", "The id of the market, once the transaction is mined"},
            {RPCResult::Type::STR_AMOUNT, "funding", "What the market maker was funded with"},
            {RPCResult::Type::STR_AMOUNT, "fee", "The opening fee, which goes to the voters"},
            {RPCResult::Type::STR, "author", "The address of this wallet that gets what the market has left"},
        }},
        RPCExamples{HelpExampleCli("createmarket", "\"Rain in Paris\" '[\"decisionid\"]' 10")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    hivemind::OpenMarket open;
    open.text = request.params[0].get_str();
    for (const UniValue& id : request.params[1].get_array().getValues()) open.decisions.push_back(ParseHashV(id, "decision"));
    const auto params{pwallet->chain().getHivemindParams()};
    if (open.decisions.empty() || open.decisions.size() > params.max_market_decisions) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("A market is about 1 to %u decisions", params.max_market_decisions));
    open.liquidity = AmountFromValue(request.params[2]);
    if (open.liquidity < params.min_liquidity) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The liquidity must be at least %s", FormatMoney(params.min_liquidity)));
    open.fee_bps = request.params[3].isNull() ? 50 : request.params[3].getInt<uint16_t>();
    // The chain would mine the transaction, open no market and pay the funding back: better to say why at once.
    if (request.params[4].isNull() || !request.params[4].get_bool()) {
        if (const std::string why{pwallet->chain().whyNoMarketOn(open.decisions)}; !why.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, why);
    }
    open.author = NewAccount(*pwallet, "market author");
    const CAmount funding{hivemind::LmsrFunding(open.liquidity, size_t{1} << open.decisions.size())};
    // And the opening fee, which goes to the voters: more, the more markets there are on the same first decision.
    const CAmount fee{pwallet->chain().getMarketOpeningFee(open.decisions)};
    const auto [tx, index]{SendAction(*pwallet, open, funding + fee)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("id", hivemind::MakeId(tx->GetHash(), index).GetHex());
    result.pushKV("funding", ValueFromAmount(funding));
    result.pushKV("fee", ValueFromAmount(fee));
    result.pushKV("author", EncodeDestination(WitnessV0KeyHash{open.author}));
    return result;
},
    };
}

RPCMethod buyshares()
{
    return RPCMethod{
        "buyshares",
        "Buy shares of a state of a market. A share pays one coin if its state comes true. The price moves with every\n"
        "trade; if, when the transaction is mined, the shares cost more than maxcost, nothing is bought and the coins come back.\n"
        "What maxcost leaves over the cost comes back too, less 0.00001 for the voters (a payout costs every node)." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"market", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The id of the market"},
            {"state", RPCArg::Type::NUM, RPCArg::Optional::NO, "The state; see getmarket"},
            {"shares", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The number of shares"},
            {"maxcost", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"1% above the current cost"}, "The most to pay, fee included"},
            {"account", RPCArg::Type::STR, RPCArg::DefaultHint{"a new address"}, "The address of this wallet that will hold the shares"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The id of the transaction"},
            {RPCResult::Type::STR, "account", "The account that will hold the shares, and that is paid when they pay"},
            {RPCResult::Type::STR_AMOUNT, "quote", "What the shares cost now"},
            {RPCResult::Type::STR_AMOUNT, "maxcost", "The most the transaction pays; the difference comes back"},
        }},
        RPCExamples{HelpExampleCli("buyshares", "\"marketid\" 1 3")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    hivemind::BuyShares buy;
    buy.market = ParseHashV(request.params[0], "market");
    buy.state = static_cast<uint8_t>(request.params[1].getInt<int>());
    buy.shares = AmountFromValue(request.params[2]);
    if (buy.shares <= 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "The number of shares must be positive");
    const auto quote{pwallet->chain().quoteMarketTrade(buy.market, buy.state, buy.shares)};
    if (!quote) throw JSONRPCError(RPC_INVALID_PARAMETER, pwallet->chain().whyNoMarketTrade(buy.market, buy.state, buy.shares));
    const CAmount max_cost{request.params[3].isNull() ? *quote + *quote / 100 + 1 : AmountFromValue(request.params[3])};
    // A new account holds no position: the chain would give the coins back, less a fee.
    if (request.params[4].isNull() && *quote < hivemind::MIN_POSITION) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("A purchase that opens a position costs at least %s: buy more shares", FormatMoney(hivemind::MIN_POSITION)));
    }
    buy.account = request.params[4].isNull() ? NewAccount(*pwallet, "market shares") : ParseTradeAccount(request.params[4]);
    const auto [tx, index]{SendAction(*pwallet, buy, max_cost)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("account", EncodeDestination(WitnessV0KeyHash{buy.account}));
    result.pushKV("quote", ValueFromAmount(*quote));
    result.pushKV("maxcost", ValueFromAmount(max_cost));
    return result;
},
    };
}

RPCMethod sellshares()
{
    return RPCMethod{
        "sellshares",
        "Sell shares back to the market maker of a market. The block that mines the transaction pays the proceeds to the account,\n"
        "less 0.00001 for the voters (a payout costs every node). A sale leaves the account no shares of the state, or shares\n"
        "worth at least 0.001, as a purchase that opens a position costs." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"market", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The id of the market"},
            {"state", RPCArg::Type::NUM, RPCArg::Optional::NO, "The state"},
            {"shares", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The number of shares"},
            {"account", RPCArg::Type::STR, RPCArg::Optional::NO, "The address of this wallet that holds the shares"},
            {"minproceeds", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"1% below the current proceeds"}, "The least to get, fees deducted, or nothing is sold"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The id of the transaction"},
            {RPCResult::Type::STR_AMOUNT, "quote", "What the shares yield now, trading fee deducted (see quotetrade)"},
            {RPCResult::Type::STR_AMOUNT, "proceeds", "What would be paid to the account now: the quote, less what a payout keeps for the voters"},
        }},
        RPCExamples{HelpExampleCli("sellshares", "\"marketid\" 1 3 \"address\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    hivemind::SellShares sell;
    sell.market = ParseHashV(request.params[0], "market");
    sell.state = static_cast<uint8_t>(request.params[1].getInt<int>());
    sell.shares = AmountFromValue(request.params[2]);
    if (sell.shares <= 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "The number of shares must be positive");
    sell.account = ParseTradeAccount(request.params[3]);
    const auto quote{pwallet->chain().quoteMarketTrade(sell.market, sell.state, -sell.shares)};
    if (!quote) throw JSONRPCError(RPC_INVALID_PARAMETER, pwallet->chain().whyNoMarketTrade(sell.market, sell.state, -sell.shares));
    if (const std::string why{pwallet->chain().whyNoMarketSale(sell.market, sell.account, sell.state, sell.shares)}; !why.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, why);
    }
    // From audit2_height a payout keeps PAYOUT_FEE for the voters: the least to get is of what is paid.
    const auto params{pwallet->chain().getHivemindParams()};
    const bool audit2{pwallet->chain().getHeight().value_or(-1) + 1 >= params.audit2_height};
    const CAmount quoted{-*quote};
    if (quoted <= 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Selling these shares would bring nothing");
    // Less than the fee: the sale closes the position and pays nothing out.
    const CAmount proceeds{std::max<CAmount>(0, quoted - (audit2 ? hivemind::PAYOUT_FEE : 0))};
    sell.min_proceeds = request.params[4].isNull() ? proceeds - proceeds / 100 : AmountFromValue(request.params[4]);
    const auto [tx, index]{SendAction(*pwallet, sell, 0, sell.account)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("quote", ValueFromAmount(quoted));
    result.pushKV("proceeds", ValueFromAmount(proceeds));
    return result;
},
    };
}

RPCMethod transfervotecoin()
{
    return RPCMethod{
        "transfervotecoin",
        "Move reputation (votecoin) from an account of this wallet to another account.\n"
        "Reputation does not move while votes are revealed (the last third of a period): a transfer mined then is ignored." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"from", RPCArg::Type::STR, RPCArg::Optional::NO, "The address of this wallet that holds the reputation"},
            {"to", RPCArg::Type::STR, RPCArg::Optional::NO, "The address of the account to move it to"},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The amount of reputation"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {{RPCResult::Type::STR_HEX, "txid", "The id of the transaction"}}},
        RPCExamples{HelpExampleCli("transfervotecoin", "\"from\" \"to\" 10")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    hivemind::TransferReputation transfer;
    transfer.from = ParseAccount(request.params[0]);
    transfer.to = ParseAccount(request.params[1]);
    transfer.amount = AmountFromValue(request.params[2]);
    if (transfer.amount <= 0 || transfer.from == transfer.to) throw JSONRPCError(RPC_INVALID_PARAMETER, "Move a positive amount to another account");
    // The chain would ignore a transfer that leaves either account with less than a voter of its own holds.
    const auto params{pwallet->chain().getHivemindParams()};
    const int64_t least{params.reputation_supply / hivemind::MAX_VOTERS};
    const auto holdings{pwallet->chain().getReputation()};
    const auto held{[&](const Account& account) { const auto it{holdings.find(account)}; return it == holdings.end() ? int64_t{0} : it->second; }};
    // (More than the account holds, as the chain stands, is sent all the same: mined, it does nothing.)
    const int64_t left{held(transfer.from) - transfer.amount};
    if ((left > 0 && left < least) || held(transfer.to) + transfer.amount < least) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("A transfer leaves each account with no reputation or with at least %s", FormatMoney(least)));
    }
    const auto [tx, index]{SendAction(*pwallet, transfer, 0, transfer.from)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    return result;
},
    };
}

RPCMethod sealvote()
{
    return RPCMethod{
        "sealvote",
        "Vote on a decision, without showing the vote: commit to it. Possible from the period after the event of the\n"
        "decision until the last third of the last period of its vote, in which the vote has to be shown with revealvote.\n"
        "A later sealed vote on the same decision replaces the earlier one." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"account", RPCArg::Type::STR, RPCArg::Optional::NO, "The address of this wallet that holds the reputation"},
            {"decision", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The id of the decision"},
            {"answer", RPCArg::Type::STR, RPCArg::Optional::NO, "\"yes\" or \"no\"; a whole number on the scale of the decision; or \"unknown\" if the answer cannot be known yet"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {{RPCResult::Type::STR_HEX, "txid", "The id of the transaction"}}},
        RPCExamples{HelpExampleCli("sealvote", "\"address\" \"decisionid\" yes")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    hivemind::SealVote seal;
    seal.voter = ParseAccount(request.params[0]);
    seal.decision = ParseHashV(request.params[1], "decision");
    // A seal made while the votes are revealed would be counted in the next round, if the decision is
    // put off -- and the salt it used is that of this round, which revealing makes public.
    const auto sealable{pwallet->chain().getDecisionSealable(seal.decision)};
    if (!sealable) throw JSONRPCError(RPC_INVALID_PARAMETER, "There is no open decision with this id");
    if (!*sealable) throw JSONRPCError(RPC_MISC_ERROR, "The votes on this decision are being revealed: a vote can no longer be sealed in this period");
    seal.commitment = hivemind::VoteCommitment(seal.decision, seal.voter, ParseAnswer(request.params[2]), VoteSalt(*pwallet, seal.voter, seal.decision));
    const auto [tx, index]{SendAction(*pwallet, seal, 0, seal.voter)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    return result;
},
    };
}

RPCMethod revealvote()
{
    return RPCMethod{
        "revealvote",
        "Show a vote that was sealed. Possible in the last third of the last period of the vote on the decision.\n"
        "The answer has to be the one that was sealed; a vote that is not shown does not count." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"account", RPCArg::Type::STR, RPCArg::Optional::NO, "The address of this wallet that sealed the vote"},
            {"decision", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The id of the decision"},
            {"answer", RPCArg::Type::STR, RPCArg::Optional::NO, "The answer that was sealed"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {{RPCResult::Type::STR_HEX, "txid", "The id of the transaction"}}},
        RPCExamples{HelpExampleCli("revealvote", "\"address\" \"decisionid\" yes")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    hivemind::RevealVote reveal;
    reveal.voter = ParseAccount(request.params[0]);
    reveal.decision = ParseHashV(request.params[1], "decision");
    reveal.answer = ParseAnswer(request.params[2]);
    // Revealed before the reveal phase, a vote is ignored -- and is no secret any more.
    if (const auto revealable{pwallet->chain().getDecisionRevealable(reveal.decision)}; !revealable) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "No open decision with this hash");
    } else if (!*revealable) {
        throw JSONRPCError(RPC_MISC_ERROR, "Votes on this decision are revealed in the reveal phase of its last period; revealed earlier, a vote would not count and everyone would know it");
    }
    reveal.salt = VoteSalt(*pwallet, reveal.voter, reveal.decision);
    // A vote sealed just before the decision was put off again carries the salt of the round before.
    const auto commitment{pwallet->chain().getVoteCommitment(reveal.decision, reveal.voter)};
    if (!commitment) throw JSONRPCError(RPC_INVALID_PARAMETER, "This account sealed no vote on the decision");
    if (hivemind::VoteCommitment(reveal.decision, reveal.voter, reveal.answer, reveal.salt) != *commitment) {
        // A vote sealed with the salt of the round before -- mined after the decision was put off --
        // is no secret: that salt was made public by the reveals of that round. It is not revealed
        // here; it has to be sealed again, which takes the salt of this round.
        const auto rolls{pwallet->chain().getDecisionRolls(reveal.decision)};
        if (rolls && *rolls > 0) {
            const uint256 earlier{VoteSaltOfRound(*pwallet, reveal.voter, reveal.decision, *rolls - 1)};
            if (hivemind::VoteCommitment(reveal.decision, reveal.voter, reveal.answer, earlier) == *commitment) {
                throw JSONRPCError(RPC_MISC_ERROR, "This vote was sealed with the salt of the previous round of voting on the decision, which is public: "
                                                   "anyone can read it. Seal it again with sealvote while votes are sealed, and reveal that");
            }
        }
        throw JSONRPCError(RPC_INVALID_PARAMETER, "That is not the answer that was sealed");
    }
    const auto [tx, index]{SendAction(*pwallet, reveal, 0)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    return result;
},
    };
}

RPCMethod sealballot()
{
    return RPCMethod{
        "sealballot",
        "Vote on several decisions at once, without showing the votes: one commitment and one signature for all of\n"
        "them, which takes much less room than a sealed vote each. The decisions must all close in the same period.\n"
        "Reveal the ballot with revealballot, with the same answers, in the reveal phase of that period. A later\n"
        "ballot for the same period replaces this one; a later sealvote replaces it for its decision." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"account", RPCArg::Type::STR, RPCArg::Optional::NO, "The address of this wallet that holds the reputation"},
            {"votes", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::NO, "The answer to each decision",
                {
                    {"decision", RPCArg::Type::STR, RPCArg::Optional::NO, "The id of a decision, and as value \"yes\", \"no\", a whole number on its scale, or \"unknown\""},
                },
            },
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The id of the transaction"},
            {RPCResult::Type::NUM, "closeperiod", "The period in whose reveal phase the ballot is revealed"},
        }},
        RPCExamples{HelpExampleCli("sealballot", "\"address\" '{\"decisionid\": \"yes\", \"otherid\": 42}'")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    hivemind::SealBallot seal;
    seal.voter = ParseAccount(request.params[0]);
    const auto answers{ParseBallotAnswers(request.params[1].get_obj())};
    std::optional<uint32_t> close_period;
    std::vector<int64_t> ordered;
    for (const auto& [decision, answer] : answers) {
        const auto close{pwallet->chain().getDecisionClosePeriod(decision)};
        const auto sealable{pwallet->chain().getDecisionSealable(decision)};
        if (!close || !sealable) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("There is no open decision %s", decision.GetHex()));
        if (!*sealable) throw JSONRPCError(RPC_MISC_ERROR, strprintf("A vote on the decision %s cannot be sealed now: its vote has not started, or its votes are being revealed", decision.GetHex()));
        if (close_period && *close != *close_period) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The votes on the decisions of a ballot close in the same period: %s closes in period %u, not %u", decision.GetHex(), *close, *close_period));
        }
        close_period = *close;
        seal.decisions.push_back(decision);
        ordered.push_back(answer);
    }
    seal.close_period = *close_period;
    seal.commitment = hivemind::BallotCommitment(seal.voter, seal.close_period, seal.decisions, ordered, BallotSalt(*pwallet, seal.voter, seal.close_period, seal.decisions));
    const auto [tx, index]{SendAction(*pwallet, seal, 0, seal.voter)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("closeperiod", seal.close_period);
    return result;
},
    };
}

RPCMethod revealballot()
{
    return RPCMethod{
        "revealballot",
        "Show the votes of a ballot that was sealed. Possible in the reveal phase of the period in which its decisions close.\n"
        "The answers have to be those that were sealed; votes that are not shown do not count." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"account", RPCArg::Type::STR, RPCArg::Optional::NO, "The address of this wallet that sealed the ballot"},
            {"votes", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::NO, "The answer to each decision, as sealed",
                {
                    {"decision", RPCArg::Type::STR, RPCArg::Optional::NO, "The id of a decision, and as value its answer"},
                },
            },
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {{RPCResult::Type::STR_HEX, "txid", "The id of the transaction"}}},
        RPCExamples{HelpExampleCli("revealballot", "\"address\" '{\"decisionid\": \"yes\", \"otherid\": 42}'")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    hivemind::RevealBallot reveal;
    reveal.voter = ParseAccount(request.params[0]);
    const auto answers{ParseBallotAnswers(request.params[1].get_obj())};
    const auto close{pwallet->chain().getDecisionClosePeriod(answers.begin()->first)};
    if (!close) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("There is no open decision %s", answers.begin()->first.GetHex()));
    reveal.close_period = *close;
    const auto ballot{pwallet->chain().getBallot(reveal.close_period, reveal.voter)};
    if (!ballot) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("This account sealed no ballot for period %u", reveal.close_period));
    // Revealed before the reveal phase, the votes are ignored -- and are no secret any more.
    if (const auto revealable{pwallet->chain().getDecisionRevealable(answers.begin()->first)}; !revealable || !*revealable) {
        throw JSONRPCError(RPC_MISC_ERROR, "A ballot is revealed in the reveal phase of the period its decisions close in; revealed earlier, it would not count and everyone would know it");
    }
    const auto& [decisions, commitment]{*ballot};
    for (const hivemind::Id& decision : decisions) {
        const auto it{answers.find(decision)};
        if (it == answers.end()) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The ballot has a vote on %s: give its answer", decision.GetHex()));
        reveal.answers.push_back(it->second);
    }
    if (answers.size() != decisions.size()) throw JSONRPCError(RPC_INVALID_PARAMETER, "The ballot has votes on fewer decisions than that");
    reveal.salt = BallotSalt(*pwallet, reveal.voter, reveal.close_period, decisions);
    if (hivemind::BallotCommitment(reveal.voter, reveal.close_period, decisions, reveal.answers, reveal.salt) != commitment) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Those are not the answers that were sealed");
    }
    const auto [tx, index]{SendAction(*pwallet, reveal, 0)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    return result;
},
    };
}

RPCMethod getwalletreputation()
{
    return RPCMethod{
        "getwalletreputation",
        "Returns the accounts of this wallet that hold reputation (votecoin), in one call.",
        {},
        RPCResult{RPCResult::Type::ARR, "", "",
        {
            {RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "account", "The account"},
                {RPCResult::Type::STR_AMOUNT, "amount", "The reputation it holds"},
            }},
        }},
        RPCExamples{HelpExampleCli("getwalletreputation", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const std::shared_ptr<const CWallet> pwallet{GetWalletForJSONRPCRequest(request)};
    if (!pwallet) return UniValue::VNULL;
    const auto holdings{pwallet->chain().getReputation()};
    UniValue result(UniValue::VARR);
    LOCK(pwallet->cs_wallet);
    for (const auto& [account, amount] : holdings) {
        // An account is a key hash: the wallet's, whichever address of it the wallet has.
        if (!pwallet->IsMine(CTxDestination{WitnessV0KeyHash{account}}) && !pwallet->IsMine(CTxDestination{PKHash{account}})) continue;
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("account", EncodeDestination(WitnessV0KeyHash{account}));
        obj.pushKV("amount", ValueFromAmount(amount));
        result.push_back(std::move(obj));
    }
    return result;
},
    };
}

} // namespace wallet
