// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/signmessage.h>
#include <consensus/params.h>
#include <addresstype.h>
#include <key.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <hivemind/state.h>
#include <hivemind/math.h>

#include <test/util/setup_common.h>
#include <tinyformat.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
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

namespace {
CKey TestKey(uint8_t n)
{
    std::vector<unsigned char> secret(32, 0);
    secret[31] = n;
    CKey key;
    key.Set(secret.begin(), secret.end(), /*fCompressedIn=*/true);
    return key;
}

Account AccountOf(const CKey& key) { return Account{key.GetPubKey().GetID()}; }

Account TestAccount(uint8_t n)
{
    Account account;
    *account.begin() = n;
    return account;
}

//! Sign the action of output `n` of `tx` with `key`, as the wallet does: once the outputs are made.
void SignOutput(CMutableTransaction& tx, uint32_t n, const CKey& key, const Consensus::HivemindParams& params)
{
    hivemind::Action action{*Assert(hivemind::ParseActionScript(tx.vout[n].scriptPubKey))};
    std::visit([&](auto& a) {
        if constexpr (requires { a.signature; }) {
            a.signature.clear();
            const std::string message{hivemind::ActionMessage(hivemind::Action{a}, tx.vin[0].prevout, n, hivemind::ActionOutputsHash(tx.vout), params)};
            BOOST_REQUIRE(key.SignCompact(MessageHash(message), a.signature));
        }
    }, action);
    tx.vout[n].scriptPubKey = hivemind::ActionScript(action);
}

//! A transaction that asks for `action`, signed by `key`.
CTransaction SignedTx(hivemind::Action action, CAmount value, uint8_t salt, const CKey& key, const Consensus::HivemindParams& params)
{
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{salt}), 0};
    tx.vout.emplace_back(value, hivemind::ActionScript(action));
    SignOutput(tx, 0, key, params);
    return CTransaction{tx};
}

//! List a decision on `event_period` and open a market on it; the id of the market.
hivemind::Id ListAndOpen(hivemind::State& state, const Consensus::HivemindParams& params, int height, uint32_t event_period, uint8_t salt, std::vector<CTxOut>& payouts)
{
    std::string reason;
    const CTransaction list{ActionTx(Listing(event_period, salt), params.listing_fee, salt)};
    BOOST_REQUIRE(state.ApplyTx(list, height, params, payouts, reason));
    hivemind::OpenMarket open;
    open.decisions = {hivemind::MakeId(list.GetHash(), 0)};
    open.liquidity = params.min_liquidity;
    open.text = "market";
    const CTransaction tx{ActionTx(open, hivemind::LmsrFunding(open.liquidity, 2) + state.MarketFee(open.decisions, params), salt + 1)};
    BOOST_REQUIRE(state.ApplyTx(tx, height, params, payouts, reason));
    const hivemind::Id id{hivemind::MakeId(tx.GetHash(), 0)};
    BOOST_REQUIRE(state.GetMarket(id));
    return id;
}

/**
 * Move reputation into a holding below a voter's own, which no transfer makes but a resolution can
 * leave: written to the store as the state keeps it (the table of reputation, 0x44).
 */
void LeaveDust(TestMarkets& markets, const Account& from, const Account& to, int64_t amount)
{
    const auto put{[&](const Account& account, int64_t held) {
        markets.store.Put(sidechain::TableKey(uint8_t{0x44}, account), sidechain::EncodeValue(held));
    }};
    put(from, markets.state.ReputationOf(from) - amount);
    put(to, markets.state.ReputationOf(to) + amount);
}

hivemind::BuyShares Buy(const hivemind::Id& market, const Account& account, int64_t shares)
{
    hivemind::BuyShares buy;
    buy.market = market;
    buy.account = account;
    buy.state = 1;
    buy.shares = shares;
    return buy;
}
} // namespace

BOOST_AUTO_TEST_CASE(settlement_paid_from_the_transaction_queue)
{
    // What a settlement pays waits with the payouts of transactions, not ahead of
    // the deposits: a market of many positions cannot hold the deposits back.
    Consensus::SidechainParams params;
    params.hivemind.period_blocks = 6;
    params.hivemind.max_rolls = 0;
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State side{store};
    hivemind::State markets{store};
    sidechain::Mainchain mainchain;
    sidechain::MainBlock genesis;
    genesis.hash = uint256{1};
    BOOST_REQUIRE(mainchain.Append(genesis));
    sidechain::MainBlock next;
    next.hash = uint256{2};
    next.prev_hash = genesis.hash;
    BOOST_REQUIRE(mainchain.Append(next));
    std::vector<CTxOut> payouts;
    std::string reason;
    BOOST_REQUIRE_MESSAGE(side.ApplyMainEvents(1, mainchain, 1, params, payouts, reason), reason);
    const hivemind::Id market{ListAndOpen(markets, params.hivemind, 1, 0, 1, payouts)};
    for (uint8_t i{1}; i <= 3; ++i) BOOST_REQUIRE(markets.ApplyTx(ActionTx(Buy(market, Account{TestAccount(i)}, ONE), 2 * COIN, 10 + i), 1, params.hivemind, payouts, reason));
    // Period 1, the last of the vote, sealing then revealing; then period 2: the decision, voted on by
    // nobody, is settled as a tie, and the market pays.
    BOOST_REQUIRE_MESSAGE(side.ApplyMainEvents(1, mainchain, 6, params, payouts, reason), reason);
    BOOST_REQUIRE_MESSAGE(side.ApplyMainEvents(1, mainchain, 10, params, payouts, reason), reason);
    payouts.clear();
    BOOST_REQUIRE_MESSAGE(side.ApplyMainEvents(1, mainchain, 12, params, payouts, reason), reason);
    BOOST_REQUIRE(markets.GetMarket(market)->settled);
    BOOST_CHECK(payouts.empty());
    BOOST_CHECK_GE(side.TxQueue().size(), 3U);
    // A deposit owed in the same block goes first.
    const CTxOut deposit{5 * COIN, CScript() << OP_TRUE};
    const auto paid{side.TakePayouts({deposit}, {})};
    BOOST_REQUIRE_GE(paid.size(), 4U);
    BOOST_CHECK(paid.front() == deposit);
}

