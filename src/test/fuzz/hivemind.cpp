// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Hivemind under fuzzing, on the store of the sidechain state: blocks of fuzzed listings, markets,
// trades, reputation transfers and votes (signed by a few known keys), with periods going by so that
// decisions resolve, markets settle and both are pruned. After every block: undone, the store is
// exactly what it was (through serialization of the undo data too); a transaction that breaks a rule
// changes nothing; every index is exactly what the tables imply; and the reputation adds up to the
// supply.

#include <common/signmessage.h>
#include <consensus/params.h>
#include <hivemind/math.h>
#include <hivemind/state.h>
#include <key.h>
#include <primitives/transaction.h>
#include <sidechain/store.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/util/setup_common.h>

#include <cassert>
#include <set>
#include <string>
#include <vector>

using namespace hivemind;

namespace {
// Never destroyed: a key's memory comes from the locked pool, which may be gone by the time
// globals are destroyed at exit.
std::vector<CKey>& g_keys{*new std::vector<CKey>};

void initialize_hivemind()
{
    static const auto testing_setup{MakeNoLogFileContext<>()};
    for (uint8_t i{1}; i <= 3; ++i) {
        std::vector<unsigned char> secret(32, 0);
        secret[31] = i;
        CKey key;
        key.Set(secret.begin(), secret.end(), /*fCompressedIn=*/true);
        g_keys.push_back(key);
    }
}

Account AccountOf(const CKey& key) { return Account{key.GetPubKey().GetID()}; }

/** A transaction with one input of its own and the action; signed, if the action is, by `key`. */
CTransaction ActionTx(Action action, CAmount value, uint32_t salt, const CKey& key)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(salt)}), salt});
    const COutPoint first{tx.vin[0].prevout};
    std::visit([&](auto& a) {
        if constexpr (requires { a.signature; }) {
            a.signature.clear();
            const bool ok{key.SignCompact(MessageHash(ActionMessage(Action{a}, first)), a.signature)};
            assert(ok);
        }
    }, action);
    tx.vout.emplace_back(value, ActionScript(action));
    return CTransaction{tx};
}

std::set<sidechain::StoreBytes> KeysUnder(const sidechain::StoreView& view, uint8_t table)
{
    std::set<sidechain::StoreBytes> keys;
    view.ForEach(sidechain::StoreBytes{table}, [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
        keys.insert(key);
        return true;
    });
    return keys;
}

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

/** Every index, rebuilt from the tables, is the one in the store; the reputation is all there. */
void CheckState(const sidechain::StoreView& view, const Consensus::HivemindParams& params)
{
    const State state{view};
    std::set<sidechain::StoreBytes> open, resolved, by_decision, unsettled, settled;
    state.ForEachDecision([&](const Id& id, const Decision& decision) {
        (decision.resolved ? resolved : open).insert(PeriodKey(decision.resolved ? 0x4a : 0x49, decision.close_period, id));
        return true;
    });
    state.ForEachMarket([&](const Id& id, const Market& market) {
        for (const Id& decision : market.opening.decisions) {
            by_decision.insert(PairKey(0x4b, decision, id));
            if (!market.settled) unsettled.insert(PairKey(0x4c, decision, id));
        }
        if (market.settled) settled.insert(PeriodKey(0x4d, market.settled_period, id));
        return true;
    });
    assert(KeysUnder(view, 0x49) == open);
    assert(KeysUnder(view, 0x4a) == resolved);
    assert(KeysUnder(view, 0x4b) == by_decision);
    assert(KeysUnder(view, 0x4c) == unsettled);
    assert(KeysUnder(view, 0x4d) == settled);
    // No position is of a market that is gone, or of a settled market (settlement pays them out).
    state.ForEachPosition([&](const PositionKey& key, int64_t shares) {
        assert(shares != 0);
        const auto market{state.GetMarket(key.market)};
        assert(market && !market->settled);
        return true;
    });
    if (sidechain::Cell<bool>{0x45, false}.Get(view)) {
        int64_t total{0};
        state.ForEachReputation([&](const Account&, int64_t amount) {
            assert(amount > 0);
            total += amount;
            return true;
        });
        assert(total == params.reputation_supply);
    }
}
} // namespace

