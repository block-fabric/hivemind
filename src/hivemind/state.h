// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_HIVEMIND_STATE_H
#define BITCOIN_HIVEMIND_STATE_H

#include <consensus/amount.h>
#include <consensus/params.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <sidechain/store.h>
#include <uint256.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

/**
 * Prediction markets (Hivemind, after the Truthcoin paper).
 *
 *  - A **decision** is a question whose answer will be known: yes or no, or a
 *    point on a scale. Listing one costs a fee and names the period in which
 *    its event falls.
 *  - A **market** is about one or more decisions and has two states per
 *    decision. Its author funds a market maker, which sells and buys shares
 *    of the states at prices that follow the demand. A share of a state pays
 *    one coin if the state comes true.
 *  - **Reputation** (votecoin) is a fixed supply held by accounts. After the
 *    event of a decision, its holders vote on the answer: first sealed, then
 *    in the open. The votes of a period are resolved together; holders who
 *    side with the outcome gain reputation from those who do not, or who do
 *    not vote.
 *  - When all decisions of a market are resolved, its shares are paid.
 *
 * Shares and reputation are balances of accounts, which are key hashes; coins
 * move through ordinary outputs. Coins paid to the maker of a market are
 * burned by the transaction that pays them, and what a market pays out is
 * created by the coinbase of the block in which it does, the way deposits
 * from the mainchain are.
 *
 * A transaction that is well formed is valid. If what it asks for cannot be
 * done when its block is connected, because a price has moved or a market has
 * closed, it does nothing and the coins it burned are paid back. That way the
 * validity of a transaction does not depend on those before it in the block.
 */
namespace hivemind {

/** Decisions and markets are identified by the hash of the output that made them. */
using Id = uint256;
/** Hash of the key of an account. */
using Account = uint160;

inline constexpr unsigned char TAG[4]{0xDB, 0x48, 0x56, 0x4D};

/** Number of blocks or parts of a period, out of three, in which votes are sealed; they are revealed in the rest. */
inline constexpr int SEAL_PARTS{2};

/** Most decisions whose vote closes in one period; a listing beyond that is refused and its fee given back. */
inline constexpr size_t MAX_DECISIONS_PER_PERIOD{100};
/** The listing fee doubles for every this many decisions closing in the same period already. */
inline constexpr size_t LISTING_FEE_STEP{10};
/** The fee to open a market doubles for every this many unsettled markets on its first decision already. */
inline constexpr size_t MARKET_FEE_STEP{4};
/** Most voters a resolution weighs one by one: holders of less than this share of the reputation are weighed together. */
inline constexpr int64_t MAX_VOTERS{10000};
/** Periods after which a resolved decision, or a settled market, leaves the state. */
inline constexpr uint32_t PRUNE_AFTER_PERIODS{30};
/**
 * Share of the reputation, in percent, that has to answer a decision for it to be resolved; with
 * fewer answers it is voted on again. Nobody loses reputation for not voting on so few.
 */
inline constexpr int64_t QUORUM_PERCENT{20};
/** Least amount a purchase of shares may pay: what cannot be bought is given back, and payouts are not free. */
inline constexpr CAmount MIN_PURCHASE{10000};
/**
 * Settlement payouts below this go to the voters' pot (from audit_height), and so do the proceeds of a
 * sale and coins given back (from audit2_height): a payout costs every node.
 */
inline constexpr CAmount MIN_PAYOUT{1000};
/**
 * Least amount a purchase that opens a position pays (from audit2_height). Every position is a payout
 * when its market settles; a position is open to anyone who names an account, so what bounds their
 * number is the coins each has to hold until then.
 */
inline constexpr CAmount MIN_POSITION{100000};
/**
 * Least shares a position of a market of `states` states holds (from audit2_height), when it is opened
 * and when a sale leaves some: MIN_POSITION for each state. A share of a state pays 1/states of a coin
 * when the market settles at even odds, so a position then pays at least MIN_POSITION, and the payouts
 * of a market are bounded by its treasury over MIN_POSITION -- not `states` times more, as positions
 * bought at a high price and left to fall would be.
 */
inline constexpr int64_t MinPositionShares(size_t states) { return MIN_POSITION * static_cast<int64_t>(states); }
/** What is kept, for the voters, of coins given back for an action that could not be done. */
inline constexpr CAmount REFUND_FEE{1000};
/**
 * What is kept, for the voters, of the proceeds of a sale and of the change of a purchase or of the
 * opening of a market (from audit2_height): each is a payout, which costs every node and takes room
 * in a block, as a refund does.
 */
inline constexpr CAmount PAYOUT_FEE{1000};
/**
 * Most payouts the transactions of a block may give rise to (from audit2_height); a transaction that
 * would go beyond is not valid in that block, and waits for the next. A quarter of what a block pays
 * (sidechain::MAX_PAYOUTS_PER_BLOCK), and half of what the queue of payouts of transactions is sure to
 * get of a block: transactions alone cannot make that queue grow, and what settlements put in it is
 * paid all the same.
 */
inline constexpr size_t MAX_TX_PAYOUTS_PER_BLOCK{250};
/** Most actions in one transaction. */
inline constexpr size_t MAX_ACTIONS_PER_TX{16};

//
// What transactions can ask for. Each is an OP_RETURN output with one data
// push: TAG, a byte for the kind, and the fields.
//

/** List a decision. The value of the output is the listing fee. */
struct ListDecision {
    static constexpr uint8_t KIND{1};
    //! Gets a share of the fees of the markets on the decision.
    Account author;
    //! The period in which the answer becomes known. Voting starts in the period after.
    uint32_t event_period{0};
    //! Number of periods the decision is voted on for.
    uint16_t vote_periods{1};
    bool scaled{false};
    //! Ends of the scale, if scaled; an answer is a whole number between them.
    int64_t min{0};
    int64_t max{0};
    std::string text;