BOOST_AUTO_TEST_CASE(settlement_flood_leaves_deposits_their_share)
{
    // A market of more positions than a block pays: its settlement waits in the queue of transactions,
    // and the deposits owed meanwhile still get their half of every block, in order, until none is left.
    Consensus::SidechainParams params;
    params.hivemind.period_blocks = 6;
    params.hivemind.max_rolls = 0;
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State side{store};
    hivemind::State markets{store};
    sidechain::Mainchain mainchain;
    sidechain::MainBlock genesis;
    genesis.hash = uint256{1};
    BOOST_REQUIRE(mainchain.Append(genesis));
    sidechain::MainBlock next;
    next.hash = uint256{2};
    next.prev_hash = genesis.hash;
    BOOST_REQUIRE(mainchain.Append(next));
    std::vector<CTxOut> payouts;
    std::string reason;
    BOOST_REQUIRE_MESSAGE(side.ApplyMainEvents(1, mainchain, 1, params, payouts, reason), reason);
    const hivemind::Id market{ListAndOpen(markets, params.hivemind, 1, 0, 1, payouts)};
    const size_t holders{sidechain::MAX_PAYOUTS_PER_BLOCK + 300};
    for (size_t i{0}; i < holders; ++i) {
        Account account;
        account.begin()[0] = 0xf0;
        account.begin()[1] = static_cast<uint8_t>(i & 0xff);
        account.begin()[2] = static_cast<uint8_t>(i >> 8);
        // Over several blocks: each pays the change of so many purchases at most.
        if (payouts.size() == hivemind::MAX_TX_PAYOUTS_PER_BLOCK) payouts.clear();
        BOOST_REQUIRE_MESSAGE(markets.ApplyTx(ActionTx(Buy(market, account, ONE), 100 * COIN, static_cast<uint8_t>(i)), 1, params.hivemind, payouts, reason), reason);
    }
    BOOST_REQUIRE_MESSAGE(side.ApplyMainEvents(1, mainchain, 6, params, payouts, reason), reason);
    BOOST_REQUIRE_MESSAGE(side.ApplyMainEvents(1, mainchain, 10, params, payouts, reason), reason);
    payouts.clear();
    BOOST_REQUIRE_MESSAGE(side.ApplyMainEvents(1, mainchain, 12, params, payouts, reason), reason);
    BOOST_REQUIRE(markets.GetMarket(market)->settled);
    BOOST_CHECK(payouts.empty());
    const size_t settled{side.TxQueue().size()};
    BOOST_REQUIRE_GE(settled, holders);

    // Deposits arrive while the settlement waits: more than half a block of them.
    const size_t half{sidechain::MAX_PAYOUTS_PER_BLOCK / 2};
    std::vector<CTxOut> deposits;
    for (size_t i{0}; i < half + 200; ++i) deposits.emplace_back(static_cast<CAmount>(COIN + i), CScript() << OP_TRUE);
    const auto is_deposit{[&](const CTxOut& o) { return o.nValue >= COIN && o.nValue < COIN + static_cast<CAmount>(deposits.size()) && o.scriptPubKey == (CScript() << OP_TRUE); }};

    // First block: half for the deposits, half for the settlement.
    const auto paid1{side.TakePayouts(deposits, {})};
    BOOST_REQUIRE_EQUAL(paid1.size(), sidechain::MAX_PAYOUTS_PER_BLOCK);
    for (size_t i{0}; i < half; ++i) BOOST_CHECK(paid1[i] == deposits[i]);
    BOOST_CHECK_EQUAL(std::count_if(paid1.begin(), paid1.end(), is_deposit), static_cast<long>(half));
    BOOST_CHECK_EQUAL(side.Queue().size(), deposits.size() - half);
    BOOST_CHECK_EQUAL(side.TxQueue().size(), settled - half);

    // Second block: the rest of the deposits, then the settlement takes what they leave.
    const auto paid2{side.TakePayouts({}, {})};
    BOOST_REQUIRE_EQUAL(paid2.size(), std::min(sidechain::MAX_PAYOUTS_PER_BLOCK, 200 + settled - half));
    for (size_t i{0}; i < 200; ++i) BOOST_CHECK(paid2[i] == deposits[half + i]);
    BOOST_CHECK(side.Queue().empty());
    BOOST_CHECK_EQUAL(std::count_if(paid2.begin() + 200, paid2.end(), is_deposit), 0);

    // The settlement is paid in full in the end, in order, each output once.
    std::vector<CTxOut> rest{side.TxQueue()};
    while (!side.TxQueue().empty()) BOOST_REQUIRE(!side.TakePayouts({}, {}).empty());
    BOOST_CHECK_EQUAL(half + (paid2.size() - 200) + rest.size(), settled);
}

BOOST_AUTO_TEST_CASE(settlement_and_bounded_tx_payouts_share_the_blocks)
{
    // The template's shared queues with the markets' routing of settlements
    // and their bound on what a block's transactions pay: a settlement waits
    // in the queue of transactions, ahead of what later transactions give rise to; blocks whose
    // transactions give rise to MAX_TX_PAYOUTS_PER_BLOCK payouts each, while deposits keep coming,
    // still pay the settlement off and never let either queue grow.
    Consensus::SidechainParams params;
    params.hivemind.period_blocks = 6;
    params.hivemind.max_rolls = 0;
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State side{store};
    hivemind::State markets{store};
    sidechain::Mainchain mainchain;
    sidechain::MainBlock genesis;
    genesis.hash = uint256{1};
    BOOST_REQUIRE(mainchain.Append(genesis));
    sidechain::MainBlock next;
    next.hash = uint256{2};
    next.prev_hash = genesis.hash;
    BOOST_REQUIRE(mainchain.Append(next));
    std::vector<CTxOut> payouts;
    std::string reason;
    BOOST_REQUIRE_MESSAGE(side.ApplyMainEvents(1, mainchain, 1, params, payouts, reason), reason);
    const hivemind::Id market{ListAndOpen(markets, params.hivemind, 1, 0, 1, payouts)};
    const size_t holders{sidechain::MAX_PAYOUTS_PER_BLOCK};
    for (size_t i{0}; i < holders; ++i) {
        Account account;
        account.begin()[0] = 0xe0;
        account.begin()[1] = static_cast<uint8_t>(i & 0xff);
        account.begin()[2] = static_cast<uint8_t>(i >> 8);
        if (payouts.size() == hivemind::MAX_TX_PAYOUTS_PER_BLOCK) payouts.clear();
        BOOST_REQUIRE_MESSAGE(markets.ApplyTx(ActionTx(Buy(market, account, ONE), 100 * COIN, static_cast<uint8_t>(i)), 1, params.hivemind, payouts, reason), reason);
    }
    BOOST_REQUIRE_MESSAGE(side.ApplyMainEvents(1, mainchain, 6, params, payouts, reason), reason);
    BOOST_REQUIRE_MESSAGE(side.ApplyMainEvents(1, mainchain, 10, params, payouts, reason), reason);
    payouts.clear();
    BOOST_REQUIRE_MESSAGE(side.ApplyMainEvents(1, mainchain, 12, params, payouts, reason), reason);
    BOOST_REQUIRE(markets.GetMarket(market)->settled);
    // Routed to the queue of transactions, not paid by the block that settled it.
    BOOST_CHECK(payouts.empty());
    const std::vector<CTxOut> settlement{side.TxQueue()};
    BOOST_REQUIRE_GE(settlement.size(), holders);

    // A second market, whose purchases give rise to payouts (their change) in the blocks below.
    payouts.clear();
    const hivemind::Id other{ListAndOpen(markets, params.hivemind, 13, 50, 3, payouts)};
    const Account buyer{TestAccount(7)};
    BOOST_REQUIRE(markets.ApplyTx(ActionTx(Buy(other, buyer, ONE), COIN, 5), 13, params.hivemind, payouts, reason));

    const size_t half{sidechain::MAX_PAYOUTS_PER_BLOCK / 2};
    std::vector<CTxOut> settlement_paid;
    size_t tx_queue_before{side.TxQueue().size()};
    uint32_t salt{0};
    for (int height{14}; height < 40; ++height) {
        // The block's transactions: as many payouts as they may give rise to, and not one more.
        std::vector<CTxOut> tx_payouts;
        while (tx_payouts.size() < hivemind::MAX_TX_PAYOUTS_PER_BLOCK) {
            ++salt;
            BOOST_REQUIRE_MESSAGE(markets.ApplyTx(ActionTx(Buy(other, buyer, ONE / 1000), COIN + salt, static_cast<uint8_t>(salt)), height, params.hivemind, tx_payouts, reason), reason);
        }
        ++salt;
        BOOST_CHECK(!markets.ApplyTx(ActionTx(Buy(other, buyer, ONE / 1000), COIN + salt, static_cast<uint8_t>(salt)), height, params.hivemind, tx_payouts, reason));
        BOOST_CHECK_EQUAL(reason, "hm-block-payouts-full");
        BOOST_CHECK_EQUAL(tx_payouts.size(), hivemind::MAX_TX_PAYOUTS_PER_BLOCK);
        // And more than half a block of deposits.
        std::vector<CTxOut> deposits;
        for (size_t i{0}; i < half + 100; ++i) deposits.emplace_back(static_cast<CAmount>(COIN + i), CScript() << OP_TRUE);
        const size_t deposits_before{side.Queue().size()};
        const auto paid{side.TakePayouts(deposits, tx_payouts)};
        // A full block, unless both queues hold less.
        BOOST_REQUIRE_EQUAL(paid.size(), std::min(sidechain::MAX_PAYOUTS_PER_BLOCK, deposits_before + deposits.size() + tx_queue_before + tx_payouts.size()));
        // Deposits get at least their half.
        BOOST_CHECK_LE(side.Queue().size(), deposits_before + deposits.size() - half);
        // The queue of transactions gets its half, more than its transactions added: it shrinks.
        const size_t tx_queue{side.TxQueue().size()};
        const size_t tx_queued{tx_queue_before + hivemind::MAX_TX_PAYOUTS_PER_BLOCK};
        BOOST_CHECK_EQUAL(tx_queue, tx_queued > half ? tx_queued - half : 0);
        tx_queue_before = tx_queue;
        for (const CTxOut& o : paid) {
            if (settlement_paid.size() < settlement.size() && o == settlement[settlement_paid.size()]) settlement_paid.push_back(o);
        }
    }
    // The settlement was paid whole, in order, before the payouts of the transactions that came after.
    BOOST_CHECK_EQUAL(settlement_paid.size(), settlement.size());
    // Once the settlement is paid, a block pays all its transactions gave rise to, and the deposits
    // take the rest: both queues are empty in the end.
    BOOST_CHECK(side.TxQueue().empty());
    BOOST_CHECK(side.Queue().empty());
}

