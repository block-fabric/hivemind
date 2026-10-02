// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <hivemind/math.h>

#include <util/check.h>

#include <algorithm>
#include <numeric>

namespace hivemind {
namespace {

using i128 = __int128;

//
// Fixed point numbers with 60 fractional bits.
//
constexpr int Q{60};
constexpr i128 Q_ONE{i128{1} << Q};
//! ln(2), rounded down.
constexpr i128 Q_LN2{799144290325165978};

/** Product of two fixed point numbers, rounded towards zero. */
i128 Mul(i128 a, i128 b) { return (a * b) / Q_ONE; }

/** exp(x) for x <= 0. */
i128 Exp(i128 x)
{
    Assert(x <= 0);
    // x = r - k * ln(2) with r in (-ln(2), 0], so that exp(x) = exp(r) / 2^k.
    const i128 k{(-x) / Q_LN2};
    if (k > Q + 2) return 0;
    const i128 r{x + k * Q_LN2};
    // The series of exp(r); its terms shrink by more than half each time.
    i128 sum{Q_ONE};
    i128 term{Q_ONE};
    for (int n{1}; n <= 40; ++n) {
        term = Mul(term, r) / n;
        if (term == 0) break;
        sum += term;
    }
    return sum >> static_cast<int>(k);
}

/** ln(y) for y >= 1. */
i128 Ln(i128 y)
{
    Assert(y >= Q_ONE);
    // y = z * 2^k with z in [1, 2).
    int k{0};
    while ((y >> k) >= 2 * Q_ONE) ++k;
    const i128 z{y >> k};
    // ln(z) = 2 * atanh(t) with t = (z - 1) / (z + 1), which is at most 1/3.
    const i128 t{((z - Q_ONE) * Q_ONE) / (z + Q_ONE)};
    const i128 t2{Mul(t, t)};
    i128 sum{0};
    i128 power{t};
    for (int n{1}; n <= 79; n += 2) {
        if (power == 0) break;
        sum += power / n;
        power = Mul(power, t2);
    }
    return 2 * sum + k * Q_LN2;
}

/** C(q) in units of 2^-32 of the smallest unit of the coin. */
i128 Cost(const std::vector<int64_t>& shares, int64_t b)
{
    Assert(b > 0 && !shares.empty() && shares.size() <= MAX_STATES);
    // b * ln(sum exp(q_i / b)) = m + b * ln(sum exp((q_i - m) / b)) with m the largest q_i,
    // which keeps every exponent at or below zero.
    const int64_t largest{*std::max_element(shares.begin(), shares.end())};
    i128 sum{0};
    for (const int64_t q : shares) sum += Exp(((i128{q} - largest) * Q_ONE) / b);
    return (i128{largest} << 32) + ((i128{b} * Ln(sum)) >> (Q - 32));
}

/** Integer square root, rounded down. */
i128 Sqrt(i128 n)
{
    if (n < 2) return n;
    i128 x{n};
    i128 y{(x + 1) / 2};
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

i128 Abs(i128 a) { return a < 0 ? -a : a; }

/**
 * Deal `total` out in proportion to `weights`, to the last unit: everyone gets
 * the share rounded down, and the units left over go to the largest remainders,
 * the first one in the list winning a tie.
 */
std::vector<int64_t> Apportion(const std::vector<i128>& weights, int64_t total)
{
    std::vector<int64_t> parts(weights.size(), 0);
    const i128 sum{std::accumulate(weights.begin(), weights.end(), i128{0})};
    if (sum <= 0) return parts;
    std::vector<std::pair<i128, size_t>> remainders;
    int64_t dealt{0};
    for (size_t i{0}; i < weights.size(); ++i) {
        const i128 product{weights[i] * total};
        parts[i] = static_cast<int64_t>(product / sum);
        dealt += parts[i];
        remainders.emplace_back(product % sum, i);
    }
    std::stable_sort(remainders.begin(), remainders.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i{0}; dealt < total; ++i, ++dealt) ++parts[remainders[i % remainders.size()].second];
    return parts;
}

/** The answer to a yes or no decision that a weighted mean of votes stands for. */
int64_t Catch(int64_t mean, int64_t tolerance)
{
    if (2 * mean > ONE + tolerance) return ONE;
    if (2 * mean < ONE - tolerance) return 0;
    return ONE / 2;
}

/** The weighted mean of a column of votes that are all cast. */
int64_t WeightedMean(const std::vector<int64_t>& values, const std::vector<int64_t>& weights)
{
    i128 sum{0}, total{0};
    for (size_t i{0}; i < values.size(); ++i) {
        sum += i128{values[i]} * weights[i];
        total += weights[i];
    }
    return total == 0 ? ONE / 2 : static_cast<int64_t>(sum / total);
}

/** The weighted median: the lowest value such that the values up to it carry half the weight. */
int64_t WeightedMedian(const std::vector<int64_t>& values, const std::vector<int64_t>& weights)
{
    std::vector<std::pair<int64_t, int64_t>> sorted;
    i128 total{0};
    for (size_t i{0}; i < values.size(); ++i) {
        if (weights[i] <= 0) continue;
        sorted.emplace_back(values[i], weights[i]);
        total += weights[i];
    }
    if (sorted.empty()) return ONE / 2;
    std::sort(sorted.begin(), sorted.end());
    i128 running{0};
    for (const auto& [value, weight] : sorted) {
        running += weight;
        if (2 * running >= total) return value;
    }
    return sorted.back().first;
}

/** The outcome of each decision that reputation `weights` gives, from votes without gaps. */
std::vector<int64_t> Outcomes(const std::vector<std::vector<int64_t>>& filled, const std::vector<int64_t>& weights,
                              const std::vector<bool>& scaled, int64_t tolerance)
{
    std::vector<int64_t> outcomes;
    std::vector<int64_t> column(filled.size());
    for (size_t j{0}; j < scaled.size(); ++j) {
        for (size_t i{0}; i < filled.size(); ++i) column[i] = filled[i][j];
        outcomes.push_back(scaled[j] ? WeightedMedian(column, weights) : Catch(WeightedMean(column, weights), tolerance));
    }
    return outcomes;
}

} // namespace

int64_t LmsrTradeCost(const std::vector<int64_t>& before, const std::vector<int64_t>& after, int64_t b)
{
    const i128 difference{Cost(after, b) - Cost(before, b)};
    // Round up, towards plus infinity.
    const i128 unit{i128{1} << 32};
    i128 cost{difference / unit};
    if (difference % unit > 0) ++cost;
    return static_cast<int64_t>(cost);
}

int64_t LmsrFunding(int64_t b, size_t states)
{
    Assert(b > 0 && states >= 2 && states <= MAX_STATES);
    const i128 product{i128{b} * Ln(i128{static_cast<int64_t>(states)} * Q_ONE)};
    return static_cast<int64_t>(product / Q_ONE) + 1;
}

std::vector<int64_t> LmsrPrices(const std::vector<int64_t>& shares, int64_t b)
{
    Assert(b > 0 && !shares.empty() && shares.size() <= MAX_STATES);
    const int64_t largest{*std::max_element(shares.begin(), shares.end())};
    std::vector<i128> weights;
    for (const int64_t q : shares) weights.push_back(Exp(((i128{q} - largest) * Q_ONE) / b));
    return Apportion(weights, ONE);
}

Resolution Resolve(const std::vector<int64_t>& reputation, const std::vector<std::vector<int64_t>>& votes,
                   const std::vector<bool>& scaled, int64_t alpha_num, int64_t alpha_den, int64_t tolerance)
{
    const size_t voters{reputation.size()};
    const size_t decisions{scaled.size()};
    Assert(votes.size() == voters && alpha_num >= 0 && alpha_den > 0 && alpha_num <= alpha_den);
    const int64_t total{std::accumulate(reputation.begin(), reputation.end(), int64_t{0})};

    Resolution result;
    result.reputation = reputation;
    result.outcomes.assign(decisions, ONE / 2);
    result.certainty.assign(decisions, 0);
    result.participation.assign(voters, 0);
    if (voters == 0 || decisions == 0 || total <= 0) return result;

    // A decision is voted again later if those who say that its answer cannot be known
    // yet outweigh those who give one, or if nobody gives one.
    std::vector<bool> roll(decisions, false);
    size_t kept{0};
    for (size_t j{0}; j < decisions; ++j) {
        i128 unknown{0}, answered{0};
        for (size_t i{0}; i < voters; ++i) {
            if (votes[i][j] == VOTE_UNKNOWN) unknown += reputation[i];
            if (votes[i][j] >= 0) answered += reputation[i];
        }
        roll[j] = answered == 0 || unknown > answered;
        if (roll[j]) {
            result.outcomes[j] = OUTCOME_ROLL;
        } else {
            ++kept;
        }
    }

    // Who voted, on what share of the decisions that are resolved now. Saying that an
    // answer cannot be known is a vote too.
    std::vector<int64_t> cast(voters, 0);
    for (size_t i{0}; i < voters; ++i) {
        for (size_t j{0}; j < decisions; ++j) {
            if (votes[i][j] != VOTE_MISSING) ++cast[i];
        }
    }

    std::vector<int64_t> scored{reputation};
    if (kept > 0) {
        // The votes on the decisions that are resolved now, with the gaps filled by what
        // the others say: nobody is scored for a vote that was not cast.
        std::vector<bool> kept_scaled;
        for (size_t j{0}; j < decisions; ++j) {
            if (!roll[j]) kept_scaled.push_back(scaled[j]);
        }
        std::vector<std::vector<int64_t>> filled(voters, std::vector<int64_t>(kept, 0));
        size_t column{0};
        for (size_t j{0}; j < decisions; ++j) {
            if (roll[j]) continue;
            std::vector<int64_t> values, weights;
            for (size_t i{0}; i < voters; ++i) {
                if (votes[i][j] < 0) continue;
                values.push_back(votes[i][j]);
                weights.push_back(reputation[i]);
            }
            const int64_t fill{scaled[j] ? WeightedMedian(values, weights) : Catch(WeightedMean(values, weights), tolerance)};
            for (size_t i{0}; i < voters; ++i) filled[i][column] = votes[i][j] >= 0 ? votes[i][j] : fill;
            ++column;
        }

        // The votes less their weighted mean, and their weighted covariance.
        std::vector<int64_t> mean(kept);
        for (size_t j{0}; j < kept; ++j) {
            i128 sum{0};
            for (size_t i{0}; i < voters; ++i) sum += i128{filled[i][j]} * reputation[i];
            mean[j] = static_cast<int64_t>(sum / total);
        }
        std::vector<std::vector<i128>> covariance(kept, std::vector<i128>(kept, 0));
        for (size_t j{0}; j < kept; ++j) {
            for (size_t k{j}; k < kept; ++k) {
                i128 sum{0};
                for (size_t i{0}; i < voters; ++i) sum += i128{filled[i][j] - mean[j]} * (filled[i][k] - mean[k]) * reputation[i];
                covariance[j][k] = covariance[k][j] = sum / total;
            }
        }

        // The first principal component: the direction along which the voters differ most.
        // Found by multiplying a vector by the covariance a fixed number of times; which of
        // its two ends is which is settled below, so its sign does not matter.
        // The start is the column of the decision the voters differ most on, which is never
        // at a right angle to the direction looked for unless that decision plays no part in it.
        constexpr i128 UNIT{1'000'000'000};
        size_t widest{0};
        for (size_t j{1}; j < kept; ++j) {
            if (covariance[j][j] > covariance[widest][widest]) widest = j;
        }
        std::vector<i128> direction{covariance[widest]};
        bool flat{covariance[widest][widest] == 0};
        for (int round{0}; round < 128 && !flat; ++round) {
            std::vector<i128> next(kept, 0);
            i128 largest{0};
            for (size_t j{0}; j < kept; ++j) {
                for (size_t k{0}; k < kept; ++k) next[j] += covariance[j][k] * direction[k];
                largest = std::max(largest, Abs(next[j]));
            }
            if (largest == 0) {
                flat = true;
                break;
            }
            // Bring the entries down to where their squares can be added up.
            int shift{0};
            while ((largest >> shift) >= (i128{1} << 40)) ++shift;
            i128 squares{0};
            for (size_t j{0}; j < kept; ++j) {
                next[j] >>= shift;
                squares += next[j] * next[j];
            }
            const i128 norm{Sqrt(squares)};
            if (norm == 0) {
                flat = true;
                break;
            }
            for (size_t j{0}; j < kept; ++j) direction[j] = (next[j] * UNIT) / norm;
        }

        if (!flat) {
            // Where each voter stands along that direction.
            std::vector<i128> position(voters, 0);
            for (size_t i{0}; i < voters; ++i) {
                for (size_t j{0}; j < kept; ++j) position[i] += i128{filled[i][j] - mean[j]} * direction[j];
            }
            const i128 lowest{*std::min_element(position.begin(), position.end())};
            const i128 highest{*std::max_element(position.begin(), position.end())};

            // One end of the line is the truth, the other the furthest from it. Each reading
            // gives a way to deal out the reputation at stake: by the distance from the end
            // that is wrong, times the reputation held.
            const auto deal{[&](bool low_is_wrong) {
                std::vector<i128> weights(voters);
                for (size_t i{0}; i < voters; ++i) weights[i] = (low_is_wrong ? position[i] - lowest : highest - position[i]) * reputation[i];
                return Apportion(weights, total);
            }};
            const std::vector<int64_t> first{deal(true)};
            const std::vector<int64_t> second{deal(false)};

            // The reading to take is the one whose outcomes are closer to those the voters
            // gave before any of this. If neither is, nobody's reputation changes.
            const std::vector<int64_t> before{Outcomes(filled, reputation, kept_scaled, tolerance)};
            const auto distance{[&](const std::vector<int64_t>& weights) {
                const std::vector<int64_t> outcomes{Outcomes(filled, weights, kept_scaled, tolerance)};
                i128 sum{0};
                for (size_t j{0}; j < kept; ++j) sum += i128{outcomes[j] - before[j]} * (outcomes[j] - before[j]);
                return sum;
            }};
            // A reading that gives nobody anything says nothing.
            const bool first_valid{std::any_of(first.begin(), first.end(), [](int64_t r) { return r > 0; })};
            const bool second_valid{std::any_of(second.begin(), second.end(), [](int64_t r) { return r > 0; })};
            const i128 first_distance{first_valid ? distance(first) : 0};
            const i128 second_distance{second_valid ? distance(second) : 0};
            const std::vector<int64_t>* chosen{nullptr};
            if (first_valid && (!second_valid || first_distance < second_distance)) chosen = &first;
            if (second_valid && (!first_valid || second_distance < first_distance)) chosen = &second;
            if (first_valid && second_valid && first_distance == second_distance) {
                // Both readings give the same outcomes. Then the side that holds more
                // reputation is taken to be right; if the sides are even, nothing changes.
                i128 low_side{0}, high_side{0};
                for (size_t i{0}; i < voters; ++i) {
                    low_side += (highest - position[i]) * reputation[i];
                    high_side += (position[i] - lowest) * reputation[i];
                }
                if (high_side > low_side) chosen = &first;
                if (low_side > high_side) chosen = &second;
            }

            if (chosen) {
                // Part of everyone's reputation is at stake; the rest is kept.
                std::vector<i128> smoothed(voters);
                for (size_t i{0}; i < voters; ++i) {
                    smoothed[i] = i128{reputation[i]} * (alpha_den - alpha_num) + i128{(*chosen)[i]} * alpha_num;
                }
                scored = Apportion(smoothed, total);
            }
        }

        // The outcomes are what the voters say, weighted by the reputation they have now.
        const std::vector<int64_t> outcomes{Outcomes(filled, scored, kept_scaled, tolerance)};
        column = 0;
        for (size_t j{0}; j < decisions; ++j) {
            if (roll[j]) continue;
            result.outcomes[j] = outcomes[column];
            for (size_t i{0}; i < voters; ++i) {
                if (filled[i][column] == outcomes[column]) result.certainty[j] += scored[i];
            }
            ++column;
        }
    }

    // Who skipped votes loses part of the reputation at stake, in proportion to the votes
    // skipped, to those who cast theirs. This is by reputation, not by head: splitting
    // reputation over many accounts changes nothing.
    std::vector<i128> kept_weights(voters), earned_weights(voters);
    i128 forfeited{0}, earning{0};
    for (size_t i{0}; i < voters; ++i) {
        const i128 stake{i128{scored[i]} * alpha_num};
        const i128 lost{stake * (static_cast<int64_t>(decisions) - cast[i]) / (i128{alpha_den} * static_cast<int64_t>(decisions))};
        kept_weights[i] = scored[i] - lost;
        forfeited += lost;
        earned_weights[i] = i128{scored[i]} * cast[i];
        earning += earned_weights[i];
    }
    if (forfeited > 0 && earning > 0) {
        const std::vector<int64_t> bonus{Apportion(earned_weights, static_cast<int64_t>(forfeited))};
        for (size_t i{0}; i < voters; ++i) result.reputation[i] = static_cast<int64_t>(kept_weights[i]) + bonus[i];
    } else {
        result.reputation = scored;
    }
    for (size_t i{0}; i < voters; ++i) result.participation[i] = cast[i] > 0 ? result.reputation[i] : 0;
    return result;
}

} // namespace hivemind
