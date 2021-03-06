// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The The Super Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXMEMPOOL_H
#define BITCOIN_TXMEMPOOL_H

#include "mempooldef.h"

/**
 * CTxMemPool stores valid-according-to-the-current-best-chain transactions
 * that may be included in the next block.
 *
 * Transactions are added when they are seen on the network (or created by the
 * local node), but not all transactions seen are added to the pool. For
 * example, the following new transactions will not be added to the mempool:
 * - a transaction which doesn't meet the minimum fee requirements.
 * - a new transaction that double-spends an input of a transaction already in
 * the pool where the new transaction does not meet the Replace-By-Fee
 * requirements as defined in BIP 125.
 * - a non-standard transaction.
 *
 * CTxMemPool::mapTx, and CTxMemPoolEntry bookkeeping:
 *
 * mapTx is a boost::multi_index that sorts the mempool on 4 criteria:
 * - transaction hash
 * - feerate [we use max(feerate of tx, feerate of tx with all descendants)]
 * - time in mempool
 * - mining score (feerate modified by any fee deltas from PrioritiseTransaction)
 *
 * Note: the term "descendant" refers to in-mempool transactions that depend on
 * this one, while "ancestor" refers to in-mempool transactions that a given
 * transaction depends on.
 *
 * In order for the feerate sort to remain correct, we must update transactions
 * in the mempool when new descendants arrive.  To facilitate this, we track
 * the set of in-mempool direct parents and direct children in mapLinks.  Within
 * each CTxMemPoolEntry, we track the size and fees of all descendants.
 *
 * Usually when a new transaction is added to the mempool, it has no in-mempool
 * children (because any such children would be an orphan).  So in
 * addUnchecked(), we:
 * - update a new entry's setMemPoolParents to include all in-mempool parents
 * - update the new entry's direct parents to include the new tx as a child
 * - update all ancestors of the transaction to include the new tx's size/fee
 *
 * When a transaction is removed from the mempool, we must:
 * - update all in-mempool parents to not track the tx in setMemPoolChildren
 * - update all ancestors to not include the tx's size/fees in descendant state
 * - update all in-mempool children to not include it as a parent
 *
 * These happen in UpdateForRemoveFromMempool().  (Note that when removing a
 * transaction along with its descendants, we must calculate that set of
 * transactions to be removed before doing the removal, or else the mempool can
 * be in an inconsistent state where it's impossible to walk the ancestors of
 * a transaction.)
 *
 * In the event of a reorg, the assumption that a newly added tx has no
 * in-mempool children is false.  In particular, the mempool is in an
 * inconsistent state while new transactions are being added, because there may
 * be descendant transactions of a tx coming from a disconnected block that are
 * unreachable from just looking at transactions in the mempool (the linking
 * transactions may also be in the disconnected block, waiting to be added).
 * Because of this, there's not much benefit in trying to search for in-mempool
 * children in addUnchecked().  Instead, in the special case of transactions
 * being added from a disconnected block, we require the caller to clean up the
 * state, to account for in-mempool, out-of-block descendants for all the
 * in-block transactions by calling UpdateTransactionsFromBlock().  Note that
 * until this is called, the mempool state is not consistent, and in particular
 * mapLinks may not be correct (and therefore functions like
 * CalculateMemPoolAncestors() and CalculateDescendants() that rely
 * on them to walk the mempool are not generally safe to use).
 *
 * Computational limits:
 *
 * Updating all in-mempool ancestors of a newly added transaction can be slow,
 * if no bound exists on how many in-mempool ancestors there may be.
 * CalculateMemPoolAncestors() takes configurable limits that are designed to
 * prevent these calculations from being too CPU intensive.
 *
 */
#include "../interface/imempoolcomponent.h"

const uint64_t MEMPOOL_DUMP_VERSION = 1;

using namespace appbase;

class CTxMemPool
{
private:

    uint32_t nCheckFrequency; //!< Value n means that n times in 2^32 we check.
    unsigned int nTransactionsUpdated; //!< Used by getblocktemplate to trigger CreateNewBlock() invocation
    CBlockPolicyEstimator *minerPolicyEstimator;

    uint64_t totalTxSize;      //!< sum of all mempool tx's virtual sizes. Differs from serialized tx size since witness data is discounted. Defined in BIP 141.
    uint64_t cachedInnerUsage; //!< sum of dynamic memory usage of all the map elements (NOT the maps themselves)

    mutable int64_t lastRollingFeeUpdate;
    mutable bool blockSinceLastRollingFeeBump;
    mutable double rollingMinimumFeeRate; //!< minimum fee to get into the pool, decreases exponentially

    void trackPackageRemoved(const CFeeRate &rate);

public:

    static const int ROLLING_FEE_HALFLIFE = 60 * 60 * 12; // public only for testing

    typedef boost::multi_index_container<
            CTxMemPoolEntry,
            boost::multi_index::indexed_by<
                    // sorted by txid
                    boost::multi_index::hashed_unique<mempoolentry_txid, SaltedTxidHasher>,
                    // sorted by fee rate
                    boost::multi_index::ordered_non_unique<
                            boost::multi_index::tag<descendant_score>,
                            boost::multi_index::identity<CTxMemPoolEntry>,
                            CompareTxMemPoolEntryByDescendantScore
                    >,
                    // sorted by entry time
                    boost::multi_index::ordered_non_unique<
                            boost::multi_index::tag<entry_time>,
                            boost::multi_index::identity<CTxMemPoolEntry>,
                            CompareTxMemPoolEntryByEntryTime
                    >,
                    // sorted by score (for mining prioritization)
                    boost::multi_index::ordered_unique<
                            boost::multi_index::tag<mining_score>,
                            boost::multi_index::identity<CTxMemPoolEntry>,
                            CompareTxMemPoolEntryByScore
                    >,
                    // sorted by fee rate with ancestors
                    boost::multi_index::ordered_non_unique<
                            boost::multi_index::tag<ancestor_score>,
                            boost::multi_index::identity<CTxMemPoolEntry>,
                            CompareTxMemPoolEntryByAncestorFee
                    >,
                    //sbtc-vm
                    // sorted by fee rate with gas price (if contract tx) or ancestors otherwise
                    boost::multi_index::ordered_non_unique<
                            boost::multi_index::tag<ancestor_score_or_gas_price>,
                            boost::multi_index::identity<CTxMemPoolEntry>,
                            CompareTxMemPoolEntryByAncestorFeeOrGasPrice
                    >
            >
    > indexed_transaction_set;

    mutable CCriticalSection cs;
    indexed_transaction_set mapTx;

    typedef indexed_transaction_set::nth_index<0>::type::iterator txiter;
    std::vector<std::pair<uint256, txiter> > vTxHashes; //!< All tx witness hashes/entries in mapTx, in random order

    struct CompareIteratorByHash
    {
        bool operator()(const txiter &a, const txiter &b) const
        {
            return a->GetTx().GetHash() < b->GetTx().GetHash();
        }
    };

    typedef std::set<txiter, CompareIteratorByHash> setEntries;

    const setEntries &GetMemPoolParents(txiter entry) const;

    const setEntries &GetMemPoolChildren(txiter entry) const;
    /*--------------------------------------------------------------------------------------------*/
    /** (try to) add transaction to memory pool
        * plTxnReplaced will be appended to with all transactions replaced from mempool **/
    virtual bool AcceptToMemoryPool(CValidationState &state, const CTransactionRef &tx, bool fLimitFree,
                                    bool *pfMissingInputs, std::list<CTransactionRef> *plTxnReplaced,
                                    bool fOverrideMempoolLimit = false,
                                    const CAmount nAbsurdFee = 0, bool rawTx = false);