BOOST_AUTO_TEST_CASE(a_position_is_opened_with_min_position)
{
    // A purchase that opens a position pays at least MIN_POSITION: positions,
    // each a payout at settlement, are not free to make by the thousand. More of a position is not bound so.
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    std::string reason;
    state.BeginBlock(1, 0, params, payouts);
    const hivemind::Id market{ListAndOpen(state, params, 1, 2, 1, payouts)};
    const Account small{TestAccount(7)}, large{TestAccount(8)};
    // About 0.00025: more than MIN_PURCHASE, less than MIN_POSITION.
    const int64_t few{50000};
    BOOST_REQUIRE(*state.Quote(market, 1, few) > hivemind::MIN_PURCHASE && *state.Quote(market, 1, few) < hivemind::MIN_POSITION);
    payouts.clear();
    BOOST_REQUIRE(state.ApplyTx(ActionTx(Buy(market, small, few), COIN, 10), 1, params, payouts, reason));
    BOOST_CHECK_EQUAL(state.PositionOf({market, small, 1}), 0);
    BOOST_CHECK(payouts.size() == 1 && payouts[0].nValue == COIN - hivemind::REFUND_FEE);
    BOOST_REQUIRE(state.ApplyTx(ActionTx(Buy(market, large, ONE), COIN, 11), 1, params, payouts, reason));
    BOOST_CHECK_EQUAL(state.PositionOf({market, large, 1}), ONE);
    BOOST_REQUIRE(state.ApplyTx(ActionTx(Buy(market, large, few), COIN, 12), 1, params, payouts, reason));
    BOOST_CHECK_EQUAL(state.PositionOf({market, large, 1}), ONE + few);

    // And it holds MIN_POSITION shares for each state of the market, whatever they cost: bought at a
    // high price, fewer would pay less than MIN_POSITION when the market settles at even odds.
    BOOST_REQUIRE(state.ApplyTx(ActionTx(Buy(market, large, 5 * ONE), 10 * COIN, 13), 1, params, payouts, reason));
    const int64_t dear{3 * hivemind::MIN_POSITION / 2};
    BOOST_REQUIRE(*state.Quote(market, 1, dear) > hivemind::MIN_POSITION && dear < hivemind::MinPositionShares(2));
    const Account other{TestAccount(9)};
    BOOST_CHECK(!state.WhyNoPurchase({market, other, 1}, dear).empty());
    BOOST_REQUIRE(state.ApplyTx(ActionTx(Buy(market, other, dear), COIN, 14), 1, params, payouts, reason));
    BOOST_CHECK_EQUAL(state.PositionOf({market, other, 1}), 0);
    BOOST_CHECK(state.WhyNoPurchase({market, other, 1}, hivemind::MinPositionShares(2)).empty());
}

BOOST_AUTO_TEST_CASE(reputation_held_whole)
{
    // A transfer leaves each account with no reputation or at least what a voter
    // of its own holds: the holdings every resolution reads stay about MAX_VOTERS at most.
    const CKey key{TestKey(1)};
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    params.genesis_account = AccountOf(key);
    const int64_t least{params.reputation_supply / hivemind::MAX_VOTERS};
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    std::string reason;
    state.BeginBlock(1, 0, params, payouts);
    uint8_t salt{0};
    const auto transfer{[&](const Account& to, int64_t amount) {
        hivemind::TransferReputation t;
        t.from = AccountOf(key);
        t.to = to;
        t.amount = amount;
        BOOST_REQUIRE(state.ApplyTx(SignedTx(t, 0, ++salt, key, params), 1, params, payouts, reason));
    }};
    const Account a{TestAccount(1)}, b{TestAccount(2)};
    transfer(a, least - 1);
    BOOST_CHECK_EQUAL(state.ReputationOf(a), 0);
    transfer(b, least);
    BOOST_CHECK_EQUAL(state.ReputationOf(b), least);
    // To leave a little behind moves it all; to leave nothing does too.
    const int64_t all{state.ReputationOf(AccountOf(key))};
    transfer(b, all - 1);
    BOOST_CHECK_EQUAL(state.ReputationOf(b), least + all);
    BOOST_CHECK_EQUAL(state.ReputationOf(AccountOf(key)), 0);
}

BOOST_AUTO_TEST_CASE(resolution_writes_what_changed)
{
    // A resolution that changes no holding writes none, and notes none in the undo data.
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    const CKey key{TestKey(1)};
    params.genesis_account = AccountOf(key);
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay base{empty, /*journal=*/false};
    std::vector<CTxOut> payouts;
    std::string reason;
    {
        hivemind::State state{base};
        state.BeginBlock(1, 0, params, payouts);
        hivemind::TransferReputation t;
        t.from = AccountOf(key);
        t.to = Account{TestAccount(1)};
        t.amount = params.reputation_supply / 2;
        BOOST_REQUIRE(state.ApplyTx(SignedTx(t, 0, 1, key, params), 1, params, payouts, reason));
        BOOST_REQUIRE(state.ApplyTx(ActionTx(Listing(0, 2), params.listing_fee, 2), 1, params, payouts, reason));
        state.BeginBlock(6, 0, params, payouts);
        state.BeginBlock(10, 0, params, payouts);
    }
    // Period 2 resolves the decision: nobody voted, it is put off, and nobody's reputation changes.
    sidechain::StoreOverlay block{base, /*journal=*/true};
    hivemind::State state{block};
    state.BeginBlock(12, 0, params, payouts);
    BOOST_CHECK_EQUAL(state.Decisions().begin()->second.rolls, 1);
    for (const auto& [k, v] : block.Changes()) BOOST_CHECK(k.empty() || k[0] != 0x44);
    BOOST_CHECK_EQUAL(state.Reputation().size(), 2U);
}

