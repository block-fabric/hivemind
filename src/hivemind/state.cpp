// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <hivemind/state.h>

#include <tinyformat.h>
#include <addresstype.h>
#include <common/signmessage.h>
#include <hash.h>
#include <hivemind/math.h>
#include <pubkey.h>
#include <streams.h>
#include <util/strencodings.h>

#include <algorithm>
#include <set>

namespace hivemind {
namespace {

/** The output that pays an account. */
CTxOut PayAccount(const Account& account, CAmount amount)
{
    return CTxOut{amount, GetScriptForDestination(WitnessV0KeyHash{account})};
}

template <typename T>
void SerializeAction(DataStream& stream, const T& action)
{
    stream << T::KIND << action;
}

/** The action without its signature: what the signature is of. */
template <typename T>
T Unsigned(T action)
{
    action.signature.clear();
    return action;
}

template <typename T>
bool CheckSignature(const T& action, const Account& signer, const COutPoint& first_input)
{
    CPubKey pubkey;
    return action.signature.size() == CPubKey::COMPACT_SIGNATURE_SIZE &&
           pubkey.RecoverCompact(MessageHash(ActionMessage(Action{Unsigned(action)}, first_input)), action.signature) &&
           pubkey.IsCompressed() && pubkey.GetID() == CKeyID{signer};
}

} // namespace

bool operator==(const Decision& a, const Decision& b)
{
    return ActionScript(a.listing) == ActionScript(b.listing) && a.height == b.height && a.close_period == b.close_period &&
           a.rolls == b.rolls && a.resolved == b.resolved && a.outcome == b.outcome && a.fee == b.fee;
}

bool operator==(const Market& a, const Market& b)
{
    return ActionScript(a.opening) == ActionScript(b.opening) && a.height == b.height && a.shares == b.shares &&
           a.treasury == b.treasury && a.fees == b.fees && a.settled == b.settled && a.settled_period == b.settled_period;
}

bool operator==(const State& a, const State& b)
{
    return a.m_started == b.m_started && a.m_period == b.m_period && a.m_reveal_phase == b.m_reveal_phase &&
           a.m_decisions == b.m_decisions && a.m_markets == b.m_markets && a.m_positions == b.m_positions &&
           a.m_votes == b.m_votes && a.m_reputation == b.m_reputation && a.m_voter_pot == b.m_voter_pot;
}

CScript ActionScript(const Action& action)
{
    DataStream stream;
    std::visit([&](const auto& a) { SerializeAction(stream, a); }, action);
    std::vector<unsigned char> data{std::begin(TAG), std::end(TAG)};
    data.insert(data.end(), UCharCast(stream.data()), UCharCast(stream.data()) + stream.size());
    return CScript() << OP_RETURN << data;
}

namespace {
std::optional<std::vector<unsigned char>> ActionData(const CScript& script)
{
    CScript::const_iterator pc{script.begin()};
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, opcode) || opcode != OP_RETURN) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || opcode > OP_PUSHDATA4 || pc != script.end()) return std::nullopt;
    if (data.size() < sizeof(TAG) || !std::equal(std::begin(TAG), std::end(TAG), data.begin())) return std::nullopt;
    data.erase(data.begin(), data.begin() + sizeof(TAG));
    return data;
}

template <typename T>
std::optional<Action> ParseAs(DataStream& stream)
{
    T action;
    stream >> action;
    if (!stream.empty()) return std::nullopt;
    return Action{std::move(action)};
}
} // namespace

bool IsActionScript(const CScript& script)
{
    return ActionData(script).has_value();
}

