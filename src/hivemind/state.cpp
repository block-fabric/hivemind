// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <hivemind/state.h>

#include <util/check.h>

#include <tinyformat.h>
#include <addresstype.h>
#include <common/signmessage.h>
#include <hash.h>
#include <hivemind/math.h>
#include <pubkey.h>
#include <streams.h>
#include <util/moneystr.h>
#include <util/strencodings.h>

#include <algorithm>
#include <numeric>
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
bool CheckSignature(const T& action, const Account& signer, const COutPoint& first_input, uint32_t n, const uint256& outputs_hash,
                    const Consensus::HivemindParams& params, int height)
{
    CPubKey pubkey;
    return action.signature.size() == CPubKey::COMPACT_SIGNATURE_SIZE &&
           pubkey.RecoverCompact(MessageHash(ActionMessage(Action{Unsigned(action)}, first_input, n, outputs_hash, params, height)), action.signature) &&
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
        case SealBallot::KIND: return ParseAs<SealBallot>(stream);
        case RevealBallot::KIND: return ParseAs<RevealBallot>(stream);
        default: return std::nullopt;
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string ActionMessage(const Action& action, const COutPoint& first_input, uint32_t n, const uint256& outputs_hash,
                          const Consensus::HivemindParams& params, int height)
{
    HashWriter hasher;
    if (height >= params.audit2_height) {
        // The chain and the kind of action, ahead of the rest: a signature for another chain made of
        // this code, or for another kind of action, is not one for this. Then the output that asks for
        // it, and what the transaction pays: the one who spends the first input could otherwise repeat
        // the action in other outputs, or pay otherwise than the signer agreed to.
        const uint8_t kind{std::visit([](const auto& a) { return std::decay_t<decltype(a)>::KIND; }, action)};
        hasher << std::string{"Hivemind action"} << params.genesis_hash << kind << n << outputs_hash;
    }
    hasher << first_input << ActionScript(action);
    return "Hivemind " + hasher.GetHash().GetHex();
}

uint256 ActionOutputsHash(const std::vector<CTxOut>& outputs)
{
    HashWriter hasher;
    hasher << std::string{"Hivemind outputs"};
    WriteCompactSize(hasher, outputs.size());
    for (const CTxOut& out : outputs) {
        CScript script{out.scriptPubKey};
        if (IsActionScript(script)) {
            if (const auto action{ParseActionScript(script)}) {
                script = ActionScript(std::visit([](auto a) {
                    if constexpr (requires { a.signature; }) a.signature.clear();
                    return Action{std::move(a)};
                }, *action));
            }
        }
        hasher << out.nValue << script;
    }
    return hasher.GetHash();
}

uint256 VoteCommitment(const Id& decision, const Account& voter, int64_t answer, const uint256& salt)
{
    HashWriter hasher;
    hasher << decision << voter << answer << salt;
    return hasher.GetHash();
}

uint256 BallotCommitment(const Account& voter, uint32_t close_period, const std::vector<Id>& decisions, const std::vector<int64_t>& answers, const uint256& salt)
{
    HashWriter hasher;
    hasher << std::string{"Hivemind ballot"} << voter << close_period << decisions << answers << salt;
    return hasher.GetHash();
}

CAmount TradeFee(CAmount amount, uint16_t fee_bps)
{
    const __int128 fee{(static_cast<__int128>(amount) * fee_bps + 9999) / 10000};
    // Of an amount that is any money at all, the fee is too; of more, it is beyond what anyone can pay.
    return static_cast<CAmount>(std::clamp<__int128>(fee, -MAX_MONEY, MAX_MONEY));
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

bool CheckTx(const CTransaction& tx, int height, const Consensus::HivemindParams& params, std::string& reject_reason)
{
    const auto invalid{[&](const char* reason) {
        reject_reason = reason;
        return false;
    }};
    size_t actions{0};
    // What signatures name of the outputs (from audit2_height), made once for the transaction.
    std::optional<uint256> outputs_hash;
    for (uint32_t n{0}; n < tx.vout.size(); ++n) {
        const CTxOut& out{tx.vout[n]};
        if (!IsActionScript(out.scriptPubKey)) continue;
        if (++actions > MAX_ACTIONS_PER_TX) return invalid("bad-hm-too-many-actions");
        const auto action{ParseActionScript(out.scriptPubKey)};
        if (!action) return invalid("bad-hm-format");
        if (tx.IsCoinBase()) return invalid("bad-hm-coinbase");
        const COutPoint& first_input{tx.vin[0].prevout};
        const auto signed_by{[&](const auto& a, const Account& signer) {
            if (!outputs_hash) outputs_hash = height >= params.audit2_height ? ActionOutputsHash(tx.vout) : uint256{};
            return CheckSignature(a, signer, first_input, n, *outputs_hash, params, height);
        }};
        // Ballots are of the rules of audit2_height: before, they are what they were, no request at all.
        if (height < params.audit2_height && (std::holds_alternative<SealBallot>(*action) || std::holds_alternative<RevealBallot>(*action))) {
            return invalid("bad-hm-format");
        }

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
            if (!signed_by(*sell, sell->account)) return invalid("bad-hm-signature");
        } else if (const auto* transfer{std::get_if<TransferReputation>(&*action)}) {
            if (transfer->amount <= 0 || transfer->amount > params.reputation_supply || transfer->from == transfer->to || out.nValue != 0) return invalid("bad-hm-transfer");
            if (!signed_by(*transfer, transfer->from)) return invalid("bad-hm-signature");
        } else if (const auto* seal{std::get_if<SealVote>(&*action)}) {
            if (out.nValue != 0) return invalid("bad-hm-vote");
            if (!signed_by(*seal, seal->voter)) return invalid("bad-hm-signature");
        } else if (std::holds_alternative<RevealVote>(*action)) {
            if (out.nValue != 0) return invalid("bad-hm-vote");
        } else if (const auto* ballot{std::get_if<SealBallot>(&*action)}) {
            // At most the decisions that can close in a period, each once.
            if (out.nValue != 0 || ballot->decisions.empty() || ballot->decisions.size() > MAX_DECISIONS_PER_PERIOD ||
                std::set<Id>{ballot->decisions.begin(), ballot->decisions.end()}.size() != ballot->decisions.size()) return invalid("bad-hm-vote");
            if (!signed_by(*ballot, ballot->voter)) return invalid("bad-hm-signature");
        } else if (const auto* reveal{std::get_if<RevealBallot>(&*action)}) {
            if (out.nValue != 0 || reveal->answers.empty() || reveal->answers.size() > MAX_DECISIONS_PER_PERIOD) return invalid("bad-hm-vote");
        }
    }
    return true;
}

namespace {
const sidechain::Table<uint256, Decision> DECISIONS{0x40};
const sidechain::Table<uint256, Market> MARKETS{0x41};
const sidechain::Table<PositionKey, int64_t> POSITIONS{0x42};
const sidechain::Table<VoteKey, Vote> VOTES{0x43};
const sidechain::Table<uint160, int64_t> REPUTATION{0x44};
const sidechain::Cell<bool> STARTED{0x45, false};
const sidechain::Cell<uint32_t> PERIOD{0x46, 0};
const sidechain::Cell<bool> REVEAL{0x47, false};
const sidechain::Cell<CAmount> VOTER_POT{0x48, 0};
const sidechain::Table<BallotKey, Ballot> BALLOTS{0x4e};
//! The last period that votes an account sealed close in (from audit2_height), until it is over.
const sidechain::Table<uint160, uint32_t> SEALED{0x4f};
constexpr uint8_t OPEN_BY_CLOSE{0x49};
constexpr uint8_t RESOLVED_BY_CLOSE{0x4a};
constexpr uint8_t MARKETS_BY_DECISION{0x4b};
constexpr uint8_t UNSETTLED_BY_DECISION{0x4c};
constexpr uint8_t SETTLED_BY_PERIOD{0x4d};

sidechain::StoreBytes PeriodKey(uint8_t table, uint32_t period, const Id& id)
{
    sidechain::StoreBytes key{table};
    sidechain::KeyCodec<uint32_t>::Encode(key, period);
    sidechain::KeyCodec<uint256>::Encode(key, id);
    return key;
}
sidechain::StoreBytes PairKey(uint8_t table, const Id& a, const Id& b)
{
    sidechain::StoreBytes key{table};
    sidechain::KeyCodec<uint256>::Encode(key, a);
    sidechain::KeyCodec<uint256>::Encode(key, b);
    return key;
}
sidechain::StoreBytes Head(uint8_t table, const Id& a)
{
    sidechain::StoreBytes key{table};
    sidechain::KeyCodec<uint256>::Encode(key, a);
    return key;
}
/** The ids at the end of the keys of an index, from `from` while `keep` says so. */
std::vector<Id> IdsIn(const sidechain::StoreView& view, const sidechain::StoreBytes& prefix, const std::function<bool(std::span<const unsigned char>)>& keep = {})
{
    std::vector<Id> ids;
    view.ForEach(prefix, [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
        std::span<const unsigned char> rest{key};
        rest = rest.subspan(1);
        if (keep && !keep(rest)) return false;
        std::span<const unsigned char> last{key};
        last = last.subspan(key.size() - 32);
        ids.push_back(sidechain::KeyCodec<uint256>::Decode(last));
        return true;
    });
    return ids;
}
} // namespace

sidechain::StoreOverlay& State::Writable() const { return *Assert(m_overlay); }
std::optional<Decision> State::GetDecision(const Id& id) const { return DECISIONS.Get(*m_view, id); }
std::optional<Market> State::GetMarket(const Id& id) const { return MARKETS.Get(*m_view, id); }
int64_t State::PositionOf(const PositionKey& key) const { return POSITIONS.Get(*m_view, key).value_or(0); }
std::optional<Vote> State::GetVote(const VoteKey& key) const { return VOTES.Get(*m_view, key); }
std::optional<Ballot> State::GetBallot(const BallotKey& key) const { return BALLOTS.Get(*m_view, key); }
int64_t State::ReputationOf(const Account& account) const { return REPUTATION.Get(*m_view, account).value_or(0); }
uint32_t State::Period() const { return PERIOD.Get(*m_view); }
std::optional<uint32_t> State::SealedUntil(const Account& account) const
{
    const auto until{SEALED.Get(*m_view, account)};
    if (!until || *until < Period()) return std::nullopt;
    return until;
}
void State::SealFor(const Account& account, uint32_t close_period)
{
    if (const auto until{SEALED.Get(*m_view, account)}; !until || *until < close_period) SEALED.Put(Writable(), account, close_period);
}
bool State::RevealPhase() const { return REVEAL.Get(*m_view); }
CAmount State::VoterPot() const { return VOTER_POT.Get(*m_view); }
std::map<Id, Decision> State::Decisions() const
{
    std::map<Id, Decision> all;
    ForEachDecision([&](const Id& id, const Decision& d) { all.emplace(id, d); return true; });
    return all;
}
std::map<Id, Market> State::Markets() const
{
    std::map<Id, Market> all;
    ForEachMarket([&](const Id& id, const Market& m) { all.emplace(id, m); return true; });
    return all;
}
std::map<Account, int64_t> State::Reputation() const
{
    std::map<Account, int64_t> all;
    ForEachReputation([&](const Account& a, int64_t r) { all.emplace(a, r); return true; });
    return all;
}
void State::ForEachDecision(const std::function<bool(const Id&, const Decision&)>& fn) const { DECISIONS.ForEach(*m_view, fn); }
void State::ForEachMarket(const std::function<bool(const Id&, const Market&)>& fn) const { MARKETS.ForEach(*m_view, fn); }
void State::ForEachPosition(const std::function<bool(const PositionKey&, int64_t)>& fn) const
{
    POSITIONS.ForEach(*m_view, [&](const PositionKey& k, const int64_t& v) { return fn(k, v); });
}
void State::ForEachPositionIn(const Id& market, const std::function<bool(const PositionKey&, int64_t)>& fn) const
{
    POSITIONS.ForEachWith(*m_view, market, [&](const PositionKey& k, const int64_t& v) { return fn(k, v); });
}
void State::ForEachVoteOn(const Id& decision, const std::function<bool(const VoteKey&, const Vote&)>& fn) const { VOTES.ForEachWith(*m_view, decision, fn); }
void State::ForEachReputation(const std::function<bool(const Account&, int64_t)>& fn) const
{
    REPUTATION.ForEach(*m_view, [&](const Account& k, const int64_t& v) { return fn(k, v); });
}

void State::SetDecision(const Id& id, const std::optional<Decision>& decision)
{
    sidechain::StoreOverlay& out{Writable()};
    if (const auto old{GetDecision(id)}) out.Erase(PeriodKey(old->resolved ? RESOLVED_BY_CLOSE : OPEN_BY_CLOSE, old->close_period, id));
    if (decision) {
        DECISIONS.Put(out, id, *decision);
        out.Put(PeriodKey(decision->resolved ? RESOLVED_BY_CLOSE : OPEN_BY_CLOSE, decision->close_period, id), {});
    } else {
        DECISIONS.Erase(out, id);
    }
}

void State::SetMarket(const Id& id, const std::optional<Market>& market)
{
    sidechain::StoreOverlay& out{Writable()};
    if (const auto old{GetMarket(id)}) {
        for (const Id& decision : old->opening.decisions) {
            out.Erase(PairKey(MARKETS_BY_DECISION, decision, id));
            if (!old->settled) out.Erase(PairKey(UNSETTLED_BY_DECISION, decision, id));
        }
        if (old->settled) out.Erase(PeriodKey(SETTLED_BY_PERIOD, old->settled_period, id));
    }
    if (market) {
        MARKETS.Put(out, id, *market);
        for (const Id& decision : market->opening.decisions) {
            out.Put(PairKey(MARKETS_BY_DECISION, decision, id), {});
            if (!market->settled) out.Put(PairKey(UNSETTLED_BY_DECISION, decision, id), {});
        }
        if (market->settled) out.Put(PeriodKey(SETTLED_BY_PERIOD, market->settled_period, id), {});
    } else {
        MARKETS.Erase(out, id);
    }
}

void State::SetPosition(const PositionKey& key, int64_t shares)
{
    if (shares == 0) {
        POSITIONS.Erase(Writable(), key);
    } else {
        POSITIONS.Put(Writable(), key, shares);
    }
}

void State::SetVote(const VoteKey& key, const std::optional<Vote>& vote)
{
    if (vote) {
        VOTES.Put(Writable(), key, *vote);
    } else {
        VOTES.Erase(Writable(), key);
    }
}

void State::SetReputation(const Account& account, std::optional<int64_t> amount)
{
    if (amount) {
        REPUTATION.Put(Writable(), account, *amount);
    } else {
        REPUTATION.Erase(Writable(), account);
    }
}

void State::SetVoterPot(CAmount pot) { VOTER_POT.Put(Writable(), pot); }

void State::SetBallot(const BallotKey& key, const std::optional<Ballot>& ballot)
{
    if (ballot) {
        BALLOTS.Put(Writable(), key, *ballot);
    } else {
        BALLOTS.Erase(Writable(), key);
    }
}

bool State::TradingOpen(const Market& market) const
{
    if (market.settled) return false;
    // Trading stops when the first of the decisions is up for the vote: from then on
    // those who vote would know more than those who trade.
    const uint32_t period{Period()};
    for (const Id& id : market.opening.decisions) {
        const auto decision{GetDecision(id)};
        if (!decision || decision->resolved || period > decision->listing.event_period) return false;
    }
    return true;
}

std::string State::WhyNoTrade(const Id& market_id, uint8_t state, int64_t shares) const
{
    const auto found{GetMarket(market_id)};
    if (!found) return "There is no such market on the chain. One that was just opened has to be mined first.";
    const Market& market{*found};
    if (market.settled) return strprintf("The market \"%s\" is settled: it has paid out.", market.opening.text);
    // A market maker has no stock of shares: it sells as many as are asked for while trading is open, and none after.
    const uint32_t period{Period()};
    for (const Id& id : market.opening.decisions) {
        const auto decision{GetDecision(id)};
        if (!decision) continue;
        if (decision->resolved || period > decision->listing.event_period) {
            return strprintf("Trading in the market \"%s\" is closed: its decision \"%s\" was about period %u, and the chain is in period %u. "
                             "From the end of that period on the answer is voted on, and shares are neither sold nor bought back until the market pays out.",
                             market.opening.text, decision->listing.text, decision->listing.event_period, period);
        }
    }
    if (state >= market.shares.size()) return strprintf("The market has the states 0 to %u.", unsigned(market.shares.size() - 1));
    if (shares == 0) return "The number of shares must not be zero.";
    if (shares < 0 && market.shares[state] + shares < 0) return "The market has not sold as many shares of this state as that.";
    return {};
}

std::string State::WhyNoSale(const PositionKey& key, int64_t shares, int height, const Consensus::HivemindParams& params) const
{
    const int64_t position{PositionOf(key)};
    if (shares > position) return "The account does not hold as many shares of this state.";
    const auto market{GetMarket(key.market)};
    if (!market || key.state >= market->shares.size() || height < params.audit2_height || shares == position) return {};
    // As the chain checks it: what is kept is worth what it would bring back after the sale.
    std::vector<int64_t> after{market->shares}, without;
    after[key.state] -= shares;
    without = after;
    without[key.state] -= position - shares;
    if (position - shares < MinPositionShares(market->shares.size()) || without[key.state] < 0 ||
        -LmsrTradeCost(after, without, market->opening.liquidity) < MIN_POSITION) {
        return strprintf("A sale leaves no shares, or at least %s shares worth at least %s: sell all %s or fewer.",
                         FormatMoney(MinPositionShares(market->shares.size())), FormatMoney(MIN_POSITION), FormatMoney(position));
    }
    return {};
}

std::string State::WhyNoPurchase(const PositionKey& key, int64_t shares, int height, const Consensus::HivemindParams& params) const
{
    const auto market{GetMarket(key.market)};
    if (!market || key.state >= market->shares.size() || height < params.audit2_height || shares <= 0) return {};
    const int64_t position{PositionOf(key)};
    std::vector<int64_t> after{market->shares};
    after[key.state] += shares;
    if (position == 0 && LmsrTradeCost(market->shares, after, market->opening.liquidity) < MIN_POSITION) {
        return strprintf("A purchase that opens a position costs at least %s: buy more shares.", FormatMoney(MIN_POSITION));
    }
    if (position + shares < MinPositionShares(market->shares.size())) {
        return strprintf("A position in this market holds at least %s shares (%s for each of its %u states): buy more shares.",
                         FormatMoney(MinPositionShares(market->shares.size())), FormatMoney(MIN_POSITION), unsigned(market->shares.size()));
    }
    return {};
}

std::optional<CAmount> State::Quote(const Id& market_id, uint8_t state, int64_t shares) const
{
    const auto found{GetMarket(market_id)};
    if (!found || !TradingOpen(*found) || state >= found->shares.size() || shares == 0) return std::nullopt;
    const Market& market{*found};
    std::vector<int64_t> after{market.shares};
    after[state] += shares;
    if (after[state] < 0) return std::nullopt;
    const CAmount cost{LmsrTradeCost(market.shares, after, market.opening.liquidity)};
    if (std::abs(cost) > MAX_MONEY) return std::nullopt;
    const CAmount fee{TradeFee(std::abs(cost), market.opening.fee_bps)};
    return cost + fee;
}

size_t State::ClosingIn(uint32_t close_period) const
{
    sidechain::StoreBytes prefix{OPEN_BY_CLOSE};
    sidechain::KeyCodec<uint32_t>::Encode(prefix, close_period);
    size_t n{0};
    m_view->ForEach(prefix, [&](const sidechain::StoreBytes&, const sidechain::StoreBytes&) {
        ++n;
        return true;
    });
    return n;
}

size_t State::MarketsOn(const Id& decision) const
{
    size_t n{0};
    m_view->ForEach(Head(UNSETTLED_BY_DECISION, decision), [&](const sidechain::StoreBytes&, const sidechain::StoreBytes&) {
        ++n;
        return true;
    });
    return n;
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
    size_t most{0};
    for (const Id& decision : decisions) most = std::max(most, MarketsOn(decision));
    const size_t doublings{most / MARKET_FEE_STEP};
    // Beyond what any amount of coins can pay, it stays there.
    if (doublings >= 40 || (params.listing_fee << doublings) > MAX_MONEY) return MAX_MONEY;
    return params.listing_fee << doublings;
}

bool State::ApplyTx(const CTransaction& tx, int height, const Consensus::HivemindParams& params,
                    std::vector<CTxOut>& payouts, std::string& reject_reason)
{
    if (!CheckTx(tx, height, params, reject_reason)) return false;
    // All or nothing: a transaction can be found out of room for payouts once it is done.
    sidechain::StoreOverlay tx_overlay{Writable(), /*journal=*/false};
    const size_t paid{payouts.size()};
    if (!State{tx_overlay}.ApplyTxSteps(tx, height, params, payouts, reject_reason)) {
        payouts.resize(paid);
        return false;
    }
    tx_overlay.MergeInto(Writable());
    return true;
}

bool State::ApplyTxSteps(const CTransaction& tx, int height, const Consensus::HivemindParams& params,
                         std::vector<CTxOut>& payouts, std::string& reject_reason)
{
    const bool audit2{height >= params.audit2_height};
    const size_t paid_before{payouts.size()};
    for (uint32_t n{0}; n < tx.vout.size(); ++n) {
        const CTxOut& out{tx.vout[n]};
        const auto action{ParseActionScript(out.scriptPubKey)};
        if (!action) continue;
        const uint32_t period{Period()};
        const bool reveal_phase{RevealPhase()};
        // A payout costs every node: from audit2_height, one of less than MIN_PAYOUT goes to the voters.
        const auto pay{[&](const Account& account, CAmount amount) {
            if (amount <= 0) return;
            if (audit2 && amount < MIN_PAYOUT) {
                SetVoterPot(VoterPot() + amount);
                return;
            }
            payouts.push_back(PayAccount(account, amount));
        }};
        // What cannot be done is not done, and the coins burned for it go back.
        // Less a small part, for the voters: payouts cost every block room, and refunds by the
        // thousand would hold back everyone else's.
        const auto refund{[&](const Account& account) {
            const CAmount kept{std::min(out.nValue, REFUND_FEE)};
            SetVoterPot(VoterPot() + kept);
            pay(account, out.nValue - kept);
        }};
        // The proceeds of a sale, and change, are payouts too: from audit2_height they pay for theirs
        // as a refund does, or a sale would make one for the price of a transaction fee.
        const auto pay_out{[&](const Account& account, CAmount amount) {
            if (amount <= 0) return;
            const CAmount kept{audit2 ? std::min(amount, PAYOUT_FEE) : 0};
            if (kept > 0) SetVoterPot(VoterPot() + kept);
            pay(account, amount - kept);
        }};

        if (const auto* list{std::get_if<ListDecision>(&*action)}) {
            if (list->event_period < period || list->event_period > period + params.max_periods_ahead) {
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
            const Id id{MakeId(tx.GetHash(), n)};
            // The first of an id stays, as it did in a map.
            if (!GetDecision(id)) SetDecision(id, decision);
        } else if (const auto* open{std::get_if<OpenMarket>(&*action)}) {
            Market market;
            market.opening = *open;
            market.height = height;
            market.shares.assign(size_t{1} << open->decisions.size(), 0);
            const CAmount funding{LmsrFunding(open->liquidity, market.shares.size())};
            // A market is opened on decisions that are still open themselves.
            bool open_decisions{true};
            for (const Id& id : open->decisions) {
                const auto decision{GetDecision(id)};
                open_decisions = open_decisions && decision && !decision->resolved && period <= decision->listing.event_period;
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
                Decision decision{*Assert(GetDecision(open->decisions[k]))};
                decision.fee += per_decision + (k == 0 ? fee - per_decision * static_cast<CAmount>(open->decisions.size()) : 0);
                SetDecision(open->decisions[k], decision);
            }
            pay_out(open->author, out.nValue - funding - fee);
            const Id id{MakeId(tx.GetHash(), n)};
            if (!GetMarket(id)) SetMarket(id, market);
        } else if (const auto* buy{std::get_if<BuyShares>(&*action)}) {
            const auto found{GetMarket(buy->market)};
            if (!found || !TradingOpen(*found) || buy->state >= found->shares.size()) {
                refund(buy->account);
                continue;
            }
            Market market{*found};
            std::vector<int64_t> after{market.shares};
            after[buy->state] += buy->shares;
            const CAmount cost{LmsrTradeCost(market.shares, after, market.opening.liquidity)};
            const PositionKey key{buy->market, buy->account, buy->state};
            // The price may have moved past what the buyer is ready to pay. And whatever the
            // rounding, a market never owes more than it holds. (No amount of money costs more
            // than MAX_MONEY: the fee and the sum below cannot overflow.)
            if (cost <= 0 || cost > MAX_MONEY || cost + TradeFee(cost, market.opening.fee_bps) > out.nValue || !MoneyRange(market.treasury + cost) ||
                (height >= params.audit_height && cost < MIN_PURCHASE) ||
                (audit2 && cost < MIN_POSITION && PositionOf(key) == 0) ||
                (audit2 && PositionOf(key) + buy->shares < MinPositionShares(after.size())) ||
                market.treasury + cost < *std::max_element(after.begin(), after.end())) {
                refund(buy->account);
                continue;
            }
            const CAmount fee{TradeFee(cost, market.opening.fee_bps)};
            market.shares = std::move(after);
            market.treasury += cost;
            market.fees += fee;
            SetMarket(buy->market, market);
            SetPosition(key, PositionOf(key) + buy->shares);
            pay_out(buy->account, out.nValue - cost - fee);
        } else if (const auto* sell{std::get_if<SellShares>(&*action)}) {
            const auto found{GetMarket(sell->market)};
            const PositionKey key{sell->market, sell->account, sell->state};
            const auto position{POSITIONS.Get(*m_view, key)};
            if (!found || !TradingOpen(*found) || !position || *position < sell->shares) continue;
            Market market{*found};
            // What is left of the position is worth MIN_POSITION, and holds MinPositionShares(), or
            // there is none left (from audit2_height): otherwise a purchase and a sale of nearly all of
            // it, in one transaction, would make a position, which is a payout at settlement, for next
            // to nothing. A sale that would leave less sells the whole position: the price can move
            // between the signing and the block, and a sale refused for it would let anyone who trades
            // first keep a holder from leaving. min_proceeds still bounds what is paid.
            int64_t shares{sell->shares};
            if (audit2 && shares < *position) {
                const int64_t keep{*position - shares};
                std::vector<int64_t> kept{market.shares}, without;
                kept[sell->state] -= shares;
                without = kept;
                without[sell->state] -= keep;
                if (keep < MinPositionShares(kept.size()) || without[sell->state] < 0 ||
                    -LmsrTradeCost(kept, without, market.opening.liquidity) < MIN_POSITION) shares = *position;
            }
            std::vector<int64_t> after{market.shares};
            after[sell->state] -= shares;
            if (after[sell->state] < 0) continue;
            const CAmount proceeds{-LmsrTradeCost(market.shares, after, market.opening.liquidity)};
            // Never more than the treasury, which is money: the fee cannot overflow.
            if (proceeds <= 0 || proceeds > market.treasury) continue;
            const CAmount fee{TradeFee(proceeds, market.opening.fee_bps)};
            // The least to get is of what is paid out: from audit2_height, less PAYOUT_FEE (a sale that
            // brings less than that pays nothing out, and can still close a position).
            if (std::max<CAmount>(0, proceeds - fee - (audit2 ? PAYOUT_FEE : 0)) < sell->min_proceeds || proceeds - fee < 0 ||
                market.treasury - proceeds < *std::max_element(after.begin(), after.end())) continue;
            market.shares = std::move(after);
            market.treasury -= proceeds;
            market.fees += fee;
            SetMarket(sell->market, market);
            SetPosition(key, *position - shares);
            pay_out(sell->account, proceeds - fee);
        } else if (const auto* transfer{std::get_if<TransferReputation>(&*action)}) {
            // Reputation does not move while votes are revealed: it would let a holder put it
            // behind whichever revealed vote turned out best. The period ends, and the votes are
            // weighed, with the last block of the reveal phase.
            if (reveal_phase) continue;
            const auto from{REPUTATION.Get(*m_view, transfer->from)};
            if (!from || *from < transfer->amount) continue;
            int64_t amount{transfer->amount};
            // Every resolution reads and may rewrite every holding: from audit2_height none is made
            // smaller than a voter of its own (reputation_supply / MAX_VOTERS), so that there are at
            // most about MAX_VOTERS of them, rather than as many as the supply has units. A transfer
            // that would leave less than that behind moves the whole holding (a resolution can change
            // the holding between the signing and the block: refused, the transfer could be kept from
            // ever emptying it); and a whole holding moves wherever it goes, even below that size, as
            // it adds no holding: one that a resolution left below it is moved into another so.
            if (audit2) {
                const int64_t least{params.reputation_supply / MAX_VOTERS};
                if (*from - amount != 0 && *from - amount < least) amount = *from;
                if (*from - amount != 0 && ReputationOf(transfer->to) + amount < least) continue;
                // A holding that sealed votes is not emptied until they are revealed (the period they
                // close in is over): a seal costs every node a row, and the holding of a voter is what
                // bounds how many there are. Emptied into another account, which seals again, and so
                // on, one stake would seal the votes of any number of accounts in one block. What is
                // moved out of it leaves it a voter's own, as a transfer always does.
                if (*from - amount == 0 && SealedUntil(transfer->from)) continue;
            }
            const int64_t left{*from - amount};
            const int64_t to{ReputationOf(transfer->to) + amount};
            SetReputation(transfer->from, left == 0 ? std::nullopt : std::optional{left});
            SetReputation(transfer->to, to);
        } else if (const auto* seal{std::get_if<SealVote>(&*action)}) {
            const auto found{GetDecision(seal->decision)};
            if (!found || found->resolved) continue;
            const Decision& decision{*found};
            // Votes are sealed from the period after the event until the last third of the last period.
            const bool in_window{period > decision.listing.event_period && period <= decision.close_period};
            if (!in_window || (period == decision.close_period && reveal_phase)) continue;
            // Only reputation that counts in the resolution seals a vote: a seal costs every node.
            if (height >= params.audit_height) {
                const auto held{REPUTATION.Get(*m_view, seal->voter)};
                if (!held || *held < params.reputation_supply / MAX_VOTERS) continue;
            }
            SetVote(VoteKey{seal->decision, seal->voter}, Vote{seal->commitment, false, 0});
            if (audit2) SealFor(seal->voter, decision.close_period);
        } else if (const auto* reveal{std::get_if<RevealVote>(&*action)}) {
            const auto found{GetDecision(reveal->decision)};
            const VoteKey key{reveal->decision, reveal->voter};
            const auto vote{GetVote(key)};
            if (!found || found->resolved || !vote || vote->revealed) continue;
            const Decision& decision{*found};
            if (period != decision.close_period || !reveal_phase) continue;
            // Only the vote that was sealed counts.
            if (!IsValidAnswer(decision.listing, reveal->answer) || VoteCommitment(reveal->decision, reveal->voter, reveal->answer, reveal->salt) != vote->commitment) continue;
            Vote revealed{*vote};
            revealed.revealed = true;
            revealed.answer = reveal->answer;
            SetVote(key, revealed);
        } else if (const auto* ballot{std::get_if<SealBallot>(&*action)}) {
            // As a single seal, whatever audit_height is: ballots are of audit2_height's rules, and a
            // seal costs every node a row.
            if (height >= params.audit_height || audit2) {
                const auto held{REPUTATION.Get(*m_view, ballot->voter)};
                if (!held || *held < params.reputation_supply / MAX_VOTERS) continue;
            }
            // Each decision as a single seal would be: open, closing in the ballot's period, in its window.
            const auto sealable{[&](const Id& id) {
                const auto decision{GetDecision(id)};
                return decision && !decision->resolved && decision->close_period == ballot->close_period &&
                       period > decision->listing.event_period && period <= decision->close_period &&
                       !(period == decision->close_period && reveal_phase);
            }};
            if (std::none_of(ballot->decisions.begin(), ballot->decisions.end(), sealable)) continue;
            // A new ballot replaces the voter's last for the period: the seals that are still the old
            // ballot's go with it.
            const BallotKey ballot_key{ballot->close_period, ballot->voter};
            if (const auto old{GetBallot(ballot_key)}) {
                for (const Id& id : old->decisions) {
                    const VoteKey key{id, ballot->voter};
                    if (const auto vote{GetVote(key)}; vote && !vote->revealed && vote->commitment == old->commitment) SetVote(key, std::nullopt);
                }
            }
            for (const Id& id : ballot->decisions) {
                if (sealable(id)) SetVote(VoteKey{id, ballot->voter}, Vote{ballot->commitment, false, 0});
            }
            SetBallot(ballot_key, Ballot{ballot->decisions, ballot->commitment});
            if (audit2) SealFor(ballot->voter, ballot->close_period);
        } else if (const auto* reveal{std::get_if<RevealBallot>(&*action)}) {
            const BallotKey ballot_key{reveal->close_period, reveal->voter};
            const auto found{GetBallot(ballot_key)};
            if (!found || period != reveal->close_period || !reveal_phase) continue;
            const Ballot& ballot{*found};
            if (reveal->answers.size() != ballot.decisions.size() ||
                BallotCommitment(reveal->voter, reveal->close_period, ballot.decisions, reveal->answers, reveal->salt) != ballot.commitment) continue;
            // Each vote that is still the ballot's seal is revealed, as a single reveal would be.
            for (size_t j{0}; j < ballot.decisions.size(); ++j) {
                const auto decision{GetDecision(ballot.decisions[j])};
                const VoteKey key{ballot.decisions[j], reveal->voter};
                const auto vote{GetVote(key)};
                if (!decision || decision->resolved || decision->close_period != period || !vote || vote->revealed ||
                    vote->commitment != ballot.commitment || !IsValidAnswer(decision->listing, reveal->answers[j])) continue;
                SetVote(key, Vote{vote->commitment, true, reveal->answers[j]});
            }
            // Done with: what it sealed is shown.
            SetBallot(ballot_key, std::nullopt);
        }
    }
    // Room for payouts is scarce (from audit2_height): what a block pays, the coinbase creates, and
    // every node keeps what waits. ApplyTx() drops what the transaction did, and the miner leaves it for
    // a later block.
    if (audit2 && payouts.size() > paid_before && payouts.size() > MAX_TX_PAYOUTS_PER_BLOCK) {
        reject_reason = "hm-block-payouts-full";
        return false;
    }
    return true;
}

void State::Settle(const Id& id, Market& market, std::map<Account, CAmount>& paid)
{
    std::vector<int64_t> outcomes;
    for (const Id& decision : market.opening.decisions) outcomes.push_back(Assert(GetDecision(decision))->outcome);

    // Every share is paid what its state came to. The positions of a market are next to
    // each other in the store, in the order of the accounts.
    CAmount left{market.treasury};
    std::vector<std::pair<PositionKey, int64_t>> positions;
    ForEachPositionIn(id, [&](const PositionKey& key, int64_t shares) {
        positions.emplace_back(key, shares);
        return true;
    });
    for (const auto& [key, shares] : positions) {
        const __int128 due{static_cast<__int128>(shares) * StatePayout(outcomes, key.state) / ONE};
        // The treasury covers what is due; the bound is there in case it ever did not.
        const CAmount payment{static_cast<CAmount>(std::min<__int128>(due, left))};
        if (payment > 0) {
            paid[key.account] += payment;
            left -= payment;
        }
        SetPosition(key, 0);
    }
    // What the market maker did not lose goes back to who funded it.
    if (left > 0) paid[market.opening.author] += left;

    // The trading fees: half to the voters, a quarter to the authors of the decisions,
    // the rest to the author of the market.
    const CAmount to_voters{market.fees / 2};
    const CAmount per_decision{(market.fees / 4) / static_cast<CAmount>(market.opening.decisions.size())};
    CAmount to_market_author{market.fees - to_voters};
    for (const Id& decision : market.opening.decisions) {
        if (per_decision > 0) paid[Assert(GetDecision(decision))->listing.author] += per_decision;
        to_market_author -= per_decision;
    }
    SetVoterPot(VoterPot() + to_voters);
    if (to_market_author > 0) paid[market.opening.author] += to_market_author;

    market.treasury = 0;
    market.fees = 0;
    market.settled = true;
}

std::optional<uint32_t> State::FirstClose() const
{
    std::optional<uint32_t> first;
    m_view->ForEach(sidechain::StoreBytes{OPEN_BY_CLOSE}, [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
        std::span<const unsigned char> rest{key};
        rest = rest.subspan(1);
        first = sidechain::KeyCodec<uint32_t>::Decode(rest);
        return false;
    });
    return first;
}

void State::Resolve(uint32_t new_period, uint32_t first_open, const Consensus::HivemindParams& params, std::map<Account, CAmount>& paid, bool audit2)
{
    // The decisions whose vote is over: open ones whose vote closed before this period, in the order
    // of their ids.
    std::vector<Id> batch{IdsIn(*m_view, sidechain::StoreBytes{OPEN_BY_CLOSE}, [&](std::span<const unsigned char> rest) {
        return sidechain::KeyCodec<uint32_t>::Decode(rest) < new_period;
    })};
    if (batch.empty()) return;
    std::sort(batch.begin(), batch.end());

    // Everyone who holds reputation takes part. Those who revealed a vote on these decisions
    // are voters, in the order of the accounts; the others are scored alike, as having
    // skipped every vote, and stand together as one more voter at the end, so that spreading
    // reputation over many accounts adds nothing to the work.
    std::set<Account> voted;
    for (const Id& id : batch) {
        ForEachVoteOn(id, [&](const VoteKey& key, const Vote& vote) {
            if (vote.revealed) voted.insert(key.voter);
            return true;
        });
    }
    std::vector<Account> voters, silent;
    std::vector<int64_t> reputation, silent_reputation;
    int64_t silent_total{0};
    // A voter needs a share of the reputation to be a row of its own: splitting reputation over
    // ever more accounts would otherwise make the resolution as large as one likes. Smaller holders
    // are scored with those who did not vote.
    const int64_t min_voter{params.reputation_supply / MAX_VOTERS};
    ForEachReputation([&](const Account& account, int64_t amount) {
        if (amount <= 0) return true;
        if (voted.contains(account) && amount >= min_voter) {
            voters.push_back(account);
            reputation.push_back(amount);
        } else {
            silent.push_back(account);
            silent_reputation.push_back(amount);
            silent_total += amount;
        }
        return true;
    });
    if (silent_total > 0) reputation.push_back(silent_total);
    std::vector<Decision> decisions;
    for (const Id& id : batch) decisions.push_back(*Assert(GetDecision(id)));
    std::vector<bool> scaled, last_round;
    for (const Decision& decision : decisions) {
        scaled.push_back(decision.listing.scaled);
        last_round.push_back(decision.rolls >= params.max_rolls);
    }
    std::vector<std::vector<int64_t>> votes(reputation.size(), std::vector<int64_t>(batch.size(), VOTE_MISSING));
    for (size_t j{0}; j < batch.size(); ++j) {
        for (size_t i{0}; i < voters.size(); ++i) {
            const auto vote{GetVote(VoteKey{batch[j], voters[i]})};
            // A vote that was sealed and not revealed is a vote not cast.
            if (!vote || !vote->revealed) continue;
            votes[i][j] = vote->answer == ANSWER_UNKNOWN ? VOTE_UNKNOWN : AnswerFraction(decisions[j].listing, vote->answer);
        }
    }

    const int64_t quorum{static_cast<int64_t>(static_cast<__int128>(params.reputation_supply) * QUORUM_PERCENT / 100)};
    const Resolution resolution{hivemind::Resolve(reputation, votes, scaled, params.alpha_num, params.alpha_den, params.tolerance, quorum, last_round)};

    // Written only where it changed: a holding that did not change costs no write, and no undo data.
    const auto set_reputation{[&](const Account& account, int64_t before, int64_t amount) {
        if (amount == before) return;
        SetReputation(account, amount > 0 ? std::optional{amount} : std::nullopt);
    }};
    // What the silent come out with is theirs in the proportions they went in with.
    std::vector<int64_t> shares{silent_total > 0 ? Share(silent_reputation, resolution.reputation.back()) : std::vector<int64_t>(silent.size(), 0)};
    std::vector<int64_t> voter_shares(resolution.reputation.begin(), resolution.reputation.begin() + voters.size());
    if (audit2) {
        // Holdings below a voter's own, which no transfer makes (from audit2_height) but which
        // resolutions leave, are folded into those of the voters (or, if nobody voted, of the holders
        // who count as voters), by what they come out with: otherwise, made at that size and left to
        // shrink, they would add up to ever more holdings for every resolution to read. A holding is
        // given a period below the line to be moved whole into another or made up again: what is
        // folded is what was below it before this resolution too.
        int64_t folded{0};
        for (size_t i{0}; i < silent.size(); ++i) {
            if (silent_reputation[i] >= min_voter || shares[i] >= min_voter) continue;
            folded += shares[i];
        }
        if (folded > 0) {
            const auto weigh{[&](const std::vector<int64_t>& amounts, bool silent_side) {
                std::vector<int64_t> weights;
                for (size_t i{0}; i < amounts.size(); ++i) weights.push_back(!silent_side || shares[i] >= min_voter ? amounts[i] : 0);
                return weights;
            }};
            std::vector<int64_t> voter_weights{weigh(voter_shares, false)}, silent_weights{weigh(shares, true)};
            const bool to_voters{std::accumulate(voter_weights.begin(), voter_weights.end(), int64_t{0}) > 0};
            const bool to_silent{!to_voters && std::accumulate(silent_weights.begin(), silent_weights.end(), int64_t{0}) > 0};
            if (to_voters || to_silent) {
                for (size_t i{0}; i < silent.size(); ++i) {
                    if (silent_reputation[i] < min_voter && shares[i] < min_voter) shares[i] = 0;
                }
                std::vector<int64_t>& to{to_voters ? voter_shares : shares};
                const std::vector<int64_t> extra{Share(to_voters ? voter_weights : silent_weights, folded)};
                for (size_t i{0}; i < to.size(); ++i) to[i] += extra[i];
            }
        }
    }
    for (size_t i{0}; i < voters.size(); ++i) set_reputation(voters[i], reputation[i], voter_shares[i]);
    for (size_t i{0}; i < silent.size(); ++i) set_reputation(silent[i], silent_reputation[i], shares[i]);
    for (size_t j{0}; j < batch.size(); ++j) {
        Decision decision{*Assert(GetDecision(batch[j]))};
        // The votes are done with, whatever comes of them.
        std::vector<VoteKey> done;
        ForEachVoteOn(batch[j], [&](const VoteKey& key, const Vote&) {
            done.push_back(key);
            return true;
        });
        for (const VoteKey& key : done) SetVote(key, std::nullopt);

        if (resolution.outcomes[j] == OUTCOME_ROLL && decision.rolls < params.max_rolls) {
            // The answer is not known yet: the decision is voted again, for as long as before, from
            // the first period that has a time for sealing -- not in periods the chain has skipped.
            ++decision.rolls;
            decision.close_period = std::max(new_period, first_open) - 1 + decision.listing.vote_periods;
            // Into the first period from then on that has room.
            while (ClosingIn(decision.close_period) >= MAX_DECISIONS_PER_PERIOD) ++decision.close_period;
            SetDecision(batch[j], decision);
            continue;
        }
        decision.resolved = true;
        // A decision that was put off too often is settled as a tie.
        decision.outcome = resolution.outcomes[j] == OUTCOME_ROLL ? ONE / 2 : resolution.outcomes[j];
        SetVoterPot(VoterPot() + decision.fee);
        decision.fee = 0;
        SetDecision(batch[j], decision);
    }

    // The markets that are decided now pay: those on the decisions just resolved (a market is opened
    // on open decisions only, so no other can be decided now), in the order of their ids.
    std::set<Id> candidates;
    for (const Id& id : batch) {
        for (const Id& market : IdsIn(*m_view, Head(UNSETTLED_BY_DECISION, id))) candidates.insert(market);
    }
    for (const Id& id : candidates) {
        Market market{*Assert(GetMarket(id))};
        if (market.settled) continue;
        const bool decided{std::all_of(market.opening.decisions.begin(), market.opening.decisions.end(), [&](const Id& decision) {
            return Assert(GetDecision(decision))->resolved;
        })};
        if (decided) {
            Settle(id, market, paid);
            market.settled_period = new_period;
            SetMarket(id, market);
        }
    }

    // The fees for the voters go to those who voted, by the reputation they come out with.
    // If nobody voted they wait for the next vote.
    __int128 weight{0};
    for (const int64_t share : resolution.participation) weight += share;
    const CAmount pot{VoterPot()};
    if (weight > 0 && pot > 0) {
        CAmount dealt{0};
        for (size_t i{0}; i < voters.size(); ++i) {
            const CAmount share{static_cast<CAmount>(static_cast<__int128>(pot) * resolution.participation[i] / weight)};
            if (share > 0) paid[voters[i]] += share;
            dealt += share;
        }
        // What rounding left over waits too.
        SetVoterPot(pot - dealt);
    }
}

void State::BeginBlock(int height, int64_t main_time, const Consensus::HivemindParams& params, std::vector<CTxOut>& payouts)
{
    sidechain::StoreOverlay& out{Writable()};
    if (!STARTED.Get(*m_view)) {
        STARTED.Put(out, true);
        if (params.reputation_supply > 0) SetReputation(params.genesis_account, params.reputation_supply);
    }
    const Clock clock{ClockAt(height, main_time, params)};
    const uint32_t period{Period()};
    const bool reveal_phase{RevealPhase()};
    // The clock of the mainchain can go back a little; a period never does, and nor does the
    // phase within a period: once votes are revealed, none can be sealed again until the next.
    if (clock.period > period) {
        // A decision whose votes could never be revealed -- its last period went by without a
        // block of this chain in the reveal phase, as after a stall or a jump of the clock -- is
        // not put off, which would use up its rounds and in the end force a tie nobody voted on:
        // it is voted on in the first period from now that still has a time for sealing.
        const uint32_t first_open{clock.period + (clock.reveal_phase ? 1 : 0)};
        std::vector<Id> due{IdsIn(*m_view, sidechain::StoreBytes{OPEN_BY_CLOSE}, [&](std::span<const unsigned char> rest) {
            return sidechain::KeyCodec<uint32_t>::Decode(rest) <= clock.period;
        })};
        std::sort(due.begin(), due.end());
        for (const Id& id : due) {
            Decision decision{*Assert(GetDecision(id))};
            if (decision.close_period == period && reveal_phase) continue;
            // Closing in this period, which the clock entered in its reveal phase: if every period
            // of its vote came after the last block, nobody could seal a vote on it.
            if (decision.close_period == clock.period && (!clock.reveal_phase || decision.close_period + 1 <= period + decision.listing.vote_periods)) continue;
            uint32_t close_period{first_open - 1 + decision.listing.vote_periods};
            while (ClosingIn(close_period) >= MAX_DECISIONS_PER_PERIOD) ++close_period;
            decision.close_period = close_period;
            SetDecision(id, decision);
            // Its seals stand, and are revealed then: the holdings that sealed them stay until then.
            if (height >= params.audit2_height) {
                std::vector<Account> voters;
                ForEachVoteOn(id, [&](const VoteKey& key, const Vote&) {
                    voters.push_back(key.voter);
                    return true;
                });
                for (const Account& voter : voters) SealFor(voter, close_period);
            }
        }
        std::map<Account, CAmount> paid;
        // Periods the chain skipped are resolved one after the other, each on its own decisions: those
        // in which some decision closed before. The others have nothing to resolve -- which, when the
        // clock jumps far (on a chain that starts at time 0, say), would be very many periods to go through.
        // A decision put off goes to a period from the clock's on, which no step of this loop resolves.
        for (uint32_t p{period + 1}; p <= clock.period; ++p) {
            const auto first{FirstClose()};
            if (!first || *first >= clock.period) break;
            p = std::max(p, *first + 1);
            Resolve(p, first_open, params, paid, height >= params.audit2_height);
        }
        PERIOD.Put(out, clock.period);
        REVEAL.Put(out, clock.reveal_phase);
        // Ballots of periods that are over are done with: their votes were weighed, or put off with
        // the decision, which takes new seals.
        std::vector<BallotKey> done;
        BALLOTS.ForEach(*m_view, [&](const BallotKey& key, const Ballot&) {
            if (key.close_period >= clock.period) return false;
            done.push_back(key);
            return true;
        });
        for (const BallotKey& key : done) SetBallot(key, std::nullopt);
        // Seals of periods that are over hold no holding any more.
        std::vector<Account> unsealed;
        SEALED.ForEach(*m_view, [&](const Account& account, const uint32_t& until) {
            if (until < clock.period) unsealed.push_back(account);
            return true;
        });
        for (const Account& account : unsealed) SEALED.Erase(out, account);
        for (const auto& [account, amount] : paid) {
            // Dust goes to the voters: a payout costs every node, and a settlement could make many.
            if (height >= params.audit_height && amount < MIN_PAYOUT) {
                SetVoterPot(VoterPot() + amount);
                continue;
            }
            payouts.push_back(PayAccount(account, amount));
        }
        Prune();
    } else if (clock.period == period) {
        REVEAL.Put(out, reveal_phase || clock.reveal_phase);
    }
}

void State::Prune()
{
    // Settled markets are kept for a while, to be looked at, then dropped; so are resolved
    // decisions, once no market that is still there is on them.
    const uint32_t period{Period()};
    if (period <= PRUNE_AFTER_PERIODS) return;
    const uint32_t before{period - PRUNE_AFTER_PERIODS};
    for (const Id& id : IdsIn(*m_view, sidechain::StoreBytes{SETTLED_BY_PERIOD}, [&](std::span<const unsigned char> rest) {
             return sidechain::KeyCodec<uint32_t>::Decode(rest) < before;
         })) {
        SetMarket(id, std::nullopt);
    }
    for (const Id& id : IdsIn(*m_view, sidechain::StoreBytes{RESOLVED_BY_CLOSE}, [&](std::span<const unsigned char> rest) {
             return sidechain::KeyCodec<uint32_t>::Decode(rest) < before;
         })) {
        // In use while a market that is still there is on it.
        if (!IdsIn(*m_view, Head(MARKETS_BY_DECISION, id)).empty()) continue;
        SetDecision(id, std::nullopt);
    }
}

} // namespace hivemind