    SERIALIZE_METHODS(ListDecision, obj) { READWRITE(obj.author, obj.event_period, obj.vote_periods, obj.scaled, obj.min, obj.max, obj.text); }
};

/** Open a market. The value of the output funds the market maker; what is too much is paid back. */
struct OpenMarket {
    static constexpr uint8_t KIND{2};
    //! Gets what the market maker has left, and a share of the trading fees.
    Account author;
    //! State s of the market is the one where decision d is answered yes, or high, if bit d of s is set.
    std::vector<Id> decisions;
    //! Liquidity of the market maker: the larger, the less a trade moves the price.
    int64_t liquidity{0};
    //! Trading fee in hundredths of a percent.
    uint16_t fee_bps{0};
    std::string text;

    SERIALIZE_METHODS(OpenMarket, obj) { READWRITE(obj.author, obj.decisions, obj.liquidity, obj.fee_bps, obj.text); }
};

/** Buy shares. The value of the output is the most to pay; what is left is paid back to the account. */
struct BuyShares {
    static constexpr uint8_t KIND{3};
    Id market;
    Account account;
    uint8_t state{0};
    //! In units of 1/ONE share.
    int64_t shares{0};

    SERIALIZE_METHODS(BuyShares, obj) { READWRITE(obj.market, obj.account, obj.state, obj.shares); }
};

/** Sell shares. Signed by the account. */
struct SellShares {
    static constexpr uint8_t KIND{4};
    Id market;
    Account account;
    uint8_t state{0};
    int64_t shares{0};
    //! The least to get, fee deducted, or the sale does not happen.
    CAmount min_proceeds{0};
    std::vector<unsigned char> signature;