BOOST_AUTO_TEST_CASE(ballots)
{
    // One seal and one reveal for the votes of a voter on many decisions.
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    const CKey key{TestKey(1)};
    const Account voter{AccountOf(key)};
    params.genesis_account = voter;
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    std::string reason;
    state.BeginBlock(1, 0, params, payouts);
    std::vector<hivemind::Id> ids;
    for (uint8_t i{1}; i <= 2; ++i) {
        const CTransaction list{ActionTx(Listing(0, i), params.listing_fee, i)};
        BOOST_REQUIRE(state.ApplyTx(list, 1, params, payouts, reason));
        ids.push_back(hivemind::MakeId(list.GetHash(), 0));
    }
    // Votes on both close in period 1: sealed in its first blocks, revealed in its last two.
    state.BeginBlock(6, 0, params, payouts);
    uint8_t salt{10};
    const auto seal{[&](const std::vector<hivemind::Id>& decisions, const std::vector<int64_t>& answers, const uint256& vote_salt, int height) {
        hivemind::SealBallot ballot;
        ballot.voter = voter;
        ballot.close_period = 1;
        ballot.decisions = decisions;
        ballot.commitment = hivemind::BallotCommitment(voter, 1, decisions, answers, vote_salt);
        const CTransaction tx{SignedTx(ballot, 0, ++salt, key, params)};
        BOOST_REQUIRE_MESSAGE(state.ApplyTx(tx, height, params, payouts, reason), reason);
        return tx;
    }};
    const auto reveal{[&](const std::vector<int64_t>& answers, const uint256& vote_salt, int height) {
        hivemind::RevealBallot r;
        r.voter = voter;
        r.close_period = 1;
        r.answers = answers;
        r.salt = vote_salt;
        BOOST_REQUIRE(state.ApplyTx(ActionTx(r, 0, ++salt), height, params, payouts, reason));
    }};
    seal(ids, {1, 0}, uint256{1}, 6);
    for (const auto& id : ids) BOOST_CHECK(state.GetVote({id, voter}));
    // A new ballot for the period replaces the last: the seal on the decision it leaves out goes.
    seal({ids[0]}, {1}, uint256{2}, 7);
    BOOST_CHECK(state.GetVote({ids[0], voter}));
    BOOST_CHECK(!state.GetVote({ids[1], voter}));
    // Too early to reveal: nothing happens.
    reveal({1}, uint256{2}, 7);
    BOOST_CHECK(!state.GetVote({ids[0], voter})->revealed);
    state.BeginBlock(10, 0, params, payouts);
    BOOST_REQUIRE(state.RevealPhase());
    // The answers of the ballot replaced open nothing; those sealed do.
    reveal({1, 0}, uint256{1}, 10);
    BOOST_CHECK(!state.GetVote({ids[0], voter})->revealed);
    reveal({0}, uint256{2}, 10);
    BOOST_CHECK(!state.GetVote({ids[0], voter})->revealed);
    reveal({1}, uint256{2}, 10);
    BOOST_CHECK(state.GetVote({ids[0], voter})->revealed);
    BOOST_CHECK_EQUAL(state.GetVote({ids[0], voter})->answer, 1);
    BOOST_CHECK(!state.GetBallot({1, voter}));
    // The next period weighs it.
    state.BeginBlock(12, 0, params, payouts);
    BOOST_CHECK(state.GetDecision(ids[0])->resolved);
    BOOST_CHECK_EQUAL(state.GetDecision(ids[0])->outcome, ONE);
    BOOST_CHECK(!state.GetDecision(ids[1])->resolved);
}

BOOST_AUTO_TEST_CASE(ballots_are_pruned)
{
    // A ballot never revealed leaves the state with its period.
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    const CKey key{TestKey(1)};
    params.genesis_account = AccountOf(key);
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    std::string reason;
    state.BeginBlock(1, 0, params, payouts);
    const CTransaction list{ActionTx(Listing(0, 1), params.listing_fee, 1)};
    BOOST_REQUIRE(state.ApplyTx(list, 1, params, payouts, reason));
    state.BeginBlock(6, 0, params, payouts);
    hivemind::SealBallot ballot;
    ballot.voter = AccountOf(key);
    ballot.close_period = 1;
    ballot.decisions = {hivemind::MakeId(list.GetHash(), 0)};
    BOOST_REQUIRE(state.ApplyTx(SignedTx(ballot, 0, 2, key, params), 6, params, payouts, reason));
    BOOST_CHECK(state.GetBallot({1, AccountOf(key)}));
    state.BeginBlock(12, 0, params, payouts);
    BOOST_CHECK(!state.GetBallot({1, AccountOf(key)}));
}

BOOST_AUTO_TEST_CASE(signatures_name_the_chain)
{
    // A signed action names the chain and its kind: a signature made for another chain is no signature.
    const CKey key{TestKey(1)};
    Consensus::HivemindParams a, b;
    a.genesis_hash = uint256{1};
    b.genesis_hash = uint256{2};
    hivemind::TransferReputation t;
    t.from = AccountOf(key);
    t.to = Account{TestAccount(1)};
    t.amount = 1;
    std::string reason;
    const CTransaction for_a{SignedTx(t, 0, 1, key, a)};
    BOOST_CHECK(hivemind::CheckTx(for_a, a, reason));
    BOOST_CHECK(!hivemind::CheckTx(for_a, b, reason));
    BOOST_CHECK_EQUAL(reason, "bad-hm-signature");
    BOOST_CHECK(hivemind::ActionMessage(t, COutPoint{}, 0, uint256{}, a) != hivemind::ActionMessage(t, COutPoint{}, 0, uint256{}, b));
}

BOOST_AUTO_TEST_CASE(dust_goes_to_the_voters)
{
    // The change of a purchase and the proceeds of a sale below MIN_PAYOUT go to the voters' pot.
    const CKey key{TestKey(1)};
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    std::string reason;
    state.BeginBlock(1, 0, params, payouts);
    const hivemind::Id market{ListAndOpen(state, params, 1, 2, 1, payouts)};
    payouts.clear();
    const Account holder{AccountOf(key)};
    const CAmount price{*state.Quote(market, 1, ONE)};
    CAmount pot{state.VoterPot()};
    BOOST_REQUIRE(state.ApplyTx(ActionTx(Buy(market, holder, ONE), price + 500, 10), 1, params, payouts, reason));
    BOOST_CHECK_EQUAL(state.PositionOf({market, holder, 1}), ONE);
    BOOST_CHECK_EQUAL(payouts.size(), 0U);
    BOOST_CHECK_EQUAL(state.VoterPot(), pot + 500);
    // A sale of a sliver of a share.
    payouts.clear();
    pot = state.VoterPot();
    hivemind::SellShares sell;
    sell.market = market;
    sell.account = holder;
    sell.state = 1;
    sell.shares = 500;
    BOOST_REQUIRE(state.ApplyTx(SignedTx(sell, 0, 11, key, params), 1, params, payouts, reason));
    BOOST_CHECK_EQUAL(state.PositionOf({market, holder, 1}), ONE - 500);
    BOOST_CHECK_EQUAL(payouts.size(), 0U);
    BOOST_CHECK_GT(state.VoterPot(), pot);
}

