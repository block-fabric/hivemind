// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <hivemind/math.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <numeric>

using hivemind::LmsrFunding;
using hivemind::LmsrPrices;
using hivemind::LmsrTradeCost;
using hivemind::ONE;
using hivemind::OUTCOME_ROLL;
using hivemind::Resolve;
using hivemind::VOTE_MISSING;
using hivemind::VOTE_UNKNOWN;

BOOST_FIXTURE_TEST_SUITE(hivemind_tests, BasicTestingSetup)

// Reference values are from the formula computed in double precision; the fixed
// point result has to be that value rounded up.
BOOST_AUTO_TEST_CASE(lmsr_cost)
{
    BOOST_CHECK_EQUAL(LmsrTradeCost({0, 0}, {100000000, 0}, 1000000000), 51249480);
    BOOST_CHECK_EQUAL(LmsrTradeCost({0, 0}, {300000000, 0}, 400000000), 177489531);
    // A sale: the trader gets a little less than the formula says.
    BOOST_CHECK_EQUAL(LmsrTradeCost({500000000, 0}, {200000000, 0}, 400000000), -211140838);
    BOOST_CHECK_EQUAL(LmsrTradeCost({0, 0, 0, 0}, {0, 250000000, 0, 0}, 1000000000), 68598725);
    BOOST_CHECK_EQUAL(LmsrTradeCost(std::vector<int64_t>(16, 0), [] { std::vector<int64_t> q(16, 0); q[0] = 100000000; return q; }(), 500000000), 6871403);
    BOOST_CHECK_EQUAL(LmsrTradeCost({123456789, 987654321}, {223456789, 987654321}, 77777777), 3043);
    // A state that is all but certain costs a coin a share.
    const int64_t certain{LmsrTradeCost({1000000000000, 0}, {1000100000000, 0}, 1000000000)};
    BOOST_CHECK(certain == 100000000 || certain == 100000001);
    // Nothing for nothing.
    BOOST_CHECK_EQUAL(LmsrTradeCost({5, 7}, {5, 7}, 1000), 0);
}

BOOST_AUTO_TEST_CASE(lmsr_no_free_coins)
{
    // Buying and selling back never leaves the trader with more, in one step or in many.
    FastRandomContext rng{/*fDeterministic=*/true};
    for (int round{0}; round < 2000; ++round) {
        const size_t states{2 + rng.randrange(15)};
        const int64_t b{static_cast<int64_t>(1000000 + rng.randrange(100000000000))};
        std::vector<int64_t> shares(states);
        for (auto& q : shares) q = rng.randrange(20 * b);
        std::vector<int64_t> after{shares};
        const size_t state{rng.randrange(states)};
        after[state] += 1 + rng.randrange(5 * b);
        const int64_t paid{LmsrTradeCost(shares, after, b)};
        const int64_t back{-LmsrTradeCost(after, shares, b)};
        BOOST_CHECK(paid > 0);
        BOOST_CHECK(back <= paid);
        BOOST_CHECK(paid - back <= 2);
        // In two steps it costs no less than in one.
        std::vector<int64_t> middle{shares};
        middle[state] += (after[state] - shares[state]) / 2;
        BOOST_CHECK(LmsrTradeCost(shares, middle, b) + LmsrTradeCost(middle, after, b) >= paid);
        // The shares never cost more than what they can pay.
        BOOST_CHECK(paid <= after[state] - shares[state] + 1);
    }
}