    SERIALIZE_METHODS(SellShares, obj) { READWRITE(obj.market, obj.account, obj.state, obj.shares, obj.min_proceeds, obj.signature); }
};

/** Move reputation to another account. Signed by the account it leaves. */
struct TransferReputation {
    static constexpr uint8_t KIND{5};
    Account from;
    Account to;
    int64_t amount{0};
    std::vector<unsigned char> signature;

    SERIALIZE_METHODS(TransferReputation, obj) { READWRITE(obj.from, obj.to, obj.amount, obj.signature); }
};

/** Commit to a vote without showing it. Signed by the voter. */
struct SealVote {
    static constexpr uint8_t KIND{6};
    Account voter;
    Id decision;
    //! VoteCommitment() of the vote.
    uint256 commitment;
    std::vector<unsigned char> signature;

    SERIALIZE_METHODS(SealVote, obj) { READWRITE(obj.voter, obj.decision, obj.commitment, obj.signature); }
};

/** Show a vote that was committed to. Needs no signature: only the voter knows what opens the seal. */
struct RevealVote {
    static constexpr uint8_t KIND{7};
    Account voter;
    Id decision;
    //! 0 for no, 1 for yes, a point of the scale, or ANSWER_UNKNOWN.
    int64_t answer{0};
    uint256 salt;

    SERIALIZE_METHODS(RevealVote, obj) { READWRITE(obj.voter, obj.decision, obj.answer, obj.salt); }
};

/** The answer of a voter who says that the answer cannot be known yet. */
inline constexpr int64_t ANSWER_UNKNOWN{std::numeric_limits<int64_t>::min()};

/**
 * Seal votes on several decisions at once (from audit2_height): one commitment, one signature, so that
 * a voter's votes on all the decisions closing in a period take little room. Signed by the voter. The
 * decisions are those whose vote closes in `close_period`; a later ballot of the voter for that
 * period replaces this one, and a seal of a single vote replaces it for that decision.
 */
struct SealBallot {
    static constexpr uint8_t KIND{8};
    Account voter;
    uint32_t close_period{0};
    std::vector<Id> decisions;
    //! BallotCommitment() of the answers.
    uint256 commitment;
    std::vector<unsigned char> signature;

    SERIALIZE_METHODS(SealBallot, obj) { READWRITE(obj.voter, obj.close_period, obj.decisions, obj.commitment, obj.signature); }
};

/**
 * Show the votes of a ballot: an answer for each of its decisions, in their order. The decisions are
 * not repeated: the chain has them from the seal. Needs no signature, as RevealVote.
 */
struct RevealBallot {
    static constexpr uint8_t KIND{9};
    Account voter;
    uint32_t close_period{0};
    std::vector<int64_t> answers;
    uint256 salt;

