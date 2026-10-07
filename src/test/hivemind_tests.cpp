// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/params.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <hivemind/state.h>
#include <hivemind/math.h>

#include <test/util/setup_common.h>
#include <tinyformat.h>

#include <boost/test/unit_test.hpp>

#include <chrono>

#include <numeric>

using hivemind::LmsrFunding;
using hivemind::LmsrPrices;
using hivemind::LmsrTradeCost;
using hivemind::ONE;
using hivemind::OUTCOME_ROLL;
using hivemind::Resolve;
using hivemind::VOTE_MISSING;
using hivemind::VOTE_UNKNOWN;
using hivemind::Account;
using hivemind::Id;
using hivemind::VoteCommitment;

namespace {
/** The markets, on a store of their own that notes what changes (as a block's would). */
struct TestMarkets {
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    hivemind::State state{store};
    size_t Positions() const
    {
        size_t n{0};
        state.ForEachPosition([&](const hivemind::PositionKey&, int64_t) { ++n; return true; });
        return n;
    }
};
} // namespace

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

BOOST_AUTO_TEST_CASE(votes_many_decisions_no_overflow)
{
    // A holder of most of the reputation and two small voters on many decisions: the products
    // in the resolution stay within 128 bits, so it is quick and exact (it once overflowed and
    // took minutes from about 1200 decisions on).
    const size_t decisions{1500};
    const std::vector<int64_t> reputation{10'000'000'000 - 2000, 1000, 1000};
    std::vector<std::vector<int64_t>> votes(3, std::vector<int64_t>(decisions, VOTE_MISSING));
    for (size_t j{0}; j < decisions; ++j) {
        votes[1][j] = (j % 2) ? ONE : 0;
        votes[2][j] = (j % 2) ? 0 : ONE;
    }
    const auto start{std::chrono::steady_clock::now()};
    const auto result{Resolve(reputation, votes, std::vector<bool>(decisions, false), 1, 10, ONE / 5)};
    BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{30});
    BOOST_CHECK_EQUAL(std::accumulate(result.reputation.begin(), result.reputation.end(), int64_t{0}), 10'000'000'000);
    for (const int64_t r : result.reputation) BOOST_CHECK(r >= 0);
}

BOOST_AUTO_TEST_CASE(votes_rolled_decisions_count_for_nothing)
{
    // Decisions that nobody can answer are voted again: whoever did not vote on them loses
    // nothing, and saying that they cannot be answered earns nothing.
    std::vector<std::vector<int64_t>> votes{{ONE}, {ONE}};
    for (int junk{0}; junk < 50; ++junk) {
        votes[0].push_back(VOTE_MISSING);
        votes[1].push_back(VOTE_UNKNOWN);
    }
    const auto result{Resolve({9900, 100}, votes, std::vector<bool>(51, false), 1, 10, ONE / 5)};
    BOOST_CHECK((result.reputation == std::vector<int64_t>{9900, 100}));
    for (size_t j{1}; j < 51; ++j) BOOST_CHECK_EQUAL(result.outcomes[j], OUTCOME_ROLL);
}

BOOST_AUTO_TEST_CASE(votes_unknown_on_an_answered_decision)
{
    // Saying that the answer cannot be known, of a decision that gets one, is no vote: it
    // cannot be wrong, so it does not earn what a vote earns.
    const auto result{Resolve({4000, 3000, 3000}, {{VOTE_UNKNOWN}, {ONE}, {ONE}}, {false}, 1, 10, ONE / 5)};
    BOOST_CHECK_EQUAL(result.outcomes[0], ONE);
    BOOST_CHECK((result.reputation == std::vector<int64_t>{3600, 3200, 3200}));
    BOOST_CHECK_EQUAL(result.participation[0], 0);
}

BOOST_AUTO_TEST_CASE(votes_few_voters_take_little)
{
    // One small voter alone in a batch: the many who did not vote lose no more in all than what
    // the voter put at stake, not a tenth of their reputation.
    const auto result{Resolve({100, 9900}, {{ONE}, {VOTE_MISSING}}, {false}, 1, 10, ONE / 5)};
    BOOST_CHECK_EQUAL(result.outcomes[0], ONE);
    BOOST_CHECK((result.reputation == std::vector<int64_t>{110, 9890}));
}

BOOST_AUTO_TEST_CASE(votes_quorum)
{
    // Too little reputation answered: the decision is voted again, and nobody loses anything.
    const auto result{Resolve({1000, 9000}, {{ONE}, {VOTE_MISSING}}, {false}, 1, 10, ONE / 5, /*quorum=*/2000)};
    BOOST_CHECK_EQUAL(result.outcomes[0], OUTCOME_ROLL);
    BOOST_CHECK((result.reputation == std::vector<int64_t>{1000, 9000}));
    BOOST_CHECK_EQUAL(Resolve({3000, 7000}, {{ONE}, {VOTE_MISSING}}, {false}, 1, 10, ONE / 5, 2000).outcomes[0], ONE);
}

BOOST_AUTO_TEST_CASE(votes_last_round_unknown_is_no_vote)
{
    // Saying the answer cannot be known puts a decision off; in its last round it counts for
    // nothing, so that it cannot be used to force a tie at no risk.
    const std::vector<std::vector<int64_t>> votes{{VOTE_UNKNOWN}, {ONE}};
    BOOST_CHECK_EQUAL(Resolve({6000, 4000}, votes, {false}, 1, 10, ONE / 5).outcomes[0], OUTCOME_ROLL);
    BOOST_CHECK_EQUAL(Resolve({6000, 4000}, votes, {false}, 1, 10, ONE / 5, 0, {true}).outcomes[0], ONE);
}

BOOST_AUTO_TEST_CASE(vote_commitment_names_the_voter)
{
    // A sealed vote cannot be copied: the same answer and salt commit to another value for another voter.
    const Id decision{uint256{7}};
    const uint256 salt{uint256{9}};
    BOOST_CHECK(VoteCommitment(decision, Account{}, 1, salt) != VoteCommitment(decision, Account{uint160::FromHex("0101010101010101010101010101010101010101").value()}, 1, salt));
}

namespace {
//! A transaction that asks for `action`, burning `value` for it.
CTransaction ActionTx(const hivemind::Action& action, CAmount value, uint8_t salt)
{
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{salt}), 0};
    tx.vout.emplace_back(value, hivemind::ActionScript(action));
    return CTransaction{tx};
}

