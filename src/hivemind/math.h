// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_HIVEMIND_MATH_H
#define BITCOIN_HIVEMIND_MATH_H

#include <cstdint>
#include <optional>
#include <vector>

/**
 * The arithmetic of the markets: the market maker and the resolution of votes.
 *
 * Every node has to get the same result to the last bit, so nothing here uses
 * floating point. Everything is integer arithmetic on fixed point numbers,
 * with the rounding of each step spelled out.
 */
namespace hivemind {

/**
 * The unit of fractions, and of shares: outcomes and prices are counted in
 * 1/ONE, and ONE shares of a state pay one coin if that state comes true.
 */
inline constexpr int64_t ONE{100'000'000};

/** Largest number of states a market can have. */
inline constexpr size_t MAX_STATES{16};

//
// The market maker (logarithmic market scoring rule).
//
// The maker of a market with liquidity b values the shares sold so far, q, at
//     C(q) = b * ln(sum_i exp(q_i / b))
// and sells or buys any number of shares for the difference in C. Its loss is
// at most b * ln(number of states), which whoever opens the market pays up front.
//

/**
 * What a trade costs: C(after) - C(before), in the smallest unit of the coin.
 * Rounded up, so that rounding is never in favour of the trader: a purchase
 * costs a fraction more, a sale yields a fraction less (a negative result,
 * closer to zero).
 *
 * @param[in] before, after  shares sold of each state, in units of 1/ONE share
 * @param[in] b              liquidity, in the smallest unit of the coin; positive
 */
int64_t LmsrTradeCost(const std::vector<int64_t>& before, const std::vector<int64_t>& after, int64_t b);

/** The most the maker can lose: b * ln(states), rounded up. */
int64_t LmsrFunding(int64_t b, size_t states);

/** The price of each state, in 1/ONE; they add up to ONE but for rounding. */
std::vector<int64_t> LmsrPrices(const std::vector<int64_t>& shares, int64_t b);

//
// The resolution of votes.
//

/** A voter did not vote on a decision. */
inline constexpr int64_t VOTE_MISSING{-1};
/** A voter says that the answer cannot be known yet. */
inline constexpr int64_t VOTE_UNKNOWN{-2};

/** An outcome that means: vote again later. */
inline constexpr int64_t OUTCOME_ROLL{-1};

struct Resolution {
    //! Reputation of each voter after the vote; adds up to what it added up to before.
    std::vector<int64_t> reputation;
    //! Outcome of each decision in 1/ONE, or OUTCOME_ROLL.
    std::vector<int64_t> outcomes;
    //! Reputation, after the vote, of the voters whose vote agrees with the outcome, per decision.
    std::vector<int64_t> certainty;
    //! Share of the reputation, after the vote, that each voter earned by voting: zero for who did not vote at all.
    std::vector<int64_t> participation;
};

/**
 * Resolve a batch of decisions from the votes on them.
 *
 * Voters are scored by how they sit with the others across the whole batch:
 * the votes are a matrix, its first principal component is the line along
 * which voters disagree most, and voters at the end of it that the outcomes
 * do not follow lose reputation to the others. A voter who skipped votes
 * loses reputation to those who cast them.
 *
 * @param[in] reputation  reputation of each voter before the vote; all positive
 * @param[in] votes       votes[voter][decision]: an answer in 0..ONE, VOTE_MISSING or VOTE_UNKNOWN
 * @param[in] scaled      per decision, whether its answer is a point on a scale, as opposed to yes (ONE) or no (0)
 * @param[in] alpha_num, alpha_den  how much of the reputation is put at stake by a vote
 * @param[in] tolerance   width of the band around ONE/2 in which a yes or no decision is a tie, in 1/ONE
 * @param[in] quorum      reputation that has to give an answer to a decision for it to be resolved now
 * @param[in] last_round  per decision, whether it is voted on for the last time: then saying that its
 *                        answer cannot be known is no vote, and it is resolved on the answers given
 *                        (empty: none is)
 *
 * Who skipped votes loses part of the reputation at stake, but never more in all than what those
 * who voted put at stake themselves: a few voters cannot take much from the many who did not vote.
 */
Resolution Resolve(const std::vector<int64_t>& reputation, const std::vector<std::vector<int64_t>>& votes,
                   const std::vector<bool>& scaled, int64_t alpha_num, int64_t alpha_den, int64_t tolerance,
                   int64_t quorum = 0, const std::vector<bool>& last_round = {});

/** Deal `total` out in proportion to `weights`, to the last unit (largest remainders first). */
std::vector<int64_t> Share(const std::vector<int64_t>& weights, int64_t total);

} // namespace hivemind

#endif // BITCOIN_HIVEMIND_MATH_H