    SERIALIZE_METHODS(RevealBallot, obj) { READWRITE(obj.voter, obj.close_period, obj.answers, obj.salt); }
};

using Action = std::variant<ListDecision, OpenMarket, BuyShares, SellShares, TransferReputation, SealVote, RevealVote, SealBallot, RevealBallot>;

/** The script of the output that asks for `action`. */
CScript ActionScript(const Action& action);
/** Whether the script claims to be a request of this kind of chain, well formed or not. */
bool IsActionScript(const CScript& script);
std::optional<Action> ParseActionScript(const CScript& script);

/**
 * The message an account signs to authorise the action of output `n` of a transaction, in a block at
 * `height`. It names the first input of the transaction, which can be spent only once, so that the
 * signature is of no use in another transaction; and, from audit2_height, the chain (its genesis
 * block) and the kind of action, so that it is of no use on another chain that shares an output with
 * this one, or as another action; and the place of the action and the outputs of the transaction
 * (`outputs_hash`, ActionOutputsHash()), so that it cannot be repeated in other outputs of the same
 * transaction, nor put in one that pays otherwise than the signer agreed to.
 */
std::string ActionMessage(const Action& action, const COutPoint& first_input, uint32_t n, const uint256& outputs_hash,
                          const Consensus::HivemindParams& params, int height);
/** The outputs of a transaction as signed actions name them: the signatures of actions left out, as they cannot sign themselves. */
uint256 ActionOutputsHash(const std::vector<CTxOut>& outputs);
/** What a sealed vote commits to. It names the voter, so that nobody can seal someone else's vote as their own. */
uint256 VoteCommitment(const Id& decision, const Account& voter, int64_t answer, const uint256& salt);
/** What a ballot commits to: the voter, the period, the decisions and an answer to each, in order. */
uint256 BallotCommitment(const Account& voter, uint32_t close_period, const std::vector<Id>& decisions, const std::vector<int64_t>& answers, const uint256& salt);
/** The identifier of the decision or market made by output `n` of transaction `txid`. */
Id MakeId(const Txid& txid, uint32_t n);

//
// The state.
//

struct Decision {
    ListDecision listing;
    //! Height of the block that listed it.
    int32_t height{0};
    //! The last period in which it is voted on.
    uint32_t close_period{0};
    //! How often it was put off because its answer was not known.
    uint8_t rolls{0};
    bool resolved{false};
    //! In 1/ONE: 0 for no, ONE for yes, ONE/2 for a tie, or where on the scale.
    int64_t outcome{0};
    //! The listing fee, which goes to the voters when it is resolved.
    CAmount fee{0};

    SERIALIZE_METHODS(Decision, obj) { READWRITE(obj.listing, obj.height, obj.close_period, obj.rolls, obj.resolved, obj.outcome, obj.fee); }
    friend bool operator==(const Decision& a, const Decision& b);
};

struct Market {
    OpenMarket opening;
    int32_t height{0};
    //! Shares sold of each state, in units of 1/ONE share.
    std::vector<int64_t> shares;
    //! What the market maker holds: its funding and what traders paid it.
    CAmount treasury{0};
    //! Trading fees collected, to be shared out when the market is settled.
    CAmount fees{0};
    bool settled{false};
    //! The period that settled it.
    uint32_t settled_period{0};

    SERIALIZE_METHODS(Market, obj) { READWRITE(obj.opening, obj.height, obj.shares, obj.treasury, obj.fees, obj.settled, obj.settled_period); }
    friend bool operator==(const Market& a, const Market& b);
};

struct PositionKey {
    Id market;
    Account account;
    uint8_t state{0};

    SERIALIZE_METHODS(PositionKey, obj) { READWRITE(obj.market, obj.account, obj.state); }
    friend auto operator<=>(const PositionKey&, const PositionKey&) = default;
};

struct VoteKey {
    Id decision;
    Account voter;

    SERIALIZE_METHODS(VoteKey, obj) { READWRITE(obj.decision, obj.voter); }
    friend auto operator<=>(const VoteKey&, const VoteKey&) = default;
};

/** A ballot is the voter's for the period its decisions close in: one at a time. */
struct BallotKey {
    uint32_t close_period{0};
    Account voter;

    SERIALIZE_METHODS(BallotKey, obj) { READWRITE(obj.close_period, obj.voter); }
    friend auto operator<=>(const BallotKey&, const BallotKey&) = default;
};

struct Ballot {
    //! As sealed: the commitment covers them all, though only those that were open to a seal got one.
    std::vector<Id> decisions;
    uint256 commitment;

    SERIALIZE_METHODS(Ballot, obj) { READWRITE(obj.decisions, obj.commitment); }
    friend bool operator==(const Ballot&, const Ballot&) = default;
};

struct Vote {
    uint256 commitment;
    bool revealed{false};
    int64_t answer{0};