BOOST_AUTO_TEST_CASE(trade_fee_in_128_bits)
{
    BOOST_CHECK_EQUAL(hivemind::TradeFee(10001, 50), 51);
    BOOST_CHECK_EQUAL(hivemind::TradeFee(MAX_MONEY, 1000), MAX_MONEY / 10);
    // Beyond 64 bits in between, and beyond money at the end: it stays at MAX_MONEY.
    BOOST_CHECK_EQUAL(hivemind::TradeFee(MAX_MONEY, 65535), MAX_MONEY);
    BOOST_CHECK_EQUAL(hivemind::TradeFee(0, 65535), 0);
}

BOOST_AUTO_TEST_CASE(payouts_pay_a_fee)
{
    // The proceeds of a sale, and change, are payouts that pay PAYOUT_FEE to the
    // voters, as a refund pays REFUND_FEE: otherwise a sale would make a payout for a transaction fee.
    const CKey key{TestKey(1)};
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    std::string reason;
    state.BeginBlock(1, 0, params, payouts);
    const hivemind::Id market{ListAndOpen(state, params, 1, 2, 1, payouts)};
    const CAmount kept{hivemind::PAYOUT_FEE};
    const Account holder{AccountOf(key)};

    // The change of a purchase.
    payouts.clear();
    CAmount pot{state.VoterPot()};
    BOOST_REQUIRE(state.ApplyTx(ActionTx(Buy(market, holder, ONE), *state.Quote(market, 1, ONE) + 50000, 10), 1, params, payouts, reason));
    BOOST_CHECK_EQUAL(state.PositionOf({market, holder, 1}), ONE);
    BOOST_REQUIRE_EQUAL(payouts.size(), 1U);
    BOOST_CHECK_EQUAL(payouts[0].nValue, 50000 - kept);
    BOOST_CHECK_EQUAL(state.VoterPot(), pot + kept);

    // The least to get is of what is paid out: the proceeds before PAYOUT_FEE are too much to ask.
    const CAmount proceeds{-*state.Quote(market, 1, -ONE)};
    hivemind::SellShares sell;
    sell.market = market;
    sell.account = holder;
    sell.state = 1;
    sell.shares = ONE;
    sell.min_proceeds = proceeds;
    payouts.clear();
    BOOST_REQUIRE(state.ApplyTx(SignedTx(sell, 0, 11, key, params), 1, params, payouts, reason));
    BOOST_CHECK_EQUAL(state.PositionOf({market, holder, 1}), ONE);
    // The proceeds of the sale, less the fee.
    BOOST_CHECK(payouts.empty());
    pot = state.VoterPot();
    sell.min_proceeds = proceeds - kept;
    BOOST_REQUIRE(state.ApplyTx(SignedTx(sell, 0, 12, key, params), 1, params, payouts, reason));
    BOOST_CHECK_EQUAL(state.PositionOf({market, holder, 1}), 0);
    BOOST_REQUIRE_EQUAL(payouts.size(), 1U);
    BOOST_CHECK_EQUAL(payouts[0].nValue, proceeds - kept);
    BOOST_CHECK_EQUAL(state.VoterPot(), pot + kept);

    // The change of the opening of a market.
    hivemind::OpenMarket open{Assert(state.GetMarket(market))->opening};
    open.text = "another";
    const CAmount cost{hivemind::LmsrFunding(open.liquidity, 2) + state.MarketFee(open.decisions, params)};
    payouts.clear();
    pot = state.VoterPot();
    BOOST_REQUIRE(state.ApplyTx(ActionTx(open, cost + 50000, 13), 1, params, payouts, reason));
    BOOST_REQUIRE_EQUAL(payouts.size(), 1U);
    BOOST_CHECK_EQUAL(payouts[0].nValue, 50000 - kept);
    BOOST_CHECK_EQUAL(state.VoterPot(), pot + kept);
}

BOOST_AUTO_TEST_CASE(block_payouts_are_bounded)
{
    // The transactions of a block give rise to MAX_TX_PAYOUTS_PER_BLOCK payouts at
    // most: one that would go beyond is not valid in the block, and changes nothing. Less than what the
    // queue of payouts of transactions is sure to get of a block, so that transactions cannot make it grow.
    static_assert(hivemind::MAX_TX_PAYOUTS_PER_BLOCK < sidechain::MAX_PAYOUTS_PER_BLOCK / 2);
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    std::string reason;
    state.BeginBlock(1, 0, params, payouts);
    const hivemind::Id market{ListAndOpen(state, params, 1, 2, 1, payouts)};
    payouts.clear();
    const Account holder{TestAccount(5)};
    BOOST_REQUIRE(state.ApplyTx(ActionTx(Buy(market, holder, ONE), COIN, 0), 1, params, payouts, reason));
    // Purchases with change: a payout each.
    while (payouts.size() < hivemind::MAX_TX_PAYOUTS_PER_BLOCK) {
        BOOST_REQUIRE_MESSAGE(state.ApplyTx(ActionTx(Buy(market, holder, ONE / 1000), COIN, static_cast<uint8_t>(payouts.size())), 1, params, payouts, reason), reason);
    }
    const uint256 before{sidechain::StoreHash(markets.store)};
    const CTransaction more{ActionTx(Buy(market, holder, ONE / 1000), COIN, 77)};
    BOOST_CHECK(!state.ApplyTx(more, 1, params, payouts, reason));
    BOOST_CHECK_EQUAL(reason, "hm-block-payouts-full");
    BOOST_CHECK_EQUAL(payouts.size(), hivemind::MAX_TX_PAYOUTS_PER_BLOCK);
    BOOST_CHECK(sidechain::StoreHash(markets.store) == before);
    // A transaction that pays nothing out is valid all the same; and the next block has room.
    BOOST_CHECK(state.ApplyTx(ActionTx(Listing(3, 9), params.listing_fee, 78), 1, params, payouts, reason));
    payouts.clear();
    BOOST_CHECK(state.ApplyTx(more, 2, params, payouts, reason));
}

BOOST_AUTO_TEST_CASE(withdrawal_refunds_count_against_the_payout_bound)
{
    // The refunds of withdrawals a transaction asks for count against the same bound
    // as the payouts of the markets: otherwise one block could put thousands in the queue for free.
    const CKey key{TestKey(1)};
    Consensus::SidechainParams params;
    params.hivemind.period_blocks = 6;
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/false};
    sidechain::State side{store};
    std::vector<CTxOut> payouts;
    std::string reason;
    const size_t many{hivemind::MAX_TX_PAYOUTS_PER_BLOCK + 10};
    CMutableTransaction withdraw;
    withdraw.vin.resize(1);
    withdraw.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{1}), 0};
    for (size_t i{0}; i < many; ++i) {
        withdraw.vout.emplace_back(100000, sidechain::WithdrawalScript(1000, key.GetPubKey().GetID(), GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey()})));
    }
    const CTransaction withdrawals{withdraw};
    BOOST_REQUIRE_MESSAGE(side.ApplyTx(withdrawals, 1, params, payouts, reason, /*main_pending=*/false), reason);
    const auto refunds{[&](size_t from, size_t to, uint8_t salt) {
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{salt}), 0};
        for (size_t i{from}; i < to; ++i) {
            sidechain::RefundRequest request;
            request.withdrawal = COutPoint{withdrawals.GetHash(), static_cast<uint32_t>(i)};
            BOOST_REQUIRE(key.SignCompact(MessageHash(sidechain::RefundMessage(request.withdrawal)), request.signature));
            tx.vout.emplace_back(0, sidechain::RefundScript(request));
        }
        return CTransaction{tx};
    }};
    // A few payouts of the block already: a transaction of refunds that would take them beyond is not valid.
    payouts.assign(5, CTxOut{1000, CScript() << OP_TRUE});
    const CTransaction all{refunds(0, hivemind::MAX_TX_PAYOUTS_PER_BLOCK, 2)};
    BOOST_CHECK(!side.ApplyTx(all, 2, params, payouts, reason, /*main_pending=*/false));
    BOOST_CHECK_EQUAL(reason, "hm-block-payouts-full");
    BOOST_CHECK_EQUAL(payouts.size(), 5U);
    BOOST_CHECK(side.GetWithdrawal(COutPoint{withdrawals.GetHash(), 0}));
    // What fits is valid; and the next block takes the rest.
    BOOST_CHECK(side.ApplyTx(refunds(0, hivemind::MAX_TX_PAYOUTS_PER_BLOCK - 5, 3), 2, params, payouts, reason, /*main_pending=*/false));
    BOOST_CHECK_EQUAL(payouts.size(), hivemind::MAX_TX_PAYOUTS_PER_BLOCK);
    BOOST_CHECK(!side.GetWithdrawal(COutPoint{withdrawals.GetHash(), 0}));
    payouts.clear();
    BOOST_CHECK(side.ApplyTx(refunds(hivemind::MAX_TX_PAYOUTS_PER_BLOCK - 5, many, 4), 3, params, payouts, reason, /*main_pending=*/false));
    BOOST_CHECK_EQUAL(payouts.size(), many - (hivemind::MAX_TX_PAYOUTS_PER_BLOCK - 5));
}