BOOST_AUTO_TEST_CASE(lmsr_funding_and_prices)
{
    BOOST_CHECK_EQUAL(LmsrFunding(1000000000, 2), 693147181);
    BOOST_CHECK_EQUAL(LmsrFunding(400000000, 4), 554517745);
    BOOST_CHECK_EQUAL(LmsrFunding(1000000000, 16), 2772588723);

    BOOST_CHECK((LmsrPrices({0, 0}, 1000) == std::vector<int64_t>{ONE / 2, ONE / 2}));
    BOOST_CHECK((LmsrPrices({7, 7, 7, 7}, 1000) == std::vector<int64_t>{ONE / 4, ONE / 4, ONE / 4, ONE / 4}));
    const std::vector<int64_t> prices{LmsrPrices({300000000, 0}, 400000000)};
    BOOST_CHECK_EQUAL(std::accumulate(prices.begin(), prices.end(), int64_t{0}), ONE);
    // e^0.75 / (e^0.75 + 1) = 0.679178699...
    BOOST_CHECK(std::abs(prices[0] - 67917870) <= 1);

    // The funding covers the worst case: whatever is bought, what the traders paid
    // plus the funding is at least what the fullest state can pay.
    FastRandomContext rng{/*fDeterministic=*/true};
    for (int round{0}; round < 500; ++round) {
        const size_t states{2 + rng.randrange(15)};
        const int64_t b{static_cast<int64_t>(1000000 + rng.randrange(10000000000))};
        std::vector<int64_t> shares(states, 0);
        int64_t treasury{LmsrFunding(b, states)};
        for (int trade{0}; trade < 20; ++trade) {
            std::vector<int64_t> after{shares};
            after[rng.randrange(states)] += rng.randrange(3 * b);
            treasury += LmsrTradeCost(shares, after, b);
            shares = after;
            BOOST_CHECK(treasury >= *std::max_element(shares.begin(), shares.end()));
        }
    }
}

BOOST_AUTO_TEST_CASE(votes_agreement_changes_nothing)
{
    const std::vector<int64_t> reputation{3000, 5000, 2000};
    const auto result{Resolve(reputation, {{ONE, 0, ONE}, {ONE, 0, ONE}, {ONE, 0, ONE}}, {false, false, false}, 1, 10, ONE / 5)};
    BOOST_CHECK(result.reputation == reputation);
    BOOST_CHECK((result.outcomes == std::vector<int64_t>{ONE, 0, ONE}));
    BOOST_CHECK((result.certainty == std::vector<int64_t>{10000, 10000, 10000}));
}

BOOST_AUTO_TEST_CASE(votes_dissenter_loses)
{
    // Three voters with a third each, one of whom answers one decision against the others.
    // A tenth of the reputation is at stake: the dissenter gets nothing of it, the others
    // split it. Twice over, as the reputation carries from one vote to the next.
    const std::vector<std::vector<int64_t>> votes{{0, ONE}, {ONE, ONE}, {ONE, ONE}};
    auto result{Resolve({3333333333, 3333333333, 3333333334}, votes, {false, false}, 1, 10, ONE / 5)};
    BOOST_CHECK((result.outcomes == std::vector<int64_t>{ONE, ONE}));
    BOOST_CHECK_EQUAL(result.reputation[0], 3000000000);
    BOOST_CHECK_EQUAL(result.reputation[1] + result.reputation[2], 7000000000);
    BOOST_CHECK(std::abs(result.reputation[1] - 3500000000) <= 1);
    BOOST_CHECK_EQUAL(result.certainty[0], 7000000000);
    BOOST_CHECK_EQUAL(result.certainty[1], 10000000000);

    result = Resolve(result.reputation, votes, {false, false}, 1, 10, ONE / 5);
    BOOST_CHECK_EQUAL(result.reputation[0], 2700000000);
    BOOST_CHECK(std::abs(result.reputation[1] - 3650000000) <= 1);
    BOOST_CHECK_EQUAL(std::accumulate(result.reputation.begin(), result.reputation.end(), int64_t{0}), 10000000000);
}