    SERIALIZE_METHODS(Vote, obj) { READWRITE(obj.commitment, obj.revealed, obj.answer); }
    friend bool operator==(const Vote&, const Vote&) = default;
};

/** Where a block is in the cycle of voting. */
struct Clock {
    uint32_t period{0};
    //! Whether votes are revealed, as opposed to sealed, at this point of the period.
    bool reveal_phase{false};
};

Clock ClockAt(int height, int64_t main_time, const Consensus::HivemindParams& params);

namespace detail {
template <typename Stream, typename Key, typename Value>
void WriteChanges(Stream& s, const std::vector<std::pair<Key, std::optional<Value>>>& changes)
{
    WriteCompactSize(s, changes.size());
    for (const auto& [key, value] : changes) {
        s << key << value.has_value();
        if (value) s << *value;
    }
}
template <typename Stream, typename Key, typename Value>
void ReadChanges(Stream& s, std::vector<std::pair<Key, std::optional<Value>>>& changes)
{
    changes.clear();
    const uint64_t count{ReadCompactSize(s)};
    for (uint64_t i{0}; i < count; ++i) {
        Key key;
        bool has_value;
        s >> key >> has_value;
        std::optional<Value> value;
        if (has_value) s >> value.emplace();
        changes.emplace_back(std::move(key), std::move(value));
    }
}
} // namespace detail

} // namespace hivemind

/** As PositionKey and VoteKey compare: field by field, hashes as their bytes. */
template <>
struct sidechain::KeyCodec<hivemind::PositionKey> {
    static void Encode(StoreBytes& out, const hivemind::PositionKey& v)
    {
        KeyCodec<uint256>::Encode(out, v.market);
        KeyCodec<uint160>::Encode(out, v.account);
        KeyCodec<uint8_t>::Encode(out, v.state);
    }
    static hivemind::PositionKey Decode(std::span<const unsigned char>& in)
    {
        hivemind::PositionKey v;
        v.market = KeyCodec<uint256>::Decode(in);
        v.account = KeyCodec<uint160>::Decode(in);
        v.state = KeyCodec<uint8_t>::Decode(in);
        return v;
    }
};
template <>
struct sidechain::KeyCodec<hivemind::VoteKey> {
    static void Encode(StoreBytes& out, const hivemind::VoteKey& v)
    {
        KeyCodec<uint256>::Encode(out, v.decision);
        KeyCodec<uint160>::Encode(out, v.voter);
    }
    static hivemind::VoteKey Decode(std::span<const unsigned char>& in)
    {
        hivemind::VoteKey v;
        v.decision = KeyCodec<uint256>::Decode(in);
        v.voter = KeyCodec<uint160>::Decode(in);
        return v;
    }
};

template <>
struct sidechain::KeyCodec<hivemind::BallotKey> {
    static void Encode(StoreBytes& out, const hivemind::BallotKey& v)
    {
        KeyCodec<uint32_t>::Encode(out, v.close_period);
        KeyCodec<uint160>::Encode(out, v.voter);
    }
    static hivemind::BallotKey Decode(std::span<const unsigned char>& in)
    {
        hivemind::BallotKey v;
        v.close_period = KeyCodec<uint32_t>::Decode(in);
        v.voter = KeyCodec<uint160>::Decode(in);
        return v;
    }
};

namespace hivemind {

/**
 * The markets, on the store of the sidechain state (sidechain/store.h), one entry per key. Tables:
 * 0x40 decisions, 0x41 markets, 0x42 positions, 0x43 sealed votes, 0x44 reputation; single values
 * 0x45 whether the reputation was given out, 0x46 the period, 0x47 whether it is in its reveal
 * phase, 0x48 the voters' pot. Indexes, so that no rule reads a table whole: 0x49 decisions not
 * resolved, by the period their vote closes in; 0x4a resolved decisions, by the same; 0x4b markets
 * by each of their decisions; 0x4c markets not settled, by each of their decisions; 0x4d settled
 * markets, by the period they settled in.
 *
 * What a block changes, the store's journal notes: its undo data.
 */
class State
{
public:
    /** Read only. */
    explicit State(const sidechain::StoreView& view) : m_view{&view}, m_overlay{nullptr} {}
    /** Read and write. */
    explicit State(sidechain::StoreOverlay& overlay) : m_view{&overlay}, m_overlay{&overlay} {}