BOOST_AUTO_TEST_CASE(malformed_requests_are_no_failure_of_the_record)
{
    // What CheckTx refuses depends on the transaction alone: a block that has it is
    // invalid whatever the mainchain did, and is not noted as a failure of the record. What depends on
    // the state (the room for payouts) is.
    Consensus::HivemindParams params;
    std::string reason;
    hivemind::ListDecision no_text{Listing(1, 1)};
    no_text.text.clear();
    BOOST_CHECK(!hivemind::CheckTx(ActionTx(no_text, params.listing_fee, 1), params, reason));
    BOOST_CHECK(sidechain::RecordIndependent(reason));
    hivemind::BuyShares none{Buy(uint256{1}, TestAccount(1), 0)};
    BOOST_CHECK(!hivemind::CheckTx(ActionTx(none, COIN, 2), params, reason));
    BOOST_CHECK(sidechain::RecordIndependent(reason));
    hivemind::TransferReputation unsigned_transfer;
    unsigned_transfer.from = TestAccount(1);
    unsigned_transfer.to = TestAccount(2);
    unsigned_transfer.amount = 1;
    BOOST_CHECK(!hivemind::CheckTx(ActionTx(unsigned_transfer, 0, 3), params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-hm-signature");
    BOOST_CHECK(sidechain::RecordIndependent(reason));
    for (const char* other : {"bad-hm-too-many-actions", "bad-hm-format", "bad-hm-coinbase", "bad-hm-vote-periods", "bad-hm-scale",
                              "bad-hm-listing-fee", "bad-hm-market-decisions", "bad-hm-liquidity", "bad-hm-fee", "bad-hm-funding",
                              "bad-hm-payment", "bad-hm-proceeds", "bad-hm-transfer", "bad-hm-vote"}) {
        BOOST_CHECK_MESSAGE(sidechain::RecordIndependent(other), other);
    }
    BOOST_CHECK(!sidechain::RecordIndependent("hm-block-payouts-full"));
}

BOOST_AUTO_TEST_CASE(a_sale_leaves_a_position_or_none)
{
    // A sale leaves no shares, or shares worth MIN_POSITION: otherwise a purchase and
    // a sale of nearly all of it would make a position, which is a payout at settlement, for next to nothing.
    // One that would leave less sells the whole position.
    const CKey key{TestKey(1)}, other{TestKey(2)};
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    std::string reason;
    state.BeginBlock(1, 0, params, payouts);
    const hivemind::Id market{ListAndOpen(state, params, 1, 2, 1, payouts)};
    const Account holder{AccountOf(key)};
    const hivemind::PositionKey position{market, holder, 1};
    BOOST_REQUIRE(state.ApplyTx(ActionTx(Buy(market, holder, ONE), COIN, 10), 1, params, payouts, reason));
    hivemind::SellShares sell;
    sell.market = market;
    sell.account = holder;
    sell.state = 1;
    sell.shares = ONE - 1;
    BOOST_CHECK(!state.WhyNoSale(position, ONE - 1).empty());
    BOOST_CHECK(state.WhyNoSale(position, ONE + 1).find("does not hold") != std::string::npos);
    BOOST_REQUIRE(state.ApplyTx(SignedTx(sell, 0, 11, key, params), 1, params, payouts, reason));
    BOOST_CHECK_EQUAL(state.PositionOf(position), 0);
    BOOST_REQUIRE(state.ApplyTx(ActionTx(Buy(market, holder, ONE), 2 * COIN, 17), 1, params, payouts, reason));
    BOOST_REQUIRE_EQUAL(state.PositionOf(position), ONE);
    // Half a share, at a price near one, is worth about half a coin: it can be kept. And the rest sold.
    sell.shares = ONE / 2;
    BOOST_CHECK(state.WhyNoSale(position, ONE / 2).empty());
    BOOST_REQUIRE(state.ApplyTx(SignedTx(sell, 0, 12, key, params), 1, params, payouts, reason));
    BOOST_CHECK_EQUAL(state.PositionOf(position), ONE / 2);
    BOOST_CHECK(state.WhyNoSale(position, ONE / 2).empty());
    BOOST_REQUIRE(state.ApplyTx(SignedTx(sell, 0, 13, key, params), 1, params, payouts, reason));
    BOOST_CHECK_EQUAL(state.PositionOf(position), 0);

    // A purchase and a sale of all but a unit, in one transaction: the purchase is made, and the sale
    // sells all of it.
    const Account fresh{AccountOf(other)};
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{14}), 0};
    tx.vout.emplace_back(COIN, hivemind::ActionScript(Buy(market, fresh, ONE)));
    sell.account = fresh;
    sell.shares = ONE - 1;
    tx.vout.emplace_back(0, hivemind::ActionScript(sell));
    SignOutput(tx, 1, other, params);
    BOOST_REQUIRE(state.ApplyTx(CTransaction{tx}, 1, params, payouts, reason));
    BOOST_CHECK_EQUAL(state.PositionOf({market, fresh, 1}), 0);

    // What is left holds MinPositionShares() as well: at a price near one, fewer shares can be worth
    // MIN_POSITION, and would pay less at even odds.
    const int64_t keep{3 * hivemind::MIN_POSITION / 2};
    sell.account = holder;
    sell.shares = ONE - keep;
    BOOST_REQUIRE(state.ApplyTx(ActionTx(Buy(market, holder, ONE), 2 * COIN, 15), 1, params, payouts, reason));
    BOOST_REQUIRE_EQUAL(state.PositionOf(position), ONE);
    BOOST_CHECK(-*state.Quote(market, 1, -keep) > hivemind::MIN_POSITION);
    BOOST_CHECK(state.WhyNoSale(position, ONE - keep).find("at least 0.002 shares") != std::string::npos);
    // min_proceeds is of what was asked for: the whole position brings more, and is sold.
    sell.min_proceeds = -*state.Quote(market, 1, -(ONE - keep)) - hivemind::PAYOUT_FEE;
    payouts.clear();
    BOOST_REQUIRE(state.ApplyTx(SignedTx(sell, 0, 16, key, params), 1, params, payouts, reason));
    BOOST_CHECK_EQUAL(state.PositionOf(position), 0);
    BOOST_REQUIRE_EQUAL(payouts.size(), 1U);
    BOOST_CHECK(payouts[0].nValue > sell.min_proceeds);
}

