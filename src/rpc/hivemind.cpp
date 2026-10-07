// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <core_io.h>
#include <hivemind/math.h>
#include <hivemind/state.h>
#include <key_io.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <univalue.h>
#include <validation.h>
#include <limits>
#include <algorithm>

using hivemind::Account;
using hivemind::Decision;
using hivemind::Id;
using hivemind::Market;
using hivemind::ONE;

namespace {

std::string AccountAddress(const Account& account) { return EncodeDestination(WitnessV0KeyHash{account}); }

Account ParseAccount(const UniValue& value)
{
    const CTxDestination dest{DecodeDestination(value.get_str())};
    if (const auto* keyhash{std::get_if<WitnessV0KeyHash>(&dest)}) return Account{*keyhash};
    // A P2PKH account is accepted, but it can only sign with a compressed key: see CheckSignature.
    if (const auto* keyhash{std::get_if<PKHash>(&dest)}) return Account{*keyhash};
    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "An account is the address of a single key (P2WPKH or P2PKH)");
}

/** The state of the markets at the tip, and the chain parameters. */
struct View {
    hivemind::State state;
    Consensus::HivemindParams params;
    int height;
};

View GetView(const JSONRPCRequest& request)
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    if (!chainman.GetConsensus().sidechain.enabled) throw JSONRPCError(RPC_MISC_ERROR, "This chain is not running as a sidechain, so it has no markets");
    LOCK(::cs_main);
    return View{chainman.ActiveChainstate().m_scdb.m_side.Hivemind(), chainman.GetConsensus().sidechain.hivemind, chainman.ActiveHeight()};
}

std::string DecisionStatus(const Decision& decision, uint32_t period)
{
    if (decision.resolved) return "resolved";
    if (period <= decision.listing.event_period) return "open";
    return "voting";
}

UniValue Fraction(int64_t value) { return ValueFromAmount(value); }

UniValue DecisionToJSON(const Id& id, const Decision& decision, const View& view)
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("id", id.GetHex());
    obj.pushKV("text", decision.listing.text);
    obj.pushKV("author", AccountAddress(decision.listing.author));
    obj.pushKV("scaled", decision.listing.scaled);
    if (decision.listing.scaled) {
        obj.pushKV("min", decision.listing.min);
        obj.pushKV("max", decision.listing.max);
    }
    obj.pushKV("eventperiod", decision.listing.event_period);
    obj.pushKV("voteperiods", decision.listing.vote_periods);
    obj.pushKV("closeperiod", decision.close_period);
    obj.pushKV("rolls", decision.rolls);
    obj.pushKV("height", decision.height);
    obj.pushKV("status", DecisionStatus(decision, view.state.Period()));
    if (decision.resolved) {
        obj.pushKV("outcome", Fraction(decision.outcome));
        if (decision.listing.scaled) {
            const __int128 range{static_cast<__int128>(decision.listing.max) - decision.listing.min};
            obj.pushKV("answer", static_cast<int64_t>(decision.listing.min + range * decision.outcome / ONE));
        } else {
            obj.pushKV("answer", decision.outcome == ONE ? "yes" : decision.outcome == 0 ? "no" : "tie");
        }
    }
    return obj;
}

UniValue MarketToJSON(const Id& id, const Market& market, const View& view)
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("id", id.GetHex());
    obj.pushKV("text", market.opening.text);
    obj.pushKV("author", AccountAddress(market.opening.author));
    UniValue decisions(UniValue::VARR);
    for (const Id& decision : market.opening.decisions) decisions.push_back(decision.GetHex());
    obj.pushKV("decisions", std::move(decisions));
    obj.pushKV("liquidity", ValueFromAmount(market.opening.liquidity));
    obj.pushKV("feebps", market.opening.fee_bps);
    obj.pushKV("height", market.height);
    obj.pushKV("status", market.settled ? "settled" : view.state.TradingOpen(market) ? "trading" : "closed");
    // Trading stops when the first of the decisions is up for the vote.
    uint32_t trading_until{std::numeric_limits<uint32_t>::max()};
    for (const Id& decision : market.opening.decisions) {
        const auto it{view.state.Decisions().find(decision)};
        if (it != view.state.Decisions().end()) trading_until = std::min(trading_until, it->second.listing.event_period);
    }
    if (trading_until != std::numeric_limits<uint32_t>::max()) obj.pushKV("tradinguntil", trading_until);
    obj.pushKV("treasury", ValueFromAmount(market.treasury));
    obj.pushKV("fees", ValueFromAmount(market.fees));
    UniValue states(UniValue::VARR);
    const std::vector<int64_t> prices{market.settled ? std::vector<int64_t>(market.shares.size(), 0) : hivemind::LmsrPrices(market.shares, market.opening.liquidity)};
    for (size_t s{0}; s < market.shares.size(); ++s) {
        UniValue state(UniValue::VOBJ);
        state.pushKV("state", s);
        // Which answer to each decision the state stands for.
        std::string name;
        for (size_t d{0}; d < market.opening.decisions.size(); ++d) name += ((s >> d) & 1) ? "Y" : "N";
        state.pushKV("answers", name);
        state.pushKV("shares", ValueFromAmount(market.shares[s]));
        if (!market.settled) state.pushKV("price", Fraction(prices[s]));
        states.push_back(std::move(state));
    }
    obj.pushKV("states", std::move(states));
    return obj;
}