    /**
     * What happens at the start of a block, before its transactions: when a
     * new period has begun, the decisions whose vote is over are resolved, the
     * reputation is dealt out again, and the markets that are decided pay.
     *
     * @param[in]     main_time  time of the mainchain block that the block follows the mainchain up to
     * @param[in,out] payouts    outputs the coinbase must have, in order
     */
    void BeginBlock(int height, int64_t main_time, const Consensus::HivemindParams& params, std::vector<CTxOut>& payouts);

    /**
     * Do what the outputs of a transaction ask for. What cannot be done is
     * left undone, and the coins burned for it are paid back.
     * @return false, and nothing done, if the transaction is not well formed (see CheckTx()), or
     *         (from audit2_height) if it would take the payouts of the block's transactions, those
     *         in `payouts`, beyond MAX_TX_PAYOUTS_PER_BLOCK.
     */
    [[nodiscard]] bool ApplyTx(const CTransaction& tx, int height, const Consensus::HivemindParams& params,
                               std::vector<CTxOut>& payouts, std::string& reject_reason);

    std::optional<Decision> GetDecision(const Id& id) const;
    std::optional<Market> GetMarket(const Id& id) const;
    /** The shares an account holds of a state of a market (0 if none). */
    int64_t PositionOf(const PositionKey& key) const;
    std::optional<Vote> GetVote(const VoteKey& key) const;
    std::optional<Ballot> GetBallot(const BallotKey& key) const;
    /** The reputation an account holds (0 if none). */
    int64_t ReputationOf(const Account& account) const;
    void ForEachDecision(const std::function<bool(const Id&, const Decision&)>& fn) const;
    void ForEachMarket(const std::function<bool(const Id&, const Market&)>& fn) const;
    void ForEachPosition(const std::function<bool(const PositionKey&, int64_t)>& fn) const;
    /** The positions in a market, by account then state. */
    void ForEachPositionIn(const Id& market, const std::function<bool(const PositionKey&, int64_t)>& fn) const;
    /** The sealed votes on a decision, by voter. */
    void ForEachVoteOn(const Id& decision, const std::function<bool(const VoteKey&, const Vote&)>& fn) const;
    void ForEachReputation(const std::function<bool(const Account&, int64_t)>& fn) const;
    /** Whole tables, as maps: read whole, for information (RPC), never by the rules. */
    std::map<Id, Decision> Decisions() const;
    std::map<Id, Market> Markets() const;
    std::map<Account, int64_t> Reputation() const;
    uint32_t Period() const;
    bool RevealPhase() const;
    /** Fees waiting for voters, because the votes so far had none to pay. */
    CAmount VoterPot() const;