BOOST_AUTO_TEST_CASE(whole_exits_cannot_be_kept_out)
{
    // A sale or a transfer that would leave less than its least moves the whole
    // holding, rather than nothing: the price, or a holding of reputation, can change between the
    // signing and the block, and anyone who trades first could otherwise keep a holder from leaving.
    const CKey key{TestKey(1)}, seller{TestKey(2)}, dust{TestKey(3)};
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    params.genesis_account = AccountOf(key);
    // A holding below a voter's own, as a resolution can leave one.
    const int64_t least{params.reputation_supply / hivemind::MAX_VOTERS};
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    std::string reason;
    state.BeginBlock(1, 0, params, payouts);
    uint8_t salt{0};
    const auto transfer{[&](const CKey& from, const Account& to, int64_t amount, int height) {
        hivemind::TransferReputation t;
        t.from = AccountOf(from);
        t.to = to;
        t.amount = amount;
        BOOST_REQUIRE(state.ApplyTx(SignedTx(t, 0, ++salt, from, params), height, params, payouts, reason));
    }};
    LeaveDust(markets, AccountOf(key), AccountOf(dust), least - 1);
    // A market whose prices move a little with every share.
    params.min_liquidity = 10 * COIN;
    const hivemind::Id market{ListAndOpen(state, params, 1, 2, 100, payouts)};
    const hivemind::PositionKey position{market, AccountOf(seller), 1};
    BOOST_REQUIRE(state.ApplyTx(ActionTx(Buy(market, AccountOf(seller), ONE), 2 * COIN, 102), 1, params, payouts, reason));
    state.BeginBlock(6, 0, params, payouts);

    // A sale meant to keep a little, which is worth enough as it is signed.
    const int64_t keep{hivemind::MinPositionShares(2) + 10000};
    hivemind::SellShares sell;
    sell.market = market;
    sell.account = AccountOf(seller);
    sell.state = 1;
    sell.shares = ONE - keep;
    BOOST_REQUIRE(state.WhyNoSale(position, sell.shares).empty());
    // Someone buys the other state first: what would be kept is worth less than MIN_POSITION.
    hivemind::BuyShares other_side{Buy(market, TestAccount(5), ONE)};
    other_side.state = 0;
    BOOST_REQUIRE(state.ApplyTx(ActionTx(other_side, 2 * COIN, 103), 6, params, payouts, reason));
    BOOST_REQUIRE_EQUAL(state.PositionOf({market, TestAccount(5), 0}), ONE);
    BOOST_REQUIRE(!state.WhyNoSale(position, sell.shares).empty());
    BOOST_REQUIRE(state.ApplyTx(SignedTx(sell, 0, ++salt, seller, params), 6, params, payouts, reason));
    BOOST_CHECK_EQUAL(state.PositionOf(position), 0);

    // A holding below a voter's own moves whole into an account that holds nothing.
    const Account empty{TestAccount(6)};
    transfer(dust, empty, least - 1, 7);
    BOOST_CHECK_EQUAL(state.ReputationOf(empty), least - 1);
    BOOST_CHECK_EQUAL(state.ReputationOf(AccountOf(dust)), 0);
    // But a part of a holding makes no holding that small.
    transfer(key, TestAccount(7), least - 1, 7);
    BOOST_CHECK_EQUAL(state.ReputationOf(TestAccount(7)), 0);
    // A transfer that would leave less than that behind moves the whole holding.
    const int64_t all{state.ReputationOf(AccountOf(key))};
    transfer(key, empty, all - 1, 7);
    BOOST_CHECK_EQUAL(state.ReputationOf(AccountOf(key)), 0);
    int64_t total{0};
    for (const auto& [account, amount] : state.Reputation()) total += amount;
    BOOST_CHECK_EQUAL(total, params.reputation_supply);
}

BOOST_AUTO_TEST_CASE(one_stake_seals_for_one_account)
{
    // A holding that sealed votes is not emptied until the period they close in is
    // over: moved on from account to account, one stake would otherwise seal the votes of every one.
    const CKey a{TestKey(1)}, b{TestKey(2)};
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    params.genesis_account = AccountOf(a);
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    std::string reason;
    state.BeginBlock(1, 0, params, payouts);
    const CTransaction list{ActionTx(Listing(0, 1), params.listing_fee, 1)};
    BOOST_REQUIRE(state.ApplyTx(list, 1, params, payouts, reason));
    const hivemind::Id id{hivemind::MakeId(list.GetHash(), 0)};
    // Votes on it close in period 1.
    state.BeginBlock(6, 0, params, payouts);
    uint8_t salt{10};
    const auto transfer{[&](const CKey& from, const Account& to, int64_t amount, int height) {
        hivemind::TransferReputation t;
        t.from = AccountOf(from);
        t.to = to;
        t.amount = amount;
        BOOST_REQUIRE(state.ApplyTx(SignedTx(t, 0, ++salt, from, params), height, params, payouts, reason));
    }};
    hivemind::SealVote seal;
    seal.voter = AccountOf(a);
    seal.decision = id;
    seal.commitment = uint256{1};
    BOOST_REQUIRE(state.ApplyTx(SignedTx(seal, 0, ++salt, a, params), 6, params, payouts, reason));
    BOOST_REQUIRE(state.GetVote({id, AccountOf(a)}));
    BOOST_CHECK(state.SealedUntil(AccountOf(a)).has_value());
    // Not emptied; but what leaves it a voter's own moves.
    const int64_t all{params.reputation_supply};
    transfer(a, AccountOf(b), all, 6);
    BOOST_CHECK_EQUAL(state.ReputationOf(AccountOf(a)), all);
    transfer(a, AccountOf(b), all / 2, 6);
    BOOST_CHECK_EQUAL(state.ReputationOf(AccountOf(b)), all / 2);
    // A ballot holds its holding as well.
    hivemind::SealBallot ballot;
    ballot.voter = AccountOf(b);
    ballot.close_period = 1;
    ballot.decisions = {id};
    ballot.commitment = uint256{2};
    BOOST_REQUIRE(state.ApplyTx(SignedTx(ballot, 0, ++salt, b, params), 6, params, payouts, reason));
    BOOST_REQUIRE(state.GetVote({id, AccountOf(b)}));
    transfer(b, TestAccount(3), all / 2, 7);
    BOOST_CHECK_EQUAL(state.ReputationOf(AccountOf(b)), all / 2);

    // A stall moves the decision: its seals stand, and so do the holdings until then.
    state.BeginBlock(4 * 6, 0, params, payouts);
    BOOST_REQUIRE_EQUAL(state.GetDecision(id)->close_period, 4U);
    BOOST_REQUIRE(state.GetVote({id, AccountOf(a)}));
    BOOST_CHECK_EQUAL(*state.SealedUntil(AccountOf(a)), 4U);
    transfer(a, TestAccount(3), all / 2, 4 * 6);
    BOOST_CHECK_EQUAL(state.ReputationOf(AccountOf(a)), all / 2);

    // Once the period is over (its reveal phase had a block), the holdings move as they like.
    state.BeginBlock(4 * 6 + 4, 0, params, payouts);
    BOOST_REQUIRE(state.RevealPhase());
    state.BeginBlock(5 * 6, 0, params, payouts);
    BOOST_REQUIRE(!state.GetVote({id, AccountOf(a)}));
    BOOST_CHECK(!state.SealedUntil(AccountOf(a)));
    BOOST_CHECK(!state.SealedUntil(AccountOf(b)));
    transfer(a, TestAccount(3), all / 2, 5 * 6);
    BOOST_CHECK_EQUAL(state.ReputationOf(AccountOf(a)), 0);
}