hivemind::ListDecision Listing(uint32_t event_period, uint8_t n)
{
    hivemind::ListDecision list;
    list.event_period = event_period;
    list.text = strprintf("decision %u", unsigned(n));
    return list;
}
} // namespace

BOOST_AUTO_TEST_CASE(listing_fee_rises_with_the_period)
{
    // Filling a period costs more the fuller it gets: the fee doubles every LISTING_FEE_STEP decisions.
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    state.BeginBlock(1, 0, params, payouts);
    std::string reason;
    const uint32_t event{2}, close{event + 1};
    BOOST_CHECK_EQUAL(state.ListingFee(close, params), params.listing_fee);
    for (uint8_t i{0}; i < hivemind::LISTING_FEE_STEP; ++i) {
        BOOST_REQUIRE_MESSAGE(state.ApplyTx(ActionTx(Listing(event, i), params.listing_fee, i), 1, params, payouts, reason), reason);
    }
    BOOST_CHECK(payouts.empty());
    BOOST_CHECK_EQUAL(state.ListingFee(close, params), 2 * params.listing_fee);
    // Paying the old fee now lists nothing, and the coins come back.
    BOOST_REQUIRE(state.ApplyTx(ActionTx(Listing(event, 50), params.listing_fee, 50), 1, params, payouts, reason));
    BOOST_CHECK_EQUAL(payouts.size(), 1U);
    BOOST_CHECK_EQUAL(state.Decisions().size(), hivemind::LISTING_FEE_STEP);
    // Another period is as cheap as ever.
    BOOST_CHECK_EQUAL(state.ListingFee(close + 1, params), params.listing_fee);
}