FUZZ_TARGET(hivemind_state, .init = initialize_hivemind)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    Consensus::HivemindParams params;
    params.period_blocks = fdp.ConsumeIntegralInRange<int>(3, 8);
    params.audit_height = fdp.ConsumeIntegralInRange<int>(0, 40);
    params.max_periods_ahead = 6;
    params.max_vote_periods = 3;
    params.max_rolls = fdp.ConsumeIntegralInRange<uint8_t>(0, 3);
    params.listing_fee = 1000;
    params.min_liquidity = 1000;
    params.genesis_account = AccountOf(g_keys[0]);
    params.reputation_supply = fdp.ConsumeIntegralInRange<int64_t>(0, 100 * COIN);

    sidechain::EmptyStore empty;
    sidechain::StoreOverlay cache{empty, /*journal=*/false};
    uint32_t salt{0};
    int height{0};
    // Votes sealed, to be revealed as sealed (or not).
    std::vector<std::tuple<size_t, Id, int64_t, uint256>> sealed;

    for (int blocks{0}; blocks < 60 && fdp.remaining_bytes() > 0; ++blocks) {
        height += fdp.ConsumeIntegralInRange<int>(1, 2 * params.period_blocks);
        const uint256 before{sidechain::StoreHash(cache)};
        sidechain::StoreOverlay block{cache, /*journal=*/true};
        State state{block};
        std::vector<CTxOut> payouts;
        state.BeginBlock(height, 0, params, payouts);
        std::vector<Id> decisions, markets;
        state.ForEachDecision([&](const Id& id, const Decision&) { decisions.push_back(id); return true; });
        state.ForEachMarket([&](const Id& id, const Market&) { markets.push_back(id); return true; });
        const auto pick{[&](const std::vector<Id>& ids) { return ids.empty() ? Id{} : ids[fdp.ConsumeIntegralInRange<size_t>(0, ids.size() - 1)]; }};
        const int txs{fdp.ConsumeIntegralInRange<int>(0, 5)};
        for (int t{0}; t < txs; ++t) {
            const size_t who{fdp.ConsumeIntegralInRange<size_t>(0, g_keys.size() - 1)};
            const Account account{AccountOf(g_keys[who])};
            CAmount value{fdp.ConsumeIntegralInRange<CAmount>(0, 10 * COIN)};
            std::optional<Action> action;
            switch (fdp.ConsumeIntegralInRange<int>(0, 6)) {
            case 0: {
                ListDecision list;
                list.author = account;
                list.event_period = state.Period() + fdp.ConsumeIntegralInRange<uint32_t>(0, 8);
                list.vote_periods = fdp.ConsumeIntegralInRange<uint16_t>(1, 4);
                list.scaled = fdp.ConsumeBool();
                if (list.scaled) list.max = fdp.ConsumeIntegralInRange<int64_t>(-5, 10);
                list.text = "d";
                value = fdp.ConsumeBool() ? state.ListingFee(list.event_period + list.vote_periods, params) : value;
                action = list;
                break;
            }
            case 1: {
                OpenMarket open;
                open.author = account;
                const size_t n{fdp.ConsumeIntegralInRange<size_t>(1, 2)};
                for (size_t i{0}; i < n; ++i) open.decisions.push_back(pick(decisions));
                open.liquidity = fdp.ConsumeIntegralInRange<int64_t>(0, 10 * params.min_liquidity);
                open.fee_bps = fdp.ConsumeIntegralInRange<uint16_t>(0, 1200);
                open.text = "m";
                if (fdp.ConsumeBool() && open.liquidity > 0) {
                    value = LmsrFunding(open.liquidity, size_t{1} << (2 * n)) + state.MarketFee(open.decisions, params);
                }
                action = open;
                break;
            }
            case 2: {
                BuyShares buy;
                buy.market = pick(markets);
                buy.account = account;
                buy.state = fdp.ConsumeIntegralInRange<uint8_t>(0, 4);
                buy.shares = fdp.ConsumeIntegralInRange<int64_t>(-1, 3 * ONE);
                action = buy;
                break;
            }
            case 3: {
                SellShares sell;
                sell.market = pick(markets);
                sell.account = account;
                sell.state = fdp.ConsumeIntegralInRange<uint8_t>(0, 4);
                sell.shares = fdp.ConsumeIntegralInRange<int64_t>(0, 3 * ONE);
                action = sell;
                value = 0;
                break;
            }
            case 4: {
                TransferReputation transfer;
                transfer.from = account;
                transfer.to = AccountOf(g_keys[fdp.ConsumeIntegralInRange<size_t>(0, g_keys.size() - 1)]);
                transfer.amount = fdp.ConsumeIntegralInRange<int64_t>(-1, params.reputation_supply + 1);
                action = transfer;
                value = 0;
                break;
            }
            case 5: {
                SealVote seal;
                seal.voter = account;
                seal.decision = pick(decisions);
                const int64_t answer{fdp.ConsumeBool() ? ANSWER_UNKNOWN : fdp.ConsumeIntegralInRange<int64_t>(-1, 11)};
                const uint256 vote_salt{static_cast<uint8_t>(fdp.ConsumeIntegral<uint8_t>())};
                seal.commitment = VoteCommitment(seal.decision, seal.voter, answer, vote_salt);
                sealed.emplace_back(who, seal.decision, answer, vote_salt);
                action = seal;
                value = 0;
                break;
            }
            default: {
                if (sealed.empty()) break;
                const auto& [voter, decision, answer, vote_salt]{sealed[fdp.ConsumeIntegralInRange<size_t>(0, sealed.size() - 1)]};
                RevealVote reveal;
                reveal.voter = AccountOf(g_keys[voter]);
                reveal.decision = decision;
                reveal.answer = fdp.ConsumeBool() ? answer : answer + 1;
                reveal.salt = vote_salt;
                action = reveal;
                value = 0;
                break;
            }
            }
            if (!action) continue;
            const CTransaction tx{ActionTx(*action, value, ++salt, g_keys[who])};
            const uint256 before_tx{sidechain::StoreHash(block)};
            std::string reason;
            if (!state.ApplyTx(tx, height, params, payouts, reason)) {
                assert(!reason.empty());
                assert(sidechain::StoreHash(block) == before_tx);
            }
        }
        const sidechain::StoreUndo undo{block.TakeUndo()};
        block.MergeInto(cache);
        CheckState(cache, params);
        DataStream stream{};
        stream << undo;
        sidechain::StoreUndo read;
        stream >> read;
        sidechain::StoreOverlay reverted{cache, /*journal=*/false};
        reverted.Revert(read);
        assert(sidechain::StoreHash(reverted) == before);
        CheckState(reverted, params);
    }
}