    /** (try to) add transaction to memory pool with a specified acceptance time **/
    bool AcceptToMemoryPoolWithTime(const CChainParams &chainparams, CValidationState &state,
                                    const CTransactionRef &tx, bool fLimitFree,
                                    bool *pfMissingInputs, int64_t nAcceptTime,
                                    std::list<CTransactionRef> *plTxnReplaced,
                                    bool fOverrideMempoolLimit, const CAmount nAbsurdFee, bool rawTx = false);

    bool AcceptToMemoryPoolWorker(const CChainParams &chainparams, CValidationState &state,
                                  const CTransactionRef &ptx, bool fLimitFree,
                                  bool *pfMissingInputs, int64_t nAcceptTime,
                                  std::list<CTransactionRef> *plTxnReplaced,
                                  bool fOverrideMempoolLimit, const CAmount &nAbsurdFee,
                                  std::vector<COutPoint> &coins_to_uncache, bool rawTx = false);

    void LimitMempoolSize(size_t limit, unsigned long age);

    // Used to avoid mempool polluting consensus critical paths if CCoinsViewMempool
    // were somehow broken and returning the wrong scriptPubKeys
    bool CheckInputsFromMempoolAndCache(const CTransaction &tx, CValidationState &state, const CCoinsViewCache &view,
                                        unsigned int flags, bool cacheSigStore, PrecomputedTransactionData &txdata);

    void UpdateMempoolForReorg(DisconnectedBlockTransactions &disconnectpool, bool fAddToMempool);

    /**
     * Check if transaction will be BIP 68 final in the next block to be created.
     *
     * Simulates calling SequenceLocks() with data from the tip of the current active chain.
     * Optionally stores in LockPoints the resulting height and time calculated and the hash
     * of the block needed for calculation or skips the calculation and uses the LockPoints
     * passed in for evaluation.
     * The LockPoints should not be considered valid if CheckSequenceLocks returns false.
     *
     * See consensus/consensus.h for flag definitions.
     */
    bool
    CheckSequenceLocks(const CTransaction &tx, int flags, LockPoints *lp = nullptr, bool useExistingLockPoints = false);

private:
    typedef std::map<txiter, setEntries, CompareIteratorByHash> cacheMap;

    struct TxLinks
    {
        setEntries parents;
        setEntries children;
    };

    typedef std::map<txiter, TxLinks, CompareIteratorByHash> txlinksMap;
    txlinksMap mapLinks;

    void UpdateParent(txiter entry, txiter parent, bool add);

    void UpdateChild(txiter entry, txiter child, bool add);

    std::vector<indexed_transaction_set::const_iterator> GetSortedDepthAndScore() const;

public:
    indirectmap<COutPoint, const CTransaction *> mapNextTx;
    std::map<uint256, CAmount> mapDeltas;

    /** Create a new CTxMemPool.
     */
    CTxMemPool(CBlockPolicyEstimator *estimator = nullptr);

    void SetEstimator(CBlockPolicyEstimator *estimator);

    /**
     * If sanity-checking is turned on, check makes sure the pool is
     * consistent (does not contain two transactions that spend the same inputs,
     * all inputs are in the mapNextTx array). If sanity-checking is turned off,
     * check does nothing.
     */
    void Check(const CCoinsViewCache *pcoins) const;

    void setSanityCheck(double dFrequency = 1.0)
    {
        nCheckFrequency = dFrequency * 4294967295.0;
    }

    // addUnchecked must updated state for all ancestors of a given transaction,
    // to track size/count of descendant transactions.  First version of
    // addUnchecked can be used to have it call CalculateMemPoolAncestors(), and
    // then invoke the second version.
    bool addUnchecked(const uint256 &hash, const CTxMemPoolEntry &entry, bool validFeeEstimate = true);

    bool addUnchecked(const uint256 &hash, const CTxMemPoolEntry &entry, setEntries &setAncestors,
                      bool validFeeEstimate = true);

    void removeRecursive(const CTransaction &tx, MemPoolRemovalReason reason = MemPoolRemovalReason::UNKNOWN);

    void removeForReorg(const CCoinsViewCache *pcoins, unsigned int nMemPoolHeight, int flags);