BOOST_AUTO_TEST_CASE(votes_majority_of_reputation_not_of_heads)
{
    // One voter with most of the reputation against four with little: the one is right.
    std::vector<std::vector<int64_t>> votes{{ONE, 0}, {0, ONE}, {0, ONE}, {0, ONE}, {0, ONE}};
    const std::vector<int64_t> reputation{6000, 1000, 1000, 1000, 1000};
    const auto result{Resolve(reputation, votes, {false, false}, 1, 10, ONE / 5)};
    BOOST_CHECK((result.outcomes == std::vector<int64_t>{ONE, 0}));
    BOOST_CHECK(result.reputation[0] > reputation[0]);
    for (size_t i{1}; i < 5; ++i) BOOST_CHECK_EQUAL(result.reputation[i], 900);
    BOOST_CHECK_EQUAL(std::accumulate(result.reputation.begin(), result.reputation.end(), int64_t{0}), 10000);

    // Splitting the reputation of a side over more accounts changes nothing.
    const auto split{Resolve({3000, 3000, 1000, 1000, 1000, 1000}, {{ONE, 0}, {ONE, 0}, {0, ONE}, {0, ONE}, {0, ONE}, {0, ONE}}, {false, false}, 1, 10, ONE / 5)};
    BOOST_CHECK((split.outcomes == result.outcomes));
    BOOST_CHECK_EQUAL(split.reputation[0] + split.reputation[1], result.reputation[0]);
}

BOOST_AUTO_TEST_CASE(votes_even_split_is_a_tie)
{
    // Two against two with the same reputation: no answer, and nobody is punished.
    const std::vector<int64_t> reputation{2500, 2500, 2500, 2500};
    const auto result{Resolve(reputation, {{ONE}, {ONE}, {0}, {0}}, {false}, 1, 10, ONE / 5)};
    BOOST_CHECK((result.outcomes == std::vector<int64_t>{ONE / 2}));
    BOOST_CHECK(result.reputation == reputation);

    // Inside the band around a half it is a tie too: 55% say yes.
    BOOST_CHECK_EQUAL(Resolve({5500, 4500}, {{ONE}, {0}}, {false}, 1, 10, ONE / 5).outcomes[0], ONE / 2);
    // Outside it is not: 61% say yes.
    BOOST_CHECK_EQUAL(Resolve({6100, 3900}, {{ONE}, {0}}, {false}, 1, 10, ONE / 5).outcomes[0], ONE);
    BOOST_CHECK_EQUAL(Resolve({3900, 6100}, {{ONE}, {0}}, {false}, 1, 10, ONE / 5).outcomes[0], 0);
}

BOOST_AUTO_TEST_CASE(votes_missing_and_unknown)
{
    // Who does not vote loses the reputation at stake to those who do, and has no say.
    auto result{Resolve({4000, 3000, 3000}, {{VOTE_MISSING, VOTE_MISSING}, {ONE, 0}, {ONE, 0}}, {false, false}, 1, 10, ONE / 5)};
    BOOST_CHECK((result.outcomes == std::vector<int64_t>{ONE, 0}));
    BOOST_CHECK((result.reputation == std::vector<int64_t>{3600, 3200, 3200}));
    BOOST_CHECK_EQUAL(result.participation[0], 0);

    // Half the votes skipped, half the loss: 200 of 4000. It goes to those who voted, by
    // reputation times votes cast, of which the voter who skipped has a quarter.
    result = Resolve({4000, 6000}, {{ONE, VOTE_MISSING}, {ONE, 0}}, {false, false}, 1, 10, ONE / 5);
    BOOST_CHECK((result.reputation == std::vector<int64_t>{3850, 6150}));

    // Nobody votes: the decision is voted again, and nothing changes.
    result = Resolve({4000, 6000}, {{VOTE_MISSING}, {VOTE_MISSING}}, {false}, 1, 10, ONE / 5);
    BOOST_CHECK((result.outcomes == std::vector<int64_t>{OUTCOME_ROLL}));
    BOOST_CHECK((result.reputation == std::vector<int64_t>{4000, 6000}));

    // Most say that the answer cannot be known yet: voted again. Saying so is a vote.
    result = Resolve({4000, 3500, 2500}, {{VOTE_UNKNOWN, ONE}, {VOTE_UNKNOWN, ONE}, {ONE, ONE}}, {false, false}, 1, 10, ONE / 5);
    BOOST_CHECK((result.outcomes == std::vector<int64_t>{OUTCOME_ROLL, ONE}));
    BOOST_CHECK((result.reputation == std::vector<int64_t>{4000, 3500, 2500}));
    // A few say so: the others decide.
    result = Resolve({2000, 4000, 4000}, {{VOTE_UNKNOWN}, {ONE}, {ONE}}, {false}, 1, 10, ONE / 5);
    BOOST_CHECK((result.outcomes == std::vector<int64_t>{ONE}));
}

