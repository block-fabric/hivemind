// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <consensus/merkle.h>
#include <interfaces/mining.h>
#include <node/context.h>
#include <pow.h>
#include <script/script.h>
#include <sidechain/mainchain.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
/**
 * A regtest sidechain in slot 9 whose mainchain is a record the test writes: each block is committed
 * to by a mainchain block of its own.
 */
struct SideChainSetup : public TestingSetup {
    SideChainSetup() : TestingSetup{ChainType::REGTEST, {.extra_args = {"-sidechainslot=9"}}} {}

    int main_blocks{0};

    sidechain::Mainchain& Record() { return *Assert(m_node.chainman->m_mainchain); }

    bool AppendMain(std::optional<uint256> bmm)
    {
        sidechain::MainBlock block;
        block.prev_hash = Record().TipHash();
        block.hash = uint256{static_cast<uint8_t>(++main_blocks)};
        block.hash.data()[31] = 0x4e;
        block.time = 1'700'000'000 + main_blocks * 600;
        block.bmm = bmm;
        return Record().Append(block);
    }

    std::unique_ptr<interfaces::BlockTemplate> Template()
    {
        return interfaces::MakeMining(m_node)->createNewBlock({.use_mempool = true, .coinbase_output_script = CScript() << OP_TRUE}, /*cooldown=*/false);
    }

    /** Mine a block, which the next mainchain block commits to. */
    bool Mine()
    {
        const auto block_template{Template()};
        if (!block_template) return false;
        CBlock block{block_template->getBlock()};
        block.hashMerkleRoot = BlockMerkleRoot(block);
        while (!CheckProofOfWork(block.GetHash(), block.nBits, m_node.chainman->GetConsensus())) ++block.nNonce;
        if (!AppendMain(block.GetHash())) return false;
        bool new_block{false};
        if (!m_node.chainman->ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/true, /*min_pow_checked=*/true, &new_block)) return false;
        m_node.validation_signals->SyncWithValidationInterfaceQueue();
        return WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash()) == block.GetHash();
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(hivemind_node_tests, SideChainSetup)

BOOST_AUTO_TEST_CASE(miner_refuses_a_record_behind_the_tip)
{
    // A block is built on the mainchain as it is on record. A record that went back below what the tip
    // of this chain acted on (the mainchain node reorganised, and this node has not followed yet) is no
    // ground to build on: the block would be invalid, and the miner says so rather than build it.
    BOOST_REQUIRE(m_node.chainman->GetConsensus().sidechain.enabled);
    BOOST_REQUIRE(AppendMain(std::nullopt));
    BOOST_REQUIRE(AppendMain(std::nullopt));
    BOOST_REQUIRE(Mine());
    BOOST_REQUIRE(Mine());
    const int acted_on{WITH_LOCK(::cs_main, return m_node.chainman->ActiveChainstate().SideState().MainHeight())};
    BOOST_REQUIRE_EQUAL(acted_on, Record().Height() - 1);
    // Up to the record, a block can be built.
    BOOST_CHECK(Template());
    // The record goes back below it.
    const std::vector<sidechain::MainBlock> forgotten{Record().Truncate(acted_on - 1)};
    BOOST_REQUIRE_EQUAL(forgotten.size(), 2U);
    BOOST_CHECK_EXCEPTION(Template(), std::runtime_error, [](const std::runtime_error& e) {
        return std::string{e.what()}.find("the block does not fit the mainchain on record (bad-sc-main-height)") != std::string::npos;
    });
    // The same blocks back on record: the miner builds again.
    for (const sidechain::MainBlock& block : forgotten) BOOST_REQUIRE(Record().Append(block));
    BOOST_CHECK(Template());
}

BOOST_AUTO_TEST_SUITE_END()