RPCMethod gethivemindinfo()
{
    return RPCMethod{
        "gethivemindinfo",
        "Returns where the markets are in the cycle of voting, and their parameters.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "period", "The current voting period"},
            {RPCResult::Type::STR, "phase", "\"seal\" while votes are sealed, \"reveal\" in the last third of a period, when the votes that end in it are revealed"},
            {RPCResult::Type::NUM, "periodseconds", /*optional=*/true, "The length of a period in seconds of mainchain time"},
            {RPCResult::Type::NUM, "starttime", /*optional=*/true, "The time at which period 0 started, where periods are counted in time: period n ends at starttime + (n + 1) * periodseconds"},
            {RPCResult::Type::NUM, "periodblocks", /*optional=*/true, "The length of a period in blocks, on test chains"},
            {RPCResult::Type::NUM, "decisions", "The number of decisions listed"},
            {RPCResult::Type::NUM, "markets", "The number of markets opened"},
            {RPCResult::Type::NUM, "voters", "The number of accounts that hold reputation"},
            {RPCResult::Type::STR_AMOUNT, "voterpot", "Fees waiting for the voters of the next vote"},
            {RPCResult::Type::STR_AMOUNT, "listingfee", "What listing a decision costs"},
            {RPCResult::Type::NUM, "maxvoteperiods", "The most periods a decision can be voted on for"},
            {RPCResult::Type::NUM, "maxperiodsahead", "How many periods after the current one the event of a decision can be at most"},
        }},
        RPCExamples{HelpExampleCli("gethivemindinfo", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const View view{GetView(request)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("period", view.state.Period());
    result.pushKV("phase", view.state.RevealPhase() ? "reveal" : "seal");
    if (view.params.period_blocks > 0) {
        result.pushKV("periodblocks", view.params.period_blocks);
    } else {
        result.pushKV("periodseconds", view.params.period_seconds);
        result.pushKV("starttime", view.params.start_time);
    }
    result.pushKV("decisions", view.state.Decisions().size());
    result.pushKV("markets", view.state.Markets().size());
    result.pushKV("voters", view.state.Reputation().size());
    result.pushKV("voterpot", ValueFromAmount(view.state.VoterPot()));
    result.pushKV("listingfee", ValueFromAmount(view.params.listing_fee));
    result.pushKV("maxvoteperiods", view.params.max_vote_periods);
    result.pushKV("maxperiodsahead", view.params.max_periods_ahead);
    return result;
},
    };
}

RPCMethod listdecisions()
{
    return RPCMethod{
        "listdecisions",
        "Returns the decisions: the questions that markets are about and that the holders of reputation vote on.",
        {
            {"status", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Only those that are \"open\" (the event has not come), \"voting\" or \"resolved\""},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::ANY, "", "A decision, as getdecision returns it but for the votes"}}},
        RPCExamples{HelpExampleCli("listdecisions", "voting")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const View view{GetView(request)};
    UniValue result(UniValue::VARR);
    for (const auto& [id, decision] : view.state.Decisions()) {
        if (!request.params[0].isNull() && DecisionStatus(decision, view.state.Period()) != request.params[0].get_str()) continue;
        result.push_back(DecisionToJSON(id, decision, view));
    }
    return result;
},
    };
}