BOOST_AUTO_TEST_CASE(stall_does_not_use_up_decisions)
{
    // The chain skips the whole period in which a decision was to be revealed: the decision is
    // voted on later, not put off, and not forced to a tie.
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    std::string reason;
    state.BeginBlock(1, 0, params, payouts);
    BOOST_REQUIRE(state.ApplyTx(ActionTx(Listing(1, 1), params.listing_fee, 1), 1, params, payouts, reason));
    const auto id{state.Decisions().begin()->first};
    BOOST_REQUIRE_EQUAL(state.Decisions().at(id).close_period, 2U);
    // Several periods go by without a block: from period 0 straight into period 9.
    state.BeginBlock(9 * 6, 0, params, payouts);
    const hivemind::Decision decision{state.Decisions().at(id)};
    BOOST_CHECK(!decision.resolved);
    BOOST_CHECK_EQUAL(decision.rolls, 0);
    BOOST_CHECK_GE(decision.close_period, 9U);
}

BOOST_AUTO_TEST_CASE(refunds_keep_a_fee)
{
    // An action that cannot be done gives its coins back, less a small part for the voters; and a
    // purchase has to be worth paying out, so that refunds cannot flood the queue of payouts.
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    std::string reason;
    state.BeginBlock(1, 0, params, payouts);
    hivemind::BuyShares buy;
    buy.market = uint256{0x99};
    buy.shares = ONE;
    BOOST_CHECK(!hivemind::CheckTx(ActionTx(buy, hivemind::MIN_PURCHASE - 1, 1), params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-hm-payment");
    BOOST_REQUIRE(state.ApplyTx(ActionTx(buy, 50000, 1), 1, params, payouts, reason));
    BOOST_REQUIRE_EQUAL(payouts.size(), 1U);
    BOOST_CHECK_EQUAL(payouts[0].nValue, 50000 - hivemind::REFUND_FEE);

    CMutableTransaction many;
    many.vin.resize(1);
    many.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{7}), 0};
    for (size_t i{0}; i <= hivemind::MAX_ACTIONS_PER_TX; ++i) many.vout.emplace_back(50000, hivemind::ActionScript(buy));
    BOOST_CHECK(!hivemind::CheckTx(CTransaction{many}, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-hm-too-many-actions");
}

BOOST_AUTO_TEST_CASE(market_fee_counts_every_decision)
{
    // The fee to open a market rises with the markets on any of its decisions, in any place, and
    // goes to the voters of its decisions.
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    std::string reason;
    state.BeginBlock(1, 0, params, payouts);
    std::vector<hivemind::Id> ids;
    for (uint8_t i{0}; i < 5; ++i) {
        const CTransaction tx{ActionTx(Listing(2, i), params.listing_fee, i)};
        BOOST_REQUIRE(state.ApplyTx(tx, 1, params, payouts, reason));
        ids.push_back(hivemind::MakeId(tx.GetHash(), 0));
    }
    const hivemind::Id d{ids[0]};
    const auto open{[&](std::vector<hivemind::Id> decisions, uint8_t salt) {
        hivemind::OpenMarket market;
        market.decisions = decisions;
        market.liquidity = params.min_liquidity;
        market.text = "market";
        const CAmount fee{state.MarketFee(decisions, params)};
        const CAmount funding{hivemind::LmsrFunding(market.liquidity, size_t{1} << decisions.size())};
        BOOST_REQUIRE(state.ApplyTx(ActionTx(market, funding + fee, salt), 1, params, payouts, reason));
        return fee;
    }};
    const CAmount before{state.Decisions().at(d).fee};
    for (uint8_t i{1}; i <= hivemind::MARKET_FEE_STEP; ++i) open({ids[i], d}, 100 + i);
    BOOST_CHECK_EQUAL(state.MarketFee({d}, params), 2 * params.listing_fee);
    // The fees of the markets that have d second went to d and to their first decision, half each.
    BOOST_CHECK_EQUAL(state.Decisions().at(d).fee, before + hivemind::MARKET_FEE_STEP * (params.listing_fee / 2));
}

BOOST_AUTO_TEST_CASE(catch_up_does_not_use_up_rolls)
{
    // The chain reaches the reveal phase of a decision, nobody reveals, and the next block comes
    // several periods later: the decision is put off once, into a period with time for sealing,
    // not put off again in every period skipped until it is forced to a tie.
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    std::string reason;
    state.BeginBlock(1, 0, params, payouts);
    BOOST_REQUIRE(state.ApplyTx(ActionTx(Listing(1, 1), params.listing_fee, 1), 1, params, payouts, reason));
    const auto id{state.Decisions().begin()->first};
    BOOST_REQUIRE_EQUAL(state.Decisions().at(id).close_period, 2U);
    // A block in the time for sealing of period 2, then one in its reveal phase.
    state.BeginBlock(2 * 6, 0, params, payouts);
    state.BeginBlock(2 * 6 + 4, 0, params, payouts);
    BOOST_REQUIRE_EQUAL(state.Decisions().at(id).close_period, 2U);
    BOOST_REQUIRE(state.RevealPhase());
    state.BeginBlock(9 * 6, 0, params, payouts);
    const hivemind::Decision decision{state.Decisions().at(id)};
    BOOST_CHECK(!decision.resolved);
    BOOST_CHECK_EQUAL(decision.rolls, 1);
    BOOST_CHECK_EQUAL(decision.close_period, 9U);
}

BOOST_AUTO_TEST_CASE(undo_of_every_block)
{
    // What a block changes, as the store notes it, takes the state back to exactly what it was:
    // through listings, markets, purchases, refunds, resolution, settlement and pruning.
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::string reason;
    int height{0};
    const auto block{[&](int at, const std::vector<CTransaction>& txs) {
        const uint256 before{sidechain::StoreHash(markets.store)};
        std::vector<CTxOut> payouts;
        state.BeginBlock(at, 0, params, payouts);
        for (const CTransaction& tx : txs) BOOST_REQUIRE(state.ApplyTx(tx, at, params, payouts, reason));
        const sidechain::StoreUndo undo{markets.store.TakeUndo()};
        // Written out and read back, as the undo data of a block is, it takes the store back.
        DataStream stream{};
        stream << undo;
        sidechain::StoreUndo read;
        stream >> read;
        {
            sidechain::StoreOverlay reverted{markets.store, /*journal=*/false};
            reverted.Revert(read);
            BOOST_CHECK(sidechain::StoreHash(reverted) == before);
        }
        height = at;
    }};
    block(1, {});
    const CTransaction list_a{ActionTx(Listing(1, 1), params.listing_fee, 1)};
    const CTransaction list_b{ActionTx(Listing(1, 2), params.listing_fee, 2)};
    // One listing too far ahead, refunded.
    block(2, {list_a, list_b, ActionTx(Listing(params.max_periods_ahead + 5, 3), params.listing_fee, 3)});
    const hivemind::Id a{hivemind::MakeId(list_a.GetHash(), 0)}, b{hivemind::MakeId(list_b.GetHash(), 0)};
    hivemind::OpenMarket open;
    open.decisions = {a, b};
    open.liquidity = params.min_liquidity;
    open.text = "market";
    const CAmount funding{hivemind::LmsrFunding(open.liquidity, 4)};
    const CTransaction open_tx{ActionTx(open, funding + state.MarketFee(open.decisions, params), 4)};
    block(3, {open_tx});
    const hivemind::Id market{hivemind::MakeId(open_tx.GetHash(), 0)};
    BOOST_REQUIRE(state.Markets().contains(market));
    for (uint8_t i{0}; i < 4; ++i) {
        hivemind::BuyShares buy;
        buy.market = market;
        buy.state = i % 4;
        buy.shares = (i + 1) * ONE;
        block(4 + i, {ActionTx(buy, 10 * COIN, 10 + i)});
    }
    BOOST_CHECK(markets.Positions() > 0);
    // Periods go by: the decisions close with no vote, are put off, resolved in the end, the
    // market settles and pays, and later both are pruned.
    while (height < 6 * 500 && !state.Markets().at(market).settled) block(height + 3, {});
    BOOST_REQUIRE(state.Markets().at(market).settled);
    BOOST_CHECK_EQUAL(markets.Positions(), 0U);
    block(height + 6 * (hivemind::PRUNE_AFTER_PERIODS + 2), {});
    BOOST_CHECK(!state.Markets().contains(market));
    BOOST_CHECK(!state.Decisions().contains(a));
}

BOOST_AUTO_TEST_CASE(clock_and_periods)
{
    // Periods of mainchain time, as on the networks: the last third of each is for revealing votes.
    Consensus::HivemindParams params;
    params.period_blocks = 0;
    params.period_seconds = 600;
    params.start_time = 1000;
    BOOST_CHECK_EQUAL(hivemind::ClockAt(7, 0, params).period, 0U);
    BOOST_CHECK_EQUAL(hivemind::ClockAt(7, 1000, params).period, 0U);
    BOOST_CHECK_EQUAL(hivemind::ClockAt(7, 1000 + 599, params).period, 0U);
    BOOST_CHECK_EQUAL(hivemind::ClockAt(7, 1000 + 600, params).period, 1U);
    BOOST_CHECK_EQUAL(hivemind::ClockAt(99, 1000 + 3 * 600 + 399, params).period, 3U);
    BOOST_CHECK(!hivemind::ClockAt(99, 1000 + 3 * 600 + 399, params).reveal_phase);
    BOOST_CHECK(hivemind::ClockAt(99, 1000 + 3 * 600 + 400, params).reveal_phase);
    // The height has no say.
    BOOST_CHECK_EQUAL(hivemind::ClockAt(1, 1000 + 3 * 600, params).period, hivemind::ClockAt(100000, 1000 + 3 * 600, params).period);

    // Periods of blocks, for tests.
    params.period_blocks = 6;
    BOOST_CHECK_EQUAL(hivemind::ClockAt(5, 999999, params).period, 0U);
    BOOST_CHECK_EQUAL(hivemind::ClockAt(6, 0, params).period, 1U);
    BOOST_CHECK(!hivemind::ClockAt(9, 0, params).reveal_phase);
    BOOST_CHECK(hivemind::ClockAt(10, 0, params).reveal_phase);

    // A state follows the clock forward, and never back when the time of the mainchain does.
    params.period_blocks = 0;
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    state.BeginBlock(1, 1000 + 5 * 600 + 500, params, payouts);
    BOOST_CHECK_EQUAL(state.Period(), 5U);
    BOOST_CHECK(state.RevealPhase());
    // Nor back to sealing, once votes are revealed: a vote could otherwise be sealed again after
    // others were shown. Neither a time of an earlier period nor one earlier in the period does that.
    state.BeginBlock(2, 1000 + 4 * 600 + 500, params, payouts);
    BOOST_CHECK_EQUAL(state.Period(), 5U);
    BOOST_CHECK(state.RevealPhase());
    state.BeginBlock(2, 1000 + 5 * 600 + 100, params, payouts);
    BOOST_CHECK(state.RevealPhase());
    state.BeginBlock(3, 1000 + 6 * 600, params, payouts);
    BOOST_CHECK_EQUAL(state.Period(), 6U);
    BOOST_CHECK(!state.RevealPhase());
    BOOST_CHECK(payouts.empty());
}

BOOST_AUTO_TEST_SUITE_END()
