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
#include <uint256.h>

#include <cstdint>
#include <map>
#include <optional>
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

using Action = std::variant<ListDecision, OpenMarket, BuyShares, SellShares, TransferReputation, SealVote, RevealVote>;

/** The script of the output that asks for `action`. */
CScript ActionScript(const Action& action);
/** Whether the script claims to be a request of this kind of chain, well formed or not. */
bool IsActionScript(const CScript& script);
std::optional<Action> ParseActionScript(const CScript& script);

/**
 * The message an account signs to authorise an action. It names the first
 * input of the transaction, which can be spent only once, so that the
 * signature is of no use in another transaction.
 */
std::string ActionMessage(const Action& action, const COutPoint& first_input);
uint256 VoteCommitment(const Id& decision, int64_t answer, const uint256& salt);
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

    SERIALIZE_METHODS(Market, obj) { READWRITE(obj.opening, obj.height, obj.shares, obj.treasury, obj.fees, obj.settled); }
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

class State
{
public:
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
     * @return false if the transaction is not well formed, see CheckTx().
     */
    [[nodiscard]] bool ApplyTx(const CTransaction& tx, int height, const Consensus::HivemindParams& params,
                               std::vector<CTxOut>& payouts, std::string& reject_reason);

    const std::map<Id, Decision>& Decisions() const { return m_decisions; }
    const std::map<Id, Market>& Markets() const { return m_markets; }
    const std::map<PositionKey, int64_t>& Positions() const { return m_positions; }
    const std::map<VoteKey, Vote>& Votes() const { return m_votes; }
    const std::map<Account, int64_t>& Reputation() const { return m_reputation; }
    uint32_t Period() const { return m_period; }
    bool RevealPhase() const { return m_reveal_phase; }
    /** Fees waiting for voters, because the votes so far had none to pay. */
    CAmount VoterPot() const { return m_voter_pot; }

    /** What buying (positive) or selling (negative) `shares` of a state of a market costs now, fee included; nothing if it cannot be traded. */
    std::optional<CAmount> Quote(const Id& market, uint8_t state, int64_t shares) const;
    /** Whether a market can be traded in now. */
    bool TradingOpen(const Market& market) const;

    SERIALIZE_METHODS(State, obj)
    {
        READWRITE(obj.m_started, obj.m_period, obj.m_reveal_phase, obj.m_decisions, obj.m_markets, obj.m_positions, obj.m_votes, obj.m_reputation, obj.m_voter_pot);
    }
    friend bool operator==(const State& a, const State& b);

private:
    void Resolve(uint32_t new_period, const Consensus::HivemindParams& params, std::map<Account, CAmount>& paid);
    void Settle(const Id& id, Market& market, std::map<Account, CAmount>& paid);

    //! Whether the reputation has been given to the account that holds it at the start.
    bool m_started{false};
    uint32_t m_period{0};
    bool m_reveal_phase{false};
    std::map<Id, Decision> m_decisions;
    std::map<Id, Market> m_markets;
    std::map<PositionKey, int64_t> m_positions;
    std::map<VoteKey, Vote> m_votes;
    std::map<Account, int64_t> m_reputation;
    CAmount m_voter_pot{0};
};

/**
 * Whether the requests among the outputs of a transaction are well formed and,
 * where needed, signed. This depends on the transaction alone.
 */
[[nodiscard]] bool CheckTx(const CTransaction& tx, const Consensus::HivemindParams& params, std::string& reject_reason);

/** The fraction of ONE that an answer to a decision stands for. */
int64_t AnswerFraction(const ListDecision& decision, int64_t answer);
/** Whether `answer` is one that can be given to the decision. */
bool IsValidAnswer(const ListDecision& decision, int64_t answer);
/** What a share of a state of a market pays, in 1/ONE of a coin, given the outcomes of its decisions. */
int64_t StatePayout(const std::vector<int64_t>& outcomes, size_t state);

} // namespace hivemind

#endif // BITCOIN_HIVEMIND_STATE_H