RPCMethod getdecision()
{
    return RPCMethod{
        "getdecision",
        "Returns a decision and the votes on it.",
        {{"id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The id of the decision"}},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "id", "The id"},
            {RPCResult::Type::STR, "text", "The question"},
            {RPCResult::Type::STR, "author", "The account that gets the share of fees of the author"},
            {RPCResult::Type::BOOL, "scaled", "Whether the answer is a point on a scale, as opposed to yes or no"},
            {RPCResult::Type::NUM, "min", /*optional=*/true, "The low end of the scale"},
            {RPCResult::Type::NUM, "max", /*optional=*/true, "The high end of the scale"},
            {RPCResult::Type::NUM, "eventperiod", "The period in which the answer becomes known"},
            {RPCResult::Type::NUM, "voteperiods", "The number of periods it is voted on for"},
            {RPCResult::Type::NUM, "closeperiod", "The last period of the vote; it is resolved by the first block after it"},
            {RPCResult::Type::NUM, "rolls", "How often it was put off because its answer was not known"},
            {RPCResult::Type::NUM, "height", "The height of the block that listed it"},
            {RPCResult::Type::STR, "status", "\"open\", \"voting\" or \"resolved\""},
            {RPCResult::Type::STR_AMOUNT, "outcome", /*optional=*/true, "The outcome as a fraction: 0 for no, 1 for yes, 0.5 for a tie, or where on the scale"},
            {RPCResult::Type::ANY, "answer", /*optional=*/true, "The outcome in words, or as a point of the scale"},
            {RPCResult::Type::ARR, "votes", "The votes",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR, "voter", "The account that voted"},
                    {RPCResult::Type::BOOL, "revealed", "Whether the vote was shown, as opposed to sealed"},
                    {RPCResult::Type::ANY, "answer", /*optional=*/true, "The answer, if revealed"},
                }},
            }},
        }},
        RPCExamples{HelpExampleCli("getdecision", "\"id\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const View view{GetView(request)};
    const Id id{ParseHashV(request.params[0], "id")};
    const auto it{view.state.Decisions().find(id)};
    if (it == view.state.Decisions().end()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No such decision");
    UniValue result{DecisionToJSON(id, it->second, view)};
    UniValue votes(UniValue::VARR);
    for (const auto& [key, vote] : view.state.Votes()) {
        if (key.decision != id) continue;
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("voter", AccountAddress(key.voter));
        obj.pushKV("revealed", vote.revealed);
        if (vote.revealed) {
            if (vote.answer == hivemind::ANSWER_UNKNOWN) {
                obj.pushKV("answer", "unknown");
            } else {
                obj.pushKV("answer", vote.answer);
            }
        }
        votes.push_back(std::move(obj));
    }
    result.pushKV("votes", std::move(votes));
    return result;
},
    };
}

RPCMethod listmarkets()
{
    return RPCMethod{
        "listmarkets",
        "Returns the markets, with the price of each of their states.",
        {
            {"status", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Only those that are \"trading\", \"closed\" (waiting for the vote) or \"settled\""},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::ANY, "", "A market, as getmarket returns it but for the positions"}}},
        RPCExamples{HelpExampleCli("listmarkets", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const View view{GetView(request)};
    UniValue result(UniValue::VARR);
    for (const auto& [id, market] : view.state.Markets()) {
        UniValue obj{MarketToJSON(id, market, view)};
        if (!request.params[0].isNull() && obj["status"].get_str() != request.params[0].get_str()) continue;
        result.push_back(std::move(obj));
    }
    return result;
},
    };
}

RPCMethod getmarket()
{
    return RPCMethod{
        "getmarket",
        "Returns a market and who holds its shares.",
        {{"id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The id of the market"}},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "id", "The id"},
            {RPCResult::Type::STR, "text", "What the market is about"},
            {RPCResult::Type::STR, "author", "The account that funded the market maker"},
            {RPCResult::Type::ARR, "decisions", "The decisions the market is about", {{RPCResult::Type::STR_HEX, "", "The id of a decision"}}},
            {RPCResult::Type::STR_AMOUNT, "liquidity", "The liquidity of the market maker"},
            {RPCResult::Type::NUM, "feebps", "The trading fee in hundredths of a percent"},
            {RPCResult::Type::NUM, "height", "The height of the block that opened it"},
            {RPCResult::Type::STR, "status", "\"trading\", \"closed\" or \"settled\""},
            {RPCResult::Type::NUM, "tradinguntil", /*optional=*/true, "The period at the end of which trading stops: the first in which one of the decisions has its event"},
            {RPCResult::Type::STR_AMOUNT, "treasury", "What the market maker holds"},
            {RPCResult::Type::STR_AMOUNT, "fees", "The trading fees collected"},
            {RPCResult::Type::ARR, "states", "The states of the market",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::NUM, "state", "The number of the state"},
                    {RPCResult::Type::STR, "answers", "The answer to each decision that the state stands for: Y (yes, or high) or N"},
                    {RPCResult::Type::STR_AMOUNT, "shares", "The shares sold"},
                    {RPCResult::Type::STR_AMOUNT, "price", /*optional=*/true, "The price of a share, which is the chance the market gives the state"},
                }},
            }},
            {RPCResult::Type::ARR, "positions", "The shares held",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR, "account", "The account"},
                    {RPCResult::Type::NUM, "state", "The state"},
                    {RPCResult::Type::STR_AMOUNT, "shares", "The shares it holds"},
                }},
            }},
        }},
        RPCExamples{HelpExampleCli("getmarket", "\"id\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const View view{GetView(request)};
    const Id id{ParseHashV(request.params[0], "id")};
    const auto it{view.state.Markets().find(id)};
    if (it == view.state.Markets().end()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No such market");
    UniValue result{MarketToJSON(id, it->second, view)};
    UniValue positions(UniValue::VARR);
    for (const auto& [key, shares] : view.state.Positions()) {
        if (key.market != id) continue;
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("account", AccountAddress(key.account));
        obj.pushKV("state", key.state);
        obj.pushKV("shares", ValueFromAmount(shares));
        positions.push_back(std::move(obj));
    }
    result.pushKV("positions", std::move(positions));
    return result;
},
    };
}