BOOST_AUTO_TEST_CASE(votes_scaled)
{
    // A point on a scale is the median by reputation, which one voter far off cannot move.
    const auto result{Resolve({3000, 3000, 4000}, {{40000000}, {42000000}, {99000000}}, {true}, 1, 10, ONE / 5)};
    BOOST_CHECK_EQUAL(result.outcomes[0], 42000000);
    BOOST_CHECK_EQUAL(std::accumulate(result.reputation.begin(), result.reputation.end(), int64_t{0}), 10000);
}

BOOST_AUTO_TEST_CASE(votes_opposite_columns)
{
    // Two decisions on which the camps answer the opposite way: the direction of the
    // disagreement is found although the columns cancel out when added up.
    const auto result{Resolve({3000, 3000, 4000}, {{ONE, 0}, {ONE, 0}, {0, ONE}}, {false, false}, 1, 10, ONE / 5)};
    BOOST_CHECK((result.outcomes == std::vector<int64_t>{ONE, 0}));
    BOOST_CHECK_EQUAL(result.reputation[2], 3600);
    BOOST_CHECK_EQUAL(result.reputation[0], 3200);
}

BOOST_AUTO_TEST_CASE(votes_conserve_reputation)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    for (int round{0}; round < 300; ++round) {
        const size_t voters{1 + rng.randrange(12)};
        const size_t decisions{1 + rng.randrange(8)};
        std::vector<int64_t> reputation(voters);
        for (auto& r : reputation) r = 1 + rng.randrange(1000000000);
        std::vector<bool> scaled(decisions);
        for (size_t j{0}; j < decisions; ++j) scaled[j] = rng.randbool();
        std::vector<std::vector<int64_t>> votes(voters, std::vector<int64_t>(decisions));
        for (auto& row : votes) {
            for (size_t j{0}; j < decisions; ++j) {
                const int kind{static_cast<int>(rng.randrange(10))};
                row[j] = kind == 0 ? VOTE_MISSING : kind == 1 ? VOTE_UNKNOWN : scaled[j] ? static_cast<int64_t>(rng.randrange(ONE + 1)) : rng.randbool() ? ONE : 0;
            }
        }
        const auto result{Resolve(reputation, votes, scaled, 1, 10, ONE / 5)};
        BOOST_CHECK_EQUAL(std::accumulate(result.reputation.begin(), result.reputation.end(), int64_t{0}),
                          std::accumulate(reputation.begin(), reputation.end(), int64_t{0}));
        for (size_t i{0}; i < voters; ++i) {
            BOOST_CHECK(result.reputation[i] >= 0);
            // Nobody loses more than what is at stake, twice: once for being wrong, once for skipping.
            BOOST_CHECK(result.reputation[i] >= reputation[i] * 8 / 10 - 1);
        }
        for (size_t j{0}; j < decisions; ++j) BOOST_CHECK(result.outcomes[j] == OUTCOME_ROLL || (result.outcomes[j] >= 0 && result.outcomes[j] <= ONE));
        // The same again gives the same.
        const auto again{Resolve(reputation, votes, scaled, 1, 10, ONE / 5)};
        BOOST_CHECK(again.reputation == result.reputation && again.outcomes == result.outcomes);
    }
}

BOOST_AUTO_TEST_SUITE_END()