    void removeConflicts(const CTransaction &tx);

    void removeForBlock(const std::vector<CTransactionRef> &vtx, unsigned int nBlockHeight);

    void clear();

    void _clear(); //lock free
    bool CompareDepthAndScore(const uint256 &hasha, const uint256 &hashb);

    void queryHashes(std::vector<uint256> &vtxid);

    bool isSpent(const COutPoint &outpoint);

    unsigned int GetTransactionsUpdated() const;

    void AddTransactionsUpdated(unsigned int n);

    /**
     * Check that none of this transactions inputs are in the mempool, and thus
     * the tx is not dependent on other mempool transactions to be included in a block.
     */
    bool HasNoInputsOf(const CTransaction &tx) const;

    /** Affect CreateNewBlock prioritisation of transactions */
    void PrioritiseTransaction(const uint256 &hash, const CAmount &nFeeDelta);

    void ApplyDelta(const uint256 hash, CAmount &nFeeDelta) const;

    void ClearPrioritisation(const uint256 hash);

public:
    /** Remove a set of transactions from the mempool.
     *  If a transaction is in this set, then all in-mempool descendants must
     *  also be in the set, unless this transaction is being removed for being
     *  in a block.
     *  Set updateDescendants to true when removing a tx that was in a block, so
     *  that any in-mempool descendants have their ancestor state updated.
     */
    void RemoveStaged(setEntries &stage, bool updateDescendants,
                      MemPoolRemovalReason reason = MemPoolRemovalReason::UNKNOWN);

    /** When adding transactions from a disconnected block back to the mempool,
     *  new mempool entries may have children in the mempool (which is generally
     *  not the case when otherwise adding transactions).
     *  UpdateTransactionsFromBlock() will find child transactions and update the
     *  descendant state for each transaction in vHashesToUpdate (excluding any
     *  child transactions present in vHashesToUpdate, which are already accounted
     *  for).  Note: vHashesToUpdate should be the set of transactions from the
     *  disconnected block that have been accepted back into the mempool.
     */
    void UpdateTransactionsFromBlock(const std::vector<uint256> &vHashesToUpdate);

    /** Try to calculate all in-mempool ancestors of entry.
     *  (these are all calculated including the tx itself)
     *  limitAncestorCount = max number of ancestors
     *  limitAncestorSize = max size of ancestors
     *  limitDescendantCount = max number of descendants any ancestor can have
     *  limitDescendantSize = max size of descendants any ancestor can have
     *  errString = populated with error reason if any limits are hit
     *  fSearchForParents = whether to search a tx's vin for in-mempool parents, or
     *    look up parents from mapLinks. Must be true for entries not in the mempool
     */
    bool CalculateMemPoolAncestors(const CTxMemPoolEntry &entry, setEntries &setAncestors, uint64_t limitAncestorCount,
                                   uint64_t limitAncestorSize, uint64_t limitDescendantCount,
                                   uint64_t limitDescendantSize, std::string &errString,
                                   bool fSearchForParents = true) const;

    /** Populate setDescendants with all in-mempool descendants of hash.
     *  Assumes that setDescendants includes all in-mempool descendants of anything
     *  already in it.  */
    void CalculateDescendants(txiter it, setEntries &setDescendants);

    /** The minimum fee to get into the mempool, which may itself not be enough
      *  for larger-sized transactions.
      *  The incrementalRelayFee policy variable is used to bound the time it
      *  takes the fee rate to go back down all the way to 0. When the feerate
      *  would otherwise be half of this, it is set to 0 instead.
      */
    CFeeRate GetMinFee(size_t sizelimit) const;

    /** Remove transactions from the mempool until its dynamic size is <= sizelimit.
      *  pvNoSpendsRemaining, if set, will be populated with the list of outpoints
      *  which are not in mempool which no longer have any spends in this mempool.
      */
    void TrimToSize(size_t sizelimit, std::vector<COutPoint> *pvNoSpendsRemaining = nullptr);