RPCMethod quotetrade()
{
    return RPCMethod{
        "quotetrade",
        "Returns what buying or selling shares of a state of a market costs now, the trading fee included.\n"
        "The price moves with every trade, so by the time a trade is mined it may cost more.",
        {
            {"market", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The id of the market"},
            {"state", RPCArg::Type::NUM, RPCArg::Optional::NO, "The state"},
            {"shares", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The number of shares; negative to sell"},
        },
        RPCResult{RPCResult::Type::STR_AMOUNT, "", "What the trade costs; negative when it pays"},
        RPCExamples{HelpExampleCli("quotetrade", "\"id\" 1 3")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const View view{GetView(request)};
    const UniValue& amount{request.params[2]};
    const bool selling{amount.getValStr().starts_with("-")};
    UniValue magnitude;
    magnitude.setNumStr(selling ? amount.getValStr().substr(1) : amount.getValStr());
    const CAmount shares{AmountFromValue(magnitude)};
    const uint256 market{ParseHashV(request.params[0], "market")};
    const auto state{static_cast<uint8_t>(request.params[1].getInt<int>())};
    const auto quote{view.state.Quote(market, state, selling ? -shares : shares)};
    if (!quote) throw JSONRPCError(RPC_INVALID_PARAMETER, view.state.WhyNoTrade(market, state, selling ? -shares : shares));
    return ValueFromAmount(*quote);
},
    };
}

RPCMethod listreputation()
{
    return RPCMethod{
        "listreputation",
        "Returns who holds the reputation (votecoin) that votes on decisions.",
        {},
        RPCResult{RPCResult::Type::ARR, "", "",
        {
            {RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "account", "The account"},
                {RPCResult::Type::STR_AMOUNT, "amount", "The reputation it holds"},
            }},
        }},
        RPCExamples{HelpExampleCli("listreputation", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const View view{GetView(request)};
    UniValue result(UniValue::VARR);
    for (const auto& [account, amount] : view.state.Reputation()) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("account", AccountAddress(account));
        obj.pushKV("amount", ValueFromAmount(amount));
        result.push_back(std::move(obj));
    }
    return result;
},
    };
}

RPCMethod listpositions()
{
    return RPCMethod{
        "listpositions",
        "Returns the shares an account holds in markets that are not settled.",
        {{"account", RPCArg::Type::STR, RPCArg::Optional::NO, "The address of the account"}},
        RPCResult{RPCResult::Type::ARR, "", "",
        {
            {RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "market", "The market"},
                {RPCResult::Type::NUM, "state", "The state"},
                {RPCResult::Type::STR_AMOUNT, "shares", "The shares held"},
            }},
        }},
        RPCExamples{HelpExampleCli("listpositions", "\"address\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const View view{GetView(request)};
    const Account account{ParseAccount(request.params[0])};
    UniValue result(UniValue::VARR);
    for (const auto& [key, shares] : view.state.Positions()) {
        if (key.account != account) continue;
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("market", key.market.GetHex());
        obj.pushKV("state", key.state);
        obj.pushKV("shares", ValueFromAmount(shares));
        result.push_back(std::move(obj));
    }
    return result;
},
    };
}

} // namespace

void RegisterHivemindRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"hivemind", &gethivemindinfo},
        {"hivemind", &listdecisions},
        {"hivemind", &getdecision},
        {"hivemind", &listmarkets},
        {"hivemind", &getmarket},
        {"hivemind", &quotetrade},
        {"hivemind", &listreputation},
        {"hivemind", &listpositions},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
