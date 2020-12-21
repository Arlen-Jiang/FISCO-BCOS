/*
 * @CopyRight:
 * FISCO-BCOS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * FISCO-BCOS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with FISCO-BCOS.  If not, see <http://www.gnu.org/licenses/>
 * (c) 2016-2018 fisco-dev contributors.
 */

/**
 * @brief : implementation of PBFT consensus
 * @file: PBFTSealer.cpp
 * @author: yujiechen
 * @date: 2018-09-28
 *
 * @author: yujiechen
 * @file:PBFTSealer.cpp
 * @date: 2018-10-26
 * @modifications: rename PBFTSealer.cpp to PBFTSealer.cpp
 */
#include "PBFTSealer.h"
#include <libutilities/JsonDataConvertUtility.h>
#include <libutilities/Worker.h>
using namespace bcos::protocol;
using namespace bcos::blockverifier;
using namespace bcos::blockchain;
using namespace bcos::p2p;
namespace bcos
{
namespace consensus
{
void PBFTSealer::handleBlock()
{
    /// check the max transaction num of a block early when generate new block
    /// in case of the block is generated by the nextleader and the transaction num is over
    /// maxTransactionLimit
    if (m_sealing.block->getTransactionSize() > m_pbftEngine->maxBlockTransactions())
    {
        PBFTSEALER_LOG(DEBUG)
            << LOG_DESC("Drop block for the transaction num is over maxTransactionLimit")
            << LOG_KV("transaction_num", m_sealing.block->getTransactionSize())
            << LOG_KV("maxTransactionLimit", m_pbftEngine->maxBlockTransactions());
        resetSealingBlock();
        /// notify to re-generate the block
        m_signalled.notify_all();
        m_blockSignalled.notify_all();
        return;
    }
    setBlock();
    PBFTSEALER_LOG(INFO) << LOG_DESC("++++++++++++++++ Generating seal on")
                         << LOG_KV("blkNum", m_sealing.block->header().number())
                         << LOG_KV("tx", m_sealing.block->getTransactionSize())
                         << LOG_KV("nodeIdx", m_pbftEngine->nodeIdx())
                         << LOG_KV("hash", m_sealing.block->header().hash().abridged());
    m_pbftEngine->generatePrepare(m_sealing.block);
    if (m_pbftEngine->shouldReset(*(m_sealing.block)))
    {
        resetSealingBlock();
        m_signalled.notify_all();
        m_blockSignalled.notify_all();
    }
}
void PBFTSealer::setBlock()
{
    m_sealing.block->header().populateFromParent(
        m_blockChain->getBlockByNumber(m_blockChain->number())->header());
    resetSealingHeader(m_sealing.block->header());
    hookAfterHandleBlock();
    // calculate transactionRoot before execBlock
    m_sealing.block->calTransactionRoot();
}

/**
 * @brief: this node can generate block or not
 * @return true: this node can generate block
 * @return false: this node can't generate block
 */
bool PBFTSealer::shouldSeal()
{
    return Sealer::shouldSeal() && m_pbftEngine->shouldSeal();
}
void PBFTSealer::start()
{
    if (m_enableDynamicBlockSize)
    {
        m_pbftEngine->onTimeout(boost::bind(&PBFTSealer::onTimeout, this, _1));
        m_pbftEngine->onCommitBlock(boost::bind(&PBFTSealer::onCommitBlock, this, _1, _2, _3));
        m_lastBlockNumber = m_blockChain->number();
    }
    m_pbftEngine->start();
    Sealer::start();
}
void PBFTSealer::stop()
{
    Sealer::stop();
    m_pbftEngine->stop();
}

/// attempt to increase m_lastTimeoutTx when m_lastTimeoutTx is no large than m_maxNoTimeoutTx
void PBFTSealer::attempIncreaseTimeoutTx()
{
    // boundary processing:
    // 1. m_lastTimeoutTx or m_maxNoTimeoutTx is large enough, return directly
    if (m_lastTimeoutTx >= m_pbftEngine->maxBlockTransactions())
    {
        m_lastTimeoutTx = m_pbftEngine->maxBlockTransactions();
        return;
    }
    if (m_maxNoTimeoutTx == m_pbftEngine->maxBlockTransactions())
    {
        m_lastTimeoutTx = m_maxNoTimeoutTx;
        return;
    }
    // attempt to increase m_lastTimeoutTx in case of cpu-fluctuation
    // if m_maxNoTimeoutTx * 0.1 is large than 1, reset m_lastTimeoutTx to 110% of m_maxNoTimeoutTx
    if (m_maxNoTimeoutTx * 0.1 > 1)
    {
        m_lastTimeoutTx = m_maxNoTimeoutTx * (1 + 0.1);
    }
    // if m_maxNoTimoutTx*0.1 is little than 1(m_lastTimeoutTx is little than 10), double
    // m_lastTimeoutTx(doubled m_lastTimeoutTx is little than 20)
    else
    {
        m_lastTimeoutTx *= 2;
    }
    if (m_lastTimeoutTx >= m_pbftEngine->maxBlockTransactions())
    {
        m_lastTimeoutTx = m_pbftEngine->maxBlockTransactions();
    }
    PBFTSEALER_LOG(INFO) << LOG_DESC("attempIncreaseTimeoutTx")
                         << LOG_KV("updatedTimeoutTx", m_lastTimeoutTx);
}

/// decrease maxBlockCanSeal to half when timeout
void PBFTSealer::onTimeout(uint64_t const& sealingTxNumber)
{
    // fix the case that maxBlockTransactions of pbftEngine has been decreased through sysconfig
    // precompile while the  m_maxBlockCanSeal remain high
    if (maxBlockCanSeal() >= m_pbftEngine->maxBlockTransactions())
    {
        m_maxBlockCanSeal = m_pbftEngine->maxBlockTransactions();
    }
    /// is syncing, modify the latest block number
    if (m_blockSync->isSyncing())
    {
        m_lastBlockNumber = m_blockSync->status().knownHighestNumber;
    }
    m_timeoutCount++;
    /// update the last minimum transaction number that lead to timeout
    if (sealingTxNumber > 0 && (m_lastTimeoutTx == 0 || (m_lastTimeoutTx > sealingTxNumber &&
                                                            sealingTxNumber > m_maxNoTimeoutTx)))
    {
        m_lastTimeoutTx = sealingTxNumber;
    }
    /// update the maxBlockCanSeal
    {
        UpgradableGuard l(x_maxBlockCanSeal);
        if (m_maxBlockCanSeal > 2)
        {
            UpgradeGuard ul(l);
            m_maxBlockCanSeal /= 2;
        }
    }
    PBFTSEALER_LOG(INFO) << LOG_DESC("decrease maxBlockCanSeal to half for PBFT timeout")
                         << LOG_KV("org_maxBlockCanSeal", m_maxBlockCanSeal * 2)
                         << LOG_KV("halfed_maxBlockCanSeal", m_maxBlockCanSeal)
                         << LOG_KV("timeoutCount", m_timeoutCount)
                         << LOG_KV("lastTimeoutTx", m_lastTimeoutTx);
}

/// increase maxBlockCanSeal when commitBlock with no-timeout
void PBFTSealer::onCommitBlock(
    uint64_t const& blockNumber, uint64_t const& sealingTxNumber, unsigned const& changeCycle)
{
    if (maxBlockCanSeal() >= m_pbftEngine->maxBlockTransactions())
    {
        m_maxBlockCanSeal = m_pbftEngine->maxBlockTransactions();
    }
    /// if is syncing or timeout, return directly
    if (m_blockSync->isSyncing() || changeCycle > 0)
    {
        m_lastBlockNumber = m_blockSync->status().knownHighestNumber;
        return;
    }
    if (blockNumber <= m_lastBlockNumber)
    {
        return;
    }
    m_lastBlockNumber = m_blockChain->number();
    /// sealing more transactions when no timeout
    if (m_timeoutCount > 0)
    {
        m_timeoutCount--;
        return;
    }
    /// update the maximum number of transactions that has been consensused without timeout
    if (sealingTxNumber > 0 && (m_maxNoTimeoutTx == 0 || m_maxNoTimeoutTx < sealingTxNumber))
    {
        m_maxNoTimeoutTx = sealingTxNumber;
        PBFTSEALER_LOG(INFO) << LOG_DESC("increase maxNoTimeoutTx")
                             << LOG_KV("maxNoTimeoutTx", m_maxNoTimeoutTx);
    }
    /// only if m_timeoutCount decreases to 0 and the current maxBlockCanSeal is smaller than
    /// maxBlockTransactions, increase maxBlockCanSeal
    if (maxBlockCanSeal() >= m_pbftEngine->maxBlockTransactions())
    {
        m_maxBlockCanSeal = m_pbftEngine->maxBlockTransactions();
        return;
    }
    // if m_lastTimeoutTx is no large than to m_maxNoTimeoutTx, try to increase m_TimeoutTx
    if (m_lastTimeoutTx <= m_maxNoTimeoutTx)
    {
        attempIncreaseTimeoutTx();
    }
    /// if the current maxBlockCanSeal is larger than m_lastTimeoutTx, return directly
    if (m_lastTimeoutTx != 0 && maxBlockCanSeal() >= m_lastTimeoutTx)
    {
        return;
    }
    /// increase the maxBlockCanSeal when its current value is smaller than the
    /// last-timeout-tx-number
    increaseMaxTxsCanSeal();
}

/// add this function to pass the codeFactor
void PBFTSealer::increaseMaxTxsCanSeal()
{
    WriteGuard l(x_maxBlockCanSeal);
    /// in case of no increase when m_maxBlockCanSeal is smaller than 2
    if (m_blockSizeIncreaseRatio * m_maxBlockCanSeal > 1)
    {
        m_maxBlockCanSeal += (m_blockSizeIncreaseRatio * m_maxBlockCanSeal);
    }
    else
    {
        m_maxBlockCanSeal += 1;
    }
    // increased m_maxBlockCanSeal is large than m_lastTimeoutTx, reset m_maxBlockCanSeal to
    // m_lastTimeoutTx
    if (m_lastTimeoutTx > 0 && m_maxBlockCanSeal > m_lastTimeoutTx)
    {
        m_maxBlockCanSeal = m_lastTimeoutTx;
    }
    // increased m_maxBlockCanSeal is little than m_maxNoTimeoutTx, reset m_maxBlockCanSeal to
    // m_maxNoTimeoutTx
    if (m_maxNoTimeoutTx > 0 && m_maxBlockCanSeal < m_maxNoTimeoutTx)
    {
        m_maxBlockCanSeal = m_maxNoTimeoutTx;
    }
    if (m_maxBlockCanSeal > m_pbftEngine->maxBlockTransactions())
    {
        m_maxBlockCanSeal = m_pbftEngine->maxBlockTransactions();
    }
    PBFTSEALER_LOG(INFO) << LOG_DESC("increase m_maxBlockCanSeal")
                         << LOG_KV("increased_maxBlockCanSeal", m_maxBlockCanSeal);
}

}  // namespace consensus
}  // namespace bcos