BOOST_AUTO_TEST_CASE(ballots_need_reputation)
{
    // A ballot is sealed only by an account with the reputation of a voter, as a single vote is.
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    const CKey holder{TestKey(1)}, nobody{TestKey(2)};
    params.genesis_account = AccountOf(holder);
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    std::string reason;
    state.BeginBlock(1, 0, params, payouts);
    const CTransaction list{ActionTx(Listing(0, 1), params.listing_fee, 1)};
    BOOST_REQUIRE(state.ApplyTx(list, 1, params, payouts, reason));
    const hivemind::Id id{hivemind::MakeId(list.GetHash(), 0)};
    state.BeginBlock(6, 0, params, payouts);
    uint8_t salt{10};
    for (const CKey* key : {&nobody, &holder}) {
        hivemind::SealBallot ballot;
        ballot.voter = AccountOf(*key);
        ballot.close_period = 1;
        ballot.decisions = {id};
        BOOST_REQUIRE(state.ApplyTx(SignedTx(ballot, 0, ++salt, *key, params), 6, params, payouts, reason));
    }
    BOOST_CHECK(!state.GetVote({id, AccountOf(nobody)}));
    BOOST_CHECK(!state.GetBallot({1, AccountOf(nobody)}));
    BOOST_CHECK(state.GetVote({id, AccountOf(holder)}));
}

BOOST_AUTO_TEST_CASE(signed_actions_are_bound_to_their_output)
{
    // A signature names the output of its action and the outputs of the transaction:
    // whoever spends the first input can neither repeat a signed action in other outputs, nor move it,
    // nor change what the transaction pays.
    const CKey key{TestKey(1)};
    Consensus::HivemindParams params;
    params.genesis_hash = uint256{1};
    hivemind::TransferReputation t;
    t.from = AccountOf(key);
    t.to = Account{TestAccount(1)};
    t.amount = 1;
    std::string reason;
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{1}), 0};
    tx.vout.emplace_back(0, hivemind::ActionScript(t));
    tx.vout.emplace_back(COIN, CScript() << OP_TRUE);
    SignOutput(tx, 0, key, params);
    BOOST_CHECK(hivemind::CheckTx(CTransaction{tx}, params, reason));
    // Repeated.
    CMutableTransaction twice{tx};
    for (int i{0}; i < 15; ++i) twice.vout.push_back(tx.vout[0]);
    BOOST_CHECK(!hivemind::CheckTx(CTransaction{twice}, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-hm-signature");
    // Moved.
    CMutableTransaction moved{tx};
    std::swap(moved.vout[0], moved.vout[1]);
    BOOST_CHECK(!hivemind::CheckTx(CTransaction{moved}, params, reason));
    // Paying otherwise.
    CMutableTransaction paying{tx};
    paying.vout[1].nValue = 2 * COIN;
    BOOST_CHECK(!hivemind::CheckTx(CTransaction{paying}, params, reason));
    // Two actions of a transaction, each signed in its place, as the wallet does it: whatever the order
    // of signing, as signatures are left out of what is signed.
    CMutableTransaction both{tx};
    t.amount = 2;
    both.vout.emplace_back(0, hivemind::ActionScript(t));
    SignOutput(both, 2, key, params);
    SignOutput(both, 0, key, params);
    BOOST_CHECK(hivemind::CheckTx(CTransaction{both}, params, reason));
}

BOOST_AUTO_TEST_CASE(clock_jump_resolves_only_what_closed)
{
    // A chain whose clock starts at time 0 (regtest with -hivemindperiodseconds) is a billion periods in
    // at its first block: only the periods in which a decision closed are resolved, not every one of them.
    Consensus::HivemindParams params;
    params.period_blocks = 0;
    params.period_seconds = 3;
    params.start_time = 0;
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    std::string reason;
    state.BeginBlock(1, 30, params, payouts);
    BOOST_REQUIRE(state.ApplyTx(ActionTx(Listing(10, 1), params.listing_fee, 1), 1, params, payouts, reason));
    const hivemind::Id id{state.Decisions().begin()->first};
    const auto start{std::chrono::steady_clock::now()};
    state.BeginBlock(2, 3'000'000'000, params, payouts);
    BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{10});
    BOOST_CHECK_EQUAL(state.Period(), 1'000'000'000U);
    // Never in the reveal phase of its period: voted on from now, not put off.
    BOOST_CHECK_EQUAL(state.GetDecision(id)->rolls, 0);
    const uint32_t close{state.GetDecision(id)->close_period};
    BOOST_CHECK_GE(close, 1'000'000'000U);
    // In the reveal phase of its period, then the next: nobody voted, and it is put off once.
    state.BeginBlock(3, 3 * int64_t{close} + 2, params, payouts);
    BOOST_REQUIRE(state.RevealPhase() && state.Period() == close);
    state.BeginBlock(4, 3 * (int64_t{close} + 1), params, payouts);
    BOOST_CHECK_EQUAL(state.GetDecision(id)->rolls, 1);
}

BOOST_AUTO_TEST_CASE(dust_holdings_are_folded)
{
    // Holdings of reputation below a voter's own that a resolution leaves so, and that
    // were so before it, are folded into those of the holders that count: they cannot add up.
    const CKey key{TestKey(1)};
    Consensus::HivemindParams params;
    params.period_blocks = 6;
    params.genesis_account = AccountOf(key);
    // Dust, as a resolution can leave it.
    const int64_t least{params.reputation_supply / hivemind::MAX_VOTERS};
    TestMarkets markets;
    hivemind::State& state{markets.state};
    std::vector<CTxOut> payouts;
    std::string reason;
    state.BeginBlock(1, 0, params, payouts);
    uint8_t salt{0};
    const auto transfer{[&](const Account& to, int64_t amount) {
        hivemind::TransferReputation t;
        t.from = AccountOf(key);
        t.to = to;
        t.amount = amount;
        BOOST_REQUIRE(state.ApplyTx(SignedTx(t, 0, ++salt, key, params), 1, params, payouts, reason));
    }};
    const Account dust{TestAccount(1)}, holder{TestAccount(2)};
    LeaveDust(markets, AccountOf(key), dust, least - 1);
    transfer(holder, 5 * least);
    BOOST_REQUIRE_EQUAL(state.ReputationOf(dust), least - 1);
    BOOST_REQUIRE(state.ApplyTx(ActionTx(Listing(0, 50), params.listing_fee, 50), 1, params, payouts, reason));
    const int64_t genesis{state.ReputationOf(AccountOf(key))};
    state.BeginBlock(6, 0, params, payouts);
    state.BeginBlock(10, 0, params, payouts);
    // Period 2 resolves the decision (nobody voted: it is put off, and nobody loses anything).
    state.BeginBlock(12, 0, params, payouts);
    BOOST_REQUIRE_EQUAL(state.Decisions().begin()->second.rolls, 1);
    BOOST_CHECK_EQUAL(state.ReputationOf(dust), 0);
    BOOST_CHECK_EQUAL(state.Reputation().size(), 2U);
    // Folded by holding.
    BOOST_CHECK(state.ReputationOf(holder) > 5 * least && state.ReputationOf(AccountOf(key)) > genesis);
    int64_t total{0};
    for (const auto& [account, amount] : state.Reputation()) total += amount;
    BOOST_CHECK_EQUAL(total, params.reputation_supply);
}

BOOST_AUTO_TEST_SUITE_END()