std::optional<Action> ParseActionScript(const CScript& script)
{
    const auto data{ActionData(script)};
    if (!data || data->empty()) return std::nullopt;
    try {
        DataStream stream{*data};
        uint8_t kind;
        stream >> kind;
        switch (kind) {
        case ListDecision::KIND: return ParseAs<ListDecision>(stream);
        case OpenMarket::KIND: return ParseAs<OpenMarket>(stream);
        case BuyShares::KIND: return ParseAs<BuyShares>(stream);
        case SellShares::KIND: return ParseAs<SellShares>(stream);
        case TransferReputation::KIND: return ParseAs<TransferReputation>(stream);
        case SealVote::KIND: return ParseAs<SealVote>(stream);
        case RevealVote::KIND: return ParseAs<RevealVote>(stream);
        default: return std::nullopt;
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string ActionMessage(const Action& action, const COutPoint& first_input)
{
    HashWriter hasher;
    hasher << first_input << ActionScript(action);
    return "Hivemind " + hasher.GetHash().GetHex();
}

uint256 VoteCommitment(const Id& decision, const Account& voter, int64_t answer, const uint256& salt)
{
    HashWriter hasher;
    hasher << decision << voter << answer << salt;
    return hasher.GetHash();
}

Id MakeId(const Txid& txid, uint32_t n)
{
    HashWriter hasher;
    hasher << txid << n;
    return hasher.GetHash();
}

bool IsValidAnswer(const ListDecision& decision, int64_t answer)
{
    if (answer == ANSWER_UNKNOWN) return true;
    if (!decision.scaled) return answer == 0 || answer == 1;
    return answer >= decision.min && answer <= decision.max;
}

int64_t AnswerFraction(const ListDecision& decision, int64_t answer)
{
    if (!decision.scaled) return answer == 0 ? 0 : ONE;
    const __int128 range{static_cast<__int128>(decision.max) - decision.min};
    return static_cast<int64_t>((static_cast<__int128>(answer) - decision.min) * ONE / range);
}

int64_t StatePayout(const std::vector<int64_t>& outcomes, size_t state)
{
    // A state is a yes or no (high or low) for each decision. It pays the product of
    // how far each decision went its way.
    __int128 payout{ONE};
    for (size_t d{0}; d < outcomes.size(); ++d) {
        payout = payout * (((state >> d) & 1) ? outcomes[d] : ONE - outcomes[d]) / ONE;
    }
    return static_cast<int64_t>(payout);
}

Clock ClockAt(int height, int64_t main_time, const Consensus::HivemindParams& params)
{
    Clock clock;
    if (params.period_blocks > 0) {
        clock.period = static_cast<uint32_t>(height / params.period_blocks);
        clock.reveal_phase = (height % params.period_blocks) * 3 >= params.period_blocks * SEAL_PARTS;
    } else {
        const int64_t elapsed{std::max<int64_t>(0, main_time - params.start_time)};
        clock.period = static_cast<uint32_t>(elapsed / params.period_seconds);
        clock.reveal_phase = (elapsed % params.period_seconds) * 3 >= params.period_seconds * SEAL_PARTS;
    }
    return clock;
}

bool CheckTx(const CTransaction& tx, const Consensus::HivemindParams& params, std::string& reject_reason)
{
    const auto invalid{[&](const char* reason) {
        reject_reason = reason;
        return false;
    }};
    size_t actions{0};
    for (const CTxOut& out : tx.vout) {
        if (!IsActionScript(out.scriptPubKey)) continue;
        if (++actions > MAX_ACTIONS_PER_TX) return invalid("bad-hm-too-many-actions");
        const auto action{ParseActionScript(out.scriptPubKey)};
        if (!action) return invalid("bad-hm-format");
        if (tx.IsCoinBase()) return invalid("bad-hm-coinbase");
        const COutPoint& first_input{tx.vin[0].prevout};

        if (const auto* list{std::get_if<ListDecision>(&*action)}) {
            if (list->text.empty() || list->text.size() > params.max_text_size) return invalid("bad-hm-text");
            if (list->vote_periods < 1 || list->vote_periods > params.max_vote_periods) return invalid("bad-hm-vote-periods");
            if (list->scaled ? list->min >= list->max : (list->min != 0 || list->max != 0)) return invalid("bad-hm-scale");
            // The lowest number stands for "unknown" in a vote: it cannot be a point of the scale.
            if (list->scaled && list->min == ANSWER_UNKNOWN) return invalid("bad-hm-scale");
            if (out.nValue < params.listing_fee) return invalid("bad-hm-listing-fee");
        } else if (const auto* open{std::get_if<OpenMarket>(&*action)}) {
            if (open->text.empty() || open->text.size() > params.max_text_size) return invalid("bad-hm-text");
            if (open->decisions.empty() || open->decisions.size() > params.max_market_decisions) return invalid("bad-hm-market-decisions");
            // The market maker handles so many states at most, whatever the parameter says.
            if ((size_t{1} << open->decisions.size()) > MAX_STATES) return invalid("bad-hm-market-decisions");
            if (std::set<Id>{open->decisions.begin(), open->decisions.end()}.size() != open->decisions.size()) return invalid("bad-hm-market-decisions");
            if (open->liquidity < params.min_liquidity || open->liquidity > MAX_MONEY) return invalid("bad-hm-liquidity");
            if (open->fee_bps > params.max_fee_bps) return invalid("bad-hm-fee");
            if (out.nValue <= 0) return invalid("bad-hm-funding");
        } else if (const auto* buy{std::get_if<BuyShares>(&*action)}) {
            if (buy->shares <= 0 || buy->shares > MAX_MONEY || buy->state >= MAX_STATES) return invalid("bad-hm-shares");
            if (out.nValue < MIN_PURCHASE) return invalid("bad-hm-payment");
        } else if (const auto* sell{std::get_if<SellShares>(&*action)}) {
            if (sell->shares <= 0 || sell->shares > MAX_MONEY || sell->state >= MAX_STATES) return invalid("bad-hm-shares");
            if (sell->min_proceeds < 0 || !MoneyRange(sell->min_proceeds) || out.nValue != 0) return invalid("bad-hm-proceeds");
            if (!CheckSignature(*sell, sell->account, first_input)) return invalid("bad-hm-signature");
        } else if (const auto* transfer{std::get_if<TransferReputation>(&*action)}) {
            if (transfer->amount <= 0 || transfer->amount > params.reputation_supply || transfer->from == transfer->to || out.nValue != 0) return invalid("bad-hm-transfer");
            if (!CheckSignature(*transfer, transfer->from, first_input)) return invalid("bad-hm-signature");
        } else if (const auto* seal{std::get_if<SealVote>(&*action)}) {
            if (out.nValue != 0) return invalid("bad-hm-vote");
            if (!CheckSignature(*seal, seal->voter, first_input)) return invalid("bad-hm-signature");
        } else if (std::holds_alternative<RevealVote>(*action)) {
            if (out.nValue != 0) return invalid("bad-hm-vote");
        }
    }
    return true;
}

bool State::TradingOpen(const Market& market) const
{
    if (market.settled) return false;
    // Trading stops when the first of the decisions is up for the vote: from then on
    // those who vote would know more than those who trade.
    for (const Id& id : market.opening.decisions) {
        const auto it{m_decisions.find(id)};
        if (it == m_decisions.end() || it->second.resolved || m_period > it->second.listing.event_period) return false;
    }
    return true;
}

std::string State::WhyNoTrade(const Id& market_id, uint8_t state, int64_t shares) const
{
    const auto it{m_markets.find(market_id)};
    if (it == m_markets.end()) return "There is no such market on the chain. One that was just opened has to be mined first.";
    const Market& market{it->second};
    if (market.settled) return strprintf("The market \"%s\" is settled: it has paid out.", market.opening.text);
    // A market maker has no stock of shares: it sells as many as are asked for while trading is open, and none after.
    for (const Id& id : market.opening.decisions) {
        const auto decision{m_decisions.find(id)};
        if (decision == m_decisions.end()) continue;
        if (decision->second.resolved || m_period > decision->second.listing.event_period) {
            return strprintf("Trading in the market \"%s\" is closed: its decision \"%s\" was about period %u, and the chain is in period %u. "
                             "From the end of that period on the answer is voted on, and shares are neither sold nor bought back until the market pays out.",
                             market.opening.text, decision->second.listing.text, decision->second.listing.event_period, m_period);
        }
    }
    if (state >= market.shares.size()) return strprintf("The market has the states 0 to %u.", unsigned(market.shares.size() - 1));
    if (shares == 0) return "The number of shares must not be zero.";
    if (shares < 0 && market.shares[state] + shares < 0) return "The market has not sold as many shares of this state as that.";
    return {};
}

std::optional<CAmount> State::Quote(const Id& market_id, uint8_t state, int64_t shares) const
{
    const auto it{m_markets.find(market_id)};
    if (it == m_markets.end() || !TradingOpen(it->second) || state >= it->second.shares.size() || shares == 0) return std::nullopt;
    const Market& market{it->second};
    std::vector<int64_t> after{market.shares};
    after[state] += shares;
    if (after[state] < 0) return std::nullopt;
    const CAmount cost{LmsrTradeCost(market.shares, after, market.opening.liquidity)};
    const CAmount fee{(std::abs(cost) * market.opening.fee_bps + 9999) / 10000};
    return cost + fee;
}

size_t State::ClosingIn(uint32_t close_period) const
{
    return static_cast<size_t>(std::count_if(m_decisions.begin(), m_decisions.end(), [&](const auto& entry) {
        return !entry.second.resolved && entry.second.close_period == close_period;
    }));
}

CAmount State::ListingFee(uint32_t close_period, const Consensus::HivemindParams& params) const
{
    return params.listing_fee << (std::min(ClosingIn(close_period), MAX_DECISIONS_PER_PERIOD) / LISTING_FEE_STEP);
}

CAmount State::MarketFee(const std::vector<Id>& decisions, const Consensus::HivemindParams& params) const
{
    if (decisions.empty()) return params.listing_fee;
    // Counted by every decision of a market, whatever its place: a market on D could otherwise be
    // made dear by markets that merely have D second, or made cheap by putting a fresh decision first.
    std::map<Id, size_t> on_decision;
    for (const auto& [id, market] : m_markets) {
        if (market.settled) continue;
        for (const Id& decision : market.opening.decisions) ++on_decision[decision];
    }
    size_t most{0};
    for (const Id& decision : decisions) {
        if (const auto it{on_decision.find(decision)}; it != on_decision.end()) most = std::max(most, it->second);
    }
    const size_t doublings{most / MARKET_FEE_STEP};
    // Beyond what any amount of coins can pay, it stays there.
    if (doublings >= 40 || (params.listing_fee << doublings) > MAX_MONEY) return MAX_MONEY;
    return params.listing_fee << doublings;
}

bool State::ApplyTx(const CTransaction& tx, int height, const Consensus::HivemindParams& params,
                    std::vector<CTxOut>& payouts, std::string& reject_reason)
{
    if (!CheckTx(tx, params, reject_reason)) return false;
    for (uint32_t n{0}; n < tx.vout.size(); ++n) {
        const CTxOut& out{tx.vout[n]};
        const auto action{ParseActionScript(out.scriptPubKey)};
        if (!action) continue;
        // What cannot be done is not done, and the coins burned for it go back.
        // Less a small part, for the voters: payouts cost every block room, and refunds by the
        // thousand would hold back everyone else's.
        const auto refund{[&](const Account& account) {
            const CAmount kept{std::min(out.nValue, REFUND_FEE)};
            m_voter_pot += kept;
            if (out.nValue > kept) payouts.push_back(PayAccount(account, out.nValue - kept));
        }};

        if (const auto* list{std::get_if<ListDecision>(&*action)}) {
            if (list->event_period < m_period || list->event_period > m_period + params.max_periods_ahead) {
                refund(list->author);
                continue;
            }
            // Bounded: what closes in one period is resolved at once, by everyone; and priced, so
            // that filling a period costs more the fuller it gets.
            const uint32_t close_period{list->event_period + list->vote_periods};
            if (ClosingIn(close_period) >= MAX_DECISIONS_PER_PERIOD || out.nValue < ListingFee(close_period, params)) {
                refund(list->author);
                continue;
            }
            Decision decision;
            decision.listing = *list;
            decision.height = height;
            decision.close_period = list->event_period + list->vote_periods;
            decision.fee = out.nValue;
            m_decisions.emplace(MakeId(tx.GetHash(), n), std::move(decision));
        } else if (const auto* open{std::get_if<OpenMarket>(&*action)}) {
            Market market;
            market.opening = *open;
            market.height = height;
            market.shares.assign(size_t{1} << open->decisions.size(), 0);
            const CAmount funding{LmsrFunding(open->liquidity, market.shares.size())};
            // A market is opened on decisions that are still open themselves.
            bool open_decisions{true};
            for (const Id& id : open->decisions) {
                const auto it{m_decisions.find(id)};
                open_decisions = open_decisions && it != m_decisions.end() && !it->second.resolved && m_period <= it->second.listing.event_period;
            }
            const CAmount fee{open_decisions ? MarketFee(open->decisions, params) : 0};
            if (!open_decisions || out.nValue < funding || out.nValue - funding < fee) {
                refund(open->author);
                continue;
            }
            market.treasury = funding;
            // The opening fee goes to those who vote on the decisions of the market, with their
            // listing fees; the rest back to the author.
            const CAmount per_decision{fee / static_cast<CAmount>(open->decisions.size())};
            for (size_t k{0}; k < open->decisions.size(); ++k) {
                m_decisions.at(open->decisions[k]).fee += per_decision + (k == 0 ? fee - per_decision * static_cast<CAmount>(open->decisions.size()) : 0);
            }
            if (out.nValue > funding + fee) payouts.push_back(PayAccount(open->author, out.nValue - funding - fee));
            m_markets.emplace(MakeId(tx.GetHash(), n), std::move(market));
        } else if (const auto* buy{std::get_if<BuyShares>(&*action)}) {
            const auto it{m_markets.find(buy->market)};
            if (it == m_markets.end() || !TradingOpen(it->second) || buy->state >= it->second.shares.size()) {
                refund(buy->account);
                continue;
            }
            Market& market{it->second};
            std::vector<int64_t> after{market.shares};
            after[buy->state] += buy->shares;
            const CAmount cost{LmsrTradeCost(market.shares, after, market.opening.liquidity)};
            const CAmount fee{(cost * market.opening.fee_bps + 9999) / 10000};
            // The price may have moved past what the buyer is ready to pay. And whatever the
            // rounding, a market never owes more than it holds.
            if (cost <= 0 || cost + fee > out.nValue || !MoneyRange(market.treasury + cost) ||
                (height >= params.audit_height && cost < MIN_PURCHASE) ||
                market.treasury + cost < *std::max_element(after.begin(), after.end())) {
                refund(buy->account);
                continue;
            }
            market.shares = std::move(after);
            market.treasury += cost;
            market.fees += fee;
            m_positions[PositionKey{buy->market, buy->account, buy->state}] += buy->shares;
            if (out.nValue > cost + fee) payouts.push_back(PayAccount(buy->account, out.nValue - cost - fee));
        } else if (const auto* sell{std::get_if<SellShares>(&*action)}) {
            const auto it{m_markets.find(sell->market)};
            const auto position{m_positions.find(PositionKey{sell->market, sell->account, sell->state})};
            if (it == m_markets.end() || !TradingOpen(it->second) || position == m_positions.end() || position->second < sell->shares) continue;
            Market& market{it->second};
            std::vector<int64_t> after{market.shares};
            after[sell->state] -= sell->shares;
            const CAmount proceeds{-LmsrTradeCost(market.shares, after, market.opening.liquidity)};
            const CAmount fee{(proceeds * market.opening.fee_bps + 9999) / 10000};
            if (proceeds <= 0 || proceeds - fee < sell->min_proceeds || proceeds - fee < 0 || proceeds > market.treasury ||
                market.treasury - proceeds < *std::max_element(after.begin(), after.end())) continue;
            market.shares = std::move(after);
            market.treasury -= proceeds;
            market.fees += fee;
            position->second -= sell->shares;
            if (position->second == 0) m_positions.erase(position);
            if (proceeds > fee) payouts.push_back(PayAccount(sell->account, proceeds - fee));
        } else if (const auto* transfer{std::get_if<TransferReputation>(&*action)}) {
            // Reputation does not move while votes are revealed: it would let a holder put it
            // behind whichever revealed vote turned out best. The period ends, and the votes are
            // weighed, with the last block of the reveal phase.
            if (m_reveal_phase) continue;
            const auto from{m_reputation.find(transfer->from)};
            if (from == m_reputation.end() || from->second < transfer->amount) continue;
            from->second -= transfer->amount;
            if (from->second == 0) m_reputation.erase(from);
            m_reputation[transfer->to] += transfer->amount;
        } else if (const auto* seal{std::get_if<SealVote>(&*action)}) {
            const auto it{m_decisions.find(seal->decision)};
            if (it == m_decisions.end() || it->second.resolved) continue;
            const Decision& decision{it->second};
            // Votes are sealed from the period after the event until the last third of the last period.
            const bool in_window{m_period > decision.listing.event_period && m_period <= decision.close_period};
            if (!in_window || (m_period == decision.close_period && m_reveal_phase)) continue;
            // Only reputation that counts in the resolution seals a vote: a seal costs every node.
            if (height >= params.audit_height) {
                const auto held{m_reputation.find(seal->voter)};
                if (held == m_reputation.end() || held->second < params.reputation_supply / MAX_VOTERS) continue;
            }
            m_votes[VoteKey{seal->decision, seal->voter}] = Vote{seal->commitment, false, 0};
        } else if (const auto* reveal{std::get_if<RevealVote>(&*action)}) {
            const auto it{m_decisions.find(reveal->decision)};
            const auto vote{m_votes.find(VoteKey{reveal->decision, reveal->voter})};
            if (it == m_decisions.end() || it->second.resolved || vote == m_votes.end() || vote->second.revealed) continue;
            const Decision& decision{it->second};
            if (m_period != decision.close_period || !m_reveal_phase) continue;
            // Only the vote that was sealed counts.
            if (!IsValidAnswer(decision.listing, reveal->answer) || VoteCommitment(reveal->decision, reveal->voter, reveal->answer, reveal->salt) != vote->second.commitment) continue;
            vote->second.revealed = true;
            vote->second.answer = reveal->answer;
        }
    }
    return true;
}

void State::Settle(const Id& id, Market& market, std::map<Account, CAmount>& paid)
{
    std::vector<int64_t> outcomes;
    for (const Id& decision : market.opening.decisions) outcomes.push_back(m_decisions.at(decision).outcome);

    // Every share is paid what its state came to. The positions of a market are next to
    // each other in the map, in the order of the accounts.
    CAmount left{market.treasury};
    auto position{m_positions.lower_bound(PositionKey{id, Account{}, 0})};
    while (position != m_positions.end() && position->first.market == id) {
        const __int128 due{static_cast<__int128>(position->second) * StatePayout(outcomes, position->first.state) / ONE};
        // The treasury covers what is due; the bound is there in case it ever did not.
        const CAmount payment{static_cast<CAmount>(std::min<__int128>(due, left))};
        if (payment > 0) {
            paid[position->first.account] += payment;
            left -= payment;
        }
        position = m_positions.erase(position);
    }
    // What the market maker did not lose goes back to who funded it.
    if (left > 0) paid[market.opening.author] += left;

    // The trading fees: half to the voters, a quarter to the authors of the decisions,
    // the rest to the author of the market.
    const CAmount to_voters{market.fees / 2};
    const CAmount per_decision{(market.fees / 4) / static_cast<CAmount>(market.opening.decisions.size())};
    CAmount to_market_author{market.fees - to_voters};
    for (const Id& decision : market.opening.decisions) {
        if (per_decision > 0) paid[m_decisions.at(decision).listing.author] += per_decision;
        to_market_author -= per_decision;
    }
    m_voter_pot += to_voters;
    if (to_market_author > 0) paid[market.opening.author] += to_market_author;

    market.treasury = 0;
    market.fees = 0;
    market.settled = true;
}

void State::Resolve(uint32_t new_period, uint32_t first_open, const Consensus::HivemindParams& params, std::map<Account, CAmount>& paid)
{
    // The decisions whose vote is over.
    std::vector<Id> batch;
    for (const auto& [id, decision] : m_decisions) {
        if (!decision.resolved && decision.close_period < new_period) batch.push_back(id);
    }
    if (batch.empty()) return;

    // Everyone who holds reputation takes part. Those who revealed a vote on these decisions
    // are voters, in the order of the accounts; the others are scored alike, as having
    // skipped every vote, and stand together as one more voter at the end, so that spreading
    // reputation over many accounts adds nothing to the work.
    std::set<Account> voted;
    for (const Id& id : batch) {
        for (auto vote{m_votes.lower_bound(VoteKey{id, Account{}})}; vote != m_votes.end() && vote->first.decision == id; ++vote) {
            if (vote->second.revealed) voted.insert(vote->first.voter);
        }
    }
    std::vector<Account> voters, silent;
    std::vector<int64_t> reputation, silent_reputation;
    int64_t silent_total{0};
    // A voter needs a share of the reputation to be a row of its own: splitting reputation over
    // ever more accounts would otherwise make the resolution as large as one likes. Smaller holders
    // are scored with those who did not vote.
    const int64_t min_voter{params.reputation_supply / MAX_VOTERS};
    for (const auto& [account, amount] : m_reputation) {
        if (amount <= 0) continue;
        if (voted.contains(account) && amount >= min_voter) {
            voters.push_back(account);
            reputation.push_back(amount);
        } else {
            silent.push_back(account);
            silent_reputation.push_back(amount);
            silent_total += amount;
        }
    }
    if (silent_total > 0) reputation.push_back(silent_total);
    std::vector<bool> scaled, last_round;
    for (const Id& id : batch) {
        scaled.push_back(m_decisions.at(id).listing.scaled);
        last_round.push_back(m_decisions.at(id).rolls >= params.max_rolls);
    }
    std::vector<std::vector<int64_t>> votes(reputation.size(), std::vector<int64_t>(batch.size(), VOTE_MISSING));
    for (size_t j{0}; j < batch.size(); ++j) {
        const Decision& decision{m_decisions.at(batch[j])};
        for (size_t i{0}; i < voters.size(); ++i) {
            const auto vote{m_votes.find(VoteKey{batch[j], voters[i]})};
            // A vote that was sealed and not revealed is a vote not cast.
            if (vote == m_votes.end() || !vote->second.revealed) continue;
            votes[i][j] = vote->second.answer == ANSWER_UNKNOWN ? VOTE_UNKNOWN : AnswerFraction(decision.listing, vote->second.answer);
        }
    }

    const int64_t quorum{static_cast<int64_t>(static_cast<__int128>(params.reputation_supply) * QUORUM_PERCENT / 100)};
    const Resolution resolution{hivemind::Resolve(reputation, votes, scaled, params.alpha_num, params.alpha_den, params.tolerance, quorum, last_round)};

    const auto set_reputation{[&](const Account& account, int64_t amount) {
        if (amount > 0) {
            m_reputation[account] = amount;
        } else {
            m_reputation.erase(account);
        }
    }};
    for (size_t i{0}; i < voters.size(); ++i) set_reputation(voters[i], resolution.reputation[i]);
    if (silent_total > 0) {
        // What the silent come out with is theirs in the proportions they went in with.
        const std::vector<int64_t> shares{Share(silent_reputation, resolution.reputation.back())};
        for (size_t i{0}; i < silent.size(); ++i) set_reputation(silent[i], shares[i]);
    }
    for (size_t j{0}; j < batch.size(); ++j) {
        Decision& decision{m_decisions.at(batch[j])};
        // The votes are done with, whatever comes of them.
        auto vote{m_votes.lower_bound(VoteKey{batch[j], Account{}})};
        while (vote != m_votes.end() && vote->first.decision == batch[j]) vote = m_votes.erase(vote);

        if (resolution.outcomes[j] == OUTCOME_ROLL && decision.rolls < params.max_rolls) {
            // The answer is not known yet: the decision is voted again, for as long as before, from
            // the first period that has a time for sealing -- not in periods the chain has skipped.
            ++decision.rolls;
            decision.close_period = std::max(new_period, first_open) - 1 + decision.listing.vote_periods;
            // Into the first period from then on that has room.
            while (std::count_if(m_decisions.begin(), m_decisions.end(), [&](const auto& entry) {
                       return !entry.second.resolved && entry.first != batch[j] && entry.second.close_period == decision.close_period;
                   }) >= static_cast<std::ptrdiff_t>(MAX_DECISIONS_PER_PERIOD)) {
                ++decision.close_period;
            }
            continue;
        }
        decision.resolved = true;
        // A decision that was put off too often is settled as a tie.
        decision.outcome = resolution.outcomes[j] == OUTCOME_ROLL ? ONE / 2 : resolution.outcomes[j];
        m_voter_pot += decision.fee;
        decision.fee = 0;
    }

    // The markets that are decided now pay.
    for (auto& [id, market] : m_markets) {
        if (market.settled) continue;
        const bool decided{std::all_of(market.opening.decisions.begin(), market.opening.decisions.end(), [&](const Id& decision) { return m_decisions.at(decision).resolved; })};
        if (decided) {
            Settle(id, market, paid);
            market.settled_period = new_period;
        }
    }

    // The fees for the voters go to those who voted, by the reputation they come out with.
    // If nobody voted they wait for the next vote.
    __int128 weight{0};
    for (const int64_t share : resolution.participation) weight += share;
    if (weight > 0 && m_voter_pot > 0) {
        CAmount dealt{0};
        for (size_t i{0}; i < voters.size(); ++i) {
            const CAmount share{static_cast<CAmount>(static_cast<__int128>(m_voter_pot) * resolution.participation[i] / weight)};
            if (share > 0) paid[voters[i]] += share;
            dealt += share;
        }
        // What rounding left over waits too.
        m_voter_pot -= dealt;
    }
}

void State::BeginBlock(int height, int64_t main_time, const Consensus::HivemindParams& params, std::vector<CTxOut>& payouts)
{
    if (!m_started) {
        m_started = true;
        if (params.reputation_supply > 0) m_reputation[params.genesis_account] = params.reputation_supply;
    }
    const Clock clock{ClockAt(height, main_time, params)};
    // The clock of the mainchain can go back a little; a period never does, and nor does the
    // phase within a period: once votes are revealed, none can be sealed again until the next.
    if (clock.period > m_period) {
        // A decision whose votes could never be revealed -- its last period went by without a
        // block of this chain in the reveal phase, as after a stall or a jump of the clock -- is
        // not put off, which would use up its rounds and in the end force a tie nobody voted on:
        // it is voted on in the first period from now that still has a time for sealing.
        const uint32_t first_open{clock.period + (clock.reveal_phase ? 1 : 0)};
        for (auto& [id, decision] : m_decisions) {
            if (decision.resolved || decision.close_period > clock.period) continue;
            if (decision.close_period == m_period && m_reveal_phase) continue;
            // Closing in this period, which the clock entered in its reveal phase: if every period
            // of its vote came after the last block, nobody could seal a vote on it.
            if (decision.close_period == clock.period && (!clock.reveal_phase || decision.close_period + 1 <= m_period + decision.listing.vote_periods)) continue;
            uint32_t close_period{first_open - 1 + decision.listing.vote_periods};
            while (ClosingIn(close_period) >= MAX_DECISIONS_PER_PERIOD) ++close_period;
            decision.close_period = close_period;
        }
        std::map<Account, CAmount> paid;
        // Periods the chain skipped are resolved one after the other, each on its own decisions.
        for (uint32_t period{m_period + 1}; period <= clock.period; ++period) Resolve(period, first_open, params, paid);
        m_period = clock.period;
        m_reveal_phase = clock.reveal_phase;
        for (const auto& [account, amount] : paid) {
            // Dust goes to the voters: a payout costs every node, and a settlement could make many.
            if (height >= params.audit_height && amount < MIN_PAYOUT) {
                m_voter_pot += amount;
                continue;
            }
            payouts.push_back(PayAccount(account, amount));
        }
        Prune();
    } else if (clock.period == m_period) {
        m_reveal_phase = m_reveal_phase || clock.reveal_phase;
    }
}

void State::Prune()
{
    // Settled markets are kept for a while, to be looked at, then dropped; so are resolved
    // decisions, once no market that is still there is on them.
    std::set<Id> in_use;
    for (auto it{m_markets.begin()}; it != m_markets.end();) {
        const Market& market{it->second};
        if (market.settled && market.settled_period + PRUNE_AFTER_PERIODS < m_period) {
            it = m_markets.erase(it);
            continue;
        }
        in_use.insert(market.opening.decisions.begin(), market.opening.decisions.end());
        ++it;
    }
    std::erase_if(m_decisions, [&](const auto& entry) {
        const Decision& decision{entry.second};
        return decision.resolved && decision.close_period + PRUNE_AFTER_PERIODS < m_period && !in_use.contains(entry.first);
    });
}

namespace {
/** The entries of `before` that `after` does not have the same: their values in `before`, nullopt where `before` has none. */
template <typename Key, typename Value>
std::vector<std::pair<Key, std::optional<Value>>> Changes(const std::map<Key, Value>& before, const std::map<Key, Value>& after)
{
    std::vector<std::pair<Key, std::optional<Value>>> changes;
    auto b{before.begin()};
    auto a{after.begin()};
    while (b != before.end() || a != after.end()) {
        if (a == after.end() || (b != before.end() && b->first < a->first)) {
            changes.emplace_back(b->first, b->second);
            ++b;
        } else if (b == before.end() || a->first < b->first) {
            changes.emplace_back(a->first, std::nullopt);
            ++a;
        } else {
            if (!(b->second == a->second)) changes.emplace_back(b->first, b->second);
            ++b;
            ++a;
        }
    }
    return changes;
}

template <typename Key, typename Value>
void Apply(std::map<Key, Value>& map, const std::vector<std::pair<Key, std::optional<Value>>>& changes)
{
    for (const auto& [key, value] : changes) {
        if (value) {
            map.insert_or_assign(key, *value);
        } else {
            map.erase(key);
        }
    }
}
} // namespace

StateUndo State::Diff(const State& before, const State& after)
{
    StateUndo undo;
    undo.started = before.m_started;
    undo.period = before.m_period;
    undo.reveal_phase = before.m_reveal_phase;
    undo.voter_pot = before.m_voter_pot;
    undo.decisions = Changes(before.m_decisions, after.m_decisions);
    undo.markets = Changes(before.m_markets, after.m_markets);
    undo.positions = Changes(before.m_positions, after.m_positions);
    undo.votes = Changes(before.m_votes, after.m_votes);
    undo.reputation = Changes(before.m_reputation, after.m_reputation);
    return undo;
}

void State::Revert(const StateUndo& undo)
{
    m_started = undo.started;
    m_period = undo.period;
    m_reveal_phase = undo.reveal_phase;
    m_voter_pot = undo.voter_pot;
    Apply(m_decisions, undo.decisions);
    Apply(m_markets, undo.markets);
    Apply(m_positions, undo.positions);
    Apply(m_votes, undo.votes);
    Apply(m_reputation, undo.reputation);
}

} // namespace hivemind
