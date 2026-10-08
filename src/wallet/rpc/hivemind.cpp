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
        // Signed for the next block, which is where the transaction goes: the message names the chain from audit2_height.
        const int next_height{wallet.chain().getHeight().value_or(-1) + 1};
        action.signature = Sign(wallet, hivemind::ActionMessage(Action{action}, mtx.vin[0].prevout, wallet.chain().getHivemindParams(), next_height), *Assert(signer));
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
        "trade; if, when the transaction is mined, the shares cost more than maxcost, nothing is bought and the coins come back." +
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
        "Sell shares back to the market maker of a market. The block that mines the transaction pays the proceeds to the account." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"market", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The id of the market"},
            {"state", RPCArg::Type::NUM, RPCArg::Optional::NO, "The state"},
            {"shares", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The number of shares"},
            {"account", RPCArg::Type::STR, RPCArg::Optional::NO, "The address of this wallet that holds the shares"},
            {"minproceeds", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"1% below the current proceeds"}, "The least to get, fee deducted, or nothing is sold"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The id of the transaction"},
            {RPCResult::Type::STR_AMOUNT, "quote", "What the shares yield now"},
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
    if (*quote >= 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Selling these shares would bring nothing");
    const CAmount proceeds{-*quote};
    sell.min_proceeds = request.params[4].isNull() ? proceeds - proceeds / 100 : AmountFromValue(request.params[4]);
    const auto [tx, index]{SendAction(*pwallet, sell, 0, sell.account)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("quote", ValueFromAmount(proceeds));
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
        const auto rolls{pwallet->chain().getDecisionRolls(reveal.decision)};
        bool found{false};
        if (rolls && *rolls > 0) {
            const uint256 earlier{VoteSaltOfRound(*pwallet, reveal.voter, reveal.decision, *rolls - 1)};
            if (hivemind::VoteCommitment(reveal.decision, reveal.voter, reveal.answer, earlier) == *commitment) {
                reveal.salt = earlier;
                found = true;
            }
        }
        if (!found) throw JSONRPCError(RPC_INVALID_PARAMETER, "That is not the answer that was sealed");
    }
    const auto [tx, index]{SendAction(*pwallet, reveal, 0)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    return result;
},
    };
}

} // namespace wallet