    /** What buying (positive) or selling (negative) `shares` of a state of a market costs now, fee included; nothing if it cannot be traded. */
    std::optional<CAmount> Quote(const Id& market, uint8_t state, int64_t shares) const;
    /** Why Quote() has no price for this trade, in words for the one who wanted it; empty if it has one. */
    std::string WhyNoTrade(const Id& market, uint8_t state, int64_t shares) const;
    /**
     * Why a sale of `shares` of a position would not be made in a block at `height`, beyond what
     * WhyNoTrade() says: the account does not hold as many, or (from audit2_height) what it would
     * keep is worth less than MIN_POSITION or fewer shares than MinPositionShares(). Empty if it would
     * be made, as the state stands.
     */
    std::string WhyNoSale(const PositionKey& key, int64_t shares, int height, const Consensus::HivemindParams& params) const;
    /**
     * Why a purchase of `shares` for a position would be refunded in a block at `height`, beyond what
     * WhyNoTrade() says: (from audit2_height) the position it opens costs less than MIN_POSITION, or it
     * leaves the position with fewer than MinPositionShares(). Empty if it would be made, as the state stands.
     */
    std::string WhyNoPurchase(const PositionKey& key, int64_t shares, int height, const Consensus::HivemindParams& params) const;
    /** Whether a market can be traded in now. */
    bool TradingOpen(const Market& market) const;
    /**
     * What listing a decision whose vote closes in `close_period` costs now: the listing fee, doubled
     * for every LISTING_FEE_STEP decisions closing then already, so that filling a period costs
     * more the fuller it gets, rather than being cheap and keeping everyone else out.
     */
    CAmount ListingFee(uint32_t close_period, const Consensus::HivemindParams& params) const;
    /**
     * The fee for opening a market on `decisions`, on top of its funding, which goes to the voters
     * of its decisions: the listing fee, doubled for every MARKET_FEE_STEP unsettled markets on the
     * one of its decisions that has the most.
     */
    CAmount MarketFee(const std::vector<Id>& decisions, const Consensus::HivemindParams& params) const;

private:
    /** What ApplyTx() does, once the transaction is checked; it may still fail, half done. */
    [[nodiscard]] bool ApplyTxSteps(const CTransaction& tx, int height, const Consensus::HivemindParams& params,
                                    std::vector<CTxOut>& payouts, std::string& reject_reason);
    /** Drop decisions and markets that are long done with. */
    void Prune();
    /** Decisions not resolved whose vote closes in `close_period`. */
    size_t ClosingIn(uint32_t close_period) const;
    /** Unsettled markets on a decision. */
    size_t MarketsOn(const Id& decision) const;
    /**
     * Resolve the decisions whose vote closed before `new_period`. A decision voted on again is
     * voted on from `first_open` at the earliest: the first period that still has a time for sealing.
     */
    void Resolve(uint32_t new_period, uint32_t first_open, const Consensus::HivemindParams& params, std::map<Account, CAmount>& paid, bool audit2);
    /** The period in which the vote on the first open decision to close closes, if there is one. */
    std::optional<uint32_t> FirstClose() const;
    void Settle(const Id& id, Market& market, std::map<Account, CAmount>& paid);

    // Writes, with the indexes kept in step.
    sidechain::StoreOverlay& Writable() const;
    void SetDecision(const Id& id, const std::optional<Decision>& decision);
    void SetMarket(const Id& id, const std::optional<Market>& market);
    void SetPosition(const PositionKey& key, int64_t shares);
    void SetVote(const VoteKey& key, const std::optional<Vote>& vote);
    void SetReputation(const Account& account, std::optional<int64_t> amount);
    void SetVoterPot(CAmount pot);
    void SetBallot(const BallotKey& key, const std::optional<Ballot>& ballot);

    const sidechain::StoreView* m_view;
    sidechain::StoreOverlay* m_overlay;
};

/**
 * Whether the requests among the outputs of a transaction are well formed and,
 * where needed, signed, in a block at `height`. This depends on the transaction
 * and the height alone.
 */
[[nodiscard]] bool CheckTx(const CTransaction& tx, int height, const Consensus::HivemindParams& params, std::string& reject_reason);

/** The trading fee on `amount` at `fee_bps` hundredths of a percent, rounded up; in 128 bits, so that no rate overflows. */
CAmount TradeFee(CAmount amount, uint16_t fee_bps);

/** The fraction of ONE that an answer to a decision stands for. */
int64_t AnswerFraction(const ListDecision& decision, int64_t answer);
/** Whether `answer` is one that can be given to the decision. */
bool IsValidAnswer(const ListDecision& decision, int64_t answer);
/** What a share of a state of a market pays, in 1/ONE of a coin, given the outcomes of its decisions. */
int64_t StatePayout(const std::vector<int64_t>& outcomes, size_t state);

} // namespace hivemind

#endif // BITCOIN_HIVEMIND_STATE_H