    /** Expire all transaction (and their dependencies) in the mempool older than time. Return the number of removed transactions. */
    int Expire(int64_t time);

    /** Returns false if the transaction is in the mempool and not within the chain limit specified. */
    bool TransactionWithinChainLimit(const uint256 &txid, size_t chainLimit) const;

    unsigned long size()
    {
        LOCK(cs);
        return mapTx.size();
    }

    uint64_t GetTotalTxSize()
    {
        LOCK(cs);
        return totalTxSize;
    }

    bool exists(uint256 hash) const
    {
        LOCK(cs);
        return (mapTx.count(hash) != 0);
    }

    CTransactionRef get(const uint256 &hash) const;

    TxMempoolInfo info(const uint256 &hash) const;

    std::vector<TxMempoolInfo> infoAll() const;

    size_t DynamicMemoryUsage() const;

    boost::signals2::signal<void(CTransactionRef)> NotifyEntryAdded;
    boost::signals2::signal<void(CTransactionRef, MemPoolRemovalReason)> NotifyEntryRemoved;

private:
    /** UpdateForDescendants is used by UpdateTransactionsFromBlock to update
     *  the descendants for a single transaction that has been added to the
     *  mempool but may have child transactions in the mempool, eg during a
     *  chain reorg.  setExclude is the set of descendant transactions in the
     *  mempool that must not be accounted for (because any descendants in
     *  setExclude were added to the mempool after the transaction being
     *  updated and hence their state is already reflected in the parent
     *  state).
     *
     *  cachedDescendants will be updated with the descendants of the transaction
     *  being updated, so that future invocations don't need to walk the
     *  same transaction again, if encountered in another transaction chain.
     */
    void UpdateForDescendants(txiter updateIt,
                              cacheMap &cachedDescendants,
                              const std::set<uint256> &setExclude);

    /** Update ancestors of hash to add/remove it as a descendant transaction. */
    void UpdateAncestorsOf(bool add, txiter hash, setEntries &setAncestors);

    /** Set ancestor state for an entry */
    void UpdateEntryForAncestors(txiter it, const setEntries &setAncestors);

    /** For each transaction being removed, update ancestors and any direct children.
      * If updateDescendants is true, then also update in-mempool descendants'
      * ancestor state. */
    void UpdateForRemoveFromMempool(const setEntries &entriesToRemove, bool updateDescendants);

    /** Sever link between specified transaction and direct children. */
    void UpdateChildrenForRemoval(txiter entry);

    /** Before calling removeUnchecked for a given transaction,
     *  UpdateForRemoveFromMempool must be called on the entire (dependent) set
     *  of transactions being removed at the same time.  We use each
     *  CTxMemPoolEntry's setMemPoolParents in order to walk ancestors of a
     *  given transaction that is removed, so we can't remove intermediate
     *  transactions in a chain before we've updated all the state for the
     *  removal.
     */
    void removeUnchecked(txiter entry, MemPoolRemovalReason reason = MemPoolRemovalReason::UNKNOWN);
};

/** 
 * CCoinsView that brings transactions from a memorypool into view.
 * It does not check for spendings by memory pool transactions.
 * Instead, it provides access to all Coins which are either unspent in the
 * base CCoinsView, or are outputs from any mempool transaction!
 * This allows transaction replacement to work as expected, as you want to
 * have all inputs "available" to check signatures, and any cycles in the
 * dependency graph are checked directly in AcceptToMemoryPool.
 * It also allows you to sign a double-spend directly in signrawtransaction,
 * as long as the conflicting transaction is not yet confirmed.
 */
class CCoinsViewMemPool : public CCoinsViewBacked
{
protected:
    const CTxMemPool &mempool;

public:
    CCoinsViewMemPool(CCoinsView *baseIn, const CTxMemPool &mempoolIn);

    bool GetCoin(const COutPoint &outpoint, Coin &coin) const override;
};


#endif // BITCOIN_TXMEMPOOL_H
